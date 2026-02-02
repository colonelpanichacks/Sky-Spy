/*
 * Sky-Spy Dual-Band RemoteID Scanner
 * 
 * Supports ESP32-C5 (dual-band 2.4GHz + 5GHz WiFi 6) and ESP32-S3 (2.4GHz only)
 * Detects drones broadcasting RemoteID via WiFi (NAN/Beacon) and Bluetooth LE
 * 
 * For ESP32-C5: Seamless dual-band scanning with fast channel hopping
 * For ESP32-S3: Single-band 2.4GHz scanning (original behavior)
 */

#if !defined(ARDUINO_ARCH_ESP32)
  #error "This program requires an ESP32"
#endif

#include <Arduino.h>
#include <HardwareSerial.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include "opendroneid.h"
#include "odid_wifi.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// Board-specific configuration
// ============================================================================

#if defined(ARDUINO_XIAO_ESP32C5)
  // XIAO ESP32-C5 - Dual-band WiFi 6 (2.4GHz + 5GHz)
  // Physical pin D2 = GPIO4 on C5 (matches S3's D2 physical position for OUI-SPY PCB)
  #define BUZZER_PIN 4          // GPIO4 (D2) - PWM capable - same physical pin as S3
  #define LED_PIN 8             // GPIO8 - Built-in LED on C5
  #define LED_INVERTED false    // LED is active HIGH on C5
  #define DUAL_BAND_ENABLED true
  #define BOARD_NAME "XIAO ESP32-C5 (Dual-Band)"
#else
  // XIAO ESP32-S3 (default) - Single-band 2.4GHz
  #define BUZZER_PIN 3          // GPIO3 (D2) - PWM capable
  #define LED_PIN 21            // GPIO21 - Built-in orange LED
  #define LED_INVERTED true     // LED is active LOW (inverted)
  #define DUAL_BAND_ENABLED false
  #define BOARD_NAME "XIAO ESP32-S3 (2.4GHz)"
#endif

// ============================================================================
// Dual-Band Channel Configuration
// ============================================================================

// 2.4GHz RemoteID channel (WiFi NAN standard)
#define CHANNEL_2_4GHZ 6

// 5GHz RemoteID channels (UNII-3 band - commonly used for RemoteID)
// These channels are available in most regions for unlicensed use
static const uint8_t channels_5ghz[] = {149, 153, 157, 161, 165};
#define NUM_5GHZ_CHANNELS (sizeof(channels_5ghz) / sizeof(channels_5ghz[0]))

// Channel hopping timing (milliseconds)
// Total cycle time = DWELL_TIME_MS * (1 + NUM_5GHZ_CHANNELS) = ~180ms
// This ensures multiple samples per RemoteID broadcast interval (typically 1Hz)
#define DWELL_TIME_MS 30

// ============================================================================
// Audio Configuration
// ============================================================================

#define DETECT_FREQ 1000          // Detection alert - high pitch
#define HEARTBEAT_FREQ 600        // Heartbeat pulse frequency
#define DETECT_BEEP_DURATION 150  // Detection beep duration (ms)
#define HEARTBEAT_DURATION 100    // Short heartbeat pulse (ms)

// ============================================================================
// Data Structures
// ============================================================================

// WiFi band enumeration for tracking detection source
enum WiFiBand {
  BAND_UNKNOWN = 0,
  BAND_2_4GHZ = 1,
  BAND_5GHZ = 2,
  BAND_BLE = 3
};

struct id_data {
  uint8_t  mac[6];
  int      rssi;
  uint32_t last_seen;
  char     op_id[ODID_ID_SIZE + 1];
  char     uav_id[ODID_ID_SIZE + 1];
  double   lat_d;
  double   long_d;
  double   base_lat_d;
  double   base_long_d;
  int      altitude_msl;
  int      height_agl;
  int      speed;
  int      heading;
  int      flag;
  WiFiBand band;           // Which band/protocol detected this drone
  uint8_t  channel;        // Channel where detected (for WiFi)
};

// ============================================================================
// Function Prototypes
// ============================================================================

void callback(void *, wifi_promiscuous_pkt_type_t);
void send_json_fast(const id_data *UAV);
void buzzerTask(void *parameter);
void channelHopTask(void *parameter);

// ============================================================================
// Global Variables
// ============================================================================

#define MAX_UAVS 8
id_data uavs[MAX_UAVS] = {0};
BLEScan* pBLEScan = nullptr;
ODID_UAS_Data UAS_data;
unsigned long last_status = 0;
unsigned long last_heartbeat = 0;

