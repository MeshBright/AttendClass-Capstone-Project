/*
 * ============================================================
 * AttendESP — Dual-Core Offline + BLE/QR Architecture
 * Core 1: Strict 2FA (RFID+FP), UI, Enrollment, Sync
 * Core 0: Dual SD Card Logging & NTP Time Sync (Background)
 * ============================================================
 */
#include <WiFi.h>
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include <MFRC522.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_Fingerprint.h>
#include <ArduinoJson.h>
#include <time.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "qrcode.h"
#include "base64.h"

// ================= NETWORK (For NTP Time & Sync) =================
const char* WIFI_SSID     = "YOUR_WIFI_SSID"; 
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET = 3600;   // WAT = UTC+1
const int   DST_OFFSET = 0;

// ================= DEVICE & CLASS CONFIG =================
const String MASTER_RFID_UID = "DDAF3906"; 
const String DEVICE_ID = "ESP32-LT101";
const String ACTIVE_SCHEDULE_ID = "SCHED_001";
const String ACTIVE_COURSE_CODE = "ENG302";

// ================= PINOUT DIRECTORY =================
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDR 0x3C

#define RFID_SCK  18
#define RFID_MISO 19
#define RFID_MOSI 23
#define RFID_CS   5
#define RFID_RST  -1 

#define SD_SCK  14
#define SD_MISO 35 
#define SD_MOSI 13
#define SD_CS   27

#define FP_RX 32 
#define FP_TX 33 

#define GREEN_LED 4
#define RED_LED   26
#define BUZZER_PIN 16

// ================= BLE CONFIG =================
#define BLE_SERVICE_UUID        "4e67a100-1234-5678-abcd-0123456789ab"
#define BLE_CHAR_STUDENTID_UUID "4e67a101-1234-5678-abcd-0123456789ab"
#define BLE_CHAR_RESULT_UUID    "4e67a102-1234-5678-abcd-0123456789ab"

BLEServer* bleServer = nullptr;
BLECharacteristic *bleStudentChar = nullptr, *bleResultChar = nullptr;
bool bleReady = false;
volatile bool bleRequestPending = false;
String bleRequestJson = "";

// ================= OBJECTS & GLOBALS =================
Adafruit_SH1106G display = Adafruit_SH1106G(128, 64, &Wire, -1);
MFRC522 rfid(RFID_CS, RFID_RST);
SPIClass sdSPI(HSPI); 
HardwareSerial fpSerial(2);
Adafruit_Fingerprint finger(&fpSerial);

// --- MODIFIED: Added MODE_SYNC ---
enum DeviceMode { MODE_ATTENDANCE, MODE_ENROLLMENT, MODE_QR_BLE, MODE_SYNC };
DeviceMode currentMode = MODE_ATTENDANCE;

// --- MODIFIED: Fixed Heap Fragmentation via static char arrays ---
struct StudentRecord { 
  char id[16]; 
  char name[48]; 
  char matric[26]; 
  char rfid[16]; 
  int fingerprintId; 
};
static const int MAX_STUDENTS = 135; // Dropped to 135 to save RAM
StudentRecord students[MAX_STUDENTS];
int studentCount = 0;

bool sdAvailable = false;
volatile bool isTimeSynced = false;
volatile bool isWifiFailed = false;

long currentSlotIndex = -1;
String currentQrBase64 = "";
int enrollmentTargetIdx = -1; 

// --- FREERTOS GLOBALS ---
struct LogEvent {
  char logType; // 'A' for Attendance, 'E' for Enrollment
  char matric[25];
  char name[45];
  char method[15]; // "2FA" or "QR_BLE"
  char rfid[15];   // Used for Enrollment only
  int fp_id;       // Used for Enrollment only
  uint32_t scan_millis; 
};
QueueHandle_t sdQueue;
SemaphoreHandle_t displayMutex;
SemaphoreHandle_t sdMutex;

// ================= HELPER FUNCTIONS =================
void beepSuccess() {
  digitalWrite(BUZZER_PIN, HIGH); delay(150); digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(GREEN_LED, HIGH); delay(300); digitalWrite(GREEN_LED, LOW);
}

