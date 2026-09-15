/*
 * PrintServer -- ESP32-C5 dedicated Wi-Fi NAT repeater for the
 * HP Color LaserJet Pro M255dw.
 *
 * The printer's own Wi-Fi to the home router is unreliable. Instead of
 * fighting USB (the C5 has no USB host), this puts a strong, short-range
 * access point right next to the printer and NATs its traffic onto the
 * home LAN through the station link. AirPrint / IPP / raw port 9100 all
 * keep working natively -- we only move packets, we don't reimplement
 * any print protocol.
 *
 *   home devices --> home Wi-Fi --> [C5 STA] --NAPT--> [C5 SoftAP] <-- printer
 *
 * Adapted from the public-domain ESP-IDF wifi/softap_sta example.
 * Credentials live in secrets.h (git-ignored); see secrets.example.h.
 */
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lwip/lwip_napt.h"
#include "lwip/prot/ip.h"
#include "lwip/def.h"

#include "secrets.h"
#include "status.h"
#include "ipp_client.h"

/* Set to 1 to build a station-only scan diagnostic (see app_main). */
#define DIAG_STA_SCAN_ONLY 0

/* POSIX TZ string for the wall clock. Default US Pacific; change to suit
 * (e.g. "EST5EDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0"). */
#ifndef TIMEZONE
#define TIMEZONE "PST8PDT,M3.2.0,M11.1.0"
#endif

/* How often to refresh the printer status over IPP. */
#define STATUS_PERIOD_MS 15000

sys_status_t g_status;
static SemaphoreHandle_t s_status_mtx;

void status_lock(void)   { xSemaphoreTake(s_status_mtx, portMAX_DELAY); }
void status_unlock(void) { xSemaphoreGive(s_status_mtx); }

/* SoftAP tunables. The channel is only a starting point: in AP+STA
 * coexistence the single radio forces the SoftAP onto the STA's channel
 * once the home connection is up, so the printer ends up on whatever
 * band/channel the home Wi-Fi uses (5 GHz if you gave a 5 GHz SSID). */
#define AP_START_CHANNEL   1
#define AP_MAX_CONN        4
#define STA_MAX_RETRY      10

/* The printer is the only DHCP client on the SoftAP, so the ESP-IDF DHCP
 * server (pool base 192.168.4.2) assigns it this address. Leases are
 * sticky per-MAC, so it stays put across reconnects. */
#define PRINTER_LAN_IP     "192.168.4.2"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define DHCPS_OFFER_DNS    0x02

