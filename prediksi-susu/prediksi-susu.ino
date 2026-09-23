#include <Arduino.h>
#include <vector>
#include <algorithm>
#include <Wire.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MAX31865.h>

// Model Bobot & Inferensi On-Device C++
#include "milk_model_weights.h"

// =================================================================
// KONFIGURASI JARINGAN & BACKEND FASTAPI
// =================================================================
const char* FASTAPI_LOG_URL = "http://10.42.165.140:7000/api/predict-log";

const char* DEVICE_ID = "FARMMERRY-001";
const char* ALAMAT_PETERNAK = "Farm Mery, Mugirejo, Kec. Sungai Pinang";
const float LATITUDE_POS = -0.4807238341289421;
const float LONGITUDE_POS = 117.20215448464293;

// =================================================================
// KONSTANTA NORMALISASI INPUT
// =================================================================
const float MIN_0 = 3.560000f;  // Suhu (°C)
const float SCALE_0 = 0.031867f;
const float MIN_1 = 253.300003f;  // R Liquid (Ohm)
const float SCALE_1 = 0.003509f;
const float MIN_2 = 1.598000f;  // EC Raw (mS/cm)
const float SCALE_2 = 0.226655f;
const float MIN_3 = 2.565000f;  // EC25 (mS/cm)
const float SCALE_3 = 0.359583f;
const float SHELF_MAX_MINUTES = 360.0f;

// =================================================================
// 0. STRUKTUR DATA & STATE MACHINE (FSM)
// =================================================================
struct ECReading {
  bool isSubmerged;
  float resistance;
  float conductance;
  float ecRaw;
  float ec25;
};

struct PredItem {
  String device_id;
  String alamat;
  float latitude;
  float longitude;
  float suhu;
  String grade;
  int sisa_waktu_menit;
};

std::vector<PredItem> predList;
int selectedPredIndex = -1;

enum SystemState {
  STATE_MENU_UTAMA,
  STATE_PREDIKSI_IDLE,
  STATE_PREDIKSI_PROCESS,
  STATE_PREDIKSI_RESULT,
  STATE_PRED_LIST_VIEW,
  STATE_PRED_ITEM_ACTION,
  STATE_PRED_PILIH_DISTRIBUSI
};

SystemState currentState = STATE_MENU_UTAMA;

int menuUtamaCursor = 0;       // 0: Prediksi Susu, 1: Log & Kirim
int prediksiResultCursor = 0;  // 0: Prediksi Lagi, 1: Kembali
int predListCursor = 0;
int itemActionCursor = 0;
int distribusiCursor = 0;

// =================================================================
// 1. PIN & HARDWARE
// =================================================================
#define OLED_SDA 8
#define OLED_SCL 9
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define BUTTON_PIN 7
unsigned long btnPressStartTime = 0;
bool isBtnPressed = false;
bool longPressTriggered = false;
const unsigned long HOLD_DURATION_MS = 350;
const unsigned long DEBOUNCE_DELAY_MS = 50;
bool actionShortClick = false;
bool actionLongPress = false;

#define PIN_LED_RED 15
#define PIN_LED_GREEN 16
#define PIN_LED_BLUE 17
const bool IS_COMMON_CATHODE = true;
const int LED_INTENSITY = 35;

void setRgbColor(bool r, bool g, bool b) {
  int valR = r ? LED_INTENSITY : 0;
  int valG = g ? LED_INTENSITY : 0;
  int valB = b ? LED_INTENSITY : 0;
  if (!IS_COMMON_CATHODE) {
    valR = 255 - valR;
    valG = 255 - valG;
    valB = 255 - valB;
  }
  analogWrite(PIN_LED_RED, valR);
  analogWrite(PIN_LED_GREEN, valG);
  analogWrite(PIN_LED_BLUE, valB);
}

// Sensor EC
#define PIN_DRIVE_A 4
#define PIN_DRIVE_B 5
#define PIN_ADC_SENSE 1
const float R_REF = 1000.0;
const float V_IN = 3.3;
const float CAL_SLOPE = 2.109942;
const float CAL_OFFSET = -2.321653;
const float ALPHA_TEMP = 0.020;
const int TOTAL_SAMPLES = 40;
const int TRIM_COUNT = 8;

Adafruit_MAX31865 thermo = Adafruit_MAX31865(10, 11, 12, 13);
#define RREF 426.0
#define RNOMINAL 100.0

