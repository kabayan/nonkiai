#!/usr/bin/env python3
"""USB 書き込み画面の配信と model-builder の読み取り専用中継。標準ライブラリのみ。"""

import argparse
import hashlib
import http.client
import io
import json
import math
import re
import threading
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

DEFAULT_BUILDER = ""
ROOT = Path(__file__).resolve().parents[1]
MAX_BLOB_BYTES = 16 * 1024 * 1024
MODEL_KEYS = ("SAAN_DEC_HEAD", "SAAN_G_EXCITE", "SAAN_PHRASE_RULE_SBV2", "SAAN_TAIL_PAUSE", "SAAN_INT8_ACT")
RUN_ID = re.compile(r"[A-Za-z0-9_-][A-Za-z0-9_.-]{0,159}\Z")
SHA256 = re.compile(r"[0-9a-fA-F]{64}\Z")


class BuilderError(Exception):
    """画面に表示できる中継エラー。"""


NUMBER = re.compile(r"[+-]?\d+(\.\d+)?([eE][+-]?\d+)?\Z")


def number(value):
    """数値または数値の文字列（loHz / hiHz は文字列で来る）を float にする。それ以外は None。"""
    if type(value) in (int, float):
        return float(value)
    if isinstance(value, str) and NUMBER.fullmatch(value.strip()):
        return float(value)
    return None


def read_voice(flags):
    """声ごとの値（f0Shape と voice.vnoise）を画面に通す。判定（励起つきで vnoise 無しの拒否など）は画面側で行う。"""
    out = {}
    shape = flags.get("f0Shape")
    if shape is not None:
        if not isinstance(shape, dict):
            raise BuilderError("bundle の f0Shape が不正です")
        values = {key: number(shape.get(key)) for key in ("k", "centerHz", "loHz", "hiHz")}
        values["tauMs"] = 75.0 if shape.get("tauMs") is None else number(shape.get("tauMs"))
        if any(value is None or value != value for value in values.values()):
            raise BuilderError("bundle の f0Shape の数値が不正です")
        out["f0Shape"] = values
    voice = flags.get("voice")
    if isinstance(voice, dict) and voice.get("vnoise") is not None:
        vnoise = number(voice["vnoise"])
        if vnoise is None or not 0 < vnoise <= 1:
            raise BuilderError("bundle の voice.vnoise が不正です")
        out["vnoise"] = vnoise   # 一覧の run では "voice" は声の名前なので、build-flags の voice.vnoise は vnoise で通す
    return out


def read_flags(bundle):
    """巨大な展開や同名ファイルを拒否し、一覧に必要なメタデータだけを読む。"""
    try:
        with zipfile.ZipFile(io.BytesIO(bundle)) as archive:
            names = archive.namelist()
            if names.count("build-flags.json") != 1 or names.count("model-int8.bin") != 1:
                raise BuilderError("bundle に重みと build-flags.json が１つずつ必要です")
            info = archive.getinfo("build-flags.json")
            if info.file_size > 65536:
                raise BuilderError("build-flags.json が大きすぎます")
            flags = json.loads(archive.read(info))
            size = flags.get("blobBytes")
            sha = flags.get("blobSha256")
            model = flags.get("model")
            if type(size) is not int or not 0 < size <= MAX_BLOB_BYTES:
                raise BuilderError("bundle の blobBytes が不正です")
            if not isinstance(sha, str) or not SHA256.fullmatch(sha):
                raise BuilderError("bundle の blobSha256 が不正です")
            if not isinstance(model, dict) or any(
                not re.fullmatch(r"\d+", str(model.get(key, ""))) for key in MODEL_KEYS
            ):
                raise BuilderError("bundle の model フラグが不足または不正です")
            if archive.getinfo("model-int8.bin").file_size != size:
                raise BuilderError("bundle の重みの大きさと blobBytes が一致しません")
            return {"blobBytes": size, "blobSha256": sha.lower(),
                    "model": {key: str(int(model[key])) for key in MODEL_KEYS}, **read_voice(flags)}
    except (ValueError, KeyError, TypeError, AttributeError, EOFError, zlib.error,
            zipfile.BadZipFile, RuntimeError, NotImplementedError) as error:
        raise BuilderError("bundle の build-flags.json を読めません") from error


