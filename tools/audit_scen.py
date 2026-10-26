"""交互场景截图：长组展开、搜索结果扁平视图（确定性，无真实鼠标）。

用法: python audit_scen.py <场景> [标签] [主题]
场景: longgroup | search | addselect
"""
import ctypes, json, os, queue, shutil, subprocess, sys, time
from ctypes import wintypes
from PIL import Image, ImageGrab

PROJ = r"E:\AI\Zcode\projects\hermes-panel-native"
APP = os.path.join(PROJ, "build", "Debug", "hermes-panel-native.exe")
PROBE = os.path.join(PROJ, "build", "Debug", "geometry-probe.exe")
FAKE = os.path.join(PROJ, "build", "Debug", "fake-hermes.exe")
TMP = r"E:\AI\Zcode\tmp"
OUT = os.path.join(TMP, "audit")
DATA = os.path.join(TMP, "audit-data")
HOME = os.path.join(DATA, "fake")
scenario = sys.argv[1] if len(sys.argv) > 1 else "longgroup"
tag = sys.argv[2] if len(sys.argv) > 2 else scenario
theme = sys.argv[3] if len(sys.argv) > 3 else "porcelain"

shutil.rmtree(DATA, ignore_errors=True)
os.makedirs(HOME, exist_ok=True)
os.makedirs(OUT, exist_ok=True)
json.dump({"distro": "Ubuntu-24.04", "readHelperPath": "helper.py", "helperPath": "helper.py",
           "pollSeconds": 600, "theme": theme, "webuiPort": 8787, "gatewayPort": 8642},
          open(os.path.join(DATA, "settings.json"), "w", encoding="utf-8"), ensure_ascii=False)

LONG = "custom:very-long-provider-name-for-truncation-test"
LONGMODEL = "deepseek-v4.1-flash-extremely-long-model-id-for-truncation-and-overflow-check"
providers = []
for i in range(9):
    providers.append({
        "name": LONG if i in (1, 4) else ("P%d" % i),
        "provider_id": LONG if i in (1, 4) else "custom:P%d" % i,
        "base_url": "https://p%d.example/v1" % i, "key_env": "P%d_KEY" % i,
        "key_set": True, "api_mode": "chat",
        "default_model": LONGMODEL if i in (1, 4) else "model-%d" % i,
        "models": ([LONGMODEL] + ["extra-%d" % i] + (["x-%02d" % k for k in range(20)] if i == 1 else []))
                  if i in (1, 4) else ["model-%d" % i, "extra-%d" % i],
        "models_discovered": i in (1, 4), "is_active": i == 0, "line": 10 * i})
fallbacks = [{"provider": LONG if i == 3 else "custom:P%d" % i,
              "model": LONGMODEL if i == 3 else "model-%d" % i,
              "base_url": "https://p%d.example/v1" % i, "key_env": "P%d_KEY" % i,
              "key_set": True} for i in range(11)]
backups = [{"file": ("config-20261026_12000%d-sync-models-very-long-provider-name-for-truncation.yaml" % i
                     if i == 2 else "config-20261026_12000%d-manual.yaml" % i),
            "kind": "config" if i % 3 else "env", "size": 4096 + i,
            "mtime_text": "2026-10-26 12:00:0%d" % i} for i in range(6)]
state = {"model": {"default": "model-0", "provider": "custom:P0", "base_url": "https://p0.example/v1",
                   "api_mode": "chat_completions", "supports_vision": True},
         "providers": providers, "fallbacks": fallbacks, "backups": backups,
         "config_mtime": 1793000000}
if scenario == "empty":
    # 直接改 state（上面已经用它建好 dict，改局部 list 不会生效）
    state["providers"] = []
    state["fallbacks"] = []
    state["backups"] = []
    state["model"] = {"default": "", "provider": "", "base_url": "", "api_mode": "",
                      "supports_vision": False}
if scenario in ("texts", "striplong"):
    # 当前模型用最长名字：hero、扁平视图、回退链、托盘提示都要截断
    state["model"]["default"] = LONGMODEL
    state["model"]["provider"] = LONG
