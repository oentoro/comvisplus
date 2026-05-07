#!/usr/bin/env bash
set -euo pipefail

OPENVINO_MAJOR="${OPENVINO_MAJOR:-2025}"
OPENVINO_VERSION="${OPENVINO_VERSION:-2025.4.0}"
OPENVINO_BUILD="${OPENVINO_BUILD:-20398.8fdad55727d}"
INSTALL_BASE="${INSTALL_BASE:-/opt/intel}"
INSTALL_DIR="${INSTALL_BASE}/openvino_${OPENVINO_VERSION}"
SYMLINK_PATH="${INSTALL_BASE}/openvino_${OPENVINO_MAJOR}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-${HOME}/Downloads}"
LINUX_FLAVOR="${LINUX_FLAVOR:-}"

if [[ "${EUID}" -ne 0 ]]; then
  SUDO="sudo"
else
  SUDO=""
fi

ARCH="$(dpkg --print-architecture 2>/dev/null || uname -m)"

case "${ARCH}" in
  amd64|x86_64)
    if [[ -z "${LINUX_FLAVOR}" ]]; then
      LINUX_FLAVOR="ubuntu22"
    fi
    ARCHIVE_NAME="openvino_toolkit_${LINUX_FLAVOR}_${OPENVINO_VERSION}.${OPENVINO_BUILD}_x86_64"
    ;;
  arm64|aarch64)
    ARCHIVE_NAME="openvino_toolkit_ubuntu20_${OPENVINO_VERSION}.${OPENVINO_BUILD}_arm64"
    ;;
  armhf)
    ARCHIVE_NAME="openvino_toolkit_debian10_${OPENVINO_VERSION}.${OPENVINO_BUILD}_armhf"
    ;;
  *)
    echo "[error] Arsitektur tidak didukung otomatis: ${ARCH}"
    exit 1
    ;;
esac

ARCHIVE_FILE="openvino_${OPENVINO_VERSION}.tgz"
ARCHIVE_URL="https://storage.openvinotoolkit.org/repositories/openvino/packages/2025.4/linux/${ARCHIVE_NAME}.tgz"

echo "[info] OpenVINO version : ${OPENVINO_VERSION}"
echo "[info] Architecture     : ${ARCH}"
if [[ -n "${LINUX_FLAVOR}" ]]; then
  echo "[info] Linux flavor     : ${LINUX_FLAVOR}"
fi
echo "[info] Archive URL      : ${ARCHIVE_URL}"
echo "[info] Install dir      : ${INSTALL_DIR}"

if ! command -v curl >/dev/null 2>&1; then
  echo "[error] curl belum terpasang. Jalankan ./setup_debian.sh dulu."
  exit 1
fi

if ! command -v tar >/dev/null 2>&1; then
  echo "[error] tar belum terpasang."
  exit 1
fi

mkdir -p "${DOWNLOAD_DIR}"
cd "${DOWNLOAD_DIR}"

download_archive() {
  echo "[info] Downloading OpenVINO archive..."
  rm -f "${ARCHIVE_FILE}"
  curl -L "${ARCHIVE_URL}" --output "${ARCHIVE_FILE}"
}

archive_matches_target() {
  tar -tf "${ARCHIVE_FILE}" 2>/dev/null | grep -q "^${ARCHIVE_NAME}/"
}

if [[ ! -f "${ARCHIVE_FILE}" ]]; then
  download_archive
else
  echo "[info] Using existing archive ${DOWNLOAD_DIR}/${ARCHIVE_FILE}"
  if ! archive_matches_target; then
    echo "[warn] Existing archive does not match expected variant: ${ARCHIVE_NAME}"
    echo "[warn] Re-downloading the correct archive."
    download_archive
  fi
fi

if ! archive_matches_target; then
  echo "[error] Archive content still does not match expected variant: ${ARCHIVE_NAME}"
  exit 1
fi

if [[ -d "${ARCHIVE_NAME}" ]]; then
  echo "[info] Removing existing extracted directory ${DOWNLOAD_DIR}/${ARCHIVE_NAME}"
  rm -rf "${ARCHIVE_NAME}"
fi

echo "[info] Extracting archive..."
tar -xf "${ARCHIVE_FILE}"

if [[ ! -d "${ARCHIVE_NAME}" ]]; then
  echo "[error] Folder hasil extract tidak ditemukan: ${ARCHIVE_NAME}"
  exit 1
fi

${SUDO} mkdir -p "${INSTALL_BASE}"

if [[ -d "${INSTALL_DIR}" ]]; then
  echo "[warn] ${INSTALL_DIR} sudah ada. Folder itu akan diganti."
  ${SUDO} rm -rf "${INSTALL_DIR}"
fi

echo "[info] Moving extracted runtime to ${INSTALL_DIR} ..."
${SUDO} mv "${ARCHIVE_NAME}" "${INSTALL_DIR}"

DEPENDENCY_SCRIPT="${INSTALL_DIR}/install_dependencies/install_openvino_dependencies.sh"

if [[ -x "${DEPENDENCY_SCRIPT}" ]]; then
  echo "[info] Installing OpenVINO system dependencies..."
  if ! ${SUDO} -E "${DEPENDENCY_SCRIPT}"; then
    echo "[warn] Dependency installer bawaan OpenVINO gagal dijalankan."
    echo "[warn] Ini umum terjadi di Debian 13 karena script upstream belum mengenali distro itu."
    echo "[warn] Instalasi runtime tetap dilanjutkan."
    echo "[warn] Jika saat build/run ada library yang kurang, install manual lewat apt lalu coba lagi."
  fi
else
  echo "[warn] Dependency installer OpenVINO tidak ditemukan di ${DEPENDENCY_SCRIPT}."
fi

if [[ -L "${SYMLINK_PATH}" || -e "${SYMLINK_PATH}" ]]; then
  ${SUDO} rm -rf "${SYMLINK_PATH}"
fi
${SUDO} ln -s "${INSTALL_DIR}" "${SYMLINK_PATH}"

cat <<EOF

[ok] OpenVINO installed.

Enable it in the current shell:

  source ${INSTALL_DIR}/setupvars.sh

Or use the stable symlink:

  source ${SYMLINK_PATH}/setupvars.sh

Next steps:

  ./build_debian.sh
  ./run_debian.sh

Optional bootstrap camera:

  export RTSP_URL='rtsp://user:password@camera/stream'
  ./run_debian.sh

EOF
