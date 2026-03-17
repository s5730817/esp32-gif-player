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
#define TFT_WIDTH 240
#define TFT_HEIGHT 320

#define XPT_MISO 39
#define XPT_CS 33
#define XPT_IRQ 36
#define XPT_SCLK 25
#define XPT_MOSI 32

static httpd_handle_t server;
static bool gif_uploaded = false;

void ili9341_init(void)
{
	// dev._use_frame_buffer = true;
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
		XPT_MOSI);
	dev._use_frame_buffer = true;
	ESP_LOGI(TAG, "Frame buffer status before init: %d", dev._use_frame_buffer);
	lcdInit(&dev, TFT_MODEL, TFT_WIDTH, TFT_HEIGHT, 0, 0);
	// Using frame buffer causes some error on line 117, reason unknown (looks like Store forbidden eventhough buffer is on)
	ESP_LOGI(TAG, "Frame buffer status after init: %d", dev._use_frame_buffer);
}

uint16_t rgb888_to_bgr565(uint8_t r, uint8_t g, uint8_t b)
{
	uint16_t r5 = (r >> 3) & 0x1F;
	uint16_t g6 = (g >> 2) & 0x3F;
	uint16_t b5 = (b >> 3) & 0x1F;
	return (b5 << 11) | (g6 << 5) | r5; // Blue in high bits, Red in low bits
}

void gif_stream_to_display(gd_GIF *gif)
{
	ESP_LOGI("MEM", "Free heap before decoding: %d", (int)esp_get_free_heap_size());
	if (!gif)
	{
		ESP_LOGE("GIF", "Failed to open GIF");
		return;
	}

	ESP_LOGI("GIF", "Streaming %dx%d GIF", gif->width, gif->height);

	int draw_w = (gif->height < TFT_WIDTH) ? gif->height : TFT_WIDTH;
	int draw_h = (gif->width < TFT_HEIGHT) ? gif->width : TFT_HEIGHT;
	ESP_LOGI("GIF", "Drawing 90deg rotated %dx%d from %dx%d GIF", draw_w, draw_h, gif->width, gif->height);

	uint8_t frame_num = 0;

	while (gd_get_frame(gif))
	{
		if (!gif->frame)
		{
			ESP_LOGE(TAG, "NO FRAME WAS LOADED");
			break;
		}

		ESP_LOGI(TAG, "Frame %i", frame_num);
		static uint16_t line[TFT_WIDTH];
		int chunk_h = draw_h / 6;
		if (chunk_h < 1) chunk_h = 1;
		uint16_t *chunk_buf = malloc(sizeof(uint16_t) * draw_w * chunk_h);
		if (!chunk_buf) {
			ESP_LOGE(TAG, "Out of memory for chunk buffer");
			break;
		}
		for (int y0 = 0; y0 < draw_h; y0 += chunk_h)
		{
			int this_h = (y0 + chunk_h > draw_h) ? (draw_h - y0) : chunk_h;
			for (int yy = 0; yy < this_h; yy++)
			{
				int dest_y = y0 + yy;
				for (int x = 0; x < draw_w; x++)
				{
					int src_x = dest_y;
					int src_y = gif->height - 1 - x;
					uint16_t color = 0xFFFF;
					if (src_x < gif->width && src_y >= 0 && src_y < gif->height)
					{
						uint8_t idx = gif->frame[src_y * gif->width + src_x];
						if (gif->palette && idx < gif->palette->size)
						{
							uint8_t *rgb = &gif->palette->colors[idx * 3];
							color = rgb888_to_bgr565(rgb[0], rgb[1], rgb[2]);
						}
					}
					chunk_buf[yy * draw_w + x] = color;
				}
				lcdDrawMultiPixels(&dev, 0, dest_y, draw_w, &chunk_buf[yy * draw_w]);
				if (draw_w < TFT_WIDTH) {
					for (int x = draw_w; x < TFT_WIDTH; x++) {
						line[x] = 0;
					}
					lcdDrawMultiPixels(&dev, draw_w, dest_y, TFT_WIDTH - draw_w, &line[draw_w]);
				}
			}
		}
		free(chunk_buf);

		if (draw_h < TFT_HEIGHT)
		{
			for (int y = draw_h; y < TFT_HEIGHT; ++y)
			{
				for (int x = 0; x < TFT_WIDTH; ++x)
				{
					line[x] = 0;
				}
				lcdDrawMultiPixels(&dev, 0, y, TFT_WIDTH, line);
			}
		}
		if (draw_w < TFT_WIDTH)
		{
			for (int y = 0; y < draw_h; ++y)
			{
				for (int x = draw_w; x < TFT_WIDTH; ++x)
				{
					line[x] = 0;
				}
				lcdDrawMultiPixels(&dev, draw_w, y, TFT_WIDTH - draw_w, &line[draw_w]);
			}
		}
		frame_num++;
		vTaskDelay(pdMS_TO_TICKS(0));
	}
}

