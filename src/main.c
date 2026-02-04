#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "BLE_SCANNER";

#define NVS_NAMESPACE "ble_alarm"
#define NVS_REGISTERED_KEY "reg_devices"

#define MAX_DEVICES 50
#define MAX_REGISTERED 5
#define UART_NUM UART_NUM_0
#define BUF_SIZE 256

// GPIO Configuration - TODO: Adjust pins based on your hardware
#define GPIO_PRESENCE_PIN  GPIO_NUM_34  // Cable presence pin (12V->0V when disconnected)
#define GPIO_CHARGER_RELAY GPIO_NUM_26  // Charger 220V AC relay control
#define GPIO_ALARM_RELAY   GPIO_NUM_33  // Alarm relay control
#define RGB_LED_RED_PIN    GPIO_NUM_25  // RGB LED Red channel
#define RGB_LED_GREEN_PIN  GPIO_NUM_27  // RGB LED Green channel
#define RGB_LED_BLUE_PIN   GPIO_NUM_32  // RGB LED Blue channel

// Timing constants
#define TAG_PRESENT_THRESHOLD_MS     60000              // Tag considered present if seen in last 60s
#define TAG_RECENT_THRESHOLD_MS      (10 * 60 * 1000)  // Tag recently present if seen in last 10 min
#define TAG_ONLINE_THRESHOLD_MS      5000               // Tag considered online if seen in last 5s

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

// Registered devices (FIFO, max 5) - store MAC addresses
typedef struct {
    uint8_t addr[6];
} registered_device_t;

static registered_device_t registered_macs[MAX_REGISTERED];
static int registered_count = 0;

// Hardware status
static bool cable_connected = false;
static bool charger_relay_state = false;
static bool alarm_relay_state = false;
static int64_t last_tag_seen_time = 0;  // Last time any registered tag was seen

// System states and RGB LED colors
typedef enum {
    STATE_CHARGING_TAG_PRESENT,    // Charging + tag present (<60s) - GREEN LED
    STATE_CHARGING_TAG_RECENT,     // Charging + tag recent (60s-10min) - ORANGE LED
    STATE_CHARGING_ALARM_ARMED,    // Charging + no tag (>10min) - RED LED
    STATE_NOT_CHARGING             // Cable disconnected - BLUE LED
} system_state_t;

typedef enum {
    LED_OFF,
    LED_GREEN,    // Charging, tag present
    LED_ORANGE,   // Charging, tag recent
    LED_RED,      // Charging, alarm armed
    LED_BLUE      // Not charging
} led_color_t;

static system_state_t current_state = STATE_CHARGING_TAG_PRESENT;

// Save registered devices to NVS
static void save_registered_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        return;
    }

    // Save count
    err = nvs_set_i32(nvs_handle, "reg_count", registered_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving count: %s", esp_err_to_name(err));
    }

    // Save array of MAC addresses
    if (registered_count > 0) {
        err = nvs_set_blob(nvs_handle, NVS_REGISTERED_KEY, registered_macs,
                          registered_count * sizeof(registered_device_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error saving MACs: %s", esp_err_to_name(err));
        }
    }

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Saved %d registered devices to NVS", registered_count);
    for (int i = 0; i < registered_count; i++) {
        ESP_LOGI(TAG, "  - MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 registered_macs[i].addr[0], registered_macs[i].addr[1],
                 registered_macs[i].addr[2], registered_macs[i].addr[3],
                 registered_macs[i].addr[4], registered_macs[i].addr[5]);
    }
}