if scenario == "error":
    open(os.path.join(HOME, "state.json"), "w", encoding="utf-8").write("{ not json at all ]")
else:
    json.dump(state, open(os.path.join(HOME, "state.json"), "w", encoding="utf-8"),
              ensure_ascii=False)

u = ctypes.WinDLL("user32"); u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
VX, VY = u.GetSystemMetrics(76), u.GetSystemMetrics(77)
class R(ctypes.Structure): _fields_=[('l',wintypes.LONG),('t',wintypes.LONG),('r',wintypes.LONG),('b',wintypes.LONG)]

inst = "scen-%d" % os.getpid()
env = dict(os.environ)
env.update({"HERMES_WSL_EXE": FAKE, "HERMES_DISABLE_UNC": "1", "FAKE_HERMES_HOME": HOME})
if scenario in ("offline", "offdiag"):
    env["HERMES_WSL_EXE"] = os.path.join(DATA, "no-such-wsl.exe")
proc = subprocess.Popen([APP, "--instance", inst, "--data-dir", DATA,
                         "--legacy-dir", os.path.join(DATA, "no-legacy")], env=env)
time.sleep(5.5)
probe = subprocess.Popen([PROBE, "--id", inst], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         text=True, encoding="utf-8", errors="replace")
probe.stdout.readline()
q = queue.Queue(); import threading
threading.Thread(target=lambda: [q.put(l) for l in probe.stdout], daemon=True).start()
def pipe(req, timeout=40.0):
    probe.stdin.write("pipe " + json.dumps(req) + "\n"); probe.stdin.flush()
    return json.loads(q.get(timeout=timeout).strip())
def grab_window():
    """把面板临时提到最上层再抓屏：D2D 窗口不吃 PrintWindow（拿到全白），
    而这块副屏上常有别的窗口压着。抓完恢复非置顶（app 自己的置顶设置不受影响）。"""
    import ctypes.wintypes as wt
    hwnd = wt.HWND(pipe({"command": "hit", "x": 0, "y": 0})["panel"])
    u2 = ctypes.WinDLL("user32")
    SWP_NOMOVE, SWP_NOSIZE, SWP_NOACTIVATE = 0x0002, 0x0001, 0x0010
    HWND_TOPMOST, HWND_NOTOPMOST = wt.HWND(-1), wt.HWND(-2)
    u2.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
    time.sleep(0.25)
    px, py, pw, ph = pipe({"command": "dump"})["panel"]
    img = ImageGrab.grab(all_screens=True).crop((px-VX, py-VY, px-VX+pw, py-VY+ph))
    u2.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
    return img

def shot(name):
    px, py, pw, ph = pipe({"command": "dump"})["panel"]
    img = ImageGrab.grab(all_screens=True).crop((px-VX, py-VY, px-VX+pw, py-VY+ph))
    img.save(os.path.join(OUT, "%s-%s.png" % (tag, name)))
