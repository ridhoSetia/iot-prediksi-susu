import math
import json
from datetime import datetime
from typing import List, Optional, Union
from contextlib import asynccontextmanager

import aiosqlite
from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from pydantic import BaseModel, Field
import uvicorn

DB_NAME = "milk_predictions.db"

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


async def init_db():
    async with aiosqlite.connect(DB_NAME) as db:
        await db.execute(
            """
            CREATE TABLE IF NOT EXISTS predictions (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                alamat TEXT NOT NULL,
                latitude REAL NOT NULL,
                longitude REAL NOT NULL,
                jarak_km REAL NOT NULL,
                jarak_tempuh_menit INTEGER NOT NULL,
                suhu REAL NOT NULL,
                grade TEXT NOT NULL,
                sisa_waktu_menit INTEGER NOT NULL,
                metode_pengiriman TEXT NOT NULL,
                status TEXT NOT NULL DEFAULT 'PROSES',
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        """
        )
        # Menambahkan kolom status secara otomatis jika tabel lama belum memilikinya
        try:
            await db.execute("ALTER TABLE predictions ADD COLUMN status TEXT NOT NULL DEFAULT 'PROSES'")
        except Exception:
            pass
        await db.commit()


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
        for connection in self.active_connections:
            try:
                await connection.send_json(message)
            except Exception:
                pass


ws_manager = ConnectionManager()


@asynccontextmanager
async def lifespan(app: FastAPI):
    await init_db()
    yield


app = FastAPI(title="Sistem Monitoring Logistik Susu", lifespan=lifespan)
app.mount("/static", StaticFiles(directory="static"), name="static")
templates = Jinja2Templates(directory="templates")


class PredictionItem(BaseModel):
    device_id: str
    alamat: str = Field(default="Desa Sukamaju RT 02")
    timestamp: Optional[int] = None
    latitude: float
    longitude: float
    suhu: float
    grade: str
    sisa_waktu_menit: int
    metode_pengiriman: Optional[str] = "DIJEMPUT_KUD"


@app.post("/api/predict-log")
async def receive_predict_log(data: Union[List[PredictionItem], PredictionItem]):
    items = data if isinstance(data, list) else [data]
    waktu_sekarang = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    broadcast_list = []

    async with aiosqlite.connect(DB_NAME) as db:
        for item in items:
            metode = item.metode_pengiriman or "DIJEMPUT_KUD"
            jarak_km, travel_min = hitung_jarak_dan_eta(
                KUD_LAT, KUD_LON, item.latitude, item.longitude
            )

            cursor = await db.execute(
                """
                INSERT INTO predictions (
                    device_id, alamat, latitude, longitude, jarak_km, jarak_tempuh_menit,
                    suhu, grade, sisa_waktu_menit, metode_pengiriman, status, created_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'PROSES', ?)
            """,
                (
                    item.device_id,
                    item.alamat,
                    item.latitude,
                    item.longitude,
                    jarak_km,
                    travel_min,
                    item.suhu,
                    item.grade,
                    item.sisa_waktu_menit,
                    metode,
                    waktu_sekarang,
                ),
            )

            record_id = cursor.lastrowid
            broadcast_list.append(
                {
                    "id": record_id,
                    "device_id": item.device_id,
                    "alamat": item.alamat,
                    "latitude": item.latitude,
                    "longitude": item.longitude,
                    "jarak_km": jarak_km,
                    "jarak_tempuh_menit": travel_min,
                    "suhu": item.suhu,
                    "grade": item.grade,
                    "sisa_waktu_menit": item.sisa_waktu_menit,
                    "metode_pengiriman": metode,
                    "status": "PROSES",
                    "created_at": waktu_sekarang,
                }
            )
        await db.commit()

    for record in broadcast_list:
        await ws_manager.broadcast({"type": "NEW_TELEMETRY", "data": record})

    return {"status": "SUCCESS", "saved": len(broadcast_list)}


@app.post("/api/predictions/{log_id}/complete")
async def mark_complete(log_id: int):
    async with aiosqlite.connect(DB_NAME) as db:
        await db.execute("UPDATE predictions SET status = 'SELESAI' WHERE id = ?", (log_id,))
        await db.commit()
    await ws_manager.broadcast({"type": "STATUS_UPDATED", "id": log_id, "status": "SELESAI"})
    return {"status": "SUCCESS", "id": log_id}


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await ws_manager.connect(websocket)
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        ws_manager.disconnect(websocket)


@app.get("/", response_class=HTMLResponse, include_in_schema=False)
async def view_dashboard(request: Request, date: Optional[str] = None):
    sekarang = datetime.now()
    tgl_hari_ini = date or sekarang.strftime("%Y-%m-%d")
    jam_mulai = f"{tgl_hari_ini} 00:00:00"
    jam_selesai = f"{tgl_hari_ini} 23:59:59"

    async with aiosqlite.connect(DB_NAME) as db:
        db.row_factory = aiosqlite.Row
        async with db.execute(
            """
            SELECT * FROM predictions 
            WHERE created_at >= ? AND created_at <= ? 
            ORDER BY sisa_waktu_menit ASC
        """,
            (jam_mulai, jam_selesai),
        ) as cursor:
            rows = await cursor.fetchall()
            logs = [dict(row) for row in rows]

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


@app.get("/history", response_class=HTMLResponse, include_in_schema=False)
async def view_history(request: Request, date_filter: Optional[str] = None):
    sekarang = datetime.now()
    tgl_hari_ini = sekarang.strftime("%Y-%m-%d")

    async with aiosqlite.connect(DB_NAME) as db:
        db.row_factory = aiosqlite.Row
        if date_filter:
            query = "SELECT * FROM predictions WHERE created_at LIKE ? ORDER BY id DESC"
            params = (f"{date_filter}%",)
        else:
            query = "SELECT * FROM predictions WHERE created_at < ? ORDER BY id DESC"
            params = (f"{tgl_hari_ini} 00:00:00",)

        async with db.execute(query, params) as cursor:
            rows = await cursor.fetchall()
            history_logs = [dict(row) for row in rows]

    return templates.TemplateResponse(
        request=request,
        name="history.html",
        context={
            "history_logs": history_logs,
            "date_filter": date_filter or "",
            "active_menu": "history",
        },
    )


if __name__ == "__main__":
    uvicorn.run(
        "main:app",
        host="0.0.0.0",
        port=7000,
        reload=True,
        reload_excludes=["*.db", "*.db-journal", "*.sqlite", "*.sqlite3"],
    )