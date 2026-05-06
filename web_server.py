"""
Web server multi-kamera untuk People Counter CCTV.
Jalankan : python web_server.py
Akses    : http://<ip-server>:5000
"""

import argparse
import json
import threading
import time
import uuid
from pathlib import Path

from flask import Flask, Response, jsonify, render_template, request

from people_counter import DEFAULT_LINE_DIR, DEFAULT_LINE_POS, DEFAULT_MODEL, PeopleCounter

CAMERAS_FILE = Path("cameras.json")
app = Flask(__name__)


# ─── Camera Manager ────────────────────────────────────────────────────────────

class CameraManager:
    def __init__(self):
        self._cams: dict[str, dict] = {}
        self._lock = threading.Lock()

    def add(
        self,
        url: str,
        label: str = "",
        line_pos: float = DEFAULT_LINE_POS,
        line_dir: str = DEFAULT_LINE_DIR,
        reconnect_delay: int = 5,
        max_reconnects: int = 0,
    ) -> str:
        cam_id = uuid.uuid4().hex[:8]
        counter = PeopleCounter(
            source=url,
            line_pos=line_pos,
            line_dir=line_dir,
            show=False,
            save_log=True,
            reconnect_delay=reconnect_delay,
            max_reconnects=max_reconnects,
        )
        thread = threading.Thread(target=counter.run, daemon=True, name=f"cam-{cam_id}")
        with self._lock:
            self._cams[cam_id] = {
                "counter": counter,
                "thread": thread,
                "label": label or url,
                "url": url,
                "line_pos": line_pos,
                "line_dir": line_dir,
            }
        thread.start()
        return cam_id

    def remove(self, cam_id: str) -> bool:
        with self._lock:
            cam = self._cams.pop(cam_id, None)
        if cam is None:
            return False
        cam["counter"].stop()
        return True

    def get(self, cam_id: str) -> dict | None:
        with self._lock:
            return self._cams.get(cam_id)

    def update_line_pos(self, cam_id: str, pos: float) -> None:
        with self._lock:
            if cam_id in self._cams:
                self._cams[cam_id]["line_pos"] = pos

    def list_all(self) -> list[dict]:
        with self._lock:
            return [
                {
                    "id": cid,
                    "label": c["label"],
                    "url": c["url"],
                    "line_pos": c["line_pos"],
                    "line_dir": c["line_dir"],
                    **c["counter"].get_stats(),
                }
                for cid, c in self._cams.items()
            ]

    def export_config(self) -> list[dict]:
        with self._lock:
            return [
                {
                    "url": c["url"],
                    "label": c["label"],
                    "line_pos": c["line_pos"],
                    "line_dir": c["line_dir"],
                }
                for c in self._cams.values()
            ]


mgr = CameraManager()


# ─── Persistensi kamera ────────────────────────────────────────────────────────

def _save_cameras() -> None:
    CAMERAS_FILE.write_text(json.dumps(mgr.export_config(), indent=2))


def _load_cameras() -> None:
    if not CAMERAS_FILE.exists():
        return
    try:
        entries = json.loads(CAMERAS_FILE.read_text())
        for e in entries:
            mgr.add(
                url=e["url"],
                label=e.get("label", ""),
                line_pos=e.get("line_pos", DEFAULT_LINE_POS),
                line_dir=e.get("line_dir", DEFAULT_LINE_DIR),
            )
        print(f"  {len(entries)} kamera dimuat dari {CAMERAS_FILE}")
    except Exception as ex:
        print(f"  Peringatan: gagal memuat cameras.json — {ex}")


# ─── Routes ────────────────────────────────────────────────────────────────────

@app.route("/")
def index():
    return render_template("index.html")


@app.route("/cameras", methods=["GET"])
def list_cameras():
    return jsonify(mgr.list_all())


@app.route("/cameras", methods=["POST"])
def add_camera():
    d = request.get_json(silent=True) or {}
    url = d.get("url", "").strip()
    if not url:
        return jsonify(error="url wajib diisi"), 400
    cam_id = mgr.add(
        url=url,
        label=d.get("label", "").strip(),
        line_pos=float(d.get("line_pos", DEFAULT_LINE_POS)),
        line_dir=d.get("line_dir", DEFAULT_LINE_DIR),
    )
    _save_cameras()
    return jsonify(id=cam_id), 201


@app.route("/cameras/<cam_id>", methods=["DELETE"])
def remove_camera(cam_id):
    ok = mgr.remove(cam_id)
    if ok:
        _save_cameras()
    return jsonify(ok=ok)


@app.route("/cameras/<cam_id>/feed")
def camera_feed(cam_id):
    def generate():
        while True:
            cam = mgr.get(cam_id)
            if cam is None:
                break
            frame = cam["counter"].get_frame()
            if frame:
                yield b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + frame + b"\r\n"
            else:
                time.sleep(0.033)

    return Response(generate(), mimetype="multipart/x-mixed-replace; boundary=frame")


@app.route("/cameras/<cam_id>/stats")
def camera_stats(cam_id):
    def generate():
        while True:
            cam = mgr.get(cam_id)
            if cam is None:
                yield f"data: {json.dumps({'removed': True})}\n\n"
                break
            yield f"data: {json.dumps(cam['counter'].get_stats())}\n\n"
            time.sleep(0.5)

    return Response(
        generate(),
        mimetype="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


@app.route("/cameras/<cam_id>/config")
def camera_config(cam_id):
    cam = mgr.get(cam_id)
    if cam is None:
        return jsonify(error="not found"), 404
    info = cam["counter"].get_line_info()
    info["label"] = cam["label"]
    return jsonify(info)


@app.route("/cameras/<cam_id>/reset", methods=["POST"])
def camera_reset(cam_id):
    cam = mgr.get(cam_id)
    if cam is None:
        return jsonify(error="not found"), 404
    cam["counter"]._reset_counts()
    return jsonify(ok=True)


@app.route("/cameras/<cam_id>/line", methods=["POST"])
def camera_line(cam_id):
    cam = mgr.get(cam_id)
    if cam is None:
        return jsonify(error="not found"), 404
    pos = float(request.get_json()["pos"])
    cam["counter"].set_line_pos(pos)
    mgr.update_line_pos(cam_id, pos)
    _save_cameras()
    return jsonify(ok=True)


# ─── CLI ───────────────────────────────────────────────────────────────────────

def parse_args():
    parser = argparse.ArgumentParser(
        description="People Counter — Web Server Multi-Kamera\n"
                    "Tambah/hapus kamera langsung dari browser.\n"
                    "Kamera tersimpan otomatis di cameras.json.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--host", default="0.0.0.0",
                        help="Alamat listen (default: %(default)s)")
    parser.add_argument("--port", type=int, default=5000,
                        help="Port web server (default: %(default)s)")
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    _load_cameras()
    print(f"\n  Web server: http://localhost:{args.port}")
    print(f"  Dari laptop: http://<ip-debian>:{args.port}\n")
    app.run(host=args.host, port=args.port, threaded=True)
