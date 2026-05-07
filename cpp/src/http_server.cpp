#include "http_server.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string trim_copy(const std::string& input);

std::string json_escape(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 8);
    for (const char ch : input) {
        switch (ch) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: output += ch; break;
        }
    }
    return output;
}

std::string http_ok(const std::string& content_type, const std::string& body) {
    std::ostringstream out;
    out
        << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-cache\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return out.str();
}

std::string http_json(int status_code, const std::string& status_text, const std::string& body) {
    std::ostringstream out;
    out
        << "HTTP/1.1 " << status_code << ' ' << status_text << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-cache\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return out.str();
}

std::string http_not_found() {
    return http_json(404, "Not Found", "{\"error\":\"not found\"}");
}

std::string http_method_not_allowed() {
    return http_json(405, "Method Not Allowed", "{\"error\":\"method not allowed\"}");
}

std::string http_bad_request(const std::string& message) {
    return http_json(400, "Bad Request", "{\"error\":\"" + json_escape(message) + "\"}");
}

std::string http_server_error(const std::string& message) {
    return http_json(500, "Internal Server Error", "{\"error\":\"" + json_escape(message) + "\"}");
}

std::string http_unauthorized() {
    const std::string body = "{\"error\":\"authentication required\"}";
    std::ostringstream out;
    out
        << "HTTP/1.1 401 Unauthorized\r\n"
        << "WWW-Authenticate: Basic realm=\"Comvisplus\"\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-cache\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return out.str();
}

std::string base64_encode(const std::string& input) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    for (std::size_t i = 0; i < input.size(); i += 3) {
        const unsigned int octet_a = static_cast<unsigned char>(input[i]);
        const unsigned int octet_b = (i + 1 < input.size()) ? static_cast<unsigned char>(input[i + 1]) : 0;
        const unsigned int octet_c = (i + 2 < input.size()) ? static_cast<unsigned char>(input[i + 2]) : 0;
        const unsigned int triple = (octet_a << 16U) | (octet_b << 8U) | octet_c;

        output += table[(triple >> 18U) & 0x3FU];
        output += table[(triple >> 12U) & 0x3FU];
        output += (i + 1 < input.size()) ? table[(triple >> 6U) & 0x3FU] : '=';
        output += (i + 2 < input.size()) ? table[triple & 0x3FU] : '=';
    }

    return output;
}

std::string lower_copy(const std::string& input) {
    std::string output = input;
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return output;
}

std::string header_value(const std::string& request_head, const std::string& header_name) {
    const std::string wanted = lower_copy(header_name);
    std::istringstream lines(request_head);
    std::string line;

    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto sep = line.find(':');
        if (sep == std::string::npos) {
            continue;
        }

        if (lower_copy(trim_copy(line.substr(0, sep))) == wanted) {
            return trim_copy(line.substr(sep + 1));
        }
    }

    return {};
}

bool is_authorized(const comvisplus::AppConfig& config, const std::string& request_head) {
    if (!config.auth_enabled) {
        return true;
    }

    const std::string auth = header_value(request_head, "Authorization");
    const std::string expected = "Basic " + base64_encode(config.auth_username + ":" + config.auth_password);
    return auth == expected;
}

std::string trim_copy(const std::string& input) {
    std::size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])) != 0) {
        start += 1;
    }
    std::size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        end -= 1;
    }
    return input.substr(start, end - start);
}

std::string url_decode(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '+') {
            output += ' ';
        } else if (ch == '%' && i + 2 < input.size()) {
            const std::string hex = input.substr(i + 1, 2);
            output += static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            i += 2;
        } else {
            output += ch;
        }
    }
    return output;
}

std::map<std::string, std::string> parse_form_body(const std::string& body) {
    std::map<std::string, std::string> values;
    std::size_t start = 0;
    while (start <= body.size()) {
        const auto end = body.find('&', start);
        const std::string token = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto sep = token.find('=');
        if (sep != std::string::npos) {
            values[url_decode(token.substr(0, sep))] = url_decode(token.substr(sep + 1));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return values;
}

float parse_float_or_default(const std::map<std::string, std::string>& values, const std::string& key, float fallback) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    try {
        return std::stof(it->second);
    } catch (...) {
        return fallback;
    }
}

int parse_int_or_default(const std::map<std::string, std::string>& values, const std::string& key, int fallback) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    try {
        return std::stoi(it->second);
    } catch (...) {
        return fallback;
    }
}

std::string guess_content_type(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".jpeg") {
        return "image/jpeg";
    }
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".jpg") {
        return "image/jpeg";
    }
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".png") {
        return "image/png";
    }
    return "application/octet-stream";
}

std::string replace_underscores(const std::string& input) {
    std::string output = input;
    std::replace(output.begin(), output.end(), '_', ' ');
    return output;
}

std::string format_capture_time(const std::string& ymd, const std::string& hms, const std::string& millis) {
    if (ymd.size() != 8 || hms.size() != 6) {
        return trim_copy(ymd + " " + hms + (millis.empty() ? "" : ("." + millis)));
    }
    std::ostringstream out;
    out
        << ymd.substr(0, 4) << '-'
        << ymd.substr(4, 2) << '-'
        << ymd.substr(6, 2) << ' '
        << hms.substr(0, 2) << ':'
        << hms.substr(2, 2) << ':'
        << hms.substr(4, 2);
    if (!millis.empty()) {
        out << '.' << millis;
    }
    return out.str();
}