void beepError() {
  digitalWrite(BUZZER_PIN, HIGH); delay(500); digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RED_LED, HIGH); delay(600); digitalWrite(RED_LED, LOW);
}

void beepClick() {
  digitalWrite(BUZZER_PIN, HIGH); delay(50); digitalWrite(BUZZER_PIN, LOW);
}

void oledPrint(String line1, String line2 = "", String line3 = "", String line4 = "") {
  if (xSemaphoreTake(displayMutex, portMAX_DELAY)) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    
    // 1. Print the main UI text
    display.println(line1);
    display.println(line2);
    display.println(line3);
    display.println(line4); 

    // 2. Draw the Persistent Status Badge in the top right corner (x=110, y=0)
    display.setCursor(110, 0);
    if (isTimeSynced) {
      display.print("[T]"); // T = Time Synced
    } else if (isWifiFailed) {
      display.print("[X]"); // X = Sync Failed
    } else {
      display.print("[~]"); // ~ = Hunting for Wi-Fi
    }

    display.display();
    xSemaphoreGive(displayMutex);
  }
}

String getRFIDString() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  rfid.PICC_HaltA();
  return uid;
}

String modeName() { 
  if (currentMode == MODE_ATTENDANCE) return "ATTENDANCE";
  if (currentMode == MODE_ENROLLMENT) return "ENROLLMENT";
  if (currentMode == MODE_QR_BLE) return "QR+BLE";
  return "CLOUD SYNC"; 
}

// ================= STUDENT DATA MANAGEMENT =================
int findStudentByRfid(const String& uid) { 
  // Temporarily cast char array to String for comparison
  for(int i=0; i<studentCount; i++) if(String(students[i].rfid) == uid) return i; 
  return -1; 
}

int findStudentByIdOrMatric(const String& searchId) { 
  for(int i=0; i<studentCount; i++) {
    if(String(students[i].id) == searchId || String(students[i].matric) == searchId) return i; 
  }
  return -1; 
}

int findNextUnenrolledStudent(int startIdx = 0) { 
  if (studentCount == 0) return -1;
  if (startIdx >= studentCount) startIdx = 0; 
  
  for(int i = startIdx; i < studentCount; i++) {
    if(strlen(students[i].rfid) == 0 || students[i].fingerprintId < 0) return i;
  }
  for(int i = 0; i < startIdx; i++) {
    if(strlen(students[i].rfid) == 0 || students[i].fingerprintId < 0) return i;
  }
  return -1; 
}

void loadStudentsFromSD() {
  if (!sdAvailable) return;
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  if (!SD.exists("/TestingGroup8StudentDatabase.json")) { xSemaphoreGive(sdMutex); return; }
  
  File f = SD.open("/TestingGroup8StudentDatabase.json", FILE_READ);
  if (!f) { xSemaphoreGive(sdMutex); return; }
  
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  xSemaphoreGive(sdMutex);
  
  if (err || !doc.is<JsonArray>()) return;
  
  studentCount = 0;
  JsonArray arr = doc.as<JsonArray>();
  for (JsonVariant v : arr) {
    if (studentCount >= MAX_STUDENTS) break;
    StudentRecord rec;
    
    // Safely copy JSON strings into the static character arrays
    strlcpy(rec.id, v["id"] | "", sizeof(rec.id));
    strlcpy(rec.name, v["name"] | "", sizeof(rec.name));
    strlcpy(rec.matric, v["matric_number"] | "", sizeof(rec.matric));
    
    String tempRfid = String((const char*)(v["rfid_card_id"] | ""));
    tempRfid.trim(); tempRfid.toUpperCase();
    strlcpy(rec.rfid, tempRfid.c_str(), sizeof(rec.rfid));
    
    rec.fingerprintId = v["fingerprint_id"] | -1;
    
    if (strlen(rec.matric) > 0) students[studentCount++] = rec;
  }
}

void saveStudentsToSD() {
  if (!sdAvailable) return;
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  SD.remove("/TestingGroup8StudentDatabase.json");
  File f = SD.open("/TestingGroup8StudentDatabase.json", FILE_WRITE);
  if (!f) { xSemaphoreGive(sdMutex); return; }
  
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < studentCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["id"] = students[i].id;
    o["name"] = students[i].name;
    o["matric_number"] = students[i].matric;
    o["rfid_card_id"] = students[i].rfid;
    o["fingerprint_id"] = students[i].fingerprintId;
  }
  serializeJson(doc, f);
  f.close();
  xSemaphoreGive(sdMutex);
}

