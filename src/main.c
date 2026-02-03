#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "BLE_SCANNER";

#define MAX_DEVICES 50
#define MAX_REGISTERED 5
#define UART_NUM UART_NUM_0
#define BUF_SIZE 256

typedef struct {
    int id;
    uint8_t addr[6];
    int8_t rssi;
    int64_t last_seen;
    bool active;
    char name[32];
    uint8_t addr_type;
    uint16_t appearance;
    bool has_name;
} ble_device_t;

static ble_device_t devices[MAX_DEVICES];
static int device_count = 0;
static int next_id = 1;  // Start from 1

// Registered devices (FIFO, max 5)
static int registered_ids[MAX_REGISTERED];
static int registered_count = 0;

// Find or add device in the list
static int find_or_add_device(uint8_t *addr) {
    // First, check if device already exists
    for (int i = 0; i < device_count; i++) {
        if (memcmp(devices[i].addr, addr, 6) == 0) {
            return i;
        }
    }

    // Add new device if space available
    if (device_count < MAX_DEVICES) {
        devices[device_count].id = next_id++;
        memcpy(devices[device_count].addr, addr, 6);
        devices[device_count].active = true;
        devices[device_count].has_name = false;
        devices[device_count].name[0] = '\0';
        devices[device_count].appearance = 0;
        devices[device_count].addr_type = 0;
        return device_count++;
    }

    return -1;
}

// Check if device is registered
static bool is_registered(int id) {
    for (int i = 0; i < registered_count; i++) {
        if (registered_ids[i] == id) {
            return true;
        }
    }
    return false;
}

// Register a device by ID (FIFO)
static void register_device(int id) {
    // Check if already registered
    if (is_registered(id)) {
        printf("Device ID %d is already registered.\n", id);
        return;
    }

    // Check if ID exists
    bool found = false;
    for (int i = 0; i < device_count; i++) {
        if (devices[i].id == id) {
            found = true;
            break;
        }
    }

    if (!found) {
        printf("Device ID %d not found.\n", id);
        return;
    }

    // FIFO: if full, remove oldest
    if (registered_count >= MAX_REGISTERED) {
        // Shift left
        for (int i = 0; i < MAX_REGISTERED - 1; i++) {
            registered_ids[i] = registered_ids[i + 1];
        }
        registered_ids[MAX_REGISTERED - 1] = id;
        printf("Registered device ID %d (removed oldest)\n", id);
    } else {
        registered_ids[registered_count++] = id;
        printf("Registered device ID %d (%d/%d)\n", id, registered_count, MAX_REGISTERED);
    }
}

// Deregister a device by ID
static void deregister_device(int id) {
    for (int i = 0; i < registered_count; i++) {
        if (registered_ids[i] == id) {
            // Shift remaining items
            for (int j = i; j < registered_count - 1; j++) {
                registered_ids[j] = registered_ids[j + 1];
            }
            registered_count--;
            printf("Deregistered device ID %d\n", id);
            return;
        }
    }
    printf("Device ID %d is not registered.\n", id);
}

// GAP callback
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;

            if (scan_result->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                int idx = find_or_add_device(scan_result->scan_rst.bda);
                if (idx >= 0) {
                    devices[idx].rssi = scan_result->scan_rst.rssi;
                    devices[idx].last_seen = esp_timer_get_time() / 1000; // Convert to ms
                    devices[idx].addr_type = scan_result->scan_rst.ble_addr_type;

                    // Parse advertisement data for device name and other info
                    uint8_t *adv_data = scan_result->scan_rst.ble_adv;
                    uint8_t adv_data_len = scan_result->scan_rst.adv_data_len;

                    for (int i = 0; i < adv_data_len;) {
                        uint8_t len = adv_data[i];
                        if (len == 0) break;

                        uint8_t type = adv_data[i + 1];

                        // Complete or shortened local name
                        if (type == 0x09 || type == 0x08) {
                            int name_len = len - 1;
                            if (name_len > 31) name_len = 31;
                            memcpy(devices[idx].name, &adv_data[i + 2], name_len);
                            devices[idx].name[name_len] = '\0';
                            devices[idx].has_name = true;
                        }
                        // Appearance
                        else if (type == 0x19 && len >= 3) {
                            devices[idx].appearance = adv_data[i + 2] | (adv_data[i + 3] << 8);
                        }

                        i += len + 1;
                    }
                }
            }
            break;
        }
        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "BLE scan started");
            }
            break;
        default:
            break;
    }
}