// Load registered devices from NVS
static void load_registered_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved devices found (first boot)");
        return;
    }

    // Load count
    int32_t count = 0;
    err = nvs_get_i32(nvs_handle, "reg_count", &count);
    if (err != ESP_OK || count <= 0) {
        nvs_close(nvs_handle);
        return;
    }

    // Load array of MAC addresses
    size_t required_size = count * sizeof(registered_device_t);
    err = nvs_get_blob(nvs_handle, NVS_REGISTERED_KEY, registered_macs, &required_size);
    if (err == ESP_OK) {
        registered_count = count;
        ESP_LOGI(TAG, "Loaded %d registered devices from NVS", registered_count);
        for (int i = 0; i < registered_count; i++) {
            ESP_LOGI(TAG, "  - MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     registered_macs[i].addr[0], registered_macs[i].addr[1],
                     registered_macs[i].addr[2], registered_macs[i].addr[3],
                     registered_macs[i].addr[4], registered_macs[i].addr[5]);
        }
    } else {
        ESP_LOGE(TAG, "Error loading MACs: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
}

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

// Check if device MAC is registered
static bool is_registered_mac(uint8_t *addr) {
    for (int i = 0; i < registered_count; i++) {
        if (memcmp(registered_macs[i].addr, addr, 6) == 0) {
            return true;
        }
    }
    return false;
}

// Check if device ID is registered (for display purposes)
static bool is_registered(int id) {
    // Find device by ID and check its MAC
    for (int i = 0; i < device_count; i++) {
        if (devices[i].id == id) {
            return is_registered_mac(devices[i].addr);
        }
    }
    return false;
}

// Register a device by ID (FIFO)
static void register_device(int id) {
    // Find device by ID
    int device_idx = -1;
    for (int i = 0; i < device_count; i++) {
        if (devices[i].id == id) {
            device_idx = i;
            break;
        }
    }

    if (device_idx == -1) {
        printf("Device ID %d not found.\n", id);
        return;
    }

    // Check if already registered
    if (is_registered_mac(devices[device_idx].addr)) {
        printf("Device ID %d is already registered.\n", id);
        return;
    }

    // FIFO: if full, remove oldest
    if (registered_count >= MAX_REGISTERED) {
        // Shift left
        for (int i = 0; i < MAX_REGISTERED - 1; i++) {
            memcpy(registered_macs[i].addr, registered_macs[i + 1].addr, 6);
        }
        memcpy(registered_macs[MAX_REGISTERED - 1].addr, devices[device_idx].addr, 6);
        printf("Registered device ID %d (removed oldest)\n", id);
    } else {
        memcpy(registered_macs[registered_count].addr, devices[device_idx].addr, 6);
        registered_count++;
        printf("Registered device ID %d (%d/%d)\n", id, registered_count, MAX_REGISTERED);
    }

    // Save to NVS
    save_registered_to_nvs();
}

// Deregister a device by ID
static void deregister_device(int id) {
    // Find device by ID
    int device_idx = -1;
    for (int i = 0; i < device_count; i++) {
        if (devices[i].id == id) {
            device_idx = i;
            break;
        }
    }

    if (device_idx == -1) {
        printf("Device ID %d not found.\n", id);
        return;
    }

    // Find in registered list by MAC
    for (int i = 0; i < registered_count; i++) {
        if (memcmp(registered_macs[i].addr, devices[device_idx].addr, 6) == 0) {
            // Shift remaining items
            for (int j = i; j < registered_count - 1; j++) {
                memcpy(registered_macs[j].addr, registered_macs[j + 1].addr, 6);
            }
            registered_count--;
            printf("Deregistered device ID %d\n", id);

            // Save to NVS
            save_registered_to_nvs();
            return;
        }
    }
    printf("Device ID %d is not registered.\n", id);
}


// Initialize GPIO and RGB LED
static void init_gpio(void) {
    // Configure cable presence input (GPIO34 is input-only, no internal pull resistors)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_PRESENCE_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,  // Input-only GPIO, use external pull-down
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Configure charger relay output
    io_conf.pin_bit_mask = (1ULL << GPIO_CHARGER_RELAY);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(GPIO_CHARGER_RELAY, 1);  // Charger ON initially
    charger_relay_state = true;

    // Configure alarm relay output
    io_conf.pin_bit_mask = (1ULL << GPIO_ALARM_RELAY);
    gpio_config(&io_conf);
    gpio_set_level(GPIO_ALARM_RELAY, 0);  // Alarm OFF initially
    alarm_relay_state = false;

    // Configure RGB LED pins
    io_conf.pin_bit_mask = (1ULL << RGB_LED_RED_PIN) |
                           (1ULL << RGB_LED_GREEN_PIN) |
                           (1ULL << RGB_LED_BLUE_PIN);
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "GPIO initialized - Presence: %d, Charger: %d, Alarm: %d, RGB: R%d G%d B%d",
             GPIO_PRESENCE_PIN, GPIO_CHARGER_RELAY, GPIO_ALARM_RELAY,
             RGB_LED_RED_PIN, RGB_LED_GREEN_PIN, RGB_LED_BLUE_PIN);
}

