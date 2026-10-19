#!/usr/bin/env python3
"""hermes-panel-read.py — 面板的只读助手（diagnose / usage / probe）。

与 hermes-panel-apply.py 并列：协议相同（stdin 一行 JSON，stdout 一行 JSON），但**永不写**
config.yaml / .env —— 只读文件、只发网络探针。apply 脚本仍是唯一写入者。

用法：
    echo '{"cmd":"diagnose"}' | python3 hermes-panel-read.py
    echo '{"cmd":"usage","days":7}' | python3 hermes-panel-read.py
    echo '{"cmd":"probe","model":"x"}' | python3 hermes-panel-read.py

环境变量（测试用）：
    HERMES_READ_HOME     覆盖 ~/.hermes（夹具）
    HERMES_READ_PROBE_URL 覆盖探针地址（夹具的 loopback 假服务器）
"""

import json
import os
import re
import subprocess
import sys
import time
import urllib.request

VERSION = "1.0.0"
HERMES_HOME = os.path.expanduser(os.environ.get("HERMES_READ_HOME", "~/.hermes"))
CONFIG = os.path.join(HERMES_HOME, "config.yaml")
ENV_FILE = os.path.join(HERMES_HOME, ".env")
LOG_DIR = os.path.join(HERMES_HOME, "logs")
WEIXIN_ACCOUNTS = os.path.join(HERMES_HOME, "weixin", "accounts")
GATEWAY_PORT = 8642

# 输出前脱敏：API key / token / Bearer 一律掩码（门禁断言输出里不含夹具 Key 原文）。
SECRET_PATTERNS = [
    re.compile(r"(?i)(api[_-]?key|api[_-]?server[_-]?key|token|secret|password)\s*[=:]\s*\S+"),
    re.compile(r"(?i)bearer\s+[A-Za-z0-9._\-]{8,}"),
    re.compile(r"\bsk-[A-Za-z0-9._\-]{8,}"),
]


def redact(text):
    out = text
    for pattern in SECRET_PATTERNS:
        out = pattern.sub(lambda m: m.group(0).split("=")[0].split(":")[0] + "=***"
                          if "=" in m.group(0) or ":" in m.group(0) else "Bearer ***", out)
    return out


def tail(path, lines):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            data = handle.readlines()
        return [redact(line.rstrip("\n")) for line in data[-lines:]]
    except OSError:
        return []


def run(cmd, timeout=5):
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
        return result.returncode, result.stdout.strip(), result.stderr.strip()
    except Exception:  # noqa: BLE001
        return -1, "", ""


def mtime(path):
    try:
        st = os.stat(path)
        return int(st.st_mtime), st.st_size
    except OSError:
        return 0, 0


