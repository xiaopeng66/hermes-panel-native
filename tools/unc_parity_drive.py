# -*- coding: utf-8 -*-
"""unc-parity 对拍驱动：unc-parity-tests.exe 先产出 C++ 行扫描器的 list JSON，
本脚本再对同一夹具跑真 apply.py（WSL 内 python3，HERMES_HOME 指向 /tmp/unc-parity/<case>），
逐字段比较 providers/fallbacks/model。任一字段不一致即退出码 1。"""
import json
import subprocess
import sys

WSL_DISTRO = "Ubuntu-24.04"
APPLY = "/mnt/e/AI/Zcode/projects/hermes-panel/wsl/hermes-panel-apply.py"
CASES = ["hermes-style", "webui-style", "edge"]
OUT = "E:/AI/Zcode/tmp/hermes-panel-native/unc-parity"

failures = []


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + (("  [" + str(detail)[:180] + "]") if detail else ""))
    if not ok:
        failures.append(name)


def run_apply(case):
    script = "printf '%s\\n' '{\"cmd\":\"list\"}' | HERMES_HOME=/tmp/unc-parity/" + case + \
             " python3 " + APPLY
    proc = subprocess.run(["wsl.exe", "-d", WSL_DISTRO, "--", "sh", "-c", script],
                          capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=60)
    lines = [l for l in proc.stdout.splitlines() if l.strip().startswith("{")]
    if not lines:
        raise RuntimeError("no JSON from apply.py: " + (proc.stdout + proc.stderr)[:200])
    return json.loads(lines[-1])


def cmp_field(py, cpp, name):
    # apply.py 缺失键 = None；C++ 缺失键也当 None
    a = py.get(name, None)
    b = cpp.get(name, None)
    check(name + (" (" + json.dumps(a, ensure_ascii=False)[:40] + ")" if a != b else ""),
          a == b, "py=%r cpp=%r" % (a, b))


def main():
    for case in CASES:
        py = run_apply(case)
        with open(OUT + "/" + case + ".cpp.json", encoding="utf-8") as fh:
            cpp = json.load(fh)
        check(case + ": apply.py ok=true", py.get("ok") is True, py.get("error"))
        inner = py.get("data", py)
        # providers
        p_py = inner.get("providers") or []
        p_cpp = cpp.get("providers") or []
        check(case + ": provider count %d" % len(p_py), len(p_py) == len(p_cpp),
              "cpp=%d" % len(p_cpp))
        for i, (a, b) in enumerate(zip(p_py, p_cpp)):
            for field in ("name", "provider_id", "base_url", "key_env", "key_set",
                          "api_mode", "default_model", "models_discovered", "is_active", "line"):
                cmp_field(a, b, case + ".p%d.%s" % (i, field))
            check(case + ".p%d.models" % i, (a.get("models") or []) == (b.get("models") or []),
                  "py=%r cpp=%r" % (a.get("models"), b.get("models")))
        # fallbacks
        f_py = inner.get("fallbacks") or []
        f_cpp = cpp.get("fallbacks") or []
        check(case + ": fallback count %d" % len(f_py), len(f_py) == len(f_cpp),
              "cpp=%d" % len(f_cpp))
        for i, (a, b) in enumerate(zip(f_py, f_cpp)):
            for field in ("index", "provider", "model", "base_url", "api_mode",
                          "key_env", "key_set", "line"):
                cmp_field(a, b, case + ".f%d.%s" % (i, field))
        # model
        m_py = inner.get("model") or {}
        m_cpp = cpp.get("model") or {}
        for field in ("default", "provider", "base_url", "api_mode"):
            cmp_field(m_py, m_cpp, case + ".model." + field)
    print("unc-parity driver: %s" % ("ALL MATCHED" if not failures else "%d FAILURES" % len(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
