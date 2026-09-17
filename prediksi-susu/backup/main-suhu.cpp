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
}