# MASTER TECHNICAL SPECIFICATION & PROJECT IMPLEMENTATION BLUEPRINT

**Sistem Cerdas Penjaminan Mutu Susu Segar Berbasis On-Device TinyML untuk Mitigasi Kerusakan Pascapanen Peternak**

*Internal Engineering Document — Technical Architecture, Firmware, Data Pipeline, & Edge AI Deployment Guidelines*

---

## BAB I: RINGKASAN EKSEKUTIF & KONSEP SISTEM

### 1. Validasi Masalah Lapangan

Susu sapi segar merupakan komoditas pangan hewani berkategori sangat rentan rusak (*perishable biological asset*). Pada rantai pasok konvensional peternakan rakyat Indonesia, titik kritis penurunan mutu (*quality degradation bottleneck*) terjadi tepat pada fase pascapanen (*post-harvest cold-chain*) selama masa tunggu di area kandang sebelum penjemputan armada Koperasi Unit Desa (KUD).

Ketiadaan fasilitas pendingin aktif (*chilling unit*) pada tingkat peternak skala mikro dan kecil menyebabkan susu tersimpan pada rentang suhu ruang tropis ($25^\circ\text{C} - 32^\circ\text{C}$). Suhu ini merupakan lingkungan termal optimal bagi koloni bakteri alami susu, khususnya *Lactic Acid Bacteria* (LAB), untuk memasuki fase eksponensial (*log phase*). Bakteri ini memetabolisme disakarida laktosa menjadi asam laktat secara cepat:

$$\text{C}_{12}\text{H}_{22}\text{O}_{11} + \text{H}_2\text{O} \xrightarrow{\text{Lactic Acid Bacteria}} 4\text{ CH}_3\text{CH(OH)COOH}$$

Fermentasi biokimia ini melepaskan ion hidrogen ($H^+$) dan ion laktat ($CH_3CH(OH)COO^-$) ke dalam matriks cairan, yang secara simultan menurunkan derajat keasaman ($pH$) dan meruntuhkan stabilitas misel protein kasein.

Akibatnya, saat susu tiba di pos penampungan KUD setelah melewati masa angkut 2–4 jam, keasaman telah melampaui ambang batas penolakan ($pH < 6.3$ atau uji alkohol positif). Kondisi ini menyebabkan ribuan liter susu ditolak secara sepihak oleh KUD maupun Industri Pengolahan Susu (IPS), memicu fenomena pembuangan susu (*food waste*) dan kerugian finansial langsung bagi peternak.

Metode pengujian konvensional di pos penampungan (seperti uji alkohol tetes manual atau uji *lactodensimeter*) bersifat **reaktif** dan **lagging**—hanya mendeteksi susu yang telah terlanjur rusak tanpa memberikan indikasi prediktif tentang sisa durasi masa simpan susu yang masih segar.

### 2. Solusi & Value Proposition

Sistem ini dirancang sebagai instrumen cerdas genggam (*Smart Handheld Milk Quality Meter*) dengan prinsip komputasi mandiri pada sisi perangkat (*Pure On-Device Edge AI*).

```
+---------------------------------------------------------------------------------------------------+
|                                  VALUE PROPOSITION ARSITEKTUR                                     |
+------------------------------+--------------------------------------------------------------------+
| On-Device Processing         | Inferensi TinyML INT8 di ESP32-S3 < 10 ms; zero-dependency         |
|                              | terhadap cloud/internet saat pengambilan keputusan di kandang[cite: 1, 2].|
+------------------------------+--------------------------------------------------------------------+
| Predictive Countdown         | Estimasi sisa jendela waktu simpan (Estimated Shelf-Life Window)   |
|                              | dalam satuan menit secara dinamis terhadap dinamika termal[cite: 1, 2].     |
+------------------------------+--------------------------------------------------------------------+
| Non-Destructive & Reagentless| Pengujian biofisika murni (< 3 detik); mengeliminasi ketergantungan|
|                              | terhadap alkohol 70% dan bahan kimia laboratorium[cite: 1, 2].             |
+------------------------------+--------------------------------------------------------------------+
| Offline-First Architecture   | Pencatatan log terstempel waktu lokal di MicroSD; auto-sync        |
|                              | asinkron ke cloud KUD begitu mendeteksi jaringan Wi-Fi[cite: 1, 2].       |
+------------------------------+--------------------------------------------------------------------+

```

### 3. Arsitektur Makro Sistem

Diagram alir kerja (*Flowchart*) end-to-end memperlihatkan alur logika operasional dari sensor fisik, komputasi edge, penyimpanan lokal, hingga sinkronisasi awan:

