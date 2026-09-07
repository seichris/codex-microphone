#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "attention_client.h"
#include "attention_pairing.h"
#include "attention_provisioning.h"
#include "attention_display.h"
#include "attention_audio.h"
#include "attention_model.h"
#include "attention_ui.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "button_input.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi_manager.h"
#include "usb_microphone.h"
#include "voice_audio.h"
#include "voice_control.h"
#include "wireless_microphone.h"
#include "esp_timer.h"

static const char *TAG = "codex_display";

typedef struct {
    char thread_id[ATTENTION_ID_MAX];
} detail_request_t;

typedef enum {
    VOICE_REQUEST_FOCUS = 0,
    VOICE_REQUEST_START,
    VOICE_REQUEST_MUTE,
} voice_request_kind_t;

typedef struct {
    voice_request_kind_t kind;
    uint32_t capture_token;
    char thread_id[ATTENTION_ID_MAX];
} voice_request_t;

typedef struct {
    attention_snapshot_t current;
    attention_snapshot_t previous_success;
    attention_snapshot_t fetched;
} poll_context_t;

static QueueHandle_t s_detail_queue;
static QueueHandle_t s_voice_queue;
static voice_control_t s_voice_control;
static uint32_t s_voice_capture_token;
static bool s_voice_wireless;
static portMUX_TYPE s_voice_control_lock = portMUX_INITIALIZER_UNLOCKED;

static bool should_use_wireless_transport(void)
{
#if CONFIG_CODEX_ATTENTION_VOICE_TRANSPORT_WIFI
    return wireless_microphone_is_ready();
#elif CONFIG_CODEX_ATTENTION_VOICE_TRANSPORT_USB
    return false;
#else
    // Auto uses the USB endpoint while a host is actively requesting samples;
    // a battery-only board has no such request and can use its paired WSS link.
    return !usb_microphone_host_active() && wireless_microphone_is_ready();
#endif
}

static uint8_t attention_reason_mask(const attention_item_t *item)
{
    if (item == NULL) return 0;

    uint8_t mask = 0;
    if (item->unread) mask |= 1U << 0;
    if (item->pinned) mask |= 1U << 1;
    if (item->new_result) mask |= 1U << 2;
    if (item->status == ATTENTION_STATUS_WAITING_INPUT
        || item->status == ATTENTION_STATUS_WAITING_APPROVAL) {
        mask |= 1U << 3;
    }
    return mask;
}

static const attention_item_t *find_attention_item(
    const attention_snapshot_t *snapshot,
    const char *thread_id
)
{
    if (snapshot == NULL || thread_id == NULL) return NULL;

    for (uint32_t index = 0; index < snapshot->count; ++index) {
        if (strcmp(snapshot->items[index].id, thread_id) == 0) {
            return &snapshot->items[index];
        }
    }
    return NULL;
}

static bool snapshot_should_chime(
    const attention_snapshot_t *previous,
    const attention_snapshot_t *next
)
{
    if (previous == NULL || next == NULL || next->source_error[0] != '\0') return false;

    for (uint32_t index = 0; index < next->count; ++index) {
        const attention_item_t *item = &next->items[index];
        const attention_item_t *old_item = find_attention_item(previous, item->id);
        if (old_item == NULL) return true;

        const uint8_t old_reasons = attention_reason_mask(old_item);
        const uint8_t new_reasons = attention_reason_mask(item);
        if ((new_reasons & (uint8_t)~old_reasons) != 0) return true;
    }

    return false;
}

static void render_snapshot(const attention_snapshot_t *snapshot)
{
    if (attention_provisioning_active()) return;
    char wireless_status[112];
    wireless_microphone_get_status(wireless_status, sizeof(wireless_status));
    bsp_display_lock(0);
    attention_ui_render(snapshot);
    attention_ui_set_wireless_status(wireless_status);
    bsp_display_unlock();
}

static void queue_detail(const char *thread_id, void *context)
{
    (void)context;
    if (thread_id == NULL || thread_id[0] == '\0' || s_detail_queue == NULL) return;

    attention_ui_show_detail_loading(thread_id);
    detail_request_t request = { 0 };
    strlcpy(request.thread_id, thread_id, sizeof(request.thread_id));
    (void)xQueueOverwrite(s_detail_queue, &request);
}

