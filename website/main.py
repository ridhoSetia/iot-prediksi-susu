from contextlib import asynccontextmanager
from datetime import datetime
import json
import math
from typing import List, Optional, Union
import aiosqlite
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
from pydantic import BaseModel, Field
import uvicorn

DB_NAME = "milk_predictions.db"

# Koordinat Pos KUD Pusat
KUD_LAT = -0.480810816247826
KUD_LON = 117.15546525804818
KUD_NAMA = "Pos Pusat KUD Penampungan Susu"


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
    await db.execute("""
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
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        """)
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


app = FastAPI(title="Milk Quality Real-Time Logistics", lifespan=lifespan)


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
                    suhu, grade, sisa_waktu_menit, metode_pengiriman, created_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
      broadcast_list.append({
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
          "created_at": waktu_sekarang,
      })
    await db.commit()

  for record in broadcast_list:
    await ws_manager.broadcast({"type": "NEW_TELEMETRY", "data": record})

  return {"status": "SUCCESS", "saved": len(broadcast_list)}


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
  await ws_manager.connect(websocket)
  try:
    while True:
      await websocket.receive_text()
  except WebSocketDisconnect:
    ws_manager.disconnect(websocket)


@app.get("/", response_class=HTMLResponse, include_in_schema=False)
async def view_dashboard():
  # Filter rentang waktu: Hari ini pukul 00:00:00 sampai 23:59:59
  sekarang = datetime.now()
  tgl_hari_ini = sekarang.strftime("%Y-%m-%d")
  jam_mulai = f"{tgl_hari_ini} 00:00:00"
  jam_selesai = f"{tgl_hari_ini} 23:59:59"

  async with aiosqlite.connect(DB_NAME) as db:
    db.row_factory = aiosqlite.Row
    async with db.execute(
        """
            SELECT * FROM predictions 
            WHERE created_at >= ? AND created_at <= ? 
            ORDER BY id DESC
        """,
        (jam_mulai, jam_selesai),
    ) as cursor:
      rows = await cursor.fetchall()
      logs = [dict(row) for row in rows]

  logs_json = json.dumps(logs)
  kud_json = json.dumps({"lat": KUD_LAT, "lon": KUD_LON, "nama": KUD_NAMA})

  return HTMLResponse(content=f"""
    <!DOCTYPE html>
    <html lang="id">
    <head>
        <meta charset="UTF-8">
        <title>Pusat Navigasi Satelit & Logistik Susu</title>
        <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" />
        <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;600;700&display=swap" rel="stylesheet">
        <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
        
        <style>
            * {{ box-sizing: border-box; margin: 0; padding: 0; }}
            body {{ 
                font-family: 'Inter', sans-serif; 
                background: #0b0f19; 
                color: #f8fafc; 
                padding: 16px;
                display: flex;
                justify-content: center;
            }}

            .container {{ 
                width: 100%; 
                max-width: 1160px; 
                display: flex; 
                flex-direction: column; 
                gap: 14px; 
            }}

            .header {{ 
                display: flex; 
                justify-content: space-between; 
                align-items: center; 
                background: #151c2c; 
                padding: 12px 18px; 
                border-radius: 10px; 
                border: 1px solid #283548; 
            }}
            .header-left {{ display: flex; align-items: center; gap: 10px; }}
            .header h1 {{ font-size: 16px; font-weight: 700; color: #38bdf8; }}
            .date-filter-badge {{
                font-size: 11px;
                background: #1e293b;
                color: #94a3b8;
                padding: 3px 8px;
                border-radius: 6px;
                border: 1px solid #334155;
            }}
            .status-tag {{ 
                display: flex; 
                align-items: center; 
                gap: 6px; 
                font-size: 12px; 
                font-weight: 600; 
                color: #4ade80; 
                background: rgba(74, 222, 128, 0.1); 
                padding: 4px 10px; 
                border-radius: 20px; 
                border: 1px solid #22c55e; 
            }}
            .dot {{ width: 7px; height: 7px; background: #4ade80; border-radius: 50%; animation: pulse 1.2s infinite; }}

            .grid-layout {{
                display: grid;
                grid-template-columns: 1.15fr 0.85fr;
                gap: 14px;
                align-items: stretch;
            }}

            .table-panel {{ 
                background: #151c2c; 
                border-radius: 10px; 
                border: 1px solid #283548; 
                display: flex;
                flex-direction: column;
                height: 440px;
                overflow: hidden;
            }}
            .panel-header {{
                padding: 10px 14px;
                font-size: 13px;
                font-weight: 700;
                color: #94a3b8;
                border-bottom: 1px solid #283548;
                background: #111726;
                display: flex;
                justify-content: space-between;
                align-items: center;
            }}
            .table-scroll-area {{
                overflow-x: auto;
                overflow-y: auto;
                flex: 1;
            }}
            table {{ width: 100%; border-collapse: collapse; text-align: left; font-size: 12px; }}
            th {{ 
                background: #0d1320; 
                color: #94a3b8; 
                padding: 8px 10px; 
                font-weight: 600; 
                border-bottom: 1px solid #283548; 
                position: sticky;
                top: 0;
                z-index: 10;
            }}
            td {{ padding: 8px 10px; border-bottom: 1px solid #1e293b; vertical-align: middle; }}

            .map-panel {{ 
                background: #151c2c; 
                border-radius: 10px; 
                border: 1px solid #283548; 
                padding: 8px; 
                position: relative; 
                height: 440px;
                display: flex;
                flex-direction: column;
            }}
            #map {{ height: 100%; width: 100%; border-radius: 6px; }}

            .route-info-box {{
                position: absolute; 
                bottom: 16px; 
                left: 16px; 
                right: 16px;
                z-index: 1000;
                background: rgba(15, 23, 42, 0.9); 
                backdrop-filter: blur(6px);
                border: 1px solid #38bdf8; 
                padding: 8px 12px; 
                border-radius: 6px;
                display: none; 
                flex-direction: column; 
                gap: 2px; 
                font-size: 11.5px;
            }}

            .chart-grid {{
                display: grid;
                grid-template-columns: 0.7fr 1.3fr;
                gap: 14px;
            }}
            .chart-card {{
                background: #151c2c;
                border: 1px solid #283548;
                border-radius: 10px;
                padding: 14px 16px;
                display: flex;
                flex-direction: column;
                height: 250px;
            }}
            .chart-title {{
                font-size: 12.5px;
                font-weight: 700;
                color: #94a3b8;
                margin-bottom: 10px;
                display: flex;
                align-items: center;
                gap: 6px;
            }}
            .chart-wrapper {{
                position: relative;
                flex: 1;
                min-height: 0;
            }}

            .badge {{ padding: 2px 6px; border-radius: 4px; font-weight: 600; font-size: 10.5px; display: inline-block; }}
            .badge-jemput {{ background: #0369a1; color: #e0f2fe; }}
            .badge-antar {{ background: #334155; color: #cbd5e1; }}

            .btn-action-group {{ display: flex; gap: 5px; align-items: center; }}
            .btn-nav {{
                background: #0284c7; color: white; border: none; padding: 4px 7px;
                border-radius: 4px; cursor: pointer; font-weight: 600; font-size: 11px;
            }}
            .btn-nav:hover {{ background: #0369a1; }}
            .btn-gmaps {{
                background: #059669; color: white; text-decoration: none; padding: 4px 7px;
                border-radius: 4px; font-weight: 600; font-size: 11px;
            }}
            .btn-gmaps:hover {{ background: #047857; }}
            .table-link-maps {{ color: #38bdf8; text-decoration: none; font-weight: 600; }}
            .table-link-maps:hover {{ text-decoration: underline; color: #7dd3fc; }}

            .flash-row {{ animation: highlight 2s ease-out; }}
            @keyframes highlight {{ 0% {{ background-color: #0284c7; }} 100% {{ background-color: transparent; }} }}
            @keyframes pulse {{ 0%, 100% {{ opacity: 1; }} 50% {{ opacity: 0.3; }} }}

            @media (max-width: 900px) {{
                .grid-layout, .chart-grid {{ grid-template-columns: 1fr; }}
                .table-panel, .map-panel {{ height: 380px; }}
            }}
        </style>
    </head>
    <body>
        <div class="container">
            <div class="header">
                <div class="header-left">
                    <h1>🛰️ Pusat Navigasi Satelit & Logistik Susu</h1>
                    <span class="date-filter-badge">📅 Hari Ini: {tgl_hari_ini} (00:00 - 24:00)</span>
                </div>
                <div class="status-tag">
                    <div class="dot"></div>
                    WEBSOCKET AKTIF
                </div>
            </div>

            <div class="grid-layout">
                <div class="table-panel">
                    <div class="panel-header">
                        <span>📋 Log Prediksi Mutu Peternak</span>
                        <span id="logCounter" style="font-size:11px; color:#38bdf8;">0 Data Hari Ini</span>
                    </div>
                    <div class="table-scroll-area">
                        <table>
                            <thead>
                                <tr>
                                    <th>ID</th>
                                    <th>Peternak</th>
                                    <th>Alamat & Koordinat</th>
                                    <th>Suhu</th>
                                    <th>Sisa Waktu (Grade)</th>
                                    <th>Distribusi</th>
                                    <th>Waktu</th>
                                    <th>Aksi</th>
                                </tr>
                            </thead>
                            <tbody id="tableBody"></tbody>
                        </table>
                    </div>
                </div>

                <div class="map-panel">
                    <div id="map"></div>
                    <div class="route-info-box" id="routeInfoBox">
                        <b style="color:#38bdf8;">🛣️ Rute Navigasi Aktif</b>
                        <span id="routeDist">Jarak: -</span>
                        <span id="routeTime">Estimasi Armada: -</span>
                    </div>
                </div>
            </div>

            <div class="chart-grid">
                <div class="chart-card">
                    <div class="chart-title">🍩 Komposisi Mutu Susu Hari Ini</div>
                    <div class="chart-wrapper">
                        <canvas id="gradeDoughnutChart"></canvas>
                    </div>
                </div>

                <div class="chart-card">
                    <div class="chart-title">⚡ Matriks Urgensi: Sisa Waktu Susu vs Waktu Tempuh Armada</div>
                    <div class="chart-wrapper">
                        <canvas id="urgencyBarChart"></canvas>
                    </div>
                </div>
            </div>
        </div>

        <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
        <script>
            let dbData = {logs_json};
            const kudInfo = {kud_json};
            const currentFilterDate = "{tgl_hari_ini}";

            const map = L.map('map').setView([kudInfo.lat, kudInfo.lon], 13);
            L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{{z}}/{{y}}/{{x}}', {{ attribution: 'Tiles &copy; Esri' }}).addTo(map);
            L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Boundaries_and_Places/MapServer/tile/{{z}}/{{y}}/{{x}}', {{ attribution: 'Labels &copy; Esri' }}).addTo(map);

            const kudIcon = L.icon({{
                iconUrl: 'https://raw.githubusercontent.com/pointhi/leaflet-color-markers/master/img/marker-icon-2x-blue.png',
                iconSize: [20, 32], iconAnchor: [10, 32]
            }});
            L.marker([kudInfo.lat, kudInfo.lon], {{icon: kudIcon}}).addTo(map)
                .bindPopup(`<b>${{kudInfo.nama}}</b><br>Basis Armada Pusat<br><a href="https://www.google.com/maps?q=${{kudInfo.lat}},${{kudInfo.lon}}" target="_blank" style="color:#0284c7; font-weight:600; text-decoration:underline;">📍 Buka Google Maps ↗</a>`).openPopup();

            const markers = {{}};
            let activeRouteLayer = null;

            async function navigasiKePeternak(lat, lon, deviceId, sisaWaktuGrade) {{
                map.flyTo([lat, lon], 14);
                if (markers[deviceId]) markers[deviceId].openPopup();

                const infoBox = document.getElementById('routeInfoBox');
                infoBox.style.display = 'flex';
                document.getElementById('routeDist').innerText = "Menghitung rute jalan...";
                document.getElementById('routeTime').innerText = "";

                try {{
                    const osrmUrl = `https://router.project-osrm.org/route/v1/driving/${{kudInfo.lon}},${{kudInfo.lat}};${{lon}},${{lat}}?overview=full&geometries=geojson`;
                    const res = await fetch(osrmUrl);
                    const data = await res.json();

                    if (data.routes && data.routes.length > 0) {{
                        if (activeRouteLayer) map.removeLayer(activeRouteLayer);
                        const routeGeoJSON = data.routes[0].geometry;
                        const distKm = (data.routes[0].distance / 1000).toFixed(2);
                        const durasiMenit = Math.round(data.routes[0].duration / 60);

                        activeRouteLayer = L.geoJSON(routeGeoJSON, {{
                            style: {{ color: '#00ffff', weight: 4, opacity: 0.95 }}
                        }}).addTo(map);

                        map.fitBounds(activeRouteLayer.getBounds(), {{ padding: [40, 40] }});
                        document.getElementById('routeDist').innerText = `Jarak Tempuh: ${{distKm}} km`;
                        document.getElementById('routeTime').innerText = `Estimasi Armada: ${{durasiMenit}} Menit | Target: ${{sisaWaktuGrade}}`;
                    }}
                }} catch (e) {{
                    if (activeRouteLayer) map.removeLayer(activeRouteLayer);
                    activeRouteLayer = L.polyline([[kudInfo.lat, kudInfo.lon], [lat, lon]], {{ color: '#f59e0b', weight: 3, dashArray: '6, 8' }}).addTo(map);
                    map.fitBounds(activeRouteLayer.getBounds(), {{ padding: [30, 30] }});
                }}
            }}

            function pasangMarker(item) {{
                const isWarning = item.grade.includes('B') || item.grade.includes('C') || item.sisa_waktu_menit <= 45;
                const warna = isWarning ? 'red' : 'green';
                const icon = L.icon({{
                    iconUrl: `https://raw.githubusercontent.com/pointhi/leaflet-color-markers/master/img/marker-icon-2x-${{warna}}.png`,
                    iconSize: [20, 32], iconAnchor: [10, 32]
                }});
                const mapsUrl = `https://www.google.com/maps?q=${{item.latitude}},${{item.longitude}}`;
                const popup = `
                    <div style="color:#0f172a; font-family:'Inter', sans-serif; font-size:11.5px; line-height: 1.4;">
                        <b style="font-size:12.5px; color:#0284c7;">${{item.device_id}}</b><br>
                        <b>Alamat:</b> ${{item.alamat}}<br>
                        <b>Suhu:</b> ${{item.suhu.toFixed(2)}} °C<br>
                        <b>Sisa Waktu:</b> ${{item.sisa_waktu_menit}} Menit (${{item.grade}})<br>
                        <b>Distribusi:</b> ${{item.metode_pengiriman}}<br>
                        <div style="margin-top: 6px; padding-top: 4px; border-top: 1px solid #e2e8f0;">
                            <a href="${{mapsUrl}}" target="_blank" style="color: #0284c7; font-weight: 700; text-decoration: underline;">📍 Buka di Google Maps ↗</a>
                        </div>
                    </div>
                `;
                if (markers[item.device_id]) {{
                    markers[item.device_id].setLatLng([item.latitude, item.longitude]).setPopupContent(popup);
                }} else {{
                    markers[item.device_id] = L.marker([item.latitude, item.longitude], {{icon: icon}}).addTo(map).bindPopup(popup);
                }}
            }}

            function tambahBarisTabel(item, isNew = false) {{
                const tbody = document.getElementById('tableBody');
                const tr = document.createElement('tr');
                if (isNew) tr.className = 'flash-row';

                const distBadge = item.metode_pengiriman.includes('JEMPUT')
                    ? '<span class="badge badge-jemput">JEMPUT</span>'
                    : '<span class="badge badge-antar">ANTAR</span>';
                const sisaWaktuGradeText = `${{item.sisa_waktu_menit}}m (${{item.grade}})`;
                const mapsUrl = `https://www.google.com/maps?q=${{item.latitude}},${{item.longitude}}`;

                tr.innerHTML = `
                    <td>#${{item.id}}</td>
                    <td style="font-weight:600; color:#38bdf8;">${{item.device_id}}</td>
                    <td style="max-width: 130px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;">
                        <a href="${{mapsUrl}}" target="_blank" class="table-link-maps" title="${{item.alamat}}">📍 ${{item.alamat}}</a>
                    </td>
                    <td>${{item.suhu.toFixed(1)}}°C</td>
                    <td style="font-weight:600; color:#f87171;">${{sisaWaktuGradeText}}</td>
                    <td>${{distBadge}}</td>
                    <td style="color:#94a3b8; font-size:11px;">${{item.created_at.split(' ')[1] || item.created_at}}</td>
                    <td>
                        <div class="btn-action-group">
                            <button class="btn-nav" onclick="navigasiKePeternak(${{item.latitude}}, ${{item.longitude}}, '${{item.device_id}}', '${{sisaWaktuGradeText}}')">Rute</button>
                        </div>
                    </td>
                `;
                if (isNew) tbody.insertBefore(tr, tbody.firstChild);
                else tbody.appendChild(tr);

                document.getElementById('logCounter').innerText = `${{tbody.children.length}} Data Hari Ini`;
            }}

            function hitungGradeStats(data) {{
                let countA = 0, countB = 0, countC = 0;
                data.forEach(d => {{
                    const gr = String(d.grade).toUpperCase();
                    if (gr.includes('A')) countA++;
                    else if (gr.includes('B')) countB++;
                    else countC++;
                }});
                return [countA, countB, countC];
            }}

            const ctxDoughnut = document.getElementById('gradeDoughnutChart').getContext('2d');
            const gradeChart = new Chart(ctxDoughnut, {{
                type: 'doughnut',
                data: {{
                    labels: ['Grade A', 'Grade B', 'Grade C / Kering'],
                    datasets: [{{
                        data: hitungGradeStats(dbData),
                        backgroundColor: ['#10b981', '#f59e0b', '#ef4444'],
                        borderWidth: 0,
                        hoverOffset: 4
                    }}]
                }},
                options: {{
                    responsive: true,
                    maintainAspectRatio: false,
                    plugins: {{
                        legend: {{
                            position: 'bottom',
                            labels: {{ color: '#cbd5e1', font: {{ size: 11 }} }}
                        }}
                    }},
                    cutout: '68%'
                }}
            }});

            function getUrgencyData(data) {{
                const top5 = data.slice(0, 5).reverse();
                return {{
                    labels: top5.map(d => `#${{d.id}} ${{d.device_id}}`),
                    shelfLife: top5.map(d => d.sisa_waktu_menit),
                    eta: top5.map(d => d.jarak_tempuh_menit || Math.max(3, Math.round((d.jarak_km / 30.0) * 60) + 3))
                }};
            }}

            const initUrgency = getUrgencyData(dbData);
            const ctxBar = document.getElementById('urgencyBarChart').getContext('2d');
            const urgencyChart = new Chart(ctxBar, {{
                type: 'bar',
                data: {{
                    labels: initUrgency.labels,
                    datasets: [
                        {{
                            label: 'Sisa Masa Simpan (Menit)',
                            data: initUrgency.shelfLife,
                            backgroundColor: '#ef4444',
                            borderRadius: 4
                        }},
                        {{
                            label: 'Waktu Tempuh Truk (Menit)',
                            data: initUrgency.eta,
                            backgroundColor: '#0284c7',
                            borderRadius: 4
                        }}
                    ]
                }},
                options: {{
                    indexAxis: 'y',
                    responsive: true,
                    maintainAspectRatio: false,
                    scales: {{
                        x: {{
                            grid: {{ color: 'rgba(255, 255, 255, 0.05)' }},
                            ticks: {{ color: '#94a3b8', font: {{ size: 10 }} }}
                        }},
                        y: {{
                            grid: {{ display: false }},
                            ticks: {{ color: '#f8fafc', font: {{ size: 11, weight: '600' }} }}
                        }}
                    }},
                    plugins: {{
                        legend: {{
                            position: 'top',
                            labels: {{ color: '#cbd5e1', font: {{ size: 11 }} }}
                        }}
                    }}
                }}
            }});

            function refreshCharts() {{
                gradeChart.data.datasets[0].data = hitungGradeStats(dbData);
                gradeChart.update();

                const urg = getUrgencyData(dbData);
                urgencyChart.data.labels = urg.labels;
                urgencyChart.data.datasets[0].data = urg.shelfLife;
                urgencyChart.data.datasets[1].data = urg.eta;
                urgencyChart.update();
            }}

            if (dbData.length > 0) {{
                dbData.forEach(row => {{
                    tambahBarisTabel(row, false);
                    pasangMarker(row);
                }});
                const newest = dbData[0];
                navigasiKePeternak(newest.latitude, newest.longitude, newest.device_id, `${{newest.sisa_waktu_menit}}m (${{newest.grade}})`);
            }}

            const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
            const ws = new WebSocket(`${{protocol}}//${{window.location.host}}/ws`);

            ws.onmessage = function(event) {{
                const msg = JSON.parse(event.data);
                if (msg.type === 'NEW_TELEMETRY') {{
                    const dataBaru = msg.data;
                    
                    // Pastikan data baru dicatat di hari yang sama dengan filter aktif
                    if (dataBaru.created_at.startsWith(currentFilterDate)) {{
                        dbData.unshift(dataBaru);
                        tambahBarisTabel(dataBaru, true);
                        pasangMarker(dataBaru);
                        navigasiKePeternak(dataBaru.latitude, dataBaru.longitude, dataBaru.device_id, `${{dataBaru.sisa_waktu_menit}}m (${{dataBaru.grade}})`);
                        refreshCharts();
                    }}
                }}
            }};

            ws.onclose = function() {{
                setTimeout(() => location.reload(), 3000);
            }};
        </script>
    </body>
    </html>
    """)


if __name__ == "__main__":
  uvicorn.run(app, host="0.0.0.0", port=7000)