#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"

const char *TAG = "esp_gif_player";

esp_err_t root_get_handler(httpd_req_t *req) {
	char *resp_str = "<!DOCTYPE html><html><body><h1>Hello from ESP32!</h1></body></html>";
	httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
	return ESP_OK;
}

void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
		ESP_LOGI(TAG, "AP started");
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
		ESP_LOGI(TAG, "Client connected");
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
		ESP_LOGI(TAG, "Client disconnected");
	}
}

httpd_handle_t webserver_start(void) {
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	httpd_handle_t server = NULL;

	if (httpd_start(&server, &config) == ESP_OK) {
		httpd_uri_t root = {
			.uri = "/",
			.method = HTTP_GET,
			.handler = root_get_handler,
			.user_ctx = NULL
		};
		httpd_register_uri_handler(server, &root);
	}
	return server;
}

void wifi_softap_init(void) {
	ESP_LOGI(TAG, "Wifi softap init");

	esp_netif_create_default_wifi_ap();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	wifi_config_t wifi_config = {
		.ap = {
			.ssid = "ESP_GIF_PLAYER",
			.ssid_len = strlen("ESP_GIF_PLAYER"),
			.channel = 1,
			.password = "coolPASSWORD12",
			.max_connection = 2,
			.authmode = WIFI_AUTH_WPA_WPA2_PSK
		},
	};

	if (strlen((char *)wifi_config.ap.password) == 0) {
		wifi_config.ap.authmode = WIFI_AUTH_OPEN;
	}

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_LOGI(TAG, "Wi-Fi AP started. SSID:%s Password:%s", 
             wifi_config.ap.ssid, wifi_config.ap.password);
}


void app_main(void)
{
	// NVS INIT	
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	// lwIP INIT
	ESP_ERROR_CHECK(esp_netif_init());
	
	// Default event loop
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;
	ESP_ERROR_CHECK(
		esp_event_handler_instance_register(			
											WIFI_EVENT,
	                                        ESP_EVENT_ANY_ID,
	                                        &event_handler,
	                                        NULL,
	                                        &instance_any_id)
					);
	ESP_ERROR_CHECK(
		esp_event_handler_instance_register(
											IP_EVENT,
	                                        IP_EVENT_STA_GOT_IP,
	                                        &event_handler,
	                                        NULL,
	                                        &instance_got_ip)
					);
	
		
	wifi_softap_init();
	webserver_start();

	ESP_LOGI(TAG, "WEBSERVER AT 192.168.4.1");
	
}
