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
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/dns.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "BLE_SCANNER";

// Relay polarity configuration
#ifdef CONFIG_HESTIA32_RELAY_ACTIVE_LOW
    #define RELAY_ON  0
    #define RELAY_OFF 1
#else
    #define RELAY_ON  1
    #define RELAY_OFF 0
#endif

// WiFi AP Configuration
#define WIFI_AP_SSID "TAG-SCANNER"
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_CONN 4
#define WIFI_AP_NO_CLIENT_TIMEOUT_MS (60 * 1000)  // 1 minute if no client connects
#define WIFI_AP_CLIENT_CONNECTED_TIMEOUT_MS (2 * 60 * 1000)  // 2 minutes after client connects

static httpd_handle_t server = NULL;
static int64_t ap_start_time = 0;
static int64_t ap_last_client_time = 0;
static bool ap_active = false;
static bool ap_has_clients = false;

// BLE Scan Phase Configuration
#define BLE_SCAN_DURATION_MS 30000  // 30 seconds

static bool ble_scan_complete = false;
static bool wifi_ap_started = false;
static esp_netif_t *wifi_ap_netif = NULL;  // Track netif for proper cleanup
static int dns_server_socket = -1;  // DNS server socket
static TaskHandle_t dns_task_handle = NULL;  // DNS task handle

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
#define INTERNAL_LED_PIN   GPIO_NUM_2   // Internal LED for AP mode indication

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
    char complete_name[32];
    uint8_t addr_type;
    uint16_t appearance;
    bool has_name;
    bool has_complete_name;
    uint16_t manufacturer_id;
    bool has_manufacturer;
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
        devices[device_count].has_complete_name = false;
        devices[device_count].has_manufacturer = false;
        devices[device_count].name[0] = '\0';
        devices[device_count].complete_name[0] = '\0';
        devices[device_count].appearance = 0;
        devices[device_count].addr_type = 0;
        devices[device_count].manufacturer_id = 0;
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

