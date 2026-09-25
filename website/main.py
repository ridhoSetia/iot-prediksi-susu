import os
import math
import json
import sqlite3
import datetime
from typing import List, Optional, Union
from fastapi import FastAPI, Request, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates
from fastapi.staticfiles import StaticFiles
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

# Import modul layanan inferensi ML & Time-Decay
try:
    from ml_service import ml_service
except Exception as e:
    print(f"[PERINGATAN] ml_service gagal diimpor: {e}")
    ml_service = None

# =================================================================
# 1. INISIALISASI DATABASE & KONSTANTA KUD
# =================================================================
DB_FILE = "milk_predictions.db"

KUD_LAT = -0.480810816247826
KUD_LON = 117.15546525804818
KUD_NAMA = "Pos KUD Penampungan Susu"


def hitung_jarak_dan_eta(lat1: float, lon1: float, lat2: float, lon2: float):
    R = 6371.0
    dlat = math.radians(lat2 - lat1)
    dlon = math.radians(lon2 - lon1)
    a = (
        math.sin(dlat / 2) ** 2
        + math.cos(math.radians(lat1))
        * math.cos(math.radians(lat2))
        * math.sin(dlon / 2) ** 2
    )
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
    jarak_km = round(R * c, 2)
    travel_min = max(3, round((jarak_km / 30.0) * 60) + 3)
    return jarak_km, travel_min


