"""hermes-panel-native 全量审查（可重复运行）。

覆盖：页面×主题布局越界审计、模态审计、随机模糊操作（含崩溃/断言检测）、
资源泄漏循环（GDI/USER/句柄/线程/内存）、三态 CPU 空转、多显示器×五套挂件尺寸与贴边。

用法：python tools/full_audit.py [--quick]
退出码：0 = 全部通过；1 = 有失败项（细节打印在 stdout 并写入 evidence/full-audit.txt）
"""
import argparse
import json
import os
import queue
import random
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = r"E:\AI\Zcode\tmp\hermes-panel-native"
EVIDENCE = os.path.join(TMP, "evidence")
DATA = os.path.join(TMP, "audit-data")
HOME = os.path.join(TMP, "audit-home")
APP = os.path.join(ROOT, "build", "Debug", "hermes-panel-native.exe")
FAKE = os.path.join(ROOT, "build", "Debug", "fake-hermes.exe")
PROBE = os.path.join(ROOT, "build", "Debug", "geometry-probe.exe")

THEMES = ["dark", "light", "slate", "carbon", "porcelain", "sand"]
STYLES = ["beacon", "tag", "flag", "capsule"]
# 可见胶囊尺寸；capsule 的窗口每边还要加 SHADOW_MARGIN_DIP（外投影的透明外扩）。
STYLE_DIP = {"beacon": (10, 76), "tag": (14, 74), "flag": (16, 58), "capsule": (30, 136)}
SHADOW_MARGIN_DIP = {"beacon": 0, "tag": 0, "flag": 0, "capsule": 10}
TABS = ["tabModels", "tabFallback", "tabAdd", "tabBackup", "tabSettings"]

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(("PASS " if ok else "FAIL ") + name + (("  [" + str(detail) + "]") if detail else ""))
    return bool(ok)


def scale_dip(dip, dpi):
    return (dip * dpi + 48) // 96


class App:
    def __init__(self):
        os.makedirs(DATA, exist_ok=True)
        os.makedirs(HOME, exist_ok=True)
        os.makedirs(EVIDENCE, exist_ok=True)
        with open(os.path.join(DATA, "latency.json"), "w", encoding="utf-8") as handle:
            json.dump({"version": "1", "samples": {
                "alpha|alpha-large": [{"t": int(time.time()), "ms": 312, "ok": True}]}}, handle)
        with open(os.path.join(DATA, "profiles.json"), "w", encoding="utf-8") as handle:
            json.dump({"version": 1, "profiles": [
                {"name": "日常", "main": {"provider": "alpha", "model": "alpha-large"},
                 "fallbacks": [{"provider": "custom:Beta", "model": "beta-1"}],
                 "favorites": ["alpha|alpha-large"], "updated": int(time.time())}]}, handle)
        # 每次审查用干净的日志：否则上一轮的 [error] 会被这一轮的扫描读进来（假失败）。
        try:
            os.remove(os.path.join(DATA, "logs", "app.log"))
        except OSError:
            pass
        self.instance = "audit-%d" % os.getpid()
        env = dict(os.environ)
        env["HERMES_WSL_EXE"] = FAKE
        env["HERMES_DISABLE_UNC"] = "1"
        env["FAKE_HERMES_HOME"] = HOME
        self.proc = subprocess.Popen(
            [APP, "--instance", self.instance, "--data-dir", DATA,
             "--legacy-dir", os.path.join(DATA, "no-legacy")], env=env)
        time.sleep(4.5)
        self.probe = subprocess.Popen([PROBE, "--id", self.instance], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, text=True,
                                      encoding="utf-8", errors="replace")
        # 探针启动会先打一行 banner：同步吃掉它，否则后面所有应答都会错位一格。
        self.probe.stdout.readline()
        self.queue = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()
        self.pipe({"command": "state"})

    def _reader(self):
        for line in self.probe.stdout:
            self.queue.put(line)

    def pipe(self, request, timeout=20.0):
        self.probe.stdin.write("pipe " + json.dumps(request) + "\n")
        self.probe.stdin.flush()
        try:
            return json.loads(self.queue.get(timeout=timeout).strip())
        except queue.Empty:
            raise RuntimeError("control plane timed out: " + json.dumps(request, ensure_ascii=False))

    def dump(self):
        return self.pipe({"command": "dump"})

    def settle(self, timeout=6.0):
        deadline = time.time() + timeout
        previous = None
        while time.time() < deadline:
            current = self.dump()["panel"]
            if current == previous:
                return True
            previous = current
            time.sleep(0.12)
        return False

    def tap(self, rect):
        self.pipe({"command": "focus"})
        x = rect["x"] + rect["width"] // 2
        y = rect["y"] + rect["height"] // 2
        self.pipe({"command": "tap", "x": x, "y": y, "screen": True})
        time.sleep(0.15)

    def close(self):
        try:
            self.probe.stdin.write("quit\n")
            self.probe.stdin.flush()
        except OSError:
            pass
        time.sleep(0.2)
        self.proc.kill()


