# Hardware Architecture & Pinout Specification

This project provides a dedicated, ultra-compact headless Internet Radio receiver utilizing the **ESP32-S3 SuperMini** paired with a **PCM5102A 32-bit I2S DAC**.

---

## 1. Components

1. **Main MCU: ESP32-S3 SuperMini**
   - SoC: `ESP32-S3FH4R2` (Dual-core Xtensa LX7 @ 160MHz / 240MHz, 4MB Quad Flash, 2MB Quad PSRAM embedded)
   - Power: 5V supplied via onboard USB Type-C connector
   - Control: Onboard BOOT button (`GPIO 0`, Active LOW with internal pull-up)
   - Form factor: Headless, ultra-compact miniature development board

2. **DAC Module: PCM5102A Mini Board**
   - Audio Interface: Standard I2S (Inter-IC Sound) 3-wire digital audio
   - Clock Mode: Internal PLL (no external MCLK / SCK required)
   - Audio Output: 3.5mm stereo line-out / headphone jack and analog L/R/AGND pin headers
   - SNR / Dynamic Range: 112 dB, supports 16-bit / 24-bit / 32-bit stereo audio up to 384 kHz

---

## 2. Pinout & Wiring

### (1) Power Connections (Parallel 5V Rail)
| ESP32-S3 SuperMini | PCM5102A DAC Board | Notes |
| :--- | :--- | :--- |
| **`5V`** | **`VIN`** | Direct 5V power from USB Type-C |
| **`GND`** | **`GND`** | Common Ground |

### (2) Digital Audio Interface (I2S)
| ESP32-S3 Pin | PCM5102A Pin | Signal Name | Description |
| :--- | :--- | :--- | :--- |
| **`GPIO 1`** | **`BCK`** | I2S Bit Clock (BCLK) | Clocks individual audio data bits |
| **`GPIO 2`** | **`LCK`** | I2S Word Select (WS / LRCK) | Left / Right audio channel framing |
| **`GPIO 3`** | **`DIN`** | I2S Serial Data (DOUT) | Serialized audio PCM sample data |
| *Not Connected* | **`SCK`** | Master Clock (MCLK) | Left open (internal PLL generates clock) |

### (3) User Controls
| Pin / Component | Function | Description |
| :--- | :--- | :--- |
| **`GPIO 0`** (Onboard BOOT) | Station Selector | Single-click cycles sequentially through saved radio stations in a loop |

---

## 3. PCM5102A Hardware Jumper Configuration

The PCM5102A mini board features solder bridge pads on the bottom layer to configure chip operating modes. Configure these pads before power-up:

| Jumper Pad | Target Setting | Configuration Details |
| :--- | :---: | :--- |
| **`H1L (FLT)`** | **`L` (GND)** | Normal latency FIR digital filter (sharp roll-off) |
| **`H2L (DEMP)`**| **`L` (GND)** | De-emphasis filter disabled |
| **`H3L (XSMT)`**| **`H` (3.3V)**| **Unmute (Soft mute disabled)**<br>*(Important: Per the PCM5102A datasheet, bridging to Low asserts hardware mute!)* |
| **`H4L (FMT)`** | **`L` (GND)** | Standard I2S format (MSB first, 1-bit delay) |

---

## 4. Hardware & RF Implementation Notes

- **Ceramic Antenna Sensitivity**:
  The SuperMini ESP32-S3 utilizes an unshielded miniature ceramic chip antenna located near the top edge of the PCB. Ensure no metallic enclosures or unshielded high-frequency emitters (e.g., active Bluetooth transmitters or noisy buck converters) are placed directly against the antenna, preserving maximum sensitivity for 2.4 GHz Wi-Fi beacon decoding.
- **Power Decoupling**:
  Ensure clean 5V power over the USB Type-C connection. If powering from an external 5V supply, adding a 100 µF electrolytic capacitor in parallel with a 0.1 µF ceramic capacitor across the 5V and GND pins is recommended for low-noise analog audio output.
