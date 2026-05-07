# Comvisplus Native

Versi proyek ini sekarang difokuskan ke:
- `C++`
- `OpenVINO`
- `OpenCV`
- target Debian untuk CPU Intel lama

## Build di Debian

Install dependency dasar:

```bash
chmod +x setup_debian.sh
./setup_debian.sh
```

Install OpenVINO runtime dari archive resmi, lalu source environment:

```bash
source /opt/intel/openvino_2025.4.0/setupvars.sh
```

Build:

```bash
chmod +x build_debian.sh
./build_debian.sh
```

## Jalankan

```bash
export RTSP_URL='rtsp://user:password@camera/stream'
chmod +x run_debian.sh
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

## Export model dari Ultralytics

```bash
yolo export model=yolov8n.pt format=openvino imgsz=320
```

Kamu bisa memberi:
- folder hasil export, mis. `yolov8n_openvino_model`
- atau file `.xml` langsung
