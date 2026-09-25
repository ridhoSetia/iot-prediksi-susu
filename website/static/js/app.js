// 1. Konfigurasi Tile Peta (Esri Satelit & Standar Jalan OSM)
const osmStandard = L.tileLayer(
  "https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
  {
    attribution: "&copy; OpenStreetMap contributors",
    maxZoom: 19,
  },
);

const satelitEsri = L.layerGroup([
  L.tileLayer(
    "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
    {
      attribution: "Tiles &copy; Esri",
      maxZoom: 19,
    },
  ),
  L.tileLayer(
    "https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Boundaries_and_Places/MapServer/tile/{z}/{y}/{x}",
    {
      attribution: "Labels &copy; Esri",
      maxZoom: 19,
    },
  ),
]);

const map = L.map("map", {
  zoomControl: false,
  layers: [satelitEsri],
}).setView([kudInfo.lat, kudInfo.lon], 12);

L.control.zoom({ position: "topright" }).addTo(map);
L.control
  .layers({ Satelit: satelitEsri, "Peta Jalan": osmStandard }, null, {
    position: "topright",
  })
  .addTo(map);

const createMarkerIcon = (color) => {
  return L.icon({
    iconUrl: `https://raw.githubusercontent.com/pointhi/leaflet-color-markers/master/img/marker-icon-2x-${color}.png`,
    shadowUrl:
      "https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/images/marker-shadow.png",
    iconSize: [22, 36],
    iconAnchor: [11, 36],
    popupAnchor: [1, -32],
    shadowSize: [36, 36],
  });
};

const kudIcon = createMarkerIcon("blue");
L.marker([kudInfo.lat, kudInfo.lon], { icon: kudIcon }).addTo(map).bindPopup(`
    <div class="text-xs">
        <span class="font-bold text-slate-800">${kudInfo.nama}</span>
        <p class="text-slate-500 mt-0.5">Basis Armada Pusat</p>
    </div>
`);

const markers = {};
let activeRouteLayer = null;
let selectedDeviceId = null;
let targetLogIdSelesai = null;
const appStartTime = Date.now();

function getExactGrade(rawGrade) {
  if (!rawGrade) return "UNKNOWN";
  const text = String(rawGrade).trim().toUpperCase();
  if (text.includes("C")) return "C";
  if (text.includes("B")) return "B";
  if (text.includes("A")) return "A";
  return "UNKNOWN";
}

// Menghitung sisa masa simpan dan penyesuaian mutu (Grade A, B, C / Rusak) seiring berjalannya waktu
function getEffectiveItemData(item, nowTime = Date.now()) {
  const isSelesai = item.status === "SELESAI";

  if (item._initial_sisa === undefined) {
    item._initial_sisa = Number(item.sisa_waktu_menit);
  }
  if (item._initial_grade === undefined) {
    item._initial_grade = item.grade;
  }

  // Jika pengiriman sudah selesai (tiba di KUD), sisa waktu dan mutu tidak berkurang lagi
  if (isSelesai) {
    const gr = getExactGrade(item.grade);
    return {
      sisaWaktu: item.sisa_waktu_menit,
      grade: gr,
      isRusak: gr === "C",
    };
  }

  // Hitung selisih menit yang telah berlalu sejak sampel diambil
  let elapsedMinutes = 0;
  if (item._receivedAt) {
    elapsedMinutes = Math.floor((nowTime - item._receivedAt) / 60000);
  } else if (item.created_at) {
    const createdTime = new Date(item.created_at.replace(" ", "T")).getTime();
    if (!isNaN(createdTime)) {
      const diffFromCreated = Math.floor((nowTime - createdTime) / 60000);
      if (diffFromCreated >= 0 && diffFromCreated < 720) {
        elapsedMinutes = diffFromCreated;
      } else {
        elapsedMinutes = Math.floor((nowTime - appStartTime) / 60000);
      }
    } else {
      elapsedMinutes = Math.floor((nowTime - appStartTime) / 60000);
    }
  } else {
    elapsedMinutes = Math.floor((nowTime - appStartTime) / 60000);
  }

  const currentSisa = Math.max(0, item._initial_sisa - elapsedMinutes);

  // Penentuan Grade Mutu Berdasarkan Waktu Berjalan (Sesuai model TinyML pelatihan):
  // > 90 menit: Grade A (Prima)
  // 30 s/d 90 menit: Grade B (Harus segera diambil)
  // < 30 menit (atau 0): Grade C (Rusak)
  let effectiveGrade = "C";
  let isRusak = false;

  if (currentSisa > 90) {
    effectiveGrade = "A";
  } else if (currentSisa >= 30) {
    effectiveGrade = "B";
  } else {
    effectiveGrade = "C";
    isRusak = true;
  }

  // Jika mutu awal dari sensor sudah Grade C, tetap Rusak
  if (getExactGrade(item._initial_grade || item.grade) === "C") {
    effectiveGrade = "C";
    isRusak = true;
  }

  return {
    sisaWaktu: currentSisa,
    grade: effectiveGrade,
    isRusak: isRusak,
  };
}

