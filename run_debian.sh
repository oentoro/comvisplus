#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BINARY_PATH="${BUILD_DIR}/comvisplus_native"

OPENVINO_DEFAULT="/opt/intel/openvino_2025.4.0"
DEFAULT_MODEL_DIR="${ROOT_DIR}/yolov8n_openvino_model"
DEFAULT_HOST="0.0.0.0"
DEFAULT_PORT="5000"

source_openvino_env() {
  local script_path="$1"
  local restore_nounset=0

  if [[ $- == *u* ]]; then
    restore_nounset=1
    set +u
  fi

  # shellcheck disable=SC1090
  source "${script_path}"

  if [[ "${restore_nounset}" -eq 1 ]]; then
    set -u
  fi
}

MODEL_PATH="${MODEL_PATH:-${DEFAULT_MODEL_DIR}}"
HOST="${HOST:-${DEFAULT_HOST}}"
PORT="${PORT:-${DEFAULT_PORT}}"

if [[ -f "${OPENVINO_DEFAULT}/setupvars.sh" ]]; then
  source_openvino_env "${OPENVINO_DEFAULT}/setupvars.sh"
  echo "[info] OpenVINO environment loaded from ${OPENVINO_DEFAULT}"
else
  echo "[warn] ${OPENVINO_DEFAULT}/setupvars.sh tidak ditemukan."
  echo "[warn] Jika OpenVINO ada di lokasi lain, source manual dulu sebelum menjalankan script ini."
fi

if [[ ! -x "${BINARY_PATH}" ]]; then
  echo "[error] Binary belum ada: ${BINARY_PATH}"
  echo "Build dulu dengan:"
  echo "  ./build_debian.sh"
  exit 1
fi

if [[ ! -d "${MODEL_PATH}" && ! -f "${MODEL_PATH}" ]]; then
  echo "[error] Model OpenVINO tidak ditemukan di: ${MODEL_PATH}"
  echo "Export dulu modelnya, misalnya:"
  echo "  yolo export model=yolov8n.pt format=openvino imgsz=320"
  exit 1
fi

echo "[info] Starting server..."
echo "[info] Host  : ${HOST}"
echo "[info] Port  : ${PORT}"
echo "[info] Model : ${MODEL_PATH}"

if [[ -n "${RTSP_URL:-}" ]]; then
  echo "[info] RTSP bootstrap : ${RTSP_URL}"
  echo "[info] RTSP_URL hanya dipakai jika cameras.json masih kosong."
else
  echo "[info] RTSP bootstrap : <tidak diset>"
  echo "[info] Kamera akan dimuat dari cameras.json atau ditambahkan lewat web."
fi

exec "${BINARY_PATH}" --host "${HOST}" --port "${PORT}" --model "${MODEL_PATH}"
