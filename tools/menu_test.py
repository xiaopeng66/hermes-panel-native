"""托盘菜单功能实测：逐个 InvokeMenu id，断言状态/设置真的变了。

用法: python menu_test.py
不测「开机自启」（会写 HKCU Run）、「打开数据目录 / 打开 WebUI」（会拉起外部窗口）。
「关于」异步打开，读出正文后自动关掉（验证版本号取的是 exe 的 VERSIONINFO）。
"""
import ctypes, json, os, queue, subprocess, threading, time
from ctypes import wintypes

PROJ = r"E:\AI\Zcode\projects\hermes-panel-native"
APP = os.path.join(PROJ, "build", "Debug", "hermes-panel-native.exe")
PROBE = os.path.join(PROJ, "build", "Debug", "geometry-probe.exe")
FAKE = os.path.join(PROJ, "build", "Debug", "fake-hermes.exe")
DATA = r"E:\AI\Zcode\tmp\audit-data"
HOME = os.path.join(DATA, "fake")

HIDDEN, FLOATING, DRAGGING, EXPANDED, COLLAPSED = 0, 1, 2, 3, 4
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(("PASS " if ok else "FAIL ") + name + (("  " + str(detail)) if detail else ""))


def submenu_checked(dump, parent_label):
    for item in dump["menu"]:
        if item.get("label") == parent_label:
            for child in item.get("children") or []:
                if child.get("checked"):
                    return child.get("label")
    return None


inst = "menutest%d" % os.getpid()
env = dict(os.environ)
env.update({"HERMES_WSL_EXE": FAKE, "HERMES_DISABLE_UNC": "1", "FAKE_HERMES_HOME": HOME})
log_dir = os.path.join(DATA, "logs")
os.makedirs(log_dir, exist_ok=True)
proc = subprocess.Popen([APP, "--instance", inst, "--data-dir", DATA,
                         "--legacy-dir", os.path.join(DATA, "no-legacy")], env=env)
time.sleep(5.5)
probe = subprocess.Popen([PROBE, "--id", inst], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         text=True, encoding="utf-8", errors="replace")
probe.stdout.readline()
q = queue.Queue()
threading.Thread(target=lambda: [q.put(line) for line in probe.stdout], daemon=True).start()


def pipe(req, timeout=40.0):
    probe.stdin.write("pipe " + json.dumps(req) + "\n")
    probe.stdin.flush()
    return json.loads(q.get(timeout=timeout).strip())


def menu(mid, timeout=40.0):
    return pipe({"command": "menu", "id": mid}, timeout)