// Set RGB LED color
static void set_led_color(led_color_t color) {
    switch (color) {
        case LED_OFF:
            gpio_set_level(RGB_LED_RED_PIN, 0);
            gpio_set_level(RGB_LED_GREEN_PIN, 0);
            gpio_set_level(RGB_LED_BLUE_PIN, 0);
            break;
        case LED_GREEN:
            gpio_set_level(RGB_LED_RED_PIN, 0);
            gpio_set_level(RGB_LED_GREEN_PIN, 1);
            gpio_set_level(RGB_LED_BLUE_PIN, 0);
            break;
        case LED_ORANGE:
            gpio_set_level(RGB_LED_RED_PIN, 1);
            gpio_set_level(RGB_LED_GREEN_PIN, 1);  // Red + Green = Orange
            gpio_set_level(RGB_LED_BLUE_PIN, 0);
            break;
        case LED_RED:
            gpio_set_level(RGB_LED_RED_PIN, 1);
            gpio_set_level(RGB_LED_GREEN_PIN, 0);
            gpio_set_level(RGB_LED_BLUE_PIN, 0);
            break;
        case LED_BLUE:
            gpio_set_level(RGB_LED_RED_PIN, 0);
            gpio_set_level(RGB_LED_GREEN_PIN, 0);
            gpio_set_level(RGB_LED_BLUE_PIN, 1);
            break;
    }
}

// Check if any registered tag is currently online
static bool is_any_tag_online(void) {
    if (registered_count == 0) return false;

    int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < registered_count; i++) {
        for (int j = 0; j < device_count; j++) {
            if (memcmp(devices[j].addr, registered_macs[i].addr, 6) == 0) {
                int64_t elapsed = now - devices[j].last_seen;
                if (elapsed < TAG_ONLINE_THRESHOLD_MS) {
                    last_tag_seen_time = now;  // Update last seen time
                    return true;
                }
                break;
            }
        }
    }
    return false;
}

// Update system state and control relays + LED
static void update_system_state(void) {
    int64_t now = esp_timer_get_time() / 1000;
    int64_t time_since_tag = now - last_tag_seen_time;

    system_state_t new_state;

    if (cable_connected) {
        // Cable connected - determine state based on tag presence timing
        if (time_since_tag < TAG_PRESENT_THRESHOLD_MS) {
            // State 1: Tag present (seen in last 60 seconds) - GREEN
            new_state = STATE_CHARGING_TAG_PRESENT;
        } else if (time_since_tag < TAG_RECENT_THRESHOLD_MS) {
            // State 2: Tag recent (60s to 10min) - ORANGE
            new_state = STATE_CHARGING_TAG_RECENT;
        } else {
            // State 3: Tag not present (>10min - alarm armed) - RED
            new_state = STATE_CHARGING_ALARM_ARMED;
        }
    } else {
        // State 4: Cable disconnected - BLUE
        new_state = STATE_NOT_CHARGING;
    }

    // State changed - log it
    if (new_state != current_state) {
        const char *state_names[] = {
            "CHARGING_TAG_PRESENT",
            "CHARGING_TAG_RECENT",
            "CHARGING_ALARM_ARMED",
            "NOT_CHARGING"
        };
        ESP_LOGI(TAG, "State change: %s -> %s",
                 state_names[current_state], state_names[new_state]);
        current_state = new_state;
    }

    // Control hardware based on state
    bool new_charger_state = false;
    bool new_alarm_state = false;
    led_color_t new_led_color = LED_OFF;

    switch (current_state) {
        case STATE_CHARGING_TAG_PRESENT:
            // Charger ON, alarm OFF, GREEN LED
            new_charger_state = true;
            new_alarm_state = false;
            new_led_color = LED_GREEN;
            break;

        case STATE_CHARGING_TAG_RECENT:
            // Charger ON, alarm OFF, ORANGE LED
            new_charger_state = true;
            new_alarm_state = false;
            new_led_color = LED_ORANGE;
            break;

        case STATE_CHARGING_ALARM_ARMED:
            // Charger ON, alarm ON (armed), RED LED
            new_charger_state = true;
            new_alarm_state = true;
            new_led_color = LED_RED;
            break;

        case STATE_NOT_CHARGING:
            // Charger OFF (cable disconnected), alarm OFF, BLUE LED
            new_charger_state = false;
            new_alarm_state = false;
            new_led_color = LED_BLUE;
            break;
    }

    // Update charger relay if changed
    if (new_charger_state != charger_relay_state) {
        charger_relay_state = new_charger_state;
        gpio_set_level(GPIO_CHARGER_RELAY, charger_relay_state ? 1 : 0);
        ESP_LOGI(TAG, "Charger relay: %s", charger_relay_state ? "ON" : "OFF");
    }

    // Update alarm relay if changed
    if (new_alarm_state != alarm_relay_state) {
        alarm_relay_state = new_alarm_state;
        gpio_set_level(GPIO_ALARM_RELAY, alarm_relay_state ? 1 : 0);
        ESP_LOGI(TAG, "Alarm relay: %s", alarm_relay_state ? "ON" : "OFF");
    }

    // Update LED
    set_led_color(new_led_color);
}

