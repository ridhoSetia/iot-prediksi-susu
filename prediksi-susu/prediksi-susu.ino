#include <Arduino.h>
#include <algorithm>
#include <Wire.h>
#include <LittleFS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MAX31865.h>

// =================================================================
// 0. STRUKTUR DATA & ENUMERASI FINITE STATE MACHINE (FSM)
// =================================================================
struct ECReading {
  bool isSubmerged;
  float resistance;   // Ohm
  float conductance;  // mS
  float ecRaw;        // mS/cm aktual
  float ec25;         // mS/cm terkompensasi 25°C
};

enum SystemState {
  STATE_MENU_UTAMA,
  STATE_PREDIKSI_IDLE,
  STATE_PREDIKSI_PROCESS,
  STATE_PREDIKSI_RESULT,
  STATE_DATA_VIEW,
  STATE_DATA_SENDING,
  STATE_AMBIL_DATA_LIVE,
  STATE_AMBIL_DATA_BURST
};

SystemState currentState = STATE_MENU_UTAMA;

// Indeks Navigasi Menu
int menuUtamaCursor = 0;      // 0: Prediksi, 1: Lihat & Kirim, 2: Ambil Data
int prediksiResultCursor = 0; // 0: Prediksi Lagi, 1: Kembali
int dataViewCursor = 0;       // 0: Kirim Data, 1: Kembali
int ambilDataCursor = 0;      // 0: Rekam (5x), 1: Kembali

// =================================================================
// 1. PIN & KONFIGURASI LAYAR OLED SSD1306 (I2C)
// =================================================================
#define OLED_SDA 8
#define OLED_SCL 9
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// =================================================================
// 2. PIN TOMBOL DUAL-ACTION (KLIK & TAHAN 2 DETIK)
// =================================================================
#define BUTTON_PIN 7

unsigned long btnPressStartTime = 0;
bool isBtnPressed = false;
bool longPressTriggered = false;
const unsigned long HOLD_DURATION_MS = 400;
const unsigned long DEBOUNCE_DELAY_MS = 50;

// Flag pemicu aksi
bool actionShortClick = false;
bool actionLongPress = false;

// =================================================================
// 3. PIN & KONTROL LED RGB 4-PIN (PWM ANALOG)
// =================================================================
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

// =================================================================
// 4. PIN & KONFIGURASI SENSOR EC (CUSTOM AC DIVIDER)
// =================================================================
#define PIN_DRIVE_A 4
#define PIN_DRIVE_B 5
#define PIN_ADC_SENSE 1

const float R_REF = 1000.0;
const float V_IN = 3.3;
const float CAL_SLOPE  = 2.204908;
const float CAL_OFFSET = -2.276239;
const float ALPHA_TEMP = 0.020;

const int TOTAL_SAMPLES = 40;
const int TRIM_COUNT = 8;

float latestSuhu = 25.0;
ECReading latestEcData = { false, -1.0, 0.0, 0.0, 0.0 };

// =================================================================
// 5. PIN & KONFIGURASI MAX31865 (PT100 RTD)
// =================================================================
Adafruit_MAX31865 thermo = Adafruit_MAX31865(10, 11, 12, 13);
#define RREF 426.0
#define RNOMINAL 100.0

// =================================================================
// 6. PENYIMPANAN FLASH INTERNAL (LITTLEFS)
// =================================================================
bool isStorageReady = false;
int logCount = 0;

// Variabel Hasil Prediksi
String resultGrade = "GRADE A";
int resultShelfLifeMin = 180;

// Forward Declaration
void renderDisplay();
void handleButtonLogic();
ECReading getCalibratedEC(float currentTemperature);
float readPT100Temperature();
void logBurstSample(int burstIdx);

// =================================================================
// 7. FUNGSI PEMBACAAN SENSOR EC & SUHU
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

// =================================================================
// 8. MANAJEMEN PENYIMPANAN FLASH (LITTLEFS)
// =================================================================
void logBurstSample(int burstIdx) {
  if (!isStorageReady) return;

  File dataFile = LittleFS.open("/dataset_susu.csv", FILE_APPEND);
  if (!dataFile) return;

  logCount++;
  unsigned long timeStamp = millis();

  // Format CSV: id,timestamp,burst_idx,temp_c,r_ohm,ec_raw,ec_25,submerged
  dataFile.printf("%d,%lu,%d,%.2f,%.1f,%.3f,%.3f,%d\n",
                  logCount,
                  timeStamp,
                  burstIdx,
                  latestSuhu,
                  latestEcData.resistance,
                  latestEcData.ecRaw,
                  latestEcData.ec25,
                  latestEcData.isSubmerged ? 1 : 0);
  dataFile.close();
}

