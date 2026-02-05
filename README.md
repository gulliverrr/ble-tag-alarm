# BLE Tag Alarm Scanner

ESP32-based BLE scanner for bike anti-theft system with Teltonika BLE tag monitoring.

## System Overview

This system monitors your bike's charging cable and BLE tag presence to:
- Control charger power (save electricity when cable disconnected)
- Trigger alarm if cable disconnected without BLE tag present (theft detection)
- Provide visual status via RGB LED

### System States

1. **CHARGING_TAG_PRESENT** (🟢 GREEN LED)
   - Cable connected + Tag seen within last 60 seconds
   - Charger: ON | Alarm: OFF

2. **CHARGING_TAG_RECENT** (🟠 ORANGE LED)
   - Cable connected + Tag seen 60s to 10min ago
   - Charger: ON | Alarm: OFF

3. **CHARGING_ALARM_ARMED** (🔴 RED LED)
   - Cable connected + No tag for >10 minutes
   - Charger: ON | Alarm: ARMED

4. **NOT_CHARGING** (🔵 BLUE LED)
   - Cable disconnected (3rd pin has no voltage)
   - Charger: OFF | Alarm: OFF

## Features

- **BLE Scanning**: 30-second initial scan, continuous scanning after config
- **WiFi Configuration Portal**: Auto-launches after boot for device registration
- **Web Interface**: Browser-based device management with captive portal support
- **NVS Storage**: Persistent device registration across reboots
- **4-State System**: Automatic charger/alarm control based on tag presence
- **LED Indicators**: RGB status LED + internal LED for AP mode
- **Multi-Platform**: iOS, Android, Windows captive portal detection

## Quick Start

1. **Initial Setup**
   - Power on ESP32 → 30-second BLE scan
   - Connect to WiFi: `TAG-SCANNER` (open network)
   - Captive portal auto-opens at `192.168.4.1`
   - Register up to 5 BLE tags via web interface
   - Click "Save & Exit" or wait for timeout

2. **Normal Operation**
   - Continuous BLE scanning and monitoring
   - RGB LED shows system state
   - Serial console shows device list (115200 baud)
   - Type device ID + ENTER to register/unregister via UART

## System States

| State | LED Color | Cable | Tag Status | Charger | Alarm |
|-------|-----------|-------|------------|---------|-------|
| **TAG_PRESENT** | 🟢 Green | Connected | <60s ago | ON | OFF |
| **TAG_RECENT** | 🟠 Orange | Connected | 60s-10min | ON | OFF |
| **ALARM_ARMED** | 🔴 Red | Connected | >10min | ON | ON |
| **NOT_CHARGING** | 🔵 Blue | Disconnected | Any | OFF | OFF |

## Hardware

- **ESP32** (2MB flash minimum) with WiFi/BLE
- **12V Relay**: Cable presence detection
- **2-Channel Relay**: Charger (220V AC) + Alarm control
- **RGB LED**: Common cathode + 220Ω resistors
- **Power**: 5V for ESP32, 12V from bike for detection

## Wiring Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         ESP32 Connections                               │
└─────────────────────────────────────────────────────────────────────────┘

┌──────────────┐
│   ESP32      │
│              │
│         GND ●─────────────────────┬───────────────┬──────────────┬─────── GND
│              │                    │               │              │
│   GPIO34 ●───────┐                │               │              │
│   (INPUT)    │   │  ┌─────────────┴──────┐        │              │
│              │   └──┤ NO                 │        │              │
│              │      │    12V Relay       │        │              │
│              │      │  (Presence Det)    │        │              │
│              │  ┌───┤ COM                │        │              │
│              │  │   │                    │        │              │
│              │  │   └──────┬─────────────┘        │              │
│   GPIO26 ●───┼──┼──────────┼──────────────────────┤              │
│   (CH-RLY)   │  │          │                      │              │
│              │  │          │                      │              │
│   GPIO33 ●───┼──┼──────────┼──────────────────────┤              │
│   (ALM-RLY)  │  │          │                      │              │
│              │  │          │                      │              │
│   GPIO25 ●───┼──┼──────────┼───[ 220Ω ]───┬───────┤              │
│   (LED-R)    │  │          │              │       │              │
│              │  │          │          ┌───▼────┐  │              │
│   GPIO27 ●───┼──┼──────────┼──[ 220Ω ]┤   RGB  │  │              │
│   (LED-G)    │  │          │          │   LED  │  │              │
│              │  │          │          │ Common │  │              │
│   GPIO32 ●───┼──┼──────────┼──[ 220Ω ]┤Cathode │  │              │
│   (LED-B)    │  │          │          └────────┘  │              │
│              │  │          │                      │              │
│   3.3V ●─────┼──┼──────────┘                      │              │
│              │  │                                 │              │
│   GND ●──────┼──┼────[ 10kΩ ]─── (pull-down for GPIO34)          │
│              │  │                                 │              │
│   5V ●───────┼──┼─────────────────────────────────┘              │
└──────────────┘  │                                                │
                  │                                                │
                  │                                                │