// ================= CLOUD SYNC LOGIC (STUB) =================
void syncDataToCloud() {
  oledPrint("CLOUD SYNC", "Starting Wi-Fi...");
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    oledPrint("CLOUD SYNC", "Wi-Fi Connected!", "Uploading logs...");
    beepSuccess();
    
    // ==========================================
    // TODO: HTTPS POST PROTOCOL GOES HERE LATER
    // ==========================================
    
    delay(3000); // Simulate upload time for now
    
    oledPrint("SYNC COMPLETE", "Data uploaded safely.");
    beepSuccess();
    delay(2000);
  } else {
    oledPrint("SYNC FAILED", "Check Wi-Fi router", "or credentials.");
    beepError();
    delay(3000);
  }

  // Turn off Wi-Fi to free up RAM and save power
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  
  // Return to Attendance Mode
  currentMode = MODE_ATTENDANCE;
  oledPrint("SYSTEM READY", modeName());
}

// ================= BLE & QR LOGIC =================
class StudentWriteCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String v = pChar->getValue();
    if (v.length() == 0) return;
    bleRequestJson = v;
    bleRequestPending = true;
  }
};

void initBLE() {
  BLEDevice::init(String("AttendESP_") + DEVICE_ID);
  BLEDevice::setPower(ESP_PWR_LVL_P9);
  bleServer = BLEDevice::createServer();
  BLEService* s = bleServer->createService(BLE_SERVICE_UUID);
  
  bleStudentChar = s->createCharacteristic(BLE_CHAR_STUDENTID_UUID, BLECharacteristic::PROPERTY_WRITE);
  bleStudentChar->setCallbacks(new StudentWriteCB());
  
  bleResultChar = s->createCharacteristic(BLE_CHAR_RESULT_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleResultChar->addDescriptor(new BLE2902());
  bleResultChar->setValue("READY");
  
  s->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  BLEDevice::startAdvertising();
  bleReady = true;
}

void bleNotifyResult(const String& msg) {
  if (bleReady && bleResultChar) {
    bleResultChar->setValue(msg.c_str());
    bleResultChar->notify();
  }
}

String makeStaticQrBase64() {
  JsonDocument doc;
  doc["t"] = "qr_static";
  doc["sid"] = ACTIVE_SCHEDULE_ID;
  doc["did"] = DEVICE_ID;
  doc["cc"] = ACTIVE_COURSE_CODE;
  doc["tk"] = ACTIVE_SCHEDULE_ID + ":" + DEVICE_ID + ":" + ACTIVE_COURSE_CODE;
  
  String json;
  serializeJson(doc, json);
  return base64::encode(json);
}

void drawQrCodeToDisplay(const String& payloadB64) {
  if (xSemaphoreTake(displayMutex, portMAX_DELAY)) {
    const uint8_t v = 10; 
    uint8_t qrcodeData[qrcode_getBufferSize(v)];
    QRCode qrcode;
    qrcode_initText(&qrcode, qrcodeData, v, 0, payloadB64.c_str());
    
    display.clearDisplay();
    int mSize = 1; 
    int offsetX = (128 - qrcode.size * mSize) / 2; 
    int offsetY = (64 - qrcode.size * mSize) / 2;  
    
    for (int y = 0; y < qrcode.size; y++) {
      for (int x = 0; x < qrcode.size; x++) {
        if (qrcode_getModule(&qrcode, x, y)) {
          display.fillRect(offsetX + x * mSize, offsetY + y * mSize, mSize, mSize, SH110X_WHITE);
        }
      }
    }
    display.display();
    xSemaphoreGive(displayMutex);
  }
}

void handleBleRequestIfAny() {
  if (!bleRequestPending) return;
  bleRequestPending = false;
  
  Serial.println("\n[BLE] Incoming Payload: " + bleRequestJson);
  
  JsonDocument req;
  if (deserializeJson(req, bleRequestJson) != DeserializationError::Ok) {
    Serial.println("[BLE REJECT] Invalid JSON formatting.");
    bleNotifyResult("ERROR:BAD_REQUEST");
    return;
  }
  
  String sId = String((const char*)(req["student_id"] | ""));
  int phoneRssi = req["rssi"] | -100; 
  
  if (sId.length() == 0) { 
    Serial.println("[BLE REJECT] Missing 'student_id'.");
    bleNotifyResult("ERROR:MISSING_FIELDS"); 
    return; 
  }

  if (phoneRssi < -80) {
    Serial.println("[BLE REJECT] Out of range! RSSI: " + String(phoneRssi));
    bleNotifyResult("ERROR:OUT_OF_RANGE");
    beepError();
    oledPrint("ACCESS DENIED", "Step closer!", "Enter classroom");
    delay(3000);
    currentSlotIndex = -1; // Force QR redraw
    return;
  }

  JsonVariant p = req["qr_payload"];
  if (p.isNull()) { 
    Serial.println("[BLE REJECT] Missing 'qr_payload' object.");
    bleNotifyResult("ERROR:MISSING_PAYLOAD"); 
    return; 
  }

  String dId = String((const char*)(p["did"] | ""));
  String tok = String((const char*)(p["tk"] | ""));
  String expectedToken = ACTIVE_SCHEDULE_ID + ":" + DEVICE_ID + ":" + ACTIVE_COURSE_CODE;

  if (dId != DEVICE_ID) { 
    Serial.println("[BLE REJECT] Wrong Classroom ID: " + dId);
    bleNotifyResult("ERROR:WRONG_CLASSROOM"); 
    return; 
  }
  
  if (tok != expectedToken) { 
    Serial.println("[BLE REJECT] Invalid Security Token: " + tok);
    bleNotifyResult("ERROR:QR_TOKEN_INVALID"); 
    return; 
  }

  int stuIdx = findStudentByIdOrMatric(sId);
  String displayName = (stuIdx >= 0) ? String(students[stuIdx].name) : "Unknown Student";
  String matricNum = (stuIdx >= 0) ? String(students[stuIdx].matric) : sId;

  // SMART ATTENDANCE LOGGING
  LogEvent bleLog;
  bleLog.logType = 'A'; 
  strcpy(bleLog.matric, matricNum.c_str());
  strcpy(bleLog.name, displayName.c_str());
  strcpy(bleLog.method, "QR_BLE");
  bleLog.scan_millis = millis();
  xQueueSend(sdQueue, &bleLog, 0);

  bleNotifyResult("OK:BLE_NEAR");
  Serial.println("[BLE SUCCESS] Logged in: " + displayName);
  
  beepSuccess();
  oledPrint("BLE CHECK-IN", "Welcome,", displayName);
  delay(3000); 

  currentSlotIndex = -1; 
}

// ================= FINGERPRINT ENROLLMENT LOGIC =================
int enrollFingerprint() {
  finger.getTemplateCount();
  int id = finger.templateCount + 1; 
  if (id > 127) return -1; 

  int p = -1;
  unsigned long timeoutTimer;

  oledPrint("PLACE FINGER", "ID: " + String(id));
  timeoutTimer = millis();
  while (p != FINGERPRINT_OK) { 
    p = finger.getImage(); 
    if (millis() - timeoutTimer > 10000) { beepError(); return -1; }
    delay(50); 
  }
  
  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) { beepError(); return -1; }

  oledPrint("REMOVE FINGER");
  beepClick();
  delay(2000);
  
  p = 0;
  timeoutTimer = millis();
  while (p != FINGERPRINT_NOFINGER) { 
    p = finger.getImage(); 
    if (millis() - timeoutTimer > 10000) { beepError(); return -1; }
    delay(50); 
  }

  oledPrint("PLACE FINGER", "AGAIN");
  p = -1;
  timeoutTimer = millis();
  while (p != FINGERPRINT_OK) { 
    p = finger.getImage(); 
    if (millis() - timeoutTimer > 10000) { beepError(); return -1; }
    delay(50); 
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) { beepError(); return -1; }

  p = finger.createModel();
  if (p == FINGERPRINT_OK) {
    p = finger.storeModel(id);
    if (p == FINGERPRINT_OK) return id;
  }
  
  beepError();
  return -1;
}

