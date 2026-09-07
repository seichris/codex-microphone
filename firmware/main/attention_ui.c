#include "attention_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "nvs.h"

static lv_obj_t *s_pairing_prompt;
static lv_obj_t *s_pairing_prompt_text;
static lv_obj_t *s_list_view;
static lv_obj_t *s_detail_view;
static lv_obj_t *s_settings_view;
static lv_obj_t *s_wireless_status;
static lv_obj_t *s_count_label;
static lv_obj_t *s_status_dot;
static lv_obj_t *s_list;
static lv_obj_t *s_current_card;
static lv_obj_t *s_current_title;
static lv_obj_t *s_current_meta;
static lv_obj_t *s_current_voice;
static lv_obj_t *s_cards[CONFIG_CODEX_ATTENTION_MAX_ITEMS];
static lv_obj_t *s_card_titles[CONFIG_CODEX_ATTENTION_MAX_ITEMS];
static lv_obj_t *s_detail_title;
static lv_obj_t *s_detail_meta;
static lv_obj_t *s_detail_text;
static lv_obj_t *s_detail_body;
static lv_obj_t *s_detail_voice;
static lv_obj_t *s_settings_title_example;
static lv_obj_t *s_settings_subtitle_example;
static lv_obj_t *s_settings_title_size;
static lv_obj_t *s_settings_subtitle_size;
static lv_obj_t *s_settings_title_up;
static lv_obj_t *s_settings_title_down;
static lv_obj_t *s_settings_subtitle_up;
static lv_obj_t *s_settings_subtitle_down;
static attention_snapshot_t s_snapshot;
static uint32_t s_selected_index;
static bool s_selected_is_current;
static bool s_current_armed;
static char s_selected_id[ATTENTION_ID_MAX];
static char s_detail_id[ATTENTION_ID_MAX];
static attention_ui_open_callback_t s_open_callback;
static attention_ui_focus_callback_t s_focus_callback;
static void *s_open_context;

static const lv_color_t COLOR_BG = LV_COLOR_MAKE(8, 10, 14);
static const lv_color_t COLOR_BORDER = LV_COLOR_MAKE(51, 61, 76);
static const lv_color_t COLOR_TEXT = LV_COLOR_MAKE(242, 245, 249);
static const lv_color_t COLOR_MUTED = LV_COLOR_MAKE(151, 162, 179);
static const lv_color_t COLOR_AMBER = LV_COLOR_MAKE(242, 190, 80);
static const lv_color_t COLOR_BLUE = LV_COLOR_MAKE(116, 170, 245);
static const lv_color_t COLOR_GREEN = LV_COLOR_MAKE(101, 212, 183);
static const lv_color_t COLOR_RED = LV_COLOR_MAKE(255, 139, 151);

#define CARD_TITLE_SUBTITLE_GAP 4
#define LIST_ITEM_GAP (CARD_TITLE_SUBTITLE_GAP * 4)
#define LIST_HEADER_TITLE_INSET 60
#define LIST_HEADER_EDGE_INSET 10
#define SETTINGS_BUTTON_SIZE 42
#define STATUS_ICON_SIZE 24
#define STATUS_ICON_FONT (&lv_font_montserrat_24)
#define LIST_HEADER_SETTINGS_INSET (LIST_HEADER_EDGE_INSET + SETTINGS_BUTTON_SIZE)
#define DETAIL_TEXT_TOP_INSET 4
#define DETAIL_BOTTOM_MARGIN 10
#define CURRENT_CARD_HEIGHT 80
#define CURRENT_CARD_WIDTH 390
#define CURRENT_CARD_BORDER 2
#define CURRENT_CARD_PADDING 10
#define CURRENT_CARD_LEFT_PADDING (LIST_HEADER_TITLE_INSET - LIST_HEADER_EDGE_INSET - CURRENT_CARD_BORDER)
#define CURRENT_CARD_TITLE_WIDTH (CURRENT_CARD_WIDTH - CURRENT_CARD_BORDER * 2 - CURRENT_CARD_LEFT_PADDING - CURRENT_CARD_PADDING - STATUS_ICON_SIZE - 8)
#define CURRENT_CARD_BOTTOM 8
#define LIST_VIEW_GAP 6
#define SETTINGS_NVS_NAMESPACE "attention_ui"
#define SETTINGS_NVS_TITLE_KEY "title_font"
#define SETTINGS_NVS_SUBTITLE_KEY "subtitle_font"

typedef struct {
    uint8_t size;
    const lv_font_t *font;
} font_option_t;

static const font_option_t TITLE_FONT_OPTIONS[] = {
    { 16, &lv_font_montserrat_16 },
    { 18, &lv_font_montserrat_18 },
    { 20, &lv_font_montserrat_20 },
    { 22, &lv_font_montserrat_22 },
    { 24, &lv_font_montserrat_24 },
    { 26, &lv_font_montserrat_26 },
    { 28, &lv_font_montserrat_28 },
    { 30, &lv_font_montserrat_30 },
    { 32, &lv_font_montserrat_32 },
    { 34, &lv_font_montserrat_34 },
    { 36, &lv_font_montserrat_36 },
};

static const font_option_t SUBTITLE_FONT_OPTIONS[] = {
    { 14, &lv_font_montserrat_14 },
    { 16, &lv_font_montserrat_16 },
    { 18, &lv_font_montserrat_18 },
    { 20, &lv_font_montserrat_20 },
    { 22, &lv_font_montserrat_22 },
    { 24, &lv_font_montserrat_24 },
    { 26, &lv_font_montserrat_26 },
    { 28, &lv_font_montserrat_28 },
};

static uint8_t s_title_font_index = 6;
static uint8_t s_subtitle_font_index = 4;

