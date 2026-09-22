#include "sdkconfig.h"
#include "lwip/opt.h"
#include "lwip/ip_addr.h"
#include "lwip/lwip_napt.h" 
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
//#include "driver/ledc.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "driver/gpio.h"



// ------------------- CONFIGURATION CONSTANTS -------------------
#define STA_SSID "yourSSID"
#define STA_PASS "yourPASSWORD"

#define AP_SSID  "ESP32_REPEATER"
#define AP_PASS  "12345678"


// Azure IoT Hub Configuration
#define IOTHUB_NAME     "YOURHUB"     // e.g., "my-iot-hub"
#define DEVICE_ID       "ESP32_Repeater"
// Paste your generated SAS Token below (starts with "SharedAccessSignature sr=...")
#define SAS_TOKEN       "yourSASToken"

//status indicators
#define GREEN 19
#define RED 18
#define LED_PWM_FREQUENCY_HZ 5000
#define LED_PWM_RESOLUTION LEDC_TIMER_8_BIT
#define LED_MAX_DUTY ((1 << 8) - 1)
static const char *TAG = "AZURE_NAT_REPEATER";

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool is_azure_connected = false;
static void set_status_leds(bool azure_connected) {
    if (azure_connected) {
        gpio_set_level(GREEN, 1); // Green ON
        gpio_set_level(RED, 0);   // Red OFF
    } else {
        gpio_set_level(GREEN, 0); // Green OFF
        gpio_set_level(RED, 1);   // Red ON
    }
}

// Initialize GPIO pins
static void init_status_leds(void) {
    gpio_reset_pin(GREEN);
    gpio_reset_pin(RED);

    gpio_set_direction(GREEN, GPIO_MODE_OUTPUT);
    gpio_set_direction(RED, GPIO_MODE_OUTPUT);

    // Default state at boot: Disconnected (Red ON, Green OFF)
    set_status_leds(false);
}


// Azure Root CA Certificate (DigiCert Global Root G2 - used by Azure IoT Hub)
extern const char azure_root_ca_pem_start[] asm("_binary_azure_root_ca_pem_start");
extern const char azure_root_ca_pem_end[]   asm("_binary_azure_root_ca_pem_end");

// Fallback embedded CA String if binary embedding is omitted
const char* default_azure_ca = 
"-----BEGIN CERTIFICATE-----\n"
"MIIDjjCCAnagAwIBAgIQAzRx57A84442806613544zANBgkqhkiG9w0BAQsFADBh\n"
"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwRAYDVQQDEylE\n"
"aWdpQ2VydCBHbG9iYWwgUm9vdCBHMiBIaWdoIEFzc3VyYW5jZSBDQS0zMB4XDTEz\n"
"MDQwMTEyMDAwMFoXDTI4MDQwMTEyMDAwMFowYTELMAkGA1UEBhMCVVMxFTATBgNV\n"
"BAoTDERpZ2lDZXJ0IEluYzEZMBcGA1UEAxMQRGlnaUNlcnQgR2xvYmFsIFJvb3Qg\n"
"RzIwGGMAA1UEBhMCVVMxFTATBgNVBAoTDERpZ2lDZXJ0IEluYzEZMBcGA1UEAxMQ\n"
"RGlnaUNlcnQgR2xvYmFsIFJvb3QgRzIwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAw\n"
"ggEKAoIBAQC3j4vqKo93yY148gI4E4R2mU9m44G2H716a4959124976g90465\n"
"-----END CERTIFICATE-----\n";

// ------------------- AZURE MQTT EVENT HANDLER -------------------
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Successfully connected to Azure IoT Hub!");
            is_azure_connected = true;
            set_status_leds(true); // Green ON, Red OFF
            esp_mqtt_client_subscribe(mqtt_client, "$iothub/twin/PATCH/properties/desired/#", 1);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from Azure IoT Hub. Retrying...");
            is_azure_connected = false;
            set_status_leds(false); // Red ON, Green OFF
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Incoming Azure Message/Twin Update:");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);

            // Parse incoming JSON Twin patch
            cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
            if (root) {
                cJSON *desired = cJSON_GetObjectItem(root, "desired");
                cJSON *target = desired ? desired : root;
                cJSON *new_ssid = cJSON_GetObjectItem(target, "extenderSsid");
                if (cJSON_IsString(new_ssid) && (new_ssid->valuestring != NULL)) {
                    ESP_LOGI(TAG, "Updating Extender SSID via Azure Device Twin to: %s", new_ssid->valuestring);
                    wifi_config_t ap_config;
                    esp_wifi_get_config(WIFI_IF_AP, &ap_config);
                    strncpy((char*)ap_config.ap.ssid, new_ssid->valuestring, sizeof(ap_config.ap.ssid));
                    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
                }
                cJSON_Delete(root);
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT TLS Error Encountered");
            break;

        default:
            break;
    }
}

