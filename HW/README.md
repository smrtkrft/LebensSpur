# LebensSpur ESP32-C6 Firmware

ESP32-C6 tabanlı IoT Dead Man's Switch firmware'i.

## 🔧 Hardware

- **MCU**: Seeed XIAO ESP32-C6 (WiFi 6, BLE 5)
- **Internal Flash**: 4MB (OTA, NVS, PHY)
- **External Flash**: W25Q256 32MB (Web files, logs, config)
- **Relay**: GPIO18 (D10) - Active HIGH
- **Button**: GPIO17 (D7) - Pull-up

### External Flash SPI Pins
| Signal | GPIO |
|--------|------|
| CS     | 21   |
| MISO   | 0    |
| MOSI   | 22   |
| SCLK   | 16   |

## 📁 Project Structure

```
HW_esp32C6/
├── CMakeLists.txt          # Project configuration
├── partitions.csv          # 4MB internal flash partition table
├── sdkconfig.defaults      # ESP-IDF default settings
├── create_ca_cert.ps1      # CA certificate generator
├── generate_certs.ps1      # SSL certificate generator
└── main/
    ├── CMakeLists.txt      # Component configuration
    ├── main.c              # Main application
    ├── wifi_manager.h/c    # WiFi AP+STA management
    ├── ext_flash.h/c       # External flash driver
    ├── file_manager.h/c    # LittleFS file system
    ├── web_server.h/c      # HTTP server
    ├── config_manager.h/c  # Configuration management
    ├── session_auth.h/c    # Cookie-based session auth
    ├── timer_scheduler.h/c # Dead Man's Switch timer
    ├── relay_manager.h/c   # Relay control (GPIO18)
    ├── button_manager.h/c  # Button input (GPIO17)
    ├── mail_sender.h/c     # SMTP email sender
    ├── log_manager.h/c     # Log management
    ├── ota_manager.h/c     # OTA update from GitHub
    └── certs/
        ├── server.crt      # SSL certificate
        └── server.key      # SSL private key
```

## ⚙️ Features

### WiFi
- **AP Mode**: `LebensSpur_XXXX` (password: `lebensspur123`)
- **STA Mode**: Connects to saved network
- AP and STA run concurrently

### Web Server
- HTTP (port 80)
- Cookie-based session authentication
- RESTful API
- Static file serving from external flash

### API Endpoints
| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Main page |
| `/api/status` | GET | System status |
| `/api/relay/*` | GET/POST | Relay control |
| `/api/wifi/*` | GET/POST | WiFi settings |
| `/api/device/info` | GET | Device info |
| `/api/timer/*` | GET/POST | Timer control |
| `/api/mail/*` | GET/POST | Mail settings |
| `/api/setup/*` | GET/POST | First-time setup |

### Storage
- **External Flash**: 32MB LittleFS
  - `/ext/web` - Web files (downloaded from GUI repo)
  - `/ext/logs` - Log files (max 1MB)
  - `/ext/config` - Configuration
  - `/ext/data` - User data

### OTA Update
- Downloads firmware from GitHub releases
- Automatic rollback support

## 🚀 Build Instructions

### Requirements
- ESP-IDF v5.5.2
- Python 3.8+
- CMake 3.16+

### Build Steps

```powershell
# Activate ESP-IDF environment
C:\Espressif\frameworks\esp-idf-v5.5.2\export.ps1

# Navigate to project directory
cd HW_esp32C6

# Set target
idf.py set-target esp32c6

# Build
idf.py build

# Flash
idf.py -p COM10 flash

# Monitor
idf.py -p COM10 monitor
```

## 📊 Memory Configuration

### Internal Flash (4MB)
| Partition | Offset | Size | Description |
|-----------|--------|------|-------------|
| nvs | 0x9000 | 48KB | NVS key-value |
| otadata | 0x15000 | 8KB | OTA data |
| phy_init | 0x17000 | 4KB | PHY calibration |
| ota_0 | 0x20000 | 1.44MB | Firmware A |
| ota_1 | 0x190000 | 1.44MB | Firmware B |
| nvs_keys | 0x300000 | 4KB | NVS encryption |

### External Flash (32MB)
- Full capacity LittleFS file system

## 📜 License

MIT License