def init_db():
    """Membuat tabel prediksi dan memastikan semua kolom tersedia."""
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute('''
        CREATE TABLE IF NOT EXISTS predictions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT NOT NULL,
            alamat TEXT NOT NULL,
            latitude REAL NOT NULL,
            longitude REAL NOT NULL,
            jarak_km REAL NOT NULL DEFAULT 0.0,
            jarak_tempuh_menit INTEGER NOT NULL DEFAULT 5,
            suhu REAL NOT NULL,
            grade TEXT NOT NULL,
            sisa_waktu_menit INTEGER NOT NULL,
            metode_pengiriman TEXT NOT NULL DEFAULT 'DIJEMPUT_KUD',
            status TEXT NOT NULL DEFAULT 'PROSES',
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    for col, col_type, default_val in [
        ("jarak_km", "REAL", "0.0"),
        ("jarak_tempuh_menit", "INTEGER", "5"),
        ("status", "TEXT", "'PROSES'"),
    ]:
        try:
            c.execute(f"ALTER TABLE predictions ADD COLUMN {col} {col_type} NOT NULL DEFAULT {default_val}")
        except Exception:
            pass
    conn.commit()
    conn.close()


init_db()


# =================================================================
# 2. WEBSOCKET CONNECTION MANAGER
# =================================================================
class ConnectionManager:
    def __init__(self):
        self.active_connections: List[WebSocket] = []

    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        self.active_connections.append(websocket)

    def disconnect(self, websocket: WebSocket):
        if websocket in self.active_connections:
            self.active_connections.remove(websocket)

    async def broadcast(self, message: dict):
        for connection in list(self.active_connections):
            try:
                await connection.send_json(message)
            except Exception:
                self.disconnect(connection)


ws_manager = ConnectionManager()


# =================================================================
# 3. INISIALISASI FASTAPI & MIDDLEWARE
# =================================================================
app = FastAPI(
    title="Smart Milk Quality Logistics API",
    description="Backend monitoring dan re-evaluasi mutu susu segar berbasis Edge AI",
    version="1.0.0"
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

os.makedirs("static", exist_ok=True)
os.makedirs("templates", exist_ok=True)

app.mount("/static", StaticFiles(directory="static"), name="static")
templates = Jinja2Templates(directory="templates")


# =================================================================
# 4. PYDANTIC SCHEMAS (DATA VALIDATION)
# =================================================================
class PredictLogItem(BaseModel):
    device_id: str = Field(..., example="FARMMERRY-001")
    alamat: str = Field(default="Desa Sukamaju RT 02", example="Farm Mery, Mugirejo, Kec. Sungai Pinang")
    latitude: float = Field(..., example=-0.480724)
    longitude: float = Field(..., example=117.202154)
    suhu: float = Field(..., example=28.45)
    grade: str = Field(..., example="GRADE_A")
    sisa_waktu_menit: int = Field(..., example=175)
    metode_pengiriman: Optional[str] = Field(default="DIJEMPUT_KUD", example="DIJEMPUT_KUD")


class RePredictRequest(BaseModel):
    suhu: float = Field(..., description="Suhu susu aktual (°C)")
    r_liquid: float = Field(..., description="Resistansi probe cairan (Ohm)")
    ec_raw: float = Field(..., description="EC mentah (mS/cm)")
    ec25: float = Field(..., description="EC terkompensasi 25°C (mS/cm)")


# =================================================================
# 5. WEBPAGE ROUTES (HTML TEMPLATES)
# =================================================================
@app.get("/", response_class=HTMLResponse, summary="Halaman Utama Dasbor")
async def dashboard_page(request: Request, date: Optional[str] = None):
    """Merender antarmuka dashboard monitoring real-time dengan data hari ini."""
    sekarang = datetime.datetime.now()
    tgl_hari_ini = date or sekarang.strftime("%Y-%m-%d")
    jam_mulai = f"{tgl_hari_ini} 00:00:00"
    jam_selesai = f"{tgl_hari_ini} 23:59:59"

    conn = sqlite3.connect(DB_FILE)
    conn.row_factory = sqlite3.Row
    c = conn.cursor()
    c.execute(
        """
        SELECT * FROM predictions 
        WHERE created_at >= ? AND created_at <= ? 
        ORDER BY sisa_waktu_menit ASC
        """,
        (jam_mulai, jam_selesai),
    )
    rows = c.fetchall()
    logs = [dict(row) for row in rows]
    conn.close()

    return templates.TemplateResponse(
        request=request,
        name="index.html",
        context={
            "logs_json": json.dumps(logs),
            "kud_json": json.dumps({"lat": KUD_LAT, "lon": KUD_LON, "nama": KUD_NAMA}),
            "tgl_hari_ini": tgl_hari_ini,
            "active_menu": "monitoring",
        },
    )


@app.get("/history", response_class=HTMLResponse, summary="Halaman Riwayat Log")
async def history_page(request: Request, date_filter: Optional[str] = None):
    """Merender tabel rekaman pengujian histori dan chart statistik dari SQLite."""
    sekarang = datetime.datetime.now()
    tgl_hari_ini = sekarang.strftime("%Y-%m-%d")

    conn = sqlite3.connect(DB_FILE)
    conn.row_factory = sqlite3.Row
    c = conn.cursor()

    if date_filter:
        query = "SELECT * FROM predictions WHERE created_at LIKE ? ORDER BY id DESC"
        params = (f"{date_filter}%",)
    else:
        query = "SELECT * FROM predictions WHERE created_at < ? ORDER BY id DESC"
        params = (f"{tgl_hari_ini} 00:00:00",)

    c.execute(query, params)
    rows = c.fetchall()

    # Jika tidak ada data sebelum hari ini dan tanpa filter, muat seluruh data agar halaman tidak kosong
    if not rows and not date_filter:
        c.execute("SELECT * FROM predictions ORDER BY id DESC")
        rows = c.fetchall()

    history_logs = [dict(row) for row in rows]
    conn.close()

    return templates.TemplateResponse(
        request=request,
        name="history.html",
        context={
            "history_logs": history_logs,
            "date_filter": date_filter or "",
            "active_menu": "history",
        },
    )


# =================================================================
# 6. WEBSOCKET & REST API ENDPOINTS
# =================================================================
@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await ws_manager.connect(websocket)
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        ws_manager.disconnect(websocket)
    except Exception:
        ws_manager.disconnect(websocket)


@app.post("/api/predictions/{log_id}/complete", summary="Konfirmasi Penerimaan Susu")
async def mark_complete(log_id: int):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("UPDATE predictions SET status = 'SELESAI' WHERE id = ?", (log_id,))
    conn.commit()
    conn.close()
    await ws_manager.broadcast({"type": "STATUS_UPDATED", "id": log_id, "status": "SELESAI"})
    return {"status": "SUCCESS", "id": log_id}


@app.post("/api/predict-log", summary="Sinkronisasi Log dari ESP32-S3")
async def receive_prediction_log(data: Union[List[PredictLogItem], PredictLogItem, dict, List[dict]]):
    """Menerima rekaman log pengujian dari mikrokontroler ESP32-S3."""
    if isinstance(data, list):
        raw_items = data
    else:
        raw_items = [data]

    if not raw_items:
        raise HTTPException(status_code=400, detail="Data log kosong.")

    waktu_sekarang = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    broadcast_list = []

    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    try:
        for item in raw_items:
            d = item if isinstance(item, dict) else item.model_dump()
            dev_id = d.get("device_id", "UNKNOWN")
            alamat = d.get("alamat", "Desa Sukamaju RT 02")
            lat = float(d.get("latitude", -0.48))
            lon = float(d.get("longitude", 117.20))
            suhu = float(d.get("suhu", 7.0))
            grade = d.get("grade", "GRADE_A")
            sisa_menit = int(d.get("sisa_waktu_menit", 120))
            metode = d.get("metode_pengiriman") or "DIJEMPUT_KUD"

            jarak_km, travel_min = hitung_jarak_dan_eta(KUD_LAT, KUD_LON, lat, lon)

            c.execute(
                """
                INSERT INTO predictions (
                    device_id, alamat, latitude, longitude, jarak_km, jarak_tempuh_menit,
                    suhu, grade, sisa_waktu_menit, metode_pengiriman, status, created_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'PROSES', ?)
                """,
                (
                    dev_id, alamat, lat, lon, jarak_km, travel_min,
                    suhu, grade, sisa_menit, metode, waktu_sekarang
                ),
            )
            rec_id = c.lastrowid
            broadcast_list.append({
                "id": rec_id,
                "device_id": dev_id,
                "alamat": alamat,
                "latitude": lat,
                "longitude": lon,
                "jarak_km": jarak_km,
                "jarak_tempuh_menit": travel_min,
                "suhu": suhu,
                "grade": grade,
                "sisa_waktu_menit": sisa_menit,
                "metode_pengiriman": metode,
                "status": "PROSES",
                "created_at": waktu_sekarang,
            })
        conn.commit()
    except Exception as e:
        conn.rollback()
        raise HTTPException(status_code=500, detail=f"Gagal menyimpan ke database: {str(e)}")
    finally:
        conn.close()

    for record in broadcast_list:
        await ws_manager.broadcast({"type": "NEW_TELEMETRY", "data": record})

    return {
        "status": "SUCCESS",
        "message": f"Berhasil menyimpan {len(broadcast_list)} rekaman log.",
        "inserted": len(broadcast_list),
        "items": broadcast_list
    }


@app.get("/api/dashboard-data", summary="Polling Pemantauan Mutu Dinamis")
async def get_dashboard_data():
    """Menyediakan metrik agregat dan data batch yang status mutunya dihitung ulang secara dinamis."""
    conn = sqlite3.connect(DB_FILE)
    conn.row_factory = sqlite3.Row
    c = conn.cursor()
    c.execute('SELECT * FROM predictions ORDER BY id DESC')
    rows = c.fetchall()
    conn.close()

    count_grade_a = 0
    count_grade_b = 0
    count_grade_c = 0
    batch_list = []

    for r in rows:
        sisa_awal = r["sisa_waktu_menit"]
        grade_awal = r["grade"]
        created_str = r["created_at"] or ""

        if ml_service:
            try:
                decay = ml_service.calculate_current_shelf_life(
                    created_at_str=created_str,
                    sisa_waktu_awal=sisa_awal
                )
                curr_grade = decay["grade_sekarang"]
                curr_sisa = decay["sisa_waktu_menit"]
                status_kual = decay["status_kualitas"]
                menit_lalu = decay["menit_berlalu"]
            except Exception:
                curr_grade = grade_awal
                curr_sisa = sisa_awal
                status_kual = "Normal"
                menit_lalu = 0
        else:
            curr_grade = grade_awal
            curr_sisa = sisa_awal
            status_kual = "Normal"
            menit_lalu = 0

        if "A" in curr_grade.upper():
            count_grade_a += 1
        elif "B" in curr_grade.upper():
            count_grade_b += 1
        else:
            count_grade_c += 1

        batch_list.append({
            "id": r["id"],
            "device_id": r["device_id"],
            "alamat": r["alamat"],
            "latitude": r["latitude"],
            "longitude": r["longitude"],
            "suhu": r["suhu"],
            "grade_awal": grade_awal,
            "sisa_waktu_awal": sisa_awal,
            "grade_sekarang": curr_grade,
            "sisa_waktu_sekarang": curr_sisa,
            "status_kualitas": status_kual,
            "menit_berlalu": menit_lalu,
            "metode_pengiriman": r["metode_pengiriman"],
            "status": r["status"] if "status" in r.keys() else "PROSES",
            "created_at": created_str
        })

    return {
        "summary": {
            "total_batch": len(rows),
            "grade_a": count_grade_a,
            "grade_b": count_grade_b,
            "grade_c": count_grade_c
        },
        "batches": batch_list
    }


@app.post("/api/re-predict", summary="Uji Inferensi Model ML di Backend")
async def api_re_predict(req: RePredictRequest):
    """Menghitung ulang grade dan sisa waktu simpan langsung via NumPy ML service."""
    if not ml_service:
        raise HTTPException(status_code=503, detail="Modul ml_service tidak tersedia.")

    hasil = ml_service.predict(
        suhu=req.suhu,
        r_liquid=req.r_liquid,
        ec_raw=req.ec_raw,
        ec25=req.ec25
    )

    if "error" in hasil:
        raise HTTPException(status_code=500, detail=hasil["error"])

    return {
        "status": "success",
        "result": hasil
    }


@app.delete("/api/history/{item_id}", summary="Hapus Baris Log Tertentu")
async def delete_history_item(item_id: int):
    """Menghapus data rekaman log pengujian berdasarkan ID."""
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute('DELETE FROM predictions WHERE id = ?', (item_id,))
    deleted = c.rowcount
    conn.commit()
    conn.close()

    if deleted == 0:
        raise HTTPException(status_code=404, detail=f"Log dengan ID #{item_id} tidak ditemukan.")

    return {"status": "success", "deleted_id": item_id}


@app.delete("/api/history", summary="Hapus Semua Riwayat Log")
async def clear_all_history():
    """Mengosongkan seluruh isi tabel riwayat pengujian."""
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute('DELETE FROM predictions')
    conn.commit()
    conn.close()
    return {"status": "success", "message": "Semua riwayat log berhasil dibersihkan."}


# =================================================================
# 7. RUNNER
# =================================================================
if __name__ == "__main__":
    import uvicorn
    uvicorn.run(
        "main:app",
        host="0.0.0.0",
        port=7000,
        reload=True,
        reload_excludes=["*.db", "*.db-journal", "*.sqlite", "*.sqlite3"]
    )