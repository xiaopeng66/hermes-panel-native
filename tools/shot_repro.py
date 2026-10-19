"""按当前 Debug 构建复现用户截图里的两个界面（真数据 + 夹具两种）。

用法：python shot_repro.py <real|fixture>
  real     —— 真 WSL（用户的 ~/.hermes 里有 30 份备份）：备份页滚动 + 回退链追加卡
  fixture  —— 假助手（shot-home 的夹具状态）：回退链追加卡（供应商下拉）
输出：evidence/repro-*.png
"""
import json
import os
import re
import subprocess
import sys
import time

from PIL import ImageGrab

ROOT = r"E:\AI\Zcode\projects\hermes-panel-native"
EVIDENCE = r"E:\AI\Zcode\tmp\hermes-panel-native\evidence"
mode = sys.argv[1] if len(sys.argv) > 1 else "fixture"

env = dict(os.environ)
data = r"E:\AI\Zcode\tmp\hermes-panel-native\repro-data"
if mode == "fixture":
    env["HERMES_WSL_EXE"] = ROOT + r"\build\Debug\fake-hermes.exe"
    env["FAKE_HERMES_HOME"] = r"E:\AI\Zcode\tmp\hermes-panel-native\shot-home"

instance = "repro-" + mode
app = ROOT + r"\build\Debug\hermes-panel-native.exe"
proc = subprocess.Popen([app, "--instance", instance, "--data-dir", data], env=env)
time.sleep(4.5)

probe = subprocess.Popen([ROOT + r"\build\Debug\geometry-probe.exe", "--id", instance],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                         encoding="utf-8", errors="replace")
probe.stdout.readline()


def pipe(request):
    probe.stdin.write("pipe " + json.dumps(request) + "\n")
    probe.stdin.flush()
    return json.loads(probe.stdout.readline().strip())


def shot(name):
    d = pipe({"command": "dump"})
    x, y, w, h = d["panel"]
    ImageGrab.grab(bbox=(x, y, x + w, y + h)).save(os.path.join(EVIDENCE, name + ".png"))
    print("saved", name, (x, y, w, h))


def tap_screen(rect):
    pipe({"command": "tap", "x": rect["x"] + rect["width"] // 2,
          "y": rect["y"] + rect["height"] // 2, "screen": True})


def tap_client(rect):
    pipe({"command": "tap", "x": rect["x"] + rect["width"] // 2,
          "y": rect["y"] + rect["height"] // 2})


# 备份页 + 滚动
tap_screen(pipe({"command": "controls"})["tabBackup"])
time.sleep(1.6)
shot("repro-backup-top")
# 滚轮：控制面没有 scroll 命令，用真实滚轮消息（SendInput 到面板中心）
import ctypes
user32 = ctypes.windll.user32
d = pipe({"command": "dump"})
cx = d["panel"][0] + d["panel"][2] // 2
cy = d["panel"][1] + 300
user32.SetCursorPos(cx, cy)
time.sleep(0.3)
MOUSEEVENTF_WHEEL = 0x0800
for _ in range(4):
    user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0, ctypes.c_long(-120 * 3), 0)
    time.sleep(0.25)
shot("repro-backup-scrolled")

# 回退链 + 追加卡
tap_screen(pipe({"command": "controls"})["tabFallback"])
time.sleep(0.6)
controls = pipe({"command": "controls"})
pipe({"command": "tap", "x": controls["_addLine"]["x"] + controls["_addLine"]["width"] // 2,
      "y": controls["_addLine"]["y"] + controls["_addLine"]["height"] // 2, "screen": True})
time.sleep(0.6)
shot("repro-fallback-fold")
# 打开来源供应商下拉
controls = pipe({"command": "controls"})
fields = controls.get("_fields", [])
if fields:
    tap_screen(fields[0])
    time.sleep(0.5)
    shot("repro-fallback-select")
try:
    probe.stdin.write("quit\n")
    probe.stdin.flush()
except OSError:
    pass
time.sleep(0.2)
proc.terminate()
print("done")
