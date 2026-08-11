#include <Arduino.h>

#if !defined(CONFIG_IDF_TARGET_ESP32)
  #error "This code is intended exclusively for classic ESP32 (CONFIG_IDF_TARGET_ESP32)! Please check your target board selection in Arduino IDE."
#endif

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>

#include "esp_mac.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "driver/i2s_std.h"

// Definice pinů
#define I2S_BCLK_PIN    GPIO_NUM_12   // Společný Bit Clock
#define I2S_WS_PIN      GPIO_NUM_27   // Společný Word Select (LRCLK)
#define I2S_DOUT_PIN    GPIO_NUM_14   // Data Out (Vysílání do DAC / BT RX mód)
#define I2S_DIN_PIN     GPIO_NUM_13   // Data In (Příjem z ADC / BT TX mód)

// Globální i2s handle
static i2s_chan_handle_t tx_chan = NULL;
static i2s_chan_handle_t rx_chan = NULL;

// Media global variables
String trackTitle  = "Unknown Title";
String trackArtist = "Unknown Artist";
String trackAlbum  = "Unknown Album";

// FIX PRO ESP32 ARDUINO CORE 3.3.x
extern "C" bool btInUse(void) {
    return true;
}

#ifdef __cplusplus
extern "C" {
#endif
    // Nativní dekodér z ROM/HAL pro ESP32
    uint8_t temprature_sens_read(void);
#ifdef __cplusplus
}
#endif

// ==========================================
// KONFIGURACE A DEFINICE
// ==========================================
#define UART_BAUD_RATE      460800
#define CONFIRM_TIMEOUT_MS  2000
#define RING_BUFFER_SIZE    (64 * 1024) // 64 kB RingBuffer

// Defaultní hodnoty
const char* DEFAULT_MODE = "TX";
const uint8_t DEFAULT_VOLUME = 13; // 10 % z rozsahu 0-127
const float DEFAULT_GAIN = 1.00f;

// Globální stavové proměnné
Preferences prefs;
String btMode = "TX";
String btName = "";
uint8_t volume = DEFAULT_VOLUME;
float gain = DEFAULT_GAIN;
bool isMuted = false;
bool isConnected = false;

// Globální proměnné pro stav spojeného zařízení
uint8_t connected_bda[6] = {0};
String currentDeviceName = "";

// RingBuffer Handle pro audio stream
RingbufHandle_t audioRingBuffer = NULL;

// RTOS Task Handles
TaskHandle_t audioTaskHandle = NULL;

// Proměnné pro 2-fázový Handshake
enum PendingCmd { CMD_NONE, PENDING_REBOOT, PENDING_RESET };
PendingCmd pendingCmd = CMD_NONE;
unsigned long pendingCmdTime = 0;

// Prototypy
void processUartCommand(String cmd);
void initBluetoothStack();
void audioProcessingTask(void *pvParameters);
String generateDefaultName();


void initTempSensor() {
}

float getChipTemperature() {
    uint8_t raw = temprature_sens_read();
    if (raw == 0 || raw == 255) {
        return -999.0f; // Neplatná hodnota
    }
    // Přepočet z RAW hodnota na stupně Celsia pro ESP32
    return (float)(raw - 32) / 1.8f;
}

void clearAudioBuffer() {
    if (audioRingBuffer != NULL) {
        size_t dummySize = 0;
        while (void* item = xRingbufferReceiveUpTo(audioRingBuffer, &dummySize, 0, 2048)) {
            vRingbufferReturnItem(audioRingBuffer, item);
        }
    }
}

// De-inicializace a uvolnění I2S sběrnice
void i2s_stop_and_deinit() {
    if (tx_chan) {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }
    if (rx_chan) {
        i2s_channel_disable(rx_chan);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
    }
    Serial.println("I2S: Uvolněno");
}

