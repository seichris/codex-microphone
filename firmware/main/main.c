#include <stdio.h>
#include <string.h>
#include "attention_client.h"
#include "attention_display.h"
#include "attention_audio.h"
#include "attention_model.h"
#include "attention_ui.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "button_input.h"
#include "esp_err.h"
#include "esp_log.h"
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
    char thread_id[ATTENTION_ID_MAX];
} voice_request_t;

static QueueHandle_t s_detail_queue;
static QueueHandle_t s_voice_queue;
static voice_control_t s_voice_control;
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
    bsp_display_lock(0);
    attention_ui_render(snapshot);
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
    (void)xQueueSend(s_voice_queue, &queued, 0);
}

static void set_voice_ui(const char *thread_id, attention_voice_state_t state)
{
    bsp_display_lock(0);
    attention_ui_set_voice_state(thread_id, state);
    bsp_display_unlock();
}

static void reconcile_wireless_failure(void)
{
    if (!wireless_microphone_has_failed()) return;

    char thread_id[ATTENTION_ID_MAX] = { 0 };
    bool should_show_error = false;
    taskENTER_CRITICAL(&s_voice_control_lock);
    if (s_voice_control.state == ATTENTION_VOICE_STARTING
        || s_voice_control.state == ATTENTION_VOICE_LISTENING) {
        strlcpy(thread_id, s_voice_control.thread_id, sizeof(thread_id));
        voice_control_voice_result(&s_voice_control, false, false);
        should_show_error = thread_id[0] != '\0';
    }
    taskEXIT_CRITICAL(&s_voice_control_lock);
    voice_audio_set_listening(false);
    attention_audio_set_suppressed(false);
    if (should_show_error) set_voice_ui(thread_id, ATTENTION_VOICE_ERROR);
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
    (void)argument;
    attention_snapshot_t current = { 0 };
    attention_snapshot_t previous_success = { 0 };
    bool has_previous_success = false;
    attention_snapshot_t fetched;

    while (true) {
        reconcile_wireless_failure();
        if (!wifi_manager_wait_connected(8000)) {
            strlcpy(current.source_error, "Wi-Fi not connected", sizeof(current.source_error));
            render_snapshot(&current);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        reconcile_wireless_failure();

        const uint64_t poll_started_at_us = (uint64_t)esp_timer_get_time();
        esp_err_t result = attention_client_fetch(&fetched);
        bool stopped = false;
        // A fresh attention poll may stop legacy USB dictation, but it must not
        // cancel an acknowledged WSS session whose own stream controls liveness.
        if (!wireless_microphone_has_active_session()) {
            taskENTER_CRITICAL(&s_voice_control_lock);
            stopped = voice_control_stop_from_remote(&s_voice_control, poll_started_at_us,
                result == ESP_OK && fetched.current_thread.available,
                result == ESP_OK ? fetched.current_thread.id : NULL,
                result == ESP_OK ? fetched.current_thread.voice_state : ATTENTION_VOICE_UNKNOWN);
            taskEXIT_CRITICAL(&s_voice_control_lock);
            if (stopped) {
                voice_audio_set_listening(false);
                attention_audio_set_suppressed(false);
            }
        }
        if (result == ESP_OK) {
            if (has_previous_success && snapshot_should_chime(&previous_success, &fetched)) {
                attention_audio_notify();
            }
            current = fetched;
            previous_success = fetched;
            has_previous_success = true;
        } else {
            snprintf(
                current.source_error,
                sizeof(current.source_error),
                "Request failed: %s",
                esp_err_to_name(result)
            );
            ESP_LOGW(TAG, "%s", current.source_error);
        }
        render_snapshot(&current);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_CODEX_ATTENTION_POLL_INTERVAL_MS));
    }
}