enum {
    SETTINGS_ACTION_TITLE_UP = 1,
    SETTINGS_ACTION_TITLE_DOWN,
    SETTINGS_ACTION_SUBTITLE_UP,
    SETTINGS_ACTION_SUBTITLE_DOWN,
};

static void update_settings_controls(void);
static void position_detail_body(void);
static void update_current_card(void);
static void update_detail_voice(void);

static void set_common_text(lv_obj_t *label, const lv_font_t *font, lv_color_t color)
{
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
}

static const font_option_t *current_title_option(void)
{
    return &TITLE_FONT_OPTIONS[s_title_font_index];
}

static const font_option_t *current_subtitle_option(void)
{
    return &SUBTITLE_FONT_OPTIONS[s_subtitle_font_index];
}

static uint8_t find_font_index(
    const font_option_t *options,
    size_t option_count,
    uint8_t size,
    uint8_t fallback
)
{
    for (size_t index = 0; index < option_count; ++index) {
        if (options[index].size == size) return (uint8_t)index;
    }
    return fallback;
}

static void load_font_settings(void)
{
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;

    uint8_t size = 0;
    if (nvs_get_u8(handle, SETTINGS_NVS_TITLE_KEY, &size) == ESP_OK) {
        s_title_font_index = find_font_index(
            TITLE_FONT_OPTIONS,
            sizeof(TITLE_FONT_OPTIONS) / sizeof(TITLE_FONT_OPTIONS[0]),
            size,
            s_title_font_index
        );
    }
    if (nvs_get_u8(handle, SETTINGS_NVS_SUBTITLE_KEY, &size) == ESP_OK) {
        s_subtitle_font_index = find_font_index(
            SUBTITLE_FONT_OPTIONS,
            sizeof(SUBTITLE_FONT_OPTIONS) / sizeof(SUBTITLE_FONT_OPTIONS[0]),
            size,
            s_subtitle_font_index
        );
    }
    nvs_close(handle);
}

static void save_font_settings(void)
{
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;

    const esp_err_t title_result = nvs_set_u8(
        handle,
        SETTINGS_NVS_TITLE_KEY,
        current_title_option()->size
    );
    const esp_err_t subtitle_result = nvs_set_u8(
        handle,
        SETTINGS_NVS_SUBTITLE_KEY,
        current_subtitle_option()->size
    );
    if (title_result == ESP_OK && subtitle_result == ESP_OK) (void)nvs_commit(handle);
    nvs_close(handle);
}

static void set_step_state(lv_obj_t *button, bool enabled)
{
    if (button == NULL) return;
    if (enabled) lv_obj_clear_state(button, LV_STATE_DISABLED);
    else lv_obj_add_state(button, LV_STATE_DISABLED);
}

static void apply_font_settings(void)
{
    const font_option_t *title = current_title_option();
    const font_option_t *subtitle = current_subtitle_option();

    for (uint32_t index = 0; index < CONFIG_CODEX_ATTENTION_MAX_ITEMS; ++index) {
        if (s_card_titles[index] != NULL) set_common_text(s_card_titles[index], title->font, COLOR_TEXT);
    }
    if (s_detail_title != NULL) set_common_text(s_detail_title, title->font, COLOR_TEXT);
    if (s_current_title != NULL) set_common_text(s_current_title, &lv_font_montserrat_20, COLOR_TEXT);
    if (s_detail_meta != NULL) set_common_text(s_detail_meta, &lv_font_montserrat_16, COLOR_MUTED);
    if (s_settings_title_example != NULL) {
        set_common_text(s_settings_title_example, title->font, COLOR_TEXT);
        set_common_text(s_settings_subtitle_example, subtitle->font, COLOR_TEXT);
    }
    update_settings_controls();
}