// Inicializace pro TX (Vysílání do DAC / BT RX mód)
bool i2s_init_tx(uint32_t sample_rate) {
    i2s_stop_and_deinit();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;
    
    if (i2s_new_channel(&chan_cfg, &tx_chan, NULL) != ESP_OK) return false;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    if (i2s_channel_init_std_mode(tx_chan, &std_cfg) != ESP_OK) return false;
    if (i2s_channel_enable(tx_chan) != ESP_OK) return false;

    Serial.println("I2S: TX Inicializován (Master)");
    return true;
}

// Inicializace pro RX (Náběr z ADC / BT TX mód)
bool i2s_init_rx(uint32_t sample_rate) {
    i2s_stop_and_deinit();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;

    if (i2s_new_channel(&chan_cfg, NULL, &rx_chan) != ESP_OK) return false;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_DIN_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    if (i2s_channel_init_std_mode(rx_chan, &std_cfg) != ESP_OK) return false;
    if (i2s_channel_enable(rx_chan) != ESP_OK) return false;

    Serial.println("I2S: RX Inicializován (Master)");
    return true;
}

// -------------------------------------------------------------
// AVRCP Control Callback
// -------------------------------------------------------------
void bt_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_CT_METADATA_RSP_EVT: {
            uint8_t id = param->meta_rsp.attr_id;
            String text = String((char*)param->meta_rsp.attr_text, param->meta_rsp.attr_length);

            if (id == ESP_AVRC_MD_ATTR_TITLE)  trackTitle = text;
            if (id == ESP_AVRC_MD_ATTR_ARTIST) trackArtist = text;
            if (id == ESP_AVRC_MD_ATTR_ALBUM)  trackAlbum = text;

            if (id == ESP_AVRC_MD_ATTR_TITLE) {
                Serial.printf("MEDIA:TITLE=%s|ARTIST=%s|ALBUM=%s\n", 
                              trackTitle.c_str(), trackArtist.c_str(), trackAlbum.c_str());
            }
            break;
        }

        case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT:
            switch (param->play_status_rsp.play_status) {
                case ESP_AVRC_PLAYBACK_PLAYING:
                    Serial.println("MEDIA:PLAYING");
                    esp_avrc_ct_send_metadata_cmd(0, ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM);
                    break;
                case ESP_AVRC_PLAYBACK_PAUSED:
                case ESP_AVRC_PLAYBACK_STOPPED:
                    Serial.println("MEDIA:PAUSED");
                    break;
                default:
                    break;
            }
            break;

        case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
            if (param->psth_rsp.key_state == 0) {
                switch (param->psth_rsp.key_code) {
                    case ESP_AVRC_PT_CMD_PLAY:     Serial.println("EVENT:AVRCP,CMD=PLAY"); break;
                    case ESP_AVRC_PT_CMD_PAUSE:    Serial.println("EVENT:AVRCP,CMD=PAUSE"); break;
                    case ESP_AVRC_PT_CMD_FORWARD:  Serial.println("EVENT:AVRCP,CMD=NEXT"); break;
                    case ESP_AVRC_PT_CMD_BACKWARD: Serial.println("EVENT:AVRCP,CMD=PREV"); break;
                    default: break;
                }
            }
            break;

        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
                esp_avrc_ct_send_metadata_cmd(0, ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM);
            }
            break;

        default:
            break;
    }
}

// -------------------------------------------------------------
// AVRCP Target Callback
// -------------------------------------------------------------
void bt_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    if (event == ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT) {
        uint8_t volume_raw = param->set_abs_vol.volume; 
        uint8_t volume_pct = (volume_raw * 100) / 127;
        Serial.printf("MEDIA:VOLUME,%d\n", volume_pct);
    }
}

// ==========================================
// A2DP DATA & CONNECTION CALLBACKS
// ==========================================
String bdaToString(const uint8_t *bda) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return String(buf);
}