// Get manufacturer name from company ID
static const char* get_manufacturer_name(uint16_t company_id) {
    switch(company_id) {
        case 0x089A: return "Teltonika";
        case 0x004C: return "Apple Inc.";
        case 0x0006: return "Microsoft";
        case 0x0075: return "Samsung Electronics";
        case 0x00E0: return "Google";
        case 0x0087: return "Garmin";
        case 0x0157: return "Huawei";
        case 0x0171: return "Xiaomi";
        default: return "Unknown";
    }
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
    gpio_set_level(GPIO_CHARGER_RELAY, RELAY_ON);  // Charger ON initially
    charger_relay_state = true;

    // Configure alarm relay output
    io_conf.pin_bit_mask = (1ULL << GPIO_ALARM_RELAY);
    gpio_config(&io_conf);
    gpio_set_level(GPIO_ALARM_RELAY, RELAY_OFF);  // Alarm OFF initially
    alarm_relay_state = false;

    // Configure RGB LED pins
    io_conf.pin_bit_mask = (1ULL << RGB_LED_RED_PIN) |
                           (1ULL << RGB_LED_GREEN_PIN) |
                           (1ULL << RGB_LED_BLUE_PIN) |
                           (1ULL << INTERNAL_LED_PIN);
    gpio_config(&io_conf);
    gpio_set_level(INTERNAL_LED_PIN, 0);  // Start off

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
        gpio_set_level(GPIO_CHARGER_RELAY, charger_relay_state ? RELAY_ON : RELAY_OFF);
        ESP_LOGI(TAG, "Charger relay: %s", charger_relay_state ? "ON" : "OFF");
    }

    // Update alarm relay if changed
    if (new_alarm_state != alarm_relay_state) {
        alarm_relay_state = new_alarm_state;
        gpio_set_level(GPIO_ALARM_RELAY, alarm_relay_state ? RELAY_ON : RELAY_OFF);
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

// ===== BLE Cleanup Function =====

// Forward declarations for WiFi functions
static void wifi_init_softap(void);
static httpd_handle_t start_webserver(void);
static void ap_monitor_task(void *param);

// DNS server task - responds to all DNS queries with ESP32's IP
static void dns_server_task(void *param) {
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(53);  // DNS port

    dns_server_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_server_socket < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    if (bind(dns_server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket bind failed");
        close(dns_server_socket);
        dns_server_socket = -1;
        vTaskDelete(NULL);
        return;
    }

    // Set socket to non-blocking mode
    int flags = fcntl(dns_server_socket, F_GETFL, 0);
    fcntl(dns_server_socket, F_SETFL, flags | O_NONBLOCK);

    ESP_LOGI(TAG, "DNS server started on port 53");

    uint8_t rx_buffer[512];
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    while (ap_active) {
        int len = recvfrom(dns_server_socket, rx_buffer, sizeof(rx_buffer) - 1, 0,
                          (struct sockaddr *)&client_addr, &client_addr_len);

        if (len > 0) {
            // Build DNS response - return our IP (192.168.4.1) for all queries
            uint8_t response[512];
            memcpy(response, rx_buffer, len);  // Copy query

            // Set response flags (standard query response, no error)
            response[2] = 0x81;  // Response, recursion available
            response[3] = 0x80;  // No error

            // Answer count = 1
            response[6] = 0x00;
            response[7] = 0x01;

            // Build answer section at end of query
            int answer_offset = len;

            // Name pointer to query name (compression)
            response[answer_offset++] = 0xC0;
            response[answer_offset++] = 0x0C;

            // Type A (IPv4 address)
            response[answer_offset++] = 0x00;
            response[answer_offset++] = 0x01;

            // Class IN
            response[answer_offset++] = 0x00;
            response[answer_offset++] = 0x01;

            // TTL (60 seconds)
            response[answer_offset++] = 0x00;
            response[answer_offset++] = 0x00;
            response[answer_offset++] = 0x00;
            response[answer_offset++] = 0x3C;

            // Data length (4 bytes for IPv4)
            response[answer_offset++] = 0x00;
            response[answer_offset++] = 0x04;

            // IP address 192.168.4.1
            response[answer_offset++] = 192;
            response[answer_offset++] = 168;
            response[answer_offset++] = 4;
            response[answer_offset++] = 1;

            sendto(dns_server_socket, response, answer_offset, 0,
                  (struct sockaddr *)&client_addr, client_addr_len);
        } else {
            // No data, sleep briefly to avoid busy-waiting
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    close(dns_server_socket);
    dns_server_socket = -1;
    ESP_LOGI(TAG, "DNS server stopped");
    vTaskDelete(NULL);
}

static void cleanup_ble_stack(void) {
    ESP_LOGI(TAG, "Stopping and deinitializing BLE stack...");

    // Stop scanning
    esp_ble_gap_stop_scanning();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Disable and deinitialize Bluedroid
    esp_err_t ret = esp_bluedroid_disable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Bluedroid disable failed: %s", esp_err_to_name(ret));
    }

    ret = esp_bluedroid_deinit();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Bluedroid deinit failed: %s", esp_err_to_name(ret));
    }

    // Disable and deinitialize BT controller
    ret = esp_bt_controller_disable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "BT controller disable failed: %s", esp_err_to_name(ret));
    }

    ret = esp_bt_controller_deinit();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "BT controller deinit failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "BLE stack fully deinitialized - IRAM freed");
}

// Timer callback to complete BLE scan and start WiFi
static void ble_scan_timer_callback(void* arg) {
    // Just set flag - can't do heavy operations in ISR context
    ble_scan_complete = true;
}

// ===== WiFi AP and HTTP Server Functions =====