// Task to print devices every 5 seconds
static void print_devices_task(void *param) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        int64_t now = esp_timer_get_time() / 1000;

        // Create sorted indices by RSSI (strongest first)
        int sorted_indices[MAX_DEVICES];
        for (int i = 0; i < device_count; i++) {
            sorted_indices[i] = i;
        }

        // Simple bubble sort by RSSI (descending)
        for (int i = 0; i < device_count - 1; i++) {
            for (int j = 0; j < device_count - i - 1; j++) {
                if (devices[sorted_indices[j]].rssi < devices[sorted_indices[j + 1]].rssi) {
                    int temp = sorted_indices[j];
                    sorted_indices[j] = sorted_indices[j + 1];
                    sorted_indices[j + 1] = temp;
                }
            }
        }

        printf("\n========== BLE Devices (Time: %lld s) ==========\n", (now + 500) / 1000);
        printf("%-3s %-20s %-6s %-8s %-20s %-6s\n", "ID", "MAC Address", "RSSI", "Type", "Name", "Status");
        printf("-------------------------------------------------------------------------\n");

        int active_count = 0;
        int displayed = 0;

        // First, display registered devices
        for (int i = 0; i < device_count && displayed < 20; i++) {
            int idx = sorted_indices[i];
            if (!is_registered(devices[idx].id)) continue;

            int64_t elapsed = now - devices[idx].last_seen;
            int elapsed_sec = (int)((elapsed + 500) / 1000);

            bool is_offline = (elapsed > 30000);
            bool is_recent_offline = (elapsed > 5000 && elapsed <= 30000);

            if (is_offline && devices[idx].active) {
                devices[idx].active = false;
            }

            const char *addr_type_str = "Unknown";
            if (devices[idx].addr_type == 0) addr_type_str = "Public";
            else if (devices[idx].addr_type == 1) addr_type_str = "Random";
            else if (devices[idx].addr_type == 2) addr_type_str = "RPA Pub";
            else if (devices[idx].addr_type == 3) addr_type_str = "RPA Rnd";

            // Color coding for registered devices
            const char *color_start = "";
            const char *color_end = "\033[0m";
            const char *status = "";
            if (is_offline) {
                color_start = "\033[0;31m";  // Red
                status = "REG-OFF";
            } else if (is_recent_offline) {
                color_start = "\033[0;33m";  // Orange/Yellow
                status = "REG-WARN";
            } else {
                color_start = "\033[0;32m";  // Green
                status = "REG-OK";
                active_count++;
            }

            printf("%s%3d %02X:%02X:%02X:%02X:%02X:%02X  %4d  %-8s ",
                   color_start,
                   devices[idx].id,
                   devices[idx].addr[0], devices[idx].addr[1], devices[idx].addr[2],
                   devices[idx].addr[3], devices[idx].addr[4], devices[idx].addr[5],
                   devices[idx].rssi, addr_type_str);

            if (devices[idx].has_name && strlen(devices[idx].name) > 0) {
                printf("%-20s", devices[idx].name);
            } else {
                printf("%-20s", "(no name)");
            }

            printf(" %-6s [%ds ago]", status, elapsed_sec);

            if (devices[idx].appearance > 0) {
                printf(" App:0x%04X", devices[idx].appearance);
            }

            printf("%s\n", color_end);
            displayed++;
        }

        // Then display non-registered devices
        for (int i = 0; i < device_count && displayed < 20; i++) {
            int idx = sorted_indices[i];
            if (is_registered(devices[idx].id)) continue;

            int64_t elapsed = now - devices[idx].last_seen;
            int elapsed_sec = (int)((elapsed + 500) / 1000);

            bool is_offline = (elapsed > 30000);
            if (is_offline && devices[idx].active) {
                devices[idx].active = false;
            }

            if (devices[idx].active) {
                const char *addr_type_str = "Unknown";
                if (devices[idx].addr_type == 0) addr_type_str = "Public";
                else if (devices[idx].addr_type == 1) addr_type_str = "Random";
                else if (devices[idx].addr_type == 2) addr_type_str = "RPA Pub";
                else if (devices[idx].addr_type == 3) addr_type_str = "RPA Rnd";

                printf("%3d %02X:%02X:%02X:%02X:%02X:%02X  %4d  %-8s ",
                       devices[idx].id,
                       devices[idx].addr[0], devices[idx].addr[1], devices[idx].addr[2],
                       devices[idx].addr[3], devices[idx].addr[4], devices[idx].addr[5],
                       devices[idx].rssi, addr_type_str);

                if (devices[idx].has_name && strlen(devices[idx].name) > 0) {
                    printf("%-20s", devices[idx].name);
                } else {
                    printf("%-20s", "(no name)");
                }

                printf(" %-6s [%ds ago]", "-", elapsed_sec);

                if (devices[idx].appearance > 0) {
                    printf(" App:0x%04X", devices[idx].appearance);
                }

                printf("\n");
                displayed++;
            }
        }

        printf("-------------------------------------------------------------------------\n");
        printf("Registered devices online: %d/%d | Type ID+ENTER to register/deregister\n", active_count, registered_count);
        printf("=========================================================================\n\n");
    }
}

