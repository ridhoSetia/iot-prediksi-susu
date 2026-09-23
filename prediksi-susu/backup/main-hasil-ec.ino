#include <Arduino.h>

// Definisi Pin Eksitasi dan ADC ESP32-S3

#define PIN_DRIVE_A 4

#define PIN_DRIVE_B 5

#define PIN_ADC_SENSE 1

// Parameter Rangkaian Pembagi Tegangan

const float R_REF = 1000.0; // Resistor referensi 1k Ohm (presisi 1%)

const float V_IN = 3.3; // Tegangan operasional GPIO

// Parameter Hasil Kalibrasi Dua Titik (Masukkan nilai riil yang diperoleh)

const float CAL_SLOPE  = 2.204908;
const float CAL_OFFSET = -2.276239;

// Koefisien Normalisasi Suhu Susu Sapi (2.0% per °C)

const float ALPHA_TEMP = 0.020;

struct ECReading
{

    bool isSubmerged;

    float resistance; // Hambatan cairan (Ohm)

    float conductance; // Konduktansi (mS)

    float ecRaw; // EC aktual pada suhu saat ini (mS/cm)

    float ec25; // EC ternormalisasi suhu standar 25°C (mS/cm)
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

    // Validasi batas ambang jika probe berada di udara atau terjadi kontak korsleting

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

    // 1. Perhitungan resistansi cairan (Hukum Ohm pada Voltage Divider)

    data.resistance = R_REF * (avgVout / (V_IN - avgVout));

    // 2. Perhitungan konduktansi G dalam satuan miliSiemens (mS)

    data.conductance = 1000.0 / data.resistance;

    // 3. Transformasi linear menggunakan koefisien kalibrasi

    data.ecRaw = (CAL_SLOPE * data.conductance) + CAL_OFFSET;

    if (data.ecRaw < 0.0)
        data.ecRaw = 0.0;

    // 4. Normalisasi termal non-linear ke suhu standar 25°C

    data.ec25 = data.ecRaw / (1.0 + ALPHA_TEMP * (currentTemperature - 25.0));

    return data;
}

void setup()
{

    Serial.begin(115200);

    pinMode(PIN_DRIVE_A, OUTPUT);

    pinMode(PIN_DRIVE_B, OUTPUT);

    pinMode(PIN_ADC_SENSE, INPUT);

    // Set kondisi awal pin eksitasi ke posisi netral

    digitalWrite(PIN_DRIVE_A, LOW);

    digitalWrite(PIN_DRIVE_B, LOW);

    analogReadResolution(12);

    analogSetAttenuation(ADC_11db);

    Serial.println("\n[SISTEM] Monitoring Sensor EC Aktif");

    Serial.printf("[KALIBRASI] Slope: %.6f | Offset: %.6f\n", CAL_SLOPE, CAL_OFFSET);

    Serial.println("------------------------------------------------------------");
}

void loop()
{

    // Nilai suhu referensi (gunakan nilai riil pembacaan saat integrasi dengan PT100)

    float suhuUji = 25.0;

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

    delay(1500);
}