┌─────────────────┴────────────────────────────────────────────────┴───────┐
│                   12V Relay Module (Presence Detection)                  │
│                                                                          │
│   12V+ (from bike 3rd pin) ──→ Coil (+)                                  │
│   GND  ─────────────────────→ Coil (-)                                   │
│                                                                          │
│   3.3V (ESP32) ─────────────→ COM (Common)                               │
│   GPIO34 (ESP32) ────────────→ NO (Normally Open)                        │
│   10kΩ Resistor ─────────────→ Between GPIO34 and GND (pull-down)        │
│                                                                          │
│   Logic: When 12V present → Relay closes → GPIO34 pulled HIGH (3.3V)     │
│          When 0V present  → Relay opens  → GPIO34 pulled LOW (via 10kΩ)  │
└──────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────────┐
│              2-Channel Relay Module (Charger & Alarm)                    │
│                                                                          │
│  Channel 1 (Charger Control):                                            │
│    VCC ────────────────────→ 5V (ESP32)                                  │
│    GND ────────────────────→ GND                                         │
│    IN1 ────────────────────→ GPIO26 (ESP32)                              │
│    COM ────────────────────→ 220V AC Live (from mains)                   │
│    NO  ────────────────────→ 220V AC Live (to charger)                   │
│                                                                          │
│  Channel 2 (Alarm Control):                                              │
│    IN2 ────────────────────→ GPIO33 (ESP32)                              │
│    COM ────────────────────→ Alarm Power (+)                             │
│    NO  ────────────────────→ Alarm Trigger                               │
│                                                                          │
│  Logic: GPIO HIGH → Relay activates (COM connects to NO)                 │
│         GPIO LOW  → Relay off (COM disconnected)                         │
└──────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────────┐
│                     RGB LED Connection Details                           │
│                                                                          │
│   Common Cathode RGB LED:                                                │
│                                                                          │
│   ESP32 GPIO25 ──[ 220Ω ]──→ Red Anode                                   │
│   ESP32 GPIO27 ──[ 220Ω ]──→ Green Anode                                 │
│   ESP32 GPIO32 ──[ 220Ω ]──→ Blue Anode                                  │
│   LED Cathode ──────────────→ GND                                        │
│                                                                          │
│   Note: Use 150-330Ω resistors depending on LED brightness needed        │
└──────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────────┐
│                    Power Supply Connections                              │
│                                                                          │
│   5V Power Supply:                                                       │
│     5V   ──→ ESP32 5V pin                                                │
│     GND  ──→ ESP32 GND                                                   │
│     5V   ──→ 2-Channel Relay Module VCC                                  │
│                                                                          │
│   Bike 3rd Pin (12V):                                                    │
│     12V+ ──→ 12V Relay Coil (+)                                          │
│     GND  ──→ 12V Relay Coil (-) + ESP32 GND (common ground!)             │
│                                                                          │
│   220V AC (Charger Control):                                             │
│     Live ──→ 2-CH Relay CH1 COM                                          │
│     CH1 NO ──→ Charger Live Input                                        │
│     Neutral ──→ Direct to Charger (bypasses relay)                       │
│     Ground ──→ Direct to Charger (bypasses relay)                        │
│                                                                          │
│   ⚠️  WARNING: 220V AC is dangerous! Use proper insulation and           │
│       ensure relay is rated for AC switching (minimum 10A)               │
└──────────────────────────────────────────────────────────────────────────┘
```

## GPIO Pin Configuration

| Function | GPIO Pin | Direction | Notes |
|----------|----------|-----------|-------|
| Cable Presence Input | GPIO34 | Input | Pulled LOW, driven HIGH by 12V relay |
| Charger Relay Control | GPIO26 | Output | Active HIGH to turn on charger |
| Alarm Relay Control | GPIO33 | Output | Active HIGH to trigger alarm |
| RGB LED - Red | GPIO25 | Output | Via 220Ω resistor |
| RGB LED - Green | GPIO27 | Output | Via 220Ω resistor |
| RGB LED - Blue | GPIO32 | Output | Via 220Ω resistor |

## Important Notes

### 12V Relay for Presence Detection
- **DO NOT** connect 12V directly to ESP32 GPIO! The relay isolates the 12V from the ESP32
- The relay coil is powered by the 12V from the bike's 3rd pin
- When 12V is present, relay closes and connects 3.3V to GPIO34 (reads HIGH)
- When 12V is absent, relay opens and GPIO34 reads LOW via external pull-down resistor
- **REQUIRED**: Add a 10kΩ pull-down resistor from GPIO34 to GND
  - GPIO34 is input-only and has no internal pull resistors
  - Without external pull-down, pin will float when relay is open
- Use a relay module with a coil rated for 12V DC
- GPIO34 is input-only on ESP32, perfect for this application

### 2-Channel Relay Module
- Most modules have active-LOW logic (relay activates when GPIO is LOW)
- Check your module's datasheet - code may need inversion
- Ensure relay is rated for 220V AC @ 10A minimum for charger control
- Use proper electrical enclosure and follow local electrical codes
- Consider using a relay with LED indicators for debugging

### Power Supply
- ESP32 needs stable 5V (via USB or regulated power supply)
- **CRITICAL**: Common ground between all components!
- 12V relay module and ESP32 must share the same GND connection
- Isolate 220V AC section from low-voltage (5V/12V) circuits

## Build & Flash

```bash
# Build
idf.py build

# Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor

# Exit monitor: Ctrl+]
```

## Usage

1. Power up the ESP32 and connect via serial monitor (115200 baud)
2. ESP32 will start scanning for BLE devices
3. Scan results display: ID, MAC address, RSSI, address type, device name, status
4. Type device ID (1-99) + ENTER to register your BLE tag
5. Registered tags appear at top with color-coded status
6. System automatically controls charger and alarm based on tag presence

### Tag Registration

- Type the device ID number followed by ENTER
- To deregister: Type the same ID again and press ENTER
- Up to 5 tags can be registered (FIFO queue)

## Configuration

Edit `src/main.c` to configure GPIO pins and timing:

### GPIO Pins
```c
#define GPIO_PRESENCE_PIN  GPIO_NUM_34  // Cable presence detection
#define GPIO_CHARGER_RELAY GPIO_NUM_26  // Charger relay control
#define GPIO_ALARM_RELAY   GPIO_NUM_33  // Alarm relay control
#define RGB_LED_RED_PIN    GPIO_NUM_25  // RGB LED Red
#define RGB_LED_GREEN_PIN  GPIO_NUM_27  // RGB LED Green
#define RGB_LED_BLUE_PIN   GPIO_NUM_32  // RGB LED Blue
```

### Timing Constants
```c
#define TAG_PRESENT_THRESHOLD_MS     60000              // 60 seconds
#define TAG_RECENT_THRESHOLD_MS      (10 * 60 * 1000)  // 10 minutes
```

## Troubleshooting

### Relay Issues
- Check if relay module is active-HIGH or active-LOW
- Verify VCC connection (5V for relay module)
- Ensure common ground between ESP32 and relay modules
- Check relay LED indicators (if available)

### Cable Presence Not Detected
- Verify 12V is present on bike's 3rd pin when connected
- Check 12V relay coil connections
- Test relay manually with multimeter
- Ensure GPIO34 is not floating (should read LOW when relay open)

### BLE Tag Not Detected
- Ensure tag is powered and within range (~10 meters)
- Check tag battery level
- Verify tag is advertising (some tags have power-saving modes)
- Register tag using its ID from the scan results

### RGB LED Issues
- Verify LED is common cathode type
- Check resistor values (150-330Ω)
- Test each color individually
- Ensure cathode is connected to GND

## Safety Warnings

⚠️ **ELECTRICAL SAFETY**
- 220V AC can be lethal! Only work on AC wiring if qualified
- Always disconnect mains power before working
- Use properly rated relays (minimum 10A AC)
- Install in proper electrical enclosure
- Follow local electrical codes and regulations
- Consider professional installation for AC components

⚠️ **AUTOMOTIVE ELECTRICAL**
- Do not short bike battery connections
- Use proper fuses on all 12V connections
- Ensure waterproof connections for outdoor use
- Protect electronics from vibration and moisture

## Future Enhancements

- NVS storage for persistent registered devices
- Web interface for configuration
- MQTT integration for remote monitoring
- Multiple alarm zones

## License

Private project
. ~/esp/esp-idf-v5.5/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 -b 1500000 flash monitor
# Exit monitor: Ctrl+]
```

## Configuration Modes

### WiFi AP Mode (Initial Setup)
- **Trigger**: Automatic after 30-second BLE scan on boot
- **SSID**: `TAG-SCANNER` (open network)
- **IP**: `192.168.4.1`
- **Captive Portal**: Auto-opens on iOS, Android, Windows
- **Timeouts**:
  - 1 minute if no client connects
  - 2 minutes after client connects
- **Exit**: "Save & Exit" button or timeout
- **LED**: Internal LED (GPIO2) blinks during AP mode

### Serial Console Mode (Advanced)
- **Baud**: 115200
- **Commands**: Type device ID (1-99) + ENTER to register/unregister
- **Display**: Updates every 5 seconds with device list and system status

## Configuration

Edit `src/main.c` to customize:

```c
// WiFi AP
#define WIFI_AP_SSID "TAG-SCANNER"
#define WIFI_AP_NO_CLIENT_TIMEOUT_MS (60 * 1000)
#define WIFI_AP_CLIENT_CONNECTED_TIMEOUT_MS (2 * 60 * 1000)

// Timing
#define TAG_PRESENT_THRESHOLD_MS     60000
#define TAG_RECENT_THRESHOLD_MS      (10 * 60 * 1000)

// BLE Scan
#define BLE_SCAN_DURATION_MS 30000