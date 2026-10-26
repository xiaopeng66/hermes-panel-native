"""审计截图台：Debug 隔离实例 + 假夹具 + 控制面导航，逐页逐主题出图（不受真实鼠标干扰）。

用法: python audit_shots.py <主题> [标签]
产出: E:/AI/Zcode/tmp/audit/<标签>-<页>.png（1:1 像素）
夹具刻意掺入长名字：供应商/模型/回退/备份文件名，用来暴露截断与换行问题。
"""
import ctypes, json, os, queue, shutil, subprocess, sys, threading, time
from ctypes import wintypes
from PIL import ImageGrab

PROJ = r"E:\AI\Zcode\projects\hermes-panel-native"
APP = os.path.join(PROJ, "build", "Debug", "hermes-panel-native.exe")
PROBE = os.path.join(PROJ, "build", "Debug", "geometry-probe.exe")
FAKE = os.path.join(PROJ, "build", "Debug", "fake-hermes.exe")
TMP = r"E:\AI\Zcode\tmp"
OUT = os.path.join(TMP, "audit")
DATA = os.path.join(TMP, "audit-data")
HOME = os.path.join(DATA, "fake")
theme = sys.argv[1] if len(sys.argv) > 1 else "porcelain"
tag = sys.argv[2] if len(sys.argv) > 2 else theme

shutil.rmtree(DATA, ignore_errors=True)
os.makedirs(HOME, exist_ok=True)
os.makedirs(OUT, exist_ok=True)
json.dump({"distro": "Ubuntu-24.04", "readHelperPath": "helper.py", "helperPath": "helper.py",
           "pollSeconds": 600, "theme": theme, "webuiPort": 8787, "gatewayPort": 8642},
          open(os.path.join(DATA, "settings.json"), "w", encoding="utf-8"), ensure_ascii=False)

LONG = "custom:very-long-provider-name-for-truncation-test"
providers = []
for i in range(9):
    name = LONG if i == 1 else ("P%d" % i)
    providers.append({"name": name, "provider_id": name if i == 1 else "custom:P%d" % i,
                      "base_url": "https://p%d.example/v1" % i, "key_env": "P%d_KEY" % i,
                      "key_set": True, "api_mode": "chat",
                      "default_model": ("deepseek-v4.1-flash-extremely-long-model-id-for-truncation"
                                        if i == 1 else "model-%d" % i),
                      "models": [("deepseek-v4.1-flash-extremely-long-model-id-for-truncation"
                                  if i == 1 else "model-%d" % i)] + ["extra-%d" % i]
                                 + (["m-%02d" % k for k in range(40)] if i == 0
                                    and os.environ.get("LONGACTIVE") else []),
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

u = ctypes.WinDLL("user32"); u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
VX, VY = u.GetSystemMetrics(76), u.GetSystemMetrics(77)
class R(ctypes.Structure): _fields_=[('l',wintypes.LONG),('t',wintypes.LONG),('r',wintypes.LONG),('b',wintypes.LONG)]

inst = "audit-%d" % os.getpid()
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
def panel_rect():
    px, py, pw, ph = pipe({"command": "dump"})["panel"]
    return px, py, pw, ph
def shot(name, box=None):
    px, py, pw, ph = panel_rect()
    img = ImageGrab.grab(all_screens=True).crop((px-VX, py-VY, px-VX+pw, py-VY+ph))
    (img.crop(box) if box else img).save(os.path.join(OUT, "%s-%s.png" % (tag, name)))
    return pw, ph
def tab(name):
    r = pipe({"command": "controls"})[name]
    pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
    time.sleep(0.7)
def wheel(delta, times=1):
    # 只发滚轮：先在面板中心点一下会命中列表行（长列表上弹出切换确认框，截到的是模态）。
    for _ in range(times):
        pipe({"command": "wheel", "delta": delta})
        time.sleep(0.12)
    time.sleep(0.4)
try:
    pipe({"command": "focus"})
    tab("tabModels"); shot("models-top")
    wheel(-360, 4); shot("models-scrolled")
    wheel(360, 8); tab("tabFallback"); shot("fallback")
    tab("tabAdd"); shot("add-new")
    tab("tabBackup"); shot("backup")
    tab("tabSettings"); shot("settings-top")
    wheel(-360, 6); shot("settings-bottom")
    print("OK", tag)
finally:
    try: probe.stdin.write("quit\n"); probe.stdin.flush()
    except OSError: pass
    time.sleep(0.3); proc.kill()
