# UART Protocol Documentation

The ESP32 Bluetooth Audio Module communicates with the host system over standard Serial UART at **460800 baud** (8N1).

All commands sent to the module must be terminated with a newline character (`\n` or `\r\n`). Responses from the module are also line-terminated.

---

## ⚙️ Serial Settings
* **Baud Rate:** `460800`
* **Data Bits:** `8`
* **Parity:** `None`
* **Stop Bits:** `1`
* **Flow Control:** `None`

---

## 🤝 2-Step Handshake Mechanism
For potentially destructive operations (`CMD:REBOOT` and `CMD:RESET-DEFAULT`), a two-phase confirmation protocol is required to prevent accidental execution:

1. Host sends the initial command (e.g., `CMD:REBOOT`).
2. Module responds with confirmation request (`REQ:CONFIRM-REBOOT`).
3. Host must send execution confirmation (`CMD:GO-REBOOT`) within **2000 ms**.
4. If time expires without confirmation, the module responds with an error (`ERR:REBOOT_TIMEOUT`) and cancels the operation.

---

## 📋 UART Command Reference

### 1. Configuration & Settings

| Command | Response / Event | Description |
| :--- | :--- | :--- |
| `GET:NAME` | `NAME:<bt_name>` | Returns the configured Bluetooth broadcast name. |
| `SET:NAME=<new_name>` | `OK:NAME_SAVED_REBOOT_REQUIRED`<br>`ERR:INVALID_NAME` | Sets a new Bluetooth name (1–30 characters). Saved in NVS; requires reboot. |
| `GET:VOL` | `VOL:<volume>` | Returns current digital volume (range: 0–127). |
| `SET:VOL=<0-127>` | `OK:VOL=<volume>`<br>`ERR:INVALID_VOL` | Sets digital volume level (0–127). Stored in NVS. |
| `SET:GAIN=<0.0-2.0>` | `OK:GAIN=<gain>`<br>`ERR:INVALID_GAIN` | Sets digital audio gain factor (0.00 to 2.00). Stored in NVS. |
| `SET:MUTE=<0\|1>` | `OK:MUTE=<0\|1>`<br>`ERR:INVALID_MUTE` | Mutes (`1`) or unmutes (`0`) audio stream immediately. |
| `GET:MODE` | `MODE:<TX\|RX>` | Returns current operating mode (`TX` = Source / `RX` = Sink). |
| `SET:MODE=<TX\|RX>` | `OK:MODE_CHANGED_REBOOTING_TO_<MODE>`<br>`OK:MODE_ALREADY_SET`<br>`ERR:INVALID_MODE` | Changes operating mode. Device automatically restarts if mode changes. |

---

### 2. Status & System Information

| Command | Response / Event | Description |
| :--- | :--- | :--- |
| `GET:STATUS` | `STATUS:<CONNECTED\|DISCONNECTED>,MODE:<TX\|RX>` | Returns connection state and active Bluetooth mode. |
| `GET:INFO` | `INFO:FW_v1.0,RAM_FREE:<bytes>,MODE:<mode>` | Returns firmware version, free Heap memory (bytes), and mode. |
| `GET:RSSI` | `RSSI:<dBm>`<br>`ERR:NOT_CONNECTED` | Requests Bluetooth signal strength (RSSI delta) from connected peer. |
| `GET:TEMP` | `TEMP:<celsius>`<br>`ERR:TEMP_READ_FAILED` | Reads internal ESP32 chip core temperature in °C. |

---

### 3. Connection & Media Control

| Command | Response / Event | Description |
| :--- | :--- | :--- |
| `CMD:DISCONNECT` | `OK:DISCONNECTED`<br>`STATUS:DISCONNECTED`<br>`ERR:NOT_CONNECTED` | Actively disconnects current Bluetooth connection. |
| `GET:REMOTENAME` | `REMOTENAME:<device_name>`<br>`REMOTENAME:UNKNOWN`<br>`ERR:NOT_CONNECTED` | Returns the name of the connected remote device. |
| `CMD:PLAY` | `OK:PLAY` | Sends AVRCP `PLAY` pass-through command. |
| `CMD:PAUSE` | `OK:PAUSE` | Sends AVRCP `PAUSE` pass-through command. |
| `CMD:NEXT` | `OK:NEXT` | Sends AVRCP `NEXT TRACK` pass-through command. |
| `CMD:PREV` | `OK:PREV` | Sends AVRCP `PREVIOUS TRACK` pass-through command. |
| `GET:TRACK` | `TRACK:<Title>\|<Artist>\|<Album>`<br>`ERR:NOT_IN_RX_MODE` | Returns cached AVRCP track metadata (Sink/RX mode only). |

---

### 4. System Maintenance & Reset (2-Step Handshake)

| Command Step 1 | Module Prompt | Command Step 2 | Result Response | Description |
| :--- | :--- | :--- | :--- | :--- |
| `CMD:REBOOT` | `REQ:CONFIRM-REBOOT` | `CMD:GO-REBOOT` | `OK:REBOOTING` | System restart sequence. Must be confirmed within 2 sec. |
| `CMD:RESET-DEFAULT` | `REQ:CONFIRM-RESET` | `CMD:GO-RESET-DEFAULT` | `OK:FACTORY_RESET_DONE` | Restores all settings in NVS to defaults and restarts. |

*Timeout Error Response:* `ERR:REBOOT_TIMEOUT` or `ERR:RESET_TIMEOUT`

---

## 📡 Asynchronous System Events & Broadcasts

The module emits unsolicited status notifications over Serial when events occur:

* **Boot Ready:**
  `INFO:READY,NAME=<name>,MODE=<mode>`
* **Bluetooth Connection Established:**
  `STATUS:CONNECTED,MAC=XX:XX:XX:XX:XX:XX`
* **Bluetooth Disconnected:**
  `STATUS:DISCONNECTED`
* **Media Playback State:**
  `MEDIA:PLAYING` / `MEDIA:PAUSED`
* **Absolute Volume Update (from Peer Device):**
  `MEDIA:VOLUME,<percentage>`
* **Metadata Received:**
  `MEDIA:TITLE=<title>|ARTIST=<artist>|ALBUM=<album>`
* **AVRCP Key Events:**
  `EVENT:AVRCP,CMD=<PLAY|PAUSE|NEXT|PREV>`