// ================= CORE 0: BACKGROUND TIME SYNC =================
void ntpSyncTask(void * pvParameters) {
  Serial.println("Background Task: Hunting for Wi-Fi time...");
  
  btStop(); 
  delay(50);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int wifiRetries = 0;

  while (WiFi.status() != WL_CONNECTED && wifiRetries < 20) {
    vTaskDelay(500 / portTICK_PERIOD_MS); 
    wifiRetries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);
    struct tm t; 
    int timeRetries = 0;
    while (!getLocalTime(&t) && timeRetries < 10) { 
      vTaskDelay(500 / portTICK_PERIOD_MS); 
      timeRetries++; 
    }
    
    Serial.println("Background Task: Time Synced Successfully!");
    isTimeSynced = true; 
    
    // --- NEW: Non-Destructive UI Injection ---
    if (xSemaphoreTake(displayMutex, portMAX_DELAY)) {
      display.fillRect(110, 0, 18, 8, SH110X_BLACK); // Erase just the top right corner
      display.setCursor(110, 0);
      display.setTextColor(SH110X_WHITE);
      display.print("[T]"); // Draw success badge
      display.display();    // Push to screen WITHOUT clearing!
      xSemaphoreGive(displayMutex);
    }
    // -----------------------------------------

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  } else {
    Serial.println("Background Task: Wi-Fi Failed.");
    isWifiFailed = true; 
    
    // --- NEW: Non-Destructive UI Injection ---
    if (xSemaphoreTake(displayMutex, portMAX_DELAY)) {
      display.fillRect(110, 0, 18, 8, SH110X_BLACK); 
      display.setCursor(110, 0);
      display.setTextColor(SH110X_WHITE);
      display.print("[X]"); // Draw failure badge
      display.display();    
      xSemaphoreGive(displayMutex);
    }
    // -----------------------------------------
  }
  vTaskDelete(NULL); 
}

