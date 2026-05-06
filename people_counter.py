"""
People Counter - menghitung orang yang lewat dari video CCTV
Menggunakan YOLOv8 untuk deteksi dan ByteTrack untuk tracking
"""

import argparse
import csv
import os
import queue
import re
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

# Satu lock global: memastikan hanya satu kamera yang inferensi pada satu waktu.
# Tiap kamera tetap punya model sendiri (agar tracker ByteTrack tidak tabrakan),
# tapi eksekusi di-serialisasi sehingga tidak ada CPU thrashing antar thread.
_INFER_LOCK = threading.Lock()


class CountingLine:
    """Garis hitung virtual — mendukung vertikal, horizontal, dan diagonal.
    Posisi disimpan sebagai dua titik ternormalisasi (0.0–1.0)."""

    def __init__(self, x1: float, y1: float, x2: float, y2: float,
                 frame_w: int, frame_h: int):
        self.frame_w = frame_w
        self.frame_h = frame_h
        self.x1n, self.y1n = x1, y1
        self.x2n, self.y2n = x2, y2

    @property
    def p1(self) -> tuple[int, int]:
        return (int(self.x1n * self.frame_w), int(self.y1n * self.frame_h))

    @property
    def p2(self) -> tuple[int, int]:
        return (int(self.x2n * self.frame_w), int(self.y2n * self.frame_h))

    def draw(self, frame: np.ndarray, state: str = "normal") -> None:
        color_map = {"normal": (0, 255, 255), "hover": (0, 165, 255), "drag": (255, 255, 255)}
        color = color_map.get(state, color_map["normal"])
        thickness = 3 if state == "drag" else 2
        cv2.line(frame, self.p1, self.p2, color, thickness)
        # Endpoint handles agar user tahu bisa di-drag
        r = 7 if state != "normal" else 5
        cv2.circle(frame, self.p1, r, color, -1)
        cv2.circle(frame, self.p2, r, color, -1)

    def side(self, cx: int, cy: int) -> int:
        """Sisi titik terhadap garis via cross product."""
        x1, y1 = self.p1
        x2, y2 = self.p2
        val = (x2 - x1) * (cy - y1) - (y2 - y1) * (cx - x1)
        return 1 if val >= 0 else -1

    def near(self, x: int, y: int, tol: int = 12) -> bool:
        return self._dist_to_segment(x, y) <= tol

    def near_endpoint(self, x: int, y: int, tol: int = 14) -> int:
        """Kembalikan 1 jika dekat P1, 2 jika dekat P2, 0 jika tidak."""
        x1, y1 = self.p1
        x2, y2 = self.p2
        if (x - x1) ** 2 + (y - y1) ** 2 <= tol ** 2:
            return 1
        if (x - x2) ** 2 + (y - y2) ** 2 <= tol ** 2:
            return 2
        return 0

    def _dist_to_segment(self, px: int, py: int) -> float:
        x1, y1 = self.p1
        x2, y2 = self.p2
        dx, dy = x2 - x1, y2 - y1
        if dx == 0 and dy == 0:
            return ((px - x1) ** 2 + (py - y1) ** 2) ** 0.5
        t = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy)))
        return ((px - x1 - t * dx) ** 2 + (py - y1 - t * dy) ** 2) ** 0.5