def audit(app, label):
    report = app.pipe({"command": "audit"})
    ok = report["count"] == 0
    detail = "" if ok else json.dumps(
        {"client": report.get("client"), "scale": report.get("scale"),
         "content": report.get("content"), "page": report.get("page"),
         "issues": report["issues"][:4]}, ensure_ascii=False)
    return check("layout audit: " + label, ok, detail)


def page_theme_sweep(app):
    for theme in THEMES:
        app.pipe({"command": "configure", "theme": theme})
        time.sleep(0.3)
        for tab in TABS:
            controls = app.pipe({"command": "controls"})
            app.tap(controls[tab])
            app.settle()
            audit(app, "%s / %s" % (theme, tab))
    app.pipe({"command": "configure", "theme": "dark"})


def modal_sweep(app):
    controls = app.pipe({"command": "controls"})
    app.tap(controls["tabSettings"])
    app.settle()
    controls = app.pipe({"command": "controls"})
    for entry in ("setDiagnose", "setUsage", "setProfiles"):
        app.tap(controls[entry])
        time.sleep(1.2)
        audit(app, "modal " + entry)
        app.pipe({"command": "vk", "vk": 27})
        time.sleep(0.4)
        controls = app.pipe({"command": "controls"})
    # 回退链：折叠表单 + 供应商下拉 + 探测结果行
    controls = app.pipe({"command": "controls"})
    app.tap(controls["tabFallback"])
    app.settle()
    controls = app.pipe({"command": "controls"})
    app.tap(controls["fbFoldToggle"])
    time.sleep(0.5)
    audit(app, "fallback fold form")
    # 折叠表单打开后控件才会出现：重新取一次控件表，别用旧表点 fbSelect。
    controls = app.pipe({"command": "controls"})
    app.tap(controls["fbSelect"])
    time.sleep(0.3)
    audit(app, "fallback provider dropdown")
    app.pipe({"command": "vk", "vk": 27})
    time.sleep(0.3)
    # 模态（危险确认）里的两份按钮
    controls = app.pipe({"command": "controls"})
    app.tap(controls["tabBackup"])
    app.settle()
    controls = app.pipe({"command": "controls"})
    if controls.get("_restore"):
        app.tap(controls["_restore"][0])
        time.sleep(0.4)
        audit(app, "restore confirm modal")
        app.pipe({"command": "vk", "vk": 27})
        time.sleep(0.3)


