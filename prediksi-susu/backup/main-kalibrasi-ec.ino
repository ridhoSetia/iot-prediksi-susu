#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ==========================================
// DEFINISI PIN & LAYAR OLED (ESP32-S3)
// ==========================================
#define OLED_SDA        8
#define OLED_SCL        9
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define SCREEN_ADDRESS  0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Definisi Pin Sensor EC
#define PIN_DRIVE_A     4
#define PIN_DRIVE_B     5
#define PIN_ADC_SENSE   1

// Parameter Sirkuit & Acuan Standar
const float R_REF = 1000.0;     // Resistor divider 1k Ohm
const float V_IN = 3.3;          // Tegangan logika ESP32-S3
const float EC_STD_LOW = 1.413;  // Titik 1: Larutan 1.413 mS/cm
const float EC_STD_HIGH = 12.88; // Titik 2: Larutan 12.880 mS/cm

// Variabel Penampung Nilai Konduktansi & Kalibrasi
float gLow = 0.0;
float gHigh = 0.0;
bool lowDone = false;
bool highDone = false;
float calSlope = 0.0;
float calOffset = 0.0;

float readConductanceMilliSiemens() {
  const int SAMPLES = 40;
  float totalVoltage = 0.0;

  for (int i = 0; i < SAMPLES; i++) {
    // Fase eksitasi AC bolak-balik
    digitalWrite(PIN_DRIVE_A, HIGH);
    digitalWrite(PIN_DRIVE_B, LOW);
    delayMicroseconds(120);

    int rawADC = analogRead(PIN_ADC_SENSE);
    float vOut = (rawADC / 4095.0) * V_IN;

    digitalWrite(PIN_DRIVE_A, LOW);
    digitalWrite(PIN_DRIVE_B, HIGH);
    delayMicroseconds(120);

    digitalWrite(PIN_DRIVE_A, LOW);
    digitalWrite(PIN_DRIVE_B, LOW);
    delay(2);

    totalVoltage += vOut;
  }

  float avgVout = totalVoltage / SAMPLES;

  // Proteksi di luar batas cairan
  if (avgVout >= (V_IN - 0.05) || avgVout <= 0.02) {
    return -1.0;
  }

  // R_cairan = R_REF * (V_out / (V_in - V_out))
  float rLiquid = R_REF * (avgVout / (V_IN - avgVout));
  
  // Konduktansi G (miliSiemens) = (1 / R) * 1000
  return (1000.0 / rLiquid);
}

void updateOled(float currentG) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (!lowDone || !highDone) {
    // Tampilan Saat Proses Kalibrasi Berjalan
    display.setCursor(0, 0);
    display.println("KALIBRASI EC 2-TITIK");
    display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

    display.setCursor(0, 14);
    if (currentG < 0) {
      display.println("Probe: TERBUKA/KERING");
    } else {
      display.printf("G: %.3f mS (%.0f R)\n", currentG, (1000.0 / currentG));
    }

    display.setCursor(0, 28);
    display.printf("P1 (1.413) : %s\n", lowDone ? "OK (Disimpan)" : "Ketik '1'");

    display.setCursor(0, 40);
    display.printf("P2 (12.88) : %s\n", highDone ? "OK (Disimpan)" : "Ketik '2'");

    display.setCursor(0, 54);
    display.println("Input via Serial Mon");
  } else {
    // Tampilan Setelah Kedua Titik Berhasil Disimpan
    display.setCursor(0, 0);
    display.println("KALIBRASI SELESAI");
    display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

    display.setCursor(0, 14);
    display.printf("Slope : %.4f\n", calSlope);
    display.setCursor(0, 25);
    display.printf("Offset: %.4f\n", calOffset);

    display.setCursor(0, 39);
    if (currentG > 0) {
      float liveEC = (calSlope * currentG) + calOffset;
      display.printf("EC: %.3f mS/cm\n", liveEC);
    } else {
      display.println("EC: -- (Di Udara)");
    }

    display.setCursor(0, 52);
    display.printf("Raw G : %.3f mS\n", currentG > 0 ? currentG : 0.0);
  }

  display.display();
}

void setup() {
  Serial.begin(115200);

  // Inisialisasi OLED SSD1306
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("[ERROR] Layar OLED SSD1306 tidak terdeteksi!");
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println("Siap Kalibrasi EC...");
  display.display();
  delay(1000);

  pinMode(PIN_DRIVE_A, OUTPUT);
  pinMode(PIN_DRIVE_B, OUTPUT);
  pinMode(PIN_ADC_SENSE, INPUT);

  digitalWrite(PIN_DRIVE_A, LOW);
  digitalWrite(PIN_DRIVE_B, LOW);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Serial.println("\n=== KALIBRASI 2 TITIK SENSOR EC (ESP32-S3) ===");
  Serial.println("Perintah:");
  Serial.println("Ketik '1' : Catat data Buffer 1.413 mS/cm");
  Serial.println("Ketik '2' : Catat data Buffer 12.880 mS/cm");
  Serial.println("----------------------------------------------");
}

void loop() {
  float currentG = readConductanceMilliSiemens();

  // Monitor nilai pembacaan waktu nyata di Serial
  if (currentG < 0) {
    Serial.println("[Status] Probe Terbuka / Udara Kering");
  } else {
    float rEq = 1000.0 / currentG;
    Serial.printf("Terkoneksi -> R: %.1f Ohm | Konduktansi: %.4f mS\n", rEq, currentG);
  }

  // Menangani input Serial untuk titik kalibrasi
  if (Serial.available() > 0) {
    char cmd = Serial.read();

    if (cmd == '1') {
      if (currentG > 0) {
        gLow = currentG;
        lowDone = true;
        Serial.printf("\n>>> Titik 1 TERSIMPAN: G_low = %.4f mS (Ref: %.3f mS/cm) <<<\n\n", gLow, EC_STD_LOW);
      } else {
        Serial.println("\n[ERROR] Probe belum terendam di larutan 1.413 mS/cm!\n");
      }
    } 
    else if (cmd == '2') {
      if (currentG > 0) {
        gHigh = currentG;
        highDone = true;
        Serial.printf("\n>>> Titik 2 TERSIMPAN: G_high = %.4f mS (Ref: %.3f mS/cm) <<<\n\n", gHigh, EC_STD_HIGH);
      } else {
        Serial.println("\n[ERROR] Probe belum terendam di larutan 12.88 mS/cm!\n");
      }
    }

    // Jika kedua titik telah tercatat, hitung koefisien regresi
    if (lowDone && highDone) {
      calSlope = (EC_STD_HIGH - EC_STD_LOW) / (gHigh - gLow);
      calOffset = EC_STD_LOW - (calSlope * gLow);

      Serial.println("\n==========================================");
      Serial.println("       HASIL KALIBRASI DUA TITIK          ");
      Serial.println("==========================================");
      Serial.printf("Konstanta Sel Efektif : %.4f\n", calSlope);
      Serial.printf("Offset ADC / Galat    : %.4f mS/cm\n", calOffset);
      Serial.println("------------------------------------------");
      Serial.println("Salin baris berikut ke firmware utama:");
      Serial.printf("const float CAL_SLOPE  = %.6f;\n", calSlope);
      Serial.printf("const float CAL_OFFSET = %.6f;\n", calOffset);
      Serial.println("==========================================\n");
    }
  }

  // Perbarui tampilan layar OLED
  updateOled(currentG);

  delay(1200);
}