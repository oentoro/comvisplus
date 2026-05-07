# C++ + OpenVINO Scaffold

Ini scaffold proyek baru untuk target:
- Debian
- Intel CPU lama
- source RTSP
- hasil bisa diakses via web

## Struktur

- `CMakeLists.txt`
  Build entry point.
- `include/types.hpp`
  Shared runtime and camera data model.
- `include/app_config.hpp`
  CLI and environment configuration.
- `include/counter_engine.hpp`
  RTSP capture + OpenVINO inference + tracking abstraction.
- `include/camera_manager.hpp`
  Lifecycle manager untuk banyak kamera.
- `include/http_server.hpp`
  Web layer abstraction.
- `src/main.cpp`
  Bootstrap aplikasi.

## Status

Yang sudah ada:
- struktur proyek CMake
- pemisahan modul runtime
- model konfigurasi untuk kamera, garis hitung, dan stats
- worker thread per kamera
- web server socket minimal
- endpoint `GET /`, `GET /cameras`, `GET /feed/{id}`, `GET /stats/{id}`
- RTSP attempt via OpenCV dengan placeholder frame saat source gagal
- backend `OpenVINO` terpisah dengan loader model IR
- baseline postprocess deteksi dan tracking centroid ringan

Yang belum diimplementasikan:
- auth/login nyata
- login/session nyata
- tambah/hapus kamera via HTTP
- persistensi `cameras.json`

## Target implementasi berikutnya

1. Tambah HTTP server ringan.
   Saat ini sudah ada socket HTTP minimal, tapi belum ada routing mutasi.
2. Load model OpenVINO IR.
   Baseline loader sudah ada, tapi masih perlu validasi output model nyata di Debian.
3. Tambah endpoint mutasi kamera dan line config.
4. Tambah auth/login dan persistensi.
5. Tambah screenshot dan SSE.

## Build dependency di Debian

Minimal:
- `cmake`
- `g++`
- `libopencv-dev`
- `openvino` runtime/dev package

Contoh:

```bash
export RTSP_URL='rtsp://user:password@camera/stream'
mkdir -p build
cd build
cmake ../cpp
cmake --build . -j
./comvisplus_native --host 0.0.0.0 --port 5000
```

Contoh export model dari Ultralytics:

```bash
yolo export model=yolov8n.pt format=openvino imgsz=320
```

Lalu jalankan binary dengan:
- `--model yolov8n_openvino_model`
- atau path langsung ke file `.xml`
