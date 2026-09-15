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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/lwip_napt.h"

#include "secrets.h"

/* SoftAP tunables. The channel is only a starting point: in AP+STA
 * coexistence the single radio forces the SoftAP onto the STA's channel
 * once the home connection is up, so the printer ends up on whatever
 * band/channel the home Wi-Fi uses (5 GHz if you gave a 5 GHz SSID). */
#define AP_START_CHANNEL   1
#define AP_MAX_CONN        4
#define STA_MAX_RETRY      10

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
        esp_wifi_connect();
        ESP_LOGI(TAG, "station started, connecting to '%s'", HOME_WIFI_SSID);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < STA_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "home Wi-Fi disconnected, retry %d/%d", s_retry_num, STA_MAX_RETRY);
        } else {
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

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    &wifi_event_handler, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_init_softap();
    wifi_init_sta();

    ESP_ERROR_CHECK(esp_wifi_start());

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
    } else {
        ESP_LOGE(TAG, "could not join home Wi-Fi '%s' -- check secrets.h", HOME_WIFI_SSID);
        return;
    }

    /* Heartbeat so the serial log shows the bridge is alive and how many
     * clients (the printer) are associated. */
    while (true) {
        wifi_sta_list_t clients;
        if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK) {
            ESP_LOGI(TAG, "[hb] bridge up, %d client(s) on AP", clients.num);
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
