"""
Web server multi-kamera untuk People Counter CCTV.
Jalankan : python web_server.py
Akses    : http://<ip-server>:5000

Kredensial dikonfigurasi lewat environment variable:
  AUTH_USERNAME  (default: admin)
  AUTH_PASSWORD  (default: admin)
  SECRET_KEY     (dianjurkan diset untuk sesi yang persisten)
"""

import argparse
import functools
import json
import os
import threading
import time
import uuid
from datetime import timedelta
from pathlib import Path

from flask import (Flask, Response, jsonify, redirect, render_template,
                   request, send_from_directory, session, url_for)

from people_counter import DEFAULT_LINE_DIR, DEFAULT_LINE_POS, PeopleCounter

CAMERAS_FILE    = Path("cameras.json")
SCREENSHOTS_DIR = Path("screenshots")

app = Flask(__name__)
app.secret_key = os.environ.get("SECRET_KEY") or os.urandom(24)
app.permanent_session_lifetime = timedelta(days=7)

# Diset ke False lewat --no-auth di CLI
_auth_enabled = True


# ─── Auth ──────────────────────────────────────────────────────────────────────

def _get_creds() -> tuple[str, str]:
    return (
        os.environ.get("AUTH_USERNAME", "admin"),
        os.environ.get("AUTH_PASSWORD", "admin"),
    )


def login_required(f):
    @functools.wraps(f)
    def decorated(*args, **kwargs):
        if not _auth_enabled or session.get("logged_in"):
            return f(*args, **kwargs)
        # API calls (JSON/stream) kembalikan 401; page requests redirect ke login
        if _wants_json_or_stream():
            return jsonify(error="Unauthorized"), 401
        return redirect(url_for("login_page", next=request.path))
    return decorated


def _wants_json_or_stream() -> bool:
    """True jika request adalah AJAX/SSE/stream, bukan navigasi browser biasa."""
    accept = request.headers.get("Accept", "")
    return (
        "application/json" in accept
        or "text/event-stream" in accept
        or "multipart/x-mixed-replace" in accept
        or request.method in ("POST", "DELETE", "PUT", "PATCH")
    )


@app.route("/login", methods=["GET", "POST"])
def login_page():
    if session.get("logged_in"):
        return redirect(url_for("index"))
    error = None
    if request.method == "POST":
        username, password = _get_creds()
        if (request.form.get("username") == username and
                request.form.get("password") == password):
            session.permanent = True
            session["logged_in"] = True
            return redirect(request.args.get("next") or url_for("index"))
        error = "Username atau password salah."
    return render_template("login.html", error=error)


@app.route("/logout")
def logout():
    session.clear()
    return redirect(url_for("login_page"))


# ─── Camera Manager ────────────────────────────────────────────────────────────