def fuzz(app, rounds):
    rng = random.Random(20261018)
    app.pipe({"command": "place", "state": 1, "edge": 0})
    time.sleep(0.3)
    for index in range(rounds):
        panel = app.dump()["panel"]
        operation = rng.randrange(9)
        if operation == 0:
            x = rng.randrange(panel[0] + 4, panel[0] + max(8, panel[2] - 4))
            y = rng.randrange(panel[1] + 4, panel[1] + max(8, panel[3] - 4))
            app.pipe({"command": "focus"})
            app.pipe({"command": "tap", "x": x, "y": y, "screen": True})
        elif operation == 1:
            app.pipe({"command": "hover", "x": rng.randrange(4, 470), "y": rng.randrange(4, 700)})
        elif operation == 2:
            app.pipe({"command": "wheel", "delta": rng.choice([-360, -120, 120, 360])})
        elif operation == 3:
            app.pipe({"command": "key", "text": rng.choice(["a", " ", "/", "1"])})
        elif operation == 4:
            app.pipe({"command": "vk", "vk": rng.choice([27, 13, 38, 40, 37, 39, 8])})
        elif operation == 5:
            app.pipe({"command": "configure", "theme": rng.choice(THEMES)})
        elif operation == 6:
            app.pipe({"command": "configure", "widgetStyle": rng.choice(STYLES)})
        elif operation == 7:
            app.pipe({"command": "place", "state": rng.choice([1, 3, 4]),
                      "edge": rng.choice([0, 1, 2, 3, 4]), "anchor": rng.randrange(0, 900)})
        else:
            app.pipe({"command": "poll"})
        if index % 25 == 24:
            audit(app, "fuzz round %d" % (index + 1))
        dump = app.dump()
        if dump.get("violations", 0) != 0:
            return check("fuzz: geometry invariants stay intact", False, "violations")
        if dump.get("panelVisible") is None:
            return check("fuzz: app answers every step", False, "missing dump")
    check("fuzz: geometry invariants stay intact", True, "%d rounds" % rounds)
    check("fuzz: app answers every step", True, "%d rounds" % rounds)
    app.pipe({"command": "place", "state": 1, "edge": 0})
    return True


def leak_cycles(app, cycles):
    app.pipe({"command": "place", "state": 1, "edge": 0})
    time.sleep(0.4)
    before = app.pipe({"command": "process"})
    for index in range(cycles):
        app.pipe({"command": "configure", "theme": THEMES[index % len(THEMES)]})
        app.pipe({"command": "configure", "widgetStyle": STYLES[index % len(STYLES)]})
        app.pipe({"command": "place", "state": 4, "edge": 1 + (index % 4), "anchor": 200})
        app.pipe({"command": "place", "state": 1, "edge": 0})
        app.pipe({"command": "poll"})
        if index % 10 == 0:
            app.pipe({"command": "tray-icon"})
            app.pipe({"command": "monitors"})
    time.sleep(1.0)
    after = app.pipe({"command": "process"})
    deltas = {key: after[key] - before[key] for key in
              ("gdi", "user", "handles", "threads", "workingSetKb")}
    check("leak: GDI objects bounded", deltas["gdi"] <= 12, deltas)
    check("leak: USER objects bounded", deltas["user"] <= 12, deltas)
    check("leak: handles bounded", deltas["handles"] <= 24, deltas)
    check("leak: threads bounded", deltas["threads"] <= 2, deltas)
    check("leak: working set bounded", deltas["workingSetKb"] <= 20480, deltas)
    return before, after


def cpu_probe(app):
    def measure(label, seconds=5.0):
        # 连续两次取较小值：第一次可能撞上刚结束的一轮助手调用，空闲 CPU 才是有意义的指标。
        readings = []
        for _ in range(2):
            deadline = time.time() + 10
            while time.time() < deadline and app.dump()["data"]["busy"]:
                time.sleep(0.2)
            time.sleep(1.2)
            start = app.pipe({"command": "process"})["cpuMs"]
            time.sleep(seconds)
            end = app.pipe({"command": "process"})["cpuMs"]
            readings.append((end - start) / (seconds * 1000.0) * 100.0)
        return min(readings)

    # 隔离数据轮询的 CPU：测量期间把轮询间隔调到 1 小时（debug 旋钮，不落盘）。
    app.pipe({"command": "configure", "interval": 3600})
    app.pipe({"command": "place", "state": 1, "edge": 0})
    expanded = measure("expanded")
    check("cpu: expanded panel < 3%", expanded < 3.0, "%.2f%%" % expanded)
    # G11：全灯正常时三套挂件都必须完全静止（不再有常驻呼吸动画）；信标只在有故障时才呼吸。
    app.pipe({"command": "configure", "widgetStyle": "beacon"})
    app.pipe({"command": "place", "state": 4, "edge": 1, "anchor": 200})
    time.sleep(0.5)
    collapsed = measure("collapsed beacon")
    # 三套静态挂件共用同一份轮询/控制面开销（展开态本身就有 1.2–1.6%），2% 会把噪声当回归。
    # 真正的"空转"是旧 pulse/halo 的 120ms 常驻重绘（约 8%），3% 仍能可靠区分。
    check("cpu: collapsed beacon < 3%", collapsed < 3.0, "%.2f%%" % collapsed)
    app.pipe({"command": "configure", "widgetStyle": "tag"})
    time.sleep(0.5)
    tag = measure("collapsed tag")
    check("cpu: collapsed tag < 3%", tag < 3.0, "%.2f%%" % tag)
    app.pipe({"command": "configure", "widgetStyle": "flag"})
    time.sleep(0.5)
    flag = measure("collapsed flag")
    check("cpu: collapsed flag < 3%", flag < 3.0, "%.2f%%" % flag)
    app.pipe({"command": "configure", "widgetStyle": "beacon"})
    app.pipe({"command": "place", "state": 1, "edge": 0})
    app.pipe({"command": "configure", "interval": 60})


