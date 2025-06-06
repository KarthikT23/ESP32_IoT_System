/*
 * wifi_app.c
 *
 *  Created on: 8 Dec 2024
 *      Author: karthik
 */
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "app_nvs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "http_server.h"
#include "lwip/netdb.h"
#include "rgb_led.h"
#include "tasks_common.h"
#include "wifi_app.h"
#include <string.h>

// Tag used for ESP Serial Communication
static const char TAG[] = "wifi_app";

// WiFi application callback
static wifi_connected_event_callback_t wifi_connected_event_cb;

// Used for returning the WiFi configuration
static wifi_config_t *wifi_config = NULL;

// Used to track the number of retries when a connection attempt fails
static int g_retry_number;

// WiFi application event group handle and status bits
static EventGroupHandle_t wifi_app_event_group;
static const int WIFI_APP_CONNECTING_USING_SAVED_CREDS_BIT = BIT0;
static const int WIFI_APP_CONNECTING_FROM_HTTP_SERVER_BIT = BIT1;
static const int WIFI_APP_STA_CONNECTED_GOT_IP_BIT = BIT2;

// Queue handle used to manipulate the main queue of events
static QueueHandle_t wifi_app_queue_handle;

// Reset button semaphore
static SemaphoreHandle_t wifi_reset_semaphore = NULL;

// netif objects for the station and access point
esp_netif_t* esp_netif_sta = NULL;
esp_netif_t* esp_netif_ap = NULL;

// Forward declarations
static void wifi_app_task(void *pvParameters);
static void wifi_reset_button_task(void *pvParam);

/*
 * WiFi Reset Button Functions
 */
void IRAM_ATTR wifi_reset_button_isr_handler(void *arg)
{
	xSemaphoreGiveFromISR(wifi_reset_semaphore, NULL);
}

static void wifi_reset_button_task(void *pvParam)
{
	for(;;)
	{
		if (xSemaphoreTake(wifi_reset_semaphore, portMAX_DELAY) == pdTRUE)
		{
			ESP_LOGI(TAG, "WiFi Reset Button Pressed");
			wifi_app_send_message(WIFI_APP_MSG_USER_REQUESTED_STA_DISCONNECT);
			vTaskDelay(pdMS_TO_TICKS(2000));
		}
	}
}

static void wifi_reset_button_config(void)
{
	// Create the binary semaphore
	wifi_reset_semaphore = xSemaphoreCreateBinary();
	
	// Configure the button
	gpio_config_t io_conf = {
		.pin_bit_mask = (1ULL << WIFI_RESET_BUTTON),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_NEGEDGE
	};
	gpio_config(&io_conf);
	
	// Create the wifi reset button task
	xTaskCreatePinnedToCore(&wifi_reset_button_task, "wifi_reset_button", 
		WIFI_RESET_BUTTON_TASK_STACK_SIZE, NULL, WIFI_RESET_BUTTON_TASK_PRIORITY, 
		NULL, WIFI_RESET_BUTTON_TASK_CORE_ID);
	
	// Install gpio isr service
	gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
	
	// Attach the interrupt service routine
	gpio_isr_handler_add(WIFI_RESET_BUTTON, wifi_reset_button_isr_handler, NULL);
}

/*
 * WiFi Event Handler
 */