function hitungGradeStats(data) {
  let countA = 0,
    countB = 0,
    countC = 0;
  data.forEach((d) => {
    const eff = getEffectiveItemData(d);
    if (eff.grade === "A") countA++;
    else if (eff.grade === "B") countB++;
    else if (eff.grade === "C") countC++;
  });
  return [countA, countB, countC];
}

function updateStatCards(data) {
  const elTotal = document.getElementById("statTotal");
  if (elTotal) elTotal.innerText = data.length;

  const elBelumSelesai = document.getElementById("statBelumSelesai");
  if (elBelumSelesai) {
    const prosesCount = data.filter(
      (d) => (d.status || "PROSES") !== "SELESAI",
    ).length;
    elBelumSelesai.innerText = prosesCount;
  }

  const elGradeC = document.getElementById("statGradeC");
  if (elGradeC) {
    const gradeCCount = data.filter(
      (d) => getEffectiveItemData(d).grade === "C",
    ).length;
    elGradeC.innerText = gradeCCount;
  }

  const elAvgSuhu = document.getElementById("statAvgSuhu");
  if (elAvgSuhu) {
    if (data.length > 0) {
      const totalSuhu = data.reduce(
        (acc, curr) => acc + Number(curr.suhu || 0),
        0,
      );
      elAvgSuhu.innerText = `${(totalSuhu / data.length).toFixed(1)}°C`;
    } else {
      elAvgSuhu.innerText = "0.0°C";
    }
  }
}

// Chart Donut Kualitas Susu
const ctxDoughnut = document
  .getElementById("gradeDoughnutChart")
  .getContext("2d");
const gradeChart = new Chart(ctxDoughnut, {
  type: "doughnut",
  data: {
    labels: ["Grade A", "Grade B", "Grade C / Rusak"],
    datasets: [
      {
        data: hitungGradeStats(dbData),
        backgroundColor: ["#10b981", "#f59e0b", "#f43f5e"],
        borderWidth: 2,
        borderColor: "#ffffff",
      },
    ],
  },
  options: {
    responsive: true,
    maintainAspectRatio: false,
    plugins: {
      legend: {
        position: "bottom",
        labels: {
          boxWidth: 12,
          font: { size: 11, family: "Inter, sans-serif" },
        },
      },
    },
    cutout: "70%",
  },
});

// Chart Urgensi (Hanya log yang belum selesai)
function getUrgencyData(data) {
  const activeData = data
    .filter((d) => (d.status || "PROSES") !== "SELESAI")
    .slice(0, 5)
    .reverse();
  return {
    items: activeData,
    labels: activeData.map((d) => {
      const eff = getEffectiveItemData(d);
      return eff.isRusak ? `${d.device_id} [RUSAK]` : `${d.device_id}`;
    }),
    shelfLife: activeData.map((d) => getEffectiveItemData(d).sisaWaktu),
    eta: activeData.map(
      (d) =>
        d.jarak_tempuh_menit ||
        Math.max(3, Math.round((d.jarak_km / 30.0) * 60) + 3),
    ),
  };
}

