#!/usr/bin/env python3
"""read-helper 门禁：用假夹具验证 diagnose 的解析/脱敏与 probe 的成功/失败路径。

用法：python tools/read_helper_check.py <read-helper 路径>
退出码 0 = 全过；非 0 = 有失败（stderr 打印 FAIL 行）。
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HELPER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tools", "hermes-panel-read.py")
SECRET = "sk-FIXTURE-SECRET-1234567890"
passed = failed = 0


def check(condition, name):
    global passed, failed
    if condition:
        passed += 1
    else:
        failed += 1
        print("FAIL " + name, file=sys.stderr)


def call(helper, home, payload, extra_env=None):
    env = dict(os.environ)
    env["HERMES_READ_HOME"] = home
    if extra_env:
        env.update(extra_env)
    env.setdefault("PYTHONUTF8", "1")
    env.setdefault("PYTHONIOENCODING", "utf-8")
    proc = subprocess.run([sys.executable, helper], input=json.dumps(payload) + "\n",
                          capture_output=True, text=True, encoding="utf-8", errors="replace",
                          env=env, timeout=60)
    lines = [line for line in proc.stdout.splitlines() if line.strip()]
    check(len(lines) == 1, "stdout is exactly one JSON line (cmd=%s)" % payload.get("cmd"))
    return json.loads(lines[0]) if lines else {}, proc


def make_fixture(base):
    home = os.path.join(base, "hermes")
    os.makedirs(os.path.join(home, "logs"))
    os.makedirs(os.path.join(home, "weixin", "accounts"))
    with open(os.path.join(home, ".env"), "w", encoding="utf-8") as handle:
        handle.write("API_SERVER_KEY=%s\nOTHER=1\n" % SECRET)
    with open(os.path.join(home, "config.yaml"), "w", encoding="utf-8") as handle:
        handle.write("model:\n  default: alpha-1\n")
    now = "2026-10-17 10:00:00"
    lines = [
        "%s,000 INFO gateway.run: Connecting to weixin..." % now,
        "%s,100 INFO gateway.platforms.weixin: [Weixin] Connected account=bf35e4bf" % now,
        "%s,200 INFO gateway.platforms.weixin: [Weixin] inbound from=o9cq message_id=1" % now,
        "%s,300 INFO gateway.platforms.base: [Weixin] Sending response (12 chars) to o9cq" % now,
        "%s,400 INFO gateway.run: Connecting to qqbot..." % now,
        "%s,500 INFO gateway.platforms.qqbot.adapter: [QQBot:1] Connected" % now,
        "%s,600 INFO gateway.platforms.qqbot.adapter: [QQBot:1] WebSocket closed: code=4009" % now,
        "%s,700 INFO gateway.platforms.qqbot.adapter: [QQBot:1] Reconnected" % now,
        "%s,800 WARNING gateway.platforms.api_server: API key=%s rejected" % (now, SECRET),
    ]
    with open(os.path.join(home, "logs", "gateway.log"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    with open(os.path.join(home, "logs", "errors.log"), "w", encoding="utf-8") as handle:
        handle.write("2026-10-17 10:00:01,000 WARNING x: token=%s bad\n" % SECRET)
    accounts = os.path.join(home, "weixin", "accounts")
    for name in ("a@im.bot.sync.json", "a@im.bot.context-tokens.json"):
        with open(os.path.join(accounts, name), "w", encoding="utf-8") as handle:
            handle.write("{}")
    return home


class ProbeHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(length)
        if self.headers.get("Authorization", "").endswith(SECRET):
            body = json.dumps({"choices": [{"message": {"content": "hi"}}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(401)
            self.end_headers()

    def log_message(self, *args):  # noqa: D401, ANN002 - silence
        return


def main():
    base = tempfile.mkdtemp(prefix="read-helper-")
    try:
        home = make_fixture(base)
        # ---- diagnose ----
        result, _ = call(HELPER, home, {"cmd": "diagnose"})
        check(result.get("ok") is True, "diagnose returns ok")
        data = result.get("data", {})
        gateway = data.get("gateway", {})
        check(gateway.get("unit") == "hermes-gateway", "gateway unit name")
        check(isinstance(gateway.get("listening"), bool), "gateway listening is a bool")
        channels = {row["name"]: row for row in data.get("channels", [])}
        check("weixin" in channels and "qqbot" in channels, "both channels parsed")
        check(channels.get("weixin", {}).get("last_activity") == "2026-10-17 10:00:00",
              "weixin last activity parsed")
        check(channels.get("weixin", {}).get("last_event", "").startswith("Sending response"),
              "weixin last event parsed")
        check("sync_mtime" in channels.get("weixin", {}), "weixin sync mtime exposed")
        check("context_tokens_mtime" in channels.get("weixin", {}),
              "weixin context-tokens mtime exposed")
        check(channels.get("qqbot", {}).get("connected") is True,
              "qqbot connected after Reconnected event")
        check(data.get("config", {}).get("mtime", 0) > 0, "config mtime present")
        check(len(data.get("logs", {}).get("gateway_tail", [])) == 9, "gateway tail has 9 lines")
        # ---- 脱敏：任何输出里都不得出现夹具 Key 原文 ----
        raw, _ = call(HELPER, home, {"cmd": "diagnose"})
        text = json.dumps(raw, ensure_ascii=False)
        check(SECRET not in text, "diagnose output does not leak the API key")
        check("***" in text, "diagnose output masks secrets with ***")
        # ---- probe：成功路径（本地假网关）----
        server = HTTPServer(("127.0.0.1", 0), ProbeHandler)
        port = server.server_address[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            result, _ = call(HELPER, home, {"cmd": "probe", "model": "alpha-1"},
                             {"HERMES_READ_PROBE_URL": "http://127.0.0.1:%d/v1/chat/completions" % port})
            check(result.get("ok") is True, "probe succeeds against the fake gateway")
            check(result.get("data", {}).get("status") == 200, "probe reports HTTP 200")
            check(isinstance(result.get("data", {}).get("ms"), int), "probe reports elapsed ms")
        finally:
            server.shutdown()
        # ---- probe：失败路径（端口无人监听）----
        result, _ = call(HELPER, home, {"cmd": "probe", "model": "alpha-1"},
                         {"HERMES_READ_PROBE_URL": "http://127.0.0.1:1/v1/chat/completions"})
        check(result.get("ok") is False, "probe fails when the gateway is down")
        check(result.get("data", {}).get("status") == 0, "probe failure reports status 0")
        # ---- 未知命令 ----
        result, proc = call(HELPER, home, {"cmd": "nope"})
        check(result.get("ok") is False and proc.returncode != 0, "unknown command is rejected")
        # ---- usage：没有 hermes-agent 时优雅失败 ----
        result, _ = call(HELPER, home, {"cmd": "usage", "days": 7})
        check(result.get("ok") is False, "usage degrades gracefully without hermes-agent")
        check("insights" in (result.get("error") or ""), "usage error names the data source")
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print("read-helper: %d passed, %d failed" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