class CameraManager:
    def __init__(self):
        self._cams: dict[str, dict] = {}
        self._lock = threading.Lock()

    def add(
        self,
        url: str,
        label: str = "",
        line_p1: tuple = (0.5, 0.0),
        line_p2: tuple = (0.5, 1.0),
        reconnect_delay: int = 5,
        max_reconnects: int = 0,
        inference_size: int = 320,
    ) -> str:
        cam_id = uuid.uuid4().hex[:8]
        counter = PeopleCounter(
            source=url,
            line_p1=tuple(line_p1),
            line_p2=tuple(line_p2),
            show=False,
            save_log=True,
            reconnect_delay=reconnect_delay,
            max_reconnects=max_reconnects,
            inference_size=inference_size,
        )
        thread = threading.Thread(target=counter.run, daemon=True, name=f"cam-{cam_id}")
        with self._lock:
            self._cams[cam_id] = {
                "counter": counter,
                "thread": thread,
                "label": label or url,
                "url": url,
                "line_p1": list(line_p1),
                "line_p2": list(line_p2),
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

    def update_line(self, cam_id: str, x1: float, y1: float, x2: float, y2: float) -> None:
        with self._lock:
            if cam_id in self._cams:
                self._cams[cam_id]["line_p1"] = [x1, y1]
                self._cams[cam_id]["line_p2"] = [x2, y2]

    def list_all(self) -> list[dict]:
        with self._lock:
            return [
                {
                    "id": cid,
                    "label": c["label"],
                    "url": c["url"],
                    **c["counter"].get_line_info(),
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
                    "line_p1": c["line_p1"],
                    "line_p2": c["line_p2"],
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
            if "line_p1" in e and "line_p2" in e:
                p1 = tuple(e["line_p1"])
                p2 = tuple(e["line_p2"])
            else:
                pos = e.get("line_pos", DEFAULT_LINE_POS)
                d   = e.get("line_dir", DEFAULT_LINE_DIR)
                if d == "horizontal":
                    p1, p2 = (0.0, pos), (1.0, pos)
                else:
                    p1, p2 = (pos, 0.0), (pos, 1.0)
            mgr.add(url=e["url"], label=e.get("label", ""), line_p1=p1, line_p2=p2)
        print(f"  {len(entries)} kamera dimuat dari {CAMERAS_FILE}")
    except Exception as ex:
        print(f"  Peringatan: gagal memuat cameras.json — {ex}")


# ─── Routes ────────────────────────────────────────────────────────────────────

@app.route("/")
@login_required
def index():
    return render_template("index.html")


@app.route("/cameras", methods=["GET"])
@login_required
def list_cameras():
    return jsonify(mgr.list_all())


@app.route("/cameras", methods=["POST"])
@login_required
def add_camera():
    d = request.get_json(silent=True) or {}
    url = d.get("url", "").strip()
    if not url:
        return jsonify(error="url wajib diisi"), 400
    p1 = (float(d.get("x1", 0.5)), float(d.get("y1", 0.0)))
    p2 = (float(d.get("x2", 0.5)), float(d.get("y2", 1.0)))
    cam_id = mgr.add(
        url=url,
        label=d.get("label", "").strip(),
        line_p1=p1,
        line_p2=p2,
    )
    _save_cameras()
    return jsonify(id=cam_id), 201


@app.route("/cameras/<cam_id>", methods=["DELETE"])
@login_required
def remove_camera(cam_id):
    ok = mgr.remove(cam_id)
    if ok:
        _save_cameras()
    return jsonify(ok=ok)


@app.route("/cameras/<cam_id>/feed")
@login_required
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
@login_required
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
@login_required
def camera_config(cam_id):
    cam = mgr.get(cam_id)
    if cam is None:
        return jsonify(error="not found"), 404
    info = cam["counter"].get_line_info()
    info["label"] = cam["label"]
    return jsonify(info)


@app.route("/cameras/<cam_id>/reset", methods=["POST"])
@login_required
def camera_reset(cam_id):
    cam = mgr.get(cam_id)
    if cam is None:
        return jsonify(error="not found"), 404
    cam["counter"]._reset_counts()
    return jsonify(ok=True)


@app.route("/cameras/<cam_id>/line", methods=["POST"])
@login_required
def camera_line(cam_id):
    cam = mgr.get(cam_id)
    if cam is None:
        return jsonify(error="not found"), 404
    d = request.get_json()
    x1, y1 = float(d["x1"]), float(d["y1"])
    x2, y2 = float(d["x2"]), float(d["y2"])
    cam["counter"].set_line(x1, y1, x2, y2)
    mgr.update_line(cam_id, x1, y1, x2, y2)
    _save_cameras()
    return jsonify(ok=True)


# ─── Screenshot routes ─────────────────────────────────────────────────────────

def _parse_screenshot_name(name: str) -> dict:
    """Parse {source}_{YYYYMMDD}_{HHMMSS}_{mmm}_{count}.jpg → metadata dict."""
    stem = name[:-4]
    parts = stem.rsplit("_", 4)
    if len(parts) == 5:
        source, date, time_s, _ms, count_s = parts
        try:
            from datetime import datetime as dt
            ts = dt.strptime(f"{date}_{time_s}", "%Y%m%d_%H%M%S").strftime("%Y-%m-%d %H:%M:%S")
            return {"source": source, "ts": ts, "count": int(count_s)}
        except ValueError:
            pass
    return {"source": name, "ts": "-", "count": 0}


@app.route("/screenshots")
@login_required
def list_screenshots():
    if not SCREENSHOTS_DIR.exists():
        return jsonify([])
    files = sorted(SCREENSHOTS_DIR.glob("*.jpg"), key=lambda f: f.stat().st_mtime, reverse=True)
    result = []
    for f in files:
        meta = _parse_screenshot_name(f.name)
        result.append({"name": f.name, "url": f"/screenshots/{f.name}",
                        "size": f.stat().st_size, **meta})
    return jsonify(result)


@app.route("/screenshots/<path:filename>")
@login_required
def get_screenshot(filename):
    return send_from_directory(SCREENSHOTS_DIR.resolve(), filename)


@app.route("/screenshots/<path:filename>", methods=["DELETE"])
@login_required
def delete_screenshot(filename):
    path = (SCREENSHOTS_DIR / filename).resolve()
    if path.parent.resolve() != SCREENSHOTS_DIR.resolve():
        return jsonify(error="forbidden"), 403
    if path.exists() and path.suffix == ".jpg":
        path.unlink()
        return jsonify(ok=True)
    return jsonify(ok=False), 404


# ─── CLI ───────────────────────────────────────────────────────────────────────

def parse_args():
    parser = argparse.ArgumentParser(
        description="People Counter — Web Server Multi-Kamera\n"
                    "Kredensial: AUTH_USERNAME / AUTH_PASSWORD env var\n"
                    "            (default: admin / admin)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--host", default="0.0.0.0",
                        help="Alamat listen (default: %(default)s)")
    parser.add_argument("--port", type=int, default=5000,
                        help="Port web server (default: %(default)s)")
    parser.add_argument("--no-auth", action="store_true",
                        help="Nonaktifkan autentikasi (hanya untuk pengembangan lokal)")
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    _auth_enabled = not args.no_auth

    _load_cameras()

    username, password = _get_creds()
    print(f"\n  Web server  : http://localhost:{args.port}")
    print(f"  Dari laptop : http://<ip-debian>:{args.port}")
    if _auth_enabled:
        print(f"  Auth        : aktif  (user={username})")
        if password == "admin":
            print("  PERINGATAN  : Gunakan env var AUTH_PASSWORD untuk ganti password default!")
        if not os.environ.get("SECRET_KEY"):
            print("  PERINGATAN  : SECRET_KEY tidak diset — sesi hilang saat server restart.")
    else:
        print("  Auth        : nonaktif (--no-auth)")
    print()

    app.run(host=args.host, port=args.port, threaded=True)