```mermaid
flowchart TD
    A([Start: Power On & Inisialisasi ESP32-S3]) --> B[Mount MicroSD, Load Model TinyML INT8 ke SRAM]
    B --> C[/Celupkan Probe ke Wadah Susu / Milk Can/]
    C --> D[Jeda Stabilisasi Termal 3 Detik]
    D --> E[Baca Sensor Suhu RTD PT100 via MAX31865: Durasi 0.1s]
    E --> F[Eksitasi Pulsa AC & Baca ADC EC: Durasi 0.2s]
    F --> G{Apakah Probe Terendam Susu?}
    G -- Tidak --> H[/Display OLED: 'Probe di Luar Cairan'/] --> C
    G -- Ya --> I[Kompensasi Termal Non-Linear: Hitung EC_25 & Delta EC Rate]
    I --> J[Inferensi TinyML INT8 di SRAM ESP32-S3: Durasi <10ms]
    J --> K{Evaluasi Status Mutu}
    K -- Grade A --> L[LED Hijau Aktif]
    K -- Grade B --> M[LED Kuning Aktif + Beep Buzzer Singkat]
    K -- Grade C --> N[LED Merah Aktif + Alarm Buzzer Kontinu]
    L & M & N --> O[/Tampilkan Status Grade & Sisa Waktu di OLED 0.96"/]
    O --> P[(Simpan Payload JSON ke Antrean MicroSD via SPI)]
    P --> Q{Apakah Sinyal Wi-Fi Terdeteksi?}
    Q -- Tidak --> R([Standby: Siaga Pengujian Sesi Berikutnya])
    Q -- Ya --> S[Baca Antrean MicroSD & Kirim Batch Payload via HTTP POST/MQTT]
    S --> T{Apakah Server Merespon HTTP 200 OK?}
    T -- Ya --> U[Hapus / Tandai Data Terkirim di MicroSD]
    T -- Tidak --> R
    U --> V[/Update Web Dashboard KUD: Peta Mutu & Early Warning Armada/]
    V --> R

```

---

## BAB II: INSTRUMENTASI PERANGKAT KERAS (HARDWARE & SENSING)

### 1. Desain Rangkaian Custom AC Voltage Divider

Pengukuran konduktivitas listrik cairan elektrolit tidak dapat menggunakan sumber arus searah (DC). Arus DC memicu fenomena **elektrolisis** (disosiasi molekul air menjadi gas $H_2$ dan $O_2$) dan **polarisasi elektroda** (akumulasi ion lawan muatan pada permukaan logam yang membentuk lapisan dielektrik isolator). Hal ini mengakibatkan degradasi probe akibat oksidasi serta penurunan drastis pembacaan arus setelah beberapa detik.

Sistem menggunakan metode **Eksitasi Tegangan Bolak-Balik Frekuensi Tinggi (Custom AC Divider)** yang dikontrol secara digital melalui 2 pin GPIO ESP32-S3:

```
    [ESP32-S3 GPIO 4 (Drive A)] 
                 |
          [ Resistor Referensi ]
          [  R_ref: 1 kΩ (1%)  ]
                 |
                 +-------------------> [ESP32-S3 GPIO 1 (ADC1_CH0 Sense)]
                 |
          [ Elektroda A (SS316) ]
                 ~  (Cairan Susu: R_susu)
          [ Elektroda B (SS316) ]
                 |
    [ESP32-S3 GPIO 5 (Drive B)]

```

#### Spesifikasi Pinout Mikrokontroler ESP32-S3:

* **GPIO 4 (Drive A):** Pin output digital fase positif ($3.3\text{ V}$).


* **GPIO 5 (Drive B):** Pin output digital fase pembalik polaritas ($0\text{ V}$).


* **GPIO 1 (ADC1_CH0):** Pin pembacaan tegangan analog ($V_{out}$). Kanal ADC1 dipilih karena memiliki isolasi sirkuit terhadap modul radio Wi-Fi/Bluetooth, mencegah *analog saturation* atau gangguan saat transmisi data nirkabel aktif.



#### Siklus Eksitasi AC Mikro (<1 ms per sampel):

1. **Fase Eksitasi Positif ($100\ \mu\text{s}$):** GPIO 4 diset `HIGH` ($3.3\text{ V}$), GPIO 5 diset `LOW` ($0\text{ V}$). Arus mengalir dari Elektroda A ke B. Pin ADC membaca $V_{out}$.


2. **Fase Pembalikan Polaritas ($100\ \mu\text{s}$):** GPIO 4 diset `LOW` ($0\text{ V}$), GPIO 5 diset `HIGH` ($3.3\text{ V}$). Elektron dialirkan berbalik arah untuk menetralkan penumpukan muatan pada probe.


3. **Fase Relaksasi / Floating ($2\text{ ms}$):** Kedua pin diset `LOW` (mode mati) untuk membiarkan medan elektrokimia larutan berada dalam kondisi setimbang (*electrostatic relaxation*).



Perhitungan resistansi cairan ($R_{susu}$) didasarkan pada hukum pembagi tegangan:

$$R_{susu} = R_{ref} \cdot \left( \frac{V_{out}}{V_{in} - V_{out}} \right)$$

Dimana $V_{in} = 3.3\text{ V}$ dan $R_{ref} = 1000\ \Omega$ (toleransi 1% *metal-film*).

### 2. Standarisasi Probe Elektroda

Konduktansi larutan ($G$) memiliki relasi langsung dengan luas penampang elektroda ($A$), jarak celah antar-elektroda ($d$), dan konduktivitas spesifik larutan ($EC$):

$$G = \frac{1}{R_{susu}} = EC \cdot \frac{A}{d} = \frac{EC}{K}$$

Dimana $K = \frac{d}{A}$ didefinisikan sebagai **Konstanta Geometri Sel Probe ($cm^{-1}$)**.

```
                <--- d = 5-8 mm --->
                |                  |
           +----+----+        +----+----+
           |  SS316  |        |  SS316  |
           | Kawat   |        | Kawat   |
           | Ø 2.0mm |        | Ø 2.0mm |
           |         |        |         |
           | [ISOLASI]        | [ISOLASI]
           | Selong- |        | Selong- |
           |  song   |        |  song   |
           |  Bakar  |        |  Bakar  |
           | (Heat-  |        | (Heat-  |
           | Shrink) |        | Shrink) |
           |         |        |         |
           +----+----+        +----+----+
                |                  |  
                |  Ujung Aktif     |  h = 5.0 mm (Presisi)
                |  Logam Terbuka   |  [Titik Kontak Ionik]
                +------------------+

```

