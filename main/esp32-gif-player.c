#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "gifdec.h"
#include "ili9340.h"

#define MOUNT_POINT "/sdcard"
const char *TAG = "esp_gif_player";

#define FILE_PATH_MAX 128
#define SCRATCH_BUFSIZE 1024

static TFT_t dev;

#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS 15
#define TFT_DC 2
#define TFT_RESET -1
#define TFT_BL 21

#define TFT_MODEL 0x9341
#define TFT_WIDTH 160
#define TFT_HEIGHT 120

#define XPT_MISO 39
#define XPT_CS 33
#define XPT_IRQ 36
#define XPT_SCLK 25
#define XPT_MOSI 32

void ili9341_init(void) {
	spi_master_init(
		&dev, 
		TFT_MOSI, 
		TFT_SCLK, 
		TFT_CS, 
		TFT_DC, 
		TFT_RESET, 
		TFT_BL,
		XPT_MISO,
		XPT_CS,
		XPT_IRQ,
		XPT_SCLK,
		XPT_MOSI
	);
	lcdInit(&dev, TFT_MODEL, TFT_WIDTH, TFT_HEIGHT, 0, 0);

}

void gif_stream_to_display(const char *path) {
    gd_GIF *gif = gd_open_gif(path);
	ESP_LOGI("MEM", "Free heap before decoding: %d", (int)esp_get_free_heap_size());
    if (!gif) {
        ESP_LOGE("GIF", "Failed to open GIF");
        return;
    }

    ESP_LOGI("GIF", "Streaming %dx%d GIF", gif->width, gif->height);


	uint8_t MIN_WIDTH = (TFT_WIDTH < gif->width) ? TFT_WIDTH : gif->width;
	uint8_t MIN_HEIGHT = (TFT_HEIGHT < gif->height) ? TFT_HEIGHT : gif->height;

	const int pal_size = gif->palette ? gif->palette->size : 0;
	uint16_t *pal565 = NULL;

	if (pal_size) {
		pal565 = malloc(pal_size * sizeof(uint16_t));
        if (pal565) {
            for (int i = 0; i < pal_size; ++i) {
                uint8_t *c = &gif->palette->colors[i*3];
                pal565[i] = ((c[0] & 0xF8) << 8) | ((c[1] & 0xFC) << 3) | (c[2] >> 3);
            }
        }
    }

    while (gd_get_frame(gif)) {
		if (!gif->frame) {
			ESP_LOGE(TAG, "NO FRAME WAS LOADED");
			break;
		}
		
		for (int y=0; y<MIN_WIDTH; ++y) {
			int y_src = (y * gif->height) / MIN_HEIGHT;
			if (y_src >= gif->height) {
				y_src = gif->height - 1;
			}
			for (int x=0; x<MIN_WIDTH; ++x) {
				int x_src = (x * gif->width) / MIN_WIDTH;
				if (x_src >= gif->width) {
					x_src = gif->width - 1;
				}
				uint8_t idx = gif->frame[y_src * gif->width + x_src];

				uint16_t color;
				if (pal565 && idx < pal_size) {
					color = pal565[idx];
				} else {
					uint8_t *rgb = &gif->palette->colors[idx*3];
					color = ((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3);
				}

				dev._frame_buffer[y * TFT_WIDTH + x] = color;
			}
		}

		int delay_ms = gif->gce.delay * 10;
		delay_ms = delay_ms > 20 ? delay_ms : 20;

		ESP_LOGI(TAG, "DRAWING FRAME RIGHT NOW!");
		ESP_LOGI(TAG, "TFT size: %dx%d", dev._width, dev._height);
		ESP_LOGI(TAG, "Frame buffer ptr: %p", dev._frame_buffer);
		ESP_LOGI(TAG, "Use frame buffer? %d", dev._use_frame_buffer);

		lcdDrawFinish(&dev);
		ESP_LOGI(TAG, "FRAME DRAWN!");
        // Delay between frames
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
	if (pal565) free(pal565);
    gd_close_gif(gif);
}

void sdcard_init(void) {
	esp_err_t ret;
	sdmmc_host_t host = SDSPI_HOST_DEFAULT();
	
	/// FIX SPI BUS CONFLICT
	host.slot = VSPI_HOST;
	
	
	spi_bus_config_t bus_cfg = {
		.mosi_io_num = 23,
		.miso_io_num = 19,
		.sclk_io_num = 18,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 4000,
	};
	ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "FAILED TO INIT SPI BUS");
	}

	// 2. Configure SD card device on SPI bus
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = 5;          // Change this to your board's CS pin
    slot_config.host_id = host.slot;

    // 3. Mount filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return;
    }

    // 4. Print card info
    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
}

esp_err_t root_get_handler(httpd_req_t *req) {
	char *resp_str = 
	"<!DOCTYPE html>"
	"<html>"
	"<body>"
	"<h1>Upload GIF</h1>"
	"<form method=\"POST\" action=\"/upload\" enctype=\"multipart/form-data\">"
	"  <input type=\"file\" name=\"file\" accept=\".gif\">"
	"  <input type=\"submit\" value=\"Upload\">"
	"</form>"
	"</body>"
	"</html>";

	httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
	return ESP_OK;
}