// ------------------- AZURE TELEMETRY TASK -------------------
static void azure_telemetry_task(void *pvParameters) {
    char pub_topic[128];
    snprintf(pub_topic, sizeof(pub_topic), "devices/%s/messages/events/", DEVICE_ID);

    while (1) {
        if (is_azure_connected) {
            wifi_ap_record_t ap_info;
            wifi_sta_list_t sta_list;

            int rssi = -99;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                rssi = ap_info.rssi;
            }
            esp_wifi_ap_get_sta_list(&sta_list);

            // Build JSON telemetry packet
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "deviceId", DEVICE_ID);
            
            cJSON *upstream = cJSON_CreateObject();
            cJSON_AddNumberToObject(upstream, "rssi", rssi);
            cJSON_AddNumberToObject(upstream, "channel", ap_info.primary);
            cJSON_AddItemToObject(root, "upstream", upstream);

            cJSON *extender = cJSON_CreateObject();
            cJSON_AddNumberToObject(extender, "connectedClients", sta_list.num);
            cJSON_AddItemToObject(root, "extender", extender);

            cJSON *system = cJSON_CreateObject();
            cJSON_AddNumberToObject(system, "freeHeapBytes", esp_get_free_heap_size());
            cJSON_AddNumberToObject(system, "uptimeSeconds", xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
            cJSON_AddItemToObject(root, "system", system);

            char *json_str = cJSON_PrintUnformatted(root);
            
            int msg_id = esp_mqtt_client_publish(mqtt_client, pub_topic, json_str, 0, 1, 0);
            ESP_LOGI(TAG, "Published Azure Telemetry (Msg ID: %d): %s", msg_id, json_str);

            cJSON_free(json_str);
            cJSON_Delete(root);
        }
        vTaskDelay(pdMS_TO_TICKS(10000)); // Publish every 10 seconds
    }
}

// ------------------- INITIALIZE AZURE MQTT -------------------
static void init_azure_mqtt(void) {
    char uri[256];
    char client_id[128];
    char username[256];

    snprintf(uri, sizeof(uri), "mqtts://%s.azure-devices.net:8883", IOTHUB_NAME);
    snprintf(client_id, sizeof(client_id), "%s", DEVICE_ID);
    snprintf(username, sizeof(username), "%s.azure-devices.net/%s/?api-version=2021-04-12", IOTHUB_NAME, DEVICE_ID);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = uri,
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
        },
        .credentials = {
            .client_id = client_id,
            .username = username,
            .authentication.password = SAS_TOKEN,
        },
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    xTaskCreate(azure_telemetry_task, "azure_telemetry_task", 4096, NULL, 5, NULL);
}
// ------------------- WI-FI & NAPT SYSTEM EVENTS -------------------

    // ------------------- WI-FI & NAPT SYSTEM EVENTS -------------------
// ------------------- WI-FI & NAPT SYSTEM EVENTS -------------------
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected from upstream AP, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "STA Connected. Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // Enable LwIP NAPT on SoftAP interface (192.168.4.1)
        u32_t ap_ip = ipaddr_addr("192.168.4.1");
        ip_napt_enable(ap_ip, 1);
        ESP_LOGI(TAG, "LwIP NAPT Enabled on 192.168.4.1");

        // Initialize Azure MQTT connection after network stack acquires IP
        if (!mqtt_client) {
            init_azure_mqtt();
        }
    }
}

// ------------------- APPLICATION MAIN -------------------
// ------------------- APPLICATION MAIN -------------------
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    init_status_leds();

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // MUST BE CALLED AFTER esp_wifi_init()
    wifi_country_t country = {
        .cc = "IN",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_AUTO
    };
    esp_wifi_set_country(&country);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t sta_config = {
        .sta = {
            .ssid = STA_SSID,
            .password = STA_PASS,
        },
    };
    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .password = AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "ESP32 AP+STA Repeater Booted. Connecting to %s...", STA_SSID);
}