// Current channel tracking (for dual-band)
volatile uint8_t current_channel = CHANNEL_2_4GHZ;
volatile WiFiBand current_band = BAND_2_4GHZ;

// Thread-safe flags for buzzer (volatile for ISR access)
volatile bool device_in_range = false;
volatile bool trigger_detection_beep = false;
volatile bool trigger_heartbeat_beep = false;
static portMUX_TYPE buzzerMux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE channelMux = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t printQueue;

// ============================================================================
// UAV Tracking
// ============================================================================

id_data* next_uav(uint8_t* mac) {
  // Check if this MAC already exists
  for (int i = 0; i < MAX_UAVS; i++) {
    if (memcmp(uavs[i].mac, mac, 6) == 0)
      return &uavs[i];
  }
  // Find empty slot
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].mac[0] == 0)
      return &uavs[i];
  }
  // Overwrite oldest entry
  return &uavs[0];
}

// ============================================================================
// BLE Scanning Callbacks
// ============================================================================

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
public:
  void onResult(BLEAdvertisedDevice device) override {
    int len = device.getPayloadLength();
    if (len <= 0) return;
      
    uint8_t* payload = device.getPayload();
    // Check for RemoteID BLE advertisement (Service UUID 0xFFFA, type 0x0D)
    if (len > 5 && payload[1] == 0x16 && payload[2] == 0xFA && 
        payload[3] == 0xFF && payload[4] == 0x0D) {
      uint8_t* mac = (uint8_t*) device.getAddress().getNative();
      id_data* UAV = next_uav(mac);
      UAV->last_seen = millis();
      UAV->rssi = device.getRSSI();
      memcpy(UAV->mac, mac, 6);
      UAV->band = BAND_BLE;
      UAV->channel = 0;  // BLE doesn't use WiFi channels
      
      uint8_t* odid = &payload[6];
      switch (odid[0] & 0xF0) {
        case 0x00: {
          ODID_BasicID_data basic;
          decodeBasicIDMessage(&basic, (ODID_BasicID_encoded*) odid);
          strncpy(UAV->uav_id, (char*) basic.UASID, ODID_ID_SIZE);
          break;
        }
        case 0x10: {
          ODID_Location_data loc;
          decodeLocationMessage(&loc, (ODID_Location_encoded*) odid);
          UAV->lat_d = loc.Latitude;
          UAV->long_d = loc.Longitude;
          UAV->altitude_msl = (int) loc.AltitudeGeo;
          UAV->height_agl = (int) loc.Height;
          UAV->speed = (int) loc.SpeedHorizontal;
          UAV->heading = (int) loc.Direction;
          break;
        }
        case 0x40: {
          ODID_System_data sys;
          decodeSystemMessage(&sys, (ODID_System_encoded*) odid);
          UAV->base_lat_d = sys.OperatorLatitude;
          UAV->base_long_d = sys.OperatorLongitude;
          break;
        }
        case 0x50: {
          ODID_OperatorID_data op;
          decodeOperatorIDMessage(&op, (ODID_OperatorID_encoded*) odid);
          strncpy(UAV->op_id, (char*) op.OperatorId, ODID_ID_SIZE);
          break;
        }
      }
      UAV->flag = 1;
      
      // Trigger buzzer alert (thread-safe, non-blocking)
      portENTER_CRITICAL_ISR(&buzzerMux);
      if (!device_in_range) {
        trigger_detection_beep = true;
        device_in_range = true;
        last_heartbeat = millis();
      }
      portEXIT_CRITICAL_ISR(&buzzerMux);
      
      {
        id_data tmp = *UAV;
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(printQueue, &tmp, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
      }
    }
  }
};

// ============================================================================
// Buzzer Task (Non-blocking, dedicated FreeRTOS task)
// ============================================================================

