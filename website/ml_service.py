import os
import datetime
import numpy as np

# Konstanta Normalisasi Min-Max
MIN_VALS = np.array([3.560000, 253.300003, 1.598000, 2.565000], dtype=np.float32)
SCALE_VALS = np.array([0.031867, 0.003509, 0.226655, 0.359583], dtype=np.float32)
SHELF_MAX_MINUTES = 360.0

# PENGATURAN SIMULASI WAKTU (Ubah ke 1.0 untuk kecepatan normal dunia nyata)
# Jika bernilai 60.0, artinya 1 detik dunia nyata = 1 menit penyusutan susu (sangat cocok untuk demo pengujian)
TIME_ACCELERATION_FACTOR = 1.0  

class MilkMLService:
    def __init__(self, weights_path="model_weights.npz"):
        self.weights_loaded = False
        if os.path.exists(weights_path):
            data = np.load(weights_path)
            self.w1, self.b1 = data['w1'], data['b1']
            self.w2, self.b2 = data['w2'], data['b2']
            self.w_grade, self.b_grade = data['w_grade'], data['b_grade']
            self.w_shelf, self.b_shelf = data['w_shelf'], data['b_shelf']
            self.weights_loaded = True
        else:
            print(f"[PERINGATAN] File bobot '{weights_path}' belum ditemukan.")

    def predict(self, suhu: float, r_liquid: float, ec_raw: float, ec25: float):
        """Menjalankan inferensi forward-pass FP32 murni via NumPy."""
        if not self.weights_loaded:
            return {"error": "Bobot model belum dimuat"}

        raw_x = np.array([suhu, r_liquid, ec_raw, ec25], dtype=np.float32)
        x = (raw_x - MIN_VALS) * SCALE_VALS

        h1 = np.maximum(0.0, np.dot(x, self.w1) + self.b1)
        h2 = np.maximum(0.0, np.dot(h1, self.w2) + self.b2)

        logits_grade = np.dot(h2, self.w_grade) + self.b_grade
        pred_grade = ["GRADE_A", "GRADE_B", "GRADE_C"][int(np.argmax(logits_grade))]

        reg_shelf = np.dot(h2, self.w_shelf) + self.b_shelf
        shelf_norm = max(0.0, float(reg_shelf[0]))
        pred_shelf_min = int(round(np.clip(shelf_norm * SHELF_MAX_MINUTES, 0, SHELF_MAX_MINUTES)))

        return {
            "grade": pred_grade,
            "sisa_waktu_menit": pred_shelf_min
        }

    @staticmethod
    def _parse_timestamp(ts_val) -> datetime.datetime:
        """Parser kebal error untuk berbagai format SQLite / ISO / Object Datetime."""
        if isinstance(ts_val, datetime.datetime):
            if ts_val.tzinfo is None:
                return ts_val.replace(tzinfo=datetime.timezone.utc)
            return ts_val

        if not ts_val:
            return datetime.datetime.now(datetime.timezone.utc)

        ts_str = str(ts_val).strip().replace("Z", "+00:00")

        # 1. Coba ISO format
        try:
            dt = datetime.datetime.fromisoformat(ts_str)
            if dt.tzinfo is None:
                dt = dt.replace(tzinfo=datetime.timezone.utc)
            return dt
        except Exception:
            pass

        # 2. Coba berbagai varian format teks SQLite (dengan/tanpa mikrodetik)
        formats = (
            "%Y-%m-%d %H:%M:%S.%f",
            "%Y-%m-%d %H:%M:%S",
            "%Y-%m-%d %H:%M",
            "%d/%m/%Y %H:%M:%S"
        )
        for fmt in formats:
            try:
                dt = datetime.datetime.strptime(ts_str, fmt)
                return dt.replace(tzinfo=datetime.timezone.utc)
            except ValueError:
                continue

        # Jika tetap gagal, gunakan waktu sekarang sebagai fallback darurat
        print(f"[ERROR PARSING] Gagal membaca format waktu: '{ts_val}'")
        return datetime.datetime.now(datetime.timezone.utc)

    @staticmethod
    def calculate_current_shelf_life(created_at_str, sisa_waktu_awal: int):
        """Menghitung peluruhan waktu simpan dinamis (Time-Decay)."""
        created_at = MilkMLService._parse_timestamp(created_at_str)
        now = datetime.datetime.now(datetime.timezone.utc)

        # Selisih dalam detik
        elapsed_seconds = (now - created_at).total_seconds()
        if elapsed_seconds < 0:
            elapsed_seconds = 0.0

        # Terapkan percepatan waktu jika diaktifkan untuk demo
        effective_elapsed_seconds = elapsed_seconds * TIME_ACCELERATION_FACTOR
        elapsed_minutes = effective_elapsed_seconds / 60.0

        # Sisa waktu menyusut pasti
        sisa_sekarang = max(0, int(round(sisa_waktu_awal - elapsed_minutes)))

        # Degradasi status mutu otomatis
        if sisa_sekarang > 90:
            grade_sekarang = "GRADE_A"
            status_kualitas = "Segar / Optimal"
        elif sisa_sekarang >= 30:
            grade_sekarang = "GRADE_B"
            status_kualitas = "Peringatan / Segera Distribusi"
        else:
            grade_sekarang = "GRADE_C"
            status_kualitas = "Kritis / Asam (Pecah)"

        return {
            "sisa_waktu_menit": sisa_sekarang,
            "grade_sekarang": grade_sekarang,
            "status_kualitas": status_kualitas,
            "menit_berlalu": int(elapsed_minutes),
            "detik_berlalu": int(elapsed_seconds)
        }

ml_service = MilkMLService()