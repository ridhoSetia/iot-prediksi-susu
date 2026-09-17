#include <Arduino.h>
#include <Adafruit_MAX31865.h>

// ======================================
// POWER MAX31865
// ======================================
#define SENSOR_POWER_PIN 14

// ======================================
// MAX31865 -> ESP32-S3
// CS, MOSI, MISO, CLK
// ======================================
#define MAX_CS 10
#define MAX_MOSI 11
#define MAX_MISO 12
#define MAX_CLK 13

Adafruit_MAX31865 thermo(
    MAX_CS,
    MAX_MOSI,
    MAX_MISO,
    MAX_CLK);

// ======================================
// PT100
// ======================================
#define RREF 426.0
#define RNOMINAL 100.0

// ======================================
// FAST RESPONSE
// ======================================

float TAU_SENSOR = 10.0;
float DERIVATIVE_ALPHA = 0.25;
float MAX_CORRECTION = 15.0;

// Interval pembacaan
const unsigned long READ_INTERVAL = 200;

unsigned long lastRead = 0;
unsigned long lastTime = 0;

float lastTemp = 0.0;
float filteredRate = 0.0;

bool firstRead = true;

// ======================================
// SENSOR EC
// ======================================
#define PIN_DRIVE_A 4
#define PIN_DRIVE_B 5
#define PIN_ADC_SENSE 1

const float R_REF = 1000.0; // Resistor referensi 1k Ohm (presisi 1%)
const float V_IN = 3.3;     // Tegangan operasional GPIO

const float CAL_SLOPE = 1.428571;
const float CAL_OFFSET = -0.052140;

const float ALPHA_TEMP = 0.020; // Koefisien normalisasi suhu (2.0% per °C)

struct ECReading
{
    bool isSubmerged;
    float resistance;  // Hambatan cairan (Ohm)
    float conductance; // Konduktansi (mS)
    float ecRaw;       // EC aktual pada suhu saat ini (mS/cm)
    float ec25;        // EC ternormalisasi suhu standar 25°C (mS/cm)
};

ECReading getCalibratedEC(float currentTemperature)
{
    ECReading data;
    const int SAMPLES = 30;
    float totalVoltage = 0.0;

    // Siklus eksitasi AC untuk mencegah polarisasi elektroda
    for (int i = 0; i < SAMPLES; i++)
    {
        // Fase 1: Pulsa Positif
        digitalWrite(PIN_DRIVE_A, HIGH);
        digitalWrite(PIN_DRIVE_B, LOW);
        delayMicroseconds(120);

        int rawADC = analogRead(PIN_ADC_SENSE);
        float vOut = (rawADC / 4095.0) * V_IN;

        // Fase 2: Pulsa Negatif (Pembalik Polaritas)
        digitalWrite(PIN_DRIVE_A, LOW);
        digitalWrite(PIN_DRIVE_B, HIGH);
        delayMicroseconds(120);

        // Fase 3: Pelepasan Arus / Grounding
        digitalWrite(PIN_DRIVE_A, LOW);
        digitalWrite(PIN_DRIVE_B, LOW);
        delay(2);

        totalVoltage += vOut;
    }

    float avgVout = totalVoltage / SAMPLES;

    // Validasi batas ambang jika probe berada di udara atau korsleting
    if (avgVout >= (V_IN - 0.05) || avgVout <= 0.02)
    {
        data.isSubmerged = false;
        data.resistance = -1.0;
        data.conductance = 0.0;
        data.ecRaw = 0.0;
        data.ec25 = 0.0;
        return data;
    }

    data.isSubmerged = true;

    // 1. Perhitungan resistansi cairan
    data.resistance = R_REF * (avgVout / (V_IN - avgVout));

    // 2. Perhitungan konduktansi G (mS)
    data.conductance = 1000.0 / data.resistance;

    // 3. Transformasi linear koefisien kalibrasi
    data.ecRaw = (CAL_SLOPE * data.conductance) + CAL_OFFSET;
    if (data.ecRaw < 0.0)
        data.ecRaw = 0.0;

    // 4. Normalisasi termal ke suhu standar 25°C
    data.ec25 = data.ecRaw / (1.0 + ALPHA_TEMP * (currentTemperature - 25.0));

    return data;
}

