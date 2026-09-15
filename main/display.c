/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Jason Dempsey
 *
 * Status display for the LilyGo T-Display C5 (1.9" ST7789, 170x320).
 *
 * Panel wiring and init parameters (SPI2, MOSI 9 / SCK 7 / CS 26 / DC 8 /
 * RST 23 / backlight 25, gap 35, mirror, color inversion, BGR) are taken
 * from LilyGo's official T-Display-C5 example. LVGL is driven through
 * esp_lvgl_port; a task refreshes the labels from g_status once a second.
 */
#include "display.h"
#include "status.h"
#include "ipp_client.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "display";

#define LCD_HOST      SPI2_HOST
#define PIN_MOSI      9
#define PIN_SCLK      7
#define PIN_CS        26
#define PIN_DC        8
#define PIN_RST       23
#define PIN_BL        25
#define LCD_H_RES     170
#define LCD_V_RES     320
#define LCD_GAP_X     35
#define LCD_PCLK_HZ   (20 * 1000 * 1000)

static lv_obj_t *lbl_title, *lbl_state, *lbl_jobs, *lbl_signal,
                *lbl_clients, *lbl_uptime, *lbl_time, *lbl_ip, *lbl_msg;

static lv_obj_t *toner_box;                     /* holds the toner bars */
static lv_obj_t *bar_obj[IPP_MAX_MARKERS];
static lv_obj_t *bar_pct[IPP_MAX_MARKERS];
static int       bars_built;                    /* number of bars created */

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;

static void panel_init(void)
{
    gpio_config_t bl = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_BL,
    };
    gpio_config(&bl);
    gpio_set_level(PIN_BL, 1);   /* backlight on */

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io, &s_io));

    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &pc, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
}

/* One left-aligned status row; returns the value label to update later. */
static lv_obj_t *make_row(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cap = lv_label_create(row);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, lv_color_hex(0x8899aa), 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "-");
    lv_obj_set_style_text_color(val, lv_color_hex(0xffffff), 0);
    return val;
}

static void build_ui(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b1622), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(scr, 8, 0);
    lv_obj_set_style_pad_row(scr, 6, 0);

    lbl_title = lv_label_create(scr);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_title, LV_PCT(100));
    lv_label_set_text(lbl_title, "Print Bridge");
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x4ea1ff), 0);

    lv_obj_t *sep = lv_obj_create(scr);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, LV_PCT(100), 2);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x24384d), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    lbl_state   = make_row(scr, "State");
    lbl_jobs    = make_row(scr, "Jobs");
    lbl_signal  = make_row(scr, "Signal");
    lbl_clients = make_row(scr, "Clients");
    lbl_uptime  = make_row(scr, "Uptime");
    lbl_time    = make_row(scr, "Time");
    lbl_ip      = make_row(scr, "Bridge");

    lv_obj_t *sep2 = lv_obj_create(scr);
    lv_obj_remove_style_all(sep2);
    lv_obj_set_size(sep2, LV_PCT(100), 2);
    lv_obj_set_style_bg_color(sep2, lv_color_hex(0x24384d), 0);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_COVER, 0);

    lv_obj_t *cap = lv_label_create(scr);
    lv_label_set_text(cap, "Toner");
    lv_obj_set_style_text_color(cap, lv_color_hex(0x8899aa), 0);

    /* Container for the per-color toner bars (populated once data arrives). */
    toner_box = lv_obj_create(scr);
    lv_obj_remove_style_all(toner_box);
    lv_obj_set_width(toner_box, LV_PCT(100));
    lv_obj_set_height(toner_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(toner_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(toner_box, 4, 0);

    /* Non-toner alerts (jams, cover open, out of paper) show here. */
    lbl_msg = lv_label_create(scr);
    lv_label_set_long_mode(lbl_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_msg, LV_PCT(100));
    lv_obj_set_style_pad_top(lbl_msg, 2, 0);
    lv_label_set_text(lbl_msg, "");
}

/* Make dark toner colors (black) visible against the dark background. */
static uint32_t visible_color(uint32_t rgb)
{
    int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    int lum = (r * 30 + g * 59 + b * 11) / 100;
    return lum < 40 ? 0xC8CED6 : rgb;   /* near-black -> light gray */
}

/* Build one toner bar row: colored letter, bar, percent label. */
static void build_bar(int k, char letter, uint32_t rgb)
{
    uint32_t col = visible_color(rgb);

    lv_obj_t *row = lv_obj_create(toner_box);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 5, 0);

    char ls[2] = { letter, '\0' };
    lv_obj_t *lab = lv_label_create(row);
    lv_label_set_text(lab, ls);
    lv_obj_set_width(lab, 12);
    lv_obj_set_style_text_color(lab, lv_color_hex(col), 0);

    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_flex_grow(bar, 1);
    lv_obj_set_height(bar, 10);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x24384d), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(col), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);

    lv_obj_t *pct = lv_label_create(row);
    lv_obj_set_width(pct, 36);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(pct, "-");

    bar_obj[k] = bar;
    bar_pct[k] = pct;
}

