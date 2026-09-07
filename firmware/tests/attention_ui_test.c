#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
// Exercise the production UI with real LVGL, exposing internal objects only in
// this test translation unit. NVS alone is stubbed; no display hardware is used.
#include "../main/attention_ui.c"
#include "display_frame.h"

static uint16_t draw_buffer[DISPLAY_FRAME_WIDTH * DISPLAY_FRAME_HEIGHT];
static uint16_t displayed[DISPLAY_FRAME_WIDTH * DISPLAY_FRAME_HEIGHT];
static uint16_t list_image[DISPLAY_FRAME_WIDTH * DISPLAY_FRAME_HEIGHT];
static uint8_t dma_buffer[DISPLAY_FRAME_STRIP_BYTES];

static int panel_ready(void *context) { (void)context; return 0; }
static int panel_strip(void *context, unsigned y, unsigned rows, const uint8_t *pixels, size_t bytes)
{
    (void)context;
    assert(bytes == rows * DISPLAY_FRAME_WIDTH * 2);
    for (unsigned i = 0; i < rows * DISPLAY_FRAME_WIDTH; ++i) {
        displayed[y * DISPLAY_FRAME_WIDTH + i] = (uint16_t)((pixels[2*i] << 8) | pixels[2*i+1]);
    }
    return 0;
}
static void flush(lv_display_t *display, const lv_area_t *bounds, uint8_t *pixels)
{
    assert(bounds->x1 == 0 && bounds->y1 == 0 && bounds->x2 == 409 && bounds->y2 == 501);
    const display_frame_io_t io = {.wait = panel_ready, .write = panel_strip};
    assert(display_frame_send((const uint16_t *)pixels, dma_buffer, &io) == 0);
    lv_display_flush_ready(display);
}

static lv_area_t area(lv_obj_t *object)
{
    lv_area_t result;
    lv_obj_get_coords(object, &result);
    return result;
}

static void assert_viewport(void)
{
    lv_obj_update_layout(lv_screen_active());
    const lv_area_t list = area(s_list);
    const lv_area_t fixed = area(s_current_card);
    assert(lv_obj_get_height(s_list) >= 250);
    assert(list.y1 >= 55);
    assert(list.y2 < fixed.y1);
    assert(lv_obj_get_height(s_current_card) == 80);
    assert(fixed.y2 == lv_display_get_vertical_resolution(NULL) - 9);
    assert(lv_obj_get_scroll_y(s_list_view) == 0);
    assert(lv_obj_get_parent(s_current_card) == s_list_view);
    assert(area(s_current_title).x1 == area(s_status_dot).x1);
    assert(area(s_current_meta).x1 == area(s_status_dot).x1);
    lv_font_glyph_dsc_t settings_glyph;
    assert(lv_font_get_glyph_dsc(STATUS_ICON_FONT, &settings_glyph, 0xf013, 0));
    assert(lv_obj_get_height(s_current_voice) == settings_glyph.box_h);
    assert(lv_obj_get_width(s_current_voice) == STATUS_ICON_SIZE);
}