static const char *TAG = "printbridge";
static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_netif_ap;
static esp_netif_t *s_netif_sta;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = event_data;
        ESP_LOGI(TAG, "printer/client joined AP: "MACSTR" (AID=%d)",
                 MAC2STR(e->mac), e->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = event_data;
        ESP_LOGW(TAG, "printer/client left AP: "MACSTR" (reason=%d)",
                 MAC2STR(e->mac), e->reason);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* Do not auto-connect here; app_main scans first, then connects. */
        ESP_LOGI(TAG, "station started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = event_data;
        if (s_retry_num < STA_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "home Wi-Fi disconnected (reason=%d), retry %d/%d",
                     e->reason, s_retry_num, STA_MAX_RETRY);
        } else {
            ESP_LOGE(TAG, "giving up on home Wi-Fi (last reason=%d)", e->reason);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = event_data;
        ESP_LOGI(TAG, "got home LAN IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_softap(void)
{
    s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {
        .ap = {
            .ssid = PRINTER_AP_SSID,
            .ssid_len = strlen(PRINTER_AP_SSID),
            .channel = AP_START_CHANNEL,
            .password = PRINTER_AP_PASS,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
        },
    };
    if (strlen(PRINTER_AP_PASS) == 0) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_LOGI(TAG, "SoftAP '%s' ready for the printer to join", PRINTER_AP_SSID);
}

/* One-shot diagnostic: list every AP the C5 can currently see, so we can
 * tell whether the home SSID is visible and on which band/channel. */
static void scan_and_log_aps(void)
{
    ESP_LOGI(TAG, "scanning for visible networks...");
    esp_err_t sr = esp_wifi_scan_start(NULL, true);
    if (sr != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed: %s", esp_err_to_name(sr));
        return;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        ESP_LOGW(TAG, "scan found 0 networks");
        return;
    }
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) return;
    esp_wifi_scan_get_ap_records(&n, recs);
    for (int i = 0; i < n; i++) {
        int freq = recs[i].primary >= 36 ? 5 : 2;  /* rough band from channel */
        ESP_LOGI(TAG, "  AP: rssi=%4d ch=%3d (%dGHz) auth=%d ssid='%s'",
                 recs[i].rssi, recs[i].primary, freq, recs[i].authmode, recs[i].ssid);
    }
    free(recs);
}

static void wifi_init_sta(void)
{
    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_config_t sta = {
        .sta = {
            .ssid = HOME_WIFI_SSID,
            .password = HOME_WIFI_PASS,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .failure_retry_cnt = STA_MAX_RETRY,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
}

/* Hand the SoftAP's DHCP clients (the printer) the same DNS server the
 * station learned, so name resolution works through the NAT. */
static void softap_set_dns_addr(void)
{
    esp_netif_dns_info_t dns;
    esp_netif_get_dns_info(s_netif_sta, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offer = DHCPS_OFFER_DNS;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_netif_ap));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_netif_ap, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer, sizeof(offer)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_netif_ap, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(s_netif_ap));
}

/* Forward the printer's service ports from the ESP32's home-LAN IP to the
 * printer sitting behind NAT, so home devices can print by connecting to
 * the ESP32's IP. Discovery (mDNS/AirPrint) does not cross NAT, so add the
 * printer on your computers by this ESP32's IP address. */
static void setup_port_forwards(void)
{
#if IP_NAPT
    esp_netif_ip_info_t sta_ip;
    if (esp_netif_get_ip_info(s_netif_sta, &sta_ip) != ESP_OK) {
        ESP_LOGE(TAG, "no STA IP; cannot set up port forwarding");
        return;
    }
    esp_ip4_addr_t printer;
    printer.addr = esp_ip4addr_aton(PRINTER_LAN_IP);

    /* ip_portmap_add() takes host-order ports (it byte-swaps internally)
     * and returns 1 on success, 0 on failure. */
    const uint16_t ports[] = { 9100, 631, 515, 80 };
    const char *names[]    = { "raw/JetDirect", "IPP", "LPD", "web UI" };
    for (int i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
        u8_t ok = ip_portmap_add(IP_PROTO_TCP,
                                 sta_ip.ip.addr, ports[i],
                                 printer.addr,   ports[i]);
        if (ok) {
            ESP_LOGI(TAG, "forward tcp/%u (%s) -> " IPSTR ":%u",
                     ports[i], names[i], IP2STR(&printer), ports[i]);
        } else {
            ESP_LOGE(TAG, "failed to forward tcp/%u (%s)", ports[i], names[i]);
        }
    }
    ESP_LOGI(TAG, "print to this bridge at " IPSTR, IP2STR(&sta_ip.ip));
#else
    ESP_LOGE(TAG, "NAPT/portmap not compiled in");
#endif
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();
    s_status_mtx = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    &wifi_event_handler, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

#if DIAG_STA_SCAN_ONLY
    /* Throwaway diagnostic: pure station mode, scan repeatedly, do nothing
     * else. Isolates whether AP+STA coexistence is what breaks scanning. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_start());
    while (true) {
        scan_and_log_aps();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
#endif

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_init_softap();
    wifi_init_sta();

    ESP_ERROR_CHECK(esp_wifi_start());

    scan_and_log_aps();

    /* Now begin connecting to the home Wi-Fi. */
    esp_wifi_connect();
    ESP_LOGI(TAG, "connecting to home Wi-Fi '%s'", HOME_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to home Wi-Fi '%s'", HOME_WIFI_SSID);
        softap_set_dns_addr();
        esp_netif_set_default_netif(s_netif_sta);
        if (esp_netif_napt_enable(s_netif_ap) != ESP_OK) {
            ESP_LOGE(TAG, "failed to enable NAPT -- routing will not work");
        } else {
            ESP_LOGI(TAG, "NAPT on: printer traffic now bridges to the home LAN");
        }
        setup_port_forwards();
    } else {
        ESP_LOGE(TAG, "could not join home Wi-Fi '%s' -- check secrets.h", HOME_WIFI_SSID);
        return;
    }

    /* Record the bridge IP for the display. */
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif_sta, &ip) == ESP_OK) {
        status_lock();
        snprintf(g_status.bridge_ip, sizeof(g_status.bridge_ip), IPSTR, IP2STR(&ip.ip));
        status_unlock();
    }

    /* Start NTP time sync (best-effort; needs internet through the STA). */
    setenv("TZ", TIMEZONE, 1);
    tzset();
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);

    /* Status loop: refresh Wi-Fi/uptime/time every tick, and the printer
     * info over IPP every STATUS_PERIOD_MS. Logs a heartbeat and feeds the
     * display via g_status. */
    int since_ipp = STATUS_PERIOD_MS;   /* query immediately on first pass */
    while (true) {
        wifi_ap_record_t ap;
        int rssi = 0;
        bool up = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
        if (up) rssi = ap.rssi;

        wifi_sta_list_t clients;
        int nclients = (esp_wifi_ap_get_sta_list(&clients) == ESP_OK) ? clients.num : 0;

        time_t now = 0;
        time(&now);
        if (now < 1600000000) now = 0;   /* not yet NTP-synced */

        status_lock();
        g_status.wifi_up   = up;
        g_status.rssi      = rssi;
        g_status.uptime_s  = (unsigned)(esp_timer_get_time() / 1000000);
        g_status.now       = now;
        g_status.ap_clients = nclients;
        status_unlock();

        if (since_ipp >= STATUS_PERIOD_MS && nclients > 0) {
            printer_info_t pi;
            if (ipp_get_printer_info(PRINTER_LAN_IP, &pi)) {
                status_lock(); g_status.printer = pi; status_unlock();
                ESP_LOGI(TAG, "[status] '%s' state=%s reasons=%s jobs=%d | rssi=%ddBm up=%us clients=%d",
                         pi.make_and_model, ipp_state_str(pi.state),
                         pi.state_reasons[0] ? pi.state_reasons : "none",
                         pi.queued_jobs, rssi, g_status.uptime_s, nclients);
            } else {
                ESP_LOGW(TAG, "[status] IPP query failed; rssi=%ddBm clients=%d", rssi, nclients);
            }
            since_ipp = 0;
        } else {
            ESP_LOGI(TAG, "[hb] bridge up, %d client(s), rssi=%ddBm", nclients, rssi);
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
        since_ipp += 3000;
    }
}