void sdcard_init(void)
{
	esp_err_t ret;
	sdmmc_host_t host = SDSPI_HOST_DEFAULT();

	/// FIX SPI BUS CONFLICT
	host.slot = HSPI_HOST;

	spi_bus_config_t bus_cfg = {
		.mosi_io_num = 23,
		.miso_io_num = 19,
		.sclk_io_num = 18,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 4000,
	};
	ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
	if (ret != ESP_OK)
	{
		ESP_LOGE(TAG, "FAILED TO INIT SPI BUS");
	}

	// 2. Configure SD card device on SPI bus
	sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
	slot_config.gpio_cs = 5; // Change this to your board's CS pin
	slot_config.host_id = host.slot;

	// 3. Mount filesystem
	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
		.format_if_mount_failed = false,
		.max_files = 5,
		.allocation_unit_size = 16 * 1024};
	sdmmc_card_t *card;
	ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

	if (ret != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
		return;
	}

	// 4. Print card info
	sdmmc_card_print_info(stdout, card);
	ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
}

esp_err_t root_get_handler(httpd_req_t *req)
{
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

esp_err_t upload_post_handler(httpd_req_t *req)
{
	ESP_LOGI(TAG, "Upload started, content len=%d", req->content_len);

	ESP_LOGI(TAG, "Received upload");

	FILE *f = fopen("/sdcard/uploaded.gif", "w");
	if (!f)
	{
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
		return ESP_FAIL;
	}

	char buf[SCRATCH_BUFSIZE];
	int total_received = 0;
	bool data_started = false;
	const char *boundary_marker = "------";

	while (1)
	{
		int recv_len = httpd_req_recv(req, buf, sizeof(buf));
		if (total_received >= req->content_len)
			break;

		ESP_LOGI(TAG, "RECEIVED: %d out of %d", total_received, req->content_len);

		if (!data_started)
		{
			char *start = strstr(buf, "\r\n\r\n");
			if (start)
			{
				start += 4;
				int data_len = recv_len - (start - buf);
				fwrite(start, 1, data_len, f);
				data_started = true;
			}
		}
		else
		{
			char *boundary = strstr(buf, boundary_marker);
			if (boundary)
			{
				int data_len = boundary - buf;
				fwrite(buf, 1, data_len, f);
				break;
			}
			else
			{
				fwrite(buf, 1, recv_len, f);
			}
		}

		total_received += recv_len;
	}

	fclose(f);
	ESP_LOGI(TAG, "File uploaded: %d bytes written", total_received);

	gif_uploaded = true;

	// Save state: uploaded -> go display mode next boot
	nvs_handle_t state_handle;
	esp_err_t nvs_err = nvs_open("storage", NVS_READWRITE, &state_handle);
	if (nvs_err == ESP_OK)
	{
		nvs_set_i8(state_handle, "is_upload_state", 0);
		nvs_commit(state_handle);
		nvs_close(state_handle);
	}

	esp_wifi_stop();

	httpd_resp_sendstr(req,
					   "<html><body><h1>Upload complete!</h1><p>File saved to SD card. Server stopping...</p><body></html>");

	return ESP_OK;
}

void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START)
	{
		ESP_LOGI(TAG, "AP started");
	}
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
	{
		ESP_LOGI(TAG, "Client connected");
	}
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
	{
		ESP_LOGI(TAG, "Client disconnected");
	}
}