class ModelBuilder:
    def __init__(self, base_url=DEFAULT_BUILDER, timeout=15):
        parsed = urllib.parse.urlsplit(base_url)
        if base_url and (parsed.scheme not in ("http", "https") or not parsed.netloc or parsed.query or parsed.fragment):
            raise ValueError("model-builder の場所は http または https の URL にしてください")
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.cache = {}
        self.lock = threading.Lock()

    def fetch(self, path, limit):
        if not self.base_url:
            raise BuilderError("model-builder の URL が未設定です（--builder で指定）")
        request = urllib.request.Request(self.base_url + "/" + path, headers={"Accept": "*/*"}, method="GET")
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                data = response.read(limit + 1)
        except (urllib.error.URLError, TimeoutError, OSError, http.client.HTTPException) as error:
            raise BuilderError("model-builder に届きません。起動状態と接続先を確認してください（" + self.base_url + "）") from error
        if len(data) > limit:
            raise BuilderError("model-builder の応答が大きすぎます")
        return data

    def runs(self):
        try:
            catalog = json.loads(self.fetch("api/models", 4 * 1024 * 1024))
            models = {str(model["id"]): model for model in catalog.get("models", [])}
            source_runs = catalog["v2Runs"]
            if not isinstance(source_runs, list):
                raise ValueError()
        except (ValueError, KeyError, TypeError, AttributeError) as error:
            raise BuilderError("model-builder の一覧を読めません") from error
        runs, warnings = [], []
        for run in source_runs:
            if not isinstance(run, dict) or not run.get("complete") or run.get("failed"):
                continue
            identifier = run.get("run", "")
            blob = run.get("model")
            if not isinstance(identifier, str) or not RUN_ID.fullmatch(identifier):
                continue
            if not isinstance(blob, dict) or not isinstance(blob.get("bytes"), (int, float)) or blob["bytes"] <= 0:
                continue
            key = (identifier, str(run.get("updatedAt", "")), blob["bytes"])
            with self.lock:
                flags = self.cache.get(key)
            try:
                if flags is None:
                    flags = read_flags(self.fetch("api/models/runs/" + identifier + "/model-bundle.zip", MAX_BLOB_BYTES + 1024 * 1024))
                    with self.lock:
                        # 同じ run の更新前のキャッシュを残さない。
                        self.cache = {k: v for k, v in self.cache.items() if k[0] != identifier}
                        self.cache[key] = flags
                if flags["blobBytes"] != blob["bytes"]:
                    raise BuilderError("一覧と bundle の重みの大きさが一致しません")
            except BuilderError as error:
                warnings.append(identifier + "：" + str(error))
                continue
            model = models.get(str(run.get("modelId")), {})
            runs.append({"run": identifier, "voice": model.get("label") or model.get("name") or run.get("modelId") or "名前なし",
                         "label": run.get("label") or identifier, "updatedAt": run.get("updatedAt"), **flags,
                         "downloadUrl": "/api/runs/" + identifier + "/model-int8.bin"})
        # updatedAt は Unix 秒。文字列比較では 9 と 10 の順序が逆になる。
        def timestamp(run):
            value = number(run.get("updatedAt"))
            return value if value is not None and math.isfinite(value) else float("-inf")
        runs.sort(key=timestamp, reverse=True)
        return {"runs": runs, "warnings": warnings}

    def model(self, identifier):
        if not RUN_ID.fullmatch(identifier):
            raise BuilderError("run の名前が不正です")
        selected = next((run for run in self.runs()["runs"] if run["run"] == identifier), None)
        if selected is None:
            raise BuilderError("この run には書き込める重みがありません。一覧を更新してください")
        data = self.fetch("api/models/runs/" + identifier + "/model-int8.bin", MAX_BLOB_BYTES)
        if len(data) != selected["blobBytes"] or hashlib.sha256(data).hexdigest() != selected["blobSha256"]:
            raise BuilderError("取得した重みが bundle の大きさ・SHA-256 と一致しません。一覧を更新してください")
        return data