static void queue_focus(const char *thread_id, void *context)
{
    (void)context;
    if (thread_id == NULL || thread_id[0] == '\0' || s_voice_queue == NULL) return;
    const voice_request_t request = {
        .kind = VOICE_REQUEST_FOCUS,
    };
    voice_request_t queued = request;
    strlcpy(queued.thread_id, thread_id, sizeof(queued.thread_id));
    taskENTER_CRITICAL(&s_voice_control_lock);
    queued.capture_token = s_voice_capture_token;
    const bool busy = s_voice_control.state == ATTENTION_VOICE_FOCUSING
        || s_voice_control.state == ATTENTION_VOICE_STARTING || s_voice_control.state == ATTENTION_VOICE_LISTENING;
    taskEXIT_CRITICAL(&s_voice_control_lock);
    if (!busy) (void)xQueueSend(s_voice_queue, &queued, 0);
}

static void set_voice_ui_for(uint32_t token, const char *thread_id, attention_voice_state_t state)
{
    bsp_display_lock(0);
    taskENTER_CRITICAL(&s_voice_control_lock);
    const bool current = token == s_voice_capture_token;
    taskEXIT_CRITICAL(&s_voice_control_lock);
    if (current) attention_ui_set_voice_state(thread_id, state);
    bsp_display_unlock();
}

static void suppress_for(uint32_t token, bool suppressed)
{
    taskENTER_CRITICAL(&s_voice_control_lock);
    if (token == s_voice_capture_token) attention_audio_set_suppressed(suppressed);
    taskEXIT_CRITICAL(&s_voice_control_lock);
}

static void reconcile_wireless_failure(void)
{
    uint32_t token;
    if (!wireless_microphone_take_failure_for(&token)) return;
    char thread_id[ATTENTION_ID_MAX] = { 0 };
    taskENTER_CRITICAL(&s_voice_control_lock);
    if (token == s_voice_capture_token && (s_voice_control.state == ATTENTION_VOICE_STARTING
        || s_voice_control.state == ATTENTION_VOICE_LISTENING)) {
        strlcpy(thread_id, s_voice_control.thread_id, sizeof(thread_id));
        voice_control_voice_result(&s_voice_control, false, false);
    }
    taskEXIT_CRITICAL(&s_voice_control_lock);
    // The transport already revoked its own gate. UI reconciliation must not
    // close a successor or consume the terminal result awaited by its caller.
    if (thread_id[0]) {
        suppress_for(token, false);
        set_voice_ui_for(token, thread_id, ATTENTION_VOICE_ERROR);
    }
}

static void make_request_id(char *output, size_t output_size, const char *prefix)
{
    snprintf(
        output,
        output_size,
        "%s-%08lx-%08lx",
        prefix,
        (unsigned long)xTaskGetTickCount(),
        (unsigned long)esp_random()
    );
}

static void poll_task(void *argument)
{
    poll_context_t *context = argument;
    bool has_previous_success = false;

    while (true) {
        reconcile_wireless_failure();
        if (attention_provisioning_active()) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        const uint64_t poll_started_at_us = (uint64_t)esp_timer_get_time();
        esp_err_t result = attention_client_fetch(&context->fetched);
        taskENTER_CRITICAL(&s_voice_control_lock);
        const uint32_t polled_token = s_voice_capture_token;
        const bool stopped = !s_voice_wireless && voice_control_stop_from_remote(&s_voice_control,
            poll_started_at_us, result == ESP_OK && context->fetched.current_thread.available,
            result == ESP_OK ? context->fetched.current_thread.id : NULL,
            result == ESP_OK ? context->fetched.current_thread.voice_state : ATTENTION_VOICE_UNKNOWN);
        taskEXIT_CRITICAL(&s_voice_control_lock);
        if (stopped) {
            voice_audio_stop_capture(polled_token);
            suppress_for(polled_token, false);
        }
        if (result == ESP_OK) {
            if (has_previous_success && snapshot_should_chime(&context->previous_success, &context->fetched)) {
                attention_audio_notify();
            }
            context->current = context->fetched;
            context->previous_success = context->fetched;
            has_previous_success = true;
        } else {
            if (result == ATTENTION_ERR_UNPAIRED || result == ATTENTION_ERR_STORAGE
                || result == ATTENTION_ERR_UNAUTHORIZED || result == ATTENTION_ERR_RESET_PENDING) {
                // Do not retain another pairing's cards or voice target across a
                // security boundary. Ordinary transient Wi-Fi failures still keep
                // the last snapshot with its distinct stale/error diagnostic.
                memset(&context->current, 0, sizeof(context->current));
                memset(&context->previous_success, 0, sizeof(context->previous_success));
                has_previous_success = false;
                bsp_display_lock(0);
                attention_ui_show_list();
                bsp_display_unlock();
            }
            strlcpy(context->current.source_error, attention_connection_error(result), sizeof(context->current.source_error));
            ESP_LOGW(TAG, "%s", context->current.source_error);
        }
        render_snapshot(&context->current);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_CODEX_ATTENTION_POLL_INTERVAL_MS));
    }
}