void webserver_start(void)
{
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	server = NULL;

	// Have to increase the limits for headers otherwise
	// get "Header is too long" on upload
	config.max_uri_handlers = 8;
	config.max_resp_headers = 16;
	config.recv_wait_timeout = 10;
	config.stack_size = 8192;
	// config.max_req_hdr_len = 1024;
	// config.max_uri_len = 1024;
	// config.max_resp_hdr_let = 1024;

	if (httpd_start(&server, &config) == ESP_OK)
	{
		httpd_uri_t root = {
			.uri = "/",
			.method = HTTP_GET,
			.handler = root_get_handler,
			.user_ctx = NULL};
		httpd_register_uri_handler(server, &root);

		httpd_uri_t upload_uri = {
			.uri = "/upload",
			.method = HTTP_POST,
			.handler = upload_post_handler,
			.user_ctx = NULL};
		httpd_register_uri_handler(server, &upload_uri);
	}
}

void wifi_softap_init(void)
{
	ESP_ERROR_CHECK(esp_netif_init());

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;

	ESP_ERROR_CHECK(
		esp_event_handler_instance_register(
			WIFI_EVENT,
			ESP_EVENT_ANY_ID,
			&event_handler,
			NULL,
			&instance_any_id));
	ESP_ERROR_CHECK(
		esp_event_handler_instance_register(
			IP_EVENT,
			IP_EVENT_STA_GOT_IP,
			&event_handler,
			NULL,
			&instance_got_ip));

	ESP_LOGI(TAG, "Wifi softap init");

	esp_netif_create_default_wifi_ap();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	wifi_config_t wifi_config = {
		.ap = {
			.ssid = "ESP_GP",
			.ssid_len = strlen("ESP_GP"),
			.channel = 1,
			.password = "coolPASSWORD12",
			.max_connection = 2,
			.authmode = WIFI_AUTH_WPA_WPA2_PSK},
	};

	if (strlen((char *)wifi_config.ap.password) == 0)
	{
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
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	nvs_handle_t state_handle;
	int8_t is_upload_state = 1;
	esp_err_t err = nvs_open("storage", NVS_READWRITE, &state_handle);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "NVS open failed: %d", err);
		is_upload_state = 1;
	}
	else
	{
		err = nvs_get_i8(state_handle, "is_upload_state", &is_upload_state);
		if (err == ESP_ERR_NVS_NOT_FOUND)
		{
			ESP_LOGI(TAG, "is_upload_state not found in NVS, defaulting to upload mode");
			is_upload_state = 1;
		}
		else if (err != ESP_OK)
		{
			ESP_LOGE(TAG, "Could not read is_upload_state: %d", err);
			is_upload_state = 1;
		}
	}
	ESP_LOGI(TAG, "Loaded is_upload_state=%d", is_upload_state);

	// Toggle for next boot: alternate every reboot
	if (state_handle != 0)
	{
		int8_t next_state = is_upload_state ? 0 : 1;
		nvs_set_i8(state_handle, "is_upload_state", next_state);
		nvs_commit(state_handle);
		ESP_LOGI(TAG, "Set is_upload_state for next boot=%d", next_state);
	}

	// Mount SD card
	sdcard_init();

	ESP_LOGI(TAG, "Before display init, largest free INTERNAL: %d", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

	if (is_upload_state)
	{
		ESP_LOGI(TAG, "Upload mode: starting SoftAP and webserver");
		nvs_close(state_handle);
		wifi_softap_init();
		webserver_start();
	}
	else
	{
		ESP_LOGI(TAG, "Display mode: playing GIF");
		nvs_close(state_handle);
		// Provide a static buffer to avoid heap fragmentation issues
		// Ensure we render directly via SPI (avoids needing a large contiguous frame buffer)
		dev._use_frame_buffer = false;

		ESP_LOGI(TAG, "Initializing display...");
		ili9341_init();
		ESP_LOGI(TAG, "Display initialized, frame buffer: %d", dev._use_frame_buffer);

		ESP_LOGI(TAG, "After display init, largest free INTERNAL: %d", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

		gd_GIF *gif = gd_open_gif("/sdcard/uploaded.gif");

		ESP_LOGI(TAG, "Playing GIF from SD card...");

		while (true)
		{
			gif_stream_to_display(gif);
			ESP_LOGI(TAG, "Rewinding...");
			if (gif == NULL) {
				break;
			}
			gd_rewind(gif);
		}
	}
}