STATIC_FILES = {
    "/settings.html": ("settings.html", "text/html; charset=utf-8"),
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    "/record.html": ("record.html", "text/html; charset=utf-8"),
    "/blue-plush-bear.png": ("blue-plush-bear.png", "image/png"),
    "/bunny.svg": ("bunny.svg", "image/svg+xml"),
    "/monitor": ("serial-monitor.html", "text/html; charset=utf-8"),
    "/serial-monitor.js": ("serial-monitor.js", "text/javascript; charset=utf-8"),
    "/": ("model-flasher.html", "text/html; charset=utf-8"),
    "/model-flasher.html": ("model-flasher.html", "text/html; charset=utf-8"),
    "/model-flasher.css": ("model-flasher.css", "text/css; charset=utf-8"),
    "/model-flasher.js": ("model-flasher.js", "text/javascript; charset=utf-8"),
    "/model-flasher-core.js": ("model-flasher-core.js", "text/javascript; charset=utf-8"),
    "/model-flasher-serial.js": ("model-flasher-serial.js", "text/javascript; charset=utf-8"),
    "/model-flasher-usb.js": ("model-flasher-usb.js", "text/javascript; charset=utf-8"),
}


def make_handler(builder):
    class Handler(BaseHTTPRequestHandler):
        def send_data(self, code, data, content_type):
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(data)

        def send_json(self, code, obj):
            self.send_data(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8")

        def do_GET(self):
            path = urllib.parse.urlsplit(self.path).path
            try:
                if path in STATIC_FILES:
                    name, mime = STATIC_FILES[path]
                    self.send_data(200, (ROOT / "webui" / name).read_bytes(), mime)
                elif path == "/api/runs":
                    self.send_json(200, builder.runs())
                elif path in ("/api/app.bin", "/api/app.json", "/api/dict.bin"):
                    filename = (ROOT / "firmware" / ("data/sanotts/k1_dict_4mb.bin" if path == "/api/dict.bin"
                                                      else "build/nonkiact2_sampler.bin"))
                    data = filename.read_bytes()
                    if path == "/api/app.json":
                        # ESP イメージヘッダ 24 B + セグメントヘッダ 8 B の後に esp_app_desc_t がある。
                        version = None
                        if len(data) >= 80 and data[0] == 0xe9 and data[32:36] == b"\x32\x54\xcd\xab":
                            version = data[48:80].split(b"\0", 1)[0].decode("utf-8", errors="replace")
                        self.send_json(200, {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "version": version})
                    else:
                        self.send_data(200, data, "application/octet-stream")
                elif path.startswith("/api/runs/") and path.endswith("/model-int8.bin"):
                    identifier = path[len("/api/runs/"):-len("/model-int8.bin")]
                    if not RUN_ID.fullmatch(identifier):
                        self.send_json(400, {"error": "run の名前が不正です"})
                        return
                    self.send_data(200, builder.model(identifier), "application/octet-stream")
                else:
                    self.send_json(404, {"error": "指定されたページはありません"})
            except BuilderError as error:
                self.send_json(502, {"error": str(error)})
            except (OSError, ValueError):
                self.send_json(500, {"error": "画面またはモデルの読み込みに失敗しました"})

        do_HEAD = do_GET

        def do_POST(self):
            self.send_json(405, {"error": "この中継は読み取り専用です"})

        do_PUT = do_DELETE = do_PATCH = do_POST

        def log_request(self, code="-", size="-"):
            print("要求：" + self.command + " " + self.path + "　応答：" + str(code), flush=True)

        def log_message(self, format, *args):
            print("通信の処理中にエラーがありました", flush=True)

    return Handler


def main():
    parser = argparse.ArgumentParser(description="声モデルの書き込み画面を localhost で開きます")
    parser.add_argument("--host", default="127.0.0.1", help="待ち受け先（既定：127.0.0.1）")
    parser.add_argument("--port", type=int, default=8095, help="ポート番号（既定：8095）")
    parser.add_argument("--builder", default=DEFAULT_BUILDER, help="model-builder の URL（未指定なら model-builder の一覧を使いません）")
    args = parser.parse_args()
    try:
        builder = ModelBuilder(args.builder)
        server = ThreadingHTTPServer((args.host, args.port), make_handler(builder))
    except (OSError, ValueError) as error:
        parser.exit(1, "中継サーバを起動できません。待ち受け先・ポート・接続先を確認してください：" + str(error) + "\n")
    print(f"USB 書き込み画面（アプリ・モデル・辞書）：http://localhost:{server.server_port}/\nモデルの取得先：{builder.base_url}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n中継サーバを終了します", flush=True)
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