const initUrgency = getUrgencyData(dbData);
const ctxBar = document.getElementById("urgencyBarChart").getContext("2d");
const urgencyChart = new Chart(ctxBar, {
  type: "bar",
  data: {
    labels: initUrgency.labels,
    datasets: [
      {
        label: "Sisa Masa Simpan",
        data: initUrgency.shelfLife,
        backgroundColor: "#ef4444",
        borderRadius: 4,
      },
      {
        label: "Estimasi Truk",
        data: initUrgency.eta,
        backgroundColor: "#0284c7",
        borderRadius: 4,
      },
    ],
  },
  options: {
    indexAxis: "y",
    responsive: true,
    maintainAspectRatio: false,
    scales: {
      x: { grid: { color: "#f1f5f9" }, ticks: { font: { size: 10 } } },
      y: { grid: { display: false }, ticks: { font: { size: 11 } } },
    },
    plugins: {
      legend: { position: "top", labels: { boxWidth: 12, font: { size: 11 } } },
      tooltip: {
        callbacks: {
          afterBody: function (context) {
            const index = context[0].dataIndex;
            const currentData = getUrgencyData(dbData);
            const rawItem = currentData.items ? currentData.items[index] : null;
            if (!rawItem) return "";

            const eff = getEffectiveItemData(rawItem);
            const sisa = eff.sisaWaktu;
            const eta = currentData.eta[index];
            const diff = sisa - eta;

            if (eff.isRusak) {
              return [
                "---------------------",
                `Mutu: Grade C (${sisa}m - RUSAK)`,
                "Status: SUSU TELAH RUSAK",
                `Selisih Waktu: ${diff >= 0 ? "+" + diff : diff} Menit`,
                "Keputusan: Tolak / Jangan Dicampur Tangki",
              ];
            }
            if (diff >= 0) {
              return [
                "---------------------",
                `Mutu: Grade ${eff.grade} (${sisa}m)`,
                `Selisih Waktu: +${diff} Menit (Surplus)`,
                "Keputusan: AMAN Tiba Tepat Waktu",
              ];
            } else {
              return [
                "---------------------",
                `Mutu: Grade ${eff.grade} (${sisa}m)`,
                `Selisih Waktu: ${diff} Menit (Defisit)`,
                "Keputusan: KRITIS - Terancam Rusak di Jalan",
              ];
            }
          },
        },
      },
    },
  },
});

function refreshCharts() {
  updateStatCards(dbData);
  gradeChart.data.datasets[0].data = hitungGradeStats(dbData);
  gradeChart.update();

  const urg = getUrgencyData(dbData);
  urgencyChart.data.labels = urg.labels;
  urgencyChart.data.datasets[0].data = urg.shelfLife;
  urgencyChart.data.datasets[1].data = urg.eta;
  urgencyChart.update();
}

// Marker Peta
function pasangMarker(item) {
  const isDone = item.status === "SELESAI";
  const eff = getEffectiveItemData(item);
  const gr = eff.grade;
  const isC = eff.isRusak;
  const warna = isDone
    ? "blue"
    : isC
      ? "red"
      : gr === "B"
        ? "orange"
        : "green";
  const icon = createMarkerIcon(warna);

  const etaMenit =
    item.jarak_tempuh_menit ||
    Math.max(3, Math.round((item.jarak_km / 30.0) * 60) + 3);
  const diff = eff.sisaWaktu - etaMenit;

  let hasilBadge = "";
  if (isC) {
    hasilBadge = `<span class="text-rose-700 font-bold bg-rose-100 px-1.5 py-0.5 rounded border border-rose-300">RUSAK (${diff >= 0 ? "+" + diff : diff}m)</span>`;
  } else if (diff < 0) {
    hasilBadge = `<span class="text-rose-700 font-bold bg-rose-50 px-1.5 py-0.5 rounded border border-rose-200">Kritis (Defisit ${Math.abs(diff)}m)</span>`;
  } else {
    hasilBadge = `<span class="text-emerald-700 font-bold bg-emerald-50 px-1.5 py-0.5 rounded border border-emerald-200">Aman (Surplus +${diff}m)</span>`;
  }

  const gradeDisplay = isC
    ? `<span class="text-rose-600 font-bold">Grade C (${eff.sisaWaktu}m - Rusak)</span>`
    : `<b>Grade ${eff.grade}</b> (${eff.sisaWaktu}m)`;

  const gmapsUrl = `https://www.google.com/maps?q=${item.latitude},${item.longitude}`;
  const popupContent = `
        <div class="text-xs space-y-1.5 font-sans">
            <div class="font-bold text-slate-800">${item.device_id}</div>
            <div class="text-slate-600 text-[11px]">${item.alamat}</div>
            <div class="text-slate-600">Suhu: <b>${item.suhu.toFixed(1)}°C</b> | Mutu: ${gradeDisplay}</div>
            <div class="text-slate-600">Logistik Truk: <b>${etaMenit}m</b> | Hasil: ${hasilBadge}</div>
            <div class="text-slate-600">Status: <b>${item.status || "PROSES"}</b></div>
            <div class="pt-1 mt-1 border-t border-slate-200">
                <a href="${gmapsUrl}" target="_blank" class="text-sky-600 font-medium hover:underline">Google Maps &rarr;</a>
            </div>
        </div>
    `;

  if (markers[item.device_id]) {
    markers[item.device_id]
      .setLatLng([item.latitude, item.longitude])
      .setIcon(icon)
      .setPopupContent(popupContent);
  } else {
    markers[item.device_id] = L.marker([item.latitude, item.longitude], {
      icon: icon,
    })
      .addTo(map)
      .bindPopup(popupContent);
  }
}

