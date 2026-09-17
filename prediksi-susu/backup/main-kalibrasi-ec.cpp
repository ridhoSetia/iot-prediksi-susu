#include <Arduino.h>

// Definisi Pin ESP32-S3
#define PIN_DRIVE_A 4
#define PIN_DRIVE_B 5
#define PIN_ADC_SENSE 1

// Parameter Sirkuit & Acuan Standar
const float R_REF = 1000.0;     // Resistor divider 1k Ohm
const float V_IN = 3.3;          // Tegangan logika ESP32-S3
const float EC_STD_LOW = 1.413;  // Titik 1: Larutan 1.413 mS/cm
const float EC_STD_HIGH = 12.88; // Titik 2: Larutan 12.880 mS/cm

// Variabel Penampung Nilai Konduktansi (mS)
float gLow = 0.0;
float gHigh = 0.0;
bool lowDone = false;
bool highDone = false;

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

void setup() {
  Serial.begin(115200);

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

  // Monitor nilai pembacaan waktu nyata
  if (currentG < 0) {
    Serial.println("[Status] Probe Terbuka / Udara Kering");
  } else {
    float rEq = 1000.0 / currentG;
    Serial.printf("Terkoneksi -> R: %.1f Ohm | Konduktansi: %.4f mS\n", rEq, currentG);
  }

  // Menangani input Serial
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

    // Jika kedua titik telah tercatat, kalkulasikan koefisien regresi
    if (lowDone && highDone) {
      float slope = (EC_STD_HIGH - EC_STD_LOW) / (gHigh - gLow);
      float offset = EC_STD_LOW - (slope * gLow);

      Serial.println("\n==========================================");
      Serial.println("       HASIL KALIBRASI DUA TITIK          ");
      Serial.println("==========================================");
      Serial.printf("Konstanta Sel Efektif : %.4f\n", slope);
      Serial.printf("Offset ADC / Galat    : %.4f mS/cm\n", offset);
      Serial.println("------------------------------------------");
      Serial.println("Salin baris berikut ke firmware utama Anda:");
      Serial.printf("const float CAL_SLOPE  = %.6f;\n", slope);
      Serial.printf("const float CAL_OFFSET = %.6f;\n", offset);
      Serial.println("==========================================\n");
    }
  }

  delay(1200);
}