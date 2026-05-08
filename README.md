# 🫀 Portable ECG and Heart Rate Logger

A low-cost, portable, Arduino-based ECG monitoring and heart rate logging system designed for rural and resource-limited healthcare environments.

---

## 📋 Project Overview

This device captures real-time ECG signals from the body using surface electrodes, processes the signals on an Arduino Nano, displays live waveforms on an OLED screen, and logs timestamped data to a microSD card — all without requiring internet connectivity.

---

## 🔧 Hardware Components

| Component | Purpose |
|---|---|
| Arduino Nano | Microcontroller / central processing unit |
| AD8232 ECG Sensor | Amplifies and filters bioelectric heart signals |
| ADS1115 (16-bit ADC) | High-resolution analog-to-digital conversion |
| SSD1306 OLED (128×32) | Real-time ECG waveform and BPM display |
| DS3231 RTC Module | Accurate timestamping for logged data |
| MicroSD Card Module | Long-term CSV data logging |
| ECG Electrodes (×3) | RA, LA, RL lead configuration |
| Li-ion / LiPo Battery | Portable power supply |
| TP4056 Charging Module | Battery charging circuit |

---

## 🔌 Wiring / Pin Connections

### I2C Bus (shared by OLED + ADS1115 + DS3231)
| Signal | Arduino Nano Pin |
|---|---|
| SDA | A4 |
| SCL | A5 |

### SPI – SD Card Module
| Signal | Arduino Nano Pin |
|---|---|
| CS (Chip Select) | D10 |
| MOSI | D11 |
| MISO | D12 |
| SCK | D13 |

### AD8232 ECG Sensor
| AD8232 Pin | Arduino Nano Pin |
|---|---|
| OUTPUT | ADS1115 A0 |
| LO+ (Leads-Off +) | D4 |
| LO- (Leads-Off -) | D5 |
| 3.3V | 3.3V |
| GND | GND |

---

## 📦 Required Libraries

Install via **Arduino IDE → Tools → Manage Libraries**:

- `Adafruit GFX Library`
- `Adafruit SSD1306`
- `Adafruit ADS1X15`
- `TimerOne`
- `RTClib` (by Adafruit)
- `SD` (built-in with Arduino IDE)

---

## 🚀 How to Upload and Use

1. Clone or download this repository.
2. Open `ECG_HeartRate_Logger/ECG_HeartRate_Logger.ino` in Arduino IDE.
3. Install all required libraries (see above).
4. Select **Board: Arduino Nano** and the correct COM port.
5. Click **Upload**.
6. Open **Serial Plotter** (`Tools → Serial Plotter`) to visualize the ECG signal live.

---

## 📁 Data Logging Format

Data is saved to `ecglog.csv` on the SD card in CSV format:

```
Timestamp,Millis,Voltage_V,BPM,Status
2025-06-01 14:32:01,12345,1.6523,72,OK
2025-06-01 14:32:02,13345,0.0000,--,LEADS_OFF
```

---

## ⚙️ Key Features

- **Real-time ECG waveform** displayed on OLED
- **BPM calculation** using adaptive R-peak detection (Pan–Tompkins inspired)
- **Leads-off detection** with on-screen warning
- **RTC timestamping** of all logged data
- **SD card logging** in CSV format for later review
- **Serial Plotter** compatible output for PC visualization
- **Fully offline** — no internet or cloud required
- **Low cost** — under ₹1500 in components

---

## 📐 System Architecture

```
Electrodes (RA, LA, RL)
        │
    AD8232 ECG Front-End
    (Amplify + Filter)
        │
    ADS1115 ADC (16-bit)
        │
    Arduino Nano
    ├── R-peak Detection → BPM
    ├── SSD1306 OLED Display
    ├── DS3231 RTC (timestamps)
    └── SD Card (CSV logging)
```

---

## 🏥 Use Case

Targeted at **rural and primary healthcare settings** where clinical-grade ECG machines are unavailable. The device can be used by community health workers to screen for basic cardiac abnormalities and store records for later consultation with a doctor.

---

## 🔮 Future Improvements

- Bluetooth (HC-05) / Wi-Fi (ESP8266) transmission to smartphone
- Multi-lead ECG support
- AI-based arrhythmia detection
- Cloud storage integration
- Rechargeable battery with fuel gauge

---

## 👥 Team

| Name | Registration No. |
|---|---|
| Divam Sharma | 12516865 |
| Hita Khera | 12517195 |
| Meda Susheel Chandra Krishna | 12517639 |
| Aman Kumar Aditya | 12517923 |

**Lovely Professional University**  
School of Computer Science and Engineering

---

## 📄 License

This project is open-source for educational and non-commercial use.