float latestSuhu = 25.0;
ECReading latestEcData = { false, -1.0, 0.0, 0.0, 0.0 };
bool isStorageReady = false;

String resultGrade = "GRADE_A";
int resultShelfLifeMin = 360;

// =================================================================
// 2. LOGIKA BACA SENSOR & FLASH
// =================================================================
ECReading getCalibratedEC(float currentTemperature) {
  ECReading data;
  float samples[TOTAL_SAMPLES];

  for (int i = 0; i < TOTAL_SAMPLES; i++) {
    digitalWrite(PIN_DRIVE_A, HIGH);
    digitalWrite(PIN_DRIVE_B, LOW);
    delayMicroseconds(120);
    int rawADC = analogRead(PIN_ADC_SENSE);
    samples[i] = (rawADC / 4095.0) * V_IN;
    digitalWrite(PIN_DRIVE_A, LOW);
    digitalWrite(PIN_DRIVE_B, HIGH);
    delayMicroseconds(120);
    digitalWrite(PIN_DRIVE_A, LOW);
    digitalWrite(PIN_DRIVE_B, LOW);
    delay(2);
  }

  std::sort(samples, samples + TOTAL_SAMPLES);
  float sumValidVoltage = 0.0;
  int validCount = TOTAL_SAMPLES - (2 * TRIM_COUNT);
  for (int i = TRIM_COUNT; i < (TOTAL_SAMPLES - TRIM_COUNT); i++) {
    sumValidVoltage += samples[i];
  }
  float avgVout = sumValidVoltage / validCount;

  if (avgVout >= (V_IN - 0.05) || avgVout <= 0.02) {
    data.isSubmerged = false;
    data.resistance = -1.0;
    data.conductance = 0.0;
    data.ecRaw = 0.0;
    data.ec25 = 0.0;
    return data;
  }

  data.isSubmerged = true;
  data.resistance = R_REF * (avgVout / (V_IN - avgVout));
  data.conductance = 1000.0 / data.resistance;
  data.ecRaw = (CAL_SLOPE * data.conductance) + CAL_OFFSET;
  if (data.ecRaw < 0.0) data.ecRaw = 0.0;
  data.ec25 = data.ecRaw / (1.0 + ALPHA_TEMP * (currentTemperature - 25.0));
  return data;
}

float readPT100Temperature() {
  uint8_t fault = thermo.readFault();
  if (fault) {
    thermo.clearFault();
    return 25.0;
  }
  return thermo.temperature(RNOMINAL, RREF);
}

void logPredictionToFlash() {
  if (!isStorageReady) return;
  File dataFile = LittleFS.open("/prediksi_log.json", FILE_APPEND);
  if (!dataFile) return;

  char buf[384];
  snprintf(buf, sizeof(buf),
           "{\"device_id\":\"%s\",\"alamat\":\"%s\",\"latitude\":%.6f,\"longitude\":%.6f,\"suhu\":%.2f,\"grade\":\"%s\",\"sisa_waktu_menit\":%d}",
           DEVICE_ID, ALAMAT_PETERNAK, LATITUDE_POS, LONGITUDE_POS,
           latestSuhu, resultGrade.c_str(), resultShelfLifeMin);

  dataFile.println(buf);
  dataFile.close();
}

void loadPredListFromFlash() {
  predList.clear();
  if (!LittleFS.exists("/prediksi_log.json")) return;

  File f = LittleFS.open("/prediksi_log.json", FILE_READ);
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    char dev[32] = "", alm[128] = "", gr[32] = "";
    float lat = 0, lon = 0, sh = 0;
    int sisa = 0;

    int n = sscanf(line.c_str(),
                   "{\"device_id\":\"%[^\"]\",\"alamat\":\"%[^\"]\",\"latitude\":%f,\"longitude\":%f,\"suhu\":%f,\"grade\":\"%[^\"]\",\"sisa_waktu_menit\":%d}",
                   dev, alm, &lat, &lon, &sh, gr, &sisa);
    if (n >= 6) {
      PredItem item;
      item.device_id = String(dev);
      item.alamat = String(alm);
      item.latitude = lat;
      item.longitude = lon;
      item.suhu = sh;
      item.grade = String(gr);
      item.sisa_waktu_menit = sisa;
      predList.push_back(item);
    }
  }
  f.close();
}

