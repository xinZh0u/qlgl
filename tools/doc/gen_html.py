#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 doc/ 下的 Markdown 生成一套静态 HTML 文档站。

用法：
    python3 tools/doc/gen_html.py              # 生成到 doc/html/
    python3 tools/doc/gen_html.py --out /tmp/x  # 换输出目录

设计：
  * 只依赖 markdown + pygments（都可选：缺了也能跑，只是代码不高亮）；
  * 布局参照 GTK 文档站：顶栏 + 左侧导航 + 正文 + 上一页/下一页；
  * 每个 Markdown 文件自动生成右侧目录（h2/h3）；
  * 不加 JS，纯静态，双击就能看，也能直接扔到任意静态站点上。

目录结构约定：
    doc/tutorial/*.md          教程（按文件名数字排序）
    doc/手册/<模块>/*.md        手册（阅读指南 → API 手册）
    doc/<名字>.md              设计文档（按 DESIGN_DOCS 的顺序，只收根目录）
"""
import argparse
import html
import os
import re
import sys

try:
    import markdown
except ImportError:
    print("需要 python3-markdown：pip install markdown", file=sys.stderr)
    raise

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# 模块的显示名与顺序
MODULES = [
    ("object", "QLObject", "对象系统：类、属性、信号"),
    ("text", "QLText", "文本：字体、字形、UTF-8"),
    ("ui", "QLUI", "界面：HTML/CSS → 静态数据"),
    ("uitext", "QLUIText", "运行期 HTML/CSS 解析"),
    ("render", "QLRender", "渲染：画布、GPU 后端、脏区"),
    ("anim", "QLAnim", "动效：帧时钟、缓动、补间"),
    ("input", "QLInput", "输入：事件队列、指针、键盘"),
]

# 根目录的设计文档：按这里的顺序出现在侧栏（不在表里的根目录 .md 不收）
DESIGN_DOCS = [
    ("图像管线设计", "图像管线：解码、绘制、<img>"),
    ("EMBED后端设计", "EMBED：占位与后端路线（未排期）"),
    ("底层实现设计", "底层实现：对象、绘制、字体"),
    ("Web兼容设计", "与 Web 的兼容边界"),
    ("对比与取舍", "与主流框架对比：站在哪一格"),
    ("开发规划", "开发规划与分期"),
    ("bench", "性能基准"),
    ("性能诊断", "性能诊断：qlperf、帧时间拆分、掉帧案例"),
]

CSS = r"""
:root {
  --bg: #ffffff;
  --bg-alt: #f6f8fa;
  --fg: #1f2328;
  --fg-dim: #59636e;
  --line: #d1d9e0;
  --accent: #0969da;
  --code-bg: #f6f8fa;
  --side-w: 260px;
}
* { box-sizing: border-box; }
body {
  margin: 0;
  font: 15px/1.65 -apple-system, "Segoe UI", "Noto Sans CJK SC", "Source Han Sans SC",
        "Microsoft YaHei", sans-serif;
  color: var(--fg);
  background: var(--bg);
}
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }

/* 顶栏 */
header.top {
  position: sticky; top: 0; z-index: 20;
  display: flex; align-items: center; gap: 18px;
  height: 52px; padding: 0 20px;
  background: #24292f; color: #fff;
  border-bottom: 1px solid #1b1f23;
}
header.top .brand { font-weight: 600; font-size: 16px; letter-spacing: .3px; }
header.top .brand small { font-weight: 400; opacity: .65; margin-left: 8px; font-size: 12px; }
header.top nav { margin-left: auto; display: flex; gap: 16px; font-size: 13px; }
header.top nav a { color: #d0d7de; }
header.top nav a:hover { color: #fff; }

/* 两栏 */
.layout { display: flex; align-items: flex-start; }
aside.side {
  position: sticky; top: 52px;
  flex: 0 0 var(--side-w); width: var(--side-w);
  max-height: calc(100vh - 52px); overflow-y: auto;
  padding: 18px 12px 40px;
  background: var(--bg-alt);
  border-right: 1px solid var(--line);
  font-size: 13.5px;
}
aside.side h4 {
  margin: 16px 8px 6px; font-size: 11px; letter-spacing: .08em;
  text-transform: uppercase; color: var(--fg-dim);
}
aside.side h4:first-child { margin-top: 0; }
aside.side a {
  display: block; padding: 4px 8px; border-radius: 5px;
  color: var(--fg); line-height: 1.4;
}
aside.side a:hover { background: #eaeef2; text-decoration: none; }
aside.side a.active { background: #ddf4ff; color: #0a3069; font-weight: 600; }
aside.side a.sub-item {
  padding-left: 18px; font-size: 12.6px; color: var(--fg-dim);
}
aside.side a.sub-item.active { background: #ddf4ff; color: #0a3069; font-weight: 600; }

main {
  flex: 1 1 auto; min-width: 0;
  padding: 28px 40px 80px;
  max-width: 900px;
}

/* 正文 */
main h1 { font-size: 30px; margin: 0 0 6px; letter-spacing: -.3px; }
main h1 + .subtitle { color: var(--fg-dim); margin: 0 0 22px; font-size: 14px; }
main h2 {
  font-size: 21px; margin: 38px 0 10px;
  padding-bottom: 6px; border-bottom: 1px solid var(--line);
}
main h3 { font-size: 16.5px; margin: 26px 0 8px; }
main h4 { font-size: 14.5px; margin: 20px 0 6px; color: var(--fg-dim); }
main p { margin: 10px 0; }
main ul, main ol { margin: 10px 0; padding-left: 26px; }
main li { margin: 4px 0; }
main hr { border: 0; border-top: 1px solid var(--line); margin: 30px 0; }
main blockquote {
  margin: 14px 0; padding: 8px 14px;
  border-left: 3px solid var(--accent);
  background: #f6f8fa; color: var(--fg-dim);
}
main blockquote p { margin: 4px 0; }

/* 行内代码与代码块 */
code {
  font: 12.7px/1.5 "SF Mono", Menlo, Consolas, "Liberation Mono", monospace;
  background: var(--code-bg); padding: .16em .38em; border-radius: 4px;
  border: 1px solid var(--line);
}
pre {
  margin: 14px 0; padding: 13px 15px; overflow-x: auto;
  background: var(--code-bg); border: 1px solid var(--line); border-radius: 7px;
}
pre code { background: none; border: 0; padding: 0; font-size: 12.7px; }

/* 表格 */
table { border-collapse: collapse; margin: 14px 0; width: 100%; font-size: 13.6px; }
th, td { border: 1px solid var(--line); padding: 6px 10px; text-align: left; }
th { background: var(--bg-alt); font-weight: 600; }
tbody tr:nth-child(even) { background: #fafbfc; }

/* 代码高亮（pygments） */
.highlight .k, .highlight .kd, .highlight .kn { color: #cf222e; }
.highlight .s, .highlight .s1, .highlight .s2 { color: #0a3069; }
.highlight .c, .highlight .c1, .highlight .cm { color: #6e7781; font-style: italic; }
.highlight .mi, .highlight .mf { color: #0550ae; }
.highlight .nf, .highlight .n { color: #8250df; }
.highlight .cp, .highlight .cpf { color: #6e7781; }

main img {
  max-width: 100%; height: auto; display: block; margin: 16px 0;
  border: 1px solid var(--line); border-radius: 8px;
}
main img.shot { max-width: 100%; }

/* 页面底部翻页 */
.pager {
  display: flex; gap: 14px; margin-top: 46px; padding-top: 18px;
  border-top: 1px solid var(--line);
}
.pager a {
  flex: 1 1 0; padding: 10px 14px; border: 1px solid var(--line);
  border-radius: 7px; color: var(--fg);
}
.pager a:hover { background: var(--bg-alt); text-decoration: none; }
.pager .dir { display: block; font-size: 11.5px; color: var(--fg-dim); }
.pager .next { text-align: right; }

/* 首页卡片 */
.cards { display: flex; flex-wrap: wrap; gap: 14px; margin: 22px 0; }
.cards a {
  flex: 1 1 220px; padding: 15px 17px; border: 1px solid var(--line);
  border-radius: 9px; color: var(--fg);
}
.cards a:hover { background: var(--bg-alt); text-decoration: none; border-color: var(--accent); }
.cards b { display: block; font-size: 15px; margin-bottom: 4px; }
.cards span { font-size: 12.8px; color: var(--fg-dim); }

@media (max-width: 860px) {
  aside.side { display: none; }
  main { padding: 20px 18px 60px; }
}
"""


def md_to_html(text):
    return markdown.markdown(
        text,
        extensions=["extra", "toc", "sane_lists", "codehilite"],
        extension_configs={
            "codehilite": {"guess_lang": False, "noclasses": False,
                           "css_class": "highlight"},
            "toc": {"permalink": False},
        },
    )


def add_anchors(body):
    """给 h2/h3 生成 id（中文标题也能锚）+ 收集目录。"""
    toc = []
    counter = [0]

    def repl(m):
        level, attrs, inner = m.group(1), m.group(2), m.group(3)
        if "id=" in attrs:
            ident = re.search(r'id="([^"]+)"', attrs).group(1)
        else:
            counter[0] += 1
            ident = "s%d" % counter[0]
        plain = re.sub(r"<[^>]+>", "", inner)
        if level == "2" or level == "3":
            toc.append((int(level), ident, plain))
        return '<h%s id="%s">%s</h%s>' % (level, ident, inner, level)

    body = re.sub(r"<h([23])([^>]*)>(.*?)</h\1>", repl, body, flags=re.S)
    return body, toc


def first_heading(text):
    m = re.search(r"^#\s+(.+)$", text, re.M)
    return m.group(1).strip() if m else None


def first_para(text):
    """第一段非标题正文，用作副标题。"""
    lines = text.splitlines()
    skip = True
    buf = []
    for ln in lines:
        if ln.startswith("#"):
            if buf:
                break
            continue
        if not ln.strip():
            if buf:
                break
            continue
        if ln.strip() in ("---",):
            continue
        buf.append(ln.strip())
    s = " ".join(buf)
    s = re.sub(r"[`*_>]", "", s)
    return s[:110]


class Page:
    def __init__(self, src, out, title, subtitle, section, subtitle_text=""):
        self.src = src
        self.out = out
        self.title = title
        self.subtitle = subtitle
        self.section = section
        self.subtitle_text = subtitle_text
        self.toc = []


def collect():
    pages = []
    for name, sub in DESIGN_DOCS:
        path = os.path.join(ROOT, "doc", name + ".md")
        if not os.path.isfile(path):
            continue
        text = open(path, encoding="utf-8").read()
        pages.append(Page(path, "design/" + name + ".html",
                          first_heading(text) or name, "设计文档",
                          ("design", "设计文档"), sub))
    tut_dir = os.path.join(ROOT, "doc", "tutorial")
    if os.path.isdir(tut_dir):
        for name in sorted(os.listdir(tut_dir)):
            if not name.endswith(".md"):
                continue
            path = os.path.join(tut_dir, name)
            text = open(path, encoding="utf-8").read()
            pages.append(Page(path, "tutorial/" + name[:-3] + ".html",
                              first_heading(text) or name, "教程",
                              ("tutorial", "入门教程"),
                              first_para(text)))
    for key, disp, _desc in MODULES:
        d = os.path.join(ROOT, "doc", "手册", key)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith(".md"):
                continue
            path = os.path.join(d, name)
            text = open(path, encoding="utf-8").read()
            pages.append(Page(path, "manual/%s/%s.html" % (key, name[:-3]),
                              first_heading(text) or name, "手册",
                              ("manual", disp)))
    return pages


def build(pages, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    title_of = {p.out: p.title for p in pages}

    # 侧栏分组
    groups = []          # [(label, [(out, title)])]
    tut = [(p.out, p.title) for p in pages if p.section[0] == "tutorial"]
    if tut:
        groups.append(("入门教程", tut))
    design = [(p.out, p.title) for p in pages if p.section[0] == "design"]
    if design:
        groups.append(("设计文档", design))
    by_mod = {}
    for p in pages:
        if p.section[0] == "manual":
            by_mod.setdefault(p.section[1], []).append((p.out, p.title))
    for key, disp, _d in MODULES:
        if disp in by_mod:
            groups.append((disp, by_mod[disp]))

    def short_label(title):
        """'QLUI API 手册（v0.1）' → 'API 手册'；'QLUI 阅读指南' → '阅读指南'"""
        if "API" in title:
            return "API 手册"
        if "阅读指南" in title:
            return "阅读指南"
        return title

    def sidebar(active, prefix):
        out = ["<h4>导航</h4>"]
        out.append("<a href='%sindex.html'%s>首页</a>"
                   % (prefix, " class='active'" if active == "index.html" else ""))
        for label, items in groups:
            if label == "入门教程":
                out.append("<h4>入门教程</h4>")
                for href, title in items:
                    cls = " class='active'" if href == active else ""
                    out.append("<a href='%s%s'%s>%s</a>"
                               % (prefix, href, cls, html.escape(title)))
                continue
            out.append("<h4>%s</h4>" % html.escape(label))
            # 指南在前、API 在后
            ordered = sorted(items, key=lambda it: (0 if "阅读指南" in it[1] else 1))
            for href, title in ordered:
                cls = " class='active'" if href == active else ""
                out.append("<a class='sub-item%s' href='%s%s'>%s</a>"
                           % (" active" if href == active else "", prefix, href,
                              html.escape(short_label(title))))
        return "\n".join(out)

    def shell(active, prefix, page_title, body, pager=""):
        return (
            "<!DOCTYPE html>\n<html lang='zh-CN'>\n<head>\n"
            "<meta charset='utf-8'>\n"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
            "<title>%s · QLGL</title>\n<style>%s</style>\n</head>\n<body>\n"
            "<header class='top'>\n"
            "  <span class='brand'>QLGL<small>C11 界面库 · 文档</small></span>\n"
            "  <nav><a href='%sindex.html'>首页</a>"
            "<a href='%stutorial/01_basics.html'>教程</a>"
            "<a href='%smanual/ui/QLUI API手册.html'>CSS 参考</a></nav>\n"
            "</header>\n<div class='layout'>\n<aside class='side'>\n%s\n</aside>\n"
            "<main>\n%s\n%s\n</main>\n</div>\n</body>\n</html>\n"
            % (html.escape(page_title), CSS, prefix, prefix, prefix,
               sidebar(active, prefix), body, pager)
        )

    # 每个页面：正文 + 翻页
    order = [p.out for p in pages]
    for p in pages:
        text = open(p.src, encoding="utf-8").read()
        body = md_to_html(text)
        body, p.toc = add_anchors(body)

        depth = p.out.count("/")
        prefix = "../" * (depth + 0)
        # 从 doc/html/<x>.html 出发：tutorial/x.html → ../，manual/m/x.html → ../../
        prefix = "../" * (0 if depth == 0 else depth)

        pager = ["<div class='pager'>"]
        i = order.index(p.out)
        if i > 0:
            prev = order[i - 1]
            pager.append("<a href='%s%s'><span class='dir'>上一页</span>%s</a>"
                         % (prefix, prev, html.escape(title_of[prev])))
        if i + 1 < len(order):
            nxt = order[i + 1]
            pager.append("<a class='next' href='%s%s'><span class='dir'>下一页</span>"
                         "%s</a>" % (prefix, nxt, html.escape(title_of[nxt])))
        pager.append("</div>")

        out = shell(p.out, prefix, p.title, body, "\n".join(pager))
        dst = os.path.join(out_dir, p.out)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w", encoding="utf-8") as f:
            f.write(out)

    # 首页
    tut_cards = "".join(
        "<a href='%s'><b>%s</b><span>%s</span></a>"
        % (p.out, html.escape(p.title), html.escape(p.subtitle_text))
        for p in pages if p.section[0] == "tutorial")
    guide = {}
    for p in pages:
        if p.section[0] == "manual" and "阅读指南" in p.title:
            guide[p.section[1]] = p.out
    mod_cards = "".join(
        "<a href='%s'><b>%s</b><span>%s</span></a>" % (guide.get(disp, "#"), disp, desc)
        for key, disp, desc in MODULES)

    index_body = """
<h1>QLGL 文档</h1>
<p class='subtitle'>C11 写界面：HTML/CSS 描述结构，构建期或运行期编译成静态数据，CPU/GPU 双后端绘制。</p>

<h2>从教程开始</h2>
<p>第一次用？照教程顺序读，每章都有可以立刻编译运行的完整程序
（源码在 <code>example/</code>）。</p>
<div class='cards'>%s</div>

<h2>模块手册</h2>
<p>每个模块两份：<b>阅读指南</b>讲“为什么这么设计、坑在哪”，
<b>API 手册</b>讲“有哪些函数、参数什么含义”。</p>
<div class='cards'>%s</div>

<h2>参考</h2>
<ul>
  <li><a href='manual/ui/QLUI API手册.html'>支持的 CSS 属性与取值</a></li>
  <li><a href='manual/uitext/QLUIText API手册.html'>运行期解析：边界与上限</a></li>
  <li><a href='manual/render/QLRender阅读指南.html'>渲染管线与脏区重绘</a></li>
</ul>
""" % (tut_cards, mod_cards)

    with open(os.path.join(out_dir, "index.html"), "w", encoding="utf-8") as f:
        f.write(shell("index.html", "", "首页", index_body))
    return len(pages)


def copy_images(out_dir):
    """doc/img/* → out/img/*（教程里用 <img src="../img/xx.png">）"""
    import shutil
    src = os.path.join(ROOT, "doc", "img")
    if not os.path.isdir(src):
        return 0
    dst = os.path.join(out_dir, "img")
    os.makedirs(dst, exist_ok=True)
    n = 0
    for name in sorted(os.listdir(src)):
        p = os.path.join(src, name)
        if os.path.isfile(p):
            shutil.copy2(p, os.path.join(dst, name))
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "doc", "html"))
    args = ap.parse_args()
    pages = collect()
    n = build(pages, args.out)
    imgs = copy_images(args.out)
    print("生成 %d 页 + %d 张图 → %s" % (n + 1, imgs, args.out))


if __name__ == "__main__":
    main()
