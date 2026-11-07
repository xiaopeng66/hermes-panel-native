#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""hermes-panel-apply.py — Hermes 模型面板的 WSL 侧助手，config.yaml / .env 的唯一写入者。

协议：stdin 一行 JSON 请求 → stdout 一行 JSON 响应（均为 UTF-8）。

  请求  {"cmd": "list"}
  响应  {"ok": true, "data": {...}}
        {"ok": false, "error": "..."}

写操作的安全模型（每一步都是硬约束）：
  1. flock 独占 ~/.hermes/.panel-config.lock（同一时刻只有一个助手在写）；
  2. 读取时先 PyYAML 解析，解析失败或缺文件直接拒绝写入；
  3. 编辑一律是「锚点定位 + 行级替换/插入」，不整体重排 YAML，注释与其余区块原样保留；
  4. 写入前校验：新内容能被 PyYAML 解析，且除声明的目标区块外，其余顶层区块逐字节未变；
  5. 备份到 ~/.hermes/backups/config-changes/（保留最近 100 份），再 chmod 600 + os.replace 原子替换。
  任何异常都不会触碰原文件。

dry_run=true 时只返回将要发生的 diff，不落盘、不动 .env。
"""
import difflib
import fcntl
import json
import os
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

VERSION = "1.0.0"

# --------------------------------------------------------------------------- paths
_DEFAULT_HOME = os.environ.get("HERMES_HOME") or os.path.expanduser("~/.hermes")
HERMES_HOME = _DEFAULT_HOME
CONFIG = os.path.join(HERMES_HOME, "config.yaml")
ENV_FILE = os.path.join(HERMES_HOME, ".env")
BACKUP_DIR = os.path.join(HERMES_HOME, "backups", "config-changes")
LOCK_FILE = os.path.join(HERMES_HOME, ".panel-config.lock")

DEFAULT_WEBUI_PORTS = (8787, 9119)
DEFAULT_GATEWAY_PORT = 8642
BACKUP_KEEP = 100

TOP_KEY_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):")
ENTRY_RE = re.compile(r"^  - (?:name|provider):\s*(.*?)\s*$")
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")

try:
    import yaml
except ImportError as exc:  # pragma: no cover
    print(json.dumps({"ok": False, "error": "PyYAML 未安装：%s" % exc}, ensure_ascii=False))
    sys.exit(2)


class PanelError(Exception):
    """可预期的业务错误，直接回给面板。"""


# --------------------------------------------------------------------------- small utils
def as_list(value):
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    if isinstance(value, (list, tuple, set)):
        return [str(v) for v in value]
    return [str(value)]


def scalar(value):
    """把 Python 值渲染成 YAML 标量，必要时加双引号（JSON 字符串是合法 YAML）。"""
    if isinstance(value, bool):
        return "true" if value else "false"
    if value is None:
        return "null"
    if isinstance(value, (int, float)):
        return str(value)
    text = str(value)
    if text == "":
        return '""'
    looks_like_bool = text.lower() in ("true", "false", "null", "yes", "no", "on", "off", "~")
    unsafe = (
        looks_like_bool
        or text[0] in "-?:,[]{}#&*!|>'\"%@`"
        or text[0] in " \t"
        or text[-1] in " \t"
        or ": " in text
        or text.endswith(":")
        or " #" in text
        or "\n" in text
    )
    return json.dumps(text, ensure_ascii=False) if unsafe else text


def yaml_key_of(text):
    """从 `      "some:key": {}` 这类行里取出键名，正确处理引号。"""
    text = text.strip()
    if text.startswith('"'):
        end = 1
        while end < len(text):
            if text[end] == "\\":
                end += 2
                continue
            if text[end] == '"':
                break
            end += 1
        try:
            return json.loads(text[: end + 1])
        except Exception:  # noqa: BLE001
            return text[1:end]
    if text.startswith("'"):
        end = text.find("'", 1)
        return text[1:end] if end > 0 else text[1:]
    return text.split(":", 1)[0].strip()


# --------------------------------------------------------------------------- block scanning
def top_blocks(lines):
    out = []
    current = None
    for i, line in enumerate(lines):
        match = TOP_KEY_RE.match(line)
        if match:
            if current:
                out.append((current[0], current[1], i))
            current = (match.group(1), i)
    if current:
        out.append((current[0], current[1], len(lines)))
    return out


def find_block(lines, name):
    for block in top_blocks(lines):
        if block[0] == name:
            return block
    return None


PROVIDER_ENTRY_FIELDS = {"name", "base_url", "api_mode", "key_env", "model", "models", "models_discovered"}
FALLBACK_ENTRY_FIELDS = {"provider", "model", "base_url", "api_mode", "key_env"}


def check_config_shape(lines, data):
    """行扫描器与解析器要互相对得上，且条目字段都在已知集合内。

    两道哨：
      1. 条数：扫描出的条目数必须等于 PyYAML 解析出的条数（历史上真出过事：
         hermes-webui 用它自己的 PyYAML 重排整份文件后，17 个供应商被看成 4 个）；
      2. 字段：条目里出现未知字段，通常意味着写出去的缩进错了——子项被解析成了
         「同级字段」（曾经把新供应商的 models 写成同级字段，文件照样能解析，
         语义却错了）。这类错误必须在写盘那一刻就拦下，不能等用户发现模型没了。
    """
    for name, allowed, key in (("custom_providers", PROVIDER_ENTRY_FIELDS, "custom_providers"),
                               ("fallback_providers", FALLBACK_ENTRY_FIELDS, "fallback_providers")):
        rows = data.get(key) or []
        block = find_block(lines, name)
        got = len(entry_ranges(lines, block)) if block else 0
        if got != len(rows):
            raise PanelError(
                "config.yaml 的 %s 区块排版不认识：解析出 %d 条，行扫描只认出 %d 条。"
                "已拒绝读取/写入以免改坏文件（原文件未改动，可用面板「设置 → 备份回滚」退回）。"
                % (name, len(rows), got))
        for index, row in enumerate(rows):
            if not isinstance(row, dict):
                continue
            extra = sorted(set(row) - allowed)
            if extra:
                raise PanelError(
                    "config.yaml 的 %s 第 %d 条里有面板不认识的字段 %s（多半是缩进写坏了）。"
                    "已拒绝继续操作以免越改越坏；原文件未改动，可点「刷新」后到「设置 → 备份回滚」处理。"
                    % (name, index + 1, "、".join(extra)))


def block_style(lines, block):
    """list-of-dict 区块的缩进风格 → (条目缩进, 字段缩进)。

    同一份文件有两种排版都要认：
      * hermes 自己写的：`  - name: x` + 4 空格字段；
      * hermes-webui 写的：它自己的写盘会把整份文件用 PyYAML 重新序列化，
        条目变成 `- name: x` + 2 空格字段。
    以前这里硬编码 2/4，于是 WebUI 写一次盘（用户日常动作）之后，面板就把
    17 个供应商看成 4 个、回退链看成空的。缩进从区块内容里推，两种都能读，
    WebUI 也不会有「另一种排版」把嵌套列表误认成条目。
    """
    _, start, end = block
    for i in range(start + 1, end):
        raw = lines[i]
        if raw.strip() and raw.lstrip().startswith("- "):
            indent = len(raw) - len(raw.lstrip(" "))
            return indent, indent + 2
    return 2, 4  # 空区块（刚建的）：按 hermes 自己的风格写


def entry_ranges(lines, block):
    """list-of-dict 区块里每个条目的 [start, end)、名字与缩进。"""
    item_indent, field_indent = block_style(lines, block)
    _, start, end = block
    prefix = " " * item_indent + "- "
    entry_re = re.compile(r"^ {%d}- (?:name|provider):\s*(.*?)\s*$" % item_indent)
    starts = [i for i in range(start + 1, end) if lines[i].startswith(prefix)]
    out = []
    for index, item_start in enumerate(starts):
        item_end = starts[index + 1] if index + 1 < len(starts) else end
        match = entry_re.match(lines[item_start])
        out.append({"start": item_start, "end": item_end, "name": match.group(1) if match else None,
                    "indent": item_indent, "findent": field_indent})
    return out


def find_entry(entries, name):
    for entry in entries:
        if entry["name"] == name:
            return entry
    return None


def sub_range(lines, entry, key):
    """条目内 `<字段缩进><key>:` 子区块的 [start, end)。

    子区块有两种合法形态，都要收全：
      * 映射：键在 K 缩进，子项在 K+2（`models:` + `    x: {}`）；
      * 列表：键在 K 缩进，元素也从 K 缩进起（`models:` + `  - deepseek-v4-flash`）
        —— PyYAML 重排后就是这样，hermes 自己的本地供应商也这么写。
    只认映射形态的后果很严重：「加模型」会把 `x: {}` 插到列表元素前面，
    写出一个既不是列表也不是映射的坏结构。
    """
    findent = entry.get("findent", 4)
    target = " " * findent + key + ":"
    for i in range(entry["start"], entry["end"]):
        if lines[i].rstrip() == target:
            j = i + 1
            while j < entry["end"]:
                line = lines[j]
                if not line.strip():
                    break
                indent = len(line) - len(line.lstrip(" "))
                if indent > findent or (line.lstrip().startswith("- ") and indent >= findent):
                    j += 1
                    continue
                break
            return (i, j)
    return None


def models_style(lines, entry):
    """`models:` 子区块是映射还是列表 → "map" / "list"（没有子区块时按 map 写）。"""
    sub = sub_range(lines, entry, "models")
    if not sub:
        return "map"
    for i in range(sub[0] + 1, sub[1]):
        if lines[i].lstrip().startswith("- "):
            return "list"
    return "map"


def count_models(lines, entry):
    sub = sub_range(lines, entry, "models")
    if not sub:
        return []
    out = []
    for i in range(sub[0] + 1, sub[1]):
        text = lines[i].strip()
        if text.startswith("- "):          # 列表形态：`- deepseek-v4-flash`
            text = text[2:].strip()
        if text.endswith(": {}") or text.endswith(":"):
            out.append(yaml_key_of(text))
        elif text and ":" not in text:     # 列表里的裸标量元素
            out.append(text.strip('"').strip("'"))
    return out


def entry_field(lines, entry, field):
    prefix = " " * entry.get("findent", 4) + field + ":"
    for i in range(entry["start"] + 1, entry["end"]):
        if lines[i].startswith(prefix):
            return lines[i].split(":", 1)[1].strip().strip('"').strip("'")
    return None


# --------------------------------------------------------------------------- line edits
def set_scalar(lines, start, end, key, value, indent):
    """在 [start,end) 内设置 `indent<key>: value`；已存在则原地替换整行，否则插到区块尾（避开尾部空行/注释）。"""
    prefix = " " * indent + key + ":"
    for i in range(start, end):
        if lines[i].startswith(prefix):
            lines[i] = "%s %s" % (prefix, scalar(value))
            return i
    at = end
    while at - 1 > start and (not lines[at - 1].strip() or lines[at - 1].lstrip().startswith("#")):
        at -= 1
    lines.insert(at, "%s %s" % (prefix, scalar(value)))
    return at


def drop_scalar(lines, start, end, key, indent=2):
    prefix = " " * indent + key + ":"
    for i in range(end - 1, start - 1, -1):
        if lines[i].startswith(prefix):
            del lines[i]
            return True
    return False


def model_item_indent(lines, sub, findent):
    """列表形态的 `models:`：沿用已有元素的缩进。

    PyYAML 重排后写的是 4（与 `models:` 对齐），手写配置里常见 6（比字段更深）。追加/替换
    如果一律用字段缩进（4），在"元素在 6"的文件里会写出一个位于 `custom_providers` 层级的
    孤立 `- xxx`，整份文件就解析不回来了（v1.20 边界测试抓到；助手原先靠写后校验拒绝落盘）。
    """
    if sub:
        for i in range(sub[0] + 1, sub[1]):
            line = lines[i]
            if line.lstrip().startswith("- "):
                return len(line) - len(line.lstrip(" "))
    return findent


def insert_model_ids(lines, entry, model_ids):
    """把模型 id 追加进条目的 `models:` 子区块（缺则新建），返回真正新增的 id。

    新行的形状跟着现有子区块走：映射形态写 `x: {}`，列表形态写 `- x`。
    """
    existing = set(count_models(lines, entry))
    wanted = []
    for model in model_ids:
        model = str(model).strip()
        if model and model not in existing and model not in wanted:
            wanted.append(model)
    if not wanted:
        return []
    findent = entry.get("findent", 4)
    style = models_style(lines, entry)
    sub = sub_range(lines, entry, "models")
    if style == "list":
        rows = [" " * model_item_indent(lines, sub, findent) + "- %s" % scalar(m)
                for m in wanted]
    else:
        rows = [" " * (findent + 2) + "%s: {}" % scalar(m) for m in wanted]
    if sub:
        lines[sub[1]:sub[1]] = rows
        return wanted
    at = entry["end"]
    marker = " " * findent + "models_discovered:"
    for i in range(entry["start"] + 1, entry["end"]):
        if lines[i].startswith(marker):
            at = i
            break
    else:
        while at - 1 > entry["start"] and (not lines[at - 1].strip() or lines[at - 1].lstrip().startswith("#")):
            at -= 1
    lines[at:at] = [" " * findent + "models:"] + rows
    return wanted


def replace_model_ids(lines, entry, model_ids):
    """把条目的 `models:` 子区块**替换**成给定集合（v1.19 同步模式），返回 (added, removed)。

    与 insert_model_ids 的区别：这里会删掉探测结果里没有的旧条目——供应商已下线的模型不该
    继续留在配置里。缩进/形态跟着现有子区块走（映射或列表），与 insert_model_ids 同一套规则。
    """
    current = count_models(lines, entry)
    wanted = []
    for model in model_ids:
        model = str(model).strip()
        if model and model not in wanted:
            wanted.append(model)
    added = [m for m in wanted if m not in current]
    removed = [m for m in current if m not in wanted]
    if not added and not removed:
        return [], []
    findent = entry.get("findent", 4)
    style = models_style(lines, entry)
    sub = sub_range(lines, entry, "models")
    if style == "list":
        rows = [" " * model_item_indent(lines, sub, findent) + "- %s" % scalar(m)
                for m in wanted]
    else:
        rows = [" " * (findent + 2) + "%s: {}" % scalar(m) for m in wanted]
    if sub:
        lines[sub[0] + 1:sub[1]] = rows
    else:
        insert_model_ids(lines, entry, wanted)
    return added, removed


def render_provider_entry(name, base_url, key_env, api_mode, model, models, models_discovered=False,
                          item_indent=2):
    """生成一个供应商条目。item_indent 跟着目标区块的现有排版走，混合缩进会写出读不回来的文件。"""
    pad = " " * item_indent
    fpad = pad + "  "
    out = ["%s- name: %s" % (pad, scalar(name))]
    if base_url:
        out.append("%sbase_url: %s" % (fpad, scalar(base_url)))
    if key_env:
        out.append("%skey_env: %s" % (fpad, scalar(key_env)))
    if api_mode:
        out.append("%sapi_mode: %s" % (fpad, scalar(api_mode)))
    if model:
        out.append("%smodel: %s" % (fpad, scalar(model)))
    if models:
        out.append("%smodels:" % fpad)
        out.extend("%s%s: {}" % (fpad + "  ", scalar(m)) for m in models)
    if models_discovered:
        out.append("%smodels_discovered: true" % fpad)
    return out


def render_fallback_entry(provider, model, base_url=None, api_mode=None, key_env=None, item_indent=2):
    pad = " " * item_indent
    fpad = pad + "  "
    out = ["%s- provider: %s" % (pad, scalar(provider))]
    if model:
        out.append("%smodel: %s" % (fpad, scalar(model)))
    if base_url:
        out.append("%sbase_url: %s" % (fpad, scalar(base_url)))
    if api_mode:
        out.append("%sapi_mode: %s" % (fpad, scalar(api_mode)))
    if key_env:
        out.append("%skey_env: %s" % (fpad, scalar(key_env)))
    return out


def append_to_list_block(lines, block, rows):
    entries = entry_ranges(lines, block)
    at = entries[-1]["end"] if entries else block[1] + 1
    while at - 1 > block[1] and (not lines[at - 1].strip() or lines[at - 1].lstrip().startswith("#")):
        at -= 1
    lines[at:at] = rows
    return at


# --------------------------------------------------------------------------- .env
def env_read_all():
    if not os.path.exists(ENV_FILE):
        return {}
    out = {}
    with open(ENV_FILE, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            stripped = line.strip()
            if not stripped or stripped.startswith("#") or "=" not in stripped:
                continue
            key, value = stripped.split("=", 1)
            out[key.strip()] = value.strip().strip('"').strip("'")
    return out


def env_key_set(name):
    if not name:
        return False
    if os.environ.get(name):
        return True
    return bool(env_read_all().get(name))


def _env_quote(value):
    text = "" if value is None else str(value)
    if text != text.strip() or any(ch in text for ch in ' \t"\'#') or text == "":
        return json.dumps(text, ensure_ascii=False)
    return text


def env_backup(note=""):
    """写 .env 之前先留一份，返回文件名（文件不存在则返回 None）。

    密钥是这套配置里唯一不能再生的东西：改错一个值，config 的备份救不回来。
    和 config 备份共用目录与保留策略，靠 env- 前缀区分。
    """
    if not os.path.exists(ENV_FILE):
        return None
    os.makedirs(BACKUP_DIR, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    safe = "".join(ch for ch in (note or "edit") if ch.isalnum() or ch in "-_")[:40] or "edit"
    path = os.path.join(BACKUP_DIR, "env-%s-%s.env" % (stamp, safe))
    counter = 1
    while os.path.exists(path):
        path = os.path.join(BACKUP_DIR, "env-%s-%s.%d.env" % (stamp, safe, counter))
        counter += 1
    with open(ENV_FILE, "r", encoding="utf-8") as handle:
        _atomic_write(path, handle.read(), 0o600)
    try:
        files = sorted(
            (os.path.join(BACKUP_DIR, f) for f in os.listdir(BACKUP_DIR) if f.startswith("env-")),
            key=os.path.getmtime,
            reverse=True,
        )
        for stale in files[BACKUP_KEEP:]:
            os.unlink(stale)
    except Exception:  # noqa: BLE001
        pass
    return os.path.basename(path)


def env_set_key(name, value, dry_run=False, note=""):
    """更新或追加 `NAME=value`，保留其余行，原子替换，权限 600。dry_run 只返回文本。

    追加时插入到文件末尾空行之前，因此文件原有的收尾换行不受影响。
    """
    if not name:
        raise PanelError("key_env 为空，无法写入 .env")
    lines = []
    if os.path.exists(ENV_FILE):
        with open(ENV_FILE, "r", encoding="utf-8") as handle:
            lines = handle.read().split("\n")
    prefix = name + "="
    for i, line in enumerate(lines):
        if line.startswith(prefix) or line.startswith("#" + prefix):
            lines[i] = prefix + _env_quote(value)
            break
    else:
        at = len(lines)
        while at - 1 >= 0 and not lines[at - 1].strip():
            at -= 1
        lines[at:at] = ["", "# --- added by hermes-panel %s ---" % time.strftime("%Y-%m-%d %H:%M:%S"),
                        prefix + _env_quote(value)]
    text = "\n".join(lines)
    if dry_run:
        return text
    backup = env_backup(note or ("set-" + name.lower()))
    _atomic_write(ENV_FILE, text, 0o600)
    return backup


def env_del_key(name, dry_run=False, note=""):
    """清除 `NAME` 的取值。返回 "removed" / "commented" / ""。

    两种形态分开处理，都保证可逆、不丢原值：
      * 我们追加的整块（"" / 标记注释 / "NAME=值"）→ 三行一起回收，文件逐字节回到追加前；
      * 原有的 "NAME=值" 行 → 注释成 "#NAME=值"（hermes 读 .env 时忽略注释，原值留档可恢复）。
    """
    if not name or not os.path.exists(ENV_FILE):
        return ("", None)
    with open(ENV_FILE, "r", encoding="utf-8") as handle:
        lines = handle.read().split("\n")
    target = name + "="
    marker = "# --- added by hermes-panel"
    for i, line in enumerate(lines):
        if (line.startswith(marker) and i + 1 < len(lines) and lines[i + 1].startswith(target)
                and i - 1 >= 0 and lines[i - 1] == ""):
            action = "removed"
            if not dry_run:
                del lines[i - 1:i + 2]
            break
    else:
        action = ""
        for i, line in enumerate(lines):
            if line.startswith(target):
                action = "commented"
                if not dry_run:
                    lines[i] = "#" + line
                break
    if not action:
        return ("", None)
    if dry_run:
        return (action, None)
    backup = env_backup(note or ("del-" + name.lower()))
    _atomic_write(ENV_FILE, "\n".join(lines), 0o600)
    return (action, backup)


def derive_key_env(name):
    cleaned = re.sub(r"[^A-Za-z0-9]+", "_", str(name)).strip("_").upper()
    if not cleaned:
        raise PanelError("供应商名 %r 推导不出环境变量名，请手动指定 key_env" % name)
    return cleaned if cleaned.endswith("_API_KEY") else cleaned + "_API_KEY"


# --------------------------------------------------------------------------- atomic write
def _atomic_write(path, text, mode):
    tmp = "%s.panel-tmp.%d" % (path, os.getpid())
    with open(tmp, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
        handle.flush()
        os.fsync(handle.fileno())
    os.chmod(tmp, mode)
    os.replace(tmp, path)


def read_config_text():
    if not os.path.exists(CONFIG):
        raise PanelError("找不到 config.yaml：%s" % CONFIG)
    with open(CONFIG, "r", encoding="utf-8") as handle:
        return handle.read()


def read_config_stable(attempts=4, settle=0.07):
    """稳定读：stat → 读 → stat，并要求「读到的字节数 == stat 的大小」。

    为什么要这么麻烦：hermes-webui 自己写盘是 truncate + write（非原子），
    正好撞上它的写入窗口时会读到一个「截断但仍是合法 YAML」的中间态
    —— 实测见过只剩 4 个供应商的 config，这种半份内容既会误导界面，
    也会让写操作基于残缺的原文做编辑。读到不稳定就重试，重试仍不稳定就报错，
    绝不把中间态当真值。
    """
    detail = ""
    for attempt in range(attempts):
        if attempt:
            time.sleep(settle)
        before = os.stat(CONFIG)
        text = read_config_text()
        after = os.stat(CONFIG)
        stable = (after.st_mtime_ns, after.st_size) == (before.st_mtime_ns, before.st_size)
        whole = len(text.encode("utf-8")) == after.st_size
        if stable and whole:
            return text
        detail = "第%d次：mtime %s→%s size %s→%s 字节 %d" % (
            attempt + 1, before.st_mtime_ns, after.st_mtime_ns, before.st_size, after.st_size,
            len(text.encode("utf-8")))
    raise PanelError("config.yaml 正在被其他写入者改动（%s），没有读到稳定内容，请稍后重试" % detail)


def parse_config(text):
    try:
        data = yaml.safe_load(text)
    except Exception as exc:  # noqa: BLE001
        raise PanelError("config.yaml 解析失败，已拒绝写入：%s" % exc)
    if not isinstance(data, dict):
        raise PanelError("config.yaml 顶层不是映射，已拒绝写入")
    return data


def build_diff(old_lines, new_lines, changed_blocks, limit=60):
    old_map = {name: "\n".join(old_lines[s:e]) for name, s, e in top_blocks(old_lines)}
    new_map = {name: "\n".join(new_lines[s:e]) for name, s, e in top_blocks(new_lines)}
    diff = []
    for name in changed_blocks:
        before = (old_map.get(name) or "").split("\n")
        after = (new_map.get(name) or "").split("\n")
        if before == after:
            continue
        rows = list(difflib.unified_diff(before, after, lineterm="", n=1))
        diff.append({"block": name, "lines": rows[:limit], "truncated": len(rows) > limit})
    return diff, old_map, new_map


class ConfigGuard:
    """一次写入的完整守护：锁 → 读 → 编辑 → 校验 → 备份 → 原子替换。"""

    def __init__(self):
        self.lock_handle = None
        self.old_text = ""
        self.old_lines = []
        self.old_data = None

    def __enter__(self):
        os.makedirs(os.path.dirname(LOCK_FILE), exist_ok=True)
        self.lock_handle = open(LOCK_FILE, "a+", encoding="utf-8")
        fcntl.flock(self.lock_handle.fileno(), fcntl.LOCK_EX)
        self.reload()
        return self

    def __exit__(self, *_exc):
        try:
            fcntl.flock(self.lock_handle.fileno(), fcntl.LOCK_UN)
            self.lock_handle.close()
        except Exception:  # noqa: BLE001
            pass
        return False

    def reload(self):
        """稳定读 + 解析：拿到的是"某一整份"的 config，不是别人写一半的中间态。"""
        text = read_config_stable()
        lines = text.split("\n")
        data = parse_config(text)
        check_config_shape(lines, data)
        self.old_text, self.old_lines, self.old_data = text, lines, data

    def verify(self, lines, changed_blocks):
        """新内容可解析，且除目标区块外其余顶层区块逐字节未变。"""
        parse_config("\n".join(lines))
        old_map = {name: "\n".join(self.old_lines[s:e]) for name, s, e in top_blocks(self.old_lines)}
        new_map = {name: "\n".join(lines[s:e]) for name, s, e in top_blocks(lines)}
        for name in set(old_map) | set(new_map):
            if name in changed_blocks:
                continue
            if name not in old_map or name not in new_map:
                raise PanelError("内部校验失败：顶层区块 %s 数量变化，已放弃写入" % name)
            if old_map[name] != new_map[name]:
                raise PanelError("内部校验失败：区块 %s 意外变化，已放弃写入" % name)
        return old_map, new_map

    def preview(self, lines, changed_blocks):
        self.verify(lines, changed_blocks)
        diff, _old, _new = build_diff(self.old_lines, lines, changed_blocks)
        return {"ok": True, "data": {"dry_run": True, "diff": diff, "changed_blocks": sorted(changed_blocks)}}

    def commit(self, lines, changed_blocks, note):
        self.verify(lines, changed_blocks)
        diff, _old, _new = build_diff(self.old_lines, lines, changed_blocks)
        backup = self.backup(note)
        _atomic_write(CONFIG, "\n".join(lines), 0o600)
        parse_config(read_config_stable())  # 写后自检：读的也必须是稳定内容
        return {"backup": backup, "diff": diff}

    def backup(self, note):
        os.makedirs(BACKUP_DIR, exist_ok=True)
        stamp = time.strftime("%Y%m%d_%H%M%S")
        safe = "".join(ch for ch in (note or "edit") if ch.isalnum() or ch in "-_")[:40] or "edit"
        path = os.path.join(BACKUP_DIR, "config-%s-%s.yaml" % (stamp, safe))
        counter = 1
        while os.path.exists(path):
            path = os.path.join(BACKUP_DIR, "config-%s-%s.%d.yaml" % (stamp, safe, counter))
            counter += 1
        _atomic_write(path, self.old_text, 0o600)
        try:
            files = sorted(
                (os.path.join(BACKUP_DIR, f) for f in os.listdir(BACKUP_DIR) if f.startswith("config-")),
                key=os.path.getmtime,
                reverse=True,
            )
            for stale in files[BACKUP_KEEP:]:
                os.unlink(stale)
        except Exception:  # noqa: BLE001
            pass
        return path


# --------------------------------------------------------------------------- misc helpers
def tcp_open(port, host="127.0.0.1", timeout=1.2):
    try:
        with socket.create_connection((host, int(port)), timeout=timeout):
            return True
    except Exception:  # noqa: BLE001
        return False


def http_json(url, headers=None, timeout=10):
    merged = {
        "Accept": "application/json",
        # Cloudflare 会按浏览器签名拦截默认的 python-urllib UA（Error 1010），带上常见 UA 才通得过
        "User-Agent": ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                       "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36"),
    }
    merged.update(headers or {})
    request = urllib.request.Request(url, headers=merged)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.status, json.loads(response.read().decode("utf-8", "replace"))


def http_post_json(url, payload=None, timeout=15):
    request = urllib.request.Request(
        url,
        data=json.dumps(payload or {}).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json",
                 "User-Agent": ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                                "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36")},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.status, json.loads(response.read().decode("utf-8", "replace"))


def systemctl_user(args, timeout=30):
    """在非登录调用下也能用：补齐 XDG/DBUS 环境。"""
    env = dict(os.environ)
    uid = os.getuid()
    env.setdefault("XDG_RUNTIME_DIR", "/run/user/%d" % uid)
    env.setdefault("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/%d/bus" % uid)
    proc = subprocess.run(["systemctl", "--user"] + args, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout, env=env)
    return proc.returncode, (proc.stdout or "").strip(), (proc.stderr or "").strip()


def unit_props(unit, props):
    """读 systemd 单元属性。

    timestamp 一律取 unix 形态（`@1791941573`）：systemd 的 "Wed 2026-10-14 09:32:53 CST"
    在 JSON 里没法可靠解析（CST 这种缩写各语言/各时区解释不同，Node 会当 UTC-6 读成另一个时刻），
    所以这里把 epoch 直接给面板，同时补一个人可读的本地时间文本。
    """
    code, out, err = systemctl_user(["show", unit, "--timestamp=unix"] + ["-p" + p for p in props])
    if code != 0:
        return {"error": (err or out)[:200]}
    result = {}
    for line in out.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            if value.startswith("@") and value[1:].isdigit():
                epoch = int(value[1:])
                result[key + "Unix"] = epoch
                result[key] = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(epoch))
            else:
                result[key] = value
    return result


def webui_ports(payload):
    ports = []
    for raw in as_list(payload.get("webui_ports")):
        try:
            ports.append(int(raw))
        except (TypeError, ValueError):
            pass
    env_port = os.environ.get("HERMES_WEBUI_PORT")
    if env_port and env_port.isdigit() and int(env_port) not in ports:
        ports.append(int(env_port))
    for port in DEFAULT_WEBUI_PORTS:
        if port not in ports:
            ports.append(port)
    return ports


def file_info(path):
    try:
        stat = os.stat(path)
        return {"path": path, "exists": True, "size": stat.st_size, "mtime": int(stat.st_mtime),
                "mtime_text": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(stat.st_mtime)),
                "mode": oct(stat.st_mode)[-3:]}
    except FileNotFoundError:
        return {"path": path, "exists": False, "size": 0, "mtime": 0, "mtime_text": "", "mode": ""}


def normalize_provider_name(name):
    name = str(name or "").strip()
    if not name:
        return name
    return name if name.startswith("custom:") else "custom:" + name


def provider_short(provider):
    provider = str(provider or "")
    return provider[len("custom:"):] if provider.startswith("custom:") else provider


def _read_lines():
    return read_config_stable().split("\n")


# --------------------------------------------------------------------------- read snapshots
def snap_model(lines):
    block = find_block(lines, "model")
    out = {"default": None, "provider": None, "base_url": None, "api_mode": None}
    if not block:
        return out
    for i in range(block[1], block[2]):
        for key in list(out):
            if lines[i].startswith("  %s:" % key):
                out[key] = lines[i].split(":", 1)[1].strip().strip('"').strip("'")
    supports = False
    for i in range(block[1], block[2]):
        if lines[i].startswith("  supports_vision:"):
            supports = lines[i].split(":", 1)[1].strip().lower() == "true"
    out["supports_vision"] = supports
    return out


def snap_providers(lines, current_provider=None):
    block = find_block(lines, "custom_providers")
    if not block:
        return []
    if current_provider is None:
        current_provider = snap_model(lines).get("provider")
    out = []
    for entry in entry_ranges(lines, block):
        key_env = entry_field(lines, entry, "key_env")
        out.append({
            "name": entry["name"],
            "provider_id": "custom:%s" % entry["name"],
            "base_url": entry_field(lines, entry, "base_url"),
            "key_env": key_env,
            "key_set": env_key_set(key_env),
            "api_mode": entry_field(lines, entry, "api_mode"),
            "default_model": entry_field(lines, entry, "model"),
            "models": count_models(lines, entry),
            "models_discovered": entry_field(lines, entry, "models_discovered") == "true",
            "is_active": current_provider == "custom:%s" % entry["name"],
            "line": entry["start"] + 1,
        })
    return out


def snap_fallbacks(lines):
    block = find_block(lines, "fallback_providers")
    if not block:
        return []
    out = []
    for index, entry in enumerate(entry_ranges(lines, block)):
        key_env = entry_field(lines, entry, "key_env")
        out.append({
            "index": index,
            "provider": entry["name"],
            "model": entry_field(lines, entry, "model"),
            "base_url": entry_field(lines, entry, "base_url"),
            "api_mode": entry_field(lines, entry, "api_mode"),
            "key_env": key_env,
            "key_set": env_key_set(key_env),
            "line": entry["start"] + 1,
        })
    return out


# --------------------------------------------------------------------------- read commands
def cmd_health(payload):
    info = file_info(CONFIG)
    parses, parse_error = True, None
    if info["exists"]:
        try:
            parse_config(read_config_stable())
        except PanelError as exc:
            parses, parse_error = False, str(exc)
    webui = [{"port": port, "reachable": tcp_open(port)} for port in webui_ports(payload)]
    gateway_port = int(payload.get("gateway_port") or os.environ.get("HERMES_GATEWAY_PORT") or DEFAULT_GATEWAY_PORT)
    return {
        "version": VERSION,
        "hermes_home": HERMES_HOME,
        "config": dict(info, parses=parses, parse_error=parse_error),
        "env_file": file_info(ENV_FILE),
        "backup_dir": BACKUP_DIR,
        "python": sys.version.split()[0],
        "yaml": getattr(yaml, "__version__", "?"),
        "uid": os.getuid(),
        "webui": webui,
        "webui_active": next((row["port"] for row in webui if row["reachable"]), None),
        "gateway": {
            "port": gateway_port,
            "reachable": tcp_open(gateway_port),
            "unit": unit_props("hermes-gateway", ["ActiveState", "ActiveEnterTimestamp", "MainPID"]),
        },
    }


def cmd_list(payload):
    with ConfigGuard() as guard:
        lines = guard.old_lines
        current = snap_model(lines)
        data = {
            "model": current,
            "providers": snap_providers(lines, current.get("provider")),
            "fallbacks": snap_fallbacks(lines),
            "files": {"config": file_info(CONFIG), "env": file_info(ENV_FILE)},
            "backup_dir": BACKUP_DIR,
            "version": VERSION,
            "uid": os.getuid(),
        }
    gateway_port = int(payload.get("gateway_port") or DEFAULT_GATEWAY_PORT)
    data["gateway"] = {"port": gateway_port, "reachable": tcp_open(gateway_port),
                       "unit": unit_props("hermes-gateway", ["ActiveState", "ActiveEnterTimestamp", "MainPID"])}
    return data


def cmd_backup_list(payload):
    limit = int(payload.get("limit") or 30)
    if not os.path.isdir(BACKUP_DIR):
        return {"backups": []}
    rows = []
    for name in os.listdir(BACKUP_DIR):
        if name.startswith("config-") and name.endswith(".yaml"):
            kind = "config"
        elif name.startswith("env-") and name.endswith(".env"):
            kind = "env"
        else:
            continue
        path = os.path.join(BACKUP_DIR, name)
        stat = os.stat(path)
        rows.append({"file": name, "path": path, "kind": kind, "size": stat.st_size,
                     "mtime": int(stat.st_mtime),
                     "mtime_text": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(stat.st_mtime))})
    rows.sort(key=lambda row: row["mtime"], reverse=True)
    return {"backups": rows[:limit]}


# --------------------------------------------------------------------------- write commands
def _resolve_target(lines, payload):
    model = str(payload.get("model") or "").strip()
    if not model:
        raise PanelError("model 不能为空")
    provider = normalize_provider_name(payload.get("provider"))
    base_url = payload.get("base_url")
    api_mode = payload.get("api_mode")
    if provider.startswith("custom:"):
        block = find_block(lines, "custom_providers")
        if not block:
            raise PanelError("config.yaml 里没有 custom_providers 区块")
        entry = find_entry(entry_ranges(lines, block), provider_short(provider))
        if not entry:
            raise PanelError("未找到供应商 %s，请先在面板里添加" % provider)
        base_url = base_url or entry_field(lines, entry, "base_url")
        api_mode = api_mode or entry_field(lines, entry, "api_mode")
    return {"model": model, "provider": provider, "base_url": base_url, "api_mode": api_mode}


def _apply_model_block(lines, target, payload, changes):
    block = find_block(lines, "model")
    if not block:
        raise PanelError("config.yaml 里没有 model 区块")
    set_scalar(lines, block[1], block[2], "default", target["model"], 2)
    changes.add("model")
    block = find_block(lines, "model")
    set_scalar(lines, block[1], block[2], "provider", target["provider"], 2)
    block = find_block(lines, "model")
    if target["base_url"]:
        set_scalar(lines, block[1], block[2], "base_url", str(target["base_url"]).rstrip("/"), 2)
    elif target["provider"] == "openai":
        set_scalar(lines, block[1], block[2], "base_url", "https://api.openai.com/v1", 2)
    else:
        drop_scalar(lines, block[1], block[2], "base_url", 2)
    block = find_block(lines, "model")
    if target["api_mode"]:
        set_scalar(lines, block[1], block[2], "api_mode", target["api_mode"], 2)
    if payload.get("supports_vision") is not None:
        block = find_block(lines, "model")
        set_scalar(lines, block[1], block[2], "supports_vision", bool(payload["supports_vision"]), 2)


def _write_result(result, **extra):
    data = {"dry_run": False, "backup": result.get("backup"), "diff": result.get("diff") or []}
    try:  # 面板用 mtime 判断「这次改动是否已被网关热读」，省一次 health 往返
        st = os.stat(CONFIG)
        data["config_mtime"] = int(st.st_mtime)
        data["config_size"] = st.st_size
    except OSError:
        pass
    data.update({k: v for k, v in extra.items() if v is not None})
    return {"ok": True, "data": data}


def cmd_set_main(payload):
    dry_run = bool(payload.get("dry_run"))
    with ConfigGuard() as guard:
        lines = list(guard.old_lines)
        target = _resolve_target(lines, payload)
        changes = set()
        _apply_model_block(lines, target, payload, changes)
        # 供应商条目自己的 `model:` 默认值默认不动：它常常与全局默认不同（例如 doulor 条目是
        # workbuddy-deepseek-v4.1-flash，而全局是 workbuddy-global-...），切换不该把它抹掉。
        if payload.get("sync_entry_default") and target["provider"].startswith("custom:"):
            block = find_block(lines, "custom_providers")
            entry = find_entry(entry_ranges(lines, block), provider_short(target["provider"]))
            if entry:
                set_scalar(lines, entry["start"], entry["end"], "model", target["model"], entry["findent"])
                changes.add("custom_providers")
        if dry_run:
            return guard.preview(lines, changes)
        result = guard.commit(lines, changes, "set-main")
    return _write_result(result, provider=target["provider"], model=target["model"])


def cmd_upsert_provider(payload):
    dry_run = bool(payload.get("dry_run"))
    name = provider_short(normalize_provider_name(payload.get("name")))
    if not NAME_RE.match(name or ""):
        raise PanelError("供应商名只能用字母、数字、点、下划线和短横线（≤64 字符）")
    base_url = str(payload.get("base_url") or "").strip()
    if base_url and not base_url.lower().startswith(("http://", "https://")):
        raise PanelError("base_url 必须以 http:// 或 https:// 开头")
    api_mode = payload.get("api_mode") or None
    key_env = payload.get("key_env") or None
    key_value = payload.get("key")
    models = [str(m).strip() for m in as_list(payload.get("models")) if str(m).strip()]
    entry_model = payload.get("model") or (models[0] if models else None)

    with ConfigGuard() as guard:
        lines = list(guard.old_lines)
        block = find_block(lines, "custom_providers")
        if not block:
            raise PanelError("config.yaml 里没有 custom_providers 区块")
        changes = {"custom_providers"}
        entry = find_entry(entry_ranges(lines, block), name)
        created = entry is None
        if created:
            if not base_url:
                raise PanelError("新建供应商必须提供 base_url")
            if not key_env and payload.get("key_env_auto", True):
                key_env = derive_key_env(name)
            rows = render_provider_entry(name, base_url, key_env, api_mode, entry_model, models,
                                         item_indent=block_style(lines, block)[0])
            append_to_list_block(lines, block, rows)
        else:
            if base_url:
                set_scalar(lines, entry["start"], entry["end"], "base_url", base_url, entry["findent"])
            if api_mode:
                set_scalar(lines, entry["start"], entry["end"], "api_mode", api_mode, entry["findent"])
            if entry_model:
                set_scalar(lines, entry["start"], entry["end"], "model", entry_model, entry["findent"])
            if key_env:
                set_scalar(lines, entry["start"], entry["end"], "key_env", key_env, entry["findent"])
            key_env = key_env or entry_field(lines, entry, "key_env")
            if key_value and not key_env:
                key_env = derive_key_env(name)
                set_scalar(lines, entry["start"], entry["end"], "key_env", key_env, entry["findent"])
            if models:
                block = find_block(lines, "custom_providers")
                entry = find_entry(entry_ranges(lines, block), name)
                insert_model_ids(lines, entry, models)
        if key_value and not key_env:
            key_env = derive_key_env(name)

        if payload.get("set_main"):
            block = find_block(lines, "custom_providers")
            entry = find_entry(entry_ranges(lines, block), name)
            target = {
                "model": payload.get("set_main_model") or entry_model,
                "provider": "custom:%s" % name,
                "base_url": (base_url or (entry_field(lines, entry, "base_url") if entry else None)),
                "api_mode": api_mode or (entry_field(lines, entry, "api_mode") if entry else None),
            }
            if not target["model"]:
                raise PanelError("要设为当前模型，必须提供 model 或至少一个 models 条目")
            if not target["base_url"]:
                raise PanelError("要设为当前模型，必须提供 base_url")
            _apply_model_block(lines, target, payload, changes)

        if dry_run:
            preview = guard.preview(lines, changes)
            preview["data"]["env_will_write"] = key_env if key_value is not None else None
            return preview
        result = guard.commit(lines, changes, "upsert-provider-%s" % name)

    env_bk = None
    if key_value is not None:
        env_bk = env_set_key(key_env, key_value, note="upsert-provider-%s" % name)
    return _write_result(result, created=created, key_env=key_env if key_value is not None else None,
                         provider="custom:%s" % name, model=entry_model, env_backup=env_bk)


def cmd_add_models(payload):
    dry_run = bool(payload.get("dry_run"))
    # v1.19（用户要求）：prune=true = 「扫描并同步」——把该供应商的 models 列表替换成探测结果，
    # 供应商已下线的模型一并删除。默认不带这个参数，老调用方的行为（只追加）完全不变。
    prune = bool(payload.get("prune"))
    name = provider_short(normalize_provider_name(payload.get("provider")))
    models = [str(m).strip() for m in as_list(payload.get("models")) if str(m).strip()]
    if not name or not models:
        raise PanelError("需要 provider 与至少一个模型 id")
    with ConfigGuard() as guard:
        lines = list(guard.old_lines)
        block = find_block(lines, "custom_providers")
        entry = find_entry(entry_ranges(lines, block), name) if block else None
        if not entry:
            raise PanelError("未找到供应商 %s" % name)
        if prune:
            added, removed = replace_model_ids(lines, entry, models)
            if not added and not removed:
                return {"ok": True, "data": {"dry_run": dry_run, "added": [], "removed": [],
                                             "diff": []}}
            if dry_run:
                preview = guard.preview(lines, {"custom_providers"})
                preview["data"]["added"] = added
                preview["data"]["removed"] = removed
                return preview
            result = guard.commit(lines, {"custom_providers"}, "sync-models-%s" % name)
            return _write_result(result, added=added, removed=removed, kept=len(models) - len(added))
        added = insert_model_ids(lines, entry, models)
        if not added:
            return {"ok": True, "data": {"dry_run": dry_run, "added": [], "diff": []}}
        if dry_run:
            preview = guard.preview(lines, {"custom_providers"})
            preview["data"]["added"] = added
            return preview
        result = guard.commit(lines, {"custom_providers"}, "add-models-%s" % name)
    return _write_result(result, added=added)


def cmd_delete_provider(payload):
    name = provider_short(normalize_provider_name(payload.get("name") or payload.get("provider")))
    if not name:
        raise PanelError("需要 name")
    with ConfigGuard() as guard:
        lines = list(guard.old_lines)
        block = find_block(lines, "custom_providers")
        entry = find_entry(entry_ranges(lines, block), name) if block else None
        if not entry:
            raise PanelError("未找到供应商 %s" % name)
        key_env = entry_field(lines, entry, "key_env")
        current = snap_model(lines)
        if current.get("provider") == "custom:%s" % name and not payload.get("force"):
            raise PanelError("该供应商正是当前模型（%s），请先切换再删除" % (current.get("default") or ""))
        fallback_refs = [row["index"] for row in snap_fallbacks(lines) if row["provider"] == name]
        if fallback_refs and not payload.get("remove_fallbacks"):
            raise PanelError("回退链里还有 %d 条引用它（第 %s 条），请先移除或勾选「一并移除」"
                             % (len(fallback_refs), ", ".join(str(i + 1) for i in fallback_refs)))
        changes = {"custom_providers"}
        del lines[entry["start"]:entry["end"]]
        if fallback_refs and payload.get("remove_fallbacks"):
            fblock = find_block(lines, "fallback_providers")
            if fblock:
                for row in reversed(entry_ranges(lines, fblock)):
                    if row["name"] == name:
                        del lines[row["start"]:row["end"]]
                changes.add("fallback_providers")
        result = guard.commit(lines, changes, "delete-provider-%s" % name)
    if payload.get("purge_key") and key_env:
        purged, env_bk = env_del_key(key_env, note="delete-provider-%s" % name)
    else:
        purged, env_bk = "", None
    return _write_result(result, deleted=name, key_env=key_env, purged_key=purged,
                         removed_fallbacks=len(fallback_refs) if payload.get("remove_fallbacks") else 0,
                         env_backup=env_bk)


def cmd_set_provider_key(payload):
    name = provider_short(normalize_provider_name(payload.get("provider")))
    if not name:
        raise PanelError("需要 provider")
    key_value = payload.get("key")
    with ConfigGuard() as guard:
        lines = list(guard.old_lines)
        block = find_block(lines, "custom_providers")
        entry = find_entry(entry_ranges(lines, block), name) if block else None
        if not entry:
            raise PanelError("未找到供应商 %s" % name)
        current_env = entry_field(lines, entry, "key_env")
        target_env = payload.get("key_env") or current_env or derive_key_env(name)
        changes = set()
        if current_env != target_env:
            set_scalar(lines, entry["start"], entry["end"], "key_env", target_env, entry["findent"])
            changes.add("custom_providers")
        result = guard.commit(lines, changes, "key-%s-%s" % ("clear" if key_value is None else "set", name)) \
            if changes else {"backup": None, "diff": []}
    cleared = key_value is None
    if cleared:
        action, env_bk = env_del_key(target_env, note="clear-key-%s" % name)
    else:
        env_bk = env_set_key(target_env, key_value, note="set-key-%s" % name)
        action = "set"
    return _write_result(result, key_env=target_env, cleared=cleared, key_action=action, env_backup=env_bk)


def cmd_fallback_add(payload):
    dry_run = bool(payload.get("dry_run"))
    provider = str(payload.get("provider") or "").strip()
    model = str(payload.get("model") or "").strip()
    if not model:
        raise PanelError("回退条目需要 model")
    if not provider and not payload.get("from_custom"):
        raise PanelError("回退条目需要 provider（或指定 from_custom 从已有供应商继承）")
    base_url = payload.get("base_url")
    api_mode = payload.get("api_mode")
    key_env = payload.get("key_env")
    key_value = payload.get("key")
    with ConfigGuard() as guard:
        lines = list(guard.old_lines)
        if payload.get("from_custom"):
            name = provider_short(normalize_provider_name(payload["from_custom"]))
            block = find_block(lines, "custom_providers")
            entry = find_entry(entry_ranges(lines, block), name) if block else None
            if not entry:
                raise PanelError("未找到供应商 %s" % name)
            provider = name
            base_url = base_url or entry_field(lines, entry, "base_url")
            api_mode = api_mode or entry_field(lines, entry, "api_mode")
            key_env = key_env or entry_field(lines, entry, "key_env")
        fblock = find_block(lines, "fallback_providers")
        if not fblock:
            raise PanelError("config.yaml 里没有 fallback_providers 区块")
        for row in snap_fallbacks(lines):
            if row["provider"] == provider and row["model"] == model:
                raise PanelError("回退链里已经有 %s / %s" % (provider, model))
        rows = render_fallback_entry(provider, model, base_url, api_mode, key_env,
                                    item_indent=block_style(lines, fblock)[0])
        entries = entry_ranges(lines, fblock)
        position = payload.get("position")
        if position is None:
            append_to_list_block(lines, fblock, rows)
        else:
            position = max(0, min(int(position), len(entries)))
            at = entries[position]["start"] if position < len(entries) else entries[-1]["end"]
            while at - 1 > fblock[1] and (not lines[at - 1].strip() or lines[at - 1].lstrip().startswith("#")):
                at -= 1
            lines[at:at] = rows
        if dry_run:
            preview = guard.preview(lines, {"fallback_providers"})
            preview["data"]["env_will_write"] = key_env if key_value else None
            return preview
        result = guard.commit(lines, {"fallback_providers"}, "fallback-add-%s" % provider)
    env_bk = None
    if key_value and key_env:
        env_bk = env_set_key(key_env, key_value, note="fallback-add-%s" % provider)
    return _write_result(result, env_backup=env_bk)


def cmd_fallback_remove(payload):
    with ConfigGuard() as guard:
        lines = list(guard.old_lines)
        block = find_block(lines, "fallback_providers")
        if not block:
            raise PanelError("config.yaml 里没有 fallback_providers 区块")
        entries = entry_ranges(lines, block)
        target = None
        index = payload.get("index")
        if index is not None and 0 <= int(index) < len(entries):
            target = entries[int(index)]
        else:
            for row in entries:
                if row["name"] == payload.get("provider") and (
                        payload.get("model") is None or entry_field(lines, row, "model") == payload.get("model")):
                    target = row
                    break
        if not target:
            raise PanelError("未找到该回退条目")
        removed = lines[target["start"]:target["end"]][:1]
        del lines[target["start"]:target["end"]]
        result = guard.commit(lines, {"fallback_providers"}, "fallback-remove")
    return _write_result(result, removed=removed)


def cmd_fallback_move(payload):
    index = int(payload.get("index"))
    to = int(payload.get("to"))
    with ConfigGuard() as guard:
        lines = list(guard.old_lines)
        block = find_block(lines, "fallback_providers")
        if not block:
            raise PanelError("config.yaml 里没有 fallback_providers 区块")
        entries = entry_ranges(lines, block)
        if not (0 <= index < len(entries)):
            raise PanelError("index 越界")
        to = max(0, min(to, len(entries) - 1))
        if to == index:
            return {"ok": True, "data": {"moved": False}}
        rows = lines[entries[index]["start"]:entries[index]["end"]]
        del lines[entries[index]["start"]:entries[index]["end"]]
        block = find_block(lines, "fallback_providers")
        entries = entry_ranges(lines, block)
        if to < len(entries):
            at = entries[to]["start"]
        else:
            at = entries[-1]["end"]
            while at - 1 > block[1] and not lines[at - 1].strip():
                at -= 1
        lines[at:at] = rows
        result = guard.commit(lines, {"fallback_providers"}, "fallback-move")
    return _write_result(result, moved=True, to=to)


def cmd_restore(payload):
    name = os.path.basename(str(payload.get("file") or ""))
    source = os.path.join(BACKUP_DIR, name)
    if not name or not os.path.exists(source):
        raise PanelError("备份文件不存在：%s" % name)
    return cmd_restore_env(name, source) if name.startswith("env-") else cmd_restore_config(name, source)


def cmd_restore_env(name, source):
    """把 .env 恢复成备份内容：先给当前 .env 留一份，再整份替换（600）。"""
    with open(source, "r", encoding="utf-8") as handle:
        text = handle.read()
    if not text.strip():
        raise PanelError("这份 .env 备份是空的，拒绝恢复")
    kept = env_backup("before-restore")
    _atomic_write(ENV_FILE, text, 0o600)
    keys = sorted({line.split("=", 1)[0].strip() for line in text.split("\n")
                   if "=" in line and not line.lstrip().startswith("#")})
    return {"changed": True, "dry_run": False, "backup": kept,
            "diff": ["恢复 .env：%d 个键（%s…）" % (len(keys), ", ".join(keys[:6]))],
            "restored_from": name, "target": "env",
            "env_size": os.path.getsize(ENV_FILE)}


def cmd_restore_config(name, source):
    with open(source, "r", encoding="utf-8") as handle:
        text = handle.read()
    parse_config(text)
    lines = text.split("\n")
    with ConfigGuard() as guard:
        old_map = {n: "\n".join(guard.old_lines[s:e]) for n, s, e in top_blocks(guard.old_lines)}
        new_map = {n: "\n".join(lines[s:e]) for n, s, e in top_blocks(lines)}
        changed = {n for n in set(old_map) | set(new_map) if old_map.get(n) != new_map.get(n)}
        result = guard.commit(lines, changed, "restore")
    return _write_result(result, restored_from=name, changed_blocks=sorted(changed))


# --------------------------------------------------------------------------- network probe
def extract_model_ids(body):
    rows = []
    if isinstance(body, dict):
        data = body.get("data")
        if isinstance(data, list):
            rows = data
        elif isinstance(body.get("models"), list):
            rows = body["models"]
    elif isinstance(body, list):
        rows = body
    out = set()
    for row in rows:
        if isinstance(row, str):
            out.add(row)
        elif isinstance(row, dict):
            ident = row.get("id") or row.get("name") or row.get("model")
            if ident:
                out.add(str(ident))
    return sorted(out)


def cmd_probe(payload):
    base_url = str(payload.get("base_url") or "").strip().rstrip("/")
    if not base_url:
        raise PanelError("需要 base_url")
    key = payload.get("key")
    if not key and payload.get("key_env"):
        key = env_read_all().get(payload["key_env"])
    timeout = float(payload.get("timeout_s") or 8)
    candidates = []
    for url in (base_url, base_url + "/models",
                base_url + "/models" if base_url.endswith("/v1") else base_url + "/v1/models"):
        if url not in candidates:
            candidates.append(url)
    headers = {"Accept": "application/json"}
    if key:
        headers["Authorization"] = "Bearer %s" % key
    attempts = []
    for url in candidates:
        started = time.time()
        try:
            status, body = http_json(url, headers=headers, timeout=timeout)
        except urllib.error.HTTPError as exc:
            attempts.append({"url": url, "status": exc.code, "ms": int((time.time() - started) * 1000),
                             "error": exc.read().decode("utf-8", "replace")[:200]})
            continue
        except Exception as exc:  # noqa: BLE001
            attempts.append({"url": url, "status": 0, "ms": int((time.time() - started) * 1000),
                             "error": str(exc)[:200]})
            continue
        models = extract_model_ids(body)
        attempts.append({"url": url, "status": status, "count": len(models), "ms": int((time.time() - started) * 1000)})
        if models:
            return {"ok": True, "data": {"models": models, "count": len(models), "url": url, "attempts": attempts}}
    return {"ok": False, "error": "探测失败：候选地址都没有返回模型列表", "data": {"attempts": attempts}}


def cmd_probe_fallback(payload):
    """逐条探测回退链的可达性。"""
    rows = []
    with ConfigGuard() as guard:
        fallbacks = snap_fallbacks(guard.old_lines)
    env_values = env_read_all()
    for row in fallbacks:
        key = env_values.get(row["key_env"]) if row["key_env"] else None
        single = cmd_probe({"base_url": row["base_url"], "key": key, "timeout_s": payload.get("timeout_s") or 6})
        rows.append({"index": row["index"], "provider": row["provider"], "model": row["model"],
                     "base_url": row["base_url"], "ok": bool(single.get("ok")),
                     "count": (single.get("data") or {}).get("count", 0),
                     "error": None if single.get("ok") else single.get("error"),
                     "has_model": row["model"] in (single.get("data") or {}).get("models", [])})
    return {"ok": True, "data": {"rows": rows}}


def cmd_webui_refresh(payload):
    """清掉 WebUI 的模型缓存（内存 60s + 磁盘 24h），让新加/改动的供应商立刻出现在它的列表里。

    注意 WebUI 的 /api/models 只列出「拿得到模型」的供应商：base_url 不可达的供应商会被它隐藏，
    面板自身的列表以 config.yaml 为准，不受此影响。
    """
    providers = as_list(payload.get("providers"))
    if not providers:
        with ConfigGuard() as guard:
            providers = [p["provider_id"] for p in snap_providers(guard.old_lines)]
    url = None
    for port in webui_ports(payload):
        if tcp_open(port):
            url = "http://127.0.0.1:%d/api/models/refresh" % port
            break
    if not url:
        return {"ok": False, "error": "WebUI 不可达，无法刷新它的模型缓存"}
    results = {}
    for provider in providers:
        try:
            _status, body = http_post_json(url, {"provider": provider})
            results[provider] = body
        except urllib.error.HTTPError as exc:
            results[provider] = {"error": "HTTP %s %s" % (exc.code, exc.read().decode("utf-8", "replace")[:120])}
        except Exception as exc:  # noqa: BLE001
            results[provider] = {"error": str(exc)[:120]}
    return {"ok": True, "data": {"port": url.rsplit(":", 1)[-1].split("/")[0], "refreshed": results}}


# --------------------------------------------------------------------------- service ops
def cmd_reload(payload):
    targets = as_list(payload.get("targets")) or ["webui"]
    mode = payload.get("mode") or "auto"
    out = {}
    for target in targets:
        if target == "webui":
            code, stdout, stderr = systemctl_user(["restart", "hermes-webui"], timeout=60)
            out["webui"] = {"rc": code, "out": stdout[:200], "err": stderr[:200],
                            "unit": unit_props("hermes-webui", ["ActiveState", "ActiveEnterTimestamp"])}
        elif target == "gateway":
            verb = "restart" if mode == "restart" else "reload"
            code, stdout, stderr = systemctl_user([verb, "hermes-gateway"], timeout=240)
            out["gateway"] = {"rc": code, "verb": verb, "out": stdout[:200], "err": stderr[:200],
                              "unit": unit_props("hermes-gateway", ["ActiveState", "ActiveEnterTimestamp"])}
        else:
            out[target] = {"rc": -1, "err": "未知目标"}
    ok = all(row.get("rc", 1) == 0 for row in out.values())
    return ({"ok": True, "data": out} if ok else {"ok": False, "error": "部分服务操作失败", "data": out})


def cmd_webui_models(payload):
    last_error = None
    for port in webui_ports(payload):
        try:
            status, body = http_json("http://127.0.0.1:%d/api/models" % port, timeout=15)
            return {"ok": True, "data": {
                "port": port, "status": status,
                "default_model": body.get("default_model"),
                "active_provider": body.get("active_provider"),
                "groups": [{"provider": g.get("provider"), "provider_id": g.get("provider_id"),
                            "models": [m.get("id") for m in (g.get("models") or [])]}
                           for g in (body.get("groups") or [])]}}
        except Exception as exc:  # noqa: BLE001
            last_error = str(exc)[:200]
    return {"ok": False, "error": "WebUI 不可达：%s" % last_error}


# --------------------------------------------------------------------------- plumbing
COMMANDS = {
    "ping": lambda payload: {"ok": True, "data": {"pong": True, "version": VERSION}},
    "health": cmd_health,
    "list": cmd_list,
    "webui-models": cmd_webui_models,
    "webui-refresh": cmd_webui_refresh,
    "backup-list": cmd_backup_list,
    "set-main": cmd_set_main,
    "upsert-provider": cmd_upsert_provider,
    "add-models": cmd_add_models,
    "delete-provider": cmd_delete_provider,
    "set-provider-key": cmd_set_provider_key,
    "fallback-list": lambda payload: {"ok": True, "data": {"fallbacks": snap_fallbacks(_read_lines())}},
    "fallback-add": cmd_fallback_add,
    "fallback-remove": cmd_fallback_remove,
    "fallback-move": cmd_fallback_move,
    "fallback-probe": cmd_probe_fallback,
    "probe": cmd_probe,
    "reload": cmd_reload,
    "restore": cmd_restore,
}


def main():
    global HERMES_HOME, CONFIG, ENV_FILE, BACKUP_DIR, LOCK_FILE
    raw = sys.stdin.readline()
    if not raw.strip():
        print(json.dumps({"ok": False, "error": "空请求"}, ensure_ascii=False))
        return 2
    try:
        payload = json.loads(raw)
    except Exception as exc:  # noqa: BLE001
        print(json.dumps({"ok": False, "error": "请求不是合法 JSON：%s" % exc}, ensure_ascii=False))
        return 2
    if not isinstance(payload, dict):
        print(json.dumps({"ok": False, "error": "请求必须是 JSON 对象"}, ensure_ascii=False))
        return 2
    if payload.get("hermes_home"):
        HERMES_HOME = os.path.expanduser(str(payload["hermes_home"]))
        CONFIG = os.path.join(HERMES_HOME, "config.yaml")
        ENV_FILE = os.path.join(HERMES_HOME, ".env")
        BACKUP_DIR = os.path.join(HERMES_HOME, "backups", "config-changes")
        LOCK_FILE = os.path.join(HERMES_HOME, ".panel-config.lock")
    cmd = payload.get("cmd") or "ping"
    handler = COMMANDS.get(cmd)
    if not handler:
        print(json.dumps({"ok": False, "error": "未知命令：%s" % cmd}, ensure_ascii=False))
        return 2
    try:
        result = handler(payload)
        if isinstance(result, dict) and "ok" not in result:
            result = {"ok": True, "data": result}
    except PanelError as exc:
        result = {"ok": False, "error": str(exc)}
    except Exception as exc:  # noqa: BLE001
        import traceback
        result = {"ok": False, "error": "%s: %s" % (type(exc).__name__, exc),
                  "trace": traceback.format_exc()[-900:]}
    try:
        print(json.dumps(result, ensure_ascii=False))
    except (UnicodeEncodeError, BrokenPipeError):  # pragma: no cover
        print(json.dumps({"ok": False, "error": "响应编码失败"}, ensure_ascii=True))
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
