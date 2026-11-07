#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 QLGL 的类型 id 表。

扫描 src/**/*.c 中的 QL_DEFINE_WITH_PRIVATE(NAME, Name, name, Inherit)，
按"父类先于子类"排序后生成两份文件：

    include/qlgl/qltype_gen.h     类型 id 枚举（公开契约）
    src/generated/qlclass_gen.c   内置类注册表

用法：
    python3 tools/qlgen/qlgen.py            # 重新生成
    python3 tools/qlgen/qlgen.py --check    # 只校验，不一致返回 1（放进 CI）
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC_DIR = ROOT / "src"
OUT_HEADER = ROOT / "include" / "qlgl" / "qltype_gen.h"
OUT_SOURCE = ROOT / "src" / "generated" / "qlclass_gen.c"

PATTERN = re.compile(
    r"QL_DEFINE_WITH_PRIVATE\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)"
)

ROOT_CLASS = {
    "NAME": "QL_OBJECT",
    "Name": "QLObject",
    "name": "ql_object",
    "inherit": None,
}


def enum_name(name_upper: str) -> str:
    """QL_WIDGET -> QL_TYPE_WIDGET"""
    return "QL_TYPE_" + (name_upper[3:] if name_upper.startswith("QL_") else name_upper)


def scan() -> dict:
    found = {}
    for path in sorted(SRC_DIR.rglob("*.c")):
        if "generated" in path.parts:
            continue
        text = path.read_text(encoding="utf-8")
        for match in PATTERN.finditer(text):
            name_upper, name_pascal, name_snake, inherit = match.groups()
            if name_snake in found:
                sys.exit("重复定义：%s（%s）" % (name_snake, path))
            found[name_snake] = {
                "NAME": name_upper,
                "Name": name_pascal,
                "name": name_snake,
                "inherit": inherit,
                "file": str(path.relative_to(ROOT)),
            }
    return found


def order(classes: dict) -> list:
    result = [dict(ROOT_CLASS)]
    done = {ROOT_CLASS["name"]}
    pending = dict(classes)
    while pending:
        progressed = False
        for key in list(pending):
            parent = pending[key]["inherit"]
            if parent in done:
                result.append(pending.pop(key))
                done.add(key)
                progressed = True
        if not progressed:
            sys.exit("继承关系无法排序：%s" % ", ".join(sorted(pending)))
    return result


def render_header(items: list) -> str:
    lines = [
        "/* 本文件由 tools/qlgen/qlgen.py 生成，请勿手工修改。 */",
        "#ifndef QLGL_TYPE_GEN_H",
        "#define QLGL_TYPE_GEN_H",
        "",
        "/* 类型 id：新类只能追加到末尾，禁止插入或重排。 */",
        "enum {",
    ]
    for index, item in enumerate(items):
        lines.append("    %-28s = %d," % (enum_name(item["NAME"]), index))
    lines += [
        "    QL_TYPE_COUNT,              /* 内置类数量，动态注册从它开始 */",
        "};",
        "",
        "#endif /* QLGL_TYPE_GEN_H */",
        "",
    ]
    return "\n".join(lines)


def render_source(items: list) -> str:
    lines = [
        "/* 本文件由 tools/qlgen/qlgen.py 生成，请勿手工修改。 */",
        "#include <qlgl/qlgl.h>",
        "",
    ]
    for item in items:
        lines.append("extern %sClass %s_class;" % (item["Name"], item["name"]))
    lines += ["", "const QLBuiltinClass ql_builtin_class_table[] = {"]
    for item in items:
        parent = "QL_TYPE_NONE"
        if item["inherit"] is not None:
            parent = enum_name(parent_of(items, item["inherit"]))
        lines.append(
            '    { %s, %s, "%s", (QLObjectClass *)(void *)&%s_class, %s_class_setup },'
            % (enum_name(item["NAME"]), parent, item["Name"], item["name"], item["name"])
        )
    lines += [
        "};",
        "",
        "const uint16_t ql_builtin_class_count =",
        "    (uint16_t)QL_ARRAY_SIZE(ql_builtin_class_table);",
        "",
    ]
    return "\n".join(lines)


def parent_of(items: list, snake: str) -> str:
    for item in items:
        if item["name"] == snake:
            return item["NAME"]
    sys.exit("找不到父类：%s" % snake)


def write(path: pathlib.Path, text: str, check: bool) -> bool:
    old = path.read_text(encoding="utf-8") if path.exists() else None
    if old == text:
        return True
    if check:
        print("与源码不同步：%s" % path.relative_to(ROOT), file=sys.stderr)
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    print("已更新：%s" % path.relative_to(ROOT))
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 QLGL 类型 id 表")
    parser.add_argument("--check", action="store_true", help="只校验不写入")
    args = parser.parse_args()

    items = order(scan())
    ok = write(OUT_HEADER, render_header(items), args.check)
    ok = write(OUT_SOURCE, render_source(items), args.check) and ok
    if args.check and not ok:
        return 1
    print("共 %d 个内置类。" % len(items))
    return 0


if __name__ == "__main__":
    sys.exit(main())
