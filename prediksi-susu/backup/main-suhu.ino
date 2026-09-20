#include <Adafruit_MAX31865.h>

// Gunakan Software SPI: buat pin tiruan jika tidak memakai pin hardware SPI bawaan Arduino
// Pinout: CS, DI (MOSI), DO (MISO), CLK
Adafruit_MAX31865 thermo = Adafruit_MAX31865(10, 11, 12, 13);
#define SENSOR_POWER_PIN 14

// Jika menggunakan Hardware SPI, gunakan baris di bawah ini (pin CS di pin 10):
// Adafruit_MAX31865 thermo = Adafruit_MAX31865(10);

// PENTING: Sesuaikan dengan jenis Rref (resistor referensi) di modul Anda
// Modul PT100 biasanya memakai RREF = 430.0 Ohm
// Modul PT1000 biasanya memakai RREF = 4300.0 Ohm
#define RREF      426.0

// Sesuaikan dengan resistansi nominal sensor pada suhu 0 derajat Celcius
// PT100 = 100.0 Ohm, PT1000 = 1000.0 Ohm
#define RNOMINAL  100.0

void setup() {
  Serial.begin(115200);

  delay(1);

  // ====================================
  // Nyalakan MAX31865 dari GPIO 14
  // ====================================
  pinMode(SENSOR_POWER_PIN, OUTPUT);

  digitalWrite(SENSOR_POWER_PIN, HIGH);

  Serial.println("Power MAX31865 ON");

  // Tunggu tegangan stabil
  delay(1);

  // ====================================
  // Inisialisasi MAX31865
  // ====================================
  thermo.begin(MAX31865_3WIRE);

  digitalWrite(SENSOR_POWER_PIN, LOW);
  delay(1);
  digitalWrite(SENSOR_POWER_PIN, HIGH);

  // 2. Inisialisasi konfigurasi 3-kabel SETELAH daya aktif stabil
  thermo.begin(MAX31865_3WIRE);

  Serial.println("========================================");
  Serial.println(" MAX31865 + PT100 3-WIRE (TERKALIBRASI)");
  Serial.println(" FAST LIQUID TEMPERATURE MONITOR");
  Serial.println("========================================\n");
}

void loop() {
  uint16_t rtd = thermo.readRTD();

  Serial.print("Nilai RTD: "); Serial.println(rtd);
  
  // Menghitung resistansi sensor saat ini
  float ratio = rtd;
  ratio /= 32768;
  Serial.print("Resistansi = "); Serial.print(RREF*ratio); Serial.println(" Ohm");
  
  // Menghitung dan menampilkan suhu dalam Celcius
  Serial.print("Suhu = "); Serial.print(thermo.temperature(RNOMINAL, RREF)); Serial.println(" C");

  // Cek apakah ada error pada pembacaan sensor
  uint8_t fault = thermo.readFault();
  if (fault) {
    Serial.print("Fault 0x"); Serial.println(fault, HEX);
    if (fault & MAX31865_FAULT_HIGHTHRESH) {
      Serial.println("RTD High Threshold"); 
    }
    if (fault & MAX31865_FAULT_LOWTHRESH) {
      Serial.println("RTD Low Threshold"); 
    }
    if (fault & MAX31865_FAULT_REFINLOW) {
      Serial.println("REFIN- > 0.85 x Bias"); 
    }
    if (fault & MAX31865_FAULT_REFINHIGH) {
      Serial.println("REFIN- < 0.85 x Bias - FORCE- open"); 
    }
    if (fault & MAX31865_FAULT_RTDINLOW) {
      Serial.println("RTDIN- < 0.85 x Bias - FORCE- open"); 
    }
    if (fault & MAX31865_FAULT_OVUV) {
      Serial.println("Under/Over voltage"); 
    }
    thermo.clearFault();
  }
  Serial.println();
  delay(100);
}
