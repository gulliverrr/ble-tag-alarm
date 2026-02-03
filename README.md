# BLE Tag Alarm Scanner

ESP32-based BLE scanner for tracking and monitoring Teltonika (or similar) Bluetooth tags.

## Features

- Scans for up to 50 BLE devices
- Register up to 5 tags for monitoring (FIFO queue)
- Color-coded status display:
  - **Green** (REG-OK): Tag is online
  - **Orange** (REG-WARN): Tag offline < 30 seconds
  - **Red** (REG-OFF): Tag offline ≥ 30 seconds
- Interactive registration via UART (type device ID + ENTER)
- Sorted by RSSI strength (strongest first)
- Real-time status updates every 5 seconds

## Hardware

- **MCU**: ESP32 (original - 2MB flash minimum)
- **UART**: Console at 115200 baud
- **GPIO Input**: Configurable pin for sensor/switch monitoring
- **Relay Output**: Configurable pin for relay control based on tag presence
- **RGB LED**: Status indicator with 3 GPIO pins (R, G, B)

## Hardware Status Indicators

### RGB LED Status
- **Blue**: Initializing or no tags registered
- **Green**: All registered tags online
- **Orange**: One or more tags in warning state (offline < 30s)
- **Red**: One or more tags offline ≥ 30 seconds

### Relay Control
- Automatically activates when any registered tag is online
- Deactivates when all registered tags are offline

## Build & Flash

```bash
# Build
idf.py build

# Flash and monitor
idf.py -p PORT flash monitor

# Exit monitor: Ctrl+]
```

## Usage

1. Scan results display: ID, MAC address, RSSI, address type, device name, status
2. Type device ID (1-99) + ENTER to register/deregister
3. Registered devices appear at top with color-coded status
4. Up to 20 devices shown in scan results

## Configuration

Edit `src/main.c` to configure GPIO pins:
- `GPIO_INPUT_PIN`: Input sensor/switch
- `GPIO_RELAY_PIN`: Relay control output
- `RGB_LED_RED_PIN`: RGB LED red channel
- `RGB_LED_GREEN_PIN`: RGB LED green channel
- `RGB_LED_BLUE_PIN`: RGB LED blue channel

## Future Enhancements

- NVS storage for persistent registered devices
- Web interface for configuration
- MQTT integration for remote monitoring
- Multiple alarm zones

## License

Private project - not for redistribution
