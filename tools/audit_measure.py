"""几何审计：同一个隔离实例逐页取 controls/audit，量边距、滚动条一致性、行高。

用法: python audit_measure.py [主题]
产出: 标准输出 JSON（每页一段），供人工比对；不写文件。
"""
import ctypes, json, os, queue, shutil, subprocess, sys, threading, time
from ctypes import wintypes

PROJ = r"E:\AI\Zcode\projects\hermes-panel-native"
APP = os.path.join(PROJ, "build", "Debug", "hermes-panel-native.exe")
PROBE = os.path.join(PROJ, "build", "Debug", "geometry-probe.exe")
FAKE = os.path.join(PROJ, "build", "Debug", "fake-hermes.exe")
TMP = r"E:\AI\Zcode\tmp"
DATA = os.path.join(TMP, "audit-data")
HOME = os.path.join(DATA, "fake")
theme = sys.argv[1] if len(sys.argv) > 1 else "porcelain"
count = int(sys.argv[2]) if len(sys.argv) > 2 else 9

shutil.rmtree(DATA, ignore_errors=True)
os.makedirs(HOME, exist_ok=True)
json.dump({"distro": "Ubuntu-24.04", "readHelperPath": "helper.py", "helperPath": "helper.py",
           "pollSeconds": 600, "theme": theme, "webuiPort": 8787, "gatewayPort": 8642},
          open(os.path.join(DATA, "settings.json"), "w", encoding="utf-8"), ensure_ascii=False)

LONG = "custom:very-long-provider-name-for-truncation-test"
providers = []
for i in range(count):
    name = LONG if i == 1 else ("P%d" % i)
    providers.append({"name": name, "provider_id": name if i == 1 else "custom:P%d" % i,
                      "base_url": "https://p%d.example/v1" % i, "key_env": "P%d_KEY" % i,
                      "key_set": True, "api_mode": "chat",
                      "default_model": ("deepseek-v4.1-flash-extremely-long-model-id-for-truncation"
                                        if i == 1 else "model-%d" % i),
                      "models": [("deepseek-v4.1-flash-extremely-long-model-id-for-truncation"
                                  if i == 1 else "model-%d" % i)] + ["extra-%d" % i],
                      "models_discovered": i == 1, "is_active": i == 0, "line": 10 * i})
fallbacks = [{"provider": ("custom:very-long-provider-name-for-truncation-test" if i == 3
                           else "custom:P%d" % i),
              "model": "model-%d" % i, "base_url": "https://p%d.example/v1" % i,
              "key_env": "P%d_KEY" % i, "key_set": True} for i in range(11)]
backups = [{"file": ("config-20261026_12000%d-sync-models-very-long-provider-name-for-truncation.yaml" % i
                     if i == 2 else "config-20261026_12000%d-manual.yaml" % i),
            "kind": "config" if i % 3 else "env", "size": 4096 + i,
            "mtime_text": "2026-10-26 12:00:0%d" % i} for i in range(6)]
state = {"model": {"default": "model-0", "provider": "custom:P0", "base_url": "https://p0.example/v1",
                   "api_mode": "chat_completions", "supports_vision": True},
         "providers": providers, "fallbacks": fallbacks, "backups": backups,
         "config_mtime": 1793000000}
json.dump(state, open(os.path.join(HOME, "state.json"), "w", encoding="utf-8"), ensure_ascii=False)

u = ctypes.WinDLL("user32")
class R(ctypes.Structure): _fields_=[('l',wintypes.LONG),('t',wintypes.LONG),('r',wintypes.LONG),('b',wintypes.LONG)]

inst = "measure-%d" % os.getpid()
env = dict(os.environ)
env.update({"HERMES_WSL_EXE": FAKE, "HERMES_DISABLE_UNC": "1", "FAKE_HERMES_HOME": HOME})
proc = subprocess.Popen([APP, "--instance", inst, "--data-dir", DATA,
                         "--legacy-dir", os.path.join(DATA, "no-legacy")], env=env)
time.sleep(5.5)
probe = subprocess.Popen([PROBE, "--id", inst], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         text=True, encoding="utf-8", errors="replace")
probe.stdout.readline()
q = queue.Queue(); threading.Thread(target=lambda: [q.put(l) for l in probe.stdout], daemon=True).start()
def pipe(req, timeout=40.0):
    probe.stdin.write("pipe " + json.dumps(req) + "\n"); probe.stdin.flush()
    return json.loads(q.get(timeout=timeout).strip())
def tab(name):
    r = pipe({"command": "controls"})[name]
    pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
    time.sleep(0.7)
def wheel(delta, times=1):
    px, py, pw, ph = pipe({"command": "dump"})["panel"]
    pipe({"command": "tap", "x": px+pw//2, "y": py+ph//2, "screen": True})
    for _ in range(times):
        pipe({"command": "wheel", "delta": delta}); time.sleep(0.12)
    time.sleep(0.4)

def summary(page):
    c = pipe({"command": "controls"})
    a = pipe({"command": "audit"})
    out = {"page": page, "client": c["_client"], "content": c["_content"],
           "margins": {"left": c["_content"]["left"],
                       "right": c["_client"]["width"]-c["_content"]["right"]},
           "scrolls": c["_scrolls"], "auditIssues": a["count"], "issues": a["issues"][:6]}
    items = [i for i in c["_list"]["items"] if "kind" in i]
    if page == "models":
        heights = {}
        for i in items:
            heights.setdefault(i["kind"], []).append(i["rect"]["height"])
        out["itemHeights"] = {k: sorted(set(v)) for k, v in heights.items()}
        out["listViewport"] = c["_list"] if "viewport" in c["_list"] else None
    if page == "fallback":
        out["ladderRowHeights"] = sorted({r["rect"]["height"] for r in c["_list"]["ladder"]})
    if page == "add":
        out["fieldBoxes"] = c["_fields"]
        out["fieldHeights"] = sorted({f["height"] for f in c["_fields"]})
        out["switchRows"] = c["_switches"]
    if page == "settings":
        out["switchRows"] = c["_switches"]
        out["switchTops"] = [s["y"] for s in c["_switches"]]
        out["switchHeights"] = sorted({s["height"] for s in c["_switches"]})
    return out

try:
    pipe({"command": "focus"})
    reports = []
    tab("tabModels"); reports.append(summary("models"))
    tab("tabFallback"); reports.append(summary("fallback"))
    tab("tabAdd"); reports.append(summary("add"))
    tab("tabBackup"); reports.append(summary("backup"))
    tab("tabSettings"); reports.append(summary("settings"))
    wheel(-360, 6); reports.append(summary("settings-scrolled"))
    print(json.dumps(reports, ensure_ascii=False, indent=1))
finally:
    try: probe.stdin.write("quit\n"); probe.stdin.flush()
    except OSError: pass
    time.sleep(0.3); proc.kill()
