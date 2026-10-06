#include <Arduino.h>
// Sertakan file rumus bobot C++ FP32 murni
#include "milk_model_weights.h"

void setup() {
  // 1. Inisialisasi Serial Monitor
  Serial.begin(115200);
  delay(2000);  // Jeda agar Serial Monitor terbuka dengan sempurna

  Serial.println("\n=== BENCHMARK INFERENSI TINYML ESP32-S3 ===");

  // 2. Siapkan data sensor (dummy) yang sudah dinormalisasi (0.0 - 1.0)
  float norm_suhu = 0.45f;
  float norm_rohm = 0.20f;
  float norm_ecraw = 0.60f;
  float norm_ec25 = 0.55f;

  // Variabel penampung hasil luaran model
  String pred_grade;
  int pred_shelf_life;

  // 3. FASE PEMANASAN (WARM-UP CACHE)
  // Eksekusi pertama memakan waktu lebih lama karena prosesor harus menyalin
  // matriks bobot dari memori Flash (ROM) ke Cache SRAM berkecepatan tinggi.
  Serial.println("Melakukan pemanasan Cache memori...");
  predictMilkModel(norm_suhu, norm_rohm, norm_ecraw, norm_ec25, pred_grade, pred_shelf_life);

  // 4. FASE PENGUJIAN UTAMA (HARDWARE BENCHMARKING)
  int jumlah_tes = 1000;
  unsigned long total_mikrodetik = 0;

  // Variabel untuk melacak stabilitas latensi
  unsigned long latensi_tercepat = 999999;  // Diisi nilai awal sangat besar
  unsigned long latensi_terlama = 0;        // Diisi nilai awal 0

  Serial.print("Memulai pengujian sebanyak ");
  Serial.print(jumlah_tes);
  Serial.println(" iterasi secara beruntun...\n");

  for (int i = 0; i < jumlah_tes; i++) {
    // Catat waktu mula
    unsigned long t_start = micros();

    // Eksekusi Inferensi Kecerdasan Buatan
    predictMilkModel(norm_suhu, norm_rohm, norm_ecraw, norm_ec25, pred_grade, pred_shelf_life);

    // Catat waktu selesai
    unsigned long t_end = micros();

    // Hitung selisih durasi satu putaran
    unsigned long durasi = t_end - t_start;

    // Akumulasi total waktu
    total_mikrodetik += durasi;

    // Lacak rekor tercepat dan terlama
    if (durasi < latensi_tercepat) latensi_tercepat = durasi;
    if (durasi > latensi_terlama) latensi_terlama = durasi;
  }

  // 5. KALKULASI METRIK EVALUASI AKHIR
  float rata_rata_mikrodetik = (float)total_mikrodetik / jumlah_tes;
  float rata_rata_milidetik = rata_rata_mikrodetik / 1000.0f;

  // 6. TAMPILKAN LAPORAN KE SERIAL MONITOR
  Serial.println("Status: Benchmark Selesai!");
  Serial.print("Output Tebakan Grade  : ");
  Serial.println(pred_grade);
  Serial.print("Output Tebakan Waktu  : ");
  Serial.print(pred_shelf_life);
  Serial.println(" Menit\n");

  Serial.println("=== HASIL LATENSI HARDWARE ESP32-S3 ===");
  Serial.print("Total Waktu (1000x)   : ");
  Serial.print(total_mikrodetik);
  Serial.println(" mikrodetik");
  Serial.println("-------------------------------------------");
  Serial.print("Latensi Tercepat      : ");
  Serial.print(latensi_tercepat);
  Serial.println(" mikrodetik");
  Serial.print("Latensi Terlama       : ");
  Serial.print(latensi_terlama);
  Serial.println(" mikrodetik");
  Serial.println("-------------------------------------------");
  Serial.print("Rata-rata Eksekusi    : ");
  Serial.print(rata_rata_mikrodetik, 2);
  Serial.print(" mikrodetik (");
  Serial.print(rata_rata_milidetik, 4);
  Serial.println(" ms)");
  Serial.println("===========================================================\n");
}

void loop() {
  // Dibiarkan kosong karena pengujian akademis hanya dilakukan sekali saat boot
}