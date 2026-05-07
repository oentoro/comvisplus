#!/usr/bin/env bash
set -euo pipefail

OPENVINO_DEFAULT="/opt/intel/openvino_2025.4.0"

if [[ "${EUID}" -ne 0 ]]; then
  SUDO="sudo"
else
  SUDO=""
fi

echo "[info] Updating apt package index..."
${SUDO} apt update

echo "[info] Installing base build dependencies..."
${SUDO} apt install -y \
  build-essential \
  cmake \
  pkg-config \
  git \
  curl \
  wget \
  libopencv-dev

echo
echo "[ok] Base Debian packages installed."
echo

if pkg-config --exists opencv4; then
  echo "[ok] OpenCV detected:"
  pkg-config --modversion opencv4
else
  echo "[warn] OpenCV pkg-config entry 'opencv4' masih belum terdeteksi."
fi

echo
if [[ -f "${OPENVINO_DEFAULT}/setupvars.sh" ]]; then
  echo "[ok] OpenVINO appears to be installed at:"
  echo "  ${OPENVINO_DEFAULT}"
  echo
  echo "To enable it in the current shell:"
  echo "  source ${OPENVINO_DEFAULT}/setupvars.sh"
else
  cat <<EOF
[warn] OpenVINO belum ditemukan di:
  ${OPENVINO_DEFAULT}

Install OpenVINO runtime archive resmi dulu, lalu source environment:

  source ${OPENVINO_DEFAULT}/setupvars.sh

Setelah itu lanjutkan dengan:

  ./build_debian.sh
  ./run_debian.sh

RTSP_URL opsional dan hanya dipakai untuk bootstrap kamera pertama.
EOF
fi
