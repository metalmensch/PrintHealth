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

    /* Printer status / toner message, pinned to the bottom, wraps as needed. */
    lbl_msg = lv_label_create(scr);
    lv_label_set_long_mode(lbl_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_msg, LV_PCT(100));
    lv_obj_set_style_pad_top(lbl_msg, 4, 0);
    lv_obj_set_style_margin_top(lbl_msg, LV_SIZE_CONTENT, 0);
    lv_obj_set_flex_grow(lbl_msg, 1);   /* push toward the bottom */
    lv_obj_set_style_text_color(lbl_msg, lv_color_hex(0x39d353), 0);
    lv_label_set_text(lbl_msg, "");
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

            /* State, colored by health: red if stopped, amber if a
             * warning reason is present, green otherwise. */
            const char *reasons = st.printer.state_reasons;
            bool warn = reasons[0] && strcmp(reasons, "none") != 0;
            lv_label_set_text(lbl_state,
                st.printer.ok ? ipp_state_str(st.printer.state) : "offline");
            lv_obj_set_style_text_color(lbl_state,
                !st.printer.ok || st.printer.state == 5 ? lv_color_hex(0xff5a5a)
                : warn ? lv_color_hex(0xffb020) : lv_color_hex(0x39d353), 0);

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

            /* Bottom line: printer status / toner message. */
            if (!st.printer.ok) {
                lv_label_set_text(lbl_msg, "printer offline");
                lv_obj_set_style_text_color(lbl_msg, lv_color_hex(0x8899aa), 0);
            } else if (reasons[0] == '\0' || !strcmp(reasons, "none")) {
                lv_label_set_text(lbl_msg, "Ready");
                lv_obj_set_style_text_color(lbl_msg, lv_color_hex(0x39d353), 0);
            } else {
                char pretty[80];
                prettify_reasons(reasons, pretty, sizeof(pretty));
                lv_label_set_text(lbl_msg, pretty);
                bool bad = strstr(reasons, "error") || strstr(reasons, "jam")
                        || strstr(reasons, "empty") || strstr(reasons, "stopped");
                lv_obj_set_style_text_color(lbl_msg,
                    bad ? lv_color_hex(0xff5a5a) : lv_color_hex(0xffb020), 0);
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