esp_err_t upload_post_handler(httpd_req_t *req) {
	ESP_LOGI(TAG, "Upload started, content len=%d", req->content_len);
	
	char filepath[FILE_PATH_MAX] = MOUNT_POINT "/uploaded.gif";
	ESP_LOGI(TAG, "Received upload");
	
	FILE *f = fopen("/sdcard/uploaded.gif", "w");
	if (!f) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
		return ESP_FAIL;
	}

	char buf[SCRATCH_BUFSIZE];
	int received;
	int total_received = 0;
	bool data_started = false;
	int boundary_offset = 0;
	const char *boundary_marker = "------";
	
	while (1) {
		int recv_len = httpd_req_recv(req, buf, sizeof(buf));
		if (total_received >= req->content_len) break;
		
		ESP_LOGI(TAG, "RECEIVED: %d out of %d", total_received, req->content_len);
		
		if (!data_started) {
			char *start = strstr(buf, "\r\n\r\n");
			if (start) {
				start += 4;
				int data_len = recv_len - (start - buf);
				fwrite(start, 1, data_len, f);
				data_started = true;
			}
		} else {
			char *boundary = strstr(buf, boundary_marker);
			if (boundary) {
				int data_len = boundary - buf;
				fwrite(buf, 1, data_len, f);
				break;
			} else {
				fwrite(buf, 1, recv_len, f);
			}
		}
			
			total_received += recv_len;

	}

	fclose(f);
	ESP_LOGI(TAG, "File uploaded: %d bytes written", total_received);

	httpd_resp_sendstr(req, 
	"<html><body><h1>Upload complete!</h1><p>File saved to SD card</p><body></html>");

	return ESP_OK;
}
	/*
	while (total_received < req->content_len) {
		ESP_LOGI(TAG, "RECEIVED %d out of %d", total_received, req->content_len);
		//ESP_LOGI(TAG, "Received: %d", received);
		received = httpd_req_recv(req, buf, sizeof(buf));
		fwrite(buf, 1, received, f);
		total_received += received;
	}
	
	if (total_received <= 0) {
		ESP_LOGE(TAG, "File upload failed!");
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
		return ESP_FAIL;
	}

	fclose(f);
	ESP_LOGI(TAG, "File uploaded: %d bytes written", total_received);

	httpd_resp_sendstr(req, 
	"<html><body><h1>Upload complete!</h1><p>File saved to SD card</p><body></html>");
	return ESP_OK;}
	*/

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
	
	// Have to increase the limits for headers otherwise
	// get "Header is too long" on upload
	config.max_uri_handlers = 8;
	config.max_resp_headers = 16;
	config.recv_wait_timeout = 10;
	config.stack_size = 8192;
	//config.max_req_hdr_len = 1024;
	//config.max_uri_len = 1024;
	//config.max_resp_hdr_let = 1024;
	
	if (httpd_start(&server, &config) == ESP_OK) {
		httpd_uri_t root = {
			.uri = "/",
			.method = HTTP_GET,
			.handler = root_get_handler,
			.user_ctx = NULL
		};
		httpd_register_uri_handler(server, &root);

		httpd_uri_t upload_uri = {
			.uri = "/upload",
			.method = HTTP_POST,
			.handler = upload_post_handler,
			.user_ctx = NULL
		};
		httpd_register_uri_handler(server, &upload_uri);
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


void fb_test_task(TFT_t *arg)
{
    TFT_t *d = &dev; // your global dev
    ESP_LOGI(TAG, "TFT size: %dx%d", d->_width, d->_height);
    size_t pixels = (size_t)d->_width * d->_height;
    ESP_LOGI(TAG, "fb ptr=%p pixels=%u bytes=%u", d->_frame_buffer, (unsigned)pixels, (unsigned)(pixels * sizeof(uint16_t)));

    // Fill with gradient
    for (int y = 0; y < d->_height; ++y) {
        for (int x = 0; x < d->_width; ++x) {
            size_t idx = (size_t)y * d->_width + x;
            if (idx < pixels) {
                // simple color pattern
                d->_frame_buffer[idx] = ((x & 0x1F) << 11) | ((y & 0x3F) << 5) | ((x+y) & 0x1F);
            } else {
                // won't happen with correct indexing
            }
        }
    }

    ESP_LOGI(TAG, "About to flush...");
    lcdDrawFinish(d);
    ESP_LOGI(TAG, "Flush done.");
    vTaskDelete(NULL);
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
	
		
	sdcard_init();
	wifi_softap_init();
	webserver_start();

	ESP_LOGI(TAG, "WEBSERVER AT 192.168.4.1");
	
	ili9341_init();
	lcdFillScreen(&dev, 0x1F3F1F);
	fb_test_task(&dev);
	//gif_stream_to_display("/sdcard/UPLOADED.GIF");
}
