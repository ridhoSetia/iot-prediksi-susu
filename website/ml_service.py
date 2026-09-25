import os
import datetime
import numpy as np

# Konstanta Normalisasi Min-Max
MIN_VALS = np.array([3.560000, 253.300003, 1.598000, 2.565000], dtype=np.float32)
SCALE_VALS = np.array([0.031867, 0.003509, 0.226655, 0.359583], dtype=np.float32)
SHELF_MAX_MINUTES = 360.0

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
            print(f"[PERINGATAN] File bobot '{weights_path}' belum ditemukan. Harap ekspor dari notebook!")

    def predict(self, suhu: float, r_liquid: float, ec_raw: float, ec25: float):
        """Menjalankan inferensi forward-pass FP32 murni via NumPy."""
        if not self.weights_loaded:
            return {"error": "Bobot model belum dimuat"}

        # 1. Normalisasi Min-Max
        raw_x = np.array([suhu, r_liquid, ec_raw, ec25], dtype=np.float32)
        x = (raw_x - MIN_VALS) * SCALE_VALS

        # 2. Forward Pass Backbone (ReLU)
        h1 = np.maximum(0.0, np.dot(x, self.w1) + self.b1)
        h2 = np.maximum(0.0, np.dot(h1, self.w2) + self.b2)

        # 3. Head Klasifikasi Grade (Argmax)
        logits_grade = np.dot(h2, self.w_grade) + self.b_grade
        grade_idx = int(np.argmax(logits_grade))
        grades = ["GRADE_A", "GRADE_B", "GRADE_C"]
        pred_grade = grades[grade_idx]

        # 4. Head Regresi Sisa Waktu
        reg_shelf = np.dot(h2, self.w_shelf) + self.b_shelf
        shelf_norm = max(0.0, float(reg_shelf[0]))
        pred_shelf_min = int(round(np.clip(shelf_norm * SHELF_MAX_MINUTES, 0, SHELF_MAX_MINUTES)))

        return {
            "grade": pred_grade,
            "sisa_waktu_menit": pred_shelf_min
        }

    @staticmethod
    def calculate_current_shelf_life(created_at_str: str, sisa_waktu_awal: int):
        """Menghitung peluruhan waktu simpan dinamis (Time-Decay)."""
        try:
            # Parsing format timestamp ISO / SQLite
            if "T" in created_at_str:
                created_at = datetime.datetime.fromisoformat(created_at_str.replace("Z", "+00:00"))
            else:
                created_at = datetime.datetime.strptime(created_at_str, "%Y-%m-%d %H:%M:%S")
                created_at = created_at.replace(tzinfo=datetime.timezone.utc)
        except Exception:
            created_at = datetime.datetime.now(datetime.timezone.utc)

        now = datetime.datetime.now(datetime.timezone.utc)
        elapsed_minutes = (now - created_at).total_seconds() / 60.0
        sisa_sekarang = max(0, int(round(sisa_waktu_awal - elapsed_minutes)))

        # Degradasi Grade Otomatis mengikuti sisa menit simpan
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
            "menit_berlalu": int(elapsed_minutes)
        }

ml_service = MilkMLService()