#include <Arduino.h>
#include <algorithm>
#include <Wire.h>
#include <LittleFS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MAX31865.h>

// =================================================================
// 1. PIN & KONFIGURASI LAYAR OLED SSD1306 (I2C)
// =================================================================
#define OLED_SDA        8
#define OLED_SCL        9
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define SCREEN_ADDRESS  0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// =================================================================
// 2. PIN TOMBOL & PENYIMPANAN FLASH INTERNAL (LITTLEFS)
// =================================================================
#define BUTTON_PIN      7

bool isStorageReady = false;
int logCount = 0;
String logStatusMsg = "Flash Siap";

// Variabel Debounce Tombol
int lastButtonReading = LOW;
int confirmedButtonState = LOW;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

// =================================================================
// 3. PIN & KONFIGURASI SENSOR EC
// =================================================================
#define PIN_DRIVE_A     4
#define PIN_DRIVE_B     5
#define PIN_ADC_SENSE   1

const float R_REF       = 1000.0;     // Resistor referensi 1k Ohm
const float V_IN        = 3.3;        // Tegangan logika ESP32-S3

// Parameter Hasil Kalibrasi Dua Titik
const float CAL_SLOPE   = 2.157738;
const float CAL_OFFSET  = -1.871534;

// Koefisien Suhu Standar
const float ALPHA_TEMP  = 0.020;      // 2.0% per derajat C

// Konfigurasi Sampling Rate & Filter EC
const unsigned long SAMPLING_INTERVAL_MS = 1000; // 1 Hz
const int TOTAL_SAMPLES = 40;
const int TRIM_COUNT    = 8;

unsigned long lastSampleTime = 0;

struct ECReading {
  bool isSubmerged;
  float resistance;   // Ohm
  float conductance;  // mS
  float ecRaw;        // mS/cm aktual
  float ec25;         // mS/cm terkompensasi 25°C
};

// Variabel Penampung Data Terkini
float latestSuhu = 25.0;
ECReading latestEcData = {false, -1.0, 0.0, 0.0, 0.0};

// =================================================================
// 4. PIN & KONFIGURASI MAX31865 (PT100)
// =================================================================
// Software SPI: CS, DI (MOSI), DO (MISO), CLK
Adafruit_MAX31865 thermo = Adafruit_MAX31865(10, 11, 12, 13);
#define SENSOR_POWER_PIN 14

#define RREF      426.0
#define RNOMINAL  100.0

// =================================================================
// 5. FUNGSI PEMBACAAN EC (TRIMMED MEAN FILTER)
// =================================================================
ECReading getCalibratedEC(float currentTemperature) {
  ECReading data;
  float samples[TOTAL_SAMPLES];

  for (int i = 0; i < TOTAL_SAMPLES; i++) {
    // Fase 1: Pulsa Positif
    digitalWrite(PIN_DRIVE_A, HIGH);
    digitalWrite(PIN_DRIVE_B, LOW);
    delayMicroseconds(120);

    int rawADC = analogRead(PIN_ADC_SENSE);
    samples[i] = (rawADC / 4095.0) * V_IN;

    // Fase 2: Pulsa Negatif
    digitalWrite(PIN_DRIVE_A, LOW);
    digitalWrite(PIN_DRIVE_B, HIGH);
    delayMicroseconds(120);

    // Fase 3: Pelepasan Arus
    digitalWrite(PIN_DRIVE_A, LOW);
    digitalWrite(PIN_DRIVE_B, LOW);
    delay(2);
  }

  // Filter statistik (buang data outlier atas & bawah)
  std::sort(samples, samples + TOTAL_SAMPLES);

  float sumValidVoltage = 0.0;
  int validCount = TOTAL_SAMPLES - (2 * TRIM_COUNT);

  for (int i = TRIM_COUNT; i < (TOTAL_SAMPLES - TRIM_COUNT); i++) {
    sumValidVoltage += samples[i];
  }
  float avgVout = sumValidVoltage / validCount;

  // Cek jika probe berada di udara/kering
  if (avgVout >= (V_IN - 0.05) || avgVout <= 0.02) {
    data.isSubmerged   = false;
    data.resistance    = -1.0;
    data.conductance   = 0.0;
    data.ecRaw         = 0.0;
    data.ec25          = 0.0;
    return data;
  }

  data.isSubmerged = true;
  data.resistance  = R_REF * (avgVout / (V_IN - avgVout));
  data.conductance = 1000.0 / data.resistance;

  // Hitung EC Aktual dan Normalisasi Suhu ke 25°C
  data.ecRaw = (CAL_SLOPE * data.conductance) + CAL_OFFSET;
  if (data.ecRaw < 0.0) data.ecRaw = 0.0;

  data.ec25 = data.ecRaw / (1.0 + ALPHA_TEMP * (currentTemperature - 25.0));

  return data;
}