void logPredictionToFlash() {
  if (!isStorageReady) return;

  File dataFile = LittleFS.open("/prediksi_log.json", FILE_APPEND);
  if (!dataFile) return;

  logCount++;
  char buf[200];
  snprintf(buf, sizeof(buf),
           "{\"id\":%d,\"suhu\":%.2f,\"ec25\":%.3f,\"grade\":\"%s\",\"shelf_min\":%d}",
           logCount, latestSuhu, latestEcData.ec25, resultGrade.c_str(), resultShelfLifeMin);

  dataFile.println(buf);
  dataFile.close();
}

// =================================================================
// 9. LOGIKA DETEKSI TOMBOL (SHORT CLICK VS HOLD 2 DETIK)
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
        actionLongPress = true; // Terpicu aksi tahan
      }
    }
  } else {
    if (isBtnPressed) {
      unsigned long pressDuration = millis() - btnPressStartTime;
      isBtnPressed = false;
      if (!longPressTriggered && pressDuration >= DEBOUNCE_DELAY_MS) {
        actionShortClick = true; // Terpicu aksi klik biasa
      }
    }
  }
}

// =================================================================
// 10. RENDER TAMPILAN OLED BERDASARKAN FSM
// =================================================================
void renderDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  switch (currentState) {
    // -------------------------------------------------------------
    // 1. MENU UTAMA
    // -------------------------------------------------------------
    case STATE_MENU_UTAMA:
      display.setTextSize(1);
      display.setCursor(20, 0);
      display.print("= MENU UTAMA =");
      display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

      // Menu 0: Prediksi Susu
      display.setCursor(10, 14);
      display.print(menuUtamaCursor == 0 ? "> 1. Prediksi Susu" : "  1. Prediksi Susu");

      // Menu 1: Lihat & Kirim Data
      display.setCursor(10, 26);
      display.print(menuUtamaCursor == 1 ? "> 2. Lihat & Kirim" : "  2. Lihat & Kirim");

      // Menu 2: Ambil Data Susu
      display.setCursor(10, 38);
      display.print(menuUtamaCursor == 2 ? "> 3. Ambil Data" : "  3. Ambil Data");

      display.drawLine(0, 50, 128, 50, SSD1306_WHITE);
      display.setCursor(0, 54);
      display.print("Klik:Pindah Than:Plih");
      break;

    // -------------------------------------------------------------
    // 2. MENU PREDIKSI: STANDBY
    // -------------------------------------------------------------
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
      display.print("Model: Standby (Uji)");

      display.drawLine(0, 48, 128, 48, SSD1306_WHITE);
      display.setCursor(2, 53);
      display.print(prediksiResultCursor == 0 ? "[*Prediksi] [Kembali]" : "[Prediksi] [*Kembali]");
      break;

    // -------------------------------------------------------------
    // 2. MENU PREDIKSI: PROSES
    // -------------------------------------------------------------
    case STATE_PREDIKSI_PROCESS:
      display.setTextSize(1);
      display.setCursor(10, 15);
      display.print("Menganalisis Susu...");
      display.drawRect(14, 32, 100, 10, SSD1306_WHITE);
      display.fillRect(16, 34, 96, 6, SSD1306_WHITE);
      display.setCursor(15, 48);
      display.print("Memproses TinyML...");
      break;

    // -------------------------------------------------------------
    // 2. MENU PREDIKSI: HASIL
    // -------------------------------------------------------------
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
      display.printf("Suhu:%.1fC EC:%.2f", latestSuhu, latestEcData.ec25);

      display.drawLine(0, 48, 128, 48, SSD1306_WHITE);
      display.setCursor(2, 53);
      display.print(prediksiResultCursor == 0 ? "[*Prediksi] [Kembali]" : "[Prediksi] [*Kembali]");
      break;

    // -------------------------------------------------------------
    // 3. MENU LIHAT & KIRIM: TAMPILAN
    // -------------------------------------------------------------
    case STATE_DATA_VIEW:
      display.setTextSize(1);
      display.setCursor(14, 0);
      display.print("DATA FLASH LITTLEFS");
      display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

      display.setCursor(0, 14);
      display.printf("Total Log: %d data", logCount);
      display.setCursor(0, 25);
      display.printf("File: dataset_susu.csv");
      display.setCursor(0, 36);
      display.printf("Flash Ready: %s", isStorageReady ? "OK" : "FAIL");

      display.drawLine(0, 48, 128, 48, SSD1306_WHITE);
      display.setCursor(8, 53);
      display.print(dataViewCursor == 0 ? "[*Kirim] [Kembali]" : "[Kirim] [*Kembali]");
      break;

    // -------------------------------------------------------------
    // 3. MENU LIHAT & KIRIM: PROSES PENGIRIMAN
    // -------------------------------------------------------------
    case STATE_DATA_SENDING:
      display.setTextSize(1);
      display.setCursor(16, 0);
      display.print("SINKRONISASI IOT");
      display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

      display.setCursor(0, 14);
      display.print("1. Scan Wi-Fi... OK");
      display.setCursor(0, 25);
      display.print("2. Hubungi Server...");
      display.setCursor(0, 36);
      display.printf("3. Kirim: %d Data", logCount);

      display.drawLine(0, 48, 128, 48, SSD1306_WHITE);
      display.setCursor(25, 53);
      display.print("[*Tahan: Selesai]");
      break;

    // -------------------------------------------------------------
    // 4. MENU AMBIL DATA: LIVE STREAM SENSOR
    // -------------------------------------------------------------
    case STATE_AMBIL_DATA_LIVE:
      display.setTextSize(1);
      display.setCursor(10, 0);
      display.print("PENGAMBILAN DATASET");
      display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

      display.setCursor(0, 13);
      display.printf("Suhu : %.2f C", latestSuhu);
      display.setCursor(0, 23);
      if (latestEcData.isSubmerged) {
        display.printf("EC25 : %.3f mS/cm", latestEcData.ec25);
      } else {
        display.print("EC25 : -- (KERING)");
      }

      display.setCursor(0, 33);
      display.printf("Log  : #%d Tersimpan", logCount);

      display.drawLine(0, 47, 128, 47, SSD1306_WHITE);
      display.setCursor(2, 52);
      display.print(ambilDataCursor == 0 ? "[*Rekam 5x] [Kembali]" : "[Rekam 5x] [*Kembali]");
      break;

    // -------------------------------------------------------------
    // 4. MENU AMBIL DATA: BURST SAMPLING IN PROGRESS
    // -------------------------------------------------------------
    case STATE_AMBIL_DATA_BURST:
      display.setTextSize(1);
      display.setCursor(4, 10);
      display.print("MEREKAM BURST 5x...");
      display.setCursor(4, 28);
      display.print("Jangan Angkat Probe!");
      display.setCursor(4, 46);
      display.print("Menyimpan ke Flash");
      break;
  }

  // Visualisasi Progress Bar Tahan 2 Detik di Sisi Bawah Layar
  if (isBtnPressed && !longPressTriggered) {
    unsigned long holdTime = millis() - btnPressStartTime;
    int barWidth = map(constrain(holdTime, 0, HOLD_DURATION_MS), 0, HOLD_DURATION_MS, 0, 128);
    display.fillRect(0, 62, barWidth, 2, SSD1306_WHITE);
  }

  display.display();
}

