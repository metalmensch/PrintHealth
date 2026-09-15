# Credits and third-party licenses

PrintServer is licensed under the **GNU General Public License v3.0 or later**
(`GPL-3.0-or-later`); see [LICENSE](LICENSE).

GPL **v3** specifically is required because the firmware combines with
Apache-2.0 code (ESP-IDF and esp_lvgl_port), which is compatible with GPLv3 but
not GPLv2.

This project builds on the following works. They are pulled in at build time by
ESP-IDF's dependency manager rather than copied into this repository, so their
source is not redistributed here; they are credited below with their licenses.

| Component | Author | License | Use |
|-----------|--------|---------|-----|
| ESP-IDF | Espressif Systems | Apache-2.0 | RTOS, Wi-Fi, lwIP/NAPT, esp_lcd, HTTP client, SNTP |
| `wifi/softap_sta` example | Espressif Systems | CC0-1.0 / Unlicense (public domain) | Basis of the AP+STA NAT bridge in `printserver_main.c` |
| LVGL | LVGL LLC and contributors | MIT | Status-screen graphics |
| `esp_lvgl_port` | Espressif Systems | Apache-2.0 | Binding LVGL to the esp_lcd panel |
| T-Display-C5 board support | LilyGo (Xinyuan-LilyGO) | MIT | ST7789 pin map and panel init parameters |

Links:
- ESP-IDF — https://github.com/espressif/esp-idf
- LVGL — https://lvgl.io
- esp_lvgl_port — https://github.com/espressif/esp-bsp/tree/master/components/esp_lvgl_port
- LilyGo T-Display-C5 — https://github.com/Xinyuan-LilyGO/T-Display-C5

The Internet Printing Protocol handling follows the PWG/IETF IPP specifications
(RFC 8010/8011).