// Fungsi Pembantu Visualisasi Perbandingan Waktu Sisa vs Masa Tempuh
function renderRouteVisualComparison(distKm, durasiMenit, deviceId) {
  const item = dbData.find((d) => d.device_id === deviceId);
  if (!item) return;

  const eff = getEffectiveItemData(item);
  const gr = eff.grade;
  const isC = eff.isRusak;
  const sisaMenit = eff.sisaWaktu;

  const nameEl = document.getElementById("routeDestName");
  if (nameEl) {
    nameEl.innerText = `${deviceId} ${item && item.alamat ? "• " + item.alamat : ""}`;
  }

  const distEl = document.getElementById("routeDist");
  if (distEl) distEl.innerText = `Jarak Tempuh: ${distKm} km`;

  const badgeGradeEl = document.getElementById("routeBadgeGrade");
  if (badgeGradeEl) {
    if (isC) {
      badgeGradeEl.className =
        "px-2 py-0.5 rounded text-[11px] font-bold border whitespace-nowrap bg-rose-100 text-rose-800 border-rose-300";
      badgeGradeEl.innerText = `Grade C (${sisaMenit}m - Rusak)`;
    } else if (gr === "B") {
      badgeGradeEl.className =
        "px-2 py-0.5 rounded text-[11px] font-bold border whitespace-nowrap bg-amber-100 text-amber-800 border-amber-300";
      badgeGradeEl.innerText = `Grade B (${sisaMenit}m)`;
    } else {
      badgeGradeEl.className =
        "px-2 py-0.5 rounded text-[11px] font-bold border whitespace-nowrap bg-emerald-100 text-emerald-800 border-emerald-300";
      badgeGradeEl.innerText = `Grade A (${sisaMenit}m)`;
    }
  }

  const shelfLifeEl = document.getElementById("routeShelfLife");
  if (shelfLifeEl) {
    if (isC) {
      shelfLifeEl.innerHTML = `<span class="text-rose-600 font-bold">${sisaMenit} Menit</span> <span class="text-[10px] text-rose-500 font-medium block">(Rusak)</span>`;
    } else {
      shelfLifeEl.innerText = `${sisaMenit} Menit`;
    }
  }

  const etaEl = document.getElementById("routeEta");
  if (etaEl) {
    etaEl.innerText = `${durasiMenit} Menit`;
  }

  const marginEl = document.getElementById("routeMargin");
  const diff = sisaMenit - durasiMenit;
  if (marginEl) {
    if (isC) {
      marginEl.innerHTML = `<span class="text-rose-600 font-bold">${diff >= 0 ? "+" + diff : diff} Menit (Rusak)</span>`;
    } else if (diff >= 0) {
      marginEl.innerHTML = `<span class="text-emerald-600 font-bold">+${diff} Menit</span>`;
    } else {
      marginEl.innerHTML = `<span class="text-rose-600 font-bold">${diff} Menit</span>`;
    }
  }

  const resultEl = document.getElementById("routeComparisonResult");
  if (resultEl) {
    if (isC) {
      resultEl.className =
        "p-2.5 rounded-lg border text-xs bg-rose-100/90 border-rose-300 text-rose-950 shadow-xs";
      resultEl.innerHTML = `
        <div class="flex items-start gap-2">
          <div>
            <div class="font-bold text-xs uppercase tracking-wide text-rose-900">Hasil: Susu Rusak (Grade C)</div>
            <div class="text-[11px] text-rose-800 mt-0.5 leading-snug">Susu peternakan ini telah melewati batas masa simpan (Rusak). Dilarang dicampur ke tangki utama KUD!</div>
          </div>
        </div>
      `;
    } else if (diff < 0) {
      resultEl.className =
        "p-2.5 rounded-lg border text-xs bg-amber-100/90 border-amber-300 text-amber-950 shadow-xs";
      resultEl.innerHTML = `
        <div class="flex items-start gap-2">
          <span class="text-base leading-none">⚠️</span>
          <div>
            <div class="font-bold text-xs uppercase tracking-wide text-amber-900">Hasil: Berisiko Rusak di Perjalanan</div>
            <div class="text-[11px] text-amber-800 mt-0.5 leading-snug">Estimasi truk tiba (${durasiMenit}m) melebihi masa simpan susu (${sisaMenit}m). Defisit ${Math.abs(diff)} menit, butuh tindakan prioritas armada!</div>
          </div>
        </div>
      `;
    } else {
      resultEl.className =
        "p-2.5 rounded-lg border text-xs bg-emerald-100/90 border-emerald-300 text-emerald-950 shadow-xs";
      resultEl.innerHTML = `
        <div class="flex items-start gap-2">
          <div>
            <div class="font-bold text-xs uppercase tracking-wide text-emerald-900">Hasil: Aman Tiba di KUD</div>
            <div class="text-[11px] text-emerald-800 mt-0.5 leading-snug">Truk tiba dalam ${durasiMenit} menit sebelum mutu susu turun. Surplus margin aman logistik: +${diff} menit.</div>
          </div>
        </div>
      `;
    }
  }
}