def monitor_sweep(app):
    monitors = app.pipe({"command": "monitors"})["monitors"]
    check("monitors: at least one", len(monitors) >= 1, len(monitors))
    for monitor in monitors:
        work = monitor["work"]
        dpi = monitor["dpi"]
        center_x = work[0] + work[2] // 2
        center_y = work[1] + work[3] // 2
        for style, (dip_w, dip_h) in STYLE_DIP.items():
            app.pipe({"command": "configure", "widgetStyle": style})
            for edge, name in ((1, "left"), (2, "right"), (3, "top"), (4, "bottom")):
                app.pipe({"command": "place", "state": 4, "edge": edge, "anchor": center_y
                          if edge <= 2 else center_x, "monitorPoint": [center_x, center_y]})
                time.sleep(0.25)
                dump = app.dump()
                strip = dump["strip"]
                # 窗口 = 可见挂件 + 2×投影外边（胶囊才有）。
                margin = scale_dip(SHADOW_MARGIN_DIP[style], dpi)
                expected_w = scale_dip(dip_w, dpi) + 2 * margin
                expected_h = scale_dip(dip_h, dpi) + 2 * margin
                if edge in (3, 4):
                    expected_w, expected_h = expected_h, expected_w
                ok = (strip[2] == expected_w and strip[3] == expected_h
                      and dump["stripVisible"] and not dump["panelVisible"])
                flush = {1: strip[0] + margin == work[0],
                         2: strip[0] + strip[2] - margin == work[0] + work[2],
                         3: strip[1] + margin == work[1],
                         4: strip[1] + strip[3] - margin == work[1] + work[3]}[edge]
                inside = (strip[0] + margin >= work[0] - 1 and strip[1] + margin >= work[1] - 1
                          and strip[0] + strip[2] - margin <= work[0] + work[2] + 1
                          and strip[1] + strip[3] - margin <= work[1] + work[3] + 1)
                check("widget %s @%s dpi%d: size" % (style, name, dpi), ok, strip)
                check("widget %s @%s dpi%d: flush+inside" % (style, name, dpi), flush and inside, strip)
    app.pipe({"command": "configure", "widgetStyle": "beacon"})
    app.pipe({"command": "place", "state": 1, "edge": 0})