std::string root_page() {
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Comvisplus Native</title>
  <style>
    body { background:#111827; color:#e5e7eb; font-family:system-ui,sans-serif; margin:0; padding:18px; }
    h1 { margin:0 0 16px; color:#38bdf8; font-size:28px; }
    .toolbar { display:flex; gap:10px; flex-wrap:wrap; align-items:end; margin-bottom:16px; background:#1f2937; border:1px solid #374151; padding:14px; border-radius:12px; }
    .field { display:flex; flex-direction:column; gap:6px; min-width:180px; flex:1; }
    .field label { font-size:12px; color:#9ca3af; }
    .field input { background:#111827; color:#e5e7eb; border:1px solid #374151; border-radius:8px; padding:9px 11px; font-size:14px; }
    button { background:#2563eb; color:#fff; border:none; border-radius:8px; padding:9px 12px; font-weight:700; cursor:pointer; font-size:13px; }
    button.danger { background:#dc2626; }
    button.secondary { background:#374151; }
    .msg { margin:0 0 14px; font-size:13px; color:#fbbf24; min-height:18px; }
    #grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(340px,1fr)); gap:14px; align-items:start; }
    .card { background:#1f2937; border:1px solid #374151; border-radius:12px; overflow:hidden; }
    .head { padding:10px 12px; border-bottom:1px solid #374151; display:flex; justify-content:space-between; gap:10px; }
    .video-wrap { position:relative; background:#000; }
    .feed { width:100%; aspect-ratio:16/9; object-fit:contain; background:#000; display:block; max-height:260px; }
    .overlay { position:absolute; inset:0; width:100%; height:100%; }
    .overlay .line-hit { stroke:transparent; stroke-width:18; cursor:move; }
    .overlay .line-main { stroke:rgba(255,255,80,0.85); stroke-width:2.5; stroke-linecap:round; }
    .overlay .handle { fill:rgba(255,230,50,0.95); stroke:rgba(0,0,0,0.4); stroke-width:1; cursor:grab; }
    .overlay .handle.dragging { fill:#fff; cursor:grabbing; }
    .stats { display:flex; gap:10px; padding:10px 12px; font-size:13px; color:#9ca3af; flex-wrap:wrap; }
    .pill { color:#fff; font-weight:700; }
    .actions { padding:0 12px 12px; display:flex; justify-content:flex-end; gap:8px; }
    /* Gallery */
    .gallery { margin-top:24px; }
    .gallery-header { display:flex; align-items:center; gap:12px; margin:0 0 12px; flex-wrap:wrap; }
    .gallery-header h2 { font-size:18px; margin:0; color:#e5e7eb; }
    .shot-count { font-size:13px; color:#6b7280; background:#1f2937; border:1px solid #374151; padding:3px 10px; border-radius:99px; }
    .filter-bar { display:flex; gap:8px; flex-wrap:wrap; margin-bottom:14px; background:#1f2937; border:1px solid #374151; padding:12px; border-radius:12px; align-items:center; }
    .filter-bar select, .filter-bar input[type=date] { background:#111827; color:#e5e7eb; border:1px solid #374151; border-radius:8px; padding:7px 10px; font-size:13px; min-width:120px; flex:1; }
    .gallery-grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(200px,1fr)); gap:12px; }
    .shot { background:#1f2937; border:1px solid #374151; border-radius:12px; overflow:hidden; }
    .shot img { width:100%; aspect-ratio:16/9; object-fit:cover; display:block; background:#000; cursor:zoom-in; transition:opacity 0.15s; }
    .shot img:hover { opacity:0.82; }
    .shot-meta { padding:10px 12px; font-size:12px; color:#9ca3af; }
    .shot-meta strong { display:block; color:#e5e7eb; margin-bottom:4px; font-size:13px; }
    .shot-actions { padding:0 12px 12px; display:flex; justify-content:flex-end; }
    .empty-state { padding:48px; text-align:center; color:#4b5563; font-size:14px; grid-column:1/-1; }
    /* Pagination */
    .pagination { display:flex; align-items:center; justify-content:center; gap:6px; margin-top:18px; flex-wrap:wrap; padding:4px; }
    .page-btn { background:#1f2937; color:#e5e7eb; border:1px solid #374151; border-radius:8px; padding:6px 14px; font-size:13px; cursor:pointer; font-weight:600; }
    .page-btn:hover:not([disabled]) { background:#374151; }
    .page-btn.active { background:#2563eb; border-color:#2563eb; color:#fff; }
    .page-btn[disabled] { opacity:0.35; cursor:default; }
    .page-info { font-size:13px; color:#6b7280; padding:0 6px; }
    /* Lightbox */
    .lightbox { display:none; position:fixed; inset:0; background:rgba(0,0,0,0.94); z-index:1000; align-items:center; justify-content:center; flex-direction:column; gap:12px; }
    .lightbox.open { display:flex; }
    .lightbox-img { max-width:92vw; max-height:80vh; object-fit:contain; border-radius:8px; box-shadow:0 8px 48px rgba(0,0,0,0.7); }
    .lightbox-close { position:absolute; top:14px; right:18px; background:rgba(255,255,255,0.12); border:none; color:#e5e7eb; font-size:24px; cursor:pointer; line-height:1; padding:6px 13px; border-radius:8px; }
    .lightbox-close:hover { background:rgba(255,255,255,0.22); }
    .lightbox-meta { color:#9ca3af; font-size:13px; text-align:center; max-width:80vw; }
    .lightbox-nav { display:flex; gap:10px; align-items:center; }
    .lightbox-nav button { background:rgba(255,255,255,0.12); border:none; color:#e5e7eb; font-size:22px; cursor:pointer; padding:8px 18px; border-radius:8px; line-height:1; }
    .lightbox-nav button:hover:not([disabled]) { background:rgba(255,255,255,0.22); }
    .lightbox-nav button[disabled] { opacity:0.28; cursor:default; }
  </style>
</head>
<body>
  <h1>Comvisplus Native</h1>
  <div class="toolbar">
    <div class="field">
      <label for="label">Nama kamera</label>
      <input id="label" placeholder="Garasi">
    </div>
    <div class="field" style="flex:2">
      <label for="url">RTSP URL</label>
      <input id="url" placeholder="rtsp://user:password@ip/stream">
    </div>
    <div class="field" style="max-width:140px">
      <label for="skip">Frame skip</label>
      <input id="skip" type="number" min="1" value="2">
    </div>
    <button onclick="addCamera()">Tambah Kamera</button>
  </div>
  <div class="msg" id="msg"></div>
  <div id="grid"></div>

  <div class="gallery">
    <div class="gallery-header">
      <h2>Screenshot Crossing</h2>
      <span id="shot-count" class="shot-count">memuat...</span>
    </div>
    <div class="filter-bar">
      <select id="filter-camera" onchange="applyFilter()">
        <option value="">Semua Kamera</option>
      </select>
      <select id="filter-direction" onchange="applyFilter()">
        <option value="">Semua Arah</option>
        <option value="right">Right</option>
        <option value="left">Left</option>
      </select>
      <input type="date" id="filter-date" onchange="applyFilter()">
      <button class="secondary" onclick="clearFilters()">Reset</button>
      <button class="danger" onclick="deleteAllFiltered()">Hapus Semua</button>
    </div>
    <div id="shots" class="gallery-grid"></div>
    <div id="pagination" class="pagination"></div>
  </div>

  <div class="lightbox" id="lightbox">
    <button class="lightbox-close" onclick="closeLightbox()">&#x2715;</button>
    <img class="lightbox-img" id="lightbox-img" src="" alt="">
    <div class="lightbox-meta" id="lightbox-meta"></div>
    <div class="lightbox-nav">
      <button id="lb-prev" onclick="lbNavigate(-1)">&#8592;</button>
      <button id="lb-next" onclick="lbNavigate(1)">&#8594;</button>
    </div>
  </div>

  <script>
    let cameraState = [];
    let allScreenshots = [];
    let currentPage = 1;
    let lbIndex = 0;
    let lbShots = [];
    const PAGE_SIZE = 24;
    const dragState = { id: null, mode: null, start: null };

    function showMessage(text) {
      document.getElementById('msg').textContent = text || '';
    }

    function getCamera(id) {
      return cameraState.find(cam => cam.id === id);
    }

    async function addCamera() {
      const label = document.getElementById('label').value.trim();
      const url = document.getElementById('url').value.trim();
      const frameSkip = document.getElementById('skip').value.trim() || '2';
      if (!url) { showMessage('RTSP URL wajib diisi.'); return; }
      const body = new URLSearchParams({ label, url, frame_skip: frameSkip });
      const res = await fetch('/cameras', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
      const text = await res.text();
      if (!res.ok) { showMessage(text); return; }
      document.getElementById('label').value = '';
      document.getElementById('url').value = '';
      showMessage('Kamera ditambahkan.');
      loadCameras();
    }

    async function removeCamera(id) {
      if (!confirm('Hapus kamera ini?')) return;
      const res = await fetch(`/cameras/${id}`, { method: 'DELETE' });
      if (!res.ok) { showMessage('Gagal menghapus kamera.'); return; }
      showMessage('Kamera dihapus.');
      loadCameras();
    }

    async function editCamera(id) {
      const cam = getCamera(id);
      if (!cam) return;
      const label = prompt('Nama kamera', cam.label ?? '');
      if (label === null) return;
      const url = prompt('RTSP URL', cam.url ?? '');
      if (url === null || !url.trim()) return;
      const frameSkip = prompt('Frame skip', String(cam.frame_skip ?? 2));
      if (frameSkip === null) return;
      const body = new URLSearchParams({
        label: label.trim(), url: url.trim(), frame_skip: frameSkip.trim() || '2',
        x1: String(cam.x1 ?? 0.5), y1: String(cam.y1 ?? 0.0),
        x2: String(cam.x2 ?? 0.5), y2: String(cam.y2 ?? 1.0),
      });
      const res = await fetch(`/cameras/${id}`, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
      const text = await res.text();
      if (!res.ok) { showMessage(text); return; }
      showMessage('Kamera diperbarui.');
      loadCameras();
    }

    async function saveLine(id) {
      const cam = getCamera(id);
      if (!cam) return;
      const body = new URLSearchParams({
        x1: String(cam.x1 ?? 0.5), y1: String(cam.y1 ?? 0.0),
        x2: String(cam.x2 ?? 0.5), y2: String(cam.y2 ?? 1.0),
      });
      const res = await fetch(`/line/${id}`, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
      const text = await res.text();
      if (!res.ok) { showMessage(text); return; }
      showMessage('Garis diperbarui.');
      loadCameras();
    }

    function clamp01(value) { return Math.max(0, Math.min(1, value)); }

    function getOverlayPoint(event, svg) {
      const rect = svg.getBoundingClientRect();
      return { x: event.clientX - rect.left, y: event.clientY - rect.top, w: rect.width, h: rect.height };
    }

    function updateOverlay(id) {
      const cam = getCamera(id);
      const svg = document.getElementById(`overlay-${id}`);
      if (!cam || !svg) return;
      const x1 = cam.x1 * 100, y1 = cam.y1 * 100, x2 = cam.x2 * 100, y2 = cam.y2 * 100;
      const hit = document.getElementById(`line-hit-${id}`);
      const line = document.getElementById(`line-main-${id}`);
      const p1 = document.getElementById(`handle-1-${id}`);
      const p2 = document.getElementById(`handle-2-${id}`);
      [hit, line].forEach(el => {
        if (!el) return;
        el.setAttribute('x1', x1); el.setAttribute('y1', y1);
        el.setAttribute('x2', x2); el.setAttribute('y2', y2);
      });
      if (p1) {
        p1.setAttribute('cx', x1); p1.setAttribute('cy', y1);
        p1.classList.toggle('dragging', dragState.id === id && dragState.mode === 'p1');
      }
      if (p2) {
        p2.setAttribute('cx', x2); p2.setAttribute('cy', y2);
        p2.classList.toggle('dragging', dragState.id === id && dragState.mode === 'p2');
      }
    }

    function beginLineDrag(id, mode, event) {
      const cam = getCamera(id);
      const svg = document.getElementById(`overlay-${id}`);
      if (!cam || !svg) return;
      const p = getOverlayPoint(event, svg);
      dragState.id = id; dragState.mode = mode;
      dragState.start = { x: p.x, y: p.y, w: p.w, h: p.h, x1: cam.x1, y1: cam.y1, x2: cam.x2, y2: cam.y2 };
      event.preventDefault();
      updateOverlay(id);
    }

    document.addEventListener('mousemove', (event) => {
      if (!dragState.id || !dragState.mode) return;
      const cam = getCamera(dragState.id);
      const svg = document.getElementById(`overlay-${dragState.id}`);
      if (!cam || !svg) return;
      const p = getOverlayPoint(event, svg);
      const start = dragState.start;
      if (dragState.mode === 'p1') {
        cam.x1 = clamp01(p.x / p.w); cam.y1 = clamp01(p.y / p.h);
      } else if (dragState.mode === 'p2') {
        cam.x2 = clamp01(p.x / p.w); cam.y2 = clamp01(p.y / p.h);
      } else if (dragState.mode === 'line') {
        const dx = (p.x - start.x) / p.w, dy = (p.y - start.y) / p.h;
        cam.x1 = clamp01(start.x1 + dx); cam.y1 = clamp01(start.y1 + dy);
        cam.x2 = clamp01(start.x2 + dx); cam.y2 = clamp01(start.y2 + dy);
      }
      updateOverlay(dragState.id);
    });

    document.addEventListener('mouseup', async () => {
      if (!dragState.id) return;
      const id = dragState.id;
      dragState.id = null; dragState.mode = null; dragState.start = null;
      updateOverlay(id);
      await saveLine(id);
    });

    // ---- Gallery ----
    function getFilters() {
      return {
        camera: document.getElementById('filter-camera').value,
        direction: document.getElementById('filter-direction').value,
        date: document.getElementById('filter-date').value.replace(/-/g, ''),
      };
    }

    function filteredShots() {
      const f = getFilters();
      return allScreenshots.filter(s => {
        if (f.camera && s.camera !== f.camera) return false;
        if (f.direction && !s.direction.toLowerCase().includes(f.direction)) return false;
        if (f.date && !s.file.includes(f.date)) return false;
        return true;
      });
    }

    function applyFilter() { currentPage = 1; renderGallery(); }

    function clearFilters() {
      document.getElementById('filter-camera').value = '';
      document.getElementById('filter-direction').value = '';
      document.getElementById('filter-date').value = '';
      applyFilter();
    }

    async function deleteShot(file) {
      if (!confirm('Hapus screenshot ini?')) return;
      const res = await fetch(`/screenshots/${encodeURIComponent(file)}`, { method: 'DELETE' });
      if (!res.ok) { showMessage('Gagal menghapus screenshot.'); return; }
      showMessage('Screenshot dihapus.');
      await loadScreenshots();
    }

    async function deleteAllFiltered() {
      const shots = filteredShots();
      if (!shots.length) { showMessage('Tidak ada screenshot untuk dihapus.'); return; }
      if (!confirm(`Hapus ${shots.length} screenshot?`)) return;
      let count = 0;
      for (const s of shots) {
        const res = await fetch(`/screenshots/${encodeURIComponent(s.file)}`, { method: 'DELETE' });
        if (res.ok) count++;
      }
      showMessage(`${count} screenshot dihapus.`);
      await loadScreenshots();
    }

    // ---- Lightbox ----
    function openLightbox(shots, index) {
      lbShots = shots;
      lbIndex = index;
      showLbImage();
      document.getElementById('lightbox').classList.add('open');
    }

    function showLbImage() {
      const shot = lbShots[lbIndex];
      document.getElementById('lightbox-img').src = shot.url;
      document.getElementById('lightbox-meta').textContent =
        shot.camera + '  ·  ' + shot.time + '  ·  ' + shot.direction + '  ·  ' + (lbIndex + 1) + ' / ' + lbShots.length;
      document.getElementById('lb-prev').disabled = lbIndex === 0;
      document.getElementById('lb-next').disabled = lbIndex === lbShots.length - 1;
    }

    function lbNavigate(dir) {
      const next = lbIndex + dir;
      if (next < 0 || next >= lbShots.length) return;
      lbIndex = next;
      showLbImage();
    }

    function closeLightbox() {
      document.getElementById('lightbox').classList.remove('open');
      document.getElementById('lightbox-img').src = '';
    }

    document.getElementById('lightbox').addEventListener('click', function(e) {
      if (e.target === this) closeLightbox();
    });

    document.addEventListener('keydown', function(e) {
      if (!document.getElementById('lightbox').classList.contains('open')) return;
      if (e.key === 'Escape') closeLightbox();
      if (e.key === 'ArrowLeft') lbNavigate(-1);
      if (e.key === 'ArrowRight') lbNavigate(1);
    });

    // ---- Render Gallery ----
    function updateCameraFilter(shots) {
      const sel = document.getElementById('filter-camera');
      const current = sel.value;
      const cameras = [...new Set(shots.map(s => s.camera))].sort();
      sel.innerHTML = '<option value="">Semua Kamera</option>' +
        cameras.map(c => '<option value="' + c + '"' + (c === current ? ' selected' : '') + '>' + c + '</option>').join('');
    }

    function renderGallery() {
      const filtered = filteredShots();
      const total = filtered.length;
      const totalPages = Math.max(1, Math.ceil(total / PAGE_SIZE));
      if (currentPage > totalPages) currentPage = totalPages;
      const start = (currentPage - 1) * PAGE_SIZE;
      const pageShots = filtered.slice(start, start + PAGE_SIZE);

      const countEl = document.getElementById('shot-count');
      countEl.textContent = total === allScreenshots.length
        ? total + ' foto'
        : total + ' dari ' + allScreenshots.length + ' foto';

      const container = document.getElementById('shots');
      container.innerHTML = '';
      if (pageShots.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'empty-state';
        empty.textContent = allScreenshots.length === 0 ? 'Belum ada screenshot' : 'Tidak ada foto yang cocok dengan filter';
        container.appendChild(empty);
      } else {
        pageShots.forEach(function(shot, i) {
          const card = document.createElement('div');
          card.className = 'shot';

          const img = document.createElement('img');
          img.src = shot.url;
          img.alt = shot.file;
          img.loading = 'lazy';
          img.addEventListener('click', function() { openLightbox(pageShots, i); });

          const meta = document.createElement('div');
          meta.className = 'shot-meta';
          const strong = document.createElement('strong');
          strong.textContent = shot.camera;
          const d1 = document.createElement('div');
          d1.textContent = shot.time;
          const d2 = document.createElement('div');
          d2.textContent = shot.direction;
          meta.appendChild(strong);
          meta.appendChild(d1);
          meta.appendChild(d2);

          const actions = document.createElement('div');
          actions.className = 'shot-actions';
          const delBtn = document.createElement('button');
          delBtn.className = 'danger';
          delBtn.textContent = 'Hapus';
          delBtn.addEventListener('click', function() { deleteShot(shot.file); });
          actions.appendChild(delBtn);

          card.appendChild(img);
          card.appendChild(meta);
          card.appendChild(actions);
          container.appendChild(card);
        });
      }
      renderPagination(total, totalPages);
    }

    function renderPagination(total, totalPages) {
      const pag = document.getElementById('pagination');
      if (totalPages <= 1) { pag.innerHTML = ''; return; }
      const delta = 2;
      const pageNums = [];
      for (let i = 1; i <= totalPages; i++) {
        if (i === 1 || i === totalPages || (i >= currentPage - delta && i <= currentPage + delta)) pageNums.push(i);
      }
      let html = '<button class="page-btn" onclick="goPage(' + (currentPage - 1) + ')"' + (currentPage === 1 ? ' disabled' : '') + '>&lsaquo; Prev</button>';
      let prev = 0;
      for (let j = 0; j < pageNums.length; j++) {
        const p = pageNums[j];
        if (prev && p - prev > 1) html += '<span class="page-info">&hellip;</span>';
        html += '<button class="page-btn' + (p === currentPage ? ' active' : '') + '" onclick="goPage(' + p + ')">' + p + '</button>';
        prev = p;
      }
      html += '<button class="page-btn" onclick="goPage(' + (currentPage + 1) + ')"' + (currentPage === totalPages ? ' disabled' : '') + '>Next &rsaquo;</button>';
      html += '<span class="page-info">Hal. ' + currentPage + ' / ' + totalPages + '</span>';
      pag.innerHTML = html;
    }

    function goPage(p) {
      const filtered = filteredShots();
      const totalPages = Math.max(1, Math.ceil(filtered.length / PAGE_SIZE));
      currentPage = Math.max(1, Math.min(p, totalPages));
      renderGallery();
      document.querySelector('.gallery').scrollIntoView({ behavior: 'smooth', block: 'start' });
    }

    // ---- Load ----
    async function loadCameras() {
      const cameras = await fetch('/cameras').then(r => r.json()).catch(() => []);
      cameraState = cameras;
      const grid = document.getElementById('grid');
      grid.innerHTML = '';
      cameras.forEach(cam => {
        const card = document.createElement('div');
        card.className = 'card';
        card.innerHTML = `
          <div class="head">
            <div>
              <div style="font-weight:700">${cam.label}</div>
              <div style="font-size:12px;color:#9ca3af">${cam.url}</div>
            </div>
            <div style="font-size:12px;color:${cam.online ? '#4ade80' : '#f87171'}">${cam.online ? 'online' : 'offline'}</div>
          </div>
          <div class="video-wrap">
            <img class="feed" src="/feed/${cam.id}" alt="">
            <svg class="overlay" id="overlay-${cam.id}" viewBox="0 0 100 100" preserveAspectRatio="none">
              <line id="line-hit-${cam.id}" class="line-hit"
                onmousedown="beginLineDrag('${cam.id}', 'line', event)"></line>
              <line id="line-main-${cam.id}" class="line-main"></line>
              <circle id="handle-1-${cam.id}" class="handle" r="2.4"
                onmousedown="beginLineDrag('${cam.id}', 'p1', event)"></circle>
              <circle id="handle-2-${cam.id}" class="handle" r="2.4"
                onmousedown="beginLineDrag('${cam.id}', 'p2', event)"></circle>
            </svg>
          </div>
          <div class="stats">
            <span>Total <span class="pill">${cam.total}</span></span>
            <span>Right <span class="pill">${cam.right}</span></span>
            <span>Left <span class="pill">${cam.left}</span></span>
          </div>
          <div class="actions">
            <button onclick="editCamera('${cam.id}')">Edit</button>
            <button class="danger" onclick="removeCamera('${cam.id}')">Hapus</button>
          </div>`;
        grid.appendChild(card);
        requestAnimationFrame(() => updateOverlay(cam.id));
      });
    }

    async function loadScreenshots() {
      const screenshots = await fetch('/screenshots').then(r => r.json()).catch(() => []);
      allScreenshots = screenshots;
      updateCameraFilter(screenshots);
      renderGallery();
    }

    loadCameras();
    loadScreenshots();
    setInterval(loadCameras, 2000);
    setInterval(loadScreenshots, 10000);
  </script>
</body>
</html>)HTML";
}

}  // namespace
namespace comvisplus {

HttpServer::HttpServer(const AppConfig& app_config, std::shared_ptr<CameraManager> manager)
    : app_config_(app_config), manager_(std::move(manager)) {}

void HttpServer::run() {
    print_routes();
    const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        throw std::runtime_error("failed to create socket");
    }

    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(app_config_.port);
    address.sin_addr.s_addr = app_config_.host == "0.0.0.0"
        ? htonl(INADDR_ANY)
        : inet_addr(app_config_.host.c_str());

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(server_fd);
        throw std::runtime_error("failed to bind socket");
    }
    if (::listen(server_fd, 16) < 0) {
        ::close(server_fd);
        throw std::runtime_error("failed to listen");
    }

    std::cout << "[web] listening on http://" << app_config_.host << ':' << app_config_.port << '\n';
    accept_loop(server_fd);
    ::close(server_fd);
}

void HttpServer::accept_loop(int server_fd) {
    while (running_) {
        sockaddr_in client_addr {};
        socklen_t client_len = sizeof(client_addr);
        const int client_fd = ::accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "[web] accept failed: " << std::strerror(errno) << '\n';
            continue;
        }
        std::thread(&HttpServer::handle_client, this, client_fd).detach();
    }
}

void HttpServer::handle_client(int client_fd) const {
    std::string request_data;
    char buffer[4096];
    while (true) {
        const ssize_t bytes_read = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (bytes_read <= 0) {
            break;
        }
        request_data.append(buffer, static_cast<std::size_t>(bytes_read));
        const auto header_end = request_data.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            continue;
        }

        std::size_t content_length = 0;
        const auto content_pos = request_data.find("Content-Length:");
        if (content_pos != std::string::npos && content_pos < header_end) {
            const auto line_end = request_data.find("\r\n", content_pos);
            const auto value = trim_copy(request_data.substr(content_pos + 15, line_end - (content_pos + 15)));
            content_length = static_cast<std::size_t>(std::stoul(value));
        }
        if (request_data.size() >= header_end + 4 + content_length) {
            break;
        }
    }

    if (request_data.empty()) {
        ::close(client_fd);
        return;
    }

    const auto header_end = request_data.find("\r\n\r\n");
    const std::string request_head = header_end == std::string::npos ? request_data : request_data.substr(0, header_end);
    const std::string request_body = header_end == std::string::npos ? std::string() : request_data.substr(header_end + 4);

    std::istringstream request_stream(request_head);
    std::string method;
    std::string path;
    std::string version;
    request_stream >> method >> path >> version;

    if (!is_authorized(app_config_, request_head)) {
        const auto response = http_unauthorized();
        ::send(client_fd, response.data(), response.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path == "/") {
        if (method != "GET") {
            const auto response = http_method_not_allowed();
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }
        const auto response = http_ok("text/html; charset=utf-8", root_page());
        ::send(client_fd, response.data(), response.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path == "/cameras") {
        if (method == "POST") {
            const auto values = parse_form_body(request_body);
            const std::string url = trim_copy(values.count("url") != 0 ? values.at("url") : "");
            if (url.empty()) {
                const auto response = http_bad_request("url wajib diisi");
                ::send(client_fd, response.data(), response.size(), 0);
                ::close(client_fd);
                return;
            }

            CameraConfig config;
            config.label = trim_copy(values.count("label") != 0 ? values.at("label") : "");
            config.rtsp_url = url;
            if (values.count("frame_skip") != 0) {
                try {
                    config.frame_skip = std::max(1, std::stoi(values.at("frame_skip")));
                } catch (...) {
                    config.frame_skip = 2;
                }
            }

            const std::string camera_id = manager_->add_camera(config);
            manager_->save_to_disk();
            const auto response = http_json(
                201,
                "Created",
                "{\"id\":\"" + json_escape(camera_id) + "\"}"
            );
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }

        if (method != "GET") {
            const auto response = http_method_not_allowed();
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }

        std::ostringstream body;
        body << '[';
        bool first = true;
        for (const auto& runtime : manager_->list_runtimes()) {
            std::lock_guard<std::mutex> lock(runtime->mutex);
            if (!first) {
                body << ',';
            }
            first = false;
            body
                << '{'
                << "\"id\":\"" << json_escape(runtime->config.id) << "\","
                << "\"label\":\"" << json_escape(runtime->config.label) << "\","
                << "\"url\":\"" << json_escape(runtime->config.rtsp_url) << "\","
                << "\"online\":" << (runtime->online ? "true" : "false") << ','
                << "\"reconnecting\":" << (runtime->reconnecting ? "true" : "false") << ','
                << "\"error\":\"" << json_escape(runtime->last_error) << "\","
                << "\"frame_skip\":" << runtime->config.frame_skip << ','
                << "\"x1\":" << runtime->config.line.x1 << ','
                << "\"y1\":" << runtime->config.line.y1 << ','
                << "\"x2\":" << runtime->config.line.x2 << ','
                << "\"y2\":" << runtime->config.line.y2 << ','
                << "\"total\":" << runtime->stats.total << ','
                << "\"right\":" << runtime->stats.right << ','
                << "\"left\":" << runtime->stats.left
                << '}';
        }
        body << ']';
        const auto response = http_ok("application/json", body.str());
        ::send(client_fd, response.data(), response.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path.rfind("/cameras/", 0) == 0) {
        if (method == "POST") {
            const std::string camera_id = path.substr(std::strlen("/cameras/"));
            const auto existing = manager_->get(camera_id);
            if (existing == nullptr) {
                const auto response = http_not_found();
                ::send(client_fd, response.data(), response.size(), 0);
                ::close(client_fd);
                return;
            }

            CameraConfig config;
            {
                std::lock_guard<std::mutex> lock(existing->mutex);
                config = existing->config;
            }

            const auto values = parse_form_body(request_body);
            const std::string url = trim_copy(values.count("url") != 0 ? values.at("url") : config.rtsp_url);
            if (url.empty()) {
                const auto response = http_bad_request("url wajib diisi");
                ::send(client_fd, response.data(), response.size(), 0);
                ::close(client_fd);
                return;
            }

            config.id = camera_id;
            config.label = trim_copy(values.count("label") != 0 ? values.at("label") : config.label);
            config.rtsp_url = url;
            config.frame_skip = std::max(1, parse_int_or_default(values, "frame_skip", config.frame_skip));
            config.line.x1 = std::clamp(parse_float_or_default(values, "x1", config.line.x1), 0.0F, 1.0F);
            config.line.y1 = std::clamp(parse_float_or_default(values, "y1", config.line.y1), 0.0F, 1.0F);
            config.line.x2 = std::clamp(parse_float_or_default(values, "x2", config.line.x2), 0.0F, 1.0F);
            config.line.y2 = std::clamp(parse_float_or_default(values, "y2", config.line.y2), 0.0F, 1.0F);

            const bool ok = manager_->update_camera(camera_id, config);
            if (ok) {
                manager_->save_to_disk();
                const auto response = http_json(200, "OK", "{\"ok\":true}");
                ::send(client_fd, response.data(), response.size(), 0);
            } else {
                const auto response = http_not_found();
                ::send(client_fd, response.data(), response.size(), 0);
            }
            ::close(client_fd);
            return;
        }

        if (method != "DELETE") {
            const auto response = http_method_not_allowed();
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }

        const std::string camera_id = path.substr(std::strlen("/cameras/"));
        const bool ok = manager_->remove_camera(camera_id);
        if (ok) {
            manager_->save_to_disk();
            const auto response = http_json(200, "OK", "{\"ok\":true}");
            ::send(client_fd, response.data(), response.size(), 0);
        } else {
            const auto response = http_not_found();
            ::send(client_fd, response.data(), response.size(), 0);
        }
        ::close(client_fd);
        return;
    }

    if (path.rfind("/line/", 0) == 0) {
        if (method != "POST") {
            const auto response = http_method_not_allowed();
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }

        const std::string camera_id = path.substr(std::strlen("/line/"));
        const auto values = parse_form_body(request_body);
        LineConfig line;
        line.x1 = std::clamp(parse_float_or_default(values, "x1", 0.5F), 0.0F, 1.0F);
        line.y1 = std::clamp(parse_float_or_default(values, "y1", 0.0F), 0.0F, 1.0F);
        line.x2 = std::clamp(parse_float_or_default(values, "x2", 0.5F), 0.0F, 1.0F);
        line.y2 = std::clamp(parse_float_or_default(values, "y2", 1.0F), 0.0F, 1.0F);
        const bool ok = manager_->update_line(camera_id, line);
        if (ok) {
            manager_->save_to_disk();
            const auto response = http_json(200, "OK", "{\"ok\":true}");
            ::send(client_fd, response.data(), response.size(), 0);
        } else {
            const auto response = http_not_found();
            ::send(client_fd, response.data(), response.size(), 0);
        }
        ::close(client_fd);
        return;
    }

    if (path == "/screenshots") {
        if (method != "GET") {
            const auto response = http_method_not_allowed();
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }

        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(app_config_.screenshots_dir, ec);

        struct ShotItem {
            std::string file;
            std::string camera;
            std::string time;
            std::string direction;
            fs::file_time_type mtime;
        };

        std::vector<ShotItem> items;
        for (const auto& entry : fs::directory_iterator(app_config_.screenshots_dir, ec)) {
            if (ec || !entry.is_regular_file()) {
                continue;
            }
            const std::string filename = entry.path().filename().string();
            const auto first = filename.find('_');
            const auto second = first == std::string::npos ? std::string::npos : filename.find('_', first + 1);
            const auto third = second == std::string::npos ? std::string::npos : filename.find('_', second + 1);
            const auto fourth = third == std::string::npos ? std::string::npos : filename.find('_', third + 1);
            const auto dot = filename.rfind('.');

            ShotItem item;
            item.file = filename;
            item.camera = first == std::string::npos ? filename : replace_underscores(filename.substr(0, first));
            const std::string ymd = (first == std::string::npos || second == std::string::npos) ? "" : filename.substr(first + 1, second - first - 1);
            const std::string hms = (second == std::string::npos || third == std::string::npos) ? "" : filename.substr(second + 1, third - second - 1);
            const std::string millis = (third == std::string::npos || fourth == std::string::npos) ? "" : filename.substr(third + 1, fourth - third - 1);
            item.time = format_capture_time(ymd, hms, millis);
            item.direction = (fourth == std::string::npos)
                ? ""
                : replace_underscores(filename.substr(fourth + 1, dot == std::string::npos ? std::string::npos : dot - fourth - 1));
            item.mtime = entry.last_write_time(ec);
            items.push_back(item);
        }

        std::sort(items.begin(), items.end(), [](const ShotItem& lhs, const ShotItem& rhs) {
            return lhs.mtime > rhs.mtime;
        });

        std::ostringstream body;
        body << '[';
        bool first = true;
        for (const auto& item : items) {
            if (!first) {
                body << ',';
            }
            first = false;
            body
                << '{'
                << "\"file\":\"" << json_escape(item.file) << "\","
                << "\"camera\":\"" << json_escape(item.camera) << "\","
                << "\"time\":\"" << json_escape(item.time) << "\","
                << "\"direction\":\"" << json_escape(item.direction) << "\","
                << "\"url\":\"/screenshots/" << json_escape(item.file) << "\""
                << '}';
        }
        body << ']';
        const auto response = http_ok("application/json", body.str());
        ::send(client_fd, response.data(), response.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path.rfind("/screenshots/", 0) == 0) {
        if (method != "GET" && method != "DELETE") {
            const auto response = http_method_not_allowed();
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }

        const std::string filename = path.substr(std::strlen("/screenshots/"));
        if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos) {
            const auto response = http_bad_request("invalid screenshot path");
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }

        namespace fs = std::filesystem;
        const fs::path image_path = fs::path(app_config_.screenshots_dir) / filename;
        if (method == "DELETE") {
            std::error_code ec;
            const bool removed = fs::remove(image_path, ec);
            if (!removed || ec) {
                const auto response = http_not_found();
                ::send(client_fd, response.data(), response.size(), 0);
            } else {
                const auto response = http_json(200, "OK", "{\"ok\":true}");
                ::send(client_fd, response.data(), response.size(), 0);
            }
            ::close(client_fd);
            return;
        }

        std::ifstream input(image_path, std::ios::binary);
        if (!input.good()) {
            const auto response = http_not_found();
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }

        std::ostringstream buffer_stream;
        buffer_stream << input.rdbuf();
        const std::string body = buffer_stream.str();
        std::ostringstream header;
        header
            << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: " << guess_content_type(filename) << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Cache-Control: no-cache\r\n"
            << "Connection: close\r\n\r\n";
        const std::string header_str = header.str();
        ::send(client_fd, header_str.data(), header_str.size(), 0);
        ::send(client_fd, body.data(), body.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path.rfind("/stats/", 0) == 0) {
        if (method != "GET") {
            const auto response = http_method_not_allowed();
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }
        const std::string camera_id = path.substr(std::strlen("/stats/"));
        const auto runtime = manager_->get(camera_id);
        if (runtime == nullptr) {
            const auto response = http_not_found();
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }

        std::ostringstream body;
        {
            std::lock_guard<std::mutex> lock(runtime->mutex);
            body
                << '{'
                << "\"id\":\"" << json_escape(runtime->config.id) << "\","
                << "\"online\":" << (runtime->online ? "true" : "false") << ','
                << "\"reconnecting\":" << (runtime->reconnecting ? "true" : "false") << ','
                << "\"total\":" << runtime->stats.total << ','
                << "\"right\":" << runtime->stats.right << ','
                << "\"left\":" << runtime->stats.left
                << '}';
        }
        const auto response = http_ok("application/json", body.str());
        ::send(client_fd, response.data(), response.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path.rfind("/feed/", 0) == 0) {
        if (method != "GET") {
            const auto response = http_method_not_allowed();
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }
        const std::string camera_id = path.substr(std::strlen("/feed/"));
        const auto runtime = manager_->get(camera_id);
        if (runtime == nullptr) {
            const auto response = http_not_found();
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }

        const std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
        ::send(client_fd, header.data(), header.size(), 0);

        std::uint64_t last_sequence = 0;
        while (runtime->running) {
            std::vector<unsigned char> jpeg;
            std::uint64_t sequence = 0;
            {
                std::lock_guard<std::mutex> lock(runtime->mutex);
                sequence = runtime->latest_frame.sequence;
                if (sequence != last_sequence) {
                    jpeg = runtime->latest_frame.jpeg;
                    last_sequence = sequence;
                }
            }

            if (!jpeg.empty()) {
                std::ostringstream part_header;
                part_header
                    << "--frame\r\n"
                    << "Content-Type: image/jpeg\r\n"
                    << "Content-Length: " << jpeg.size() << "\r\n\r\n";
                const std::string part_header_str = part_header.str();
                if (::send(client_fd, part_header_str.data(), part_header_str.size(), 0) <= 0) {
                    break;
                }
                if (::send(client_fd, reinterpret_cast<const char*>(jpeg.data()), jpeg.size(), 0) <= 0) {
                    break;
                }
                if (::send(client_fd, "\r\n", 2, 0) <= 0) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        ::close(client_fd);
        return;
    }

    const auto response = http_not_found();
    ::send(client_fd, response.data(), response.size(), 0);
    ::close(client_fd);
}

void HttpServer::print_routes() const {
    std::cout << "[routes] GET /\n";
    std::cout << "[routes] GET /cameras\n";
    std::cout << "[routes] POST /cameras\n";
    std::cout << "[routes] POST /cameras/{id}\n";
    std::cout << "[routes] DELETE /cameras/{id}\n";
    std::cout << "[routes] POST /line/{id}\n";
    std::cout << "[routes] GET /screenshots\n";
    std::cout << "[routes] GET /screenshots/{file}\n";
    std::cout << "[routes] DELETE /screenshots/{file}\n";
    std::cout << "[routes] GET /feed/{id}\n";
    std::cout << "[routes] GET /stats/{id}\n";
}

}  // namespace comvisplus