// ================= CORE 0: SD CARD LOGGING TASK =================
void sdWriteTask(void * pvParameters) {
  LogEvent event;
  for(;;) {
    if (isTimeSynced || isWifiFailed) {
      if (xQueueReceive(sdQueue, &event, 10) == pdPASS) {
        if (sdAvailable) {
          char finalTimestamp[30];

          if (isTimeSynced) {
            time_t now; 
            time(&now); 
            time_t exact_scan_time = now - ((millis() - event.scan_millis) / 1000); 
            struct tm * timeinfo = localtime(&exact_scan_time);
            strftime(finalTimestamp, sizeof(finalTimestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
          } else {
            strcpy(finalTimestamp, "TIME_NOT_SYNCED");
          }

          xSemaphoreTake(sdMutex, portMAX_DELAY);
          
          if (event.logType == 'A') {
            File logFile = SD.open("/attendance.csv", FILE_APPEND);
            if (logFile) {
              logFile.print(finalTimestamp); logFile.print(",");
              logFile.print(event.name); logFile.print(",");
              logFile.print(event.matric); logFile.print(",");
              logFile.println(event.method);
              logFile.close();
            }
          } 
          else if (event.logType == 'E') {
            File logFile = SD.open("/enrollment.csv", FILE_APPEND);
            if (logFile) {
              logFile.print(finalTimestamp); logFile.print(",");
              logFile.print(event.name); logFile.print(",");
              logFile.print(event.matric); logFile.print(",");
              logFile.print(event.rfid); logFile.print(",");
              logFile.println(event.fp_id);
              logFile.close();
            }
          }
          
          xSemaphoreGive(sdMutex);
        }
      }
    } else {
      vTaskDelay(500 / portTICK_PERIOD_MS);
    }
  }
}

// ================= MASTER NAVIGATION =================
void handleMasterNavigation() {
  beepClick();
  int menuTaps = 0;
  unsigned long lastTapTime = millis();
  const unsigned long MENU_TIMEOUT = 3000; 

  // Step 1: Show the visual menu (Added Sync)
  oledPrint("ADMIN MENU", "1:Att 2:Enr 3:QR", "4:Sync 5:Wipe", "Taps: 0");
  
  delay(1000); 

  // Step 2: Listen for menu selections
  while (millis() - lastTapTime < MENU_TIMEOUT) { 
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      if (getRFIDString() == MASTER_RFID_UID) {
        menuTaps++;
        lastTapTime = millis(); 
        beepClick();
        oledPrint("ADMIN MENU", "1:Att 2:Enr 3:QR", "4:Sync 5:Wipe", "Taps: " + String(menuTaps));
        delay(500); 
      }
    }
    delay(20);
  }

  // =========================================================
  // MEMORY MANAGEMENT: Destroy BLE if we are leaving QR Mode
  // =========================================================
  if (menuTaps != 3 && bleReady) {
    BLEDevice::deinit(true); // Free the 50KB heap!
    bleReady = false;
    bleServer = nullptr;
  }

  // Step 3: Execute the selected mode
  if (menuTaps == 1 || menuTaps == 0) { 
    currentMode = MODE_ATTENDANCE;
    beepSuccess();
    oledPrint("MODE: ATTENDANCE", "Activated");
    delay(1200);
    oledPrint("SYSTEM READY", modeName());
  } 
  
  else if (menuTaps == 2) { 
    currentMode = MODE_ENROLLMENT;
    enrollmentTargetIdx = findNextUnenrolledStudent(0); 
    beepSuccess();
    oledPrint("MODE: ENROLLMENT", "Activated");
    delay(1200);
    if (enrollmentTargetIdx >= 0) {
      oledPrint("ENROLL:", String(students[enrollmentTargetIdx].name), "Card=Save | FP=Skip");
    } else {
      oledPrint("ENROLLMENT", "All onboarded!");
      delay(1500);
      currentMode = MODE_ATTENDANCE;
      oledPrint("SYSTEM READY", modeName());
    }
  } 
  
  else if (menuTaps == 3) { 
    currentMode = MODE_QR_BLE; 
    currentSlotIndex = -1; 
    beepSuccess();
    oledPrint("MODE: QR & BLE", "Activated");
    delay(1200);
  } 
  
  // === NEW SYNC MODE (Tap 4) ===
  else if (menuTaps == 4) {
    currentMode = MODE_SYNC;
    beepSuccess();
    oledPrint("MODE: CLOUD SYNC", "Activated");
    delay(1200);
  }
  
  // === MOVED WIPE MODE (Tap 5+) ===
  else if (menuTaps >= 5) { 
    beepError(); 
    oledPrint("WIPE SYSTEM?", "Card = YES", "Finger = NO");
    
    unsigned long confirmTimeout = millis();
    bool wipeConfirmed = false;
    
    while (millis() - confirmTimeout < 10000) {
      if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        rfid.PICC_HaltA(); 
        wipeConfirmed = true;
        break; 
      }
      if (finger.getImage() == FINGERPRINT_OK) {
        while(finger.getImage() != FINGERPRINT_NOFINGER) { delay(50); }
        break; 
      }
      delay(50);
    }
    
    if (wipeConfirmed) {
      beepError(); 
      oledPrint("SYSTEM WIPE", "Erasing FP Sensor...");
      finger.emptyDatabase(); 
      delay(1000);

      oledPrint("SYSTEM WIPE", "Clearing SD Links...");
      for (int i = 0; i < studentCount; i++) {
        strlcpy(students[i].rfid, "", sizeof(students[i].rfid));
        students[i].fingerprintId = -1;
      }
      saveStudentsToSD(); 

      beepSuccess();
      oledPrint("WIPE COMPLETE", "Restarting Board...");
      delay(2000);
      ESP.restart(); 
      
    } else {
      beepClick();
      oledPrint("WIPE CANCELLED", "Data is Safe");
      delay(1500);
      currentMode = MODE_ATTENDANCE;
      oledPrint("SYSTEM READY", modeName());
    }
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== AttendESP Booting ===");

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);

  displayMutex = xSemaphoreCreateMutex();
  sdMutex = xSemaphoreCreateMutex();
  sdQueue = xQueueCreate(20, sizeof(LogEvent)); 

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(OLED_ADDR, true);
  oledPrint("AttendESP", "Booting Hardware...");

  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI);
  rfid.PCD_Init();

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, sdSPI)) {
    sdAvailable = true;
    if (!SD.exists("/attendance.csv")) {
      File f = SD.open("/attendance.csv", FILE_WRITE);
      if (f) { f.println("Time,Name,Matric Number,Logging Type"); f.close(); }
    }
    if (!SD.exists("/enrollment.csv")) {
      File f = SD.open("/enrollment.csv", FILE_WRITE);
      if (f) { f.println("Time,Name,Matric Number,RFID,Fingerprint ID"); f.close(); }
    }
  } else {
    oledPrint("SD WARNING", "Not Found!");
    delay(2000);
  }

  fpSerial.begin(57600, SERIAL_8N1, FP_RX, FP_TX);
  finger.begin(57600);

  while (fpSerial.available()) {
    fpSerial.read(); 
  }

  bool fpFound = false;
  for (int i = 0; i < 3; i++) {
    if (finger.verifyPassword()) {
      fpFound = true;
      break; 
    }
    delay(200); 
  }

  if (!fpFound) {
    oledPrint("FP WARNING", "Not Found!");
    delay(2000);
  }

  loadStudentsFromSD();

  xTaskCreatePinnedToCore(sdWriteTask, "SDTask", 8000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(ntpSyncTask, "NTPTask", 4000, NULL, 1, NULL, 0);

  oledPrint("SYSTEM READY", modeName());
}