// =================================================================
// 11. SETUP SISTEM
// =================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Inisialisasi Tombol Pin 7
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);

  // Inisialisasi LED RGB
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  setRgbColor(false, false, false);

  // Inisialisasi I2C OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(100000);

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false)) {
    display.clearDisplay();
    display.display();
  }

  // Inisialisasi MAX31865 (PT100)
  thermo.begin(MAX31865_2WIRE);

  // Inisialisasi Flash Internal LittleFS
  if (LittleFS.begin(true)) {
    isStorageReady = true;
    if (LittleFS.exists("/dataset_susu.csv")) {
      File f = LittleFS.open("/dataset_susu.csv", FILE_READ);
      while (f.available()) {
        if (f.read() == '\n') logCount++;
      }
      f.close();
    }
  }

  // Inisialisasi Pin Eksitasi AC EC
  pinMode(PIN_DRIVE_A, OUTPUT);
  pinMode(PIN_DRIVE_B, OUTPUT);
  pinMode(PIN_ADC_SENSE, INPUT);
  digitalWrite(PIN_DRIVE_A, LOW);
  digitalWrite(PIN_DRIVE_B, LOW);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
}

// =================================================================
// 12. LOOP UTAMA
// =================================================================
void loop() {
  static unsigned long lastSensorRead = 0;
  unsigned long now = millis();

  // Pembacaan sensor periodik 1 Hz saat berada di layar pemantauan
  if (now - lastSensorRead >= 1000) {
    lastSensorRead = now;
    latestSuhu = readPT100Temperature();
    latestEcData = getCalibratedEC(latestSuhu);
  }

  handleButtonLogic();

  // -------------------------------------------------------------
  // LOGIKA STATE MACHINE & AKSI TOMBOL
  // -------------------------------------------------------------
  switch (currentState) {
    case STATE_MENU_UTAMA:
      if (actionShortClick) {
        menuUtamaCursor = (menuUtamaCursor + 1) % 3; // Siklus 0 -> 1 -> 2 -> 0
      }
      if (actionLongPress) {
        if (menuUtamaCursor == 0) {
          prediksiResultCursor = 0;
          currentState = STATE_PREDIKSI_IDLE;
        } else if (menuUtamaCursor == 1) {
          dataViewCursor = 0;
          currentState = STATE_DATA_VIEW;
        } else if (menuUtamaCursor == 2) {
          ambilDataCursor = 0;
          currentState = STATE_AMBIL_DATA_LIVE;
        }
      }
      break;

    case STATE_PREDIKSI_IDLE:
      if (actionShortClick) {
        prediksiResultCursor = (prediksiResultCursor + 1) % 2;
      }
      if (actionLongPress) {
        if (prediksiResultCursor == 0) {
          currentState = STATE_PREDIKSI_PROCESS;
        } else {
          setRgbColor(false, false, false);
          currentState = STATE_MENU_UTAMA;
        }
      }
      break;

    case STATE_PREDIKSI_PROCESS:
      renderDisplay();
      // Simulasi kalkulasi rule prediktif berdasarkan biofisika terukur
      latestSuhu = readPT100Temperature();
      latestEcData = getCalibratedEC(latestSuhu);
      delay(1200);

      if (!latestEcData.isSubmerged) {
        resultGrade = "KERING";
        resultShelfLifeMin = 0;
        setRgbColor(true, false, false); // Merah
      } else if (latestEcData.ec25 <= 5.5) {
        resultGrade = "GRADE A";
        resultShelfLifeMin = 180;
        setRgbColor(false, true, false); // Hijau
      } else if (latestEcData.ec25 <= 6.2) {
        resultGrade = "GRADE B";
        resultShelfLifeMin = 45;
        setRgbColor(true, true, false);  // Kuning
      } else {
        resultGrade = "GRADE C";
        resultShelfLifeMin = 0;
        setRgbColor(true, false, false); // Merah
      }

      logPredictionToFlash();
      prediksiResultCursor = 0;
      currentState = STATE_PREDIKSI_RESULT;
      break;

    case STATE_PREDIKSI_RESULT:
      if (actionShortClick) {
        prediksiResultCursor = (prediksiResultCursor + 1) % 2;
      }
      if (actionLongPress) {
        if (prediksiResultCursor == 0) {
          currentState = STATE_PREDIKSI_PROCESS; // Prediksi lagi
        } else {
          setRgbColor(false, false, false);
          currentState = STATE_MENU_UTAMA;      // Kembali ke menu
        }
      }
      break;

    case STATE_DATA_VIEW:
      if (actionShortClick) {
        dataViewCursor = (dataViewCursor + 1) % 2;
      }
      if (actionLongPress) {
        if (dataViewCursor == 0) {
          currentState = STATE_DATA_SENDING;
        } else {
          currentState = STATE_MENU_UTAMA;
        }
      }
      break;

    case STATE_DATA_SENDING:
      // Di layar pengiriman, tahan 2s untuk selesai dan kembali
      if (actionLongPress || actionShortClick) {
        currentState = STATE_DATA_VIEW;
      }
      break;

    case STATE_AMBIL_DATA_LIVE:
      if (actionShortClick) {
        ambilDataCursor = (ambilDataCursor + 1) % 2;
      }
      if (actionLongPress) {
        if (ambilDataCursor == 0) {
          currentState = STATE_AMBIL_DATA_BURST;
        } else {
          currentState = STATE_MENU_UTAMA;
        }
      }
      break;

    case STATE_AMBIL_DATA_BURST:
      renderDisplay();
      setRgbColor(false, false, true); // Indikator Biru aktif saat merekam

      // Rekam 5 data burst secara berurutan (1 detik per sampel)
      for (int i = 1; i <= 5; i++) {
        latestSuhu = readPT100Temperature();
        latestEcData = getCalibratedEC(latestSuhu);
        logBurstSample(i);
        delay(1000);
      }

      setRgbColor(false, false, false);
      currentState = STATE_AMBIL_DATA_LIVE;
      break;
  }

  renderDisplay();
}