def error_paths(app):
    """异常路径：畸形/空/超大/GBK 回复、协议错退出、配置文件损坏——面板不许崩，且能恢复。"""
    def toggle(name, on):
        path = os.path.join(HOME, name)
        if on:
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("1\n")
        elif os.path.exists(path):
            os.remove(path)

    def poll_and_wait():
        app.pipe({"command": "poll"})
        deadline = time.time() + 20
        while time.time() < deadline:
            if not app.dump()["data"]["busy"]:
                # busy 在投递结果**之前**释放：等一拍让 UI 线程把结果落地再断言。
                time.sleep(0.7)
                return
            time.sleep(0.2)

    def healthy():
        for attempt in range(6):
            poll_and_wait()
            dump = app.dump()
            if (dump["data"]["providers"] == 3 and not dump["data"]["loading"]
                    and dump["violations"] == 0):
                return True
            time.sleep(0.6)
        return False

    # 助手错误显示在模型页（空态/圆点），先把页面切回去再断言。
    app.pipe({"command": "place", "state": 1, "edge": 0})
    time.sleep(0.3)
    controls = app.pipe({"command": "controls"})
    app.tap(controls["tabModels"])
    app.settle()

    for name, expect_error in (("reply_garbage", True), ("reply_empty", False),
                               ("reply_huge", False), ("exit_two", True),
                               ("gbk_reply", False)):
        toggle(name, True)
        poll_and_wait()
        dump = app.dump()
        alive = dump["data"]["loading"] is False and dump["violations"] == 0
        check("error path: %s keeps the app alive" % name, alive,
              dump.get("helperError", "")[:60])
        if expect_error:
            check("error path: %s surfaces a helper error" % name,
                  bool(dump.get("helperError")), dump.get("helperError", "")[:60])
        toggle(name, False)
        check("error path: %s recovers after the fixture clears" % name, healthy())

    # 配置文件损坏：启动一个全新实例，settings/latency/profiles 全是坏 JSON。
    broken = os.path.join(TMP, "audit-broken")
    os.makedirs(broken, exist_ok=True)
    for name in ("settings.json", "latency.json", "profiles.json"):
        with open(os.path.join(broken, name), "w", encoding="utf-8") as handle:
            handle.write("{ this is not json")
    env = dict(os.environ)
    env["HERMES_WSL_EXE"] = FAKE
    env["HERMES_DISABLE_UNC"] = "1"
    env["FAKE_HERMES_HOME"] = HOME
    instance = "audit-broken-%d" % os.getpid()
    proc = subprocess.Popen([APP, "--instance", instance, "--data-dir", broken,
                             "--legacy-dir", os.path.join(broken, "no-legacy")], env=env)
    time.sleep(4.5)
    probe = subprocess.Popen([PROBE, "--id", instance], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, text=True,
                             encoding="utf-8", errors="replace")
    probe.stdout.readline()
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in probe.stdout], daemon=True).start()
    try:
        probe.stdin.write("pipe " + json.dumps({"command": "dump"}) + "\n")
        probe.stdin.flush()
        dump = json.loads(lines.get(timeout=15).strip())
        check("error path: corrupted settings/latency/profiles still boot",
              dump["data"]["providers"] == 3 and dump["violations"] == 0,
              dump.get("stored", {}).get("theme"))
    except Exception as error:  # noqa: BLE001
        check("error path: corrupted settings/latency/profiles still boot", False, str(error))
    finally:
        try:
            probe.stdin.write("quit\n")
            probe.stdin.flush()
        except OSError:
            pass
        time.sleep(0.2)
        proc.kill()


def log_scan():
    path = os.path.join(DATA, "logs", "app.log")
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    except OSError:
        return check("log: no [error] lines", False, "log missing")
    errors = [line for line in text.splitlines() if "[error]" in line]
    check("log: no [error] lines", not errors, errors[:3])
    leaks = [line for line in text.splitlines()
             if any(token in line for token in ("sk-", "Bearer ", "api_key=", "API_KEY="))]
    check("log: no secret-looking content", not leaks, leaks[:2])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true", help="减半的模糊与泄漏轮次")
    args = parser.parse_args()
    rounds = 300 if args.quick else 800
    cycles = 60 if args.quick else 150
    app = App()
    try:
        app.settle()
        data = app.dump()["data"]
        check("prime: fixture data loaded", data["providers"] == 3 and data["fallbacks"] == 2, data)
        page_theme_sweep(app)
        modal_sweep(app)
        fuzz(app, rounds)
        leak_cycles(app, cycles)
        cpu_probe(app)
        monitor_sweep(app)
        error_paths(app)
        log_scan()
    except Exception as error:  # noqa: BLE001
        check("audit run completed", False, "%s: %s" % (type(error).__name__, error))
    finally:
        app.close()
    failed = [name for name, ok, _ in results if not ok]
    summary = "full audit: %d passed, %d failed" % (len(results) - len(failed), len(failed))
    print(summary)
    with open(os.path.join(EVIDENCE, "full-audit.txt"), "w", encoding="utf-8") as handle:
        for name, ok, detail in results:
            handle.write(("PASS " if ok else "FAIL ") + name + (("  [" + str(detail) + "]") if detail else "") + "\n")
        handle.write(summary + "\n")
    if failed:
        print("failed: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