static void detail_task(void *argument)
{
    attention_detail_t *detail = argument;
    detail_request_t request;

    while (true) {
        if (xQueueReceive(s_detail_queue, &request, portMAX_DELAY) != pdTRUE) continue;

        memset(detail, 0, sizeof(*detail));
        esp_err_t result;
        result = attention_client_fetch_detail(request.thread_id, detail);

        bsp_display_lock(0);
        if (attention_ui_is_detail_for(request.thread_id)) {
            if (result == ESP_OK) {
                attention_ui_render_detail(detail);
            } else {
                char message[ATTENTION_ERROR_MAX];
                snprintf(message, sizeof(message), "Could not load latest text: %s", esp_err_to_name(result));
                attention_ui_show_detail_error(request.thread_id, message);
            }
        }
        bsp_display_unlock();
    }
}

static bool voice_request_current(const voice_request_t *request)
{
    taskENTER_CRITICAL(&s_voice_control_lock);
    const bool current = request->capture_token == s_voice_capture_token;
    taskEXIT_CRITICAL(&s_voice_control_lock);
    return current;
}

static void voice_task(void *argument)
{
    (void)argument;
    voice_request_t request;
    while (true) {
        if (xQueueReceive(s_voice_queue, &request, portMAX_DELAY) != pdTRUE) continue;
        if (!voice_request_current(&request)) continue;
        const bool network_ready = wifi_manager_wait_connected(8000);
        if (!voice_request_current(&request)) continue;
        if (!network_ready) {
            voice_audio_stop_capture(request.capture_token);
            taskENTER_CRITICAL(&s_voice_control_lock);
            if (s_voice_capture_token == request.capture_token)
                voice_control_voice_result(&s_voice_control, false, false);
            taskEXIT_CRITICAL(&s_voice_control_lock);
            suppress_for(request.capture_token, false);
            set_voice_ui_for(request.capture_token, request.thread_id, ATTENTION_VOICE_ERROR);
            continue;
        }
        char request_id[97];
        attention_desktop_state_t response = { 0 };
        if (request.kind == VOICE_REQUEST_FOCUS) {
            suppress_for(request.capture_token, true);
            set_voice_ui_for(request.capture_token, request.thread_id, ATTENTION_VOICE_FOCUSING);
            make_request_id(request_id, sizeof(request_id), "focus");
            const esp_err_t result = attention_client_focus(request.thread_id, request_id, &response);
            if (voice_request_current(&request) && (result != ESP_OK
                || strcmp(response.request_id, request_id) || strcmp(response.thread_id, request.thread_id))) {
                bsp_display_lock(0);
                attention_ui_fixed_focus_failed();
                attention_ui_set_voice_state(request.thread_id, ATTENTION_VOICE_ERROR);
                bsp_display_unlock();
            }
            suppress_for(request.capture_token, false);
            continue;
        }
        if (request.kind == VOICE_REQUEST_MUTE) {
            make_request_id(request_id, sizeof(request_id), "mute");
            const esp_err_t result = attention_client_voice(request.thread_id, "mute", request_id, &response);
            if (result != ESP_OK) ESP_LOGW(TAG, "Desktop mute acknowledgement failed: %s", esp_err_to_name(result));
            suppress_for(request.capture_token, false);
            continue;
        }
        taskENTER_CRITICAL(&s_voice_control_lock);
        const bool focusing = s_voice_capture_token == request.capture_token
            && s_voice_control.state == ATTENTION_VOICE_FOCUSING;
        taskEXIT_CRITICAL(&s_voice_control_lock);
        if (!focusing) continue;
        set_voice_ui_for(request.capture_token, request.thread_id, ATTENTION_VOICE_FOCUSING);
        make_request_id(request_id, sizeof(request_id), "focus");
        const esp_err_t focus_result = attention_client_focus(request.thread_id, request_id, &response);
        taskENTER_CRITICAL(&s_voice_control_lock);
        const voice_control_action_t action = s_voice_capture_token == request.capture_token
            ? voice_control_focus_result(&s_voice_control,
                focus_result == ESP_OK && !strcmp(response.request_id, request_id), response.thread_id)
            : VOICE_CONTROL_ACTION_NONE;
        const bool canceled = s_voice_control.state == ATTENTION_VOICE_MUTED;
        taskEXIT_CRITICAL(&s_voice_control_lock);
        if (action != VOICE_CONTROL_ACTION_START) {
            voice_audio_stop_capture(request.capture_token);
            suppress_for(request.capture_token, false);
            if (!canceled) set_voice_ui_for(request.capture_token, request.thread_id, ATTENTION_VOICE_ERROR);
            continue;
        }
        suppress_for(request.capture_token, true);
        set_voice_ui_for(request.capture_token, request.thread_id, ATTENTION_VOICE_STARTING);
        vTaskDelay(pdMS_TO_TICKS(400));
        taskENTER_CRITICAL(&s_voice_control_lock);
        const bool requested = s_voice_capture_token == request.capture_token
            && s_voice_control.state == ATTENTION_VOICE_STARTING;
        taskEXIT_CRITICAL(&s_voice_control_lock);
        if (!requested) { suppress_for(request.capture_token, false); continue; }

        const bool wireless = should_use_wireless_transport();
        taskENTER_CRITICAL(&s_voice_control_lock);
        if (s_voice_capture_token == request.capture_token) s_voice_wireless = wireless;
        taskEXIT_CRITICAL(&s_voice_control_lock);
        esp_err_t start_result = ESP_ERR_INVALID_STATE;
        if (wireless) {
            make_request_id(request_id, sizeof(request_id), "wireless");
            start_result = wireless_microphone_start_session_authorized(
                request.thread_id, request_id, request.capture_token);
        } else {
#if !CONFIG_CODEX_ATTENTION_VOICE_TRANSPORT_WIFI
            make_request_id(request_id, sizeof(request_id), "voice");
            memset(&response, 0, sizeof(response));
            const esp_err_t result = attention_client_voice(request.thread_id, "start-or-resume", request_id, &response);
            const bool acknowledged = result == ESP_OK && !strcmp(response.request_id, request_id)
                && !strcmp(response.thread_id, request.thread_id) && response.voice_state == ATTENTION_VOICE_LISTENING;
            if (acknowledged)
                start_result = voice_audio_start_capture(VOICE_AUDIO_SOURCE_USB, request.capture_token);
#endif
        }
        taskENTER_CRITICAL(&s_voice_control_lock);
        const bool still_requested = s_voice_capture_token == request.capture_token
            && s_voice_control.state == ATTENTION_VOICE_STARTING;
        if (still_requested) {
            voice_control_voice_result(&s_voice_control, start_result == ESP_OK, start_result == ESP_OK);
            if (start_result == ESP_OK) s_voice_control.recording_started_at_us = (uint64_t)esp_timer_get_time();
        }
        taskEXIT_CRITICAL(&s_voice_control_lock);
        if (!still_requested || start_result != ESP_OK) {
            voice_audio_stop_capture(request.capture_token);
            suppress_for(request.capture_token, false);
        }
        set_voice_ui_for(request.capture_token, request.thread_id,
            still_requested ? (start_result == ESP_OK ? ATTENTION_VOICE_LISTENING : ATTENTION_VOICE_ERROR)
                : ATTENTION_VOICE_MUTED);
    }
}