// Navigasi & Rute ke Peternakan
async function navigasiKePeternakan(
  event,
  lat,
  lon,
  deviceId,
  sisaWaktuGrade,
  grade,
  status,
) {
  // Support jika dipanggil langsung tanpa parameter event
  if (typeof event === "number") {
    status = grade;
    grade = sisaWaktuGrade;
    sisaWaktuGrade = deviceId;
    deviceId = lon;
    lon = lat;
    lat = event;
    event = null;
  }

  if (event && typeof event.preventDefault === "function") {
    event.preventDefault();
    event.stopPropagation();
  }

  selectedDeviceId = deviceId;

  const infoBox = document.getElementById("routeInfoBox");
  if (infoBox) {
    infoBox.classList.remove("hidden");
    const nameEl = document.getElementById("routeDestName");
    if (nameEl) nameEl.innerText = `Menghitung rute: ${deviceId}...`;
  }

  // Cari item terkini untuk mengetahui grade efektif
  const currentItem = dbData.find((d) => d.device_id === deviceId);
  const eff = currentItem
    ? getEffectiveItemData(currentItem)
    : { grade: getExactGrade(grade), isRusak: getExactGrade(grade) === "C" };

  // Penentuan warna rute sesuai status & grade efektif
  let routeColor = "#0284c7"; // Selesai = Biru
  if (status !== "SELESAI") {
    if (eff.grade === "A") routeColor = "#10b981"; // Hijau
    else if (eff.grade === "B") routeColor = "#f59e0b"; // Kuning
    else if (eff.isRusak || eff.grade === "C") routeColor = "#ef4444"; // Merah
  }

  try {
    const osrmUrl = `https://router.project-osrm.org/route/v1/driving/${kudInfo.lon},${kudInfo.lat};${lon},${lat}?overview=full&geometries=geojson`;
    const res = await fetch(osrmUrl);
    const data = await res.json();

    // Cegah race condition jika user telah memilih peternakan lain
    if (selectedDeviceId !== deviceId) return;

    if (data.routes && data.routes.length > 0) {
      if (activeRouteLayer) map.removeLayer(activeRouteLayer);

      activeRouteLayer = L.geoJSON(data.routes[0].geometry, {
        style: { color: routeColor, weight: 5, opacity: 0.9 },
      }).addTo(map);

      // Berhenti dan fokus langsung ke lokasi peternakan yang diklik
      map.flyTo([lat, lon], 15, {
        animate: true,
        duration: 0.8,
      });

      if (markers[deviceId]) {
        markers[deviceId].openPopup();
      }

      const distKm = (data.routes[0].distance / 1000).toFixed(1);
      const durasiMenit = Math.round(data.routes[0].duration / 60);

      renderRouteVisualComparison(distKm, durasiMenit, deviceId);
    }
  } catch (e) {
    console.warn(
      "OSRM offline atau gagal merespons, beralih ke polyline lurus:",
      e,
    );
    if (selectedDeviceId !== deviceId) return;

    if (activeRouteLayer) map.removeLayer(activeRouteLayer);
    activeRouteLayer = L.polyline(
      [
        [kudInfo.lat, kudInfo.lon],
        [lat, lon],
      ],
      {
        color: routeColor,
        weight: 4,
        dashArray: "6, 8",
      },
    ).addTo(map);

    // Berhenti dan fokus ke lokasi peternakan
    map.flyTo([lat, lon], 15, {
      animate: true,
      duration: 0.8,
    });

    if (markers[deviceId]) {
      markers[deviceId].openPopup();
    }

    const distKm = (
      L.latLng(kudInfo.lat, kudInfo.lon).distanceTo(L.latLng(lat, lon)) / 1000
    ).toFixed(1);
    const durasiMenit = Math.max(3, Math.round((distKm / 30.0) * 60) + 3);

    renderRouteVisualComparison(distKm, durasiMenit, deviceId);
  }
}