def diagnose():
    # ---- 网关单元 ----
    gateway = {"unit": "hermes-gateway", "active_state": "unknown", "active_since": "",
               "main_pid": 0, "listening": False}
    # systemctl show 的 --value 多属性输出顺序不可靠（实测第一行是 MainPID 而非 ActiveState），
    # 改成解析完整的 key=value 输出。
    code, out, _ = run(["systemctl", "--user", "show", "hermes-gateway", "--no-pager"])
    if code == 0 and out:
        values = {}
        for line in out.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                values[key] = value
        gateway["active_state"] = values.get("ActiveState") or "unknown"
        gateway["active_since"] = values.get("ActiveEnterTimestamp") or ""
        try:
            gateway["main_pid"] = int(values.get("MainPID") or 0)
        except ValueError:
            gateway["main_pid"] = 0
    code, out, _ = run(["ss", "-tln"])
    if code == 0:
        gateway["listening"] = any(":%d " % GATEWAY_PORT in line for line in out.splitlines())

    # ---- 日志尾部 ----
    logs = {
        "gateway_tail": tail(os.path.join(LOG_DIR, "gateway.log"), 40),
        "errors_tail": tail(os.path.join(LOG_DIR, "errors.log"), 20),
    }

    # ---- 通道状态：从 gateway.log 聚合每个平台最近一条事件 ----
    channels = {}
    platform_re = re.compile(r"gateway\.platforms\.([a-z_]+)")
    bracket_re = re.compile(r"\[([A-Za-z]+)\]")
    connect_re = re.compile(r"(?i)\b(Connected|Disconnected|Reconnected|Resume sent|"
                            r"Session resumed|Session timed out|listening|inbound|Sending response|"
                            r"background handler start|rate limited)\b")
    connect_events = {"Connected", "Reconnected", "Resume sent", "Session resumed", "listening"}
    disconnect_events = {"Disconnected", "Session timed out"}
    try:
        with open(os.path.join(LOG_DIR, "gateway.log"), "r", encoding="utf-8",
                  errors="replace") as handle:
            data = handle.readlines()
    except OSError:
        data = []
    # 倒序扫描：每个平台第一次遇到的事件就是 last_activity/last_event；connected 取第一条
    # **连接类**事件（消息事件不改变连接状态）。平台名优先用 [Name] 前缀——响应发送走
    # gateway.platforms.base，但括号里仍是 [Weixin]。
    seen_connect = set()
    for line in reversed(data):
        bracket = bracket_re.search(line)
        match = platform_re.search(line)
        name = bracket.group(1).lower() if bracket else (match.group(1) if match else None)
        if not name:
            continue
        entry = channels.setdefault(name, {"name": name, "last_activity": "", "last_event": "",
                                           "connected": False})
        event = connect_re.search(line)
        if not entry["last_activity"]:
            entry["last_activity"] = line[:19]
            entry["last_event"] = redact(event.group(1) if event else
                                         line.split(":", 2)[-1].strip()[:120])
        if name not in seen_connect and event and (
                event.group(1) in connect_events or event.group(1) in disconnect_events):
            seen_connect.add(name)
            entry["connected"] = event.group(1) in connect_events

    # ---- 微信的失联判据：sync.json 在更新、context-tokens.json 不动 = 服务器不再投递 ----
    weixin_extra = {}
    try:
        for entry in os.listdir(WEIXIN_ACCOUNTS):
            if entry.endswith(".sync.json"):
                sync_mtime, _ = mtime(os.path.join(WEIXIN_ACCOUNTS, entry))
                base = entry[:-len(".sync.json")]
                ctx_mtime, _ = mtime(os.path.join(WEIXIN_ACCOUNTS, base + ".context-tokens.json"))
                weixin_extra = {"sync_mtime": sync_mtime, "context_tokens_mtime": ctx_mtime}
                break
    except OSError:
        pass
    if weixin_extra:
        channels.setdefault("weixin", {"name": "weixin", "last_activity": "", "last_event": "",
                                       "connected": False})
        channels["weixin"].update(weixin_extra)

    config_mtime, config_size = mtime(CONFIG)
    now = int(time.time())
    for entry in channels.values():
        if entry.get("last_activity"):
            try:
                stamp = time.mktime(time.strptime(entry["last_activity"], "%Y-%m-%d %H:%M:%S"))
                entry["stale_seconds"] = max(0, now - int(stamp))
            except ValueError:
                entry["stale_seconds"] = 0
        else:
            entry["stale_seconds"] = 0

    return {
        "version": VERSION,
        "gateway": gateway,
        "channels": list(channels.values()),
        "logs": logs,
        "config": {"mtime": config_mtime, "size": config_size},
    }