static void detail_task(void *argument)
{
    (void)argument;
    detail_request_t request;

    while (true) {
        if (xQueueReceive(s_detail_queue, &request, portMAX_DELAY) != pdTRUE) continue;

        attention_detail_t detail = { 0 };
        esp_err_t result;
        if (!wifi_manager_wait_connected(8000)) result = ESP_ERR_TIMEOUT;
        else result = attention_client_fetch_detail(request.thread_id, &detail);

        bsp_display_lock(0);
        if (attention_ui_is_detail_for(request.thread_id)) {
            if (result == ESP_OK) {
                attention_ui_render_detail(&detail);
            } else {
                char message[ATTENTION_ERROR_MAX];
                snprintf(message, sizeof(message), "Could not load latest text: %s", esp_err_to_name(result));
                attention_ui_show_detail_error(request.thread_id, message);
            }
        }
        bsp_display_unlock();
    }
}

static void voice_task(void *argument)
{
    (void)argument;
    voice_request_t request;

    while (true) {
        if (xQueueReceive(s_voice_queue, &request, portMAX_DELAY) != pdTRUE) continue;
        if (!wifi_manager_wait_connected(8000)) {
            voice_audio_set_listening(false);
            attention_audio_set_suppressed(false);
            set_voice_ui(request.thread_id, ATTENTION_VOICE_ERROR);
            continue;
        }

        char request_id[97];
        attention_desktop_state_t response = { 0 };
        if (request.kind == VOICE_REQUEST_FOCUS) {
            attention_audio_set_suppressed(true);
            set_voice_ui(request.thread_id, ATTENTION_VOICE_FOCUSING);
            make_request_id(request_id, sizeof(request_id), "focus");
            esp_err_t result = attention_client_focus(request.thread_id, request_id, &response);
            if (result != ESP_OK
                || strcmp(response.request_id, request_id) != 0
                || strcmp(response.thread_id, request.thread_id) != 0) {
                bsp_display_lock(0);
                attention_ui_fixed_focus_failed();
                attention_ui_set_voice_state(request.thread_id, ATTENTION_VOICE_ERROR);
                bsp_display_unlock();
            }
            attention_audio_set_suppressed(false);
            continue;
        }

        if (request.kind == VOICE_REQUEST_MUTE) {
            set_voice_ui(request.thread_id, ATTENTION_VOICE_MUTED);
            if (wireless_microphone_has_active_session()) {
                if (wireless_microphone_stop_session() != ESP_OK) {
                    set_voice_ui(request.thread_id, ATTENTION_VOICE_ERROR);
                }
                attention_audio_set_suppressed(false);
                continue;
            }
            make_request_id(request_id, sizeof(request_id), "mute");
            esp_err_t result = attention_client_voice(
                request.thread_id,
                "mute",
                request_id,
                &response
            );
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Desktop mute acknowledgement failed: %s", esp_err_to_name(result));
            }
            attention_audio_set_suppressed(false);
            continue;
        }

        set_voice_ui(request.thread_id, ATTENTION_VOICE_FOCUSING);
        make_request_id(request_id, sizeof(request_id), "focus");
        esp_err_t result = attention_client_focus(request.thread_id, request_id, &response);
        taskENTER_CRITICAL(&s_voice_control_lock);
        voice_control_action_t action = voice_control_focus_result(
            &s_voice_control,
            result == ESP_OK && strcmp(response.request_id, request_id) == 0,
            response.thread_id
        );
        const bool cancelled = s_voice_control.state == ATTENTION_VOICE_MUTED
            && strcmp(s_voice_control.thread_id, request.thread_id) == 0;
        taskEXIT_CRITICAL(&s_voice_control_lock);
        if (action != VOICE_CONTROL_ACTION_START) {
            voice_audio_set_listening(false);
            attention_audio_set_suppressed(false);
            if (!cancelled) set_voice_ui(request.thread_id, ATTENTION_VOICE_ERROR);
            continue;
        }

        attention_audio_set_suppressed(true);
        set_voice_ui(request.thread_id, ATTENTION_VOICE_STARTING);
        // Give the Desktop deep link a bounded moment to finish switching tasks
        // before sending the global Voice shortcut.
        vTaskDelay(pdMS_TO_TICKS(400));
        taskENTER_CRITICAL(&s_voice_control_lock);
        const bool should_start = s_voice_control.state == ATTENTION_VOICE_STARTING
            && strcmp(s_voice_control.thread_id, request.thread_id) == 0;
        taskEXIT_CRITICAL(&s_voice_control_lock);
        if (!should_start) {
            attention_audio_set_suppressed(false);
            continue;
        }

        if (should_use_wireless_transport()) {
            make_request_id(request_id, sizeof(request_id), "wireless");
            const esp_err_t wireless_result = wireless_microphone_start_session(
                request.thread_id, request_id
            );
            taskENTER_CRITICAL(&s_voice_control_lock);
            const bool wireless_still_requested = s_voice_control.state == ATTENTION_VOICE_STARTING
                && strcmp(s_voice_control.thread_id, request.thread_id) == 0;
            if (wireless_still_requested) {
                voice_control_voice_result(&s_voice_control, wireless_result == ESP_OK, wireless_result == ESP_OK);
                if (wireless_result == ESP_OK) {
                    s_voice_control.recording_started_at_us = (uint64_t)esp_timer_get_time();
                }
            }
            taskEXIT_CRITICAL(&s_voice_control_lock);
            set_voice_ui(
                request.thread_id,
                wireless_still_requested
                    ? (wireless_result == ESP_OK ? ATTENTION_VOICE_LISTENING : ATTENTION_VOICE_ERROR)
                    : ATTENTION_VOICE_MUTED
            );
            if (wireless_result != ESP_OK || !wireless_still_requested) {
                attention_audio_set_suppressed(false);
            }
            continue;
        }

#if CONFIG_CODEX_ATTENTION_VOICE_TRANSPORT_WIFI
        // Wi-Fi-only is an explicit choice. Never silently route it through
        // the legacy USB HTTP path when the paired listener is unavailable.
        set_voice_ui(request.thread_id, ATTENTION_VOICE_ERROR);
        attention_audio_set_suppressed(false);
        continue;
#endif

        make_request_id(request_id, sizeof(request_id), "voice");
        memset(&response, 0, sizeof(response));
        result = attention_client_voice(
            request.thread_id,
            "start-or-resume",
            request_id,
            &response
        );
        const bool acknowledged = result == ESP_OK
            && strcmp(response.request_id, request_id) == 0
            && strcmp(response.thread_id, request.thread_id) == 0
            && response.voice_state == ATTENTION_VOICE_LISTENING;
        taskENTER_CRITICAL(&s_voice_control_lock);
        const bool still_requested = s_voice_control.state == ATTENTION_VOICE_STARTING
            && strcmp(s_voice_control.thread_id, request.thread_id) == 0;
        if (still_requested) {
            voice_control_voice_result(&s_voice_control, acknowledged, acknowledged);
            if (acknowledged) s_voice_control.recording_started_at_us = (uint64_t)esp_timer_get_time();
        }
        taskEXIT_CRITICAL(&s_voice_control_lock);
        voice_audio_set_listening(acknowledged && still_requested);
        if (!acknowledged || !still_requested) attention_audio_set_suppressed(false);
        set_voice_ui(
            request.thread_id,
            still_requested
                ? (acknowledged ? ATTENTION_VOICE_LISTENING : ATTENTION_VOICE_ERROR)
                : ATTENTION_VOICE_MUTED
        );
    }
}