void bt_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    if (event == ESP_A2D_CONNECTION_STATE_EVT) {
        clearAudioBuffer();
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            isConnected = true;
            memcpy(connected_bda, param->conn_stat.remote_bda, 6);
            currentDeviceName = "";

            String macStr = bdaToString(connected_bda);
            Serial.println("STATUS:CONNECTED,MAC=" + macStr);

            if (btMode == "RX") {
                esp_avrc_ct_send_get_play_status_cmd(0);
            }

        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            isConnected = false;
            memset(connected_bda, 0, 6);
            currentDeviceName = "";
            Serial.println("STATUS:DISCONNECTED");
        }
    }
}

int32_t bt_a2dp_data_cb(uint8_t *data, int32_t len) {
    if (data == NULL || len <= 0) return 0;

    if (isMuted) {
        clearAudioBuffer();
        memset(data, 0, len);
        return len;
    }

    size_t bytesRead = 0;
    if (audioRingBuffer != NULL && !isMuted) {
        void* item = xRingbufferReceiveUpTo(audioRingBuffer, &bytesRead, 0, len);
        if (item != NULL) {
            memcpy(data, item, bytesRead);
            vRingbufferReturnItem(audioRingBuffer, item);

            if (gain != 1.00f) {
                int16_t *samples = (int16_t *)data;
                size_t sampleCount = bytesRead / 2;
                for (size_t i = 0; i < sampleCount; i++) {
                    int32_t sample = (int32_t)(samples[i] * gain);
                    if (sample > 32767) sample = 32767;
                    if (sample < -32768) sample = -32768;
                    samples[i] = (int16_t)sample;
                }
            }
        } else {
            memset(data, 0, len);
            bytesRead = len;
        }
    } else {
        memset(data, 0, len);
        bytesRead = len;
    }
    return bytesRead;
}

void bt_a2dp_sink_data_cb(const uint8_t *data, uint32_t len) {
    if (data == NULL || len == 0 || isMuted) return;

    if (gain != 1.00f) {
        int16_t *samples = (int16_t *)data;
        size_t sampleCount = len / 2;
        for (size_t i = 0; i < sampleCount; i++) {
            int32_t sample = (int32_t)(samples[i] * gain);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            samples[i] = (int16_t)sample;
        }
    }

    if (audioRingBuffer != NULL) {
        // Zvýšen timeout na 10 ms pro prevenci ztráty dat
        xRingbufferSend(audioRingBuffer, (void*)data, len, pdMS_TO_TICKS(10));
    }
}

// ==========================================
// POMOCNÉ SPRÁVY PAMĚTI A SHODY DEFAULTU
// ==========================================
String generateDefaultName() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char nameBuf[32];
    snprintf(nameBuf, sizeof(nameBuf), "VTomRadio-MaSo-%02X%02X%02X", mac[3], mac[4], mac[5]);
    return String(nameBuf);
}

void loadNVSConfig() {
    prefs.begin("bt_config", false);
    btMode = prefs.getString("mode", DEFAULT_MODE);
    btName = prefs.getString("bt_name", generateDefaultName());
    volume = prefs.getUChar("volume", DEFAULT_VOLUME);
    gain = prefs.getFloat("gain", DEFAULT_GAIN);
    prefs.end();
}

void resetToDefaults() {
    prefs.begin("bt_config", false);
    prefs.clear();
    prefs.putString("mode", DEFAULT_MODE);
    prefs.putString("bt_name", generateDefaultName());
    prefs.putUChar("volume", DEFAULT_VOLUME);
    prefs.putFloat("gain", DEFAULT_GAIN);
    prefs.end();
}

// ==========================================
// INICIALIZACE BT STACKU
// ==========================================
void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
        case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
            if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
                currentDeviceName = String((char *)param->read_rmt_name.rmt_name);
            }
            break;

        case ESP_BT_GAP_READ_RSSI_DELTA_EVT:
            if (param->read_rssi_delta.stat == ESP_BT_STATUS_SUCCESS) {
                Serial.printf("RSSI:%d\n", param->read_rssi_delta.rssi_delta);
            } else {
                Serial.println("ERR:RSSI_FAILED");
            }
            break;

        case ESP_BT_GAP_CFM_REQ_EVT:
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;

        default:
            break;
    }
}

