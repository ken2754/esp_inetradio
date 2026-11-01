# ESP32-S3 SuperMini Internet Radio

An ultra-compact, headless, high-fidelity Internet Radio receiver built for the **ESP32-S3 SuperMini** (`ESP32-S3FH4R2`: 4MB Flash / 2MB Quad PSRAM) and **PCM5102A 32-bit I2S DAC**.

Stream MP3 and AAC web radio stations directly to your headphones, active speakers, or amplifier with zero display overhead, responsive web management, and instant hardware station switching.

---

## Features

- **Multi-AP Wi-Fi Auto-Connection & Signal Prioritization**:
  - Store up to 5 registered Wi-Fi networks in non-volatile storage (NVS).
  - Automatically scans all channels on boot and connects to the registered access point with the **highest signal strength (strongest RSSI)**.
  - Automatically falls back to an emergency SoftAP (**`192.168.4.1`**, SSID: `ESP32-InetRadio`) if no registered network is reachable.
- **Stutter-Free Streaming & Dual-Codec Decoding**:
  - Leverages 2MB integrated PSRAM to allocate a **256 KB audio ring buffer**, absorbing network jitter and temporary packet loss.
  - Automatic decoding for both **MP3** and **AAC** streams using Espressif's optimized audio codec libraries.
  - Real-time **ICY metadata extraction** to display song title and artist information on the Web UI.
- **High-Quality I2S Digital Audio Output**:
  - Outputs directly to the PCM5102A 32-bit DAC via standard 3-wire I2S (`GPIO 1`, `GPIO 2`, `GPIO 3`).
  - Uses the PCM5102A's internal low-jitter PLL (no master clock `MCLK` / `SCK` pin needed).
  - Perceptual quadratic digital volume scaling (0% to 100%).
- **Responsive Web Configuration Interface**:
  - Fully embedded, self-contained single-page application (no internet connection or external CDN required).
  - Accessible via mDNS at **`http://inetradio.local`** or the assigned local IP.
  - Live playback dashboard with real-time track info, audio format, buffer utilization, and volume slider.
  - Graphical Wi-Fi network scanner and multi-AP configuration.
  - Station editor to add, reorder, or test custom stream URLs.
- **Curated SomaFM Presets**:
  - Preloaded out-of-the-box with 6 popular SomaFM channels:
    - *Groove Salad*, *Drone Zone*, *Secret Agent*, *DEF CON Radio*, *Lush*, *Indie Pop Rocks!*
  - Includes a one-click "Reset to SomaFM Presets" function.
- **Physical Button Station Cycling**:
  - Press the onboard **BOOT button (`GPIO 0`)** to loop sequentially through saved radio stations without needing a smartphone or PC.

---

## Hardware Architecture

```
                      +-----------------------------+
                      |   5V USB Type-C Power       |
                      +--------------+--------------+
                                     |
              +----------------------+----------------------+
              | 5V                                          | 5V
              v                                             v
+----------------------------+              +----------------------------+
|     ESP32-S3 SuperMini     |              |     PCM5102A I2S DAC       |
|                            |  GPIO 1: BCK |                            |
| Dual-core LX7 @ 160MHz     +------------->| BCK (Bit Clock)            |
| 4MB Flash / 2MB Quad PSRAM |  GPIO 2: LCK |                            |
|                            +------------->| LCK (Word Select / WS)     |
| Onboard BOOT Button:       |  GPIO 3: DIN |                            |
| GPIO 0 (Station Cycling)   +------------->| DIN (Data Out)             |
|                            |              |                            |
| 2.4 GHz Wi-Fi / SoftAP     |              | 3.5mm Headphone Jack       |
+----------------------------+              | Stereo Line-Out (L/R/AGND) |
                                            +----------------------------+
```

### Pinout
| ESP32-S3 SuperMini | PCM5102A Pin | Signal | Description |
| :--- | :--- | :--- | :--- |
| **`5V`** | **`VIN`** | Power | 5V DC power from USB-C |
| **`GND`** | **`GND`** | Ground | Common system ground |
| **`GPIO 1`** | **`BCK`** | I2S BCLK | Bit Clock |
| **`GPIO 2`** | **`LCK`** | I2S WS | Word Select (Left/Right Clock) |
| **`GPIO 3`** | **`DIN`** | I2S DOUT | Serial Audio Data |
| *Not Connected* | **`SCK`** | Master Clock | Left floating (Internal PLL mode) |
| **`GPIO 0`** | Onboard Button | BOOT | Single-click cycles stations |