static void enqueue_voice(const voice_request_t *request)
{
    if (xQueueSend(s_voice_queue, request, 0) == pdTRUE) return;
    voice_audio_stop_capture(request->capture_token);
    taskENTER_CRITICAL(&s_voice_control_lock);
    if (s_voice_capture_token == request->capture_token)
        voice_control_voice_result(&s_voice_control, false, false);
    taskEXIT_CRITICAL(&s_voice_control_lock);
    suppress_for(request->capture_token, false);
    set_voice_ui_for(request->capture_token, request->thread_id, ATTENTION_VOICE_ERROR);
}

static void button_task(void *argument)
{
    (void)argument;
    button_input_event_t event;
    while (true) {
        reconcile_wireless_failure();
        voice_request_t expired = { .kind = VOICE_REQUEST_MUTE };
        bool expired_wireless = false;
        taskENTER_CRITICAL(&s_voice_control_lock);
        if (voice_control_expire(&s_voice_control, (uint64_t)esp_timer_get_time())) {
            strlcpy(expired.thread_id, s_voice_control.thread_id, sizeof(expired.thread_id));
            expired.capture_token = s_voice_capture_token;
            expired_wireless = s_voice_wireless;
        }
        taskEXIT_CRITICAL(&s_voice_control_lock);
        if (expired.thread_id[0]) {
            voice_audio_stop_capture(expired.capture_token);
            const esp_err_t result = expired_wireless ? wireless_microphone_stop_session() : ESP_OK;
            if (!expired_wireless) enqueue_voice(&expired);
            suppress_for(expired.capture_token, false);
            set_voice_ui_for(expired.capture_token, expired.thread_id,
                result == ESP_OK ? ATTENTION_VOICE_MUTED : ATTENTION_VOICE_ERROR);
        }
        const bool has_event = button_input_poll(&event);
        if (attention_provisioning_tick(has_event ? event : BUTTON_INPUT_NONE) || !has_event) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (event == BUTTON_INPUT_BOOT_LONG || event == BUTTON_INPUT_PWR_LONG) {
            // Privacy boundary precedes display locking, target lookup and all
            // transport waits. This token cannot authorize a later canceled take.
            const uint32_t token = voice_audio_revoke_capture();
            voice_request_t request = { .kind = VOICE_REQUEST_MUTE };
            taskENTER_CRITICAL(&s_voice_control_lock);
            const voice_control_action_t stop = voice_control_begin_toggle(&s_voice_control, NULL);
            strlcpy(request.thread_id, s_voice_control.thread_id, sizeof(request.thread_id));
            request.capture_token = s_voice_capture_token;
            const bool wireless = s_voice_wireless;
            taskEXIT_CRITICAL(&s_voice_control_lock);
            if (stop == VOICE_CONTROL_ACTION_MUTE) {
                const esp_err_t result = wireless ? wireless_microphone_stop_session() : ESP_OK;
                if (!wireless) enqueue_voice(&request);
                suppress_for(request.capture_token, false);
                set_voice_ui_for(request.capture_token, request.thread_id,
                    result == ESP_OK ? ATTENTION_VOICE_MUTED : ATTENTION_VOICE_ERROR);
                continue;
            }
            char selected[ATTENTION_ID_MAX] = { 0 };
            bsp_display_lock(0);
            (void)attention_ui_get_voice_target_id(selected, sizeof(selected));
            bsp_display_unlock();
            taskENTER_CRITICAL(&s_voice_control_lock);
            const voice_control_action_t start = voice_control_begin_toggle(&s_voice_control, selected);
            if (start == VOICE_CONTROL_ACTION_FOCUS) {
                s_voice_capture_token = token;
                s_voice_wireless = false;
                request.kind = VOICE_REQUEST_START;
                request.capture_token = token;
                strlcpy(request.thread_id, s_voice_control.thread_id, sizeof(request.thread_id));
            }
            taskEXIT_CRITICAL(&s_voice_control_lock);
            if (start == VOICE_CONTROL_ACTION_FOCUS) enqueue_voice(&request);
            continue;
        }
        bsp_display_lock(0);
        if (event == BUTTON_INPUT_BOOT_SHORT) {
            if (attention_ui_is_settings_visible()) attention_ui_show_list();
            else if (attention_ui_is_detail_visible()) {
                attention_ui_show_list();
                if (attention_ui_select_next()) (void)attention_ui_activate_selected();
            } else (void)attention_ui_select_next();
        } else if (event == BUTTON_INPUT_PWR_SHORT) {
            if (attention_ui_is_detail_visible() || attention_ui_is_settings_visible()) attention_ui_show_list();
            else (void)attention_ui_activate_selected();
        }
        bsp_display_unlock();
    }
}