#### Alasan Ilmiah Pembatasan Ujung Aktif 5.0 mm:

1. **Invariansi Konstanta Sel Terhadap Kedalaman Celup:** Jika kawat dibiarkan terbuka sepanjang 3–5 cm, kedalaman celup yang berbeda akan mengubah nilai luas area basah ($A = 2\pi r \cdot h$). Akibatnya, nilai $K$ berfluktuasi secara masif di setiap pengujian. Isolasi selongsong bakar (*heat-shrink tube*) mengunci panjang basah elektroda secara kaku pada $h = 5.0\text{ mm}$, menjamin nilai $K$ bernilai konstan ($100\%$ invarian) tidak peduli seberapa dalam probe dimasukkan ke dalam wadah susu.


2. **Impedance Matching ke Zona Linear ADC ESP32-S3:** Konduktivitas susu murni relatif tinggi ($4.0 - 7.0\text{ mS/cm}$). Kawat telanjang panjang akan memperbesar $A$, menurunkan resistansi cairan hingga $<30\ \Omega$. Pada pembagi tegangan $1\text{ k}\Omega$, tegangan $V_{out}$ jatuh di bawah $0.1\text{ V}$—masuk ke *dead-zone* ADC ESP32 yang non-linear. Ujung aktif $5.0\text{ mm}$ menahan resistansi cairan pada rentang optimal $150\ \Omega - 300\ \Omega$, menghasilkan rentang tegangan $0.4\text{ V} - 0.7\text{ V}$ yang berada tepat di tengah area paling linear dan minim *noise* dari ADC1.


3. **Pemberantasan Efek Fringe Field & Kapasitansi Lapisan Ganda ($C_{dl}$):** Ujung 5 mm membatasi jalur medan listrik hanya melintas rapat di antara kedua elektroda (berfungsi sebagai *point electrodes*), meniadakan distorsi medan liar (*stray fields*) akibat dinding wadah seng atau aluminium. Luas permukaan kecil juga menekan nilai kapasitansi parasitik antarmuka logam-cairan ($C_{dl}$).


4. **Anti-Biofouling Lemak & Kasein:** Globula lipid dan misel protein susu sangat mudah teradsorpsi pada permukaan logam. Area kontak aktif seluas $5\text{ mm}$ memiliki rasio bilas (*rinse ratio*) tinggi, mencegah penumpukan kerak isolator lemak secara akumulatif.



### 3. Integrasi Sensor Suhu RTD PT100 + MAX31865

Kompensasi termal wajib dilakukan karena konduktivitas listrik cairan memiliki koefisien temperatur positif sebesar $\alpha \approx 2.0\% / ^\circ\text{C}$. Fluktuasi suhu sebesar $5^\circ\text{C}$ dapat membiaskan pembacaan EC hingga $10\%$, yang dapat mengacaukan pembedaan status mutu.

Sistem menggunakan probe RTD PT100 Platinum kelas industri *food-grade* yang dihubungkan ke IC amplifier presisi MAX31865 dengan topologi 3-kawat (*3-wire configuration*).

#### Konfigurasi Bus SPI ESP32-S3:

* **SCK:** GPIO 12


* **MISO:** GPIO 13


* **MOSI:** GPIO 11


* **CS (Chip Select):** GPIO 10



#### Mitigasi Interferensi Elektrik (*Floating Shield Protection*):

Selongsong logam stainless steel sensor PT100 merupakan konduktor yang tercelup bersamaan ke dalam susu. Jika selongsong PT100 terhubung ke ground sirkuit (GND ESP32), arus eksitasi AC dari probe EC akan mengalami kebocoran (*ground-loop leakage*), membiaskan kurva pembagi tegangan dan merusak integritas data suhu pada MAX31865. Selubung logam PT100 **wajib dibiarkan mengambang (*isolated floating shield*)** terhadap sirkuit daya mikrokontroler, dengan jarak fisik minimum 1.5 cm dari probe EC pada *housing casing*.

### 4. Pipeline & Time-Multiplexing Sensor

Untuk menjamin tidak adanya tabrakan gelombang listrik di dalam cairan, firmware menerapkan **Time-Multiplexing Sequence**:

```
Time (s)  0.0            3.0       3.1           3.3           3.31
          |---------------|---------|-------------|-------------|
Operasi:  Stabilisasi     Baca      Eksitasi AC   Kompensasi    Inferensi
          Termal          PT100     & Baca EC     EC25          TinyML
Status:   Probe Masuk     EC: OFF   PT100: IDLE   Matematika    Output

```

1. **Fase Stabilisasi Termal ($t = 0.0\text{ s} - 3.0\text{ s}$):** Memberikan jeda waktu bagi selongsong probe untuk mencapai kesetimbangan termodinamika cairan susu (*thermal equilibrium*).


2. **Fase Pembacaan Suhu ($t = 3.0\text{ s} - 3.1\text{ s}$):** Pin GPIO 4 dan 5 dikunci pada level `LOW` (mati total). ESP32-S3 membaca register resistansi RTD melalui bus SPI MAX31865 (waktu konversi internal ADC MAX31865: $\approx 65\text{ ms}$). Suhu aktual susu ($T$) tersimpan.