> [!IMPORTANT]
> **PCM5102A Solder Jumper Configuration**:
> On the bottom of the PCM5102A board, solder bridge pads must be set as follows:
> - **`H1L (FLT)`**: `L` (Normal latency FIR filter)
> - **`H2L (DEMP)`**: `L` (De-emphasis OFF)
> - **`H3L (XSMT)`**: **`H` (Unmute)** *(Note: Bridge to `H` (3.3V). Grounding this pin asserts hardware mute!)*
> - **`H4L (FMT)`**: `L` (Standard I2S format)

For detailed hardware schematics and specifications, see [HARDWARE.md](HARDWARE.md).

---

## Getting Started

### Prerequisites
- [ESP-IDF v5.3](https://docs.espressif.com/projects/esp-idf/en/v5.3.5/esp32s3/get-started/) or later.
- Standard USB Type-C data cable connected to the ESP32-S3 SuperMini.

### Build and Flash

1. **Clone the repository**:
   ```bash
   git clone https://github.com/your-username/esp_inetradio.git
   cd esp_inetradio
   ```

2. **Set up the ESP-IDF environment**:
   ```bash
   source ~/esp/esp-idf/export.sh
   ```

3. **Build the firmware**:
   ```bash
   idf.py build
   ```

4. **Flash to the ESP32-S3 and open serial monitor**:
   ```bash
   idf.py -p /dev/ttyACM0 flash monitor
   ```

---

## First-Time Wi-Fi Setup

1. **Power on the device**:
   If no configured Wi-Fi network is detected, the device automatically starts in SoftAP mode.
2. **Connect to SoftAP**:
   - SSID: **`ESP32-InetRadio`**
   - Password: *None (Open network)*
   - Default IP: **`192.168.4.1`**
3. **Open Configuration Portal**:
   Navigate to **`http://192.168.4.1`** in your browser.
4. **Configure Wi-Fi**:
   - Under **Wi-Fi AP Settings**, click **Scan Nearby APs** to discover your local networks.
   - Enter your network SSID and password (up to 5 slots available).
   - Click **Save Wi-Fi & Reconnect**.
5. **Listen to Radio**:
   The radio connects to the strongest configured network and immediately begins streaming the first SomaFM station through your connected headphones or speakers.

---

## REST API Reference

The device provides an HTTP REST API on port 80:

| Endpoint | Method | Description | Example Payload |
| :--- | :--- | :--- | :--- |
| `/api/status` | `GET` | Get live Wi-Fi, player, codec, and track status | — |
| `/api/config` | `GET` | Retrieve saved stations and Wi-Fi slots | — |
| `/api/volume` | `POST` | Set digital playback volume (0–100%) | `{"volume": 65}` |
| `/api/station/select` | `POST` | Select station by index or advance to next | `{"index": 2}` or `{"next": true}` |
| `/api/stations` | `POST` | Update station list or reset to defaults | `{"stations": [...], "current_station": 0}` |
| `/api/wifi` | `POST` | Update saved Wi-Fi AP credentials | `{"wifi_aps": [...]}` |
| `/api/wifi/scan` | `GET` | Perform active Wi-Fi scan and return APs | — |

---

## Project Structure

```
esp_inetradio/
├── CMakeLists.txt          # Top-level CMake project definition
├── HARDWARE.md             # Hardware architecture and pinout specification
├── README.md               # Project documentation
├── partitions.csv          # 4MB flash partition table (3MB factory app partition)
├── sdkconfig.defaults      # ESP32-S3 SuperMini optimized build configuration
└── main/
    ├── CMakeLists.txt      # Component build rules & web asset embedding
    ├── idf_component.yml   # ESP-IDF component dependencies (audio codec, mdns, button)
    ├── app_config.h/.c     # NVS persistence & SomaFM station presets
    ├── audio_player.h/.c   # I2S driver, PSRAM ring buffer, HTTP stream & ICY parser
    ├── wifi_manager.h/.c   # Wi-Fi active scan, RSSI selection & SoftAP fallback
    ├── web_server.h/.c     # Embedded HTTP server & REST API handlers
    ├── main.c              # Application lifecycle & hardware BOOT button handler
    └── web/
        └── index.html      # Responsive web management interface
```

---

## License

This project is licensed under the [MIT License](LICENSE).