// ================= CORE 1: MAIN LOOP =================
void loop() {
  
  // --- NEW: HANDLE CLOUD SYNC MODE ---
  if (currentMode == MODE_SYNC) {
    syncDataToCloud(); 
    return; // Skip the rest of the loop for this cycle
  }

// --- A. HANDLE QR & BLE MODE ---
  if (currentMode == MODE_QR_BLE) {
    
    // === THE FIX: Prevent Dual-Radio Hardware Crash ===
    // If the background Wi-Fi task is still running, wait for it!
    if (!isTimeSynced && !isWifiFailed) {
      oledPrint("RADIO BUSY", "Syncing time...", "Please wait...");
      delay(500);
      return; // Skip the rest of the loop until Wi-Fi releases the antenna
    }

    // Now it is 100% safe to turn on Bluetooth
    if (!bleReady) {
      WiFi.disconnect(true); // Double-tap the Wi-Fi to ensure it is dead
      WiFi.mode(WIFI_OFF); 
      delay(50);
      initBLE(); 
    }

    if (currentSlotIndex == -1) { 
      currentQrBase64 = makeStaticQrBase64(); 
      drawQrCodeToDisplay(currentQrBase64); 
      currentSlotIndex = 1; 
    }
    
    handleBleRequestIfAny();
  }

  // --- B. HANDLE THE SKIP BUTTON (Fingerprint tap during Enrollment) ---
  if (currentMode == MODE_ENROLLMENT && enrollmentTargetIdx >= 0) {
    if (finger.getImage() == FINGERPRINT_OK) {
      beepClick();
      
      enrollmentTargetIdx = findNextUnenrolledStudent(enrollmentTargetIdx + 1);
      
      if (enrollmentTargetIdx >= 0) {
        oledPrint("SKIPPED", "Next:", String(students[enrollmentTargetIdx].name));
        delay(1000);
        oledPrint("ENROLL:", String(students[enrollmentTargetIdx].name), "Card=Save | FP=Skip");
      } else {
        oledPrint("ENROLLMENT", "All available", "students onboarded!");
        delay(2000);
        currentMode = MODE_ATTENDANCE;
        oledPrint("SYSTEM READY", modeName());
      }
      
      while(finger.getImage() != FINGERPRINT_NOFINGER) { delay(50); }
    }
  }

  // --- C. HANDLE RFID SCANS ---
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String scannedUID = getRFIDString();
    Serial.println("\nCard Tapped: " + scannedUID);

    if (scannedUID == MASTER_RFID_UID) {
      handleMasterNavigation();
      return; 
    }

    // === ENROLLMENT MODE (Save Card & Finger) ===
    if (currentMode == MODE_ENROLLMENT) {
      if (enrollmentTargetIdx < 0) {
        oledPrint("ENROLLMENT", "All students onboarded");
        delay(1500);
      } else {
        StudentRecord& t = students[enrollmentTargetIdx];
        if (findStudentByRfid(scannedUID) >= 0) { 
          oledPrint("RFID IN USE", "Try another card"); 
          beepError(); 
          delay(900); 
          oledPrint("ENROLL:", String(t.name), "Card=Save | FP=Skip");
        } else {
          oledPrint("CARD SAVED", "Matric: " + String(t.matric), "Setting up Finger...");
          beepSuccess();
          delay(1500);
          
          int newFingerID = enrollFingerprint();
          
          if (newFingerID != -1) {
            oledPrint("ENROLL SUCCESS!", "Card & Finger Linked", String(t.name));
            
            strlcpy(t.rfid, scannedUID.c_str(), sizeof(t.rfid));
            t.fingerprintId = newFingerID;
            saveStudentsToSD();

            LogEvent enrollLog;
            enrollLog.logType = 'E'; 
            strcpy(enrollLog.matric, t.matric);
            strcpy(enrollLog.name, t.name);
            strcpy(enrollLog.rfid, t.rfid);
            enrollLog.fp_id = newFingerID;
            enrollLog.scan_millis = millis(); 
            xQueueSend(sdQueue, &enrollLog, 0);
            
            beepSuccess();
            delay(1500);

            enrollmentTargetIdx = findNextUnenrolledStudent(enrollmentTargetIdx + 1);
            if (enrollmentTargetIdx >= 0) {
              oledPrint("ENROLL:", String(students[enrollmentTargetIdx].name), "Card=Save | FP=Skip");
            } else {
              oledPrint("ENROLLMENT", "List completed");
              delay(1500);
              currentMode = MODE_ATTENDANCE;
              oledPrint("SYSTEM READY", modeName());
            }

          } else {
            oledPrint("ENROLL FAILED", "Try again");
            delay(1500);
            oledPrint("ENROLL:", String(t.name), "Card=Save | FP=Skip");
          }
        }
      }
    }
    
// === STRICT 2FA ATTENDANCE MODE ===
    else if (currentMode == MODE_ATTENDANCE) {
      beepClick();
      
      int stuIdx = findStudentByRfid(scannedUID);
      if (stuIdx >= 0) {
        oledPrint("CARD VERIFIED", String(students[stuIdx].name), "Place Finger...");
        
        unsigned long fpTimeout = millis();
        int matchedFingerID = -1;
        bool sensorTouched = false; // NEW: Track if a physical touch happened
        
        while(millis() - fpTimeout < 5000) {
          if (finger.getImage() == FINGERPRINT_OK) {
            sensorTouched = true; // The sensor saw a finger!
            
            if (finger.image2Tz() == FINGERPRINT_OK) {
              if (finger.fingerSearch() == FINGERPRINT_OK) {
                matchedFingerID = finger.fingerID; // Finger is in the database
              }
              break; // Finger was processed (match or no match). Exit the loop instantly!
            }
          }
          delay(50);
        }

        // VERDICT 1: PERFECT MATCH
        if (matchedFingerID != -1 && matchedFingerID == students[stuIdx].fingerprintId) {
          oledPrint("2FA SUCCESS!", "Welcome,", String(students[stuIdx].name));
          beepSuccess();
          
          LogEvent newLog;
          newLog.logType = 'A'; 
          strcpy(newLog.matric, students[stuIdx].matric);
          strcpy(newLog.name, students[stuIdx].name);
          strcpy(newLog.method, "2FA");
          newLog.scan_millis = millis(); 
          xQueueSend(sdQueue, &newLog, 0);
          
        } 
        // VERDICT 2: UNIFIED REJECTION (Wrong person OR Not in database)
        else if (sensorTouched) {
          oledPrint("ACCESS DENIED", "ID MISMATCH", "Wrong fingerprint");
          beepError();
        } 
        // VERDICT 3: NO TOUCH AT ALL
        else {
          oledPrint("2FA TIMEOUT", "Finger not", "detected");
          beepError();
        }
        
      } else {
        oledPrint("ACCESS DENIED", "Unknown Card");
        beepError();
      }
      
      delay(2000);
      oledPrint("SYSTEM READY", modeName());
    }
  }

  delay(50);
}