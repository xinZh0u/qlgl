#!/usr/bin/env python3
"""生成器错误路径：这些写法必须**明确报错**，不能静默降级。

对应规划里的红线"明确失败优于半个正确"：`@keyframes` / `animation` 里本实现
做不到的组合，一律在构建期爆掉，而不是运行时什么都不发生。
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
QLUI = os.path.join(ROOT, "tools", "qlui", "qlui.py")

HTML = "<div id='a'>a</div>"

PULSE = "@keyframes pulse { from { opacity: 0 } to { opacity: 1 } }\n"

# (CSS, 期望的错误信息子串；None 表示这份 CSS 必须编译通过)
CASES = [
    (PULSE + "#a { animation: pulse 1s }", None),
    ("#a { animation: pulse 1s }", "没有对应的 @keyframes"),
    (PULSE + "#a { animation-name: pulse }", "没写 animation-duration"),
    (PULSE + "#a { animation: nope 1s }", "没有对应的 @keyframes"),
    (PULSE + "#a { animation: pulse 1s }", None),
    (PULSE + "#a { animation: pulse 1s 300ms }", None),          # 第二时长是 delay
    (PULSE + "#a { animation: pulse 1s 2s 3s }", "时长写了三个"),
    (PULSE + "#a { animation: pulse 1s; animation-delay: 250ms }", None),
    (PULSE + "#a { animation: pulse 0ms }", "不能为 0"),
    (PULSE + "#a { animation: pulse 1s paused }", "暂不支持 paused"),
    (PULSE + "#a { animation: pulse 1s steps(3) }", "暂不支持 steps()"),
    ("@keyframes pulse { 0% { box-shadow: 0 0 } 100% { box-shadow: 0 0 } }",
     "不支持的属性"),
    # 文本色 / 边框色现在可以过渡，也可以在关键帧里插值
    ("#a { transition: color 200ms; }", None),
    ("#a { transition: border-color 200ms; }", None),
    ("#a { transition: all 200ms; }", None),
    (PULSE + "#a { animation: pulse 1s }", None),
    ("@keyframes pulse { from { color: #111 } to { color: #eee } }"
     " #a { animation: pulse 1s }", None),
    ("@keyframes pulse { from { border-color: #111 } to { border-color: #eee } }"
     " #a { animation: pulse 1s }", None),
    ("@keyframes pulse { 0% { background-color: red } 50% { opacity: 0 } }",
     "属性不一致"),
    ("@keyframes pulse { 0% { opacity: 1 } 0% { opacity: 0 } }", "重复的百分比"),
    ("@keyframes pulse { 120% { opacity: 1 } }", "0%~100%"),
    ("@keyframes none { from { opacity: 1 } }", "关键字冲突"),
    # 乱序的关键帧是合法的：生成器会排序
    ("@keyframes pulse { 50% { opacity: 1 } 20% { opacity: 0 } }"
     "#a { animation: pulse 1s }", None),
    # 只有 50% 一帧也合法：首末帧取元素基础样式
    ("@keyframes pulse { 50% { opacity: 0 } } #a { animation: pulse 1s }", None),
    ("#a { transition: 100ms 50ms }", "暂不支持 delay"),
    # transform：只支持 translate / 轴对齐 scale（含分轴，假翻转用）
    ("#a { transform: scale(1.5) }", None),
    ("#a { transform: translate(4px, 6px) scale(2) }", None),
    ("#a { transform: scale(1.2, 2) }", None),
    ("#a { transform: scaleX(0) }", None),
    ("#a { transform: scaleY(0.5) }", None),
    ("#a { transform: scale(-1) }", "0~32.767"),
    ("#a { transform: scale(40) }", "0~32.767"),
    ("#a { transform: rotate(30deg) }", "只支持 translate()/scale()"),
    ("#a { transform: matrix(1,0,0,1,0,0) }", "只支持 translate()/scale()"),
    # 动画按基础样式起播，写在伪类里会静默失效 → 构建期报错
    (PULSE + "#a:hover { animation: pulse 1s }", "状态规则里不支持 animation"),
]


def run(css, out_dir):
    with tempfile.NamedTemporaryFile("w", suffix=".css", delete=False,
                                     encoding="utf-8") as f:
        f.write(css)
        css_path = f.name
    with tempfile.NamedTemporaryFile("w", suffix=".html", delete=False,
                                     encoding="utf-8") as f:
        f.write(HTML)
        html_path = f.name
    try:
        return subprocess.run(
            [sys.executable, QLUI, "--html", html_path, "--css", css_path,
             "--name", "t", "--out", out_dir],
            capture_output=True, text=True)
    finally:
        os.unlink(css_path)
        os.unlink(html_path)


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for css, expect in CASES:
            res = run(css, tmp)
            if expect is None:
                if res.returncode != 0:
                    print("[FAIL] 应该通过：%r\n%s" % (css, res.stderr.strip()))
                    failures += 1
                continue
            if res.returncode == 0:
                print("[FAIL] 应该报错但通过了：%r" % css)
                failures += 1
            elif expect not in res.stderr:
                print("[FAIL] %r 的错误信息里没有 %r：\n%s"
                      % (css, expect, res.stderr.strip()))
                failures += 1

    print("== test_qlui_errors ==\n  %d 个用例，%d 个失败" % (len(CASES), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
