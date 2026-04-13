#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "onewire_bus.h"
#include "ds18b20.h"
#include "mqtt5_client.h"
#include "Secrets.h"


// Define if using internal pulup or not
#if CONFIG_EXAMPLE_ONEWIRE_ENABLE_INTERNAL_PULLUP
#define ONEWIRE_ENABLE_INTERNAL_PULLUP 1
#else
#define ONEWIRE_ENABLE_INTERNAL_PULLUP 0
#endif

// Define if using UART or not
#if CONFIG_EXAMPLE_ONEWIRE_BACKEND_UART
#define ONEWIRE_UART_PORT_NUM CONFIG_EXAMPLE_ONEWIRE_UART_PORT_NUM
#endif

// Define BUS GPIO and max amount of sensors
#define ONEWIRE_BUS_GPIO    CONFIG_EXAMPLE_ONEWIRE_BUS_GPIO
#define ONEWIRE_MAX_DS18B20 CONFIG_EXAMPLE_ONEWIRE_MAX_DS18B20

static const char *TAG = "Verak";

static void wifi_event_handler(void* args, esp_event_base_t event_base, int32_t event_id, void* event_data){
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START){
        esp_wifi_connect();
    }else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED){
        ESP_LOGD(TAG, "Disconnected, retyring...\n");
    }else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP){
        ESP_LOGI(TAG, "Connected!\n");
    }
}

void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    // initialize bus and set config values
    onewire_bus_handle_t bus = NULL;
    onewire_bus_config_t bus_config = {
        // GPIO to use
        .bus_gpio_num = ONEWIRE_BUS_GPIO,
        .flags = {
            // If internal pull up is used or not
            .en_pull_up = ONEWIRE_ENABLE_INTERNAL_PULLUP
        }
    };

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_config);

    wifi_config_t wifi_config_sta = {
        .sta = {
            .ssid = SSID,
            .password = PASS
        }
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config_sta);

    esp_wifi_start();

// Define and configure backend to decide how timing is generated
#if CONFIG_EXAMPLE_ONEWIRE_BACKEND_RMT
    // RMT config
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10,
    };
    // Initialize bus and set it to &bus with RMT config
    ESP_ERROR_CHECK(onewire_new_bus_rmt(&bus_config, &rmt_config, &bus));

#elif CONFIG_EXAMPLE_ONEWIRE_BACKEND_UART
    // UART config
    onewire_bus_uart_config_t uart_config = {
        // Set the UART port number
        .uart_port_num = ONEWIRE_UART_PORT_NUM,
    };
    // Initiaize bus and set it to &bus using UART config
    ESP_ERROR_CHECK(onewire_new_bus_uart(&bus_config, &uart_config, &bus));

#else
#error "No 1 wire backend selected in menu config"
#endif

    // Initalize starting device number
    int ds18b20_device_num = 0;
    // Inizialize array of ds18b20 devices with length of ONEWIRE_MAX_DS18B20 set in the config
    ds18b20_device_handle_t ds18b20s[ONEWIRE_MAX_DS18B20];
    // Initialize interator
    onewire_device_iter_handle_t iter = NULL;
    // Initialize sensor/device
    onewire_device_t next_onewire_device;
    // Initialize search result (when searching for devices)
    esp_err_t search_result = ESP_OK;

    // create an interator on the buss and set it to &iter
    ESP_ERROR_CHECK(onewire_new_device_iter(bus, &iter));
    ESP_LOGI(TAG, "Device iterator cerated, start searching...");

    do{
        // Use the onewire iterator to search for the next device address and outpul the device handle to the var
        search_result = onewire_device_iter_get_next(iter, &next_onewire_device);

        // If device is found
        if(search_result == ESP_OK){
            // Initialize a sensor config (still not sure what to put here)
            ds18b20_config_t ds_config = {};
            // Initialize sensor memory address
            onewire_device_address_t address;

            // If it can get a a device from enumeration, set it with config to the device array
            if(ds18b20_new_device_from_enumeration(&next_onewire_device, &ds_config, &ds18b20s[ds18b20_device_num]) == ESP_OK){
                // Get the device address and set it to &address
                ds18b20_get_device_address(ds18b20s[ds18b20_device_num], &address);
                ESP_LOGI(TAG, "Found a DS18B20[%d], address: %016llX", ds18b20_device_num, address);

                // Iterate the counter
                ds18b20_device_num++;
                // Check if the counter is less than the max device amount
                if(ds18b20_device_num >= ONEWIRE_MAX_DS18B20){
                    ESP_LOGI(TAG, "Max DS18B20 number reached, stop searching...");
                    break;
                }
            }else{
                ESP_LOGI(TAG, "Found an unknown device, address: %016llX", next_onewire_device.address);
            }
        }
        // Do this all while search_result is ESP_OK
    }while(search_result != ESP_ERR_NOT_FOUND);

    // Clean up the iterator
    ESP_ERROR_CHECK(onewire_del_device_iter(iter));
    ESP_LOGI(TAG, "Searching done, %d DS18B20 device(s) found", ds18b20_device_num);

    // initialize temperature variable
    float temperature;

    // Main super loop
    while(1){
        // Delay one second
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Convert temps for all sensors on bus
        ESP_ERROR_CHECK(ds18b20_trigger_temperature_conversion_for_all(bus));

        // for each sensor, get the timeperature and log it
        for (int i = 0; i < ds18b20_device_num; i++){
            ESP_ERROR_CHECK(ds18b20_get_temperature(ds18b20s[i], &temperature));
            ESP_LOGI(TAG, "temperature read from DS18B20[%d]: %.2f°C", i, temperature);
        }
    }
}
