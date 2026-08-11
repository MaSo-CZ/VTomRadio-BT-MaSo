# VTomRadio-BT-MaSo

Bluetooth module firmware/code for the eRadio VTomRadio project based on ESP32.

---

### ⚠️ Development / Work in Progress
This project is currently in the **initial development and testing phase** and is **not intended for final deployment or production use**.

> **Important Notice:** The communication protocol, UART commands, and data exchange style are under active development and **are subject to major changes**. Do not take current implementation details as final.

---

## 📡 Hardware & Communication

- **Core Microcontroller:** Classic ESP32 (`CONFIG_IDF_TARGET_ESP32`)
- **Primary Interface:** UART communication (Plain text Request/Response commands).
- **Development & Testing Interface:** UART over USB is currently used for testing and debugging via the **Arduino IDE Serial Monitor** at a baud rate of `460800`.
- **Audio Interface:** I2S bus (Master mode) for audio streaming in both TX and RX Bluetooth modes.

---

## 🛠️ Development Environment & Requirements

This firmware is developed and built using the following stack:

- **IDE:** Arduino IDE `2.3.10`
- **Board Package:** `esp32` by Espressif Systems v`3.3.11` (includes underlying ESP-IDF stack v5.x)
- **Target Chip:** Classic ESP32 (ESP32-WROOM / ESP32-D0WD core)

---

## ⚖️ License & Terms of Use

This project is licensed under the **GNU General Public License v3.0 (GPLv3)**.

### What you CAN do:
- 🟢 **Use it freely:** Download, run, and use this firmware for personal, educational, or commercial purposes.
- 🟢 **Modify it:** Alter the source code to suit your specific needs.
- 🟢 **Distribute it:** Share the original or modified source code with others.

### What you MUST do:
- 🔵 **Keep it Open Source:** If you distribute modified or derivative versions of this software, **you must make the full source code publicly available** under the same GPLv3 license.
- 🔵 **Include License & Copyright:** You must retain the original copyright notice and license text in all copies or substantial portions of the code.

### What you CANNOT do:
- 🔴 **No Proprietary / Closed Source Distribution:** You **cannot** take this code, modify it, and distribute it as a closed-source or proprietary product without providing the source code upon request.

---

*For full license terms, see the [LICENSE](LICENSE) file in this repository.*