// Modal Konfirmasi Penerimaan
function bukaModalKonfirmasi(id, deviceId, gradeText) {
  targetLogIdSelesai = id;

  const modalDeviceId = document.getElementById("modalDeviceId");
  if (modalDeviceId) modalDeviceId.innerText = deviceId;

  const modalGradeInfo = document.getElementById("modalGradeInfo");
  if (modalGradeInfo) modalGradeInfo.innerText = gradeText;

  const modal = document.getElementById("confirmModal");
  if (modal) {
    modal.classList.remove("hidden");
    modal.classList.add("flex");
  }
}

function tutupModalKonfirmasi() {
  targetLogIdSelesai = null;
  const modal = document.getElementById("confirmModal");
  if (modal) {
    modal.classList.add("hidden");
    modal.classList.remove("flex");
  }
}

// Eksekusi Konfirmasi Selesai
async function konfirmasiSelesaiAction() {
  if (!targetLogIdSelesai) return;

  const id = targetLogIdSelesai;
  const btn = document.getElementById("btnKonfirmasiSelesai");
  const originalText = btn ? btn.innerText : "";
  if (btn) {
    btn.disabled = true;
    btn.innerText = "Menyimpan...";
  }

  try {
    const res = await fetch(`/api/predictions/${id}/complete`, {
      method: "POST",
    });
    if (res.ok) {
      const item = dbData.find((d) => d.id === id);
      if (item) {
        item.status = "SELESAI";
        renderTabel(dbData);
        pasangMarker(item);
        refreshCharts();

        const eff = getEffectiveItemData(item);
        const sisaWaktuGradeText = eff.isRusak
          ? `${eff.sisaWaktu}m (Grade C - Rusak)`
          : `${eff.sisaWaktu}m (Grade ${eff.grade})`;

        navigasiKePeternakan(
          null,
          item.latitude,
          item.longitude,
          item.device_id,
          sisaWaktuGradeText,
          eff.grade,
          "SELESAI",
        );
      }
    } else {
      console.error("Gagal menyelesaikan status via server:", res.status);
    }
  } catch (e) {
    console.error("Gagal menyelesaikan pengiriman:", e);
  } finally {
    if (btn) {
      btn.disabled = false;
      btn.innerText = originalText;
    }
    tutupModalKonfirmasi();
  }
}

// Event listener tombol konfirmasi di modal
const btnKonfirmasi = document.getElementById("btnKonfirmasiSelesai");
if (btnKonfirmasi) {
  btnKonfirmasi.addEventListener("click", konfirmasiSelesaiAction);
}

// Tutup modal jika klik di luar box (backdrop) atau tombol Escape
const confirmModalEl = document.getElementById("confirmModal");
if (confirmModalEl) {
  confirmModalEl.addEventListener("click", function (e) {
    if (e.target === confirmModalEl) {
      tutupModalKonfirmasi();
    }
  });
}
document.addEventListener("keydown", function (e) {
  if (e.key === "Escape") {
    tutupModalKonfirmasi();
  }
});