def log_tail():
    try:
        with open(os.path.join(log_dir, "app.log"), encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


try:
    dump = pipe({"command": "dump"})
    check("initial floating", dump["state"] == FLOATING, dump["state"])

    d = menu(1001)
    check("toggle hides", d["state"] == HIDDEN, d["state"])
    d = menu(1001)
    check("toggle shows", d["state"] == FLOATING, d["state"])

    d = menu(1002)
    check("dock collapses", d["state"] == COLLAPSED, d["state"])
    check("dock sets edge", d["edge"] != 0, d["edge"])
    items = [i for i in d["menu"] if not i.get("separator")]
    check("first item says expand", items[0]["label"] == "展开面板", items[0]["label"])
    dock = [i for i in d["menu"] if i.get("label") == "贴边收起"]
    check("dock checked", bool(dock and dock[0]["checked"]))

    d = menu(1001)
    check("toggle expands from strip", d["state"] == EXPANDED, d["state"])
    d = menu(1001)
    check("toggle hides from expanded", d["state"] == HIDDEN, d["state"])
    d = menu(1001)
    check("toggle from hidden expands", d["state"] == EXPANDED, d["state"])
    d = menu(1002)
    check("undock clears edge", d["edge"] == 0, d["edge"])
    check("undock floats the panel", d["state"] == FLOATING, d["state"])
    d = menu(1001)
    check("toggle hides after undock", d["state"] == HIDDEN, d["state"])
    d = menu(1001)
    check("toggle shows floating again", d["state"] == FLOATING, d["state"])

    before = pipe({"command": "dump"})["topmost"]
    d = menu(1008)
    check("topmost toggles", d["topmost"] != before, (before, d["topmost"]))
    d = menu(1008)
    check("topmost back", d["topmost"] == before, (before, d["topmost"]))

    for mid, seconds in ((1009, 30), (1010, 60), (1011, 120), (1012, 300)):
        d = menu(mid)
        check("poll %d" % seconds, d["pollSeconds"] == seconds, d["pollSeconds"])

    for mid, label in ((1020, "深色"), (1021, "浅色"), (1022, "跟随系统"),
                       (1025, "石板"), (1026, "碳黑"), (1027, "瓷白"), (1028, "沙丘")):
        d = menu(mid)
        check("theme check %s" % label, submenu_checked(d, "主题") == label,
              submenu_checked(d, "主题"))

    d = menu(1024)
    check("close action = quit", submenu_checked(d, "关闭按钮") == "退出程序",
          submenu_checked(d, "关闭按钮"))
    d = menu(1023)
    check("close action = hide", submenu_checked(d, "关闭按钮") == "隐藏到托盘",
          submenu_checked(d, "关闭按钮"))

    check("menu has 4 groups", sum(1 for i in d["menu"] if i.get("separator")) == 3,
          sum(1 for i in d["menu"] if i.get("separator")))

    before_refresh = log_tail().count("refresh started")
    menu(1004)
    time.sleep(1.2)
    check("refresh starts a poll", log_tail().count("refresh started") > before_refresh,
          (before_refresh, log_tail().count("refresh started")))

    mark = len(log_tail())
    menu(1006)
    time.sleep(1.2)
    check("restart gateway hits the helper", "reload" in log_tail()[mark:],
          log_tail()[mark:][-140:])
    mark = len(log_tail())
    menu(1033)
    time.sleep(1.2)
    check("restart webui hits the helper", "reload" in log_tail()[mark:],
          log_tail()[mark:][-140:])

    # 关于：MessageBoxW 会阻塞控制面，异步调用后读出正文再关掉
    def ask_about():
        try:
            menu(1032)
        except Exception:  # noqa: BLE001
            pass

    worker = threading.Thread(target=ask_about, daemon=True)
    worker.start()
    time.sleep(1.2)
    user32 = ctypes.WinDLL("user32")
    user32.FindWindowW.restype = ctypes.c_void_p
    user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
    user32.GetWindowTextW.argtypes = [ctypes.c_void_p, wintypes.LPWSTR, ctypes.c_int]
    user32.GetClassNameW.argtypes = [ctypes.c_void_p, wintypes.LPWSTR, ctypes.c_int]
    user32.PostMessageW.argtypes = [ctypes.c_void_p, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p]
    box = user32.FindWindowW("#32770", "关于 Hermes 模型面板")
    texts = []
    if box:
        enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, ctypes.c_void_p, ctypes.c_void_p)

        def on_child(child, _param):
            txt = ctypes.create_unicode_buffer(2048)
            user32.GetWindowTextW(child, txt, 2048)
            texts.append(txt.value)
            return True

        user32.EnumChildWindows.argtypes = [ctypes.c_void_p, enum_proc, ctypes.c_void_p]
        user32.EnumChildWindows(box, enum_proc(on_child), None)
        user32.PostMessageW(box, 0x0010, 0, 0)   # WM_CLOSE
    worker.join(timeout=8)
    body = " | ".join(t for t in texts if t)
    exe_version = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "(Get-Item '%s').VersionInfo.FileVersion" % APP],
        capture_output=True, text=True).stdout.strip()
    check("about dialog opened", bool(box), box)
    check("about shows exe version", ("版本：" + exe_version) in body,
          "exe=%s body=%s" % (exe_version, body[:160]))

    print("--- summary: %d/%d passed" % (sum(1 for x in results if x), len(results)))
finally:
    try:
        probe.stdin.write("quit\n")
        probe.stdin.flush()
    except OSError:
        pass
    time.sleep(0.3)
    proc.kill()