static void make_root(lv_obj_t *root)
{
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_bg_color(root, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
}

static void settings_icon_clicked(lv_event_t *event);
static void settings_back_clicked(lv_event_t *event);
static void settings_step_clicked(lv_event_t *event);

static void style_icon_button(lv_obj_t *button)
{
    lv_obj_set_style_radius(button, 0, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 0, LV_STATE_PRESSED);
}

static lv_obj_t *create_action_button(
    lv_obj_t *parent,
    const char *text,
    lv_coord_t width,
    lv_coord_t height,
    const lv_font_t *font,
    lv_color_t color,
    lv_event_cb_t callback,
    void *user_data
)
{
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_40, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, COLOR_MUTED, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    set_common_text(label, font, color);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *create_badge(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    set_common_text(label, &lv_font_montserrat_18, color);
    return label;
}

static void format_age(uint32_t seconds, char *buffer, size_t size)
{
    if (seconds < 60) snprintf(buffer, size, "now");
    else if (seconds < 3600) snprintf(buffer, size, "%lum", (unsigned long)(seconds / 60));
    else if (seconds < 86400) snprintf(buffer, size, "%luh", (unsigned long)(seconds / 3600));
    else snprintf(buffer, size, "%lud", (unsigned long)(seconds / 86400));
}

static int32_t find_item(const char *thread_id)
{
    if (thread_id == NULL || thread_id[0] == '\0') return -1;
    for (uint32_t index = 0; index < s_snapshot.count; ++index) {
        if (strcmp(s_snapshot.items[index].id, thread_id) == 0) return (int32_t)index;
    }
    return -1;
}

static lv_color_t voice_state_color(attention_voice_state_t state)
{
    if (state == ATTENTION_VOICE_LISTENING) return COLOR_GREEN;
    if (state == ATTENTION_VOICE_ERROR) return COLOR_RED;
    if (state == ATTENTION_VOICE_FOCUSING || state == ATTENTION_VOICE_STARTING) return COLOR_AMBER;
    return COLOR_MUTED;
}

// Draw the microphone directly so it does not depend on an optional font glyph.
static void draw_microphone(lv_event_t *event)
{
    lv_obj_t *icon = lv_event_get_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t bounds;
    lv_obj_get_coords(icon, &bounds);
    const lv_color_t color = lv_obj_get_style_text_color(icon, 0);
    const int32_t x = bounds.x1;
    const int32_t y = bounds.y1;

    lv_draw_rect_dsc_t capsule;
    lv_draw_rect_dsc_init(&capsule);
    capsule.bg_opa = LV_OPA_TRANSP;
    capsule.border_color = color;
    capsule.border_width = 2;
    capsule.radius = 5;
    const lv_area_t body = {x + 7, y + 1, x + 16, y + 14};
    lv_draw_rect(layer, &capsule, &body);

    lv_draw_arc_dsc_t cradle;
    lv_draw_arc_dsc_init(&cradle);
    cradle.color = color;
    cradle.width = 2;
    cradle.center = (lv_point_t){x + 12, y + 11};
    cradle.radius = 10;
    cradle.start_angle = 0;
    cradle.end_angle = 180;
    lv_draw_arc(layer, &cradle);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.width = 2;
    const int32_t segments[][4] = {{3, 8, 3, 11}, {21, 8, 21, 11},
                                  {12, 20, 12, 23}, {7, 23, 17, 23}};
    for (unsigned i = 0; i < sizeof(segments) / sizeof(segments[0]); ++i) {
        line.p1 = (lv_point_precise_t){x + segments[i][0], y + segments[i][1]};
        line.p2 = (lv_point_precise_t){x + segments[i][2], y + segments[i][3]};
        lv_draw_line(layer, &line);
    }
}

static lv_obj_t *create_microphone(lv_obj_t *parent)
{
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(icon, STATUS_ICON_SIZE, STATUS_ICON_SIZE);
    lv_obj_set_style_text_color(icon, COLOR_MUTED, 0);
    lv_obj_add_event_cb(icon, draw_microphone, LV_EVENT_DRAW_MAIN, NULL);
    return icon;
}

static void position_microphone(lv_obj_t *icon, lv_obj_t *title)
{
    lv_obj_update_layout(title);
    lv_obj_align_to(icon, title, LV_ALIGN_OUT_RIGHT_TOP, 8, 0);
    lv_obj_invalidate(icon);
}

static void apply_selection(bool scroll)
{
    for (uint32_t index = 0; index < s_snapshot.count; ++index) {
        lv_obj_t *card = s_cards[index];
        if (card == NULL) continue;
        const bool selected = !s_selected_is_current && index == s_selected_index;
        if (s_card_titles[index] != NULL) {
            lv_obj_set_style_text_color(
                s_card_titles[index],
                selected ? COLOR_GREEN : COLOR_TEXT,
                0
            );
        }
    }


    if (s_current_card != NULL) {
        lv_obj_set_style_border_color(
            s_current_card,
            s_selected_is_current ? COLOR_GREEN : COLOR_BORDER,
            0
        );
        if (s_current_title != NULL) {
            lv_obj_set_style_text_color(
                s_current_title,
                s_selected_is_current ? COLOR_GREEN : COLOR_TEXT,
                0
            );
        }
    }

    if (scroll && s_snapshot.count > 0 && s_selected_index < s_snapshot.count
        && s_cards[s_selected_index] != NULL) {
        lv_obj_scroll_to_view(s_cards[s_selected_index], LV_ANIM_ON);
    }
}

static void select_index(uint32_t index, bool scroll)
{
    if (s_snapshot.count == 0) {
        s_selected_index = 0;
        s_selected_id[0] = '\0';
        return;
    }
    s_selected_index = index % s_snapshot.count;
    s_selected_is_current = false;
    s_current_armed = false;
    strlcpy(s_selected_id, s_snapshot.items[s_selected_index].id, sizeof(s_selected_id));
    apply_selection(scroll);
}

static void select_current(void)
{
    if (!s_snapshot.current_thread.available) return;
    s_selected_is_current = true;
    strlcpy(s_selected_id, s_snapshot.current_thread.id, sizeof(s_selected_id));
    apply_selection(false);
}

static void card_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    const uintptr_t encoded = (uintptr_t)lv_event_get_user_data(event);
    if (encoded == 0) return;
    const uint32_t index = (uint32_t)(encoded - 1U);
    if (index >= s_snapshot.count) return;
    select_index(index, false);
    if (s_open_callback != NULL) s_open_callback(s_selected_id, s_open_context);
}

static void current_card_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !s_snapshot.current_thread.available) return;
    if (s_selected_is_current && s_current_armed
        && strcmp(s_selected_id, s_snapshot.current_thread.id) == 0) {
        s_current_armed = false;
        if (s_open_callback != NULL) s_open_callback(s_selected_id, s_open_context);
        return;
    }

    select_current();
    s_current_armed = true;
    if (s_focus_callback != NULL) s_focus_callback(s_selected_id, s_open_context);
}