void rewritePredListToFlash() {
  if (predList.empty()) {
    LittleFS.remove("/prediksi_log.json");
    return;
  }
  File f = LittleFS.open("/prediksi_log.json", FILE_WRITE);
  for (const auto& item : predList) {
    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"device_id\":\"%s\",\"alamat\":\"%s\",\"latitude\":%.6f,\"longitude\":%.6f,\"suhu\":%.2f,\"grade\":\"%s\",\"sisa_waktu_menit\":%d}",
             item.device_id.c_str(), item.alamat.c_str(), item.latitude, item.longitude,
             item.suhu, item.grade.c_str(), item.sisa_waktu_menit);
    f.println(buf);
  }
  f.close();
}

// =================================================================
// 3. SINKRONISASI FASTAPI
// =================================================================
void showSendStatus(const char* l1, const char* l2, const char* l3) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(16, 0);
  display.print("SINKRONISASI IOT");
  display.drawLine(0, 9, 128, 9, SSD1306_WHITE);
  display.setCursor(0, 16);
  display.print(l1);
  display.setCursor(0, 28);
  display.print(l2);
  display.setCursor(0, 40);
  display.print(l3);
  display.drawLine(0, 52, 128, 52, SSD1306_WHITE);
  display.display();
}

bool connectWiFiWithManager() {
  showSendStatus("1. Cek Wi-Fi...", "Mencari jaringan", "yang tersimpan...");
  setRgbColor(false, false, true);

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  wm.setAPCallback([](WiFiManager* myWiFiManager) {
    setRgbColor(true, true, false);
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(8, 0);
    display.print("WIFI TIDAK ADA!");
    display.drawLine(0, 9, 128, 9, SSD1306_WHITE);
    display.setCursor(0, 14);
    display.print("1. Konek Wi-Fi HP:");
    display.setCursor(0, 24);
    display.print("   SSID: MILK-SETUP");
    display.setCursor(0, 36);
    display.print("2. Buka Browser HP:");
    display.setCursor(0, 46);
    display.print("   IP: 192.168.4.1");
    display.drawLine(0, 55, 128, 55, SSD1306_WHITE);
    display.display();
  });

  bool res = wm.autoConnect("MILK-SETUP");
  if (!res) {
    setRgbColor(true, false, false);
    showSendStatus("Gagal Terhubung!", "Waktu Habis", "Kembali ke Menu");
    delay(2000);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    setRgbColor(false, false, false);
    return false;
  }
  return true;
}

void sendSinglePrediction(int index, const char* deliveryMode) {
  if (index < 0 || index >= predList.size()) return;
  if (!connectWiFiWithManager()) return;

  showSendStatus("2. Kirim JSON...", "Ke Web FastAPI", deliveryMode);

  PredItem item = predList[index];
  char rowBuf[450];
  snprintf(rowBuf, sizeof(rowBuf),
           "[{\"device_id\":\"%s\",\"alamat\":\"%s\",\"latitude\":%.6f,\"longitude\":%.6f,\"suhu\":%.2f,\"grade\":\"%s\",\"sisa_waktu_menit\":%d,\"metode_pengiriman\":\"%s\"}]",
           item.device_id.c_str(), item.alamat.c_str(), item.latitude, item.longitude,
           item.suhu, item.grade.c_str(), item.sisa_waktu_menit, deliveryMode);

  WiFiClient plainClient;
  HTTPClient http;
  http.setTimeout(5000);
  http.begin(plainClient, FASTAPI_LOG_URL);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST((uint8_t*)rowBuf, strlen(rowBuf));
  if (code == 200) {
    setRgbColor(false, true, false);
    showSendStatus("SUKSES TERKIRIM!", "Data Diterima Web", "Menghapus dari Flash");
    predList.erase(predList.begin() + index);
    rewritePredListToFlash();
  } else {
    setRgbColor(true, false, false);
    showSendStatus("Gagal Kirim JSON!", ("HTTP: " + String(code)).c_str(), "Data Tetap Tersimpan");
  }
  delay(2000);
  http.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  setRgbColor(false, false, false);
}