// Monitor cable presence and update system
static void system_monitor_task(void *param) {
    while (1) {
        // Read cable presence pin (HIGH = cable connected, LOW = disconnected)
        bool current_cable_state = gpio_get_level(GPIO_PRESENCE_PIN);

        if (current_cable_state != cable_connected) {
            cable_connected = current_cable_state;
            ESP_LOGI(TAG, "Cable %s", cable_connected ? "CONNECTED" : "DISCONNECTED");
        }

        // Check if any registered tag is online (updates last_tag_seen_time)
        is_any_tag_online();

        // Update system state and hardware
        update_system_state();

        vTaskDelay(pdMS_TO_TICKS(100));  // Check every 100ms
    }
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

        // Display system status
        const char *state_names[] = {
            "CHARGING_TAG_PRESENT",
            "CHARGING_TAG_RECENT",
            "CHARGING_ALARM_ARMED",
            "NOT_CHARGING"
        };
        const char *led_colors[] = {"GREEN", "ORANGE", "RED", "BLUE"};
        printf("\n*** System Status ***\n");
        printf("State: %s (%s LED)\n", state_names[current_state], led_colors[current_state]);
        printf("Cable: %s | Charger: %s | Alarm: %s\n",
               cable_connected ? "CONNECTED" : "DISCONNECTED",
               charger_relay_state ? "ON" : "OFF",
               alarm_relay_state ? "ARMED" : "OFF");

        int64_t time_since_tag = (now - last_tag_seen_time) / 1000;  // seconds
        if (last_tag_seen_time > 0) {
            printf("Last tag seen: %lld seconds ago\n", time_since_tag);
            if (time_since_tag < 60) {
                printf("  Status: Tag PRESENT\n");
            } else if (time_since_tag < 600) {
                printf("  Status: Tag RECENT (%d min left)\n", 10 - (int)(time_since_tag / 60));
            } else {
                printf("  Status: Alarm ARMED (no tag for >10min)\n");
            }
        } else {
            printf("No registered tag seen yet\n");
        }
        printf("*********************\n");
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

    // Load registered devices from NVS
    load_registered_from_nvs();

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


    // Initialize GPIO and start monitoring
    init_gpio();
    set_led_color(LED_GREEN);  // Start in charging state
    cable_connected = gpio_get_level(GPIO_PRESENCE_PIN);  // Read initial state
    ESP_LOGI(TAG, "Initial cable state: %s", cable_connected ? "CONNECTED" : "DISCONNECTED");

    // Start tasks
    xTaskCreate(print_devices_task, "print_devices", 4096, NULL, 5, NULL);
    xTaskCreate(console_input_task, "console_input", 4096, NULL, 5, NULL);
    xTaskCreate(system_monitor_task, "system_monitor", 2048, NULL, 5, NULL);

    // Main loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