/* Turn an IPP reason list like "cyan-toner-low,toner-low-warning" into
 * "cyan toner low, toner low warning" for display. */
static void prettify_reasons(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j < n - 1; i++) {
        char c = in[i];
        if (c == '-') c = ' ';
        else if (c == ',') { if (j < n - 2) out[j++] = ','; c = ' '; }
        out[j++] = c;
    }
    out[j] = '\0';
}

static void fmt_uptime(unsigned s, char *out, size_t n)
{
    unsigned d = s / 86400; s %= 86400;
    unsigned h = s / 3600;  s %= 3600;
    unsigned m = s / 60;    s %= 60;
    if (d)      snprintf(out, n, "%ud %uh %um", d, h, m);
    else if (h) snprintf(out, n, "%uh %um %us", h, m, s);
    else        snprintf(out, n, "%um %us", m, s);
}

static void update_task(void *arg)
{
    char buf[64];
    while (true) {
        status_lock();
        sys_status_t st = g_status;   /* copy under lock */
        status_unlock();

        if (lvgl_port_lock(0)) {
            if (st.printer.ok && st.printer.make_and_model[0]) {
                lv_label_set_text(lbl_title, st.printer.make_and_model);
            }

            /* State, colored by state: offline/stopped red, idle blue,
             * printing green. (Toner health is shown by the bars below.) */
            const char *reasons = st.printer.state_reasons;
            lv_label_set_text(lbl_state,
                st.printer.ok ? ipp_state_str(st.printer.state) : "offline");
            uint32_t scol;
            if (!st.printer.ok || st.printer.state == 5) scol = 0xff5a5a;  /* offline/stopped */
            else if (st.printer.state == 4)              scol = 0x39d353;  /* printing */
            else if (st.printer.state == 3)              scol = 0x4ea1ff;  /* idle */
            else                                         scol = 0xffffff;
            lv_obj_set_style_text_color(lbl_state, lv_color_hex(scol), 0);

            snprintf(buf, sizeof(buf), "%d", st.printer.queued_jobs);
            lv_label_set_text(lbl_jobs, st.printer.ok ? buf : "-");

            if (st.wifi_up) {
                int q = 2 * (st.rssi + 100);
                if (q > 100) q = 100;
                if (q < 0) q = 0;
                snprintf(buf, sizeof(buf), "%ddBm (%d%%)", st.rssi, q);
            } else {
                snprintf(buf, sizeof(buf), "down");
            }
            lv_label_set_text(lbl_signal, buf);
            lv_obj_set_style_text_color(lbl_signal,
                st.wifi_up ? lv_color_hex(0xffffff) : lv_color_hex(0xff5a5a), 0);

            snprintf(buf, sizeof(buf), "%d", st.ap_clients);
            lv_label_set_text(lbl_clients, buf);

            fmt_uptime(st.uptime_s, buf, sizeof(buf));
            lv_label_set_text(lbl_uptime, buf);

            if (st.now) {
                struct tm tm; localtime_r(&st.now, &tm);
                strftime(buf, sizeof(buf), "%a %H:%M:%S", &tm);
            } else {
                snprintf(buf, sizeof(buf), "syncing...");
            }
            lv_label_set_text(lbl_time, buf);

            lv_label_set_text(lbl_ip, st.bridge_ip[0] ? st.bridge_ip : "-");

            /* Toner graph: build the bars once we know the markers, then keep
             * their values updated. */
            if (st.printer.ok && st.printer.marker_count > 0 && bars_built == 0) {
                int n = st.printer.marker_count;
                if (n > IPP_MAX_MARKERS) n = IPP_MAX_MARKERS;
                for (int k = 0; k < n; k++)
                    build_bar(k, st.printer.marker_letter[k], st.printer.marker_rgb[k]);
                bars_built = n;
            }
            for (int k = 0; k < bars_built; k++) {
                int lvl = st.printer.marker_level[k];
                if (lvl > 100) lvl = 100;
                lv_bar_set_value(bar_obj[k], lvl < 0 ? 0 : lvl, LV_ANIM_OFF);
                char pb[16];
                if (lvl < 0) snprintf(pb, sizeof(pb), "?");
                else snprintf(pb, sizeof(pb), "%d%%", lvl);
                lv_label_set_text(bar_pct[k], pb);
                /* Empty (0%) reads as a solid red bar; low (<=10%) reddens the
                 * number; otherwise the normal dark track / white number. */
                lv_obj_set_style_bg_color(bar_obj[k],
                    lvl == 0 ? lv_color_hex(0xff5a5a) : lv_color_hex(0x24384d),
                    LV_PART_MAIN);
                lv_obj_set_style_text_color(bar_pct[k],
                    (lvl >= 0 && lvl <= 10) ? lv_color_hex(0xff5a5a)
                                            : lv_color_hex(0xffffff), 0);
            }

            /* Message line: non-toner alerts only (toner is shown by the bars). */
            char other[80]; other[0] = '\0';
            if (st.printer.ok && reasons[0] && strcmp(reasons, "none")) {
                const char *p = reasons;
                while (*p) {
                    const char *e = strchr(p, ',');
                    int tl = e ? (int)(e - p) : (int)strlen(p);
                    char tok[48];
                    int c = tl < (int)sizeof(tok) - 1 ? tl : (int)sizeof(tok) - 1;
                    memcpy(tok, p, c); tok[c] = '\0';
                    if (!strstr(tok, "toner")) {
                        char pretty[48];
                        prettify_reasons(tok, pretty, sizeof(pretty));
                        if (other[0]) strncat(other, ", ", sizeof(other) - strlen(other) - 1);
                        strncat(other, pretty, sizeof(other) - strlen(other) - 1);
                    }
                    if (!e) break;
                    p = e + 1;
                }
            }
            if (!st.printer.ok) {
                lv_label_set_text(lbl_msg, "printer offline");
                lv_obj_set_style_text_color(lbl_msg, lv_color_hex(0x8899aa), 0);
            } else if (other[0]) {
                lv_label_set_text(lbl_msg, other);
                bool bad = strstr(other, "jam") || strstr(other, "empty")
                        || strstr(other, "error") || strstr(other, "open");
                lv_obj_set_style_text_color(lbl_msg,
                    bad ? lv_color_hex(0xff5a5a) : lv_color_hex(0xffb020), 0);
            } else {
                lv_label_set_text(lbl_msg, "");
            }

            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void display_init(void)
{
    panel_init();

    lvgl_port_cfg_t pcfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&pcfg));

    lvgl_port_display_cfg_t dcfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = LCD_H_RES * 40,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&dcfg);

    if (lvgl_port_lock(0)) {
        build_ui(disp);
        lvgl_port_unlock();
    }

    xTaskCreate(update_task, "disp_update", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "display initialized");
}
