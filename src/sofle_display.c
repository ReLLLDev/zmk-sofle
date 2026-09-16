/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <lvgl.h>
#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/position_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#else
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#endif

static lv_obj_t *screen_obj;
/* The event thread only increments this counter; LVGL owns all animation state. */
static atomic_t key_presses;
static atomic_val_t observed_presses;
static uint32_t last_tap;
static uint8_t paw_pose; /* 0: resting, 1: left paw, 2: right paw */
static bool next_paw;
static bool eyes_closed;

static int key_cat_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *event = as_zmk_position_state_changed(eh);
    if (event && event->state) {
        atomic_inc(&key_presses);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(key_cat, key_cat_listener);
ZMK_SUBSCRIPTION(key_cat, zmk_position_state_changed);

struct oled_state {
    uint8_t battery;
    uint8_t layer;
    char layer_name[5];
    uint8_t profile;
    bool usb;
    bool connected;
};

static struct oled_state current_state;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Keep old Studio-saved names useful, while respecting new user-defined names. */
static void layer_caption(char result[5], const char *name, uint8_t index) {
    static const char *const legacy[] = {"LAYER0", "layer1", "layer2", "layer3", "layer4", "layer5"};
    static const char *const captions[] = {"EN", "EN-M", "NUM", "RU", "RU-M", "NUM"};
    for (int i = 0; i < ARRAY_SIZE(legacy); i++) {
        if (name && strcmp(name, legacy[i]) == 0) {
            snprintf(result, 5, "%s", captions[i]);
            return;
        }
    }
    if (!name || !name[0]) {
        snprintf(result, 5, "L%u", (unsigned int)index);
        return;
    }
    for (int i = 0; i < 4; i++) {
        unsigned char ch = name[i];
        if (ch >= 128 || (ch && ch < 32)) {
            snprintf(result, 5, "L%u", (unsigned int)index);
            return;
        }
        result[i] = ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch;
        if (!ch) {
            return;
        }
    }
    result[4] = '\0';
}
#endif

static struct oled_state read_state(const zmk_event_t *eh) {
    struct oled_state state = {0};
    const struct zmk_battery_state_changed *battery = as_zmk_battery_state_changed(eh);
    state.battery = battery ? battery->state_of_charge : zmk_battery_state_of_charge();
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    struct zmk_endpoint_instance endpoint = zmk_endpoints_selected();
    state.usb = endpoint.transport == ZMK_TRANSPORT_USB;
    state.profile = zmk_ble_active_profile_index() + 1;
    state.connected = zmk_ble_active_profile_is_connected();
    state.layer = zmk_keymap_highest_layer_active();
    layer_caption(state.layer_name,
                  zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(state.layer)), state.layer);
#else
    state.connected = zmk_split_bt_peripheral_is_connected();
#endif
    return state;
}

static void update_status(struct oled_state state) {
    current_state = state;
    if (screen_obj) {
        lv_obj_invalidate(screen_obj);
    }
}

