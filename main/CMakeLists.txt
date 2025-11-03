idf_component_register(
	SRCS "esp32-gif-player.c"
        INCLUDE_DIRS "."
	REQUIRES esp_wifi esp_netif esp_event esp_http_server nvs_flash driver fatfs sdmmc ili9340 gifdec
)