void buzzerTask(void *parameter) {
  for (;;) {
    // Check for detection beep trigger
    portENTER_CRITICAL(&buzzerMux);
    bool do_detection = trigger_detection_beep;
    if (do_detection) trigger_detection_beep = false;
    portEXIT_CRITICAL(&buzzerMux);
    
    if (do_detection) {
      Serial.println("DRONE DETECTED! Playing alert sequence: 3 quick beeps + LED flashes");
      for (int i = 0; i < 3; i++) {
        tone(BUZZER_PIN, DETECT_FREQ, DETECT_BEEP_DURATION);
        #if LED_INVERTED
        digitalWrite(LED_PIN, LOW);   // Turn on LED (inverted logic)
        #else
        digitalWrite(LED_PIN, HIGH);  // Turn on LED (normal logic)
        #endif
        vTaskDelay(pdMS_TO_TICKS(150));
        #if LED_INVERTED
        digitalWrite(LED_PIN, HIGH);  // Turn off LED (inverted logic)
        #else
        digitalWrite(LED_PIN, LOW);   // Turn off LED (normal logic)
        #endif
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      Serial.println("Detection complete - drone identified!");
    }
    
    // Check for heartbeat beep trigger
    portENTER_CRITICAL(&buzzerMux);
    bool do_heartbeat = trigger_heartbeat_beep;
    if (do_heartbeat) trigger_heartbeat_beep = false;
    portEXIT_CRITICAL(&buzzerMux);
    
    if (do_heartbeat) {
      Serial.println("Heartbeat: Drone still in range");
      tone(BUZZER_PIN, HEARTBEAT_FREQ, HEARTBEAT_DURATION);
      #if LED_INVERTED
      digitalWrite(LED_PIN, LOW);
      #else
      digitalWrite(LED_PIN, HIGH);
      #endif
      vTaskDelay(pdMS_TO_TICKS(100));
      #if LED_INVERTED
      digitalWrite(LED_PIN, HIGH);
      #else
      digitalWrite(LED_PIN, LOW);
      #endif
      vTaskDelay(pdMS_TO_TICKS(50));
      tone(BUZZER_PIN, HEARTBEAT_FREQ, HEARTBEAT_DURATION);
      #if LED_INVERTED
      digitalWrite(LED_PIN, LOW);
      #else
      digitalWrite(LED_PIN, HIGH);
      #endif
      vTaskDelay(pdMS_TO_TICKS(100));
      #if LED_INVERTED
      digitalWrite(LED_PIN, HIGH);
      #else
      digitalWrite(LED_PIN, LOW);
      #endif
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ============================================================================
// JSON Output with Band Information
// ============================================================================

const char* bandToString(WiFiBand band) {
  switch (band) {
    case BAND_2_4GHZ: return "2.4GHz";
    case BAND_5GHZ:   return "5GHz";
    case BAND_BLE:    return "BLE";
    default:          return "unknown";
  }
}

void send_json_fast(const id_data *UAV) {
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           UAV->mac[0], UAV->mac[1], UAV->mac[2],
           UAV->mac[3], UAV->mac[4], UAV->mac[5]);
  char json_msg[320];
  snprintf(json_msg, sizeof(json_msg),
    "{\"mac\":\"%s\",\"rssi\":%d,\"band\":\"%s\",\"channel\":%d,\"drone_lat\":%.6f,\"drone_long\":%.6f,\"drone_altitude\":%d,\"pilot_lat\":%.6f,\"pilot_long\":%.6f,\"basic_id\":\"%s\"}",
    mac_str, UAV->rssi, bandToString(UAV->band), UAV->channel,
    UAV->lat_d, UAV->long_d, UAV->altitude_msl,
    UAV->base_lat_d, UAV->base_long_d, UAV->uav_id);
  Serial.println(json_msg);
}

// ============================================================================
// Channel Hopping Task (Dual-Band Support)
// ============================================================================

#if DUAL_BAND_ENABLED
void channelHopTask(void *parameter) {
  uint8_t channel_index = 0;  // Index into 5GHz channel array
  bool on_5ghz = false;       // Currently on 5GHz band?
  
  Serial.println("[DUAL-BAND] Channel hopping task started");
  Serial.printf("[DUAL-BAND] Scanning: 2.4GHz ch%d + 5GHz ch", CHANNEL_2_4GHZ);
  for (int i = 0; i < NUM_5GHZ_CHANNELS; i++) {
    Serial.printf("%d%s", channels_5ghz[i], (i < NUM_5GHZ_CHANNELS - 1) ? "," : "\n");
  }
  
  for (;;) {
    uint8_t next_channel;
    WiFiBand next_band;
    
    if (!on_5ghz) {
      // Currently on 2.4GHz, switch to first 5GHz channel
      next_channel = channels_5ghz[0];
      next_band = BAND_5GHZ;
      channel_index = 0;
      on_5ghz = true;
    } else {
      // Move through 5GHz channels, then back to 2.4GHz
      channel_index++;
      if (channel_index >= NUM_5GHZ_CHANNELS) {
        // Done with 5GHz, go back to 2.4GHz
        next_channel = CHANNEL_2_4GHZ;
        next_band = BAND_2_4GHZ;
        on_5ghz = false;
      } else {
        // Next 5GHz channel
        next_channel = channels_5ghz[channel_index];
        next_band = BAND_5GHZ;
      }
    }
    
    // Switch channel (thread-safe update of current channel info)
    portENTER_CRITICAL(&channelMux);
    current_channel = next_channel;
    current_band = next_band;
    portEXIT_CRITICAL(&channelMux);
    
    // Actually change the WiFi channel
    esp_wifi_set_channel(next_channel, WIFI_SECOND_CHAN_NONE);
    
    // Dwell on this channel before hopping
    vTaskDelay(pdMS_TO_TICKS(DWELL_TIME_MS));
  }
}
#endif

// ============================================================================
// BLE Scan Task
// ============================================================================

void bleScanTask(void *parameter) {
  for (;;) {
    BLEScanResults* foundDevices = pBLEScan->start(1, false);
    pBLEScan->clearResults();
    delay(100);
  }
}

// ============================================================================
// WiFi Process Task (placeholder for potential future processing)
// ============================================================================

void wifiProcessTask(void *parameter) {
  for (;;) {
    delay(10);
  }
}

// ============================================================================
// WiFi Promiscuous Mode Callback
// ============================================================================

void callback(void *buffer, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  
  wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buffer;
  uint8_t *payload = packet->payload;
  int length = packet->rx_ctrl.sig_len;
  
  // Get current channel/band info (thread-safe)
  uint8_t detect_channel;
  WiFiBand detect_band;
  portENTER_CRITICAL_ISR(&channelMux);
  detect_channel = current_channel;
  detect_band = current_band;
  portEXIT_CRITICAL_ISR(&channelMux);
  
  // Check for NAN Action Frame (WiFi Aware RemoteID)
  static const uint8_t nan_dest[6] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
  if (memcmp(nan_dest, &payload[4], 6) == 0) {
    if (odid_wifi_receive_message_pack_nan_action_frame(&UAS_data, nullptr, payload, length) == 0) {
      id_data UAV;
      memset(&UAV, 0, sizeof(UAV));
      memcpy(UAV.mac, &payload[10], 6);
      UAV.rssi = packet->rx_ctrl.rssi;
      UAV.last_seen = millis();
      UAV.band = detect_band;
      UAV.channel = detect_channel;
      
      if (UAS_data.BasicIDValid[0]) {
        strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
      }
      if (UAS_data.LocationValid) {
        UAV.lat_d = UAS_data.Location.Latitude;
        UAV.long_d = UAS_data.Location.Longitude;
        UAV.altitude_msl = (int)UAS_data.Location.AltitudeGeo;
        UAV.height_agl = (int)UAS_data.Location.Height;
        UAV.speed = (int)UAS_data.Location.SpeedHorizontal;
        UAV.heading = (int)UAS_data.Location.Direction;
      }
      if (UAS_data.SystemValid) {
        UAV.base_lat_d = UAS_data.System.OperatorLatitude;
        UAV.base_long_d = UAS_data.System.OperatorLongitude;
      }
      if (UAS_data.OperatorIDValid) {
        strncpy(UAV.op_id, (char *)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
      }
      
      id_data* storedUAV = next_uav(UAV.mac);
      *storedUAV = UAV;
      storedUAV->flag = 1;
      
      // Trigger buzzer alert (thread-safe, non-blocking)
      portENTER_CRITICAL_ISR(&buzzerMux);
      if (!device_in_range) {
        trigger_detection_beep = true;
        device_in_range = true;
        last_heartbeat = millis();
      }
      portEXIT_CRITICAL_ISR(&buzzerMux);
      
      {
        id_data tmp = *storedUAV;
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(printQueue, &tmp, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
      }
    }
  }
  // Check for Beacon Frame with RemoteID Vendor Specific IE
  else if (payload[0] == 0x80) {
    int offset = 36;
    while (offset < length) {
      int typ = payload[offset];
      int len = payload[offset + 1];
      
      // Check for RemoteID vendor specific element
      // OUI 90:3a:e6 (Parrot) or fa:0b:bc (ASD-STAN)
      if ((typ == 0xdd) &&
          (((payload[offset + 2] == 0x90 && payload[offset + 3] == 0x3a && payload[offset + 4] == 0xe6)) ||
           ((payload[offset + 2] == 0xfa && payload[offset + 3] == 0x0b && payload[offset + 4] == 0xbc)))) {
        int j = offset + 7;
        if (j < length) {
          memset(&UAS_data, 0, sizeof(UAS_data));
          odid_message_process_pack(&UAS_data, &payload[j], length - j);
          
          id_data UAV;
          memset(&UAV, 0, sizeof(UAV));
          memcpy(UAV.mac, &payload[10], 6);
          UAV.rssi = packet->rx_ctrl.rssi;
          UAV.last_seen = millis();
          UAV.band = detect_band;
          UAV.channel = detect_channel;
          
          if (UAS_data.BasicIDValid[0]) {
            strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
          }
          if (UAS_data.LocationValid) {
            UAV.lat_d = UAS_data.Location.Latitude;
            UAV.long_d = UAS_data.Location.Longitude;
            UAV.altitude_msl = (int)UAS_data.Location.AltitudeGeo;
            UAV.height_agl = (int)UAS_data.Location.Height;
            UAV.speed = (int)UAS_data.Location.SpeedHorizontal;
            UAV.heading = (int)UAS_data.Location.Direction;
          }
          if (UAS_data.SystemValid) {
            UAV.base_lat_d = UAS_data.System.OperatorLatitude;
            UAV.base_long_d = UAS_data.System.OperatorLongitude;
          }
          if (UAS_data.OperatorIDValid) {
            strncpy(UAV.op_id, (char *)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
          }
          
          id_data* storedUAV = next_uav(UAV.mac);
          *storedUAV = UAV;
          storedUAV->flag = 1;
          
          // Trigger buzzer alert (thread-safe, non-blocking)
          portENTER_CRITICAL_ISR(&buzzerMux);
          if (!device_in_range) {
            trigger_detection_beep = true;
            device_in_range = true;
            last_heartbeat = millis();
          }
          portEXIT_CRITICAL_ISR(&buzzerMux);
          
          {
            id_data tmp = *storedUAV;
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xQueueSendFromISR(printQueue, &tmp, &xHigherPriorityTaskWoken);
            if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
          }
        }
      }
      offset += len + 2;
    }
  }
}

// ============================================================================
// Printer Task (JSON output over USB Serial)
// ============================================================================

void printerTask(void *param) {
  id_data UAV;
  for (;;) {
    if (xQueueReceive(printQueue, &UAV, portMAX_DELAY)) {
      send_json_fast(&UAV);
    }
  }
}

// ============================================================================
// Initialization Functions
// ============================================================================

void initializeSerial() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n========================================");
  Serial.println("       Sky-Spy RemoteID Scanner");
  Serial.println("========================================");
  Serial.printf("Board: %s\n", BOARD_NAME);
  #if DUAL_BAND_ENABLED
  Serial.println("Mode: DUAL-BAND (2.4GHz + 5GHz WiFi)");
  #else
  Serial.println("Mode: SINGLE-BAND (2.4GHz WiFi only)");
  #endif
  Serial.println("Protocols: WiFi NAN, WiFi Beacon, BLE");
  Serial.println("========================================\n");
}

void initializeBuzzer() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  Serial.printf("Buzzer initialized on GPIO%d\n", BUZZER_PIN);
}

void initializeLED() {
  pinMode(LED_PIN, OUTPUT);
  #if LED_INVERTED
  digitalWrite(LED_PIN, HIGH);  // Turn off LED (inverted logic)
  #else
  digitalWrite(LED_PIN, LOW);   // Turn off LED (normal logic)
  #endif
  Serial.printf("LED initialized on GPIO%d (inverted: %s)\n", LED_PIN, LED_INVERTED ? "yes" : "no");
}

void initializeWiFi() {
  nvs_flash_init();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&callback);
  
  #if DUAL_BAND_ENABLED
  // Start on 2.4GHz channel 6, channel hopping task will handle the rest
  esp_wifi_set_channel(CHANNEL_2_4GHZ, WIFI_SECOND_CHAN_NONE);
  Serial.printf("WiFi promiscuous mode enabled (starting on 2.4GHz ch%d)\n", CHANNEL_2_4GHZ);
  #else
  // Single-band: stay on channel 6
  esp_wifi_set_channel(CHANNEL_2_4GHZ, WIFI_SECOND_CHAN_NONE);
  Serial.printf("WiFi promiscuous mode enabled (fixed on ch%d)\n", CHANNEL_2_4GHZ);
  #endif
}

void initializeBLE() {
  BLEDevice::init("DroneID");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  Serial.println("BLE scanning initialized");
}

// ============================================================================
// Setup
// ============================================================================

void setup() {
  setCpuFrequencyMhz(160);
  
  initializeSerial();
  initializeBuzzer();
  initializeLED();
  
  // Boot beep and LED flash to confirm device is ready
  tone(BUZZER_PIN, 800, 200);
  #if LED_INVERTED
  digitalWrite(LED_PIN, LOW);
  #else
  digitalWrite(LED_PIN, HIGH);
  #endif
  delay(250);
  #if LED_INVERTED
  digitalWrite(LED_PIN, HIGH);
  #else
  digitalWrite(LED_PIN, LOW);
  #endif
  
  initializeWiFi();
  initializeBLE();
  
  // Create print queue
  printQueue = xQueueCreate(MAX_UAVS, sizeof(id_data));
  
  // Create FreeRTOS tasks
  // Note: ESP32-C5 is single-core (RISC-V), tasks will be scheduled on core 0
  // ESP32-S3 is dual-core, we pin tasks to specific cores
  
  #if defined(CONFIG_IDF_TARGET_ESP32C5)
  // Single-core RISC-V chip: all tasks on core 0
  xTaskCreate(bleScanTask, "BLEScanTask", 10000, NULL, 1, NULL);
  xTaskCreate(wifiProcessTask, "WiFiProcessTask", 10000, NULL, 1, NULL);
  xTaskCreate(printerTask, "PrinterTask", 10000, NULL, 1, NULL);
  xTaskCreate(buzzerTask, "BuzzerTask", 4096, NULL, 1, NULL);
  #if DUAL_BAND_ENABLED
  xTaskCreate(channelHopTask, "ChannelHopTask", 4096, NULL, 2, NULL);  // Higher priority for timing
  #endif
  #else
  // Dual-core chips (ESP32-S3): distribute tasks across cores
  xTaskCreatePinnedToCore(bleScanTask, "BLEScanTask", 10000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(wifiProcessTask, "WiFiProcessTask", 10000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(printerTask, "PrinterTask", 10000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(buzzerTask, "BuzzerTask", 4096, NULL, 1, NULL, 1);
  #if DUAL_BAND_ENABLED
  xTaskCreatePinnedToCore(channelHopTask, "ChannelHopTask", 4096, NULL, 2, NULL, 0);
  #endif
  #endif
  
  memset(uavs, 0, sizeof(uavs));
  
  Serial.println("\n[+] Sky-Spy initialized and scanning...\n");
}

// ============================================================================
// Main Loop
// ============================================================================

void loop() {
  unsigned long current_millis = millis();
  
  // Status message every 60 seconds
  if ((current_millis - last_status) > 60000UL) {
    #if DUAL_BAND_ENABLED
    Serial.println("{\"status\":\"active\",\"mode\":\"dual-band\",\"bands\":[\"2.4GHz\",\"5GHz\",\"BLE\"]}");
    #else
    Serial.println("{\"status\":\"active\",\"mode\":\"single-band\",\"bands\":[\"2.4GHz\",\"BLE\"]}");
    #endif
    last_status = current_millis;
  }
  
  // Handle heartbeat pulse if drone is in range (thread-safe)
  portENTER_CRITICAL(&buzzerMux);
  bool in_range = device_in_range;
  portEXIT_CRITICAL(&buzzerMux);
  
  if (in_range) {
    // Check if 5 seconds have passed since last heartbeat
    if (current_millis - last_heartbeat >= 5000) {
      portENTER_CRITICAL(&buzzerMux);
      trigger_heartbeat_beep = true;
      portEXIT_CRITICAL(&buzzerMux);
      last_heartbeat = current_millis;
    }
    
    // Check if drone has gone out of range (no detection for 7 seconds)
    bool drone_still_detected = false;
    for (int i = 0; i < MAX_UAVS; i++) {
      if (uavs[i].mac[0] != 0 && (current_millis - uavs[i].last_seen) < 7000) {
        drone_still_detected = true;
        break;
      }
    }
    
    if (!drone_still_detected) {
      Serial.println("Drone out of range - stopping heartbeat");
      portENTER_CRITICAL(&buzzerMux);
      device_in_range = false;
      portEXIT_CRITICAL(&buzzerMux);
    }
  }
}