// Render Tabel Logistik: 2 Baris dalam 1 Kolom Aksi (Rute di atas, Selesai di bawah)
function renderTabel(data, highlightedId = null) {
  const tbody = document.getElementById("tableBody");
  tbody.innerHTML = "";

  if (!data || data.length === 0) {
    tbody.innerHTML = `
      <tr>
        <td colspan="7" class="py-12 text-center text-slate-400">
          Belum ada data pengiriman susu untuk hari ini.
        </td>
      </tr>
    `;
    const counter = document.getElementById("logCounter");
    if (counter) counter.innerText = "0 Data";
    return;
  }

  data.forEach((item) => {
    const isSelesai = item.status === "SELESAI";
    const tr = document.createElement("tr");
    tr.className = `hover:bg-slate-50 transition-colors ${item.id === highlightedId ? "row-new-highlight" : ""} ${isSelesai ? "opacity-75 bg-slate-50/50" : ""}`;

    const distBadge = (item.metode_pengiriman || "")
      .toUpperCase()
      .includes("JEMPUT")
      ? '<span class="bg-blue-50 text-blue-700 px-1.5 py-0.5 rounded font-medium text-[11px] border border-blue-200">Jemput</span>'
      : '<span class="bg-slate-100 text-slate-700 px-1.5 py-0.5 rounded font-medium text-[11px] border border-slate-200">Antar</span>';

    const eff = getEffectiveItemData(item);
    const gr = eff.grade;
    const isC = eff.isRusak;
    const etaMenit =
      item.jarak_tempuh_menit ||
      Math.max(3, Math.round((item.jarak_km / 30.0) * 60) + 3);
    const diff = eff.sisaWaktu - etaMenit;

    let gradeBadgeContent = "";
    if (isC) {
      gradeBadgeContent = `<span class="px-1.5 py-0.5 rounded border text-[11px] font-bold whitespace-nowrap inline-flex items-center gap-1.5 bg-rose-100 text-rose-800 border-rose-300">
          <span>Grade C (${eff.sisaWaktu}m)</span>
          <span class="text-[10px] bg-rose-200 text-rose-900 px-1 rounded font-bold">Rusak</span>
      </span>`;
    } else {
      const marginLabel =
        diff >= 0
          ? `<span class="text-[10px] text-emerald-800 bg-emerald-100/90 px-1 rounded font-semibold">+${diff}m</span>`
          : `<span class="text-[10px] text-rose-800 bg-rose-200/90 px-1 rounded font-bold">${diff}m</span>`;
      const bgStyle =
        gr === "B"
          ? "bg-amber-50 text-amber-700 border-amber-200"
          : "bg-emerald-50 text-emerald-700 border-emerald-200";
      gradeBadgeContent = `<span class="px-1.5 py-0.5 rounded border text-[11px] font-medium whitespace-nowrap inline-flex items-center gap-1.5 ${bgStyle}">
          <span>Grade ${eff.grade} (${eff.sisaWaktu}m)</span>
          ${marginLabel}
      </span>`;
    }

    const modalGradeText = isC
      ? `Grade C (${eff.sisaWaktu}m - Rusak)`
      : `Grade ${eff.grade} (${eff.sisaWaktu}m)`;

    const sisaWaktuGradeText = isC
      ? `${eff.sisaWaktu}m (Grade C - Rusak)`
      : `${eff.sisaWaktu}m (Grade ${eff.grade})`;

    // 2 Baris dalam 1 kolom aksi: Rute di atas, Selesai di bawah (ringkas & hemat tempat)
    const actionButtons = isSelesai
      ? `<div class="flex flex-col gap-1 w-16 mx-auto py-0.5">
           <button type="button" class="w-full bg-slate-100 hover:bg-slate-200 border border-slate-300 text-slate-700 py-0.5 px-1 rounded text-[10px] font-medium leading-tight transition-colors shadow-xs"
               onclick="navigasiKePeternakan(event, ${item.latitude}, ${item.longitude}, '${item.device_id}', '${sisaWaktuGradeText}', '${eff.grade}', 'SELESAI')">
               Rute
           </button>
           <button type="button" disabled class="w-full bg-emerald-50 border border-emerald-200 text-emerald-700 py-0.5 px-1 rounded text-[10px] font-medium leading-tight cursor-default opacity-90 text-center">
               Selesai ✓
           </button>
         </div>`
      : `<div class="flex flex-col gap-1 w-16 mx-auto py-0.5">
           <button type="button" class="w-full bg-slate-100 hover:bg-slate-200 border border-slate-300 text-slate-700 py-0.5 px-1 rounded text-[10px] font-medium leading-tight transition-colors shadow-xs"
               onclick="navigasiKePeternakan(event, ${item.latitude}, ${item.longitude}, '${item.device_id}', '${sisaWaktuGradeText}', '${eff.grade}', '${item.status || "PROSES"}')">
               Rute
           </button>
           <button type="button" class="w-full bg-emerald-600 hover:bg-emerald-700 text-white py-0.5 px-1 rounded text-[10px] font-medium leading-tight transition-colors shadow-sm text-center"
               onclick="bukaModalKonfirmasi(${item.id}, '${item.device_id}', '${modalGradeText}')">
               Selesai
           </button>
         </div>`;

    tr.innerHTML = `
        <td class="py-2 px-3 whitespace-nowrap font-mono text-[11px] text-slate-600">${item.created_at || "-"}</td>
        <td class="py-2 px-3 font-medium text-slate-900 whitespace-nowrap">${item.device_id}</td>
        <td class="py-2 px-3 max-w-[160px] truncate text-slate-600" title="${item.alamat}">${item.alamat}</td>
        <td class="py-2 px-3 text-right font-medium text-slate-700 whitespace-nowrap">${item.suhu.toFixed(1)}°C</td>
        <td class="py-2 px-3 whitespace-nowrap">
            ${gradeBadgeContent}
        </td>
        <td class="py-2 px-3 whitespace-nowrap">${distBadge}</td>
        <td class="py-2 px-2 text-center align-middle">${actionButtons}</td>
    `;
    tbody.appendChild(tr);
  });

  const counter = document.getElementById("logCounter");
  if (counter) counter.innerText = `${data.length} Data`;
}

