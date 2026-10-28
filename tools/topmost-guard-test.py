# 置顶带守护门禁：真机验证「面板被外来 topmost 压住后，守护在超时内自动抬回带顶」。
# 背景（DESIGN v1.34）：WS_EX_TOPMOST 只是进"置顶带"，带内 z-order 按"谁最后被抬高"排，
# 别的置顶窗口随时会压上来；守护（kTopmostGuardTimer → EnforceTopmost）负责抬回。
# 本门禁起真实 exe（无控制面依赖），用外来探针窗口实测两轮"盖住→抬回"循环。
# 用法：PYTHONUTF8=1 python tools/topmost-guard-test.py
import ctypes
import ctypes.wintypes as wt
import os
import shutil
import subprocess
import sys
import tempfile
import time

u32 = ctypes.WinDLL("user32", use_last_error=True)

try:
    u32.SetProcessDpiAwarenessContext(wt.HANDLE(-4))  # per-monitor v2：全程物理像素
except Exception:
    pass

u32.DefWindowProcW.argtypes = [wt.HWND, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
u32.DefWindowProcW.restype = ctypes.c_longlong
u32.WindowFromPoint.argtypes = [wt.POINT]
u32.WindowFromPoint.restype = wt.HWND
u32.GetAncestor.argtypes = [wt.HWND, ctypes.c_uint]
u32.GetAncestor.restype = wt.HWND
u32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
u32.SetWindowPos.restype = wt.BOOL
u32.DestroyWindow.argtypes = [wt.HWND]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.GetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int]
u32.GetWindowLongPtrW.restype = ctypes.c_longlong
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.UnregisterClassW.argtypes = [wt.LPCWSTR, wt.HINSTANCE]

WS_EX_TOPMOST = 0x00000008
WS_POPUP = 0x80000000
WS_VISIBLE = 0x10000000
HWND_TOPMOST = wt.HWND(-1)
SWP_NOMOVE, SWP_NOSIZE, SWP_NOACTIVATE = 0x0002, 0x0001, 0x0010
GA_ROOT = 2
LIFT_TIMEOUT_S = 4.0
ROUNDS = 2

WndProc = ctypes.WINFUNCTYPE(ctypes.c_longlong, wt.HWND, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t)


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", ctypes.c_uint), ("lpfnWndProc", WndProc), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH), ("lpszMenuName", wt.LPCWSTR),
                ("lpszClassName", wt.LPCWSTR)]


def panel_center(panel):
    rect = wt.RECT()
    if not u32.GetWindowRect(panel, ctypes.byref(rect)):
        raise RuntimeError("GetWindowRect failed")
    return (rect.left + rect.right) // 2, (rect.top + rect.bottom) // 2


def root_at(x, y):
    return u32.GetAncestor(u32.WindowFromPoint(wt.POINT(x, y)), GA_ROOT)


def make_foreign_topmost():
    """外来置顶探针窗口（模拟截图贴图/其他挂件）：新创建的 topmost 落在带顶。"""
    wndproc = WndProc(u32.DefWindowProcW)
    cls = WNDCLASSW()
    cls.lpfnWndProc = wndproc
    cls.lpszClassName = "TopmostGuardProbe"
    cls.hInstance = ctypes.windll.kernel32.GetModuleHandleW(None)
    if not u32.RegisterClassW(ctypes.byref(cls)):
        raise RuntimeError("RegisterClassW failed")
    hwnd = u32.CreateWindowExW(WS_EX_TOPMOST, "TopmostGuardProbe", "probe",
                               WS_POPUP | WS_VISIBLE, 0, 0, 64, 64,
                               None, None, cls.hInstance, None)
    if not hwnd:
        raise RuntimeError("CreateWindowExW failed")
    return hwnd, wndproc, cls.hInstance


def wait_lifted(panel, timeout=LIFT_TIMEOUT_S):
    cx, cy = panel_center(panel)
    start = time.time()
    while time.time() - start < timeout:
        if root_at(cx, cy) == panel:
            return time.time() - start
        time.sleep(0.1)
    return None


def find_visible_panel(pid, class_name, deadline_s=12.0):
    deadline = time.time() + deadline_s
    while time.time() < deadline:
        found = []

        @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        def cb(hwnd, _):
            buf = ctypes.create_unicode_buffer(64)
            u32.GetClassNameW(hwnd, buf, 64)
            owner = wt.DWORD(0)
            u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid and buf.value == class_name and u32.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True

        u32.EnumWindows(cb, 0)
        if found:
            return found[0]
        time.sleep(0.2)
    return None


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = os.path.join(repo, "build", "Release", "hermes-panel-native.exe")
    if not os.path.exists(exe):
        print("FAIL: 先编译 Release（cmake --build build --config Release）:", exe)
        return 1
    data = tempfile.mkdtemp(prefix="topmost-guard-")
    env = dict(os.environ)
    env["HERMES_DISABLE_UNC"] = "1"
    proc = subprocess.Popen([exe, "--instance", "tgtest", "--data-dir", data,
                             "--legacy-dir", os.path.join(data, "no-legacy")], env=env)
    probe = keepalive = None
    try:
        panel = find_visible_panel(proc.pid, "HermesModelPanelNative.Panel")
        if not panel:
            print("FAIL: 12s 内没等到可见的面板窗口")
            return 1
        style = u32.GetWindowLongPtrW(panel, -20)  # GWL_EXSTYLE
        if not style & WS_EX_TOPMOST:
            print("FAIL: 面板默认就应带 WS_EX_TOPMOST（设置默认置顶开）")
            return 1
        probe, keepalive, hinstance = make_foreign_topmost()
        for round_index in range(1, ROUNDS + 1):
            rect = wt.RECT()
            u32.GetWindowRect(panel, ctypes.byref(rect))
            u32.SetWindowPos(probe, HWND_TOPMOST, rect.left, rect.top,
                             rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE)
            cx, cy = panel_center(panel)
            if root_at(cx, cy) == panel:
                print("FAIL: 第 %d 轮外来 topmost 没能盖住面板，测试无效" % round_index)
                return 1
            elapsed = wait_lifted(panel)
            if elapsed is None:
                print("FAIL: 第 %d 轮被盖后 %.1fs 内守护没有抬回" % (round_index, LIFT_TIMEOUT_S))
                return 1
            print("PASS 第 %d/%d 轮：被盖后 %.2fs 自动抬回" % (round_index, ROUNDS, elapsed))
        print("PASS: 置顶带守护工作正常（%d 轮覆盖循环）" % ROUNDS)
        return 0
    finally:
        if probe:
            u32.DestroyWindow(probe)
            u32.UnregisterClassW("TopmostGuardProbe", hinstance)
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(data, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
