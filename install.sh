#!/bin/bash
set -e

echo "======================================"
echo "  Install People Counter Dependencies"
echo "======================================"

# Aktifkan venv
source "$(dirname "$0")/venv/bin/activate"

# Install packages
pip install --upgrade pip -q
pip install -r "$(dirname "$0")/requirements.txt"

echo ""
echo "✓ Instalasi selesai!"
echo ""
echo "Cara pakai:"
echo "  source venv/bin/activate"
echo "  python people_counter.py video.mp4"
echo "  python people_counter.py video.mp4 --output hasil.mp4"
echo "  python people_counter.py 0                        # webcam"
echo "  python people_counter.py rtsp://...               # RTSP CCTV"
echo ""
echo "Opsi tambahan:"
echo "  --model yolov8s.pt      # lebih akurat dari nano"
echo "  --line-pos 0.4          # geser garis ke kiri/atas"
echo "  --line-dir horizontal   # garis horizontal (orang jalan vertikal)"
echo ""
