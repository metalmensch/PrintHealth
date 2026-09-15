idf_component_register(SRCS "printserver_main.c" "ipp_client.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_http_client
                                nvs_flash lwip esp_timer)