3. **Fase Pembacaan Konduktivitas ($t = 3.1\text{ s} - 3.3\text{ s}$):** Jalur SPI PT100 di-idle-kan. Sistem mengaktifkan burst eksitasi AC bolak-balik selama 200 ms untuk mengambil 50 sampel ADC oversampling, lalu menghitung nilai median $R_{susu}$. Pin penggerak dikembalikan ke status `LOW`.


4. **Fase Kompensasi Termal Non-Linear ($t = 3.3\text{ s}$):** Konduktivitas mentah dihitung dan dinormalisasi secara non-linear ke temperatur referensi baku $25^\circ\text{C}$ ($EC_{25}$) menggunakan koefisien kompensasi susu murni $\alpha = 0.020$:



$$EC_{raw} = \left( \frac{1}{R_{susu}} \right) \cdot K_{cell} \cdot 1000 \quad [\text{mS/cm}]$$

$$EC_{25} = \frac{EC_{raw}}{1.0 + \alpha \cdot (T - 25.0)} \quad [\text{mS/cm}]$$

---

## BAB III: PROTOKOL PENGUMPULAN DATA & VALIDASI BIOKIMIA

### 1. Skenario Variasi Suhu Eksperimen

Pelatihan model prediktif multivariat memerlukan dataset runtun waktu yang memetakan dinamika degradasi biologis susu di bawah berbagai tekanan termal lingkungan. Pengujian dilakukan simultan menggunakan 3 wadah toples susu segar murni (masing-masing 1.5 Liter, perahan hari yang sama dari sapi perah sehat) selama durasi 6 jam penuh (360 menit):

```
+---------------------------------------------------------------------------------------------------+
|                               SKENARIO MATRIKS UJI SUHU LINGKUNGAN                                |
+------------------+----------------+---------------------------------------------------------------+
| Sampel / Wadah   | Rentang Suhu   | Karakteristik Biologis & Peran Pelatihan                      |
+------------------+----------------+---------------------------------------------------------------+
| Wadah Dingin     | 10°C – 15°C    | Simulasi penggunaan cooler box es peternak. Laju pembelahan   |
| (S_DINGIN)       | (Cooler Box)   | bakteri tertekan dalam lag phase panjang. Memasok data Grade A|
|                  |                | stabil dan bertindak sebagai negative control (censored data)[cite: 1].|
+------------------+----------------+---------------------------------------------------------------+
| Wadah Ruang      | 25°C – 30°C    | Kondisi riil kandang/pos KUD tropis. Bakteri aktif membelah   |
| (S_RUANG)        | (Ambient)      | pada jam ke-2 hingga ke-4; kurva baseline degradasi normal[cite: 1, 2].|
+------------------+----------------+---------------------------------------------------------------+
| Wadah Hangat     | 35°C – 38°C    | Accelerated shelf-life testing (pembusukan dipercepat akibat  |
| (S_HANGAT)       | (Water Bath)   | paparan terik matahari bak pengangkut terbuka). Kerusakan masif|
|                  |                | terjadi dalam 1.5 – 2.5 jam[cite: 1].                                 |
+------------------+----------------+---------------------------------------------------------------+

```

### 2. SOP Pengambilan Sampel Celup Berkala

Guna mencegah fenomena *fouling* lapisan lipid pada probe, eksperimen menggunakan **Metode Celup Berkala (*Intermittent Dipping Protocol*)**:

```
      [Interval Diam: 10 Menit]
           (Probe di Udara)
                  |
                  v
       [Aduk Susu Perlahan 3s]
                  |
                  v
    [Celupkan Probe: Total 10-15s]
    - Detik 0-3 : Penyetaraan Suhu Logam (Settling Time)
    - Detik 4-8 : 5-Burst Sampling (1 Baris Data / Detik)
    - Detik 9-10: Angkat Probe dari Susu
                  |
                  v
   [Protokol Pembilasan & Sanitasi]
   - Semprot Aquades Deionisasi (Melarutkan Sisa Kasein/Lemak)
   - Tepuk Kering Menggunakan Tisu Bebas Serat (Lint-Free)
                  |
                  v
       [Kembali ke Interval Diam]

```

* **Interval Siklus:** Dilakukan tepat setiap **10 menit sekali** pada ketiga wadah secara bergiliran.


* **Mekanisme 5-Burst Sampling:** Saat probe tercelup di detik ke-4 hingga ke-8, mikrokontroler merekam 5 kali pembacaan berturut-turut ($1\text{ baris data per detik}$). Kelima baris ini menangkap fluktuasi derau mikroskopis ADC di lapangan, yang berfungsi sebagai augmentasi alami (*sensor jitter augmentation*) agar model tidak *overfit* terhadap angka diskrit.



### 3. Ground Truth Benchmarking

Label biologis mutlak (*ground truth*) diuji secara paralel setiap **30 menit** (pada menit ke-0, 30, 60, ..., 360) sebagai titik jangkar (*anchor points*):

#### SOP Uji Alkohol 70% Standar Penerimaan Industri:

1. Ambil tepat $1.0\text{ ml}$ susu menggunakan spuit tanpa jarum berukuran $1\text{ ml}$.


2. Tuangkan ke dalam cup plastik mini transparan atau sendok stainless steel bersih.


3. Tambahkan $1.0\text{ ml}$ alkohol $70\%$ menggunakan spuit kedua (rasio volumetrik mutlak $1:1$).


4. Putar melingkar perlahan selama 5 detik, lalu amati dinding wadah di bawah penyinaran cahaya:


* **`NEGATIF`:** Cairan mengalir mulus, homogen, tidak meninggalkan butiran pasir.


