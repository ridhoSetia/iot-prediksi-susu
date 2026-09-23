#include <Arduino.h>
#include <vector>
#include <algorithm>
#include <Wire.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MAX31865.h>

// =================================================================
// KONFIGURASI ENDPOINT GOOGLE SHEETS
// =================================================================
const char* GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbyGx5daEb_lkUq2WD2f-iD82yQpj9wLc-Y01VFf20ZO6zrfXdRb4MjHNG4K-7RXnFfr/exec";

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

enum SystemState {
  STATE_MENU_UTAMA,
  STATE_AMBIL_DATA_LIVE,
  STATE_AMBIL_DATA_BURST
};

SystemState currentState = STATE_MENU_UTAMA;

// Kursor Navigasi
int menuUtamaCursor = 0; // 0: Ambil Data, 1: Kirim CSV ke Cloud
int ambilDataCursor = 0; // 0: Ganti ID Wadah, 1: Rekam 5x, 2: Kembali

const char* SAMPLE_IDS[] = { "S_RUANG", "S_DINGIN", "S_HANGAT" };
const int TOTAL_SAMPLE_IDS = 3;
int currentSampleIdx = 0;

// =================================================================
// 1. PIN & HARDWARE SETUP
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
    valR = 255 - valR; valG = 255 - valG; valB = 255 - valB;
  }
  analogWrite(PIN_LED_RED, valR);
  analogWrite(PIN_LED_GREEN, valG);
  analogWrite(PIN_LED_BLUE, valB);
}

// Sensor EC (Custom AC Divider)
#define PIN_DRIVE_A 4
#define PIN_DRIVE_B 5
#define PIN_ADC_SENSE 1
const float R_REF = 1000.0;
const float V_IN = 3.3;
const float CAL_SLOPE  = 2.109942;
const float CAL_OFFSET = -2.321653;
const float ALPHA_TEMP = 0.020;
const int TOTAL_SAMPLES = 40;
const int TRIM_COUNT = 8;

// Sensor Suhu PT100 + MAX31865 SPI
Adafruit_MAX31865 thermo = Adafruit_MAX31865(10, 11, 12, 13);
#define RREF 426.0
#define RNOMINAL 100.0

float latestSuhu = 25.0;
ECReading latestEcData = { false, -1.0, 0.0, 0.0, 0.0 };
bool isStorageReady = false;
int csvLogCount = 0;

// =================================================================
// 2. LOGIKA AKUISISI SENSOR & LOG CSV LITTLEFS
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

void logBurstSample(int burstIdx) {
  if (!isStorageReady) return;
  bool fileBaru = !LittleFS.exists("/dataset_susu.csv");
  File dataFile = LittleFS.open("/dataset_susu.csv", FILE_APPEND);
  if (!dataFile) return;

  if (fileBaru || dataFile.size() == 0) {
    dataFile.println("sample_id,id,timestamp,burst_idx,temp_c,r_ohm,ec_raw,ec_25,submerged");
  }

  csvLogCount++;
  dataFile.printf("%s,%d,%lu,%d,%.2f,%.1f,%.3f,%.3f,%d\n",
                  SAMPLE_IDS[currentSampleIdx], csvLogCount, millis(), burstIdx,
                  latestSuhu, latestEcData.resistance, latestEcData.ecRaw,
                  latestEcData.ec25, latestEcData.isSubmerged ? 1 : 0);
  dataFile.close();
}

// =================================================================
// 3. LOGIKA PENGIRIMAN CSV KE GOOGLE SHEETS
// =================================================================
void showSendStatus(const char* l1, const char* l2, const char* l3) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(16, 0);
  display.print("SINKRONISASI CSV");
  display.drawLine(0, 9, 128, 9, SSD1306_WHITE);
  display.setCursor(0, 16); display.print(l1);
  display.setCursor(0, 28); display.print(l2);
  display.setCursor(0, 40); display.print(l3);
  display.drawLine(0, 52, 128, 52, SSD1306_WHITE);
  display.display();
}

bool connectWiFiWithManager() {
  showSendStatus("1. Cek Wi-Fi...", "Mencari jaringan", "tersimpan...");
  setRgbColor(false, false, true);

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  wm.setAPCallback([](WiFiManager *myWiFiManager) {
    setRgbColor(true, true, false);
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(8, 0); display.print("WIFI TIDAK ADA!");
    display.drawLine(0, 9, 128, 9, SSD1306_WHITE);
    display.setCursor(0, 14); display.print("1. Konek Wi-Fi HP:");
    display.setCursor(0, 24); display.print("   SSID: MILK-SETUP");
    display.setCursor(0, 36); display.print("2. Buka Browser HP:");
    display.setCursor(0, 46); display.print("   IP: 192.168.4.1");
    display.drawLine(0, 55, 128, 55, SSD1306_WHITE);
    display.display();
  });

  bool res = wm.autoConnect("MILK-SETUP");
  if (!res) {
    setRgbColor(true, false, false);
    showSendStatus("Gagal Terhubung!", "Waktu Habis", "Batal Kirim");
    delay(2000);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    setRgbColor(false, false, false);
    return false;
  }
  return true;
}