void initBluetoothStack() {
    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        Serial.printf("ERR: nvs_flash_init failed: %s\n", esp_err_to_name(ret));
    }

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_bt_controller_disable();
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        esp_bt_controller_deinit();
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.mode = ESP_BT_MODE_CLASSIC_BT;
    
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        Serial.printf("ERR: controller_init failed: %s\n", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        Serial.printf("ERR: controller_enable failed: %s\n", esp_err_to_name(ret));
        return;
    }

    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        ret = esp_bluedroid_init();
        if (ret != ESP_OK) {
            Serial.printf("ERR: bluedroid_init failed: %s\n", esp_err_to_name(ret));
            return;
        }
    }

    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        ret = esp_bluedroid_enable();
        if (ret != ESP_OK) {
            Serial.printf("ERR: bluedroid_enable failed: %s\n", esp_err_to_name(ret));
            return;
        }
    }

    esp_bt_dev_set_device_name(btName.c_str());
    esp_bt_gap_register_callback(bt_app_gap_cb);

    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(bt_avrc_ct_cb);
    esp_avrc_tg_init();
    esp_avrc_tg_register_callback(bt_avrc_tg_cb);

    if (btMode == "TX") {
        esp_a2d_register_callback(bt_a2dp_cb);
        esp_a2d_source_init();
        esp_a2d_source_register_data_callback(bt_a2dp_data_cb);
    } else {
        esp_a2d_register_callback(bt_a2dp_cb);
        esp_a2d_sink_init();
        esp_a2d_sink_register_data_callback(bt_a2dp_sink_data_cb);
    }

    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t io_cap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &io_cap, sizeof(uint8_t));

    ret = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    if (ret != ESP_OK) {
        Serial.printf("ERR: set_scan_mode failed: %s\n", esp_err_to_name(ret));
    } else {
        Serial.println("INFO: BT Scan mode set to CONNECTABLE & DISCOVERABLE");
    }
}

