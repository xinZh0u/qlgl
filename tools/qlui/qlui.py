#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 HTML + CSS 编译成 C 文件（相当于 Qt 的 uic）—— **兼容壳，不是实现**。

M3.5 第 4 步之后，HTML/CSS 的语义只剩一份：`src/ui/qlui_parse.c`。
这个脚本现在只做三件事：

  1. 找到宿主工具 `qltext`（环境变量 `QLTEXT` 或常见构建目录）；
  2. 原样转发命令行（`--html/--css/--name/--out` → `qltext cc`）；
  3. 转发它的 stdout/stderr 与退出码。

也就是说，`qlui.py` 与 `qltext`、与运行期 `ql_ui_load_text()` 产出的 doc
**是同一段代码算出来的**，不存在两份解析器互相漂移的可能。

用法（与以前完全一样）：
    python3 tools/qlui/qlui.py --html ui/index.html --css ui/index.css --out build/

等价的直接写法（推荐，少一层 Python）：
    qltext cc --html ui/index.html --css ui/index.css --out build/
"""

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def find_qltext():
    env = os.environ.get("QLTEXT")
    if env:
        return env
    found = shutil.which("qltext")
    if found:
        return found
    for sub in ("build", "build-rel", "build-san", "build-debug", "build/Release"):
        cand = os.path.join(ROOT, sub, "qltext")
        if os.path.isfile(cand) and os.access(cand, os.X_OK):
            return cand
    return None


def main(argv):
    tool = find_qltext()
    if tool is None:
        sys.stderr.write(
            "qlui.py: 找不到宿主工具 qltext。\n"
            "  先构建它：cmake --build build --target qltext\n"
            "  或者用 QLTEXT=/path/to/qltext 指定。\n")
        return 127
    sys.stderr.write("qlui.py: 已降级为壳，实际由 %s 完成（HTML/CSS 语义只有一份）\n"
                     % tool)
    return subprocess.call([tool, "cc"] + argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