void sendCsvDataOnly() {
  if (!LittleFS.exists("/dataset_susu.csv") || csvLogCount == 0) {
    showSendStatus("CSV Kosong!", "Belum ada dataset", "untuk dikirim.");
    delay(2000);
    return;
  }

  if (!connectWiFiWithManager()) return;

  showSendStatus("2. Kirim CSV...", "Ke Google Sheets", "Membaca berkas...");
  File fCsv = LittleFS.open("/dataset_susu.csv", FILE_READ);
  String payloadCsv = "{\"records\":[";
  bool firstLine = true, firstRow = true;
  int count = 0;

  while (fCsv.available()) {
    String line = fCsv.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (firstLine) { firstLine = false; continue; }

    if (!firstRow) payloadCsv += ",";
    firstRow = false;

    payloadCsv += "[";
    int startIdx = 0, colIdx = 0;
    while (startIdx < line.length()) {
      int commaIdx = line.indexOf(',', startIdx);
      if (commaIdx == -1) commaIdx = line.length();
      String token = line.substring(startIdx, commaIdx);
      token.trim();

      if (colIdx > 0) payloadCsv += ",";
      if (colIdx == 0) payloadCsv += "\"" + token + "\"";
      else payloadCsv += token;

      colIdx++;
      startIdx = commaIdx + 1;
    }
    payloadCsv += "]";
    count++;
  }
  fCsv.close();
  payloadCsv += "]}";

  WiFiClientSecure secClient;
  secClient.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.begin(secClient, GOOGLE_SCRIPT_URL);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(payloadCsv);
  if (code == 200 || code == 302) {
    LittleFS.remove("/dataset_susu.csv");
    csvLogCount = 0;
    setRgbColor(false, true, false);
    showSendStatus("SUKSES CSV!", "Data Masuk Sheets", "Flash CSV Bersih");
  } else {
    setRgbColor(true, false, false);
    showSendStatus("Gagal Kirim CSV!", ("HTTP: " + String(code)).c_str(), "Data Tetap Tersimpan");
  }
  delay(2500);
  http.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  setRgbColor(false, false, false);
}

// =================================================================
// 4. LOGIKA TOMBOL & ANTARMUKA OLED
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
      display.setCursor(16, 0); display.print("= MENU LOG DATA =");
      display.drawLine(0, 9, 128, 9, SSD1306_WHITE);
      display.setCursor(10, 18); display.print(menuUtamaCursor == 0 ? "> 1. Ambil Data Susu" : "  1. Ambil Data Susu");
      display.setCursor(10, 32); display.print(menuUtamaCursor == 1 ? "> 2. Kirim CSV Cloud" : "  2. Kirim CSV Cloud");
      display.drawLine(0, 50, 128, 50, SSD1306_WHITE);
      display.setCursor(0, 54); display.print("Klik:Pindah Than:Pilih");
      break;

    case STATE_AMBIL_DATA_LIVE:
      display.setTextSize(1);
      display.setCursor(10, 0); display.print("PENGAMBILAN DATASET");
      display.drawLine(0, 9, 128, 9, SSD1306_WHITE);
      display.setCursor(0, 12);
      if (ambilDataCursor == 0) display.printf("> ID : <%s>", SAMPLE_IDS[currentSampleIdx]);
      else display.printf("  ID : %s", SAMPLE_IDS[currentSampleIdx]);
      display.setCursor(0, 22); display.printf("  Suhu : %.2f C", latestSuhu);
      display.setCursor(0, 32);
      if (latestEcData.isSubmerged) display.printf("  EC25 : %.3f mS", latestEcData.ec25);
      else display.print("  EC25 : -- (KERING)");
      display.setCursor(0, 42); display.printf("  CSV  : #%d Baris", csvLogCount);
      display.drawLine(0, 51, 128, 51, SSD1306_WHITE);
      display.setCursor(4, 54); display.print(ambilDataCursor == 1 ? "[*Rekam]" : "[Rekam]");
      display.setCursor(68, 54); display.print(ambilDataCursor == 2 ? "[*Kembali]" : "[Kembali]");
      break;

    case STATE_AMBIL_DATA_BURST:
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
    if (LittleFS.exists("/dataset_susu.csv")) {
      File f = LittleFS.open("/dataset_susu.csv", FILE_READ);
      while (f.available()) {
        if (f.read() == '\n') csvLogCount++;
      }
      f.close();
      if (csvLogCount > 0) csvLogCount--; // Kurangi baris header
    }
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

  // Sampling sensor background tiap 1 detik
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
          ambilDataCursor = 0;
          currentState = STATE_AMBIL_DATA_LIVE;
        } else {
          sendCsvDataOnly(); // Kirim CSV ke Google Sheets
          currentState = STATE_MENU_UTAMA;
        }
      }
      break;

    case STATE_AMBIL_DATA_LIVE:
      if (actionShortClick) ambilDataCursor = (ambilDataCursor + 1) % 3;
      if (actionLongPress) {
        if (ambilDataCursor == 0) {
          currentSampleIdx = (currentSampleIdx + 1) % TOTAL_SAMPLE_IDS;
        } else if (ambilDataCursor == 1) {
          currentState = STATE_AMBIL_DATA_BURST;
        } else {
          currentState = STATE_MENU_UTAMA;
        }
      }
      break;

    case STATE_AMBIL_DATA_BURST:
      setRgbColor(false, false, true); // Indikator biru menyala selama rekam
      for (int i = 1; i <= 5; i++) {
        latestSuhu = readPT100Temperature();
        latestEcData = getCalibratedEC(latestSuhu);
        logBurstSample(i);

        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(6, 4); display.printf("REKAM: %s", SAMPLE_IDS[currentSampleIdx]);
        display.drawLine(0, 14, 128, 14, SSD1306_WHITE);
        display.setCursor(4, 20); display.print("Jangan Angkat Probe!");
        display.setCursor(4, 34); display.printf("Burst : %d / 5", i);
        display.setCursor(4, 46); display.printf("Suhu  : %.2f C", latestSuhu);
        display.setCursor(4, 56); display.printf("EC25  : %.3f mS/cm", latestEcData.ec25);
        display.display();
        delay(1000);
      }
      setRgbColor(false, false, false);
      currentState = STATE_AMBIL_DATA_LIVE;
      break;
  }

  renderDisplay();
}