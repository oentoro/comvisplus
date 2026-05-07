# Comvisplus Native

`Comvisplus Native` adalah baseline people counter CCTV berbasis:
- `C++`
- `OpenCV`
- `OpenVINO`
- target `Debian` untuk CPU Intel lama

Fokus proyek ini:
- ambil source dari `RTSP`
- jalankan inferensi lokal via `OpenVINO`
- tampilkan hasil di web
- kelola kamera langsung dari browser

## Fitur Saat Ini

- server web lokal
- daftar kamera multi-stream
- feed `MJPEG` per kamera
- stats counter per kamera
- tambah kamera dari web
- edit kamera dari web
- hapus kamera dari web
- edit garis hitung langsung dengan drag di overlay video
- simpan/muat konfigurasi ke [cameras.json](/Users/oentoro/Projects/comvisplus/cameras.json)

## Struktur Proyek

- [cpp/CMakeLists.txt](/Users/oentoro/Projects/comvisplus/cpp/CMakeLists.txt)
  Entry point build CMake.
- [cpp/src/main.cpp](/Users/oentoro/Projects/comvisplus/cpp/src/main.cpp)
  Bootstrap aplikasi.
- [cpp/src/counter_engine.cpp](/Users/oentoro/Projects/comvisplus/cpp/src/counter_engine.cpp)
  Loop RTSP, overlay, dan counting baseline.
- [cpp/src/openvino_backend.cpp](/Users/oentoro/Projects/comvisplus/cpp/src/openvino_backend.cpp)
  Loader dan inferensi `OpenVINO`.
- [cpp/src/http_server.cpp](/Users/oentoro/Projects/comvisplus/cpp/src/http_server.cpp)
  HTTP server dan UI web.
- [build_debian.sh](/Users/oentoro/Projects/comvisplus/build_debian.sh)
  Configure + build untuk Debian.
- [run_debian.sh](/Users/oentoro/Projects/comvisplus/run_debian.sh)
  Jalankan server.
- [setup_debian.sh](/Users/oentoro/Projects/comvisplus/setup_debian.sh)
  Install dependency Debian dasar.

## Setup Debian

Jalankan:

```bash
chmod +x setup_debian.sh build_debian.sh run_debian.sh
./setup_debian.sh
```

Script ini menginstall dependency dasar:

```bash
build-essential
cmake
pkg-config
git
curl
wget
libopencv-dev
```

## Install OpenVINO

Install `OpenVINO Runtime` dari archive resmi, lalu aktifkan environment:

```bash
source /opt/intel/openvino_2025.4.0/setupvars.sh
```

Kalau lokasi instalasi berbeda, sesuaikan path-nya.

## Export Model

Export model dari Ultralytics:

```bash
yolo export model=yolov8n.pt format=openvino imgsz=320
```

Untuk hardware lama, tetap disarankan:
- `yolov8n`
- `imgsz=320`
- `frame_skip=2` atau lebih

Binary bisa menerima:
- folder hasil export, mis. `yolov8n_openvino_model`
- atau file `.xml` langsung

## Build

```bash
source /opt/intel/openvino_2025.4.0/setupvars.sh
./build_debian.sh
```

## Run

```bash
export RTSP_URL='rtsp://user:password@camera/stream'
./run_debian.sh
```

Override opsional:

```bash
export HOST=0.0.0.0
export PORT=5000
export MODEL_PATH=./yolov8n_openvino_model
export RTSP_URL='rtsp://user:password@camera/stream'
./run_debian.sh
```

Setelah jalan, buka:

```text
http://IP_DEBIAN:5000
```

## Catatan Status

Yang sudah ada:
- manajemen kamera dari web
- persistensi `cameras.json`
- drag line editor di browser
- restart worker saat RTSP kamera diubah

Yang masih baseline / belum final:
- parser output model `OpenVINO` masih perlu validasi di mesin Debian target
- auth/login belum ada
- screenshot gallery belum ada
- SSE belum ada
- compile test belum diverifikasi di environment Debian dari sesi ini