// =================================================================
// 6. FUNGSI PEMBACAAN SUHU MAX31865
// =================================================================
float readPT100Temperature() {
  uint8_t fault = thermo.readFault();
  if (fault) {
    Serial.printf("[FAULT PT100] Kode: 0x%02X\n", fault);
    thermo.clearFault();
    return 25.0; // Fallback ke suhu acuan jika ada fault
  }

  return thermo.temperature(RNOMINAL, RREF);
}

// =================================================================
// 7. FUNGSI RENDER DISPLAY OLED
// =================================================================
void updateOled(float suhu, const ECReading &ec) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Header Title
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("EC & SUHU MONITOR");
  display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

  // Nilai Suhu PT100
  display.setCursor(0, 13);
  display.printf("Suhu   : %.2f C", suhu);

  if (!ec.isSubmerged) {
    display.setCursor(0, 25);
    display.print("EC@25C : -- mS/cm");

    display.setCursor(0, 37);
    display.print("Status : PROBE KERING");
  } else {
    display.setCursor(0, 25);
    display.printf("EC@25C : %.3f mS", ec.ec25);

    display.setCursor(0, 37);
    display.printf("EC Akt : %.3f mS/cm", ec.ecRaw);
  }

  // Baris Status Flash & Log
  display.drawLine(0, 49, 128, 49, SSD1306_WHITE);
  display.setCursor(0, 53);
  display.printf("Log: %s", logStatusMsg.c_str());

  display.display();
}

// =================================================================
// 8. FUNGSI MANAJEMEN PENYIMPANAN FLASH INTERNAL (LITTLEFS)
// =================================================================
void logDataToFlash() {
  if (!isStorageReady) {
    logStatusMsg = "FS Error";
    Serial.println("[LOG ERROR] Flash internal belum siap / gagal mount.");
    return;
  }

  File dataFile = LittleFS.open("/data_log.json", FILE_APPEND);
  if (!dataFile) {
    logStatusMsg = "Gagal Tulis";
    Serial.println("[LOG ERROR] Gagal membuka /data_log.json untuk append!");
    return;
  }

  logCount++;
  unsigned long timeStamp = millis();

  // Format 1 baris JSON utuh (NDJSON)
  char jsonBuffer[256];
  snprintf(jsonBuffer, sizeof(jsonBuffer),
           "{\"id\":%d,\"time_ms\":%lu,\"temp_c\":%.2f,\"ec25\":%.3f,\"ec_raw\":%.3f,\"r_ohm\":%.1f,\"g_ms\":%.3f,\"submerged\":%s}",
           logCount,
           timeStamp,
           latestSuhu,
           latestEcData.isSubmerged ? latestEcData.ec25 : 0.0,
           latestEcData.isSubmerged ? latestEcData.ecRaw : 0.0,
           latestEcData.resistance,
           latestEcData.conductance,
           latestEcData.isSubmerged ? "true" : "false");

  dataFile.println(jsonBuffer);
  dataFile.close();

  logStatusMsg = "Saved #" + String(logCount);
  Serial.printf("[FLASH TERSIMPAN] %s\n", jsonBuffer);
}

