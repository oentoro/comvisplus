"""
People Counter - menghitung orang yang lewat dari video CCTV
Menggunakan YOLOv8 untuk deteksi dan ByteTrack untuk tracking
"""

import argparse
import csv
import os
import threading
import time
from collections import defaultdict
from datetime import datetime
from pathlib import Path

import cv2
import numpy as np
from ultralytics import YOLO


# ─── Konfigurasi default ───────────────────────────────────────────────────────
DEFAULT_MODEL = "yolov8n.pt"   # nano=cepat, bisa ganti: yolov8s/m/l/x.pt
DEFAULT_LINE_POS = 0.5         # posisi garis 0.0–1.0 (tengah frame)
DEFAULT_LINE_DIR = "vertical"  # "vertical" atau "horizontal"
PERSON_CLASS_ID = 0            # COCO class 0 = person
CONF_THRESHOLD = 0.3
MIN_TRACK_FRAMES = 3           # frame minimum sebelum track dihitung valid


class CountingLine:
    """Garis virtual untuk mendeteksi crossing."""

    def __init__(self, position: float, direction: str, frame_w: int, frame_h: int):
        self.direction = direction
        self.frame_w = frame_w
        self.frame_h = frame_h

        if direction == "vertical":
            self.x = int(frame_w * position)
        else:
            self.y = int(frame_h * position)

    def draw(self, frame: np.ndarray, state: str = "normal") -> None:
        # normal=kuning, hover=oranye, drag=putih+tebal
        color_map = {
            "normal": (0, 255, 255),
            "hover":  (0, 165, 255),
            "drag":   (255, 255, 255),
        }
        color = color_map.get(state, color_map["normal"])
        thickness = 4 if state == "drag" else 2
        if self.direction == "vertical":
            cv2.line(frame, (self.x, 0), (self.x, self.frame_h), color, thickness)
        else:
            cv2.line(frame, (0, self.y), (self.frame_w, self.y), color, thickness)

    def near(self, x: int, y: int, tol: int = 15) -> bool:
        """True jika kursor dalam jarak tol piksel dari garis."""
        if self.direction == "vertical":
            return abs(x - self.x) <= tol
        return abs(y - self.y) <= tol

    def side(self, cx: int, cy: int) -> int:
        """Kembalikan sisi objek relatif terhadap garis (-1 atau +1)."""
        if self.direction == "vertical":
            return 1 if cx >= self.x else -1
        else:
            return 1 if cy >= self.y else -1