// HTTP handler for common captive portal detection URLs
static esp_err_t captive_detect_handler(httpd_req_t *req) {
    // Redirect to main configuration page to trigger captive portal
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    const char* redirect = "<html><head><meta http-equiv='refresh' content='0;url=http://192.168.4.1/'></head><body>Redirecting...</body></html>";
    httpd_resp_send(req, redirect, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// HTTP handler for captive portal - redirect all unknown requests to root
static esp_err_t captive_portal_handler(httpd_req_t *req) {
    const char* redirect =
        "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='0;url=http://192.168.4.1/'>"
        "</head><body>Redirecting...</body></html>";
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, redirect, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// HTTP handler for root page (web interface)
static esp_err_t root_get_handler(httpd_req_t *req) {
    char *html_page = malloc(4096);
    if (!html_page) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    snprintf(html_page, 4096,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>BLE Tag Alarm</title><style>"
        "body{font-family:Arial,sans-serif;margin:20px;background:#f0f0f0}"
        ".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1)}"
        "h1{color:#333;text-align:center}h2{color:#555;border-bottom:2px solid #4CAF50;padding-bottom:5px}"
        ".device{background:#f9f9f9;padding:10px;margin:10px 0;border-radius:5px;border-left:4px solid #2196F3}"
        ".device.registered{border-left-color:#4CAF50}.device.online{background:#e8f5e9}"
        ".btn{padding:8px 15px;margin:5px;border:none;border-radius:4px;cursor:pointer;font-size:14px}"
        ".btn-register{background:#4CAF50;color:white}.btn-unregister{background:#f44336;color:white}"
        ".btn:hover{opacity:0.8}.info{color:#666;font-size:14px}.timeout{color:#FF9800;font-weight:bold;text-align:center;font-size:16px}"
        ".mac{font-family:monospace;color:#333}.rssi{color:#888;font-size:12px}"
        "</style></head><body><div class='container'>"
        "<h1>🔒 BLE Tag Alarm</h1>"
        "<p class='timeout'>AP will shutdown in: <span id='countdown'>--:--</span></p>"
        "<div style='text-align:center;margin:20px 0'>"
        "<button class='btn' style='background:#2196F3;color:white;padding:12px 30px;font-size:16px' onclick='saveAndExit()'>💾 Save & Exit AP Mode</button>"
        "</div>"
        "<h2>📡 Detected Devices</h2><div id='devices'>Loading...</div>"
        "<script>"
        "let apStartTime=Date.now();"
        "let clientConnectedTimeout=%d;"
        "let noClientTimeout=%d;"
        "let hasClient=false;"
        "function updateCountdown(){"
        "let elapsed=Date.now()-apStartTime;"
        "let remaining=hasClient?clientConnectedTimeout-elapsed:noClientTimeout-elapsed;"
        "if(remaining<=0){document.getElementById('countdown').textContent='SHUTTING DOWN';return;}"
        "let mins=Math.floor(remaining/60000);let secs=Math.floor((remaining%%60000)/1000);"
        "document.getElementById('countdown').textContent=mins+':'+(secs<10?'0':'')+secs;"
        "}"
        "setInterval(updateCountdown,1000);updateCountdown();"
        "async function fetchDevices(){"
        "try{let res=await fetch('/api/devices');let data=await res.json();"
        "let html='';data.devices.forEach(d=>{"
        "let onlineClass=d.online?' online':'';"
        "let regClass=d.registered?' registered':'';"
        "html+='<div class=\"device'+onlineClass+regClass+'\">';"
        "let displayName=(d.manufacturer&&d.manufacturer!=='Unknown'?d.manufacturer+' - ':'')+d.name;"
        "html+='<div><strong>'+displayName+'</strong></div>';"
        "html+='<div class=\"mac\">MAC: '+d.mac+'</div>';"
        "html+='<div class=\"rssi\">RSSI: '+d.rssi+' dBm | Last seen: '+d.last_seen_ago+'</div>';"
        "if(d.registered){"
        "html+='<button class=\"btn btn-unregister\" onclick=\"unregisterDevice('+d.id+')\">❌ Unregister</button>';"
        "}else{"
        "html+='<button class=\"btn btn-register\" onclick=\"registerDevice('+d.id+')\">✅ Register</button>';"
        "}"
        "html+='</div>';});"
        "document.getElementById('devices').innerHTML=html||'<p class=\"info\">No devices detected</p>';"
        "}catch(e){document.getElementById('devices').innerHTML='<p class=\"info\">Error loading devices</p>';}}"
        "async function registerDevice(id){"
        "try{await fetch('/api/register?id='+id,{method:'POST'});fetchDevices();}catch(e){alert('Failed to register');}}"
        "async function unregisterDevice(id){"
        "try{await fetch('/api/unregister?id='+id,{method:'POST'});fetchDevices();}catch(e){alert('Failed to unregister');}}"
        "async function saveAndExit(){"
        "if(confirm('Save configuration and exit AP mode?')){"
        "try{await fetch('/api/save',{method:'POST'});}catch(e){alert('Failed to save');}}}"
        "setInterval(fetchDevices,3000);fetchDevices();"
        "fetch('/api/devices').then(r=>r.json()).then(d=>{if(d.devices.length>0)hasClient=true;});"
        "</script></div></body></html>",
        WIFI_AP_CLIENT_CONNECTED_TIMEOUT_MS, WIFI_AP_NO_CLIENT_TIMEOUT_MS);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    free(html_page);
    return ESP_OK;
}

// HTTP handler for device list API
static esp_err_t api_devices_handler(httpd_req_t *req) {
    char *json_response = malloc(8192);
    if (!json_response) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int pos = sprintf(json_response, "{\"devices\":[");
    int64_t now = esp_timer_get_time() / 1000;

    for (int i = 0; i < device_count; i++) {
        if (!devices[i].active) continue;

        int64_t ago = now - devices[i].last_seen;
        bool online = (ago < TAG_ONLINE_THRESHOLD_MS);
        bool registered = is_registered_mac(devices[i].addr);

        pos += sprintf(json_response + pos,
            "%s{\"id\":%d,\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
            "\"name\":\"%s\",\"manufacturer\":\"%s\",\"rssi\":%d,\"last_seen_ago\":\"%lld s\","
            "\"online\":%s,\"registered\":%s}",
            (i > 0 && pos > 13) ? "," : "",
            devices[i].id,
            devices[i].addr[0], devices[i].addr[1], devices[i].addr[2],
            devices[i].addr[3], devices[i].addr[4], devices[i].addr[5],
            devices[i].has_name ? devices[i].name : "Unknown",
            devices[i].has_manufacturer ? get_manufacturer_name(devices[i].manufacturer_id) : "Unknown",
            devices[i].rssi,
            ago / 1000,
            online ? "true" : "false",
            registered ? "true" : "false");
    }

    pos += sprintf(json_response + pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, pos);
    free(json_response);
    return ESP_OK;
}

// HTTP handler for device registration
static esp_err_t api_register_handler(httpd_req_t *req) {
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char id_str[16];
        if (httpd_query_key_value(query, "id", id_str, sizeof(id_str)) == ESP_OK) {
            int id = atoi(id_str);
            register_device(id);
            httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id parameter");
    return ESP_FAIL;
}

// HTTP handler for device unregistration
static esp_err_t api_unregister_handler(httpd_req_t *req) {
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char id_str[16];
        if (httpd_query_key_value(query, "id", id_str, sizeof(id_str)) == ESP_OK) {
            int id = atoi(id_str);
            deregister_device(id);
            httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id parameter");
    return ESP_FAIL;
}

// HTTP handler for save and exit
static esp_err_t api_save_handler(httpd_req_t *req) {
    // Configuration is already saved by register/unregister handlers
    // Just trigger AP shutdown
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");

    // Set flag to stop AP immediately
    ap_active = false;
    ESP_LOGI(TAG, "User requested save & exit - shutting down AP");

    return ESP_OK;
}

// Start HTTP server
static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    config.stack_size = 8192;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t api_devices = {
            .uri = "/api/devices",
            .method = HTTP_GET,
            .handler = api_devices_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &api_devices);

        httpd_uri_t api_register = {
            .uri = "/api/register",
            .method = HTTP_POST,
            .handler = api_register_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &api_register);

        httpd_uri_t api_unregister = {
            .uri = "/api/unregister",
            .method = HTTP_POST,
            .handler = api_unregister_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &api_unregister);

        httpd_uri_t api_save = {
            .uri = "/api/save",
            .method = HTTP_POST,
            .handler = api_save_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &api_save);

        // iOS/macOS captive portal detection
        httpd_uri_t hotspot_detect = {
            .uri = "/hotspot-detect.html",
            .method = HTTP_GET,
            .handler = captive_detect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &hotspot_detect);

        // Android captive portal detection
        httpd_uri_t generate_204 = {
            .uri = "/generate_204",
            .method = HTTP_GET,
            .handler = captive_detect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &generate_204);

        // Windows captive portal detection
        httpd_uri_t connecttest = {
            .uri = "/connecttest.txt",
            .method = HTTP_GET,
            .handler = captive_detect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &connecttest);

        // Generic success page
        httpd_uri_t success = {
            .uri = "/success.txt",
            .method = HTTP_GET,
            .handler = captive_detect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &success);

        // Captive portal catch-all handler - must be last
        httpd_uri_t captive = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = captive_portal_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &captive);

        ESP_LOGI(TAG, "HTTP server started on http://192.168.4.1");
        return server;
    }

    ESP_LOGE(TAG, "Failed to start HTTP server");
    return NULL;
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station connected: %02x:%02x:%02x:%02x:%02x:%02x",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5]);
        if (!ap_has_clients) {
            ap_has_clients = true;
            ap_last_client_time = esp_timer_get_time() / 1000;
            ESP_LOGI(TAG, "First client connected - starting 2-minute countdown");
        }
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station disconnected: %02x:%02x:%02x:%02x:%02x:%02x",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5]);
    }
}

// Initialize WiFi AP
static void wifi_init_softap(void) {
    // Initialize network interface (OK if already initialized)
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    // Create event loop (OK if already exists)
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    // Create netif if it doesn't exist
    if (!wifi_ap_netif) {
        wifi_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .password = "",
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,  // No password
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Start DNS server task for captive portal
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &dns_task_handle);

    ESP_LOGI(TAG, "WiFi AP started: SSID=%s, Channel=%d", WIFI_AP_SSID, WIFI_AP_CHANNEL);
    ESP_LOGI(TAG, "Connect to http://192.168.4.1 to manage devices");
}

// AP monitor task - shuts down AP after timeout
static void ap_monitor_task(void *param) {
    while (ap_active) {
        int64_t now = esp_timer_get_time() / 1000;
        int64_t elapsed = now - ap_start_time;

        // Check timeout conditions
        // Timeout if no client connects within 1 minute
        if (!ap_has_clients && elapsed >= WIFI_AP_NO_CLIENT_TIMEOUT_MS) {
            ESP_LOGI(TAG, "AP no-client timeout (1 min), shutting down WiFi AP");
            ap_active = false;  // Trigger shutdown
        }
        // Or timeout 2 minutes after client connects
        else if (ap_has_clients && (now - ap_last_client_time) >= WIFI_AP_CLIENT_CONNECTED_TIMEOUT_MS) {
            ESP_LOGI(TAG, "AP client-connected timeout (2 min), shutting down WiFi AP");
            ap_active = false;  // Trigger shutdown
        }

        // Check if we should shutdown (either from timeout or manual trigger)
        if (!ap_active) {
            break;  // Exit loop to perform shutdown
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Shutdown AP (reached when ap_active becomes false)
    ESP_LOGI(TAG, "Shutting down WiFi AP...");

    // Give DNS server time to exit cleanly
    vTaskDelay(pdMS_TO_TICKS(200));

    if (server) {
        httpd_stop(server);
        server = NULL;
    }

    // DNS task should have exited by now
    dns_task_handle = NULL;

    esp_wifi_stop();
    esp_wifi_deinit();

    // Destroy netif to allow clean restart
    if (wifi_ap_netif) {
        esp_netif_destroy(wifi_ap_netif);
        wifi_ap_netif = NULL;
    }

    ESP_LOGI(TAG, "WiFi AP stopped");
    vTaskDelete(NULL);
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

                    // Parse both advertisement data and scan response data
                    for (int pass = 0; pass < 2; pass++) {
                        uint8_t *data = (pass == 0) ? adv_data : scan_result->scan_rst.ble_adv;
                        uint8_t data_len = (pass == 0) ? adv_data_len : scan_result->scan_rst.adv_data_len + scan_result->scan_rst.scan_rsp_len;

                        // On second pass, use scan response if available
                        if (pass == 1 && scan_result->scan_rst.scan_rsp_len > 0) {
                            data = adv_data + adv_data_len;
                            data_len = scan_result->scan_rst.scan_rsp_len;
                        } else if (pass == 1) {
                            break; // No scan response data
                        }

                    for (int i = 0; i < data_len;) {
                        uint8_t len = data[i];
                        if (len == 0) break;

                        uint8_t type = data[i + 1];

                        // Shortened local name
                        if (type == 0x08 && !devices[idx].has_name) {
                            int name_len = len - 1;
                            if (name_len > 31) name_len = 31;
                            memcpy(devices[idx].name, &data[i + 2], name_len);
                            devices[idx].name[name_len] = '\0';
                            devices[idx].has_name = true;
                        }
                        // Complete local name
                        else if (type == 0x09 && !devices[idx].has_complete_name) {
                            int name_len = len - 1;
                            if (name_len > 31) name_len = 31;
                            memcpy(devices[idx].complete_name, &data[i + 2], name_len);
                            devices[idx].complete_name[name_len] = '\0';
                            devices[idx].has_complete_name = true;
                            // Also copy to regular name if not set
                            if (!devices[idx].has_name) {
                                memcpy(devices[idx].name, &data[i + 2], name_len);
                                devices[idx].name[name_len] = '\0';
                                devices[idx].has_name = true;
                            }
                        }
                        // Appearance
                        else if (type == 0x19 && len >= 3) {
                            devices[idx].appearance = data[i + 2] | (data[i + 3] << 8);
                        }
                        // Manufacturer Specific Data
                        else if (type == 0xFF && len >= 3 && !devices[idx].has_manufacturer) {
                            devices[idx].manufacturer_id = data[i + 2] | (data[i + 3] << 8);
                            devices[idx].has_manufacturer = true;
                        }

                        i += len + 1;
                    }
                    } // end pass loop
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
        // Check if BLE scan completed and need to switch to AP mode
        if (ble_scan_complete && !wifi_ap_started) {
            ESP_LOGI(TAG, "BLE scan period (30s) complete. Found %d devices.", device_count);

            // Clean up BLE stack to free IRAM
            cleanup_ble_stack();

            // Start WiFi AP
            ESP_LOGI(TAG, "Starting WiFi AP for device management...");
            wifi_init_softap();
            ap_start_time = esp_timer_get_time() / 1000;
            ap_last_client_time = ap_start_time;
            ap_active = true;
            ap_has_clients = false;
            wifi_ap_started = true;

            server = start_webserver();
            if (server) {
                xTaskCreate(ap_monitor_task, "ap_monitor", 4096, NULL, 5, NULL);
            }

            ESP_LOGI(TAG, "WiFi AP started. Connect to SSID: TAG-SCANNER, IP: 192.168.4.1");
        }

        // Flash internal LED if in AP mode
        if (wifi_ap_started && ap_active) {
            gpio_set_level(INTERNAL_LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(INTERNAL_LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;  // Skip device printing in AP mode
        }

        // If AP was active but stopped, turn off LED and reset flag
        if (wifi_ap_started && !ap_active) {
            gpio_set_level(INTERNAL_LED_PIN, 0);
            wifi_ap_started = false;
            ble_scan_complete = false;  // Reset flag so we don't restart AP immediately

            // Reinitialize BLE for continuous scanning
            ESP_LOGI(TAG, "Restarting BLE scanning after AP shutdown");

            esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
            esp_err_t ret = esp_bt_controller_init(&bt_cfg);
            if (ret == ESP_OK) {
                ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
                if (ret == ESP_OK) {
                    ret = esp_bluedroid_init();
                    if (ret == ESP_OK) {
                        ret = esp_bluedroid_enable();
                        if (ret == ESP_OK) {
                            ret = esp_ble_gap_register_callback(esp_gap_cb);
                            if (ret == ESP_OK) {
                                static esp_ble_scan_params_t ble_scan_params = {
                                    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
                                    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
                                    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
                                    .scan_interval          = 0x50,
                                    .scan_window            = 0x30,
                                    .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
                                };
                                esp_ble_gap_set_scan_params(&ble_scan_params);
                                esp_ble_gap_start_scanning(0);
                                ESP_LOGI(TAG, "BLE scanning resumed");
                            }
                        }
                    }
                }
            }

            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restart BLE: %s", esp_err_to_name(ret));
            }

            ESP_LOGI(TAG, "Resuming normal operation after AP shutdown");
        }

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
        printf("%-3s %-20s %-6s %-8s %-35s %-6s\n", "ID", "MAC Address", "RSSI", "Type", "Device", "Status");
        printf("-------------------------------------------------------------------------------------------------------\n");

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

            // Display <Manufacturer> - <Name> format
            char display_name[64];
            const char *device_name = "";
            if (devices[idx].has_complete_name && strlen(devices[idx].complete_name) > 0) {
                device_name = devices[idx].complete_name;
            } else if (devices[idx].has_name && strlen(devices[idx].name) > 0) {
                device_name = devices[idx].name;
            } else {
                device_name = "(no name)";
            }

            if (devices[idx].has_manufacturer) {
                snprintf(display_name, sizeof(display_name), "%s - %s",
                        get_manufacturer_name(devices[idx].manufacturer_id), device_name);
            } else {
                snprintf(display_name, sizeof(display_name), "%s", device_name);
            }
            printf("%-35s", display_name);

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

                // Display <Manufacturer> - <Name> format
                char display_name[64];
                const char *device_name = "";
                if (devices[idx].has_complete_name && strlen(devices[idx].complete_name) > 0) {
                    device_name = devices[idx].complete_name;
                } else if (devices[idx].has_name && strlen(devices[idx].name) > 0) {
                    device_name = devices[idx].name;
                } else {
                    device_name = "(no name)";
                }

                if (devices[idx].has_manufacturer) {
                    snprintf(display_name, sizeof(display_name), "%s - %s",
                            get_manufacturer_name(devices[idx].manufacturer_id), device_name);
                } else {
                    snprintf(display_name, sizeof(display_name), "%s", device_name);
                }
                printf("%-35s", display_name);

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

    ESP_LOGI(TAG, "=== BLE scan started for 30 seconds ===");
    ESP_LOGI(TAG, "Looking for BLE tags...");

    // Create timer to stop BLE scan after 30 seconds and start WiFi AP
    const esp_timer_create_args_t timer_args = {
        .callback = &ble_scan_timer_callback,
        .name = "ble_scan_timer"
    };
    esp_timer_handle_t scan_timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &scan_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(scan_timer, BLE_SCAN_DURATION_MS * 1000));  // microseconds

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