#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${ROOT_DIR}/.venv-export"
MODEL_PATH="${MODEL_PATH:-${ROOT_DIR}/yolov8n.pt}"
IMG_SIZE="${IMG_SIZE:-320}"
PYTHON_BIN="${PYTHON_BIN:-python3}"

if ! command -v "${PYTHON_BIN}" >/dev/null 2>&1; then
  echo "[error] ${PYTHON_BIN} tidak ditemukan."
  echo "Install dulu Python 3:"
  echo "  sudo apt install -y python3 python3-pip python3-venv"
  exit 1
fi

if [[ ! -f "${MODEL_PATH}" ]]; then
  echo "[error] File model tidak ditemukan: ${MODEL_PATH}"
  exit 1
fi

if [[ ! -d "${VENV_DIR}" ]]; then
  echo "[info] Creating export virtualenv at ${VENV_DIR} ..."
  "${PYTHON_BIN}" -m venv "${VENV_DIR}"
fi

# shellcheck disable=SC1091
source "${VENV_DIR}/bin/activate"

echo "[info] Installing export dependencies..."
python -m pip install --upgrade pip
python -m pip install ultralytics

echo "[info] Exporting ${MODEL_PATH} to OpenVINO with imgsz=${IMG_SIZE} ..."
python - <<PY
from ultralytics import YOLO

model = YOLO(r"${MODEL_PATH}")
model.export(format="openvino", imgsz=${IMG_SIZE})
PY

echo
echo "[ok] Export selesai."
echo "[info] Output biasanya ada di folder:"
echo "  ${ROOT_DIR}/$(basename "${MODEL_PATH}" .pt)_openvino_model"
