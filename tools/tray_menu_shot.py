"""打开 native 面板的托盘右键菜单并截图（真机证据：菜单分组 + 中文不乱码）。

Release 包里没有控制面，所以这条走 Debug 构建：直接给托盘回调窗口（message-only 的
HermesModelPanelNative.Control）投递 kTrayMessage + WM_RBUTTONUP（与托盘图标被右击时
shell 发来的一模一样），再把光标挪到菜单区域外一点、抓屏，最后 Esc 关掉菜单。

用法：python tools/tray_menu_shot.py <instance-id> <输出png>
"""
import ctypes
import sys
import time
from ctypes import wintypes

from PIL import ImageGrab

kTRAY_MESSAGE = 0x8000 + 10          # WM_APP + 10（src/app/main.cpp）
WM_RBUTTONUP, WM_LBUTTONUP, WM_KEYDOWN, WM_KEYUP = 0x0205, 0x0202, 0x0100, 0x0101
HWND_MESSAGE = wintypes.HWND(-3)
CONTROL_CLASS = "HermesModelPanelNative.Control"


def main() -> int:
    instance, output = sys.argv[1], sys.argv[2]
    user32 = ctypes.windll.user32
    user32.FindWindowExW.restype = wintypes.HWND
    user32.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
    user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
    user32.GetSystemMetrics.restype = ctypes.c_int

    control = user32.FindWindowExW(HWND_MESSAGE, None, CONTROL_CLASS, instance)
    if not control:
        print("control window not found for instance", instance)
        return 2
    # 光标放在屏幕中部：菜单会出现在这里（TrackPopupMenu 用光标位置）。
    screen_w, screen_h = user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)
    x, y = screen_w // 3, screen_h // 4
    user32.SetCursorPos(x, y)
    time.sleep(0.3)
    user32.PostMessageW(control, kTRAY_MESSAGE, 0x4242, WM_RBUTTONUP)
    time.sleep(1.2)                                    # 菜单弹出 + 绘制
    # 抓菜单区域（比 17 项菜单略大，留出阴影与子菜单箭头）
    box = (x - 8, y - 8, x + 620, y + 560)
    ImageGrab.grab(bbox=box).save(output)
    print("saved", output)
    # Esc 关菜单（菜单有自己的消息循环，用键盘把它关掉，应用不受影响）
    user32.PostMessageW(control, WM_KEYDOWN, 0x1B, 0)
    user32.PostMessageW(control, WM_KEYUP, 0x1B, 0)
    time.sleep(0.3)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