// =================================================================
// 4. LOGIKA TOMBOL & ANTARMUKA
// =================================================================
void handleButtonLogic() {
  actionShortClick = false;
  actionLongPress = false;
  int reading = digitalRead(BUTTON_PIN);

  if (reading == HIGH) {
    if (!isBtnPressed) {
      isBtnPressed = true;
      btnPressStartTime = millis();
      longPressTriggered = false;
    } else {
      unsigned long holdDuration = millis() - btnPressStartTime;
      if (holdDuration >= HOLD_DURATION_MS && !longPressTriggered) {
        longPressTriggered = true;
        actionLongPress = true;
      }
    }
  } else {
    if (isBtnPressed) {
      unsigned long pressDuration = millis() - btnPressStartTime;
      isBtnPressed = false;
      if (!longPressTriggered && pressDuration >= DEBOUNCE_DELAY_MS) {
        actionShortClick = true;
      }
    }
  }
}

void renderDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  switch (currentState) {
    case STATE_MENU_UTAMA:
      display.setTextSize(1);
      display.setCursor(20, 0);
      display.print("= MENU UTAMA =");
      display.drawLine(0, 9, 128, 9, SSD1306_WHITE);
      display.setCursor(10, 18);
      display.print(menuUtamaCursor == 0 ? "> 1. Prediksi Susu" : "  1. Prediksi Susu");
      display.setCursor(10, 32);
      display.print(menuUtamaCursor == 1 ? "> 2. Log & Kirim" : "  2. Log & Kirim");
      display.drawLine(0, 50, 128, 50, SSD1306_WHITE);
      display.setCursor(0, 54);
      display.print("Klik:Pindah Than:Pilih");
      break;

    case STATE_PRED_LIST_VIEW:
      {
        display.setTextSize(1);
        display.setCursor(6, 0);
        display.printf("LIST PREDIKSI (%d)", predList.size());
        display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

        int totalRows = predList.empty() ? 1 : (predList.size() + 2);
        int startIdx = max(0, min(predListCursor - 1, totalRows - 3));

        for (int i = 0; i < 3; i++) {
          int targetRow = startIdx + i;
          if (targetRow >= totalRows) break;

          int yPos = 13 + (i * 12);
          display.setCursor(0, yPos);
          bool isSelected = (targetRow == predListCursor);

          if (predList.empty()) {
            display.print(isSelected ? "> [Kembali]" : "  [Kembali]");
          } else if (targetRow < predList.size()) {
            PredItem item = predList[targetRow];
            String shortGrade = item.grade;
            shortGrade.replace("GRADE_", "GR_");
            display.printf("%s#%d %s %.1fC", isSelected ? ">" : " ", targetRow + 1, shortGrade.c_str(), item.suhu);
          } else if (targetRow == predList.size()) {
            display.print(isSelected ? "> [Hapus Semua]" : "  [Hapus Semua]");
          } else {
            display.print(isSelected ? "> [Kembali]" : "  [Kembali]");
          }
        }

        display.drawLine(0, 51, 128, 51, SSD1306_WHITE);
        display.setCursor(0, 54);
        display.print("Than:Pilih Item");
        break;
      }

    case STATE_PRED_ITEM_ACTION:
      {
        display.setTextSize(1);
        display.setCursor(14, 0);
        display.printf("DATA #%d TERPILIH", selectedPredIndex + 1);
        display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

        PredItem item = predList[selectedPredIndex];
        display.setCursor(0, 12);
        display.printf("%s | %.1fC | %dm", item.grade.c_str(), item.suhu, item.sisa_waktu_menit);

        display.setCursor(5, 24);
        display.print(itemActionCursor == 0 ? "> 1. Kirim Data" : "  1. Kirim Data");
        display.setCursor(5, 34);
        display.print(itemActionCursor == 1 ? "> 2. Hapus Data" : "  2. Hapus Data");
        display.setCursor(5, 44);
        display.print(itemActionCursor == 2 ? "> 3. Batal/Kembali" : "  3. Batal/Kembali");
        break;
      }

    case STATE_PRED_PILIH_DISTRIBUSI:
      display.setTextSize(1);
      display.setCursor(8, 0);
      display.print("METODE DISTRIBUSI");
      display.drawLine(0, 9, 128, 9, SSD1306_WHITE);
      display.setCursor(5, 14);
      display.print(distribusiCursor == 0 ? "> 1. Dijemput KUD" : "  1. Dijemput KUD");
      display.setCursor(5, 26);
      display.print(distribusiCursor == 1 ? "> 2. Diantar Sendiri" : "  2. Diantar Sendiri");
      display.setCursor(5, 38);
      display.print(distribusiCursor == 2 ? "> 3. Batal" : "  3. Batal");
      display.drawLine(0, 50, 128, 50, SSD1306_WHITE);
      display.setCursor(0, 54);
      display.print("Pilih opsi distribusi");
      break;

    case STATE_PREDIKSI_IDLE:
      display.setTextSize(1);
      display.setCursor(15, 0);
      display.print("PREDIKSI MUTU SUSU");
      display.drawLine(0, 9, 128, 9, SSD1306_WHITE);
      display.setCursor(0, 14);
      display.printf("Suhu : %.2f C", latestSuhu);
      display.setCursor(0, 25);
      display.printf("EC25 : %.3f mS/cm", latestEcData.isSubmerged ? latestEcData.ec25 : 0.0);
      display.setCursor(0, 36);
      display.print(latestEcData.isSubmerged ? "Probe: TERENDAM" : "Probe: KERING/UDARA");
      display.drawLine(0, 48, 128, 48, SSD1306_WHITE);
      display.setCursor(2, 53);
      display.print(prediksiResultCursor == 0 ? "[*Prediksi] [Kembali]" : "[Prediksi] [*Kembali]");
      break;

    case STATE_PREDIKSI_PROCESS:
      display.setTextSize(1);
      display.setCursor(10, 15);
      display.print("Menganalisis Susu...");
      display.drawRect(14, 32, 100, 10, SSD1306_WHITE);
      display.fillRect(16, 34, 96, 6, SSD1306_WHITE);
      display.setCursor(15, 48);
      display.print("Inferensi AI...");
      break;

    case STATE_PREDIKSI_RESULT:
      display.setTextSize(1);
      display.setCursor(24, 0);
      display.print("HASIL ANALISIS");
      display.drawLine(0, 9, 128, 9, SSD1306_WHITE);
      display.setCursor(0, 14);
      display.printf("Status: %s", resultGrade.c_str());
      display.setCursor(0, 25);
      display.printf("Sisa  : %d Menit", resultShelfLifeMin);
      display.setCursor(0, 36);
      display.printf("T:%.1fC EC25:%.2f", latestSuhu, latestEcData.ec25);
      display.drawLine(0, 48, 128, 48, SSD1306_WHITE);
      display.setCursor(2, 53);
      display.print(prediksiResultCursor == 0 ? "[*Prediksi] [Kembali]" : "[Prediksi] [*Kembali]");
      break;
  }

  if (isBtnPressed && !longPressTriggered) {
    unsigned long holdTime = millis() - btnPressStartTime;
    int barWidth = map(constrain(holdTime, 0, HOLD_DURATION_MS), 0, HOLD_DURATION_MS, 0, 128);
    display.fillRect(0, 62, barWidth, 2, SSD1306_WHITE);
  }

  display.display();
}

