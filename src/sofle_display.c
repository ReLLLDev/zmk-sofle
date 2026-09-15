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
    uint8_t profile;
    bool usb;
    bool connected;
};

static struct oled_state current_state;

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

/* Original pixel art, sized for the 40x22 area beside the status labels. */
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
            if (cat_head[y][x] == '#') {
                cat_pixel(ctx, ink, area, 6 + x, y);
            }
        }
    }
    for (int i = 0; i < 4; i++) {
        cat_pixel(ctx, ink, area, 7 - i, 11 + i);
        cat_pixel(ctx, ink, area, 23 + i, 11 + i);
    }
    for (int x = 1; x < 31; x++) {
        cat_pixel(ctx, ink, area, x, 17);
        cat_pixel(ctx, ink, area, x, 21);
    }
    for (int y = 18; y < 21; y++) {
        cat_pixel(ctx, ink, area, 1, y);
        cat_pixel(ctx, ink, area, 30, y);
    }
    for (int x = 4; x < 28; x += 4) {
        cat_pixel(ctx, ink, area, x, 19);
        cat_pixel(ctx, ink, area, x + 1, 19);
    }
    for (int paw = 0; paw < 2; paw++) {
        int x = paw ? 21 : 5;
        int y = paw_pose == paw + 1 ? 16 : 13;
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
        text(ctx, &ink, &area, &lv_font_unscii_8, "L", 13);
    } else {
        snprintf(label, sizeof(label), "L%u%s", (unsigned int)current_state.profile,
                 current_state.connected ? "+" : "?");
        text(ctx, &ink, &area, &lv_font_unscii_8, label, 13);
    }
#else
    text(ctx, &ink, &area, &lv_font_unscii_8, current_state.connected ? "R +" : "R ?", 13);
#endif
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
    draw_cat(ctx, &ink, &area);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    text(ctx, &ink, &area, &lv_font_unscii_8, "LYR", 90);
    snprintf(label, sizeof(label), "%u", (unsigned int)current_state.layer);
#else
    text(ctx, &ink, &area, &lv_font_unscii_8, "LINK", 90);
    snprintf(label, sizeof(label), "%s", current_state.connected ? "OK" : "--");
#endif
    text(ctx, &ink, &area, &lv_font_unscii_16, label, 103);
}

static void animate(lv_timer_t *timer) {
    (void)timer;
    atomic_val_t count = atomic_get(&key_presses);
    uint8_t previous_pose = paw_pose;
    if (count != observed_presses) {
        observed_presses = count;
        next_paw = !next_paw;
        paw_pose = next_paw ? 1 : 2;
        last_tap = lv_tick_get();
    } else if (paw_pose && lv_tick_elaps(last_tap) >= 180) {
        paw_pose = 0;
    }
    if (paw_pose != previous_pose) {
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
    sofle_oled_init();
    /* ZMK stops lv_task_handler when blanking: no independent animation worker. */
    lv_timer_create(animate, 50, NULL);
    return screen_obj;
}