* **`SERPIHAN_HALUS`:** Tampak partikel mikro atau pasir halus menempel di dinding wadah saat dimiringkan.


* **`PECAH_PADAT`:** Terkoagulasi masif menyerupai bubur tahu; cairan bening (*whey*) memisah jelas dari dadih (*curd*).





#### Pengukuran pH Digital:

Probe pH meter laboratorium terkalibrasi dua titik ($pH\ 4.01$ dan $pH\ 6.86$) dicelupkan ke sampel kontrol untuk membaca pergeseran konsentrasi ion hidrogen secara kuantitatif.

#### Logika Penanganan Wadah Dingin (Negative Control & Censored Data):

Susu pada wadah dingin ($10^\circ\text{C} - 15^\circ\text{C}$) tidak akan pecah/rusak dalam rentang pengujian 6 jam. Dalam teori reliabilitas machine learning, kondisi ini merupakan *right-censored biological data* yang sangat valid.

* Seluruh data wadah dingin dari menit ke-0 hingga ke-360 diberi label mutu mutlak: **`GRADE_A`** (karena hasil uji alkohol selalu negatif dan $pH \ge 6.6$).


* Untuk target regresi sisa waktu (`label_shelf_life_min`), sistem menerapkan **Metode Capping Batas Operasional Maksimum (360 Menit)**. Selama suhu $\le 15^\circ\text{C}$ dan konduktivitas stabil, nilai dipatok pada batas atas $360\text{ menit}$. Hal ini menjaga distribusi loss fungsi regresi tetap stabil tanpa terdistorsi oleh angka ekstrapolasi tak hingga.



---

## BAB IV: MANAJEMEN DATASET & FEATURE ENGINEERING

### 1. Skema Tabel CSV Mentah

Berkas pencatatan gabungan dari MicroSD ESP32-S3 dan log pengujian manual laboratorium disimpan dengan spesifikasi kolom terstruktur:

```
+---------------------------------------------------------------------------------------------------+
|                                    SKEMA STRUKTUR DATASET MENTAH                                  |
+---------------------+---------+-------------------+-----------------------------------------------+
| Nama Kolom          | Tipe    | Kategori Pipeline | Definisi & Penjelasan Teknis                  |
+---------------------+---------+-------------------+-----------------------------------------------+
| sample_id           | String  | Metadata (Drop)   | Identitas wadah: 'S_RUANG', 'S_DINGIN', dll[cite: 1].|
| timestamp           | Int64   | Metadata (Drop)   | Unix epoch time (detik) saat pengujian[cite: 1, 2].   |
| elapsed_min         | Int32   | Temporal Operator | Menit relatif sejak awal perah (penyebut Δt)[cite: 1].|
| burst_idx           | Int8    | Metadata (Drop)   | Indeks urutan sampling burst (1 sampai 5)[cite: 1].    |
| temp_c              | Float32 | Feature Input (X) | Temperatur aktual susu (°C) dari PT100[cite: 1, 2].  |
| r_liquid            | Float32 | Sensor Mentah(Drop| Hambatan analog cairan mentah (Ohm)[cite: 1, 2].     |
| ec_25               | Float32 | Feature Input (X) | Konduktivitas cairan suhu standar 25°C (mS/cm)|
| ph_actual           | Float32 | Ground Truth(Drop)| pH objektif dari instrumen pH meter digital[cite: 1].|
| alcohol_test        | String  | Ground Truth(Drop)| Hasil uji alkohol: NEGATIF, dll[cite: 1].    |
| label_grade         | String  | Target Output (y1)| Kelas mutu biologis (GRADE_A, B, C)[cite: 1, 2].     |
| label_shelf_life_min| Float32 | Target Output (y2)| Hitung mundur sisa waktu aman (menit)[cite: 1, 2].   |
+---------------------+---------+-------------------+-----------------------------------------------+

```

### 2. Data Preprocessing & Pipeline Cleaning

Sebelum dataset disalurkan ke pipeline model, proses pembersihan dijalankan dengan urutan ketat:

```
[CSV Mentah]
     |
     v
[Agregasi 5-Burst Sampling per Sesi (Mean temp_c & ec_25)]
     |
     v
[Perhitungan delta_ec_rate (Delta EC25 / Delta t) via elapsed_min]
     |
     v
[Eliminasi Metadata & Raw Columns (sample_id, timestamp, r_liquid)] --> Mencegah Data Leakage!
     |
     v
[Pemisahan Ground Truth Biologis (ph_actual, alcohol_test)] ---------> Digunakan murni untuk audit
     |
     +-----------------------------------+
     |                                   |
     v                                   v
[Matriks Input X]                 [Vektor Target y]
- temp_c                          - y1 (Klasifikasi): One-Hot label_grade
- ec_25                           - y2 (Regresi): label_shelf_life_min
- delta_ec_rate

```

* **Pencegahan Data Leakage:** Kolom identitas wadah (`sample_id`), stempel waktu (`timestamp`), dan nilai biologis manual (`ph_actual`, `alcohol_test`) **mutlak dibuang**. Jika tidak dibuang, jaringan saraf tiruan akan menghafal bahwa ID wadah tertentu selalu menghasilkan grade tertentu, menyebabkan model gagal melakukan generalisasi di lingkungan baru.



### 3. Kalkulasi Kinetika Ion ($\Delta EC / \Delta t$)

Laju pergeseran ionik dihitung menggunakan turunan pertama konduktivitas terhadap interval waktu antar-sesi ($10\text{ menit}$):