static void wifi_app_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT)
	{
		switch (event_id)
		{
			case WIFI_EVENT_AP_START:
				ESP_LOGI(TAG, "WIFI_EVENT_AP_START");
				break;
			
			case WIFI_EVENT_AP_STOP:
				ESP_LOGI(TAG, "WIFI_EVENT_AP_STOP");
				break;
				
			case WIFI_EVENT_AP_STACONNECTED:
				ESP_LOGI(TAG, "WIFI_EVENT_AP_STACONNECTED");
				break;	
				
			case WIFI_EVENT_AP_STADISCONNECTED:
				ESP_LOGI(TAG, "WIFI_EVENT_AP_STADISCONNECTED");
				break;		
										
			case WIFI_EVENT_STA_START:
				ESP_LOGI(TAG, "WIFI_EVENT_STA_START");
				break;	
				
			case WIFI_EVENT_STA_CONNECTED:
				ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED");
				break;	
				
			case WIFI_EVENT_STA_DISCONNECTED:
				ESP_LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED");
				
				wifi_event_sta_disconnected_t *disconnect_event = (wifi_event_sta_disconnected_t*)event_data;
				ESP_LOGI(TAG, "Disconnect reason: %d", disconnect_event->reason);
				
				if (g_retry_number < MAX_CONNECTION_RETRIES)
				{
					esp_wifi_connect();
					g_retry_number++;
				}
				else
				{
					wifi_app_send_message(WIFI_APP_MSG_STA_DISCONNECTED);
				}
				break;		
		}		
	}
	else if (event_base == IP_EVENT)
	{
		switch (event_id)
		{
			case IP_EVENT_STA_GOT_IP:
				ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP");
				wifi_app_send_message(WIFI_APP_MSG_STA_CONNECTED_GOT_IP);
				break;
		}
	}
}

// Initializes the WiFi application event handler for WiFi and IP events
static void wifi_app_event_handler_init()
{
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	
	esp_event_handler_instance_t instance_wifi_event;
	esp_event_handler_instance_t instance_ip_event;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_app_event_handler, NULL, &instance_wifi_event));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_app_event_handler, NULL, &instance_ip_event));
}

// Initializes the TCP stack and default WiFi configuration
static void wifi_app_default_wifi_init(void)
{
	ESP_ERROR_CHECK(esp_netif_init());
	
	wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	esp_netif_sta = esp_netif_create_default_wifi_sta();
	esp_netif_ap = esp_netif_create_default_wifi_ap();
}

// Configures the WiFi access point settings and assigns the static IP to the SoftAP
static void wifi_app_soft_ap_config(void)
{
	wifi_config_t ap_config = {
		.ap = {
			.ssid = WIFI_AP_SSID,
			.ssid_len = strlen(WIFI_AP_SSID),
			.password = WIFI_AP_PASSWORD,
			.channel = WIFI_AP_CHANNEL,
			.ssid_hidden = WIFI_AP_SSID_HIDDEN,
			.authmode = WIFI_AUTH_WPA2_PSK,
			.max_connection = WIFI_AP_MAX_CONNECTIONS,
			.beacon_interval = WIFI_AP_BEACON_INTERVAL,
		},
	};
	
	// Configure DHCP for the AP
	esp_netif_ip_info_t ap_ip_info;
	memset(&ap_ip_info, 0x00, sizeof(ap_ip_info));
	
	esp_netif_dhcps_stop(esp_netif_ap);
	inet_pton(AF_INET, WIFI_AP_IP, &ap_ip_info.ip);
	inet_pton(AF_INET, WIFI_AP_GATEWAY, &ap_ip_info.gw);
	inet_pton(AF_INET, WIFI_AP_NETMASK, &ap_ip_info.netmask);
	ESP_ERROR_CHECK(esp_netif_set_ip_info(esp_netif_ap, &ap_ip_info));
	ESP_ERROR_CHECK(esp_netif_dhcps_start(esp_netif_ap));
	
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
	ESP_ERROR_CHECK(esp_wifi_set_bandwidth(ESP_IF_WIFI_AP, WIFI_AP_BANDWIDTH));
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_STA_POWER_SAVE));
}

// Connects the ESP32 to an external AP using the updated station configuration
static void wifi_app_connect_sta(void)
{
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, wifi_app_get_wifi_config()));
	ESP_ERROR_CHECK(esp_wifi_connect());
}

