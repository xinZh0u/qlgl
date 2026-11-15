#!/usr/bin/env python3
"""检查 typedef 命名是否遵守 doc/命名与API风格.md。

规则（只查 typedef，避免对普通标识符误报）：
  1. 函数指针 typedef：名字必须以 `_cb` 结尾；
  2. 其它 typedef：`QL` + CamelCase，或 `ql_..._t`；
  3. 白名单：QLType（运行时类型 id，类比 GType）。

用法：
    python3 tools/check_style.py            # 默认扫 include/qlgl 与 src/
    python3 tools/check_style.py a.h b.h    # 只扫指定文件
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# `QLType` 是唯一允许的 CamelCase 标量：运行时类型 id（见风格文档 §1）。
ALLOW = {"QLType"}

FN_PTR = re.compile(r"\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")
LAST_NAME = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$", re.S)
CAMEL = re.compile(r"QL[A-Z][A-Za-z0-9]*")
SCALAR = re.compile(r"ql_[a-z0-9_]+_t")


def strip_directives(text: str) -> str:
    """把预处理指令（含续行）替换成空白，保留换行与偏移，宏体不会被当成 typedef。"""
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        line_start = i
        while i < n and text[i] in " \t":
            i += 1
        if i < n and text[i] == "#":
            # 整条指令（可能带 \ 续行）
            while i < n:
                if text[i] == "\n" and (i == 0 or text[i - 1] != "\\"):
                    break
                if text[i] != "\n":
                    out[i] = " "
                i += 1
        else:
            while i < n and text[i] != "\n":
                i += 1
        if i < n:
            i += 1   # 跳过换行
    return "".join(out)


def _depth_at(stmt: str, target: int) -> int:
    depth = 0
    for c in stmt[:target]:
        if c in "({[":
            depth += 1
        elif c in ")}]":
            depth = max(0, depth - 1)
    return depth


def typedef_statements(text: str):
    """逐条产出 (起始偏移, 语句)——按花括号深度找结尾的 `;`。"""
    pos = 0
    while True:
        m = re.search(r"\btypedef\b", text[pos:])
        if m is None:
            return
        start = pos + m.start()
        i = start + len("typedef")
        depth = 0
        while i < len(text):
            c = text[i]
            if c in "({[":
                depth += 1
            elif c in ")}]":
                depth = max(0, depth - 1)
            elif c == ";" and depth == 0:
                break
            i += 1
        if i >= len(text):
            return
        yield start, text[start:i + 1]
        pos = i + 1


def check_file(path: Path):
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        return [(path, 0, "?", "无法读取：%s" % exc)]
    scan = strip_directives(text)
    bad = []
    for start, stmt in typedef_statements(scan):
        line = text.count("\n", 0, start) + 1
        # 只认"语句顶层"的函数指针；结构体成员（深度 > 0）不算 typedef 名
        fn = None
        for m in FN_PTR.finditer(stmt):
            if _depth_at(stmt, m.start()) == 0:
                fn = m
                break
        if fn is not None:
            name = fn.group(1)
            if not name.endswith("_cb"):
                bad.append((path, line, name, "回调 typedef 必须以 _cb 结尾"))
            continue
        m = LAST_NAME.search(stmt)
        if m is None or _depth_at(stmt, m.start()) != 0:
            continue
        name = m.group(1)
        body = stmt[:m.start()]
        aggregate = re.search(r"\b(struct|union|enum)\b", body) is not None
        if aggregate:
            # 结构体 / 句柄 / 枚举：QL + CamelCase
            if not CAMEL.fullmatch(name):
                bad.append((path, line, name,
                            "结构体/句柄/枚举必须用 QL+CamelCase"))
        elif name in ALLOW or SCALAR.fullmatch(name):
            continue
        else:
            # 标量别名：ql_..._t（QLType 是白名单例外）
            bad.append((path, line, name,
                        "标量别名必须是 ql_*_t（QLType 除外）"))
    return bad


def default_files():
    files = sorted((ROOT / "include" / "qlgl").glob("*.h"))
    for sub in ("core", "draw", "text", "ui", "port"):
        files.extend(sorted((ROOT / "src" / sub).glob("*.h")))
    return files


def main(argv):
    files = [Path(a) for a in argv[1:]] or default_files()
    failures = []
    for f in files:
        failures.extend(check_file(f))
    for path, line, name, why in failures:
        try:
            rel = path.relative_to(ROOT)
        except ValueError:
            rel = path
        print("%s:%d: %s —— %s" % (rel, line, name, why))
    if failures:
        print("\n%d 处命名不符合 doc/命名与API风格.md" % len(failures))
        return 1
    print("命名检查通过（%d 个头文件）" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