/* Snapshot ZMK state in the event context; all LVGL calls run on its work queue. */
ZMK_DISPLAY_WIDGET_LISTENER(sofle_oled, struct oled_state, update_status, read_state)
ZMK_SUBSCRIPTION(sofle_oled, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(sofle_oled, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(sofle_oled, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(sofle_oled, zmk_layer_state_changed);
#else
ZMK_SUBSCRIPTION(sofle_oled, zmk_split_peripheral_status_changed);
#endif

/* Original cat face, enlarged below to fill the portrait display width. */
static const char *const cat_head[] = {
    "...#...........#...",
    "...##.........##...",
    "...#.#.......#.#...",
    "...#..#######..#...",
    "..#.............#..",
    ".#...............#.",
    "#.................#",
    "#....#.......#....#",
    "#.................#",
    "#........#........#",
    ".#......#.#......#.",
    "..#.............#..",
    "...#############...",
};

/* Portrait coordinates (32x128) mapped to the native SSD1306 (128x32).
 * Native left becomes portrait bottom: native_x = 127-y, native_y = x.
 * Drawing pixels directly also works with Zephyr's packed monochrome buffer;
 * LVGL's generic software rotation expects a different buffer layout.
 */
static void pixel(lv_draw_ctx_t *ctx, lv_draw_rect_dsc_t *ink,
                  const lv_area_t *origin, int x, int y) {
    if (x < 0 || x >= 32 || y < 0 || y >= 128) {
        return;
    }
    lv_area_t area = {.x1 = origin->x1 + 127 - y, .y1 = origin->y1 + x,
                      .x2 = origin->x1 + 127 - y, .y2 = origin->y1 + x};
    lv_draw_rect(ctx, ink, &area);
}

/* Both UNSCII fonts are uncompressed. Use LVGL glyph metrics for placement. */
static void text(lv_draw_ctx_t *ctx, lv_draw_rect_dsc_t *ink, const lv_area_t *area,
                 const lv_font_t *font, const char *value, int y) {
    int width = 0;
    for (const char *p = value; *p; p++) {
        width += lv_font_get_glyph_width(font, *p, p[1]);
    }
    int x = (32 - width) / 2;
    for (const char *p = value; *p; p++) {
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, *p, p[1])) {
            continue;
        }
        const uint8_t *bitmap = lv_font_get_glyph_bitmap(font, *p);
        if (bitmap) {
            for (int row = 0; row < glyph.box_h; row++) {
                for (int col = 0; col < glyph.box_w; col++) {
                    int bit = (row * glyph.box_w + col) * glyph.bpp;
                    int shift = 8 - glyph.bpp - bit % 8;
                    int value = (bitmap[bit / 8] >> shift) & ((1 << glyph.bpp) - 1);
                    if (value >= (1 << (glyph.bpp - 1))) {
                        pixel(ctx, ink, area, x + glyph.ofs_x + col,
                              y + font->line_height - font->base_line - glyph.box_h -
                                  glyph.ofs_y + row);
                    }
                }
            }
        }
        x += glyph.adv_w;
    }
}

static void cat_pixel(lv_draw_ctx_t *ctx, lv_draw_rect_dsc_t *ink,
                      const lv_area_t *area, int x, int y) {
    pixel(ctx, ink, area, x, 57 + y);
}

static void draw_cat(lv_draw_ctx_t *ctx, lv_draw_rect_dsc_t *ink,
                     const lv_area_t *area) {
    for (int y = 0; y < ARRAY_SIZE(cat_head); y++) {
        for (int x = 0; cat_head[y][x]; x++) {
            if (cat_head[y][x] == '#' && !(eyes_closed && y == 7)) {
                for (int yy = y * 18 / 13; yy < (y + 1) * 18 / 13; yy++) {
                    for (int xx = x * 27 / 19; xx < (x + 1) * 27 / 19; xx++) {
                        cat_pixel(ctx, ink, area, 2 + xx, yy);
                    }
                }
            }
        }
    }
    if (eyes_closed) {
        /* Restore face sides, replacing only the two eyes with closed lids. */
        cat_pixel(ctx, ink, area, 2, 9);
        cat_pixel(ctx, ink, area, 28, 9);
        cat_pixel(ctx, ink, area, 2, 10);
        cat_pixel(ctx, ink, area, 28, 10);
        for (int x = 0; x < 4; x++) {
            cat_pixel(ctx, ink, area, 7 + x, 11);
            cat_pixel(ctx, ink, area, 20 + x, 11);
        }
    }
    for (int i = 0; i < 4; i++) {
        cat_pixel(ctx, ink, area, 7 - i, 16 + i);
        cat_pixel(ctx, ink, area, 23 + i, 16 + i);
    }
    for (int x = 1; x < 31; x++) {
        cat_pixel(ctx, ink, area, x, 25);
        cat_pixel(ctx, ink, area, x, 31);
    }
    for (int y = 26; y < 31; y++) {
        cat_pixel(ctx, ink, area, 1, y);
        cat_pixel(ctx, ink, area, 30, y);
    }
    for (int x = 4; x < 28; x += 4) {
        cat_pixel(ctx, ink, area, x, 28);
        cat_pixel(ctx, ink, area, x + 1, 28);
    }
    for (int paw = 0; paw < 2; paw++) {
        int x = paw ? 21 : 5;
        int y = paw_pose == paw + 1 ? 24 : 20;
        ink->bg_color = lv_color_black();
        for (int py = 0; py < 4; py++) {
            for (int px = 0; px < 6; px++) {
                cat_pixel(ctx, ink, area, x + px, y + py);
            }
        }
        ink->bg_color = lv_color_white();
        for (int px = 1; px < 5; px++) {
            cat_pixel(ctx, ink, area, x + px, y);
            cat_pixel(ctx, ink, area, x + px, y + 3);
        }
        for (int py = 1; py < 3; py++) {
            cat_pixel(ctx, ink, area, x, y + py);
            cat_pixel(ctx, ink, area, x + 5, y + py);
        }
    }
}

static void draw_screen(lv_event_t *event) {
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target(event), &area);
    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    ink.bg_color = lv_color_white();
    ink.bg_opa = LV_OPA_COVER;
    char label[8];
    if (!current_state.usb) {
        static const uint8_t bluetooth[] = {0x08, 0x0c, 0x2a, 0x19, 0x0e,
                                            0x19, 0x2a, 0x0c, 0x08};
        for (int y = 0; y < ARRAY_SIZE(bluetooth); y++) {
            for (int x = 0; x < 6; x++) {
                if (bluetooth[y] & (0x20 >> x)) {
                    pixel(ctx, &ink, &area, 13 + x, 1 + y);
                }
            }
        }
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (current_state.usb) {
        text(ctx, &ink, &area, &lv_font_unscii_8, "USB", 2);
        text(ctx, &ink, &area, &lv_font_unscii_8, "OK", 13);
    } else {
        snprintf(label, sizeof(label), "%u  ", (unsigned int)current_state.profile);
        text(ctx, &ink, &area, &lv_font_unscii_8, label, 13);
    }
#else
    text(ctx, &ink, &area, &lv_font_unscii_8, "LINK", 13);
#endif
    if (!current_state.usb) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        const int mark_y = 14;
#else
        const int mark_y = 3;
#endif
        if (current_state.connected) {
            for (int i = 0; i < 3; i++) {
                pixel(ctx, &ink, &area, 21 + i, mark_y + 2 + i);
            }
            for (int i = 0; i < 5; i++) {
                pixel(ctx, &ink, &area, 23 + i, mark_y + 4 - i);
            }
        } else {
            for (int i = 0; i < 5; i++) {
                pixel(ctx, &ink, &area, 22 + i, mark_y + i);
                pixel(ctx, &ink, &area, 26 - i, mark_y + i);
            }
        }
    }
    /* Compact 3x5 digits scaled to 9x15: even 100 fits in 32 pixels. */
    static const uint16_t digits[] = {0x7b6f, 0x2c97, 0x73e7, 0x73cf, 0x5bc9,
                                      0x79cf, 0x79ef, 0x7249, 0x7bef, 0x7bcf};
    snprintf(label, sizeof(label), "%u", (unsigned int)current_state.battery);
    int length = strlen(label);
    int start = (32 - (length * 10 - 1)) / 2;
    for (int i = 0; i < length; i++) {
        uint16_t bits = digits[label[i] - '0'];
        for (int bit = 0; bit < 15; bit++) {
            if (bits & (1 << (14 - bit))) {
                for (int dy = 0; dy < 3; dy++) {
                    for (int dx = 0; dx < 3; dx++) {
                        pixel(ctx, &ink, &area, start + i * 10 + bit % 3 * 3 + dx,
                              27 + bit / 3 * 3 + dy);
                    }
                }
            }
        }
    }
    text(ctx, &ink, &area, &lv_font_unscii_8, "%", 44);
    if (current_state.battery <= 15) {
        /* Static low-battery indicator; never wake the display just to flash it. */
        for (int x = 1; x < 9; x++) {
            pixel(ctx, &ink, &area, x, 45);
            pixel(ctx, &ink, &area, x, 51);
        }
        for (int y = 46; y < 51; y++) {
            pixel(ctx, &ink, &area, 1, y);
            pixel(ctx, &ink, &area, 8, y);
        }
        pixel(ctx, &ink, &area, 9, 47);
        pixel(ctx, &ink, &area, 9, 48);
        pixel(ctx, &ink, &area, 4, 47);
        pixel(ctx, &ink, &area, 4, 48);
        pixel(ctx, &ink, &area, 4, 50);
    }
    draw_cat(ctx, &ink, &area);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    text(ctx, &ink, &area, &lv_font_unscii_8, current_state.layer_name, 99);
    snprintf(label, sizeof(label), "%u", (unsigned int)current_state.layer);
    text(ctx, &ink, &area, &lv_font_unscii_16, label, 110);
#else
    text(ctx, &ink, &area, &lv_font_unscii_8, "KEYS", 99);
    text(ctx, &ink, &area, &lv_font_unscii_8,
         paw_pose ? "TYPE" : eyes_closed ? "ZZZ" : "REST", 114);
#endif
}