def usage(payload):
    days = int(payload.get("days", 7) or 7)
    days = max(1, min(days, 365))
    try:
        sys.path.insert(0, os.path.join(HERMES_HOME, "hermes-agent"))
        from hermes_state import SessionDB  # type: ignore
        from agent.insights import InsightsEngine  # type: ignore
    except Exception as exc:  # noqa: BLE001
        return {"ok": False, "error": "insights unavailable: %s" % exc}
    db = None
    try:
        db = SessionDB(read_only=True)
        report = InsightsEngine(db).generate(days=days, source=None)
    except Exception as exc:  # noqa: BLE001
        return {"ok": False, "error": "insights failed: %s" % exc}
    finally:
        if db is not None:
            try:
                db.close()
            except Exception:  # noqa: BLE001
                pass
    report = report if isinstance(report, dict) else {}
    overview = report.get("overview") or {}
    models = []
    for row in (report.get("models") or [])[:12]:
        models.append({
            "model": row.get("model", ""),
            "sessions": row.get("sessions", 0),
            "input_tokens": row.get("input_tokens", 0),
            "output_tokens": row.get("output_tokens", 0),
            "total_tokens": row.get("total_tokens",
                                    int(row.get("input_tokens", 0)) + int(row.get("output_tokens", 0))),
        })
    platforms = [{"platform": row.get("platform", ""), "sessions": row.get("sessions", 0),
                  "messages": row.get("messages", 0)} for row in (report.get("platforms") or [])[:8]]
    activity = (report.get("activity") or {}).get("by_day") or []
    return {
        "ok": True,
        "data": {
            "days": days,
            "empty": bool(report.get("empty", False)),
            "generated_at": int(report.get("generated_at", time.time())),
            "overview": {
                # insights 的 overview 用 total_ 前缀（实测：total_input_tokens 等）。
                "total_sessions": overview.get("total_sessions", 0),
                "total_messages": overview.get("total_messages", 0),
                "input_tokens": overview.get("total_input_tokens", 0),
                "output_tokens": overview.get("total_output_tokens", 0),
                "cache_read_tokens": overview.get("total_cache_read_tokens", 0),
                "total_tokens": overview.get("total_tokens", 0),
                "avg_msgs_session": overview.get("avg_messages_per_session", 0),
            },
            "models": models,
            "platforms": platforms,
            "activity": activity[:14],
        },
    }


def read_env_key(name):
    try:
        with open(ENV_FILE, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if line.startswith(name + "="):
                    value = line.split("=", 1)[1].strip().strip('"').strip("'")
                    return value
    except OSError:
        pass
    return ""


def probe(payload):
    model = str(payload.get("model") or "").strip()
    url = os.environ.get("HERMES_READ_PROBE_URL") or (
        "http://127.0.0.1:%d/v1/chat/completions" % GATEWAY_PORT)
    key = read_env_key("API_SERVER_KEY")
    if not key:
        return {"ok": False, "error": "API_SERVER_KEY not found in .env"}
    body = json.dumps({"model": model, "messages": [{"role": "user", "content": "ping"}],
                       "max_tokens": 1}).encode("utf-8")
    request = urllib.request.Request(url, data=body, method="POST",
                                     headers={"Content-Type": "application/json",
                                              "Authorization": "Bearer " + key})
    started = time.monotonic()
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            response.read(64)
            elapsed = int((time.monotonic() - started) * 1000)
            return {"ok": True, "data": {"ms": elapsed, "status": response.status, "model": model,
                                         "error": ""}}
    except Exception as exc:  # noqa: BLE001
        elapsed = int((time.monotonic() - started) * 1000)
        return {"ok": False, "data": {"ms": elapsed, "status": 0, "model": model,
                                      "error": type(exc).__name__}}


COMMANDS = {"diagnose": lambda payload: {"ok": True, "data": diagnose()},
            "usage": usage,
            "probe": probe}


def main():
    raw = sys.stdin.readline()
    if not raw.strip():
        print(json.dumps({"ok": False, "error": "empty request"}))
        return 2
    try:
        payload = json.loads(raw)
    except Exception:  # noqa: BLE001
        print(json.dumps({"ok": False, "error": "request is not valid JSON"}))
        return 2
    if not isinstance(payload, dict):
        print(json.dumps({"ok": False, "error": "request must be a JSON object"}))
        return 2
    handler = COMMANDS.get(payload.get("cmd") or "")
    if not handler:
        print(json.dumps({"ok": False, "error": "unknown cmd: %s" % payload.get("cmd")}))
        return 2
    try:
        result = handler(payload)
    except Exception as exc:  # noqa: BLE001
        result = {"ok": False, "error": "%s: %s" % (type(exc).__name__, exc)}
    try:
        print(json.dumps(result, ensure_ascii=False))
    except (UnicodeEncodeError, BrokenPipeError):
        print(json.dumps({"ok": False, "error": "response encoding failed"}, ensure_ascii=True))
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