$$\frac{\Delta EC_{25}}{\Delta t} = \frac{EC_{25}(t) - EC_{25}(t-1)}{t - (t-1)}$$

Script implementasi Python (Pandas) untuk pra-pemrosesan data:

```python
import numpy as np
import pandas as pd
from sklearn.preprocessing import MinMaxScaler

# 1. Load dataset eksperimen lab
df_raw = pd.read_csv("dataset_susu_mentah.csv")  #[cite: 1]

# 2. Agregasi burst sampling (mereduksi 5 baris burst menjadi 1 baris rata-rata stabil)
df_grouped = (
    df_raw.groupby(["sample_id", "elapsed_min"])
    .agg(
        {
            "temp_c": "mean",
            "ec_25": "mean",
            "label_grade": "first",
            "label_shelf_life_min": "first",
        }
    )
    .reset_index()
)  #[cite: 1]

# 3. Urutkan berdasarkan wadah dan linimasa waktu
df_grouped = df_grouped.sort_values(
    by=["sample_id", "elapsed_min"]
).reset_index(drop=True)  #[cite: 1]

# 4. Kalkulasi Delta EC / Delta t (Kinetika Ionik)
# Menghitung selisih pembacaan terhadap sesi 10 menit sebelumnya per grup wadah
delta_ec = df_grouped.groupby("sample_id")["ec_25"].diff()  #[cite: 1]
delta_t = df_grouped.groupby("sample_id")["elapsed_min"].diff()  #[cite: 1]

df_grouped["delta_ec_rate"] = delta_ec / delta_t  #[cite: 1]
# Imputasi t0 (menit ke-0) dengan 0.0 karena belum terjadi perubahan kinetika
df_grouped["delta_ec_rate"] = df_grouped["delta_ec_rate"].fillna(
    0.0
)  #[cite: 1]

# 5. Ekstraksi Matriks Fitur (X) dan Target (y)
feature_cols = ["temp_c", "ec_25", "delta_ec_rate"]  #[cite: 1]
X = df_grouped[feature_cols].values  #[cite: 1]

# Normalisasi Min-Max Scaler ke rentang [0.0, 1.0]
scaler = MinMaxScaler()  #[cite: 1]
X_scaled = scaler.fit_transform(X)  #[cite: 1]

# Target 1: One-Hot Encoding Klasifikasi Mutu (Grade A, B, C)
y_grade = pd.get_dummies(df_grouped["label_grade"])[
    ["GRADE_A", "GRADE_B", "GRADE_C"]
].values  #[cite: 1]

# Target 2: Regresi Sisa Waktu Simpan (Menit)
y_shelf_life = df_grouped["label_shelf_life_min"].values.astype(
    np.float32
)  #[cite: 1]

```

### 4. Logika Pelabelan Retrospektif

Pelabelan target luaran dieksekusi secara terstruktur melalui dua fungsi deterministik:

#### Matriks Keputusan Penentuan Mutu (`label_grade`):

$$\text{Grade} = \begin{cases}  \text{GRADE\_A}, & \text{jika } \text{alcohol\_test} = \text{NEGATIF} \land pH \ge 6.6 \\  \text{GRADE\_B}, & \text{jika } \text{alcohol\_test} = \text{SERPIHAN\_HALUS} \lor (6.4 \le pH < 6.6) \\  \text{GRADE\_C}, & \text{jika } \text{alcohol\_test} = \text{PECAH\_PADAT} \lor pH < 6.3  \end{cases}$$

#### Formula Hitung Mundur Sisa Waktu (`label_shelf_life_min`):

Berdasarkan pencatatan waktu faktual pecah ($T_{rusak}$) pada wadah terkait:

$$\text{label\_shelf\_life\_min}(t) = \max\Big(0, \ T_{rusak} - t\Big)$$

* Contoh pada Wadah Ruang ($T_{rusak} = 240\text{ menit}$): Pada $t = 60$, nilai label adalah $240 - 60 = 180\text{ menit}$. Pada $t = 240$, nilai menyentuh $0\text{ menit}$, dan untuk $t > 240$, nilai dikunci konstan pada $0\text{ menit}$.



---

## BAB V: PEMODELAN MACHINE LEARNING (ON-DEVICE TINYML)

### 1. Pemilihan Algoritma

Sistem mengadopsi arsitektur **Multi-Task Multi-Layer Perceptron (MLP)**.

```
+---------------------------------------------------------------------------------------------------+
|                                PERBANDINGAN PENDEKATAN MODEL DI ESP32-S3                          |
+--------------------+------------------------+------------------------+----------------------------+
| Parameter          | Klasik (Random Forest) | Deep Learning (LSTM)   | Multi-Task MLP (Dipilih)   |
+--------------------+------------------------+------------------------+----------------------------+
| Footprint Memori   | Sedang (Pohon if-else  | Berat (>200 KB Flash), | Sangat Ringan (<15 KB INT8)|
| SRAM ESP32-S3      | butuh banyak node code)| boros alokasi SRAM[cite: 1].| aman dalam SRAM 512KB[cite: 1].   |
+--------------------+------------------------+------------------------+----------------------------+
| Multi-Task Output  | Tidak Mendukung        | Mendukung              | Sangat Optimal             |
| Eksekusi           | (Wajib deploy 2 model) | (Terlalu berlebihan)   | (1 Backbone, 2 Heads)[cite: 1].   |
+--------------------+------------------------+------------------------+----------------------------+
| Native INT8 Support| Terbatas/Perlu Parser  | Kompleks (Operasi      | Terintegrasi penuh pada    |
| TFLite Micro       | C manual               | Recurrent Gate)        | TFLite Micro via ESP-NN[cite: 1]. |
+--------------------+------------------------+------------------------+----------------------------+
| Latensi Inferensi  | ~15 ms                 | >120 ms                | < 5 ms (Instruksi Vektor)[cite: 1]|
+--------------------+------------------------+------------------------+----------------------------+

```