// =================================================================
// 5. SETUP & MAIN LOOP
// =================================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  setRgbColor(false, false, false);

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(100000);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false)) {
    display.clearDisplay();
    display.display();
  }

  thermo.begin(MAX31865_2WIRE);

  if (LittleFS.begin(true)) {
    isStorageReady = true;
  }

  pinMode(PIN_DRIVE_A, OUTPUT);
  pinMode(PIN_DRIVE_B, OUTPUT);
  pinMode(PIN_ADC_SENSE, INPUT);
  digitalWrite(PIN_DRIVE_A, LOW);
  digitalWrite(PIN_DRIVE_B, LOW);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
}

void loop() {
  static unsigned long lastSensorRead = 0;
  unsigned long now = millis();

  if (now - lastSensorRead >= 1000) {
    lastSensorRead = now;
    latestSuhu = readPT100Temperature();
    latestEcData = getCalibratedEC(latestSuhu);
  }

  handleButtonLogic();

  switch (currentState) {
    case STATE_MENU_UTAMA:
      if (actionShortClick) menuUtamaCursor = (menuUtamaCursor + 1) % 2;
      if (actionLongPress) {
        if (menuUtamaCursor == 0) {
          prediksiResultCursor = 0;
          currentState = STATE_PREDIKSI_IDLE;
        } else {
          loadPredListFromFlash();
          predListCursor = 0;
          currentState = STATE_PRED_LIST_VIEW;
        }
      }
      break;

    case STATE_PREDIKSI_IDLE:
      if (actionShortClick) prediksiResultCursor = (prediksiResultCursor + 1) % 2;
      if (actionLongPress) {
        if (prediksiResultCursor == 0) currentState = STATE_PREDIKSI_PROCESS;
        else {
          setRgbColor(false, false, false);
          currentState = STATE_MENU_UTAMA;
        }
      }
      break;

    case STATE_PREDIKSI_PROCESS:
      {
        renderDisplay();
        setRgbColor(false, false, true);

        delay(500);
        latestSuhu = readPT100Temperature();
        latestEcData = getCalibratedEC(latestSuhu);

        if (!latestEcData.isSubmerged) {
          resultGrade = "KERING";
          resultShelfLifeMin = 0;
          setRgbColor(true, false, false);
        } else {
          // Normalisasi Min-Max
          float in_suhu = (latestSuhu - MIN_0) * SCALE_0;
          float in_rohm = (latestEcData.resistance - MIN_1) * SCALE_1;
          float in_ecraw = (latestEcData.ecRaw - MIN_2) * SCALE_2;
          float in_ec25 = (latestEcData.ec25 - MIN_3) * SCALE_3;

          // Eksekusi Inferensi C++ Murni (Latensi < 1 ms)
          predictMilkModel(in_suhu, in_rohm, in_ecraw, in_ec25,
                           resultGrade, resultShelfLifeMin, SHELF_MAX_MINUTES);

          // LED Feedback
          if (resultGrade == "GRADE_A") setRgbColor(false, true, false);
          else if (resultGrade == "GRADE_B") setRgbColor(true, true, false);
          else setRgbColor(true, false, false);
        }

        logPredictionToFlash();
        prediksiResultCursor = 0;
        currentState = STATE_PREDIKSI_RESULT;
        break;
      }

    case STATE_PREDIKSI_RESULT:
      if (actionShortClick) prediksiResultCursor = (prediksiResultCursor + 1) % 2;
      if (actionLongPress) {
        if (prediksiResultCursor == 0) currentState = STATE_PREDIKSI_PROCESS;
        else {
          setRgbColor(false, false, false);
          currentState = STATE_MENU_UTAMA;
        }
      }
      break;

    case STATE_PRED_LIST_VIEW:
      {
        int totalOptions = predList.empty() ? 1 : (predList.size() + 2);
        if (actionShortClick) {
          predListCursor = (predListCursor + 1) % totalOptions;
        }
        if (actionLongPress) {
          if (predList.empty()) {
            currentState = STATE_MENU_UTAMA;
          } else if (predListCursor < predList.size()) {
            selectedPredIndex = predListCursor;
            itemActionCursor = 0;
            currentState = STATE_PRED_ITEM_ACTION;
          } else if (predListCursor == predList.size()) {
            predList.clear();
            LittleFS.remove("/prediksi_log.json");
            predListCursor = 0;
          } else {
            currentState = STATE_MENU_UTAMA;
          }
        }
        break;
      }

    case STATE_PRED_ITEM_ACTION:
      if (actionShortClick) itemActionCursor = (itemActionCursor + 1) % 3;
      if (actionLongPress) {
        if (itemActionCursor == 0) {
          distribusiCursor = 0;
          currentState = STATE_PRED_PILIH_DISTRIBUSI;
        } else if (itemActionCursor == 1) {
          predList.erase(predList.begin() + selectedPredIndex);
          rewritePredListToFlash();
          predListCursor = 0;
          currentState = STATE_PRED_LIST_VIEW;
        } else {
          currentState = STATE_PRED_LIST_VIEW;
        }
      }
      break;

    case STATE_PRED_PILIH_DISTRIBUSI:
      if (actionShortClick) distribusiCursor = (distribusiCursor + 1) % 3;
      if (actionLongPress) {
        if (distribusiCursor == 0) {
          sendSinglePrediction(selectedPredIndex, "DIJEMPUT");
          currentState = STATE_PRED_LIST_VIEW;
        } else if (distribusiCursor == 1) {
          sendSinglePrediction(selectedPredIndex, "ANTAR");
          currentState = STATE_PRED_LIST_VIEW;
        } else {
          currentState = STATE_PRED_ITEM_ACTION;
        }
      }
      break;
  }

  renderDisplay();
}