static void create_current_card(void)
{
    s_current_card = lv_obj_create(s_list_view);
    lv_obj_remove_flag(s_current_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_current_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_current_card, CURRENT_CARD_WIDTH, CURRENT_CARD_HEIGHT);
    lv_obj_set_flex_grow(s_current_card, 0);
    lv_obj_set_style_radius(s_current_card, 10, 0);
    lv_obj_set_style_bg_color(s_current_card, lv_color_make(18, 23, 31), 0);
    lv_obj_set_style_bg_opa(s_current_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_current_card, CURRENT_CARD_BORDER, 0);
    lv_obj_set_style_border_color(s_current_card, COLOR_BORDER, 0);
    lv_obj_set_style_pad_all(s_current_card, CURRENT_CARD_PADDING, 0);
    lv_obj_set_style_pad_left(s_current_card, CURRENT_CARD_LEFT_PADDING, 0);
    lv_obj_add_event_cb(s_current_card, current_card_clicked, LV_EVENT_CLICKED, NULL);

    s_current_voice = create_microphone(s_current_card);

    s_current_title = lv_label_create(s_current_card);
    lv_obj_set_width(s_current_title, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_current_title, CURRENT_CARD_TITLE_WIDTH, 0);
    lv_label_set_long_mode(s_current_title, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_current_title, "Connecting to the Mac");
    set_common_text(s_current_title, &lv_font_montserrat_20, COLOR_TEXT);
    lv_obj_align(s_current_title, LV_ALIGN_TOP_LEFT, 0, 2);

    s_current_meta = lv_label_create(s_current_card);
    lv_obj_set_width(s_current_meta, lv_pct(100));
    lv_label_set_long_mode(s_current_meta, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_current_meta, "Waiting for bridge status");
    set_common_text(s_current_meta, &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_align(s_current_meta, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

void attention_ui_show_startup_status(const char *message, bool failed)
{
    if (s_current_card == NULL) return;
    lv_label_set_text(s_current_title, failed ? "Device startup failed" : "Starting device");
    lv_label_set_text(s_current_meta, message);
}

static void update_current_card(void)
{
    if (s_current_card == NULL) return;
    const attention_current_thread_t *current = &s_snapshot.current_thread;
    if (!current->available) {
        if (s_snapshot.source_error[0] != '\0') {
            lv_label_set_text(s_current_title, "Bridge data unavailable");
            lv_label_set_text(s_current_meta, s_snapshot.source_error);
        } else if (s_snapshot.desktop_control_availability == ATTENTION_DESKTOP_CONTROL_UNKNOWN) {
            lv_label_set_text(s_current_title, "Controller status unknown");
            lv_label_set_text(s_current_meta, "Check the Mac bridge and companion");
        } else if (s_snapshot.desktop_control_availability == ATTENTION_DESKTOP_CONTROL_UNAVAILABLE) {
            lv_label_set_text(s_current_title, "Voice controller unavailable");
            lv_label_set_text(s_current_meta, "Start the Mac companion");
        } else {
            // No target ID is not evidence that no task is open on the Mac.
            lv_label_set_text(s_current_title, "Mac selection is unknown");
            lv_label_set_text(s_current_meta, s_snapshot.capabilities.desktop_focus
                ? "For Voice, hold a button on a list task"
                : "Desktop focus needs setup on the Mac");
        }
        lv_obj_set_style_text_color(s_current_voice, COLOR_MUTED, 0);
        position_microphone(s_current_voice, s_current_title);
        lv_obj_add_state(s_current_card, LV_STATE_DISABLED);
        s_current_armed = false;
        if (s_selected_is_current) s_selected_is_current = false;
        return;
    }

    lv_obj_clear_state(s_current_card, LV_STATE_DISABLED);
    lv_label_set_text(s_current_title, current->title);
    lv_label_set_text(s_current_meta, current->project);
    position_microphone(s_current_voice, s_current_title);
    lv_obj_set_style_text_color(s_current_voice, voice_state_color(current->voice_state), 0);
}

static lv_obj_t *create_card(const attention_item_t *item, uint32_t index)
{
    lv_obj_t *card = lv_obj_create(s_list);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, 0, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, CARD_TITLE_SUBTITLE_GAP, 0);
    lv_obj_set_style_pad_row(card, CARD_TITLE_SUBTITLE_GAP, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(card, card_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)(index + 1U));

    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title, item->title);
    set_common_text(title, current_title_option()->font, COLOR_TEXT);
    s_card_titles[index] = title;

    char age[16];
    char meta[ATTENTION_PROJECT_MAX + 24];
    format_age(item->age_seconds, age, sizeof(age));
    snprintf(meta, sizeof(meta), "%s  |  %s", item->project, age);
    lv_obj_t *meta_label = lv_label_create(card);
    lv_obj_set_width(meta_label, lv_pct(100));
    lv_label_set_long_mode(meta_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(meta_label, meta);
    set_common_text(meta_label, current_subtitle_option()->font, COLOR_MUTED);

    lv_obj_t *badges = lv_obj_create(card);
    lv_obj_remove_flag(badges, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(badges, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(badges, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(badges, 0, 0);
    lv_obj_set_style_pad_all(badges, 0, 0);
    lv_obj_set_style_pad_column(badges, 6, 0);
    lv_obj_set_style_pad_row(badges, 5, 0);
    lv_obj_set_flex_flow(badges, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(badges, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    if (item->status == ATTENTION_STATUS_WAITING_APPROVAL) create_badge(badges, "APPROVAL", COLOR_AMBER);
    if (item->status == ATTENTION_STATUS_WAITING_INPUT) create_badge(badges, "INPUT", COLOR_AMBER);
    if (item->new_result) create_badge(badges, "NEW", COLOR_BLUE);
    if (item->status == ATTENTION_STATUS_RUNNING) create_badge(badges, "RUNNING", COLOR_GREEN);
    if (item->status == ATTENTION_STATUS_ERROR) create_badge(badges, "ERROR", COLOR_RED);
    return card;
}

static void create_list_view(lv_obj_t *screen)
{
    s_list_view = lv_obj_create(screen);
    make_root(s_list_view);
    // Budget the viewport between fixed siblings. Only the list grows and
    // scrolls; its content height must never size the outer screen.
    lv_obj_set_flex_flow(s_list_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list_view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_list_view, LIST_VIEW_GAP, 0);
    lv_obj_set_style_pad_bottom(s_list_view, CURRENT_CARD_BOTTOM, 0);

    lv_obj_t *header = lv_obj_create(s_list_view);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(header, lv_pct(100), 54);
    lv_obj_set_flex_grow(header, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_style_pad_left(header, LIST_HEADER_TITLE_INSET, 0);
    lv_obj_set_style_pad_right(header, LIST_HEADER_SETTINGS_INSET, 0);
    lv_obj_set_style_pad_column(header, 10, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cluster = lv_obj_create(header);
    lv_obj_remove_flag(cluster, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cluster, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cluster, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cluster, 0, 0);
    lv_obj_set_style_pad_all(cluster, 0, 0);
    lv_obj_set_style_pad_column(cluster, 8, 0);
    lv_obj_set_flex_flow(cluster, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cluster, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    s_status_dot = lv_obj_create(cluster);
    lv_obj_remove_flag(s_status_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_status_dot, 11, 11);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_dot, COLOR_RED, 0);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_status_dot, 0, 0);

    lv_obj_t *title = lv_label_create(cluster);
    lv_label_set_text(title, "Pinned, Unread");
    set_common_text(title, &lv_font_montserrat_16, COLOR_TEXT);

    s_count_label = lv_label_create(cluster);
    lv_obj_set_width(s_count_label, 40);
    lv_label_set_text(s_count_label, "(--)");
    set_common_text(s_count_label, &lv_font_montserrat_16, COLOR_TEXT);

    lv_obj_t *settings_button = create_action_button(
        header,
        LV_SYMBOL_SETTINGS,
        SETTINGS_BUTTON_SIZE,
        SETTINGS_BUTTON_SIZE,
        STATUS_ICON_FONT,
        COLOR_TEXT,
        settings_icon_clicked,
        NULL
    );
    style_icon_button(settings_button);

    lv_obj_t *divider = lv_obj_create(s_list_view);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(divider, 390, 1);
    lv_obj_set_flex_grow(divider, 0);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_style_bg_color(divider, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);

    s_list = lv_obj_create(s_list_view);
    lv_obj_set_size(s_list, 390, 0);
    lv_obj_set_style_min_height(s_list, 0, 0);
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, LIST_ITEM_GAP, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    create_current_card();
}

static void create_detail_view(lv_obj_t *screen)
{
    s_detail_view = lv_obj_create(screen);
    make_root(s_detail_view);
    lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *header = lv_obj_create(s_detail_view);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(header, 382, 160);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 14, 0);

    s_detail_title = lv_label_create(header);
    lv_obj_set_width(s_detail_title, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_detail_title, 318, 0);
    lv_label_set_long_mode(s_detail_title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_detail_title, "Loading…");
    set_common_text(s_detail_title, current_title_option()->font, COLOR_TEXT);
    lv_obj_align(s_detail_title, LV_ALIGN_TOP_LEFT, 0, 26);

    s_detail_meta = lv_label_create(header);
    lv_obj_set_width(s_detail_meta, 350);
    lv_label_set_long_mode(s_detail_meta, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_detail_meta, "Codex");
    lv_obj_set_style_text_align(s_detail_meta, LV_TEXT_ALIGN_CENTER, 0);
    set_common_text(s_detail_meta, &lv_font_montserrat_16, COLOR_MUTED);
    lv_obj_align(s_detail_meta, LV_ALIGN_TOP_MID, 0, 0);

    s_detail_voice = create_microphone(header);

    s_detail_body = lv_obj_create(s_detail_view);
    lv_obj_set_size(s_detail_body, 382, 312);
    lv_obj_align(s_detail_body, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_radius(s_detail_body, 0, 0);
    lv_obj_set_style_bg_opa(s_detail_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_detail_body, 0, 0);
    lv_obj_set_style_pad_all(s_detail_body, 16, 0);
    lv_obj_set_style_pad_top(s_detail_body, DETAIL_TEXT_TOP_INSET, 0);
    lv_obj_set_scroll_dir(s_detail_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_detail_body, LV_SCROLLBAR_MODE_AUTO);

    s_detail_text = lv_label_create(s_detail_body);
    lv_obj_set_width(s_detail_text, 348);
    lv_label_set_long_mode(s_detail_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_detail_text, "Loading latest text…");
    lv_obj_set_style_text_line_space(s_detail_text, 10, 0);
    set_common_text(s_detail_text, &lv_font_montserrat_20, COLOR_TEXT);
    position_detail_body();
}

static void position_detail_body(void)
{
    if (s_detail_view == NULL || s_detail_title == NULL || s_detail_body == NULL) return;

    lv_obj_t *header = lv_obj_get_parent(s_detail_title);
    lv_obj_update_layout(s_detail_view);
    lv_obj_update_layout(header);
    lv_obj_update_layout(s_detail_title);
    position_microphone(s_detail_voice, s_detail_title);

    const lv_coord_t header_y = lv_obj_get_y(header);
    const lv_coord_t title_y = lv_obj_get_y(s_detail_title);
    const lv_coord_t title_height = lv_obj_get_height(s_detail_title);
    const lv_coord_t body_y = header_y + title_y + title_height;
    const lv_coord_t view_height = lv_obj_get_height(s_detail_view);
    const lv_coord_t body_height = view_height > body_y + DETAIL_BOTTOM_MARGIN
        ? view_height - body_y - DETAIL_BOTTOM_MARGIN
        : 1;

    lv_obj_align(s_detail_body, LV_ALIGN_TOP_MID, 0, body_y);
    lv_obj_set_height(s_detail_body, body_height);
}

static void scroll_detail_to_end(void)
{
    position_detail_body();
    lv_obj_update_layout(s_detail_body);
    lv_obj_scroll_to_y(s_detail_body, LV_COORD_MAX, LV_ANIM_OFF);
}

static void update_detail_voice(void)
{
    if (s_detail_voice == NULL) return;
    attention_voice_state_t state = ATTENTION_VOICE_UNKNOWN;
    if (s_snapshot.current_thread.available
        && strcmp(s_detail_id, s_snapshot.current_thread.id) == 0) {
        state = s_snapshot.current_thread.voice_state;
    }
    position_microphone(s_detail_voice, s_detail_title);
    lv_obj_set_style_text_color(s_detail_voice, voice_state_color(state), 0);
}

static void create_settings_font_row(
    lv_obj_t *parent,
    lv_coord_t y,
    const char *caption,
    const char *example,
    bool title_row
)
{
    lv_obj_t *caption_label = lv_label_create(parent);
    lv_label_set_text(caption_label, caption);
    set_common_text(caption_label, &lv_font_montserrat_16, COLOR_MUTED);
    lv_obj_align(caption_label, LV_ALIGN_TOP_LEFT, 16, y);

    lv_obj_t *example_label = lv_label_create(parent);
    lv_obj_set_width(example_label, 250);
    lv_label_set_long_mode(example_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(example_label, example);
    set_common_text(
        example_label,
        title_row ? current_title_option()->font : current_subtitle_option()->font,
        COLOR_TEXT
    );
    lv_obj_align(example_label, LV_ALIGN_TOP_LEFT, 16, y + 24);

    lv_obj_t *size_label = lv_label_create(parent);
    lv_obj_set_width(size_label, 80);
    set_common_text(size_label, &lv_font_montserrat_14, COLOR_GREEN);
    lv_obj_align(size_label, LV_ALIGN_TOP_LEFT, 16, y + 112);

    const uintptr_t up_action = title_row
        ? SETTINGS_ACTION_TITLE_UP
        : SETTINGS_ACTION_SUBTITLE_UP;
    const uintptr_t down_action = title_row
        ? SETTINGS_ACTION_TITLE_DOWN
        : SETTINGS_ACTION_SUBTITLE_DOWN;
    lv_obj_t *up_button = create_action_button(
        parent,
        LV_SYMBOL_UP,
        42,
        42,
        &lv_font_montserrat_24,
        COLOR_TEXT,
        settings_step_clicked,
        (void *)up_action
    );
    lv_obj_align(up_button, LV_ALIGN_TOP_RIGHT, -62, y + 28);

    lv_obj_t *down_button = create_action_button(
        parent,
        LV_SYMBOL_DOWN,
        42,
        42,
        &lv_font_montserrat_24,
        COLOR_TEXT,
        settings_step_clicked,
        (void *)down_action
    );
    lv_obj_align(down_button, LV_ALIGN_TOP_RIGHT, -12, y + 28);

    if (title_row) {
        s_settings_title_example = example_label;
        s_settings_title_size = size_label;
        s_settings_title_up = up_button;
        s_settings_title_down = down_button;
    } else {
        s_settings_subtitle_example = example_label;
        s_settings_subtitle_size = size_label;
        s_settings_subtitle_up = up_button;
        s_settings_subtitle_down = down_button;
    }
}

static void create_settings_view(lv_obj_t *screen)
{
    s_settings_view = lv_obj_create(screen);
    make_root(s_settings_view);
    lv_obj_add_flag(s_settings_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back_button = create_action_button(
        s_settings_view,
        LV_SYMBOL_LEFT,
        SETTINGS_BUTTON_SIZE,
        SETTINGS_BUTTON_SIZE,
        &lv_font_montserrat_24,
        COLOR_TEXT,
        settings_back_clicked,
        NULL
    );
    style_icon_button(back_button);
    lv_obj_align(back_button, LV_ALIGN_TOP_LEFT, LIST_HEADER_SETTINGS_INSET, 6);

    lv_obj_t *heading = lv_label_create(s_settings_view);
    lv_obj_set_width(heading, 250);
    lv_label_set_text(heading, "Settings");
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    set_common_text(heading, &lv_font_montserrat_24, COLOR_TEXT);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *content = lv_obj_create(s_settings_view);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(content, 390, 420);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);

    create_settings_font_row(content, 22, "Title size", "Example thread title", true);
    create_settings_font_row(content, 216, "Subtitle size", "Example project  |  2h", false);
    s_wireless_status = lv_label_create(s_settings_view);
    lv_obj_set_width(s_wireless_status, 390);
    lv_label_set_long_mode(s_wireless_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_wireless_status, LV_TEXT_ALIGN_CENTER, 0);
    set_common_text(s_wireless_status, &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_align(s_wireless_status, LV_ALIGN_TOP_MID, 0, 46);
    attention_ui_set_wireless_status("Wi-Fi mic: starting");
    update_settings_controls();
}

void attention_ui_set_wireless_status(const char *message)
{
    if (s_wireless_status != NULL) lv_label_set_text(s_wireless_status, message == NULL ? "" : message);
}

static void update_settings_controls(void)
{
    if (s_settings_title_example == NULL || s_settings_subtitle_example == NULL) return;

    const font_option_t *title = current_title_option();
    const font_option_t *subtitle = current_subtitle_option();
    set_common_text(s_settings_title_example, title->font, COLOR_TEXT);
    set_common_text(s_settings_subtitle_example, subtitle->font, COLOR_MUTED);
    lv_label_set_text_fmt(s_settings_title_size, "%u px", (unsigned)title->size);
    lv_label_set_text_fmt(s_settings_subtitle_size, "%u px", (unsigned)subtitle->size);
    set_step_state(
        s_settings_title_up,
        s_title_font_index + 1U < sizeof(TITLE_FONT_OPTIONS) / sizeof(TITLE_FONT_OPTIONS[0])
    );
    set_step_state(s_settings_title_down, s_title_font_index > 0);
    set_step_state(
        s_settings_subtitle_up,
        s_subtitle_font_index + 1U < sizeof(SUBTITLE_FONT_OPTIONS) / sizeof(SUBTITLE_FONT_OPTIONS[0])
    );
    set_step_state(s_settings_subtitle_down, s_subtitle_font_index > 0);
}

static void settings_step_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    const uintptr_t action = (uintptr_t)lv_event_get_user_data(event);

    if (action == SETTINGS_ACTION_TITLE_UP) {
        if (s_title_font_index + 1U < sizeof(TITLE_FONT_OPTIONS) / sizeof(TITLE_FONT_OPTIONS[0])) {
            s_title_font_index++;
        }
    } else if (action == SETTINGS_ACTION_TITLE_DOWN) {
        if (s_title_font_index > 0) s_title_font_index--;
    } else if (action == SETTINGS_ACTION_SUBTITLE_UP) {
        if (s_subtitle_font_index + 1U < sizeof(SUBTITLE_FONT_OPTIONS) / sizeof(SUBTITLE_FONT_OPTIONS[0])) {
            s_subtitle_font_index++;
        }
    } else if (action == SETTINGS_ACTION_SUBTITLE_DOWN) {
        if (s_subtitle_font_index > 0) s_subtitle_font_index--;
    } else {
        return;
    }

    save_font_settings();
    apply_font_settings();
}

static void settings_icon_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) attention_ui_show_settings();
}

static void settings_back_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) attention_ui_show_list();
}

void attention_ui_init(
    attention_ui_open_callback_t open_callback,
    attention_ui_focus_callback_t focus_callback,
    void *context
)
{
    s_open_callback = open_callback;
    s_focus_callback = focus_callback;
    s_open_context = context;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(s_cards, 0, sizeof(s_cards));
    memset(s_card_titles, 0, sizeof(s_card_titles));
    s_selected_id[0] = '\0';
    s_detail_id[0] = '\0';
    s_selected_is_current = false;
    s_current_armed = false;
    load_font_settings();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    create_list_view(screen);
    create_detail_view(screen);
    create_settings_view(screen);
}

void attention_ui_render(const attention_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;

    char prior_selected[ATTENTION_ID_MAX];
    char prior_current[ATTENTION_ID_MAX];
    strlcpy(prior_selected, s_selected_id, sizeof(prior_selected));
    strlcpy(prior_current, s_snapshot.current_thread.id, sizeof(prior_current));
    s_snapshot = *snapshot;

    if (strcmp(prior_current, s_snapshot.current_thread.id) != 0) s_current_armed = false;

    const int32_t selected = find_item(prior_selected);
    if (s_snapshot.current_thread.available
        && strcmp(prior_selected, s_snapshot.current_thread.id) == 0) {
        s_selected_is_current = true;
        strlcpy(s_selected_id, prior_selected, sizeof(s_selected_id));
    } else if (selected >= 0) {
        s_selected_index = (uint32_t)selected;
        s_selected_is_current = false;
        strlcpy(s_selected_id, prior_selected, sizeof(s_selected_id));
    } else if (s_snapshot.count > 0) {
        s_selected_index = 0;
        s_selected_is_current = false;
        strlcpy(s_selected_id, s_snapshot.items[0].id, sizeof(s_selected_id));
    } else if (s_snapshot.current_thread.available) {
        s_selected_index = 0;
        s_selected_is_current = true;
        strlcpy(s_selected_id, s_snapshot.current_thread.id, sizeof(s_selected_id));
    } else {
        s_selected_index = 0;
        s_selected_is_current = false;
        s_selected_id[0] = '\0';
    }

    char count[20];
    if (snapshot->total_count > 99) snprintf(count, sizeof(count), "99+");
    else snprintf(count, sizeof(count), "%lu", (unsigned long)snapshot->total_count);
    lv_label_set_text_fmt(s_count_label, "(%s)", count);

    const bool status_ok = snapshot->source_error[0] == '\0'
        && snapshot->desktop_state_available;
    lv_obj_set_style_bg_color(s_status_dot, status_ok ? COLOR_GREEN : COLOR_RED, 0);

    const int32_t prior_scroll_y = lv_obj_get_scroll_y(s_list);
    memset(s_cards, 0, sizeof(s_cards));
    memset(s_card_titles, 0, sizeof(s_card_titles));
    lv_obj_clean(s_list);
    if (snapshot->count == 0) {
        lv_obj_t *empty = lv_obj_create(s_list);
        lv_obj_remove_flag(empty, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(empty, lv_pct(100), 150);
        lv_obj_set_style_radius(empty, 0, 0);
        lv_obj_set_style_bg_opa(empty, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(empty, 0, 0);

        lv_obj_t *label = lv_label_create(empty);
        lv_label_set_text(label, snapshot->source_error[0] == '\0'
            ? "Inbox clear\nNo unread, pinned, or waiting threads."
            : "No cached threads\nCheck the Mac bridge and Wi-Fi.");
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(label, 10, 0);
        set_common_text(label, &lv_font_montserrat_20, COLOR_MUTED);
        lv_obj_center(label);
    } else {
        for (uint32_t index = 0; index < snapshot->count; ++index) {
            s_cards[index] = create_card(&snapshot->items[index], index);
        }
        apply_selection(false);
    }

    // lv_obj_clean resets scroll. Restore it only after flex has measured the
    // new cards; bounded scrolling clamps it if the inbox became shorter.
    lv_obj_update_layout(s_list);
    lv_obj_scroll_to_y(s_list, prior_scroll_y, LV_ANIM_OFF);
    update_current_card();
    apply_selection(false);
    update_detail_voice();

    if (attention_ui_is_detail_visible() && find_item(s_detail_id) < 0
        && (!s_snapshot.current_thread.available
            || strcmp(s_detail_id, s_snapshot.current_thread.id) != 0)) {
        attention_ui_show_list();
    }
}

bool attention_ui_select_next(void)
{
    if (s_snapshot.count == 0 && !s_snapshot.current_thread.available) return false;
    if (s_selected_is_current) {
        s_current_armed = false;
        if (s_snapshot.count > 0) select_index(0, true);
        return true;
    }
    if (s_snapshot.current_thread.available && s_selected_index + 1U >= s_snapshot.count) {
        select_current();
        s_current_armed = false;
        return true;
    }
    select_index((s_selected_index + 1U) % s_snapshot.count, true);
    return true;
}

bool attention_ui_activate_selected(void)
{
    if (s_selected_id[0] == '\0') return false;
    s_current_armed = false;
    if (s_open_callback != NULL) s_open_callback(s_selected_id, s_open_context);
    return true;
}

bool attention_ui_get_voice_target_id(char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) return false;
    if (attention_ui_is_settings_visible()) return false;
    const char *target = attention_ui_is_detail_visible() ? s_detail_id : s_selected_id;
    if (target[0] == '\0') return false;
    strlcpy(output, target, output_size);
    return true;
}

void attention_ui_set_voice_state(const char *thread_id, attention_voice_state_t state)
{
    if (thread_id == NULL || !s_snapshot.current_thread.available
        || strcmp(thread_id, s_snapshot.current_thread.id) != 0) return;
    s_snapshot.current_thread.voice_state = state;
    update_current_card();
    update_detail_voice();
}

void attention_ui_fixed_focus_failed(void)
{
    s_current_armed = false;
}

bool attention_ui_get_selected_id(char *output, size_t output_size)
{
    if (output == NULL || output_size == 0 || s_selected_id[0] == '\0') return false;
    strlcpy(output, s_selected_id, output_size);
    return true;
}

void attention_ui_show_list(void)
{
    s_detail_id[0] = '\0';
    s_current_armed = false;
    lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_settings_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    apply_selection(true);
    lv_obj_invalidate(lv_screen_active());
}

void attention_ui_show_settings(void)
{
    s_current_armed = false;
    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_settings_view, LV_OBJ_FLAG_HIDDEN);
    apply_font_settings();
    lv_obj_invalidate(lv_screen_active());
}

void attention_ui_show_detail_loading(const char *thread_id)
{
    if (thread_id == NULL || thread_id[0] == '\0') return;
    strlcpy(s_detail_id, thread_id, sizeof(s_detail_id));

    const int32_t index = find_item(thread_id);
    if (index >= 0) {
        const attention_item_t *item = &s_snapshot.items[index];
        lv_label_set_text(s_detail_title, item->title);
        lv_label_set_text(s_detail_meta, item->project);
    } else if (s_snapshot.current_thread.available
        && strcmp(thread_id, s_snapshot.current_thread.id) == 0) {
        lv_label_set_text(s_detail_title, s_snapshot.current_thread.title);
        lv_label_set_text(s_detail_meta, s_snapshot.current_thread.project);
    } else {
        lv_label_set_text(s_detail_title, "Codex thread");
        lv_label_set_text(s_detail_meta, "Codex");
    }
    lv_label_set_text(s_detail_text, "Loading latest text…");
    update_detail_voice();
    scroll_detail_to_end();
    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_screen_active());
}

void attention_ui_render_detail(const attention_detail_t *detail)
{
    if (detail == NULL || !attention_ui_is_detail_for(detail->id)) return;
    lv_label_set_text(s_detail_title, detail->title);
    lv_label_set_text(s_detail_meta, detail->project);
    lv_label_set_text(s_detail_text, detail->text);
    scroll_detail_to_end();
}

void attention_ui_show_detail_error(const char *thread_id, const char *message)
{
    if (!attention_ui_is_detail_for(thread_id)) return;
    lv_label_set_text(s_detail_text, message == NULL ? "Unknown bridge error" : message);
    scroll_detail_to_end();
}

bool attention_ui_is_detail_visible(void)
{
    return !lv_obj_has_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
}

bool attention_ui_is_settings_visible(void)
{
    return !lv_obj_has_flag(s_settings_view, LV_OBJ_FLAG_HIDDEN);
}

bool attention_ui_is_detail_for(const char *thread_id)
{
    return attention_ui_is_detail_visible()
        && thread_id != NULL
        && strcmp(s_detail_id, thread_id) == 0;
}

// A full-screen overlay consumes touch while the hardware confirmation gate
// consumes buttons. It never changes the selected/current-thread model.
void attention_ui_show_pairing_prompt(const char *message, bool failed)
{
    if (s_pairing_prompt == NULL) {
        s_pairing_prompt = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_pairing_prompt, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_pairing_prompt, COLOR_BG, 0);
        lv_obj_set_style_bg_opa(s_pairing_prompt, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(s_pairing_prompt, 28, 0);
        lv_obj_add_flag(s_pairing_prompt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_pairing_prompt, LV_OBJ_FLAG_SCROLLABLE);
        s_pairing_prompt_text = lv_label_create(s_pairing_prompt);
        lv_obj_set_width(s_pairing_prompt_text, LV_PCT(100));
        lv_label_set_long_mode(s_pairing_prompt_text, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(s_pairing_prompt_text, &lv_font_montserrat_22, 0);
        lv_obj_align(s_pairing_prompt_text, LV_ALIGN_CENTER, 0, 0);
    }
    lv_obj_set_style_text_color(s_pairing_prompt_text, failed ? COLOR_RED : COLOR_TEXT, 0);
    lv_label_set_text(s_pairing_prompt_text, message);
}

void attention_ui_hide_pairing_prompt(void)
{
    if (s_pairing_prompt != NULL) lv_obj_delete(s_pairing_prompt);
    s_pairing_prompt = NULL;
    s_pairing_prompt_text = NULL;
}