// Handles successful connection
static void handle_wifi_connected(void)
{
	EventBits_t eventBits = xEventGroupGetBits(wifi_app_event_group);
	
	xEventGroupSetBits(wifi_app_event_group, WIFI_APP_STA_CONNECTED_GOT_IP_BIT);
	rgb_led_wifi_connected();
	http_server_monitor_send_message(HTTP_MSG_WIFI_CONNECT_SUCCESS);
	
	// Save credentials only if connecting from HTTP server (not from saved creds)
	if (eventBits & WIFI_APP_CONNECTING_USING_SAVED_CREDS_BIT) 
	{
		xEventGroupClearBits(wifi_app_event_group, WIFI_APP_CONNECTING_USING_SAVED_CREDS_BIT);
	}
	else 
	{
		app_nvs_save_sta_creds();
	}
	
	if (eventBits & WIFI_APP_CONNECTING_FROM_HTTP_SERVER_BIT)
	{
		xEventGroupClearBits(wifi_app_event_group, WIFI_APP_CONNECTING_FROM_HTTP_SERVER_BIT);
	}
	
	// Call callback if set
	if (wifi_connected_event_cb)
	{
		wifi_app_call_callback();
	}
}

// Handles disconnection based on context
static void handle_wifi_disconnected(void)
{
	EventBits_t eventBits = xEventGroupGetBits(wifi_app_event_group);
	
	if (eventBits & WIFI_APP_CONNECTING_USING_SAVED_CREDS_BIT)
	{
		ESP_LOGI(TAG, "Failed to connect using saved credentials");
		xEventGroupClearBits(wifi_app_event_group, WIFI_APP_CONNECTING_USING_SAVED_CREDS_BIT);
		app_nvs_clear_sta_creds();
	}
	else if (eventBits & WIFI_APP_CONNECTING_FROM_HTTP_SERVER_BIT)
	{
		ESP_LOGI(TAG, "Failed to connect from HTTP server");
		xEventGroupClearBits(wifi_app_event_group, WIFI_APP_CONNECTING_FROM_HTTP_SERVER_BIT);
		http_server_monitor_send_message(HTTP_MSG_WIFI_CONNECT_FAIL);
	}
	else 
	{
		ESP_LOGI(TAG, "Unexpected disconnection");
	}
	
	// Clear connected bit if it was set
	if (eventBits & WIFI_APP_STA_CONNECTED_GOT_IP_BIT)
	{
		xEventGroupClearBits(wifi_app_event_group, WIFI_APP_STA_CONNECTED_GOT_IP_BIT);
	}
}