class PeopleCounter:
    def __init__(
        self,
        source: str,
        model_path: str = DEFAULT_MODEL,
        line_pos: float = DEFAULT_LINE_POS,
        line_dir: str = DEFAULT_LINE_DIR,
        output: str | None = None,
        show: bool = True,
        save_log: bool = True,
        reconnect_delay: int = 5,
        max_reconnects: int = 0,
    ):
        self.source = source
        self.model = YOLO(model_path)
        self.line_pos = line_pos
        self.line_dir = line_dir
        self.output_path = output
        self.show = show
        self.save_log = save_log
        self.reconnect_delay = reconnect_delay
        self.max_reconnects = max_reconnects  # 0 = reconnect selamanya

        # State tracking
        self.track_history: dict[int, list[tuple[int, int]]] = defaultdict(list)
        self.track_frames: dict[int, int] = defaultdict(int)
        self.last_side: dict[int, int] = {}
        self.crossed_ids: set[int] = set()

        self.count_left = 0   # kanan→kiri atau bawah→atas
        self.count_right = 0  # kiri→kanan atau atas→bawah
        self.total_count = 0

        self.log_rows: list[dict] = []
        self.start_time = datetime.now()

        # State mouse drag
        self._dragging = False
        self._hover = False

        # Web streaming (thread-safe)
        self._latest_frame: bytes | None = None
        self._frame_lock = threading.Lock()
        self.running = False
        self._stop_event = threading.Event()

    def _is_rtsp(self) -> bool:
        return isinstance(self.source, str) and self.source.lower().startswith("rtsp://")

    # ─── Buka video ────────────────────────────────────────────────────────────
    def _open_source(self) -> cv2.VideoCapture:
        try:
            src = int(self.source)  # webcam index
        except ValueError:
            src = self.source

        if self._is_rtsp():
            # Paksa TCP (lebih stabil dari UDP default), set timeout 5 detik
            os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = (
                "rtsp_transport;tcp|stimeout;5000000"
            )

        cap = cv2.VideoCapture(src, cv2.CAP_FFMPEG)

        # Kurangi buffer internal agar frame tidak stale saat reconnect
        cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

        if not cap.isOpened():
            raise RuntimeError(f"Tidak bisa membuka sumber video: {self.source}")
        return cap

    # ─── Reconnect helper ──────────────────────────────────────────────────────
    def _reconnect(self, attempt: int) -> cv2.VideoCapture | None:
        """Coba buka ulang sumber. Kembalikan cap baru atau None jika gagal."""
        delay = min(self.reconnect_delay * attempt, 60)  # maks tunggu 60 detik
        ts = datetime.now().strftime("%H:%M:%S")
        print(f"[{ts}] Reconnect #{attempt} — tunggu {delay}s ...", flush=True)
        if self._stop_event.wait(delay):  # kembali lebih cepat jika stop() dipanggil
            return None
        try:
            cap = self._open_source()
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Reconnect berhasil.", flush=True)
            return cap
        except RuntimeError:
            return None

    def _reset_tracker_state(self) -> None:
        """Bersihkan state tracker setelah reconnect (ID track akan berubah)."""
        self.track_history.clear()
        self.track_frames.clear()
        self.last_side.clear()
        # crossed_ids tetap dipertahankan agar hitungan tidak terduplikasi

    def stop(self) -> None:
        self.running = False
        self._stop_event.set()

    def _reset_counts(self) -> None:
        """Reset semua hitungan dan state tracker (tombol R)."""
        self.total_count = 0
        self.count_left = 0
        self.count_right = 0
        self.crossed_ids.clear()
        self.track_history.clear()
        self.track_frames.clear()
        self.last_side.clear()
        ts = datetime.now().strftime("%H:%M:%S")
        print(f"[{ts}] Hitungan direset.", flush=True)

    # ─── Mouse callback untuk drag garis ──────────────────────────────────────
    def _mouse_callback(self, event, x: int, y: int, flags, param) -> None:
        if not hasattr(self, "counting_line"):
            return
        line = self.counting_line

        if event == cv2.EVENT_MOUSEMOVE:
            self._hover = line.near(x, y)
            if self._dragging:
                if line.direction == "vertical":
                    line.x = max(1, min(x, line.frame_w - 1))
                else:
                    line.y = max(1, min(y, line.frame_h - 1))
                # Posisi berubah — side lama tidak valid lagi
                self.last_side.clear()
                self.crossed_ids.clear()

        elif event == cv2.EVENT_LBUTTONDOWN and line.near(x, y):
            self._dragging = True

        elif event == cv2.EVENT_LBUTTONUP:
            if self._dragging:
                self._dragging = False
                pos = line.x / line.frame_w if line.direction == "vertical" else line.y / line.frame_h
                print(f"[Garis] Posisi baru: {pos:.2%}", flush=True)

    # ─── Setup video writer ────────────────────────────────────────────────────
    def _make_writer(self, cap, line: CountingLine):
        if self.output_path is None:
            return None
        w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        fps = cap.get(cv2.CAP_PROP_FPS) or 25
        fourcc = cv2.VideoWriter_fourcc(*"mp4v")
        return cv2.VideoWriter(self.output_path, fourcc, fps, (w, h))

    # ─── Overlay UI ────────────────────────────────────────────────────────────
    def _draw_overlay(self, frame: np.ndarray, fps_text: str) -> None:
        h, w = frame.shape[:2]

        # Panel hitam semi-transparan di kiri atas
        overlay = frame.copy()
        cv2.rectangle(overlay, (10, 10), (280, 130), (0, 0, 0), -1)
        cv2.addWeighted(overlay, 0.5, frame, 0.5, 0, frame)

        texts = [
            (f"Total Lewat : {self.total_count}", (255, 255, 255)),
            (f"Kanan / Bawah : {self.count_right}", (100, 255, 100)),
            (f"Kiri  / Atas  : {self.count_left}", (100, 200, 255)),
            (fps_text, (200, 200, 200)),
        ]
        for i, (text, color) in enumerate(texts):
            cv2.putText(frame, text, (18, 38 + i * 25),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.65, color, 2)

        # Label garis + hint drag
        if self._dragging:
            label = "Geser..."
            label_color = (255, 255, 255)
        elif self._hover:
            label = "Garis Hitung  [klik & geser]"
            label_color = (0, 165, 255)
        else:
            label = "Garis Hitung"
            label_color = (0, 255, 255)

        if self.line_dir == "vertical":
            lx = self.counting_line.x
            cv2.putText(frame, label, (lx + 5, 20),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, label_color, 1)
        else:
            ly = self.counting_line.y
            cv2.putText(frame, label, (5, ly - 5),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, label_color, 1)

    # ─── Proses satu frame ─────────────────────────────────────────────────────
    def _process_frame(self, frame: np.ndarray) -> np.ndarray:
        results = self.model.track(
            frame,
            persist=True,
            classes=[PERSON_CLASS_ID],
            conf=CONF_THRESHOLD,
            tracker="bytetrack.yaml",
            verbose=False,
        )

        if results[0].boxes.id is None:
            return frame

        boxes = results[0].boxes.xyxy.cpu().numpy()
        ids = results[0].boxes.id.cpu().numpy().astype(int)
        confs = results[0].boxes.conf.cpu().numpy()

        for box, tid, conf in zip(boxes, ids, confs):
            x1, y1, x2, y2 = map(int, box)
            cx = (x1 + x2) // 2
            cy = (y1 + y2) // 2

            self.track_frames[tid] += 1
            self.track_history[tid].append((cx, cy))

            # Gambar bounding box
            color = (0, 200, 0)
            cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
            cv2.circle(frame, (cx, cy), 4, (0, 0, 255), -1)
            cv2.putText(frame, f"ID:{tid}", (x1, y1 - 8),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.55, color, 2)

            # Trail pergerakan (5 titik terakhir)
            trail = self.track_history[tid][-5:]
            for i in range(1, len(trail)):
                cv2.line(frame, trail[i - 1], trail[i], (0, 165, 255), 2)

            # Hitung crossing hanya jika track sudah cukup stabil
            if self.track_frames[tid] < MIN_TRACK_FRAMES:
                continue

            side = self.counting_line.side(cx, cy)

            if tid not in self.last_side:
                self.last_side[tid] = side
                continue

            prev_side = self.last_side[tid]
            self.last_side[tid] = side

            if prev_side != side and tid not in self.crossed_ids:
                self.crossed_ids.add(tid)
                self.total_count += 1

                if side == 1:
                    self.count_right += 1
                    direction_label = "kanan" if self.line_dir == "vertical" else "bawah"
                else:
                    self.count_left += 1
                    direction_label = "kiri" if self.line_dir == "vertical" else "atas"

                ts = datetime.now().strftime("%H:%M:%S")
                print(f"[{ts}] Orang #{self.total_count} terdeteksi (ID:{tid}) → {direction_label}")

                if self.save_log:
                    self.log_rows.append({
                        "timestamp": ts,
                        "track_id": tid,
                        "arah": direction_label,
                        "total_kumulatif": self.total_count,
                    })

        return frame

    # ─── Simpan log CSV ────────────────────────────────────────────────────────
    def _save_csv(self) -> None:
        if not self.log_rows:
            return
        log_path = Path(self.source).stem + "_log.csv" if Path(self.source).exists() else "count_log.csv"
        with open(log_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=["timestamp", "track_id", "arah", "total_kumulatif"])
            writer.writeheader()
            writer.writerows(self.log_rows)
        print(f"\nLog disimpan: {log_path}")

    # ─── API untuk web server ──────────────────────────────────────────────────
    def get_frame(self) -> bytes | None:
        with self._frame_lock:
            return self._latest_frame

    def get_stats(self) -> dict:
        return {"total": self.total_count, "right": self.count_right, "left": self.count_left}

    def get_line_info(self) -> dict:
        if not hasattr(self, "counting_line"):
            return {"direction": self.line_dir, "pos": self.line_pos}
        line = self.counting_line
        pos = line.x / line.frame_w if line.direction == "vertical" else line.y / line.frame_h
        return {"direction": line.direction, "pos": round(pos, 4)}

    def set_line_pos(self, pos: float) -> None:
        if not hasattr(self, "counting_line"):
            return
        line = self.counting_line
        if line.direction == "vertical":
            line.x = max(1, min(int(pos * line.frame_w), line.frame_w - 1))
        else:
            line.y = max(1, min(int(pos * line.frame_h), line.frame_h - 1))
        self.last_side.clear()
        self.crossed_ids.clear()

    # ─── Loop utama ────────────────────────────────────────────────────────────
    def run(self) -> None:
        self.running = True
        cap = self._open_source()
        w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT)) or -1

        self.counting_line = CountingLine(self.line_pos, self.line_dir, w, h)
        writer = self._make_writer(cap, self.counting_line)

        print(f"\n{'='*50}")
        print(f"  People Counter - CCTV AI")
        print(f"  Sumber  : {self.source}")
        print(f"  Resolusi: {w}x{h}")
        print(f"  Garis   : {self.line_dir} @ {self.line_pos:.0%}")
        print(f"  Tekan Q untuk berhenti | R untuk reset hitungan")
        print(f"  Drag garis kuning untuk memindahkan posisi hitung")
        print(f"{'='*50}\n")

        frame_count = 0
        fps_display = 0.0
        t_prev = time.perf_counter()
        consecutive_fails = 0
        reconnect_attempt = 0
        MAX_CONSECUTIVE_FAILS = 5  # frame gagal berturut-turut sebelum reconnect

        while self.running:
            ret, frame = cap.read()

            # ── Tangani frame gagal (putus koneksi) ──────────────────────────
            if not ret:
                consecutive_fails += 1

                # Untuk file video biasa, EOF berarti selesai
                if not self._is_rtsp():
                    break

                if consecutive_fails < MAX_CONSECUTIVE_FAILS:
                    continue

                # RTSP putus — coba reconnect
                ts = datetime.now().strftime("%H:%M:%S")
                print(f"[{ts}] Koneksi RTSP terputus.", flush=True)
                cap.release()

                reconnect_attempt += 1
                if self.max_reconnects > 0 and reconnect_attempt > self.max_reconnects:
                    print("Batas maksimum reconnect tercapai. Berhenti.")
                    break

                # Tampilkan layar "Reconnecting..." selama menunggu
                if self.show and w > 0 and h > 0:
                    blank = np.zeros((h, w, 3), dtype=np.uint8)
                    msg = f"Reconnecting... (percobaan #{reconnect_attempt})"
                    cv2.putText(blank, msg, (w // 2 - 220, h // 2),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 200, 255), 2)
                    cv2.imshow("People Counter (tekan Q untuk keluar)", blank)
                    cv2.waitKey(1)

                new_cap = self._reconnect(reconnect_attempt)
                if new_cap is None:
                    continue  # akan coba lagi di iterasi berikutnya

                cap = new_cap
                self._reset_tracker_state()
                consecutive_fails = 0
                t_prev = time.perf_counter()
                continue

            # ── Frame valid ──────────────────────────────────────────────────
            consecutive_fails = 0
            reconnect_attempt = 0  # reset counter setelah berhasil

            frame_count += 1
            frame = self._process_frame(frame)
            line_state = "drag" if self._dragging else ("hover" if self._hover else "normal")
            self.counting_line.draw(frame, state=line_state)

            # Hitung FPS setiap 15 frame
            if frame_count % 15 == 0:
                t_now = time.perf_counter()
                fps_display = 15 / (t_now - t_prev)
                t_prev = t_now

            progress = f"{frame_count}/{total_frames}" if total_frames > 0 else str(frame_count)
            fps_text = f"FPS: {fps_display:.1f}  Frame: {progress}"
            self._draw_overlay(frame, fps_text)

            # Simpan frame untuk web streaming
            _, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 75])
            with self._frame_lock:
                self._latest_frame = jpeg.tobytes()

            if writer:
                writer.write(frame)

            if self.show:
                win = "People Counter  |  Q: keluar  |  R: reset"
                cv2.imshow(win, frame)
                # Daftarkan callback sekali saat window sudah ada
                if frame_count == 1:
                    cv2.setMouseCallback(win, self._mouse_callback)
                key = cv2.waitKey(1) & 0xFF
                if key == ord("q"):
                    break
                if key == ord("r"):
                    self._reset_counts()

        # Cleanup
        cap.release()
        if writer:
            writer.release()
            print(f"Video output disimpan: {self.output_path}")
        cv2.destroyAllWindows()

        if self.save_log:
            self._save_csv()

        duration = datetime.now() - self.start_time
        print(f"\n{'='*50}")
        print(f"  HASIL AKHIR")
        print(f"  Total orang lewat  : {self.total_count}")
        print(f"  Kanan / Bawah      : {self.count_right}")
        print(f"  Kiri  / Atas       : {self.count_left}")
        print(f"  Durasi proses      : {str(duration).split('.')[0]}")
        print(f"  Total frame        : {frame_count}")
        print(f"{'='*50}\n")
        self.running = False