// ==========================================
// THREAD PRO AUDIO / 2. JÁDRO (CORE 1)
// ==========================================
void audioProcessingTask(void *pvParameters) {
    audioRingBuffer = xRingbufferCreate(RING_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);

    if (audioRingBuffer == NULL) {
        Serial.println("ERR:RINGBUFFER_FAILED");
    }

    size_t bytesRead = 0;
    size_t bytesWritten = 0;
    
    // OPRAVA: static zabrání alokaci 2 KB na stacku tasku!
    static uint8_t i2sRxBuf[2048]; 

    for (;;) {
        if (isMuted) {
            clearAudioBuffer();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (btMode == "RX" && tx_chan != NULL) {
            void* item = xRingbufferReceiveUpTo(audioRingBuffer, &bytesRead, pdMS_TO_TICKS(10), 2048);
            if (item != NULL) {
                i2s_channel_write(tx_chan, item, bytesRead, &bytesWritten, portMAX_DELAY);
                vRingbufferReturnItem(audioRingBuffer, item);
            }
        } 
        else if (btMode == "TX" && rx_chan != NULL) {
            if (i2s_channel_read(rx_chan, i2sRxBuf, sizeof(i2sRxBuf), &bytesRead, pdMS_TO_TICKS(10)) == ESP_OK) {
                if (bytesRead > 0 && audioRingBuffer != NULL) {
                    // OPRAVA: Timeout nastaven na 10 ms pro ochranu proti ztrátě dat
                    xRingbufferSend(audioRingBuffer, i2sRxBuf, bytesRead, pdMS_TO_TICKS(10));
                }
            }
        } 
        else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// ==========================================
// KONTROLA TIMEOUTU HANDSHAKE
// ==========================================
void checkHandshakeTimeout() {
    if (pendingCmd != CMD_NONE) {
        if (millis() - pendingCmdTime > CONFIRM_TIMEOUT_MS) {
            if (pendingCmd == PENDING_REBOOT) {
                Serial.println("ERR:REBOOT_TIMEOUT");
            } else if (pendingCmd == PENDING_RESET) {
                Serial.println("ERR:RESET_TIMEOUT");
            }
            pendingCmd = CMD_NONE;
        }
    }
}

// ==========================================
// PARSOVÁNÍ A ZPRACOVÁNÍ UART PŘÍKAZŮ (CORE 0)
// ==========================================
void processUartCommand(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;

    if (cmd == "GET:NAME") {
        Serial.println("NAME:" + btName);
    }
    else if (cmd.startsWith("SET:NAME=")) {
        String newName = cmd.substring(9);
        if (newName.length() >= 1 && newName.length() <= 30) {
            prefs.begin("bt_config", false);
            prefs.putString("bt_name", newName);
            prefs.end();
            btName = newName;
            Serial.println("OK:NAME_SAVED_REBOOT_REQUIRED");
        } else {
            Serial.println("ERR:INVALID_NAME");
        }
    }
    else if (cmd == "GET:VOL") {
        Serial.println("VOL:" + String(volume));
    }
    else if (cmd.startsWith("SET:VOL=")) {
        int val = cmd.substring(8).toInt();
        if (val >= 0 && val <= 127) {
            volume = (uint8_t)val;
            prefs.begin("bt_config", false);
            prefs.putUChar("volume", volume);
            prefs.end();
            Serial.println("OK:VOL=" + String(volume));
        } else {
            Serial.println("ERR:INVALID_VOL");
        }
    }
    else if (cmd.startsWith("SET:GAIN=")) {
        float val = cmd.substring(9).toFloat();
        if (val >= 0.0f && val <= 2.0f) {
            gain = val;
            prefs.begin("bt_config", false);
            prefs.putFloat("gain", gain);
            prefs.end();
            Serial.println("OK:GAIN=" + String(gain, 2));
        } else {
            Serial.println("ERR:INVALID_GAIN");
        }
    }
    else if (cmd.startsWith("SET:MUTE=")) {
        int val = cmd.substring(9).toInt();
        if (val == 0 || val == 1) {
            isMuted = (val == 1);
            Serial.println("OK:MUTE=" + String(isMuted ? 1 : 0));
        } else {
            Serial.println("ERR:INVALID_MUTE");
        }
    }
    else if (cmd == "GET:STATUS") {
        Serial.println("STATUS:" + String(isConnected ? "CONNECTED" : "DISCONNECTED") + ",MODE:" + btMode);
    }
    else if (cmd == "CMD:DISCONNECT") {
        if (isConnected) {
            clearAudioBuffer();
            if (btMode == "TX") {
                esp_a2d_source_disconnect(connected_bda);
            } else {
                esp_a2d_sink_disconnect(connected_bda);
            }
            isConnected = false;
            Serial.println("OK:DISCONNECTED");
            Serial.println("STATUS:DISCONNECTED");
        } else {
            Serial.println("ERR:NOT_CONNECTED");
        }
    }
    else if (cmd == "GET:REMOTENAME" || cmd == "GETREMOTENAME") {
        if (isConnected) {
            if (currentDeviceName.length() > 0) {
                Serial.println("REMOTENAME:" + currentDeviceName);
            } else {
                esp_bt_gap_read_remote_name(connected_bda);
                Serial.println("REMOTENAME:UNKNOWN");
            }
        } else {
            Serial.println("ERR:NOT_CONNECTED");
        }
    }
    else if (cmd == "CMD:PLAY") {
        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_PRESSED);
        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_RELEASED);
        Serial.println("OK:PLAY");
    }
    else if (cmd == "CMD:PAUSE") {
        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_PRESSED);
        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_RELEASED);
        Serial.println("OK:PAUSE");
    }
    else if (cmd == "CMD:NEXT") {
        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
        Serial.println("OK:NEXT");
    }
    else if (cmd == "CMD:PREV") {
        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
        Serial.println("OK:PREV");
    }
    else if (cmd == "GET:MODE") {
        Serial.println("MODE:" + btMode);
    }
    else if (cmd.startsWith("SET:MODE=")) {
        String newMode = cmd.substring(9);
        newMode.toUpperCase();
        if (newMode == "TX" || newMode == "RX") {
            if (newMode == btMode) {
                Serial.println("OK:MODE_ALREADY_SET");
            } else {
                prefs.begin("bt_config", false);
                prefs.putString("mode", newMode);
                prefs.end();
                Serial.println("OK:MODE_CHANGED_REBOOTING_TO_" + newMode);
                i2s_stop_and_deinit();
                Serial.flush();
                delay(100);
                ESP.restart();
            }
        } else {
            Serial.println("ERR:INVALID_MODE");
        }
    }
    else if (cmd == "GET:TRACK" || cmd == "GET:METADATA") {
        if (btMode == "RX") {
            Serial.printf("TRACK:%s|%s|%s\n", trackTitle.c_str(), trackArtist.c_str(), trackAlbum.c_str());
        } else {
            Serial.println("ERR:NOT_IN_RX_MODE");
        }
    }
    else if (cmd == "GET:INFO") {
        Serial.printf("INFO:FW_v1.0,RAM_FREE:%u,MODE:%s\n", ESP.getFreeHeap(), btMode.c_str());
    }
    else if (cmd == "GET:RSSI") {
        if (isConnected) {
            esp_bt_gap_read_rssi_delta(connected_bda);
        } else {
            Serial.println("ERR:NOT_CONNECTED");
        }
    }
    else if (cmd == "GET:TEMP") {
        float temp = getChipTemperature();
        if (temp > -100.0f) {
            Serial.printf("TEMP:%.1f\n", temp);
        } else {
            Serial.println("ERR:TEMP_READ_FAILED");
        }
    }
    else if (cmd == "CMD:REBOOT") {
        pendingCmd = PENDING_REBOOT;
        pendingCmdTime = millis();
        Serial.println("REQ:CONFIRM-REBOOT");
    }
    else if (cmd == "CMD:GO-REBOOT") {
        if (pendingCmd == PENDING_REBOOT) {
            pendingCmd = CMD_NONE;
            Serial.println("OK:REBOOTING");
            Serial.flush();
            delay(100);
            ESP.restart();
        } else {
            Serial.println("ERR:REBOOT_TIMEOUT");
        }
    }
    else if (cmd == "CMD:RESET-DEFAULT") {
        pendingCmd = PENDING_RESET;
        pendingCmdTime = millis();
        Serial.println("REQ:CONFIRM-RESET");
    }
    else if (cmd == "CMD:GO-RESET-DEFAULT") {
        if (pendingCmd == PENDING_RESET) {
            pendingCmd = CMD_NONE;
            resetToDefaults();
            Serial.println("OK:FACTORY_RESET_DONE");
            Serial.flush();
            delay(100);
            ESP.restart();
        } else {
            Serial.println("ERR:RESET_TIMEOUT");
        }
    }
    else {
        Serial.println("ERR:UNKNOWN_COMMAND");
    }
}

// ==========================================
// ARDUINO MAIN SETUP & LOOP (CORE 0)
// ==========================================
void setup() {
    delay(500);
    Serial.begin(UART_BAUD_RATE);
    // Nastavení krátkého neblokujícího timeoutu pro čtení z UART
    Serial.setTimeout(50); 
    delay(500);

    initTempSensor();
    loadNVSConfig();

    xTaskCreatePinnedToCore(
        audioProcessingTask,
        "AudioTask",
        8192,
        NULL,
        2,
        &audioTaskHandle,
        1
    );

    if (btMode == "RX") {
        i2s_init_tx(44100);
    } else {
        i2s_init_rx(44100);
    }

    initBluetoothStack();

    Serial.println("INFO:READY,NAME=" + btName + ",MODE=" + btMode);
}

void loop() {
    if (Serial.available() > 0) {
        String inputStr = Serial.readStringUntil('\n');
        processUartCommand(inputStr);
    }

    checkHandshakeTimeout();
}