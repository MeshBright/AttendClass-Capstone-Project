# AttendClass: Edge-Computing Attendance Architecture 🚀

![ESP32](https://img.shields.io/badge/ESP32-Hardware-blue)
![FreeRTOS](https://img.shields.io/badge/OS-FreeRTOS-orange)
![C++](https://img.shields.io/badge/Language-C%2B%2B-green)

AttendESP is a standalone, offline-first edge-computing attendance server built for the Federal University of Technology, Minna. It physically eliminates proxy attendance using strict Biometric Two-Factor Authentication (2FA) and Bluetooth Low Energy (BLE) geofencing.

> **🔗 Full Stack Integration:** > This repository contains the **Embedded Hardware & C++ Firmware**. 
> The Node.js/Next.js Cloud Dashboard and database architecture can be found in our **[Backend Repository](https://github.com/Ayomide-16/AttendClass.git)**.

## 🧠 Engineering Architecture
This project abandons standard single-core microcontroller loops in favor of a multi-threaded **FreeRTOS** architecture to maximize silicon efficiency.

1. **Dual-Core Processing:** Heavy background I/O (NTP Wi-Fi syncs, saving attendance logs in SD card and direct-to-cloud CSV streaming) are pinned to Core 0. This isolates the UI and biometric polling on Core 1, slashing user-facing latency by 79%.
2. **Hardware Isolation:** SPI, I2C, and UART protocols are explicitly routed and isolated to prevent bus collisions across the RFID, OLED, and Biometric sensors.
3. **RF Resilience:** Implemented dynamic radio-toggling to safely share the ESP32's single physical antenna between the Wi-Fi stack and the BLE server without triggering Watchdog resets.

## ⚙️ Features
* **100% Offline Capability:** Saves dynamically generated CSV logs to local MicroSD storage using `sdMutex` and FreeRTOS queues.
* **Strict 2FA Pipeline:** Requires sequential Master Card, Student RFID, and Optical Fingerprint matches for physical check-in.
* **BLE "Hit-and-Run" Geofence:** Calculates phone RSSI distance to mathematically block students from checking in outside classroom radius.
* **Zero-RAM HTTP Streaming:** Streams large CSV logs directly from the SD card to the cloud backend, bypassing fatal heap fragmentation.

## 🛠️ Hardware Bill of Materials (BOM)
* DOIT ESP32 DevKit V1
* AS608 Optical Fingerprint Sensor (UART)
* MFRC522 RFID Reader (VSPI)
* MicroSD Card Module (HSPI)
* SH1106 I2C OLED Display

## 👨‍💻 Contributors
* **[Zang Methushael]** - Lead Embedded Systems Engineer (Hardware/Firmware)
* **[Ayomide Samuel]** - Lead Software Engineer (Cloud Backend & Dashboard)
