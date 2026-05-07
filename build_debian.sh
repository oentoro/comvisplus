#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
CPP_DIR="${ROOT_DIR}/cpp"

OPENVINO_DEFAULT="/opt/intel/openvino_2025.4.0"
OPENVINO_DIR_CMAKE_DEFAULT="${OPENVINO_DEFAULT}/runtime/cmake"

if [[ ! -d "${CPP_DIR}" ]]; then
  echo "[error] Folder cpp/ tidak ditemukan."
  exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "[error] cmake belum terpasang. Install dulu:"
  echo "  sudo apt update && sudo apt install -y cmake build-essential pkg-config libopencv-dev"
  exit 1
fi

if ! command -v pkg-config >/dev/null 2>&1; then
  echo "[error] pkg-config belum terpasang."
  exit 1
fi

if ! pkg-config --exists opencv4; then
  echo "[error] OpenCV pkg-config 'opencv4' tidak ditemukan."
  echo "Install dulu:"
  echo "  sudo apt update && sudo apt install -y libopencv-dev"
  exit 1
fi

if [[ -f "${OPENVINO_DEFAULT}/setupvars.sh" ]]; then
  # shellcheck disable=SC1091
  source "${OPENVINO_DEFAULT}/setupvars.sh"
  echo "[info] OpenVINO environment loaded from ${OPENVINO_DEFAULT}"
else
  echo "[warn] ${OPENVINO_DEFAULT}/setupvars.sh tidak ditemukan."
  echo "[warn] Jika OpenVINO ada di lokasi lain, source manual dulu sebelum menjalankan script ini."
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

CMAKE_ARGS=("${CPP_DIR}")

if [[ -d "${OPENVINO_DIR_CMAKE_DEFAULT}" ]]; then
  CMAKE_ARGS+=("-DOpenVINO_DIR=${OPENVINO_DIR_CMAKE_DEFAULT}")
fi

echo "[info] Configuring project..."
cmake "${CMAKE_ARGS[@]}"

echo "[info] Building project..."
cmake --build . -j"$(nproc)"

cat <<EOF

[ok] Build selesai.

Jalankan dengan contoh:

  export RTSP_URL='rtsp://user:password@camera/stream'
  ${BUILD_DIR}/comvisplus_native --host 0.0.0.0 --port 5000 --model ${ROOT_DIR}/yolov8n_openvino_model

Kalau model OpenVINO kamu berupa file XML langsung:

  ${BUILD_DIR}/comvisplus_native --model /path/to/model.xml

EOF
