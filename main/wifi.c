#include <string.h>
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_system.h"

#include "freertos/event_groups.h"
#include "nvs.h"
#include "log.h"

#include "wifi.h"
#include "globals.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;



bool init_wifi(bool override)
{

    iot_configuration.wifi_settings = get_wifi_settings_from_nvs();
    switch (iot_configuration.wifi_settings.mode) 
    {
        case -1:
        // Put panic here
        break;

        // Factory Default in AP Mode
        case 1:
        wifi_init_softap(iot_configuration.wifi_settings);
        break;
        
        //Start up in infra mode and connect to existing wireless network.
        case 2:
        wifi_init_sta(iot_configuration.wifi_settings);
        break;
    }

    return true;
}


wifi_connection_s get_wifi_settings_from_nvs()
{
    struct wifi_connection_s settings;
    settings.mode = iot_nvs_load_int_value_if_exist("wifi-mode");
    settings.ssid = iot_nvs_load_str_value_if_exist("wifi-ssid");
    settings.passwd = iot_nvs_load_str_value_if_exist("wifi-passwd"); 

    return settings;
}



//static const char *TAG = "Jason's App";

//wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();


static void wifi_event_handler_sta(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 10) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


static void wifi_event_handler_ap(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
  if (event_id == WIFI_EVENT_AP_STACONNECTED) {
      wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
      ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                MAC2STR(event->mac), event->aid);
  } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
      wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
      ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                MAC2STR(event->mac), event->aid);

  }
}


void wifi_init_sta(wifi_connection_s settings)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler_sta,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler_sta,
                                                        NULL,
                                                        &instance_got_ip));
     
    wifi_config_t wifi_config = {
        .sta = {
            //.ssid = EXAMPLE_ESP_WIFI_SSID,
            //.password = EXAMPLE_ESP_WIFI_PASS,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (pasword len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
             * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            
            .threshold.authmode = WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .sae_h2e_identifier = "*",
        },
    }; 
    strncpy((char*)wifi_config.sta.ssid, (char*)settings.ssid, 32);
    strncpy((char*)wifi_config.sta.password, (char*)settings.passwd,64);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 wifi_config.ap.ssid, wifi_config.ap.password);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                wifi_config.ap.ssid, wifi_config.ap.password);
                //iot_nvs_set_int_value_if_exist("wifi-mode", 1);
                ESP_LOGI(TAG, "Resetting back to factory defaults and resetarting.....");
                esp_restart();

    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}




void wifi_init_softap(wifi_connection_s settings)
    {
    ESP_LOGI(TAG, "WiFi Start");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler_ap,
                                                        NULL,
                                                        NULL));

    ESP_LOGI(TAG, "esp_event_handler_instance_register Complete");
    wifi_config_t wifi_config = {
        .ap = {
            .channel = 5,
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                    .required = true,
            },
        },
  };

    strncpy((char*)wifi_config.ap.ssid, (char*)settings.ssid, 32);
    strncpy((char*)wifi_config.ap.password, (char*)settings.passwd, 64);
  
  

    ESP_LOGI(TAG, "AP Configuration Complete.");

    ESP_LOGI(TAG, "Mode set to AP.");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_LOGI(TAG, "Enabling WIFI.");
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_LOGI(TAG, "Starting AP.");
    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s", settings.ssid, settings.passwd);

  
    ESP_ERROR_CHECK(esp_wifi_start());

  
}