### 2. Topologi Jaringan Saraf Tiruan (Multi-Head Architecture)

Model dirancang bertubuh tunggal (*shared backbone*) untuk mengekstraksi representasi biofisika cairan, kemudian bercabang menjadi dua kepala output terpisah:

```
                     [ Input Vector: 3 Dimensi ]
                     [ temp_c, ec_25, delta_ec ]
                                  |
                                  v
                    [ Shared Dense Layer 1: 16 Units ]
                    [ Aktivasi: ReLU | BatchNorm ]
                                  |
                                  v
                    [ Shared Dense Layer 2: 8 Units ]
                    [ Aktivasi: ReLU ]
                                  |
                 +----------------+----------------+
                 |                                 |
                 v                                 v
     [ Head 1: Klasifikasi Mutu ]     [ Head 2: Regresi Sisa Waktu ]
     [ Dense Layer: 3 Units ]         [ Dense Layer: 1 Unit ]
     [ Aktivasi: Softmax ]            [ Aktivasi: Linear ]
     [ Output: P(A), P(B), P(C) ]     [ Output: Menit Simpan ]

```

### 3. Konfigurasi Pelatihan & Metrik Evaluasi

Script implementasi model menggunakan TensorFlow/Keras Functional API:

```python
import tensorflow as tf
from tensorflow.keras import layers, Model  #[cite: 3, 24]

# 1. Definisi Input Layer
inputs = layers.Input(
    shape=(3,), name="sensor_features"
)  # [temp_c, ec_25, delta_ec_rate][cite: 1, 8]

# 2. Shared Backbone
x = layers.Dense(16, activation="relu", name="shared_dense_1")(
    inputs
)  #[cite: 1, 8]
x = layers.Dense(8, activation="relu", name="shared_dense_2")(x)  #[cite: 1, 8]

# 3. Branching Heads
out_classification = layers.Dense(3, activation="softmax", name="grade_output")(
    x
)  #[cite: 1, 8]
out_regression = layers.Dense(1, activation="linear", name="shelf_life_output")(
    x
)  #[cite: 1, 8]

# 4. Instansiasi Model
model = Model(
    inputs=inputs,
    outputs=[out_classification, out_regression],
    name="MilkQualityMultiTaskMLP",
)  #[cite: 1, 3]

# 5. Kompilasi Model dengan Multi-Loss Weighting
model.compile(
    optimizer=tf.keras.optimizers.Adam(learning_rate=0.005),  #[cite: 1, 3]
    loss={
        "grade_output": "categorical_crossentropy",  #[cite: 1, 3]
        "shelf_life_output": "mean_squared_error",  #[cite: 1, 3]
    },
    loss_weights={
        "grade_output": 1.0,  # Bobot prioritas klasifikasi
        "shelf_life_output": 0.01,  # Normalisasi magnitudo MSE menit
    },
    metrics={
        "grade_output": ["accuracy"],  #[cite: 10, 18]
        "shelf_life_output": ["mae"],  #[cite: 1, 3]
    },
)

model.summary()  #[cite: 14]

```

#### Kriteria Ambang Batas Kelayakan Produksi:

* **Klasifikasi:** F1-Score pada kelas `GRADE_C` wajib $\ge 98\%$ dengan matriks kontingensi menunjukkan **$0$ False Negatives** (tidak ada toleransi untuk susu rusak yang terprediksi sebagai Grade A).


* **Regresi:** Mean Absolute Error (MAE) pada set data uji $\le 12\text{ menit}$.



### 4. Kuantisasi & Deployment TinyML (TFLite Micro)

Model dilatih dalam presisi 32-bit Floating Point (FP32), kemudian dikonversi menjadi format bilangan bulat 8-bit Integer (INT8) menggunakan TensorFlow Lite Converter dengan *Full Integer Post-Training Quantization*:

```python
# Kalibrasi dataset perwakilan untuk menetapkan rentang kuantisasi INT8
def representative_data_gen():
  for i in range(len(X_scaled)):
    # Mengalirkan sampel skalar ke generator tensor rank-2
    yield [X_scaled[i : i + 1].astype(np.float32)]


converter = tf.lite.TFLiteConverter.from_keras_model(model)  #[cite: 1]
converter.optimizations = [tf.lite.Optimize.DEFAULT]  #[cite: 1]
converter.representative_dataset = representative_data_gen  #[cite: 1]

# Memastikan operasi komputasi dikunci penuh ke format Integer murni
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type = tf.int8
converter.inference_output_type = tf.int8

tflite_quant_model = converter.convert()  #[cite: 1]

# Simpan ke format biner .tflite
with open("milk_quality_model_int8.tflite", "wb") as f:
  f.write(tflite_quant_model)

```

#### Ekspor ke C-Array (`model_data.h`):

Berkas biner `.tflite` dikonversi ke kode sumber C++ menggunakan utility Linux `xxd`:

```bash
xxd -i milk_quality_model_int8.tflite > model_data.h

```

Header `model_data.h` menghasilkan representasi array biner di memori Flash:

```cpp
const unsigned char g_milk_quality_model_data[] = {
  0x1c, 0x00, 0x00, 0x00, 0x54, 0x46, 0x4c, 0x33, 0x14, 0x00, 0x20, 0x00,
  ...
};
const int g_milk_quality_model_data_len = 11456; // Ukuran ringkas ~11.4 KB

```

---

## BAB VI: ARSITEKTUR IOT OFFLINE-FIRST & CLOUD

### 1. Mekanisme Offline Buffer MicroSD

Alat ukur beroperasi secara mandiri di area kandang pelosok yang tidak memiliki cakupan sinyal nirkabel. Setiap siklus inferensi menghasilkan struktur data terenkapsulasi yang ditulis langsung ke antrean berkas lokal `/spool/queue.jsonl` pada kartu memori MicroSD (antarmuka SPI):

#### Spesifikasi Struktur Payload Telemetri (JSON):

```json
{
  "device_id": "ESP32S3-MILK-001",
  "farmer_id": "PTR-KUD-042",
  "batch_id": "CAN-A-09",
  "timestamp": 1714567890,
  "metrics": {
    "temperature_c": 28.45,
    "r_liquid_ohm": 192.1,
    "ec_25": 4.621,
    "delta_ec_rate": 0.0048
  },
  "prediction": {
    "grade": "GRADE_A",
    "shelf_life_remaining_min": 175,
    "confidence_score": 0.964,
    "is_rejected": false
  }
}

```

* Nilai `timestamp` dicatat dari RTC internal ESP32-S3 yang telah tersinkronisasi SNTP. Hal ini menjamin audit historis waktu perah tetap akurat meskipun data baru tersinkronisasi ke server beberapa jam kemudian.



### 2. Logika Sinkronisasi Asinkron

Mekanisme pengosongan antrean (*spooling upload*) dijalankan di latar belakang (*background task*) pada Core 0 ESP32-S3, sementara Core 1 didedikasikan penuh untuk akuisisi sensor dan inferensi TinyML:

```
[Loop Rutin Firmware] 
        |
        +---> Periksa Status Jaringan Wi-Fi (Non-blocking scan)
                    |
          +---------+---------+
          |                   |
     [Tidak Terhubung]   [Terhubung]
          |                   |
          v                   v
     [Tetap Simpan      [Buka File /spool/queue.jsonl di MicroSD]
      ke MicroSD]             |
                              v
                        [Baca Baris Antrean Tertua]
                              |
                              v
                        [HTTP POST /api/v1/telemetry / Publish MQTT]
                              |
                     +--------+--------+
                     |                 |
               [HTTP 200 OK]    [Koneksi Putus / Timeout]
                     |                 |
                     v                 v
          [Tandai Pointer /       [Tutup File & Coba Lagi
           Hapus Baris Terkirim]   pada Siklus Berikutnya]

```

Payload dikirimkan secara sekuensial (*batch chunk*). Penghapusan baris antrean pada MicroSD hanya dieksekusi apabila server backend merespons dengan kode status **`HTTP 200 OK`** atau konfirmasi **`MQTT PUBACK`**.

### 3. Aplikasi Dasbor Pemantauan KUD

Data yang berhasil tersinkronisasi di server diproses oleh sistem backend terpusat untuk disajikan ke operator Koperasi Unit Desa:

```
+---------------------------------------------------------------------------------------------------+
|                        DASBOR SENTRALISASI LOGISTIK KUD SUSU MANDIRI                              |
+---------------------------------------------------------------------------------------------------+
| STATUS ARMADA LOGISTIK TRUK PENDINGIN:                                                            |
| Truk 01: En-route Peternak B (Rute Prioritas 1) | Suhu Tangki: 4.1°C                              |
| Truk 02: Standby Pos Penampungan Barat           | Suhu Tangki: 3.8°C                              |
+---------------------------------------------------------------------------------------------------+
| MONITORING ANTREAN MUTU SUSU TINGKAT PETERNAK (REAL-TIME):                                       |
| [!] ALARM PRIORITAS KRITIS:                                                                       |
| ID Peternak : PTR-012 (Kelompok Jaya 1)                                                           |
| Sisa Waktu  : 35 Menit (Status: EARLY WARNING / GRADE B)                                          |
| Lokasi      : Desa Sukamaju RT 02 (Jarak Tempuh Armada: 18 Menit)                                 |
| Tindakan    : Dispatch Otomatis Truk 01 Dialihkan Mendahulukan Peternak Ini!                      |
+---------------------------------------------------------------------------------------------------+
| HISTORI PENERIMAAN SUSU HARI INI:                                                                |
| - Total Volume Diuji : 4.250 Liter                                                                |
| - Grade A (Premium)  : 89.2% (Diterima Penuh)                                                     |
| - Grade B (Warning)  : 9.8%  (Segera Didinginkan ke Tangki Pusat)                                |
| - Grade C (Reject)   : 1.0%  (Ditolak di Tingkat Kandang - Zero Spill Waste di Tangki KUD)       |
+---------------------------------------------------------------------------------------------------+

```

#### Dampak Operasional Integrasi Cloud:

1. **Transparansi Transaksi Mutu:** Peternak dan pos penampungan memiliki bukti data biofisika numerik yang sama, meniadakan perdebatan subjektif saat penetapan harga susu per liter.


2. **Dynamic Routing Armada Pendingin:** Dasbor secara otomatis mengkalkulasi ulang rute truk tangki pendingin untuk menjemput susu milik kelompok ternak yang memiliki *Estimated Shelf-Life Window* paling mendesak, menyelamatkan komoditas sebelum terlanjur mengalami koagulasi asam.