void dumpLogData() {
  Serial.println("\n--- MEMERIKSA DATA LOG FLASH ---");
  if (!isStorageReady) {
    Serial.println("[ERROR] LittleFS belum siap!");
    return;
  }

  if (!LittleFS.exists("/data_log.json")) {
    Serial.println("[INFO] Berkas /data_log.json BELUM ADA di Flash.");
    Serial.println("-> Tekan tombol Pin 7 atau ketik 'w' untuk merekam data pertama.");
    return;
  }

  File dataFile = LittleFS.open("/data_log.json", FILE_READ);
  if (!dataFile) {
    Serial.println("[ERROR] Gagal membuka /data_log.json untuk dibaca.");
    return;
  }

  size_t fSize = dataFile.size();
  Serial.printf("[INFO] Ukuran berkas: %u bytes\n", fSize);

  if (fSize == 0) {
    Serial.println("[INFO] Berkas ada tetapi masih kosong (0 byte).");
    dataFile.close();
    return;
  }

  Serial.println("================ DUMP DATA LOG FLASH ================");
  while (dataFile.available()) {
    Serial.write(dataFile.read());
  }
  dataFile.close();
  Serial.println("================ AKHIR DARI DATA LOG ================\n");
}

void clearLogData() {
  if (LittleFS.remove("/data_log.json")) {
    logCount = 0;
    logStatusMsg = "Log Direset";
    Serial.println("\n[OK] Berkas /data_log.json berhasil dihapus.");
  } else {
    Serial.println("\n[ERROR] Berkas log tidak ditemukan atau gagal dihapus.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500); // Beri jeda 1.5 detik agar port USB serial di Linux (/dev/ttyUSB0) stabil

  Serial.println("\n========================================");
  Serial.println("[BOOT 1/5] ESP32-S3 Hidup & Serial Terbaca");
  Serial.println("========================================");

  pinMode(BUTTON_PIN, INPUT_PULLDOWN);

  // ==========================================================
  // Inisialisasi Layar OLED (Dengan Proteksi Timeout)
  // ==========================================================
  Serial.println("[BOOT 2/5] Menghubungkan ke Layar OLED...");
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(250); // Cegah sistem hang jika OLED tidak merespons!

  bool oledStatus = false;
  for (int coba = 1; coba <= 3; coba++) {
    if (display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
      oledStatus = true;
      Serial.printf(" -> [OK] Layar OLED terdeteksi (Percobaan %d)\n", coba);
      break;
    }
    Serial.printf(" -> [PERINGATAN] OLED belum siap (Percobaan %d/3)...\n", coba);
    delay(100);
  }

  if (!oledStatus) {
    Serial.println(" -> [LEWAT] OLED tidak terdeteksi, melanjutkan program...");
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(10, 25);
    display.println("Sistem Siap...");
    display.display();
  }

  // ==========================================================
  // Inisialisasi MAX31865
  // ==========================================================
  Serial.println("[BOOT 3/5] Menyalakan Sensor MAX31865 (PT100)...");
  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, HIGH);
  delay(1);
  thermo.begin(MAX31865_3WIRE);
  digitalWrite(SENSOR_POWER_PIN, LOW);
  delay(1);
  digitalWrite(SENSOR_POWER_PIN, HIGH);
  thermo.begin(MAX31865_3WIRE);
  Serial.println(" -> [OK] MAX31865 Siap");

  // ==========================================================
  // Inisialisasi Flash Internal (LittleFS)
  // ==========================================================
  Serial.println("[BOOT 4/5] Membuka Flash Internal (LittleFS)...");
  Serial.println(" -> Catatan: Jika pertama kali, proses format flash memakan waktu 30-60 detik.");
  
  if (!LittleFS.begin(true)) {
    Serial.println(" -> [GAGAL] LittleFS gagal di-mount!");
    isStorageReady = false;
    logStatusMsg = "Flash Gagal";
  } else {
    isStorageReady = true;
    logStatusMsg = "Flash Siap";

    if (LittleFS.exists("/data_log.json")) {
      File f = LittleFS.open("/data_log.json", FILE_READ);
      while (f.available()) {
        if (f.read() == '\n') logCount++;
      }
      f.close();
      logStatusMsg = "#" + String(logCount) + " Ada";
    }

    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes  = LittleFS.usedBytes();
    Serial.printf(" -> [OK] Flash Siap. Terpakai: %u KB / %u KB | Log: %d data\n",
                  usedBytes / 1024, totalBytes / 1024, logCount);
  }

  // ==========================================================
  // Inisialisasi Sensor EC
  // ==========================================================
  Serial.println("[BOOT 5/5] Mengaktifkan Pin ADC Sensor EC...");
  pinMode(PIN_DRIVE_A, OUTPUT);
  pinMode(PIN_DRIVE_B, OUTPUT);
  pinMode(PIN_ADC_SENSE, INPUT);
  digitalWrite(PIN_DRIVE_A, LOW);
  digitalWrite(PIN_DRIVE_B, LOW);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Serial.println("\n--- SELURUH SISTEM SIAP DIGUNAKAN ---");
  Serial.println("Tekan tombol Pin 7 atau kirim 'w' di Serial untuk simpan data.");
}

// =================================================================
// 10. LOOP UTAMA
// =================================================================
void loop() {
  unsigned long currentMillis = millis();

  // -------------------------------------------------------------
  // A. SAMPLING DATA SENSOR (1 Hz NON-BLOCKING)
  // -------------------------------------------------------------
  if (currentMillis - lastSampleTime >= SAMPLING_INTERVAL_MS) {
    lastSampleTime = currentMillis;

    latestSuhu = readPT100Temperature();
    latestEcData = getCalibratedEC(latestSuhu);

    if (!latestEcData.isSubmerged) {
      Serial.printf("Suhu: %5.2f °C | [PERINGATAN] Probe EC di udara / cairan tidak terdeteksi.\n", latestSuhu);
    } else {
      Serial.printf("Suhu: %5.2f °C | R: %6.1f Ω | G: %6.3f mS | EC Akt: %5.3f mS/cm | EC@25C: %5.3f mS/cm\n",
                    latestSuhu,
                    latestEcData.resistance,
                    latestEcData.conductance,
                    latestEcData.ecRaw,
                    latestEcData.ec25);
    }

    updateOled(latestSuhu, latestEcData);
  }

  // -------------------------------------------------------------
  // B. DETEKSI PUSH BUTTON DENGAN DEBOUNCE (PIN 7)
  // -------------------------------------------------------------
  int currentReading = digitalRead(BUTTON_PIN);

  if (currentReading != lastButtonReading) {
    lastDebounceTime = currentMillis;
  }

  if ((currentMillis - lastDebounceTime) > debounceDelay) {
    if (currentReading != confirmedButtonState) {
      confirmedButtonState = currentReading;

      // Terpicu saat tombol ditekan ke HIGH (rangkaian pull-down)
      if (confirmedButtonState == HIGH) {
        logDataToFlash();
        updateOled(latestSuhu, latestEcData);
      }
    }
  }

  lastButtonReading = currentReading;

  // -------------------------------------------------------------
  // C. KONTROL INTERAKTIF DARI SERIAL MONITOR
  // -------------------------------------------------------------
  while (Serial.available()) {
    char cmd = Serial.read();

    // Lewatkan spasi, enter, atau newline
    if (cmd == '\r' || cmd == '\n' || cmd == ' ') continue;

    Serial.printf("\n[SERIAL COMMAND] Diterima perintah: '%c'\n", cmd);

    if (cmd == 'w' || cmd == 'W') {
      Serial.println("-> Merekam data ke Flash...");
      logDataToFlash();
      updateOled(latestSuhu, latestEcData);
    } 
    else if (cmd == 'r' || cmd == 'R') {
      dumpLogData();
    } 
    else if (cmd == 'c' || cmd == 'C') {
      clearLogData();
      updateOled(latestSuhu, latestEcData);
    } 
    else {
      Serial.println("[PANDUAN KONTROL]");
      Serial.println("  'w' : Rekam 1 data baru ke Flash");
      Serial.println("  'r' : Tampilkan seluruh isi file log");
      Serial.println("  'c' : Hapus file log");
    }
  }
}