// Main WiFi application task
static void wifi_app_task(void *pvParameters)
{
	wifi_app_queue_message_t msg;
	
	// Initialize the event handler
	wifi_app_event_handler_init();
	
	// Initialize the TCP/IP stack and WiFi config
	wifi_app_default_wifi_init();
		
	// SoftAP config
	wifi_app_soft_ap_config();
	
	// Start WiFi
	ESP_ERROR_CHECK(esp_wifi_start());
	
	// Send first event message
	wifi_app_send_message(WIFI_APP_MSG_LOAD_SAVED_CREDENTIALS);
	
	for(;;)
	{
		if (xQueueReceive(wifi_app_queue_handle, &msg, portMAX_DELAY))
		{
			switch (msg.msgID)
			{
				case WIFI_APP_MSG_LOAD_SAVED_CREDENTIALS:
					ESP_LOGI(TAG, "WIFI_APP_MSG_LOAD_SAVED_CREDENTIALS");
					
					if (app_nvs_load_sta_creds())
					{
						ESP_LOGI(TAG, "Loaded station configuration");
						wifi_app_connect_sta();
						xEventGroupSetBits(wifi_app_event_group, WIFI_APP_CONNECTING_USING_SAVED_CREDS_BIT);
					}
					else 
					{
						ESP_LOGI(TAG, "Unable to load station configuration");
					}
					wifi_app_send_message(WIFI_APP_MSG_START_HTTP_SERVER);
					break;
					
				case WIFI_APP_MSG_START_HTTP_SERVER:
					ESP_LOGI(TAG, "WIFI_APP_MSG_START_HTTP_SERVER");
					http_server_start();
					rgb_led_http_server_started();
					break;
					
				case WIFI_APP_MSG_CONNECTING_FROM_HTTP_SERVER:
					ESP_LOGI(TAG, "WIFI_APP_MSG_CONNECTING_FROM_HTTP_SERVER");
					xEventGroupSetBits(wifi_app_event_group, WIFI_APP_CONNECTING_FROM_HTTP_SERVER_BIT);
					wifi_app_connect_sta();
					g_retry_number = 0;
					http_server_monitor_send_message(HTTP_MSG_WIFI_CONNECT_INIT);
					break;
					
				case WIFI_APP_MSG_STA_CONNECTED_GOT_IP:
					ESP_LOGI(TAG, "WIFI_APP_MSG_STA_CONNECTED_GOT_IP");
					handle_wifi_connected();
					break;
				
				case WIFI_APP_MSG_USER_REQUESTED_STA_DISCONNECT:
					ESP_LOGI(TAG, "WIFI_APP_MSG_USER_REQUESTED_STA_DISCONNECT");
					
					if (xEventGroupGetBits(wifi_app_event_group) & WIFI_APP_STA_CONNECTED_GOT_IP_BIT)
					{
						g_retry_number = MAX_CONNECTION_RETRIES; // Prevent auto-reconnect
						ESP_ERROR_CHECK(esp_wifi_disconnect());
						app_nvs_clear_sta_creds();
						ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
						rgb_led_http_server_started();
						http_server_monitor_send_message(HTTP_MSG_WIFI_USER_DISCONNECT);
						xEventGroupClearBits(wifi_app_event_group, WIFI_APP_STA_CONNECTED_GOT_IP_BIT);
					}
					break;
					
				case WIFI_APP_MSG_STA_DISCONNECTED:
					ESP_LOGI(TAG, "WIFI_APP_MSG_STA_DISCONNECTED");
					handle_wifi_disconnected();
					break;	

				default:
					break;		 	
			}
		}
	}
}

/*
 * Public API Functions
 */
BaseType_t wifi_app_send_message(wifi_app_message_e msgID)
{
	wifi_app_queue_message_t msg;
	msg.msgID = msgID;
	return xQueueSend(wifi_app_queue_handle, &msg, portMAX_DELAY);	
}

wifi_config_t* wifi_app_get_wifi_config(void)
{
	return wifi_config;
}

void wifi_app_set_callback(wifi_connected_event_callback_t cb)
{
	wifi_connected_event_cb = cb;
}

void wifi_app_call_callback(void)
{
	if (wifi_connected_event_cb)
	{
		wifi_connected_event_cb();
	}
}

void wifi_app_start(void)
{
	ESP_LOGI(TAG, "Starting WiFi Application");
	
	// Start WiFi started LED
	rgb_led_wifi_app_started();
	
	// Disable default WiFi logging messages
	esp_log_level_set("wifi", ESP_LOG_NONE);
	
	// Allocate memory for the wifi configuration
	wifi_config = (wifi_config_t*)malloc(sizeof(wifi_config_t));
	memset(wifi_config, 0x00, sizeof(wifi_config_t));
	
	// Create message queue
	wifi_app_queue_handle = xQueueCreate(3, sizeof(wifi_app_queue_message_t));
	
	// Create wifi application event group
	wifi_app_event_group = xEventGroupCreate();
	
	// Configure WiFi reset button
	wifi_reset_button_config();
	
	// Start the WiFi application task
	xTaskCreatePinnedToCore(&wifi_app_task, "wifi_app_task", WIFI_APP_TASK_STACK_SIZE, NULL, WIFI_APP_TASK_PRIORITY, NULL, WIFI_APP_TASK_CORE_ID);
}