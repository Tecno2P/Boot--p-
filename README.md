# ⚡ ESP32 Dual Boot Manager

A production-grade, dual-OTA boot manager for ESP32 (4MB flash) built on ESP-IDF v5.1+.

[![Build Status](https://github.com/YOUR_USER/YOUR_REPO/actions/workflows/build.yml/badge.svg)](https://github.com/YOUR_USER/YOUR_REPO/actions/workflows/build.yml)

---

## Features

| Feature | Details |
|---------|---------|
| **Dual OTA** | `ota_0` / `ota_1` with automatic rollback |
| **Boot Manager Mode** | Hold BOOT button or triggered by 3 consecutive failures |
| **Web UI** | Dashboard, OTA upload, OTA URL, WiFi config, partition switch |
| **AP + STA** | Simultaneous AP (always on) + STA (connect to router) |
| **OTA Methods** | Browser file upload or HTTP/HTTPS URL |
| **Crash Recovery** | Automatic rollback on crash; force boot manager on loop |
| **GitHub Actions** | Auto-build + auto-release on every push to `main` |

---

## Project Structure

```
esp32-boot-manager/
├── .github/
│   └── workflows/
│       └── build.yml              # Auto-build + auto-release
├── components/
│   ├── boot_manager/              # Boot loop detection, forced mode
│   │   ├── CMakeLists.txt
│   │   ├── boot_manager.h
│   │   └── boot_manager.c
│   ├── ota_manager/               # OTA via URL and file upload
│   │   ├── CMakeLists.txt
│   │   ├── ota_manager.h
│   │   └── ota_manager.c
│   ├── wifi_manager/              # AP+STA dual-mode WiFi
│   │   ├── CMakeLists.txt
│   │   ├── wifi_manager.h
│   │   └── wifi_manager.c
│   └── web_server/                # HTTP server + all API endpoints
│       ├── CMakeLists.txt
│       ├── web_server.h
│       └── web_server.c
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                     # Entry point
│   └── app_version.h
├── web_ui/                        # Served from SPIFFS
│   ├── index.html
│   ├── style.css
│   └── app.js
├── CMakeLists.txt                 # Root (includes SPIFFS image build)
├── partitions.csv                 # 4MB partition layout
└── sdkconfig.defaults             # Build configuration
```

---

## Partition Layout

| Partition | Type | Offset | Size |
|-----------|------|--------|------|
| nvs | data/nvs | 0x9000 | 16 KB |
| otadata | data/ota | 0xD000 | 8 KB |
| phy_init | data/phy | 0xF000 | 4 KB |
| ota_0 | app/ota_0 | 0x10000 | 1664 KB |
| ota_1 | app/ota_1 | 0x1B0000 | 1664 KB |
| spiffs | data/spiffs | 0x350000 | 704 KB |

---

## Boot Behavior

```
Power on
  │
  ├── BOOT button held? ──YES──► Boot Manager Mode
  │
  ├── 3+ consecutive failures? ──YES──► Boot Manager Mode (forced)
  │
  └── NO ──► Normal Application Boot
               └── Mark OTA valid
               └── Clear failure counter
               └── Run user application
```

### Boot Manager Mode

1. Starts WiFi: **AP `ESP32-BootManager`** (always) + **STA** (if credentials saved)
2. Mounts SPIFFS for web UI
3. Starts HTTP server on port 80
4. Access at: **http://192.168.4.1** (via AP) or STA IP

---

## Prerequisites

- ESP-IDF v5.1 or newer ([Installation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/))
- Python 3.8+
- Git

---

## Build & Flash

### 1. Clone and set up

```bash
git clone https://github.com/YOUR_USER/YOUR_REPO.git
cd esp32-boot-manager
```

### 2. Set target and configure

```bash
idf.py set-target esp32
# sdkconfig.defaults is applied automatically on first build
```

### 3. Build

```bash
idf.py build
```

This builds:
- `build/bootloader/bootloader.bin`
- `build/partition_table/partition-table.bin`
- `build/esp32_boot_manager.bin` (app)
- `build/spiffs.bin` (web UI)

### 4. Flash everything (first time)

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

This flashes all binaries including SPIFFS. Replace `/dev/ttyUSB0` with your port.

### 5. Flash app only (subsequent updates)

```bash
idf.py -p /dev/ttyUSB0 app-flash monitor
```

### Manual esptool flash

```bash
esptool.py -p /dev/ttyUSB0 -b 460800 \
  --chip esp32 \
  --before default_reset \
  --after hard_reset \
  write_flash \
  --flash_mode dio \
  --flash_freq 40m \
  --flash_size 4MB \
  0x1000   build/bootloader/bootloader.bin \
  0x8000   build/partition_table/partition-table.bin \
  0x10000  build/esp32_boot_manager.bin \
  0x350000 build/spiffs.bin
```

---

## Web Interface

### Access

| Network | URL |
|---------|-----|
| Via AP (ESP32 hotspot) | http://192.168.4.1 |
| Via STA (your router) | http://<device-IP> |

### Pages

- **Dashboard** — Firmware info, partition status, network, system health
- **OTA Update** — Upload `.bin` from browser OR flash from URL
- **WiFi Config** — Save STA credentials; reconnect logic
- **Partitions** — Visual partition status; one-click switch

---

## OTA Update Methods

### Method 1: Browser file upload

1. Enter Boot Manager Mode (hold BOOT, power on)
2. Connect to `ESP32-BootManager` / `12345678`
3. Open http://192.168.4.1 → OTA Update tab
4. Drop or select `app.bin` → Upload & Flash
5. Device reboots automatically

### Method 2: URL (e.g. GitHub Releases)

1. Enter Boot Manager Mode
2. Open http://192.168.4.1 → OTA Update tab → URL section
3. Paste URL:
   ```
   https://github.com/YOUR_USER/YOUR_REPO/releases/latest/download/app.bin
   ```
4. Click Flash → device downloads, flashes, and reboots

### Method 3: API (curl)

```bash
# OTA from URL
curl -X POST http://192.168.4.1/api/ota/url \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com/firmware.bin"}'

# OTA upload binary
curl -X POST http://192.168.4.1/api/ota/upload \
  -H "Content-Type: application/octet-stream" \
  --data-binary @build/esp32_boot_manager.bin

# Switch partition
curl -X POST http://192.168.4.1/api/partition/switch

# Status
curl http://192.168.4.1/api/status
```

---

## API Reference

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/status` | Full device status (JSON) |
| GET | `/api/wifi/status` | WiFi state + saved SSID |
| POST | `/api/ota/url` | `{"url":"..."}` — OTA from URL |
| POST | `/api/ota/upload` | Binary body — OTA file upload |
| POST | `/api/partition/switch` | Switch to other OTA slot + reboot |
| POST | `/api/wifi/config` | `{"ssid":"...","password":"..."}` |
| POST | `/api/reboot` | Reboot device |

---

## WiFi Configuration

```bash
# Save credentials via API
curl -X POST http://192.168.4.1/api/wifi/config \
  -H "Content-Type: application/json" \
  -d '{"ssid":"MyNetwork","password":"MyPassword"}'
```

Credentials are stored in NVS namespace `wifi_config`. The device tries to
connect to STA on every boot. AP mode always stays active.

---

## Safety & Recovery

### Rollback Protection

The bootloader has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. New firmware
starts in `ESP_OTA_IMG_PENDING_VERIFY` state. The app must call
`esp_ota_mark_app_valid_cancel_rollback()` within the watchdog timeout,
otherwise the bootloader rolls back to the previous working partition.

### Boot Loop Detection

`boot_manager.c` tracks consecutive failures in NVS (`boot_state` namespace).
After **3 failures**, it forces Boot Manager Mode so you can reflash.

Counter is cleared on successful startup via `boot_manager_clear_counter()`.

### Watchdog

`CONFIG_ESP_TASK_WDT_TIMEOUT_S=30` — triggers panic/reset if any task hangs.

---

## Testing

### OTA Update Test

```bash
# 1. Build firmware
idf.py build

# 2. Check initial partition
curl http://192.168.4.1/api/status | python3 -m json.tool | grep running_partition

# 3. Upload new firmware
curl -X POST http://192.168.4.1/api/ota/upload \
  -H "Content-Type: application/octet-stream" \
  --data-binary @build/esp32_boot_manager.bin

# 4. Device reboots; verify partition switched
sleep 15
curl http://192.168.4.1/api/status | python3 -m json.tool | grep running_partition
```

### Boot Switch Test

```bash
# 1. Verify standby partition has valid firmware
curl http://192.168.4.1/api/status | python3 -m json.tool

# 2. Switch partition
curl -X POST http://192.168.4.1/api/partition/switch

# 3. After reboot (15s), verify
sleep 15
curl http://192.168.4.1/api/status | python3 -m json.tool | grep running_partition
```

### Failure Recovery Test

```bash
# Simulate boot loop by flashing a firmware that crashes immediately.
# After 3 reboots the device will enter Boot Manager automatically.
# Connect to AP and reflash with good firmware.
```

### Rollback Test

```bash
# Flash firmware that does NOT call esp_ota_mark_app_valid_cancel_rollback()
# The watchdog (30s) will expire and the bootloader rolls back.
# Monitor via: idf.py -p PORT monitor
```

---

## GitHub Actions

Every push to `main` or `testing`:
1. **Builds** firmware with `espressif/esp-idf-ci-action@v1`
2. **Uploads** build artifacts (retained 30 days)
3. **Creates a GitHub Release** (on `main` pushes) with all `.bin` files attached

The `app.bin` in releases can be used directly for OTA:
```
https://github.com/YOUR_USER/YOUR_REPO/releases/latest/download/app.bin
```

---

## Customization

### Change AP credentials

Edit `main/main.c`:
```c
wifi_manager_config_t wifi_cfg = {
    .ap_ssid     = "MyDevice",
    .ap_password = "mysecret",
    ...
};
```

### Change BOOT button GPIO

Edit `main/main.c`:
```c
#define BOOT_BUTTON_GPIO    GPIO_NUM_0   // change to your GPIO
```

### Change max boot failures

Edit `components/boot_manager/boot_manager.c`:
```c
#define BOOT_MAX_FAILURES   3   // change as needed
```

### App version

Edit `main/app_version.h`:
```c
#define APP_VERSION "1.0.0"
```

---

## License

MIT License. See LICENSE file.