// Console input task to register/deregister devices
static void console_input_task(void *param) {
    uint8_t data[BUF_SIZE];
    int pos = 0;

    printf("\n=== Console Commands ===\n");
    printf("Type device ID (1-99) + ENTER to register/deregister\n");
    printf("========================\n\n");

    while (1) {
        int len = uart_read_bytes(UART_NUM, &data[pos], 1, pdMS_TO_TICKS(100));

        if (len > 0) {
            if (data[pos] == '\n' || data[pos] == '\r') {
                if (pos > 0) {
                    data[pos] = '\0';
                    int id = atoi((char*)data);

                    if (id >= 1 && id <= 99) {
                        if (is_registered(id)) {
                            deregister_device(id);
                        } else {
                            register_device(id);
                        }
                    } else if (id != 0) {
                        printf("Invalid ID. Enter 1-99.\n");
                    }
                    pos = 0;
                }
            } else if (pos < BUF_SIZE - 1) {
                pos++;
            }
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "BLE Scanner for Teltonika Tags");
    ESP_LOGI(TAG, "========================================");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Release memory for classic BT (we only need BLE)
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    // Initialize BT controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return;
    }

    // Register GAP callback
    ret = esp_ble_gap_register_callback(esp_gap_cb);
    if (ret) {
        ESP_LOGE(TAG, "GAP register failed: %s", esp_err_to_name(ret));
        return;
    }

    // Configure scan parameters
    static esp_ble_scan_params_t ble_scan_params = {
        .scan_type              = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval          = 0x50,  // 50ms
        .scan_window            = 0x30,  // 30ms
        .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
    };

    esp_ble_gap_set_scan_params(&ble_scan_params);

    // Start scanning (0 = continuous)
    esp_ble_gap_start_scanning(0);

    ESP_LOGI(TAG, "BLE scanning started - looking for tags...");

    // Configure UART for console input
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_NUM, &uart_config);
    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);

    // Start tasks
    xTaskCreate(print_devices_task, "print_devices", 4096, NULL, 5, NULL);
    xTaskCreate(console_input_task, "console_input", 4096, NULL, 5, NULL);

    // Main loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