class PeopleCounter:
    def __init__(
        self,
        source: str,
        model_path: str = DEFAULT_MODEL,
        line_p1: tuple[float, float] = (0.5, 0.0),
        line_p2: tuple[float, float] = (0.5, 1.0),
        output: str | None = None,
        show: bool = True,
        save_log: bool = True,
        save_screenshots: bool = True,
        reconnect_delay: int = 5,
        max_reconnects: int = 0,
        inference_size: int = 320,
        frame_skip: int = 1,
    ):
        self.source = source
        self.model = YOLO(model_path)
        self.line_p1 = line_p1
        self.line_p2 = line_p2
        self.output_path = output
        self.show = show
        self.save_log = save_log
        self.save_screenshots = save_screenshots
        self.reconnect_delay = reconnect_delay
        self.max_reconnects = max_reconnects  # 0 = reconnect selamanya
        self.inference_size = inference_size  # ukuran input model (320 lebih cepat dari 640)
        self.frame_skip = max(1, frame_skip)  # proses 1 dari setiap frame_skip frame

        self._screenshot_dir = Path("screenshots")
        self._screenshot_queue: list[int] = []  # count values yang menunggu screenshot

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

        # State drag (desktop)
        self._dragging = False          # drag seluruh garis
        self._drag_endpoint = 0         # 1=P1, 2=P2, 0=tidak ada
        self._drag_start = (0, 0)       # posisi mouse saat mulai drag
        self._drag_p1_start = (0.0, 0.0)
        self._drag_p2_start = (0.0, 0.0)
        self._hover = False

        # Web streaming (thread-safe)
        self._latest_frame: bytes | None = None
        self._frame_lock = threading.Lock()
        self.running = False
        self._stop_event = threading.Event()
        self._reconnecting = False

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

    # ─── Screenshot ────────────────────────────────────────────────────────
    def _source_stem(self) -> str:
        part = str(self.source).rstrip("/").split("/")[-1].split("?")[0]
        return re.sub(r"[^\w-]", "_", part) or "cam"

    def _save_screenshot(self, frame: "np.ndarray", count: int) -> None:
        self._screenshot_dir.mkdir(exist_ok=True)
        ts  = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:19]  # YYYYMMDD_HHMMSS_mmm
        out = self._screenshot_dir / f"{self._source_stem()}_{ts}_{count:04d}.jpg"
        cv2.imwrite(str(out), frame, [cv2.IMWRITE_JPEG_QUALITY, 90])
        print(f"[Screenshot] {out}", flush=True)

    # ─── Mouse callback untuk drag garis ──────────────────────────────────────
    def _mouse_callback(self, event, x: int, y: int, flags, param) -> None:
        if not hasattr(self, "counting_line"):
            return
        line = self.counting_line

        if event == cv2.EVENT_MOUSEMOVE:
            ep = line.near_endpoint(x, y)
            self._hover = bool(ep) or line.near(x, y)

            if self._drag_endpoint:
                nx = max(0.0, min(1.0, x / line.frame_w))
                ny = max(0.0, min(1.0, y / line.frame_h))
                if self._drag_endpoint == 1:
                    line.x1n, line.y1n = nx, ny
                else:
                    line.x2n, line.y2n = nx, ny
                self.last_side.clear()
                self.crossed_ids.clear()
            elif self._dragging:
                mx, my = self._drag_start
                dx = (x - mx) / line.frame_w
                dy = (y - my) / line.frame_h
                x1s, y1s = self._drag_p1_start
                x2s, y2s = self._drag_p2_start
                line.x1n = max(0.0, min(1.0, x1s + dx))
                line.y1n = max(0.0, min(1.0, y1s + dy))
                line.x2n = max(0.0, min(1.0, x2s + dx))
                line.y2n = max(0.0, min(1.0, y2s + dy))
                self.last_side.clear()
                self.crossed_ids.clear()

        elif event == cv2.EVENT_LBUTTONDOWN:
            ep = line.near_endpoint(x, y)
            if ep:
                self._drag_endpoint = ep
            elif line.near(x, y):
                self._dragging = True
                self._drag_start = (x, y)
                self._drag_p1_start = (line.x1n, line.y1n)
                self._drag_p2_start = (line.x2n, line.y2n)

        elif event == cv2.EVENT_LBUTTONUP:
            if self._drag_endpoint or self._dragging:
                print(f"[Garis] P1=({line.x1n:.2f},{line.y1n:.2f}) "
                      f"P2=({line.x2n:.2f},{line.y2n:.2f})", flush=True)
            self._drag_endpoint = 0
            self._dragging = False

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

        # Label garis di titik tengah
        mid_x = (self.counting_line.p1[0] + self.counting_line.p2[0]) // 2
        mid_y = (self.counting_line.p1[1] + self.counting_line.p2[1]) // 2

        if self._dragging or self._drag_endpoint:
            label = "Geser..."
            label_color = (255, 255, 255)
        elif self._hover:
            label = "Drag endpoint/garis"
            label_color = (0, 165, 255)
        else:
            label = "Garis Hitung"
            label_color = (0, 255, 255)

        cv2.putText(frame, label, (mid_x + 8, mid_y),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, label_color, 1)

    # ─── Proses satu frame ─────────────────────────────────────────────────────
    def _process_frame(self, frame: np.ndarray) -> np.ndarray:
        with _INFER_LOCK:
            results = self.model.track(
                frame,
                persist=True,
                classes=[PERSON_CLASS_ID],
                conf=CONF_THRESHOLD,
                tracker="bytetrack.yaml",
                imgsz=self.inference_size,
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
                    direction_label = "A"
                else:
                    self.count_left += 1
                    direction_label = "B"

                ts = datetime.now().strftime("%H:%M:%S")
                print(f"[{ts}] Orang #{self.total_count} terdeteksi (ID:{tid}) → {direction_label}")

                if self.save_screenshots:
                    self._screenshot_queue.append(self.total_count)

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
            return {"x1": self.line_p1[0], "y1": self.line_p1[1],
                    "x2": self.line_p2[0], "y2": self.line_p2[1]}
        line = self.counting_line
        return {"x1": round(line.x1n, 4), "y1": round(line.y1n, 4),
                "x2": round(line.x2n, 4), "y2": round(line.y2n, 4)}

    def set_line(self, x1: float, y1: float, x2: float, y2: float) -> None:
        if not hasattr(self, "counting_line"):
            return
        line = self.counting_line
        line.x1n = max(0.0, min(1.0, x1))
        line.y1n = max(0.0, min(1.0, y1))
        line.x2n = max(0.0, min(1.0, x2))
        line.y2n = max(0.0, min(1.0, y2))
        self.last_side.clear()
        self.crossed_ids.clear()

    # ─── Thread capture (terpisah dari inferensi) ──────────────────────────────
    def _capture_loop(self, cap: cv2.VideoCapture, frame_q: queue.Queue) -> None:
        """Baca frame terus-menerus dan masukkan ke queue.
        Inferensi yang lambat akan otomatis men-drop frame lama (queue size=1)."""
        consecutive_fails = 0
        reconnect_attempt = 0
        MAX_FAILS = 5

        while self.running:
            ret, frame = cap.read()

            if not ret:
                consecutive_fails += 1
                if not self._is_rtsp():
                    break
                if consecutive_fails < MAX_FAILS:
                    continue

                ts = datetime.now().strftime("%H:%M:%S")
                print(f"[{ts}] Koneksi RTSP terputus.", flush=True)
                cap.release()
                self._reconnecting = True

                reconnect_attempt += 1
                if self.max_reconnects > 0 and reconnect_attempt > self.max_reconnects:
                    print("Batas maksimum reconnect tercapai. Berhenti.")
                    break

                new_cap = self._reconnect(reconnect_attempt)
                if new_cap is None:
                    continue

                cap = new_cap
                self._reset_tracker_state()
                consecutive_fails = 0
                reconnect_attempt = 0
                self._reconnecting = False
            else:
                consecutive_fails = 0
                reconnect_attempt = 0
                try:
                    frame_q.put_nowait(frame)  # drop frame lama jika inferensi ketinggalan
                except queue.Full:
                    pass

        cap.release()
        self.running = False  # sinyal ke inference loop untuk berhenti

    # ─── Loop utama (inferensi) ────────────────────────────────────────────────
    def run(self) -> None:
        self.running = True
        cap = self._open_source()
        w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT)) or -1

        self.counting_line = CountingLine(*self.line_p1, *self.line_p2, w, h)
        writer = self._make_writer(cap, self.counting_line)

        print(f"\n{'='*50}")
        print(f"  People Counter - CCTV AI")
        print(f"  Sumber     : {self.source}")
        print(f"  Resolusi   : {w}x{h}")
        print(f"  Garis      : P1={self.line_p1} → P2={self.line_p2}")
        print(f"  Inf. size  : {self.inference_size}px")
        print(f"  Tekan Q untuk berhenti | R untuk reset hitungan")
        print(f"  Drag garis kuning untuk memindahkan posisi hitung")
        print(f"{'='*50}\n")

        # Jalankan capture di thread terpisah agar tidak blocking inferensi
        frame_q: queue.Queue = queue.Queue(maxsize=1)
        cap_thread = threading.Thread(
            target=self._capture_loop, args=(cap, frame_q),
            daemon=True, name="capture",
        )
        cap_thread.start()

        frame_count = 0
        fps_display = 0.0
        t_prev = time.perf_counter()
        win = "People Counter  |  Q: keluar  |  R: reset"

        while self.running:
            # ── Tampilkan layar reconnecting ──────────────────────────────────
            if self._reconnecting:
                if self.show and w > 0 and h > 0:
                    blank = np.zeros((h, w, 3), dtype=np.uint8)
                    cv2.putText(blank, "Reconnecting...", (w // 2 - 160, h // 2),
                                cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 200, 255), 2)
                    cv2.imshow(win, blank)
                    if cv2.waitKey(100) & 0xFF == ord("q"):
                        self.running = False
                else:
                    time.sleep(0.1)
                continue

            # ── Ambil frame terbaru dari queue ────────────────────────────────
            try:
                frame = frame_q.get(timeout=0.5)
            except queue.Empty:
                if self.show:
                    if cv2.waitKey(1) & 0xFF == ord("q"):
                        self.running = False
                continue

            # ── Inferensi + render ────────────────────────────────────────────
            frame_count += 1
            # Jalankan inferensi hanya setiap frame_skip frame; frame yang
            # dilewati tetap di-stream (dengan garis tapi tanpa bounding box).
            if frame_count % self.frame_skip == 0:
                frame = self._process_frame(frame)
            line_state = "drag" if self._dragging else ("hover" if self._hover else "normal")
            self.counting_line.draw(frame, state=line_state)

            if frame_count % 15 == 0:
                t_now = time.perf_counter()
                fps_display = 15 / (t_now - t_prev)
                t_prev = t_now

            progress = f"{frame_count}/{total_frames}" if total_frames > 0 else str(frame_count)
            fps_text = f"FPS: {fps_display:.1f}  Frame: {progress}"
            self._draw_overlay(frame, fps_text)

            # Simpan screenshot setelah frame selesai di-render (garis + bbox sudah tergambar)
            if self._screenshot_queue:
                for count in self._screenshot_queue:
                    self._save_screenshot(frame, count)
                self._screenshot_queue.clear()

            _, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 75])
            with self._frame_lock:
                self._latest_frame = jpeg.tobytes()

            if writer:
                writer.write(frame)

            if self.show:
                cv2.imshow(win, frame)
                if frame_count == 1:
                    cv2.setMouseCallback(win, self._mouse_callback)
                key = cv2.waitKey(1) & 0xFF
                if key == ord("q"):
                    self.running = False
                elif key == ord("r"):
                    self._reset_counts()

        cap_thread.join(timeout=5)
        if writer:
            writer.release()
            print(f"Video output disimpan: {self.output_path}")
        cv2.destroyAllWindows()

        if self.save_log:
            self._save_csv()

        duration = datetime.now() - self.start_time
        print(f"\n{'='*50}")
        print(f"  HASIL AKHIR")
        print(f"  Total orang lewat   : {self.total_count}")
        print(f"  Kanan / Bawah       : {self.count_right}")
        print(f"  Kiri  / Atas        : {self.count_left}")
        print(f"  Durasi proses       : {str(duration).split('.')[0]}")
        print(f"  Frame diproses      : {frame_count}")
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
        help="Posisi garis (default: %(default)s = tengah). Diabaikan jika --line-p1/p2 diisi.",
    )
    parser.add_argument(
        "--line-dir",
        choices=["vertical", "horizontal", "diagonal-nw", "diagonal-ne"],
        default=DEFAULT_LINE_DIR,
        help="Arah garis: vertical/horizontal/diagonal-nw(↘)/diagonal-ne(↗)  (default: %(default)s)",
    )
    parser.add_argument(
        "--line-p1", metavar="X,Y",
        help="Endpoint P1 ternormalisasi, contoh: '0.2,0.1'  (override --line-pos/dir)",
    )
    parser.add_argument(
        "--line-p2", metavar="X,Y",
        help="Endpoint P2 ternormalisasi, contoh: '0.8,0.9'  (override --line-pos/dir)",
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
        "--no-screenshots",
        action="store_true",
        help="Jangan simpan screenshot saat ada orang lewat",
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
    parser.add_argument(
        "--imgsz",
        type=int,
        default=320,
        metavar="PX",
        help="Ukuran input inferensi YOLOv8 (default: %(default)s). "
             "Lebih kecil=lebih cepat, lebih besar=lebih akurat. Pilihan: 160/320/480/640",
    )
    parser.add_argument(
        "--frame-skip",
        type=int,
        default=1,
        metavar="N",
        help="Inferensi setiap N frame; frame lain di-stream tanpa deteksi. "
             "1=setiap frame, 2=setiap 2 frame, dst. (default: %(default)s)",
    )
    args = parser.parse_args()

    # Hitung p1/p2 dari argumen
    if args.line_p1 and args.line_p2:
        p1 = tuple(float(v) for v in args.line_p1.split(","))
        p2 = tuple(float(v) for v in args.line_p2.split(","))
    else:
        pos = args.line_pos
        d   = args.line_dir
        if d == "vertical":
            p1, p2 = (pos, 0.0), (pos, 1.0)
        elif d == "horizontal":
            p1, p2 = (0.0, pos), (1.0, pos)
        elif d == "diagonal-nw":
            p1, p2 = (0.0, 0.0), (1.0, 1.0)
        else:  # diagonal-ne
            p1, p2 = (1.0, 0.0), (0.0, 1.0)
    args.computed_p1 = p1
    args.computed_p2 = p2
    return args


if __name__ == "__main__":
    args = parse_args()
    counter = PeopleCounter(
        source=args.source,
        model_path=args.model,
        line_p1=args.computed_p1,
        line_p2=args.computed_p2,
        output=args.output,
        show=not args.no_show,
        save_log=not args.no_log,
        save_screenshots=not args.no_screenshots,
        reconnect_delay=args.reconnect_delay,
        max_reconnects=args.max_reconnects,
        inference_size=args.imgsz,
        frame_skip=args.frame_skip,
    )
    counter.run()