static void settle_scroll(void)
{
    lv_tick_inc(1000);
    lv_timer_handler();
    lv_obj_update_layout(lv_screen_active());
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(410, 502);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    attention_ui_init(NULL, NULL, NULL);
    attention_ui_set_wireless_status("Wi-Fi mic: TLS -32512 / ESP 32769 / HTTP 0");
    assert(strcmp(lv_label_get_text(s_wireless_status), "Wi-Fi mic: TLS -32512 / ESP 32769 / HTTP 0") == 0);
    lv_obj_update_layout(lv_screen_active());
    assert(area(s_wireless_status).y2 < 94);
    attention_ui_show_startup_status("Starting secure connection", false);
    assert(strcmp(lv_label_get_text(s_current_title), "Starting device") == 0);
    assert(strcmp(lv_label_get_text(s_current_meta), "Starting secure connection") == 0);
    attention_ui_show_startup_status("Cannot start attention_poll: out of memory", true);
    assert(strcmp(lv_label_get_text(s_current_title), "Device startup failed") == 0);
    assert(strcmp(lv_label_get_text(s_current_meta), "Cannot start attention_poll: out of memory") == 0);
    attention_snapshot_t snapshot = { .count = 8, .total_count = 8, .desktop_state_available = true };
    for (unsigned i = 0; i < CONFIG_CODEX_ATTENTION_MAX_ITEMS; ++i) {
        snprintf(snapshot.items[i].id, sizeof(snapshot.items[i].id), "thread-%016u", i);
        snprintf(snapshot.items[i].title, sizeof(snapshot.items[i].title),
            "Thread %u: investigate display layout and voice status", i);
        strlcpy(snapshot.items[i].project, "codex-esp32-display", sizeof(snapshot.items[i].project));
    }
    attention_ui_render(&snapshot);
    assert(strcmp(lv_label_get_text(s_current_title), "Device startup failed") != 0);
    assert_viewport();
    assert(lv_obj_get_height(s_list) == 341);
    settle_scroll();
    lv_refr_now(display);
    memcpy(list_image, displayed, sizeof(list_image));
    unsigned text_pixels = 0;
    for (int y = 75; y < 340; ++y) for (int x = 20; x < 390; ++x) {
        if (displayed[y * 410 + x] != lv_color_to_u16(COLOR_BG)) ++text_pixels;
    }
    assert(text_pixels > 1000);
    attention_ui_show_detail_loading(snapshot.items[0].id);
    attention_detail_t detail = {0};
    strlcpy(detail.id, snapshot.items[0].id, sizeof(detail.id));
    strlcpy(detail.title, "Detail screen", sizeof(detail.title));
    strlcpy(detail.text, "DETAIL TEXT MUST DISAPPEAR\n\nReturning to the list must replace every pixel, including the background.", sizeof(detail.text));
    attention_ui_render_detail(&detail);
    settle_scroll();
    lv_refr_now(display);
    assert(memcmp(list_image, displayed, sizeof(list_image)) != 0);
    attention_ui_show_list();
    settle_scroll();
    lv_refr_now(display);
    assert(memcmp(list_image, displayed, sizeof(list_image)) == 0);
    attention_ui_show_settings();
    settle_scroll();
    attention_ui_show_list();
    settle_scroll();
    lv_refr_now(display);
    assert(memcmp(list_image, displayed, sizeof(list_image)) == 0);
    const lv_area_t fixed = area(s_current_card);
    assert(area(s_card_titles[0]).y1 >= area(s_list).y1);
    assert(area(s_card_titles[0]).y2 <= area(s_list).y2);
    for (unsigned i = 1; i < snapshot.count; ++i) {
        assert(attention_ui_select_next());
        settle_scroll();
        assert_viewport();
        assert(area(s_current_card).y1 == fixed.y1);
        assert(area(s_cards[i]).y1 >= area(s_list).y1);
        assert(area(s_cards[i]).y2 <= area(s_list).y2);
        attention_ui_render(&snapshot);
    }
    assert(lv_obj_get_scroll_y(s_list) > 0);

    // Long titles at every saved title size and the largest subtitle size,
    // followed by shrinking the list after scrolling to its end.
    for (unsigned font = 0; font < sizeof(TITLE_FONT_OPTIONS) / sizeof(TITLE_FONT_OPTIONS[0]); ++font) {
        s_title_font_index = font;
        s_subtitle_font_index = 7;
        snapshot.count = CONFIG_CODEX_ATTENTION_MAX_ITEMS;
        for (unsigned i = 0; i < snapshot.count; ++i) {
            memset(snapshot.items[i].title, 'W', ATTENTION_TITLE_MAX - 1);
            snapshot.items[i].title[ATTENTION_TITLE_MAX - 1] = '\0';
        }
        attention_ui_render(&snapshot);
        assert_viewport();
        lv_obj_scroll_to_y(s_list, LV_COORD_MAX, LV_ANIM_OFF);
        assert(lv_obj_get_scroll_y(s_list) > 0);
        snapshot.count = 1;
        attention_ui_render(&snapshot);
        assert_viewport();
        assert(area(s_cards[0]).y1 <= area(s_list).y1);
        assert(area(s_cards[0]).y2 >= area(s_list).y1);
        assert(lv_obj_get_scroll_y(s_list) <= LV_MAX(0, lv_obj_get_height(s_cards[0]) - lv_obj_get_height(s_list)));
    }

    // The remaining-space budget also holds if the root viewport changes.
    lv_obj_set_height(s_list_view, 450);
    lv_obj_update_layout(s_list_view);
    assert(lv_obj_get_height(s_list) == 289);
    lv_obj_set_height(s_list_view, lv_pct(100));
    snapshot.count = 0;
    attention_ui_render(&snapshot);
    assert_viewport();

    assert(strcmp(lv_label_get_text(s_current_title), "Controller status unknown") == 0);
    snapshot.desktop_control_availability = ATTENTION_DESKTOP_CONTROL_UNAVAILABLE;
    attention_ui_render(&snapshot);
    assert(strcmp(lv_label_get_text(s_current_title), "Voice controller unavailable") == 0);
    assert(lv_obj_has_state(s_current_card, LV_STATE_DISABLED));
    snapshot.desktop_control_availability = ATTENTION_DESKTOP_CONTROL_AVAILABLE;
    snapshot.capabilities.desktop_focus = true;
    attention_ui_render(&snapshot);
    assert(strcmp(lv_label_get_text(s_current_title), "Mac selection is unknown") == 0);
    assert(strstr(lv_label_get_text(s_current_meta), "hold a button") != NULL);
    assert(lv_obj_has_state(s_current_card, LV_STATE_DISABLED));
    snapshot.capabilities.desktop_focus = false;
    attention_ui_render(&snapshot);
    assert(strstr(lv_label_get_text(s_current_meta), "needs setup") != NULL);
    strlcpy(snapshot.source_error, "Wi-Fi disconnected", sizeof(snapshot.source_error));
    attention_ui_render(&snapshot);
    assert(strcmp(lv_label_get_text(s_current_title), "Bridge data unavailable") == 0);
    snapshot.source_error[0] = '\0';
    snapshot.current_thread.available = true;
    strlcpy(snapshot.current_thread.id, "thread-0000000000000000", sizeof(snapshot.current_thread.id));
    strlcpy(snapshot.current_thread.title, "Known device target", sizeof(snapshot.current_thread.title));
    snapshot.current_thread.focus_confidence = ATTENTION_FOCUS_INFERRED;
    attention_ui_render(&snapshot);
    assert(strcmp(lv_label_get_text(s_current_title), "Known device target") == 0);
    assert(!lv_obj_has_state(s_current_card, LV_STATE_DISABLED));
    snapshot.current_thread.focus_confidence = ATTENTION_FOCUS_CONFIRMED;
    attention_ui_render(&snapshot);
    assert_viewport();
    strlcpy(snapshot.current_thread.project, "codex-esp32-display", sizeof(snapshot.current_thread.project));
    const attention_voice_state_t states[] = {ATTENTION_VOICE_UNKNOWN, ATTENTION_VOICE_READY,
        ATTENTION_VOICE_FOCUSING, ATTENTION_VOICE_STARTING, ATTENTION_VOICE_LISTENING,
        ATTENTION_VOICE_MUTED, ATTENTION_VOICE_ERROR};
    const lv_color_t colors[] = {COLOR_MUTED, COLOR_MUTED, COLOR_AMBER, COLOR_AMBER,
        COLOR_GREEN, COLOR_MUTED, COLOR_RED};
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        snapshot.current_thread.voice_state = states[i];
        attention_ui_render(&snapshot);
        lv_obj_update_layout(lv_screen_active());
        assert(lv_color_eq(lv_obj_get_style_text_color(s_current_voice, 0), colors[i]));
        assert(strcmp(lv_label_get_text(s_current_meta), "codex-esp32-display") == 0);
        assert(area(s_current_voice).x1 > area(s_current_title).x2);
        assert(area(s_current_voice).x2 < area(s_current_card).x2);
        assert(area(s_current_voice).y2 < area(s_current_meta).y1);
    }
    memset(snapshot.current_thread.title, 'W', sizeof(snapshot.current_thread.title) - 1);
    snapshot.current_thread.title[sizeof(snapshot.current_thread.title) - 1] = '\0';
    attention_ui_render(&snapshot);
    lv_obj_update_layout(lv_screen_active());
    assert(area(s_current_voice).x1 > area(s_current_title).x2);
    assert(area(s_current_voice).x2 < area(s_current_card).x2);
    attention_ui_show_detail_loading(snapshot.current_thread.id);
    lv_obj_update_layout(lv_screen_active());
    assert(area(s_detail_voice).x1 > area(s_detail_title).x2);
    assert(area(s_detail_voice).x2 < area(lv_obj_get_parent(s_detail_title)).x2);
    assert(lv_color_eq(lv_obj_get_style_text_color(s_detail_voice, 0), COLOR_RED));
    attention_ui_show_list();

    const char *capture = getenv("ATTENTION_UI_CAPTURE");
    if (capture != NULL) {
        s_title_font_index = 6;
        s_subtitle_font_index = 4;
        snapshot.count = 3;
        strlcpy(snapshot.items[0].title, "Review display changes", sizeof(snapshot.items[0].title));
        strlcpy(snapshot.items[1].title, "Device dictation", sizeof(snapshot.items[1].title));
        strlcpy(snapshot.items[2].title, "Current task synchronization", sizeof(snapshot.items[2].title));
        strlcpy(snapshot.current_thread.title, "Codex voice", sizeof(snapshot.current_thread.title));
        snapshot.current_thread.voice_state = ATTENTION_VOICE_LISTENING;
        attention_ui_render(&snapshot);
        settle_scroll();
        lv_refr_now(display);
        FILE *image = fopen(capture, "wb");
        assert(image != NULL);
        fprintf(image, "P6\n410 502\n255\n");
        for (unsigned i = 0; i < 410 * 502; ++i) {
            const uint16_t pixel = displayed[i];
            fputc(((pixel >> 11) & 31) * 255 / 31, image);
            fputc(((pixel >> 5) & 63) * 255 / 63, image);
            fputc((pixel & 31) * 255 / 31, image);
        }
        fclose(image);
    }
    printf("LVGL %d.%d.%d: list 341px; fixed card 80px; full-frame detail/settings return, scrolling, refresh, fonts, resize and status passed\n",
        LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    lv_deinit();
    return 0;
}