try:
    pipe({"command": "focus"})
    if scenario == "longgroup":
        items = pipe({"command": "controls"})["_list"]["items"]
        head = [i for i in items if i.get("kind") == 0]
        # group 1 = 第一个长名字供应商（index 1）
        target = [i for i in head if i.get("group") == 1][0]
        r = target["rect"]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(0.9)
        shot("longgroup")
        pipe({"command": "wheel", "delta": -120}); time.sleep(0.4)
        shot("longgroup-2")
    elif scenario == "search":
        search = pipe({"command": "controls"})["searchBox"]
        pipe({"command": "tap", "x": search["x"]+search["width"]//2,
              "y": search["y"]+search["height"]//2, "screen": True})
        time.sleep(0.5)
        pipe({"command": "key", "text": "deepseek"})
        time.sleep(0.8)
        shot("search")
    elif scenario in ("empty", "error", "offline"):
        shot("models")
        r = pipe({"command": "controls"})["tabFallback"]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(0.9); shot("fallback")
        r = pipe({"command": "controls"})["tabBackup"]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(0.9); shot("backup")
    elif scenario == "texts":
        shot("models-hero")
        r = pipe({"command": "controls"})["tabFallback"]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(0.9); shot("fallback")
        r = pipe({"command": "controls"})["tabBackup"]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(0.9); shot("backup")
    elif scenario == "offdiag":
        c = pipe({"command": "controls"})
        print("client", c["_client"], "content", c["_content"])
        print("origin", c["_origin"], "scale", c["_scale"])
        for k in ("searchBox", "refresh", "offlineStart", "_offline", "tabModels"):
            print(k, c.get(k))
        print("audit", json.dumps(pipe({"command": "audit"}), ensure_ascii=False)[:600])
        shot("offdiag")
    elif scenario == "fields":
        r = pipe({"command": "controls"})["tabAdd"]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(0.8)
        f = pipe({"command": "controls"})["_fields"]
        for index, text in ((0, "my-relay-very-long-name-for-truncation-check"),
                            (1, "https://api.example.com/v1"),
                            (2, "sk-1234567890abcdefghijklmnop")):
            box = f[index]
            pipe({"command": "tap", "x": box["x"]+box["width"]//2,
                  "y": box["y"]+box["height"]//2, "screen": True})
            time.sleep(0.4)
            pipe({"command": "key", "text": text})
            time.sleep(0.4)
        shot("fields-typed")
        print("add fields:", json.dumps(pipe({"command": "dump"})["add"]["fields"], ensure_ascii=False))
        c = pipe({"command": "controls"})
        print("keyReveal", c.get("keyReveal"))
        if "keyReveal" in c:
            r = c["keyReveal"]
            pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
            time.sleep(0.8)
        shot("fields-revealed")
    elif scenario == "setfield":
        r = pipe({"command": "controls"})["tabSettings"]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(0.8)
        f = pipe({"command": "controls"})["_fields"]
        for index, text in ((0, "-LTS-extra-long-distro-name"), (2, "1234")):
            box = f[index]
            pipe({"command": "tap", "x": box["x"]+box["width"]//2,
                  "y": box["y"]+box["height"]//2, "screen": True})
            time.sleep(0.4)
            pipe({"command": "key", "text": text})
            time.sleep(0.5)
            shot("setfield-%d" % index)
    elif scenario == "hover":
        c = pipe({"command": "controls"})
        ox, oy = c["_origin"]["x"], c["_origin"]["y"]
        # 1) 展开的长组里悬停一条非当前行
        items = c["_list"]["items"]
        heads = [i for i in items if i.get("kind") == 0 and i.get("group") == 1]
        if heads:
            r = heads[0]["rect"]
            pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
            time.sleep(0.8)
        c = pipe({"command": "controls"})
        rows = [i for i in c["_list"]["items"] if i.get("kind") == 1 and i.get("group") == 1]
        if len(rows) > 1:
            r = rows[1]["rect"]
            pipe({"command": "hover", "x": r["x"]-ox+30, "y": r["y"]-oy+r["height"]//2})
            time.sleep(0.3)
        shot("row")
        # 2) 悬停组头
        c = pipe({"command": "controls"})
        head = [i for i in c["_list"]["items"] if i.get("kind") == 0 and i.get("group") == 1][0]
        r = head["rect"]
        pipe({"command": "hover", "x": r["x"]-ox+60, "y": r["y"]-oy+r["height"]//2})
        time.sleep(0.3)
        shot("grouphead")
        # 3) 悬停底栏 tab
        r = c["tabFallback"]
        pipe({"command": "hover", "x": r["x"]-ox+r["width"]//2, "y": r["y"]-oy+r["height"]//2})
        time.sleep(0.3)
        shot("tab")
        # 4) 悬停刷新同步按钮
        r = c["syncModels"]
        pipe({"command": "hover", "x": r["x"]-ox+r["width"]//2, "y": r["y"]-oy+r["height"]//2})
        time.sleep(0.3)
        shot("sync")
        # 5) 悬停列表滑条（需可滚动）：悬停右缘拇指位置
        sc = c["_scrolls"]["models"]
        print("models scroll", sc["thumb"], sc["track"])
    elif scenario == "scrollhover":
        r = pipe({"command": "controls"})["tabFallback"]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(0.9)
        c = pipe({"command": "controls"})
        ox, oy = c["_origin"]["x"], c["_origin"]["y"]
        th = c["_scrolls"]["fallback"]["thumb"]
        shot("before")
        print("hover ->", pipe({"command": "hover", "x": th["x"]-ox+th["width"]//2,
              "y": th["y"]-oy+th["height"]//2}))
        time.sleep(0.35)
        d = pipe({"command": "dump"})
        print("pointer", d["models"]["pointer"])
        shot("hot")
        print("thumb", th, "track", c["_scrolls"]["fallback"]["track"])
    elif scenario == "setscroll":
        r = pipe({"command": "controls"})["tabSettings"]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(0.9)
        c = pipe({"command": "controls"})
        ox, oy = c["_origin"]["x"], c["_origin"]["y"]
        th = c["_scrolls"]["page"]["thumb"]
        print("page thumb", th, "max", c["_scrolls"]["page"]["maxScroll"])
        shot("before")
        print("pre", pipe({"command": "controls"})["_pointer"],
              pipe({"command": "controls"})["_scrolls"]["page"]["hot"])
        pipe({"command": "hover", "x": th["x"]-ox+th["width"]//2, "y": th["y"]-oy+th["height"]//2})
        time.sleep(0.35)
        c2 = pipe({"command": "controls"})
        print("post", c2["_pointer"], c2["_scrolls"]["page"]["hot"])
        shot("hot")
    elif scenario == "diagmodal":
        r = pipe({"command": "controls"})["tabSettings"]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(0.9)
        for _ in range(6):
            pipe({"command": "wheel", "delta": -360}); time.sleep(0.12)
        time.sleep(0.5)
        c = pipe({"command": "controls"})
        r = c["setDiagnose"]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(1.0)
        d = pipe({"command": "dump"})
        m = d["modal"]
        print("title", m.get("title"))
        print("facts", len(m.get("facts", [])), json.dumps(m.get("facts", [])[:4], ensure_ascii=False)[:300])
        a = pipe({"command": "audit"})
        print("audit", a["count"], a["issues"][:3])
        px, py, pw, ph = d["panel"]
        ImageGrab.grab(all_screens=True).crop((px-VX, py-VY, px-VX+pw, py-VY+ph)).save(
            os.path.join(OUT, "%s-modal.png" % tag))
    elif scenario == "themes":
        def panel_shot(name):
            px, py, pw, ph = pipe({"command": "dump"})["panel"]
            ImageGrab.grab(all_screens=True).crop((px-VX, py-VY, px-VX+pw, py-VY+ph)).save(
                os.path.join(OUT, "%s-%s.png" % (tag, name)))
        for theme in ("dark", "light", "slate", "carbon", "porcelain", "sand"):
            pipe({"command": "configure", "theme": theme})
            time.sleep(0.6)
            c = pipe({"command": "controls"})
            r = c["tabFallback"]
            pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
            time.sleep(0.8)
            panel_shot("fallback-" + theme)
            c = pipe({"command": "controls"})
            r = c["tabSettings"]
            pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
            time.sleep(0.8)
            for _ in range(6):
                pipe({"command": "wheel", "delta": -360}); time.sleep(0.1)
            time.sleep(0.4)
            panel_shot("paged-" + theme)
            c = pipe({"command": "controls"})
            r = c["setDiagnose"]
            pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
            time.sleep(0.9)
            panel_shot("modal-" + theme)
            c = pipe({"command": "controls"})
            if "modalCancel" in c:
                r = c["modalCancel"]
                pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
            else:
                pipe({"command": "vk", "vk": 27})
            time.sleep(0.5)
        print("themes done")
    elif scenario == "strip":
        from PIL import ImageGrab
        def shot_rect(name, rect, pad=16):
            x, y, w, h = rect
            img = ImageGrab.grab(all_screens=True).crop((x-VX-pad, y-VY-pad, x-VX+w+pad, y-VY+h+pad))
            img.save(os.path.join(OUT, "%s-%s.png" % (tag, name)))
            return img.size
        for style in ("tag", "capsule"):
            for theme in ("porcelain", "dark"):
                pipe({"command": "configure", "widgetStyle": style, "theme": theme})
                time.sleep(0.4)
                pipe({"command": "place", "state": 4, "edge": 1, "float": [400, 300]})
                time.sleep(0.8)
                d = pipe({"command": "dump"})
                print(style, theme, "strip", d["strip"])
                shot_rect("strip-%s-%s" % (style, theme), d["strip"])
                pipe({"command": "strip-card", "show": True})
                time.sleep(0.6)
                d = pipe({"command": "dump"})
                print("  card", d["hoverCard"] if "hoverCard" in d else "?")
                img = ImageGrab.grab(all_screens=True)
                # 卡片在贴条旁边：抓一块足够大的区域
                x, y, w, h = d["strip"]
                img.crop((max(0,x-VX-220), max(0,y-VY-40), x-VX+w+240, y-VY+h+80)).save(
                    os.path.join(OUT, "%s-card-%s-%s.png" % (tag, style, theme)))
                pipe({"command": "strip-card", "show": False})
                time.sleep(0.3)
    elif scenario == "striplong":
        for style in ("tag", "capsule"):
            pipe({"command": "configure", "widgetStyle": style, "theme": "porcelain"})
            time.sleep(0.4)
            pipe({"command": "place", "state": 4, "edge": 1, "float": [400, 300]})
            time.sleep(0.8)
            d = pipe({"command": "dump"})
            print(style, "strip", d["strip"])
            x, y, w, h = d["strip"]
            ImageGrab.grab(all_screens=True).crop((max(0,x-VX-12), max(0,y-VY-12),
                x-VX+w+12, y-VY+h+12)).resize(((w+24)*3,(h+24)*3), Image.LANCZOS).save(
                os.path.join(OUT, "%s-%s.png" % (tag, style)))
    elif scenario == "striph":
        for style, edge in (("capsule", 2), ("tag", 4), ("tag", 2)):
            pipe({"command": "configure", "widgetStyle": style, "theme": "porcelain"})
            time.sleep(0.3)
            pipe({"command": "place", "state": 4, "edge": edge, "float": [400, 300]})
            time.sleep(0.7)
            d = pipe({"command": "dump"})
            x, y, w, h = d["strip"]
            print(style, "edge", edge, d["strip"])
            pad = 14
            ImageGrab.grab(all_screens=True).crop((max(0,x-VX-pad), max(0,y-VY-pad),
                x-VX+w+pad, y-VY+h+pad)).resize(((w+2*pad)*3,(h+2*pad)*3), Image.LANCZOS).save(
                os.path.join(OUT, "%s-%s-e%d.png" % (tag, style, edge)))
    elif scenario == "dpi":
        mons = pipe({"command": "monitors"})
        print(json.dumps(mons, ensure_ascii=False)[:800])
        items = mons.get("monitors", mons) if isinstance(mons, dict) else mons
        pick = None
        for m in items:
            if isinstance(m, dict) and int(m.get("dpi", 96)) >= 168:
                pick = m
                break
        if pick is None:
            print("no high-dpi monitor")
        else:
            dpi = int(pick.get("dpi", 192))
            b = pick.get("bounds", [0, 0, 0, 0])
            left, top = int(b[0]), int(b[1])
            print("pick", pick)
            pipe({"command": "place", "state": 3, "edge": 0,
                  "float": [left + 200, top + 200], "monitorPoint": [left + 300, top + 300]})
            time.sleep(1.2)
            d = pipe({"command": "dump"})
            print("panel", d["panel"])
            px, py, pw, ph = d["panel"]
            try:
                grab_window().save(os.path.join(OUT, "%s-models.png" % tag))
            except Exception as exc:
                print("PrintWindow failed:", exc)
                ImageGrab.grab(all_screens=True).crop((px-VX, py-VY, px-VX+pw, py-VY+ph)).save(
                    os.path.join(OUT, "%s-models.png" % tag))
            c = pipe({"command": "controls"})
            a = pipe({"command": "audit"})
            print("scale", d["fit"]["scale"], "content", c["_content"],
                  "margins", (c["_content"]["left"], c["_client"]["width"]-c["_content"]["right"]))
            print("scrolls", json.dumps(c["_scrolls"]["models"], ensure_ascii=False))
            print("audit", a["count"], a["issues"][:4])
            for name in ("tabFallback", "tabBackup", "tabSettings"):
                r = c[name]
                pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
                time.sleep(0.9)
                c = pipe({"command": "controls"})
                d = pipe({"command": "dump"})
                px, py, pw, ph = d["panel"]
                print(name, "panel", d["panel"], "monitor", d["monitorId"])
                try:
                    grab_window().save(os.path.join(OUT, "%s-%s.png" % (tag, name)))
                except Exception as exc:
                    print("PrintWindow failed:", exc)
                    ImageGrab.grab(all_screens=True).crop((px-VX, py-VY, px-VX+pw, py-VY+ph)).save(
                        os.path.join(OUT, "%s-%s.png" % (tag, name)))
                a = pipe({"command": "audit"})
                print(name, "audit", a["count"], a["issues"][:3])
    elif scenario == "dpiprobe":
        mons = pipe({"command": "monitors"})
        print(json.dumps(mons, ensure_ascii=False)[:600])
        r = pipe({"command": "place", "state": 3, "edge": 0, "float": [-1900, -600],
                  "monitorPoint": [-1900, -600]})
        print("after place: panel", r["panel"], "float", r["float"], "monitor",
              r["monitorId"], "expected", r["expectedPanel"])
        time.sleep(1.0)
        d = pipe({"command": "dump"})
        print("later: panel", d["panel"], "float", d["float"], "monitor", d["monitorId"],
              "expected", d["expectedPanel"], "scale", d["fit"]["scale"])
    elif scenario == "diag":
        search = pipe({"command": "controls"})["searchBox"]
        pipe({"command": "tap", "x": search["x"]+search["width"]//2,
              "y": search["y"]+search["height"]//2, "screen": True})
        time.sleep(0.5)
        print("focus:", json.dumps(pipe({"command": "dump"})["focus"], ensure_ascii=False))
        print("search before:", json.dumps(pipe({"command": "dump"})["search"], ensure_ascii=False))
        pipe({"command": "key", "text": "deepseek"})
        time.sleep(0.6)
        d = pipe({"command": "dump"})
        print("search after:", json.dumps(d["search"], ensure_ascii=False))
        print("models.search:", d["models"]["search"])
        print("filters:", json.dumps(d["models"]["filters"], ensure_ascii=False))
        print("flat?", d["models"]["matches"], "groups:", len(d["models"]["groups"]))
        shot("diag")
    elif scenario == "addselect":
        r = pipe({"command": "controls"})["tabAdd"]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(0.8)
        r = pipe({"command": "controls"})["subTabDel"]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(0.8)
        shot("add-delete")
        c = pipe({"command": "controls"})
        # 删除页的供应商下拉：优先用命名的 select 控件
        r = c["_fields"][0]
        pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
        time.sleep(0.8)
        shot("add-delete-open")
        opts = pipe({"command": "controls"})["_select"]
        print("options:", len(opts))
        if len(opts) > 1:
            r = opts[1]   # P1 = 长名字供应商
            pipe({"command": "tap", "x": r["x"]+r["width"]//2, "y": r["y"]+r["height"]//2, "screen": True})
            time.sleep(0.8)
        shot("add-delete-selected")
    print("OK", tag)
finally:
    try: probe.stdin.write("quit\n"); probe.stdin.flush()
    except OSError: pass
    time.sleep(0.3); proc.kill()
