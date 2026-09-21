#!/usr/bin/env python3
"""server.py — FGOA 卡组编辑器后端（纯标准库，Deck 零依赖）

端点：
  GET  /              → index.html
  GET  /catalog.json  → 卡目录（build_catalog.py 生成）
  GET  /img/<file>    → 卡图 BMP（DEVICE/print/FGO11_AllServants/ 内，防路径穿越）
  GET  /api/deck      → 当前 App/deck.json（join 卡名）
  POST /api/deck      → {cards:[{file,copies}]} 校验后写 deck.json（先备份 .bak）

环境变量：
  FGOA_ROOT  FGOA 安装根（含 App/ DEVICE/ 的那层）。
             默认 $HOME/.var/app/com.usebottles.bottles/data/bottles/bottles/FGOA/drive_c/FGOA
             WSL 调试: FGOA_ROOT=/mnt/e/Games/FGOA/FGOA python3 server.py
  FGOA_PORT  默认 8931
"""
import json
import os
import re
import shutil
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_ROOT = (Path.home() / ".var/app/com.usebottles.bottles/data/bottles/"
                "bottles/FGOA/drive_c/FGOA")
FGOA_ROOT = Path(os.environ.get("FGOA_ROOT", DEFAULT_ROOT))
PORT = int(os.environ.get("FGOA_PORT", "8931"))

CARD_DIR = FGOA_ROOT / "DEVICE/print/FGO11_AllServants"
DECK_JSON = FGOA_ROOT / "App/deck.json"
CARDS_PATH_WIN = r"..\DEVICE\print\FGO11_AllServants"  # SelectedCards 前缀（Windows 客户端 DLL 按反斜杠解析，勿改）
# CardsPath 用正斜杠：服务端在 Linux 下 normpath(join(app_root, CardsPath)) 不识别反斜杠，
# 反斜杠会导致 catalog 加载失败、call_up 从者卡全判 invalid（0.35 根因）。
CARDS_PATH_POSIX = "../DEVICE/print/FGO11_AllServants"

MAX_CARDS = 5
MAX_COPIES = 3
FILE_RE = re.compile(r"^\d{5}_(?:SVT|CE)\d{5}_A\d{2}_(?:NORMAL|HOLO)\.bmp$")

CATALOG = json.loads((HERE / "catalog.json").read_text(encoding="utf-8"))
BY_FILE = {c["file"]: c for c in CATALOG}


def read_deck() -> dict:
    """读 deck.json → {cards:[{file, copies, ...卡名}], error?}"""
    if not DECK_JSON.exists():
        return {"cards": [], "error": f"deck.json 不存在: {DECK_JSON}"}
    try:
        raw = json.loads(DECK_JSON.read_text(encoding="utf-8"))
    except Exception as e:
        return {"cards": [], "error": f"deck.json 解析失败: {e}"}
    cards = []
    files = raw.get("SelectedCards", [])
    copies = raw.get("SelectedCardCopies", [1] * len(files))
    for i, win_path in enumerate(files):
        fn = win_path.replace("\\", "/").rsplit("/", 1)[-1]
        entry = {"file": fn, "copies": copies[i] if i < len(copies) else 1}
        entry.update({k: BY_FILE[fn][k] for k in ("nameZh", "nameJa", "type", "finish")} if fn in BY_FILE else {})
        cards.append(entry)
    return {"cards": cards}


def write_deck(cards: list) -> dict:
    """校验并写 deck.json。cards: [{file, copies}]"""
    if not cards:
        return {"ok": False, "error": "卡组为空"}
    if len(cards) > MAX_CARDS:
        return {"ok": False, "error": f"超过 {MAX_CARDS} 张上限"}
    files, copies = [], []
    for c in cards:
        fn, n = c.get("file", ""), c.get("copies", 1)
        if not FILE_RE.match(fn) or fn not in BY_FILE:
            return {"ok": False, "error": f"未知卡文件: {fn}"}
        if not (CARD_DIR / fn).is_file():
            return {"ok": False, "error": f"卡图缺失: {fn}"}
        if not isinstance(n, int) or not 1 <= n <= MAX_COPIES:
            return {"ok": False, "error": f"copies 须为 1~{MAX_COPIES}: {fn}"}
        files.append(fn)
        copies.append(n)
    payload = {
        "SelectedCards": [f"{CARDS_PATH_WIN}\\{fn}" for fn in files],
        "SelectedCardCopies": copies,
        "CardsPath": CARDS_PATH_POSIX,
    }
    if DECK_JSON.exists():
        shutil.copy2(DECK_JSON, DECK_JSON.with_suffix(".json.bak"))
    DECK_JSON.write_text(json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
                         encoding="utf-8")
    return {"ok": True, "count": len(files), "path": str(DECK_JSON)}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code: int = 200) -> None:
        self._send(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"),
                   "application/json; charset=utf-8")

    def log_message(self, fmt, *args):  # 静默默认访问日志，保留错误
        if args and isinstance(args[1], str) and args[1].startswith(("4", "5")):
            super().log_message(fmt, *args)

    def do_GET(self) -> None:
        path = self.path.split("?", 1)[0]
        if path == "/":
            self._send(200, (HERE / "index.html").read_bytes(), "text/html; charset=utf-8")
        elif path == "/catalog.json":
            self._send(200, (HERE / "catalog.json").read_bytes(), "application/json; charset=utf-8")
        elif path == "/api/deck":
            self._json(read_deck())
        elif path.startswith("/img/"):
            fn = path[5:]
            if not FILE_RE.match(fn):
                return self._json({"error": "bad name"}, 400)
            p = CARD_DIR / fn
            if not p.is_file():
                return self._json({"error": "not found"}, 404)
            self._send(200, p.read_bytes(), "image/bmp")
        else:
            self._json({"error": "not found"}, 404)

    def do_POST(self) -> None:
        if self.path.split("?", 1)[0] != "/api/deck":
            return self._json({"error": "not found"}, 404)
        try:
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            cards = json.loads(body)["cards"]
            assert isinstance(cards, list)
        except Exception:
            return self._json({"ok": False, "error": "请求体须为 {cards:[...]}"}, 400)
        self._json(write_deck(cards), 200)


def main() -> None:
    if not CARD_DIR.is_dir():
        sys.exit(f"[carddeck] 卡图目录不存在: {CARD_DIR}\n"
                 f"          请检查 FGOA_ROOT（当前={FGOA_ROOT}）")
    print(f"[carddeck] FGOA_ROOT={FGOA_ROOT}")
    print(f"[carddeck] deck.json={DECK_JSON}")
    print(f"[carddeck] 打开 http://127.0.0.1:{PORT}/ （Ctrl+C 退出）")
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