// WebSocket Real-time Handler (Auto-reconnect tanpa mereload halaman web)
let reconnectTimeout = null;

function connectWebSocket() {
  if (reconnectTimeout) {
    clearTimeout(reconnectTimeout);
    reconnectTimeout = null;
  }

  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const ws = new WebSocket(`${protocol}//${window.location.host}/ws`);

  ws.onopen = function () {
    const dot = document.getElementById("wsStatusDot");
    const text = document.getElementById("wsStatusText");
    if (dot) dot.className = "h-2 w-2 rounded-full bg-emerald-500";
    if (text) text.innerText = "Real-time terhubung";
  };

  ws.onmessage = function (event) {
    try {
      const msg = JSON.parse(event.data);
      if (msg.type === "NEW_TELEMETRY") {
        const dataBaru = msg.data;
        dataBaru._receivedAt = Date.now();
        dataBaru._initial_sisa = dataBaru.sisa_waktu_menit;
        dataBaru._initial_grade = dataBaru.grade;

        if (dataBaru.created_at.startsWith(currentFilterDate)) {
          const existingIdx = dbData.findIndex((d) => d.id === dataBaru.id);
          if (existingIdx >= 0) {
            dbData[existingIdx] = dataBaru;
          } else {
            dbData.push(dataBaru);
          }
          dbData.sort(
            (a, b) =>
              getEffectiveItemData(a).sisaWaktu -
              getEffectiveItemData(b).sisaWaktu,
          );
          renderTabel(dbData, dataBaru.id);
          pasangMarker(dataBaru);
          refreshCharts();
        }
      } else if (msg.type === "STATUS_UPDATED") {
        const target = dbData.find((d) => d.id === msg.id);
        if (target) {
          target.status = msg.status;
          renderTabel(dbData);
          pasangMarker(target);
          refreshCharts();
        }
      }
    } catch (e) {
      console.warn("Gagal memproses pesan real-time:", e);
    }
  };

  ws.onclose = function () {
    const dot = document.getElementById("wsStatusDot");
    const text = document.getElementById("wsStatusText");
    if (dot) dot.className = "h-2 w-2 rounded-full bg-rose-500";
    if (text) text.innerText = "Terputus (menghubungkan ulang...)";
    reconnectTimeout = setTimeout(connectWebSocket, 3000);
  };

  ws.onerror = function () {
    ws.close();
  };
}

// Inisialisasi Pertama Kali saat Halaman Dimuat
if (dbData && dbData.length > 0) {
  dbData.sort(
    (a, b) =>
      getEffectiveItemData(a).sisaWaktu - getEffectiveItemData(b).sisaWaktu,
  );
  renderTabel(dbData);
  dbData.forEach((row) => pasangMarker(row));

  const mostUrgent =
    dbData.find((d) => (d.status || "PROSES") !== "SELESAI") || dbData[0];
  const effMostUrgent = getEffectiveItemData(mostUrgent);
  const mostUrgentText = effMostUrgent.isRusak
    ? `${effMostUrgent.sisaWaktu}m (Grade C - Rusak)`
    : `${effMostUrgent.sisaWaktu}m (Grade ${effMostUrgent.grade})`;

  navigasiKePeternakan(
    null,
    mostUrgent.latitude,
    mostUrgent.longitude,
    mostUrgent.device_id,
    mostUrgentText,
    effMostUrgent.grade,
    mostUrgent.status,
  );
} else {
  renderTabel([]);
}
updateStatCards(dbData);
connectWebSocket();

// Timer pembaruan waktu berjalan berkala (setiap 15 detik)
setInterval(() => {
  if (dbData && dbData.length > 0) {
    dbData.sort(
      (a, b) =>
        getEffectiveItemData(a).sisaWaktu - getEffectiveItemData(b).sisaWaktu,
    );
    renderTabel(dbData);
    dbData.forEach((row) => pasangMarker(row));
    refreshCharts();

    if (
      selectedDeviceId &&
      !document.getElementById("routeInfoBox").classList.contains("hidden")
    ) {
      const activeItem = dbData.find((d) => d.device_id === selectedDeviceId);
      if (activeItem) {
        const distKm = (
          L.latLng(kudInfo.lat, kudInfo.lon).distanceTo(
            L.latLng(activeItem.latitude, activeItem.longitude),
          ) / 1000
        ).toFixed(1);
        const durasiMenit =
          activeItem.jarak_tempuh_menit ||
          Math.max(3, Math.round((distKm / 30.0) * 60) + 3);
        renderRouteVisualComparison(distKm, durasiMenit, selectedDeviceId);
      }
    }
  }
}, 15000);