static void animate(lv_timer_t *timer) {
    (void)timer;
    atomic_val_t count = atomic_get(&key_presses);
    uint8_t previous_pose = paw_pose;
    bool previous_eyes = eyes_closed;
    if (count != observed_presses) {
        observed_presses = count;
        next_paw = !next_paw;
        paw_pose = next_paw ? 1 : 2;
        last_tap = lv_tick_get();
    } else if (paw_pose && lv_tick_elaps(last_tap) >= 180) {
        paw_pose = 0;
    }
    /* Brief blink every four seconds; sleepy after 15 seconds without presses.
     * ZMK still owns the 30-second blanking timer; this never generates activity.
     */
    uint32_t quiet_ms = lv_tick_elaps(last_tap);
    eyes_closed = !paw_pose && (quiet_ms >= 15000 || quiet_ms % 4000 >= 3800);
    if (paw_pose != previous_pose || eyes_closed != previous_eyes) {
        lv_obj_invalidate(screen_obj);
    }
}

lv_obj_t *zmk_display_status_screen(void) {
    screen_obj = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen_obj);
    lv_obj_set_style_bg_color(screen_obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen_obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen_obj, draw_screen, LV_EVENT_DRAW_MAIN, NULL);
    last_tap = lv_tick_get();
    sofle_oled_init();
    /* ZMK stops lv_task_handler when blanking: no independent animation worker. */
    lv_timer_create(animate, 50, NULL);
    return screen_obj;
}