static void button_task(void *argument)
{
    (void)argument;
    button_input_event_t event;

    while (true) {
        reconcile_wireless_failure();
        char expired_thread[ATTENTION_ID_MAX] = { 0 };
        taskENTER_CRITICAL(&s_voice_control_lock);
        if (voice_control_expire(&s_voice_control, (uint64_t)esp_timer_get_time())) {
            strlcpy(expired_thread, s_voice_control.thread_id, sizeof(expired_thread));
        }
        taskEXIT_CRITICAL(&s_voice_control_lock);
        if (expired_thread[0] != '\0') {
            voice_audio_set_listening(false);
            attention_audio_set_suppressed(false);
            set_voice_ui(expired_thread, ATTENTION_VOICE_MUTED);
        }
        if (!button_input_poll(&event)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        bool stop_wireless_after_unlock = false;
        bool queue_voice_request = false;
        voice_request_t queued_voice_request = { 0 };
        bsp_display_lock(0);
        if (event == BUTTON_INPUT_BOOT_SHORT) {
            if (attention_ui_is_settings_visible()) {
                attention_ui_show_list();
            } else if (attention_ui_is_detail_visible()) {
                attention_ui_show_list();
                if (attention_ui_select_next()) (void)attention_ui_activate_selected();
            } else {
                (void)attention_ui_select_next();
            }
        } else if (event == BUTTON_INPUT_PWR_SHORT) {
            if (attention_ui_is_detail_visible() || attention_ui_is_settings_visible()) {
                attention_ui_show_list();
            }
            else (void)attention_ui_activate_selected();
        } else if (event == BUTTON_INPUT_BOOT_LONG || event == BUTTON_INPUT_PWR_LONG) {
            char thread_id[ATTENTION_ID_MAX];
            if (attention_ui_get_voice_target_id(thread_id, sizeof(thread_id))) {
                const bool wireless_active = wireless_microphone_has_active_session();
                // Privacy boundary: close the PCM gate before muting this task
                // or switching Voice to another selected task. The network
                // stop is deferred until after the display lock is released.
                voice_audio_set_listening(false);
                taskENTER_CRITICAL(&s_voice_control_lock);
                const voice_control_action_t action = voice_control_begin_toggle(
                    &s_voice_control,
                    thread_id
                );
                taskEXIT_CRITICAL(&s_voice_control_lock);
                if (action == VOICE_CONTROL_ACTION_MUTE) {
                    attention_ui_set_voice_state(thread_id, ATTENTION_VOICE_MUTED);
                }
                queued_voice_request = (voice_request_t){
                    .kind = action == VOICE_CONTROL_ACTION_MUTE
                        ? VOICE_REQUEST_MUTE
                        : VOICE_REQUEST_START,
                };
                strlcpy(queued_voice_request.thread_id, thread_id, sizeof(queued_voice_request.thread_id));
                stop_wireless_after_unlock = wireless_active;
                queue_voice_request = true;
            }
        }
        bsp_display_unlock();
        if (stop_wireless_after_unlock) {
            const esp_err_t stop_result = wireless_microphone_stop_session();
            attention_audio_set_suppressed(false);
            queue_voice_request = false;
            if (stop_result != ESP_OK) {
                set_voice_ui(queued_voice_request.thread_id, ATTENTION_VOICE_ERROR);
            }
        }
        if (queue_voice_request) (void)xQueueSend(s_voice_queue, &queued_voice_request, 0);
    }
}

static void create_task_or_log(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    UBaseType_t priority
)
{
    if (xTaskCreate(task, name, stack_depth, NULL, priority, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create %s task", name);
    }
}

void app_main(void)
{
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    lv_display_t *display = attention_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "Could not start Waveshare display BSP");
        return;
    }
    ESP_ERROR_CHECK(bsp_display_backlight_on());

    s_detail_queue = xQueueCreate(1, sizeof(detail_request_t));
    s_voice_queue = xQueueCreate(4, sizeof(voice_request_t));
    if (s_detail_queue == NULL || s_voice_queue == NULL) {
        ESP_LOGE(TAG, "Could not create request queues");
        return;
    }

    voice_control_init(&s_voice_control);

    bsp_display_lock(0);
    attention_ui_init(queue_detail, queue_focus, NULL);
    bsp_display_unlock();

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
    ESP_ERROR_CHECK(wifi_manager_start());
    audio_result = wireless_microphone_init();
    if (audio_result != ESP_OK && audio_result != ESP_ERR_INVALID_STATE && audio_result != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Wireless microphone disabled: %s", esp_err_to_name(audio_result));
    }

    create_task_or_log(poll_task, "attention_poll", 16384, 5);
    create_task_or_log(detail_task, "attention_detail", 16384, 5);
    create_task_or_log(voice_task, "desktop_voice", 12288, 6);
    create_task_or_log(button_task, "attention_buttons", 4096, 6);
}