# ─── CLI ───────────────────────────────────────────────────────────────────────
def parse_args():
    parser = argparse.ArgumentParser(
        description="Hitung orang yang lewat dari video CCTV menggunakan YOLOv8 + ByteTrack"
    )
    parser.add_argument(
        "source",
        help="Path ke video (misal: video.mp4), URL RTSP, atau angka untuk webcam (0, 1, ...)",
    )
    parser.add_argument(
        "--model",
        default=DEFAULT_MODEL,
        help="Model YOLOv8: yolov8n/s/m/l/x.pt  (default: %(default)s, n=cepat, x=akurat)",
    )
    parser.add_argument(
        "--line-pos",
        type=float,
        default=DEFAULT_LINE_POS,
        metavar="0.0–1.0",
        help="Posisi garis hitung (default: %(default)s = tengah frame)",
    )
    parser.add_argument(
        "--line-dir",
        choices=["vertical", "horizontal"],
        default=DEFAULT_LINE_DIR,
        help="Arah garis (default: %(default)s). vertical=orang jalan kiri-kanan, horizontal=orang jalan atas-bawah",
    )
    parser.add_argument(
        "--output",
        metavar="FILE.mp4",
        help="Simpan video output (opsional)",
    )
    parser.add_argument(
        "--no-show",
        action="store_true",
        help="Jangan tampilkan jendela video (berguna untuk proses batch/server)",
    )
    parser.add_argument(
        "--no-log",
        action="store_true",
        help="Jangan simpan log CSV",
    )
    parser.add_argument(
        "--reconnect-delay",
        type=int,
        default=5,
        metavar="DETIK",
        help="Jeda awal sebelum reconnect RTSP, bertambah tiap percobaan (default: %(default)s)",
    )
    parser.add_argument(
        "--max-reconnects",
        type=int,
        default=0,
        metavar="N",
        help="Batas percobaan reconnect, 0 = reconnect selamanya (default: %(default)s)",
    )
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    counter = PeopleCounter(
        source=args.source,
        model_path=args.model,
        line_pos=args.line_pos,
        line_dir=args.line_dir,
        output=args.output,
        show=not args.no_show,
        save_log=not args.no_log,
        reconnect_delay=args.reconnect_delay,
        max_reconnects=args.max_reconnects,
    )
    counter.run()