// ======================================
// SETUP
// ======================================
void setup()
{

  Serial.begin(115200);

  delay(500);

  // ====================================
  // Nyalakan MAX31865 dari GPIO 14
  // ====================================
  pinMode(SENSOR_POWER_PIN, OUTPUT);

  digitalWrite(SENSOR_POWER_PIN, HIGH);

  Serial.println("Power MAX31865 ON");

  // Tunggu tegangan stabil
  delay(500);

  // ====================================
  // Inisialisasi MAX31865
  // ====================================
  thermo.begin(MAX31865_3WIRE);

  digitalWrite(SENSOR_POWER_PIN, LOW);
  delay(1000);
  digitalWrite(SENSOR_POWER_PIN, HIGH);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" MAX31865 + PT100 3-WIRE");
  Serial.println(" FAST LIQUID TEMPERATURE");
  Serial.println("========================================");
  Serial.println();

  // ====================================
  // Inisialisasi Sensor EC (Dari Kode 2)
  // ====================================
  pinMode(PIN_DRIVE_A, OUTPUT);
  pinMode(PIN_DRIVE_B, OUTPUT);
  pinMode(PIN_ADC_SENSE, INPUT);

  digitalWrite(PIN_DRIVE_A, LOW);
  digitalWrite(PIN_DRIVE_B, LOW);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Serial.println("[SISTEM] Monitoring Sensor EC Aktif");
  Serial.printf("[KALIBRASI] Slope: %.6f | Offset: %.6f\n", CAL_SLOPE, CAL_OFFSET);
  Serial.println("------------------------------------------------------------");
}

// ======================================
// LOOP
// ======================================
void loop()
{

  if (millis() - lastRead < READ_INTERVAL)
  {
    return;
  }

  lastRead = millis();

  // ======================================
  // Baca RTD
  // ======================================
  uint16_t rtd = thermo.readRTD();

  float ratio =
      (float)rtd / 32768.0;

  float resistance =
      ratio * RREF;

  float temperature =
      thermo.temperature(
          RNOMINAL,
          RREF);

  // ======================================
  // Cek fault
  // ======================================
  uint8_t fault =
      thermo.readFault();

  if (fault)
  {

    Serial.print("FAULT : 0x");
    Serial.println(fault, HEX);

    if (fault & MAX31865_FAULT_HIGHTHRESH)
    {
      Serial.println("RTD High Threshold");
    }

    if (fault & MAX31865_FAULT_LOWTHRESH)
    {
      Serial.println("RTD Low Threshold");
    }

    if (fault & MAX31865_FAULT_REFINLOW)
    {
      Serial.println("REFIN Low");
    }

    if (fault & MAX31865_FAULT_REFINHIGH)
    {
      Serial.println("FORCE Open");
    }

    if (fault & MAX31865_FAULT_RTDINLOW)
    {
      Serial.println("RTDIN Low");
    }

    if (fault & MAX31865_FAULT_OVUV)
    {
      Serial.println("Under / Over Voltage");
    }

    thermo.clearFault();

    delay(50);

    return;
  }

  // ======================================
  // FAST RESPONSE
  // ======================================
  float fastTemperature =
      temperature;

  if (!firstRead)
  {

    float dt =
        (millis() - lastTime) / 1000.0;

    if (dt > 0)
    {

      float tempRate =
          (temperature - lastTemp) / dt;

      filteredRate =
          DERIVATIVE_ALPHA * tempRate + (1.0 - DERIVATIVE_ALPHA) * filteredRate;

      float correction =
          TAU_SENSOR * filteredRate;

      // Batasi koreksi
      if (correction > MAX_CORRECTION)
      {

        correction =
            MAX_CORRECTION;
      }

      if (correction < -MAX_CORRECTION)
      {

        correction =
            -MAX_CORRECTION;
      }

      fastTemperature =
          temperature + correction;
    }
  }
  else
  {

    firstRead = false;
  }

  lastTemp =
      temperature;

  lastTime =
      millis();

  // ======================================
  // OUTPUT
  // ======================================
  Serial.println(
      "----------------------------------------");

  Serial.print(
      "RTD Raw       : ");

  Serial.println(rtd);

  Serial.print(
      "Ratio         : ");

  Serial.println(
      ratio,
      6);

  Serial.print(
      "Resistance    : ");

  Serial.print(
      resistance,
      3);

  Serial.println(
      " ohm");

  Serial.print(
      "Temp Sensor   : ");

  Serial.print(
      temperature,
      2);

  Serial.println(
      " C");

  Serial.print(
      "Change Rate   : ");

  Serial.print(
      filteredRate,
      3);

  Serial.println(
      " C/s");

  Serial.print(
      "Temp Fast     : ");

  Serial.print(
      fastTemperature,
      2);

  Serial.println(
      " C");

  Serial.println();

  // ======================================
  // OUTPUT SENSOR EC
  // ======================================
  float suhuUji = fastTemperature;

  ECReading ecData = getCalibratedEC(suhuUji);

  if (!ecData.isSubmerged)
  {
    Serial.println("[PERINGATAN] Probe tidak mendeteksi cairan.");
  }
  else
  {
    Serial.printf("R: %6.1f Ω | G: %6.3f mS | EC Aktual: %5.3f mS/cm | Suhu: %4.1f °C | EC25: %5.3f mS/cm\n",
                  ecData.resistance,
                  ecData.conductance,
                  ecData.ecRaw,
                  suhuUji,
                  ecData.ec25);
  }

  Serial.println();
  delay(50);
}