static void show_startup_status(const char *message, bool failed)
{
    bsp_display_lock(0);
    attention_ui_show_startup_status(message, failed);
    bsp_display_unlock();
    ESP_LOGI(TAG, "%s; internal free=%u largest=%u", message,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static bool create_worker(TaskFunction_t task, const char *name, uint32_t stack_depth,
                          void *argument, UBaseType_t priority, bool external_stack)
{
    // Network workers are long-lived and never run with caches disabled. Keep
    // their stacks in PSRAM; leave the button worker on internal RAM. Tasks
    // created with caps must use vTaskDeleteWithCaps if teardown is ever added.
    const BaseType_t result = external_stack
        ? xTaskCreateWithCaps(task, name, stack_depth, argument, priority, NULL,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : xTaskCreate(task, name, stack_depth, argument, priority, NULL);
    if (result == pdPASS) return true;
    char message[96];
    snprintf(message, sizeof(message), "Cannot start %s: out of memory", name);
    show_startup_status(message, true);
    return false;
}

void app_main(void)
{
    esp_err_t nvs_result = nvs_flash_init_partition("nvs");
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init_partition("nvs");
    }
    ESP_ERROR_CHECK(nvs_result);

    lv_display_t *display = attention_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "Could not start Waveshare display BSP");
        return;
    }
    ESP_ERROR_CHECK(bsp_display_backlight_on());

    voice_control_init(&s_voice_control);
    bsp_display_lock(0);
    attention_ui_init(queue_detail, queue_focus, NULL);
    bsp_display_unlock();
    show_startup_status("Allocating task memory", false);

    // These single-owner buffers scale with the configured card count and
    // detail size. They must not live on a task stack or consume internal RAM.
    poll_context_t *poll = heap_caps_calloc(1, sizeof(*poll), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    attention_detail_t *detail = heap_caps_calloc(1, sizeof(*detail), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (poll == NULL || detail == NULL) {
        heap_caps_free(poll);
        heap_caps_free(detail);
        show_startup_status("Cannot allocate task buffers", true);
        return;
    }

    s_detail_queue = xQueueCreate(1, sizeof(detail_request_t));
    s_voice_queue = xQueueCreate(4, sizeof(voice_request_t));
    if (s_detail_queue == NULL || s_voice_queue == NULL) {
        show_startup_status("Cannot allocate request queues", true);
        heap_caps_free(poll);
        heap_caps_free(detail);
        return;
    }

    show_startup_status("Starting microphone", false);
    ESP_ERROR_CHECK(button_input_init());
    esp_err_t audio_result = voice_audio_init();
    if (audio_result == ESP_OK) {
        esp_err_t usb_result = usb_microphone_init();
        if (usb_result != ESP_OK) {
            ESP_LOGW(TAG, "USB microphone disabled: %s", esp_err_to_name(usb_result));
        }
    } else {
        ESP_LOGW(TAG, "Voice microphone disabled: %s", esp_err_to_name(audio_result));
    }
    audio_result = attention_audio_init();
    if (audio_result != ESP_OK) {
        ESP_LOGW(TAG, "Attention audio disabled: %s", esp_err_to_name(audio_result));
    }
    // Leave legacy wireless namespaces intact. Pairing uses its own encrypted
    // partition and READS an owner-provisioned HMAC eFuse; it never burns one.
    const esp_err_t pairing_result = attention_pairing_init();
    if (pairing_result != ESP_OK) ESP_LOGW(TAG, "Secure pairing storage unavailable");
    ESP_ERROR_CHECK(attention_connection_init());
    const esp_err_t provisioning_result = attention_provisioning_start();
    if (provisioning_result != ESP_OK) ESP_LOGW(TAG, "Serial pairing unavailable");
    show_startup_status("Starting Wi-Fi", false);
    attention_pairing_record_t *pairing = calloc(1, sizeof(*pairing));
    esp_err_t wifi_result = ESP_ERR_INVALID_STATE;
    if (pairing != NULL && attention_pairing_copy(pairing) == ESP_OK) {
        wifi_result = wifi_manager_start_with_credentials(pairing->ssid, pairing->password);
    } else if (CONFIG_CODEX_ATTENTION_WIRELESS_URL[0] != '\0') {
        // Preserve the existing independently paired WSS microphone path.
        wifi_result = wifi_manager_start();
    }
    if (pairing != NULL) attention_pairing_zero(pairing, sizeof(*pairing));
    free(pairing);
    if (wifi_result != ESP_OK) ESP_LOGI(TAG, "Wi-Fi waits for valid provisioning");
    show_startup_status("Starting secure connection", false);
    audio_result = wireless_microphone_init();
    if (audio_result != ESP_OK && audio_result != ESP_ERR_INVALID_STATE && audio_result != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Wireless microphone disabled: %s", esp_err_to_name(audio_result));
    }

    show_startup_status("Starting task workers", false);
    if (!create_worker(detail_task, "attention_detail", 8192, detail, 5, true)) return;
    if (!create_worker(voice_task, "desktop_voice", 12288, NULL, 6, true)) return;
    if (!create_worker(button_task, "attention_buttons", 4096, NULL, 6, false)) return;
    // Start polling last so a failed worker cannot leave the initial screen
    // indefinitely or have its startup error immediately hidden by a poll.
    (void)create_worker(poll_task, "attention_poll", 8192, poll, 5, true);
}
