#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 doc/ 下的 Markdown 生成一套静态 HTML 文档站。

用法：
    python3 tools/doc/gen_html.py              # 生成到 doc/html/
    python3 tools/doc/gen_html.py --out /tmp/x  # 换输出目录

设计：
  * 只依赖 markdown + pygments（都可选：缺了也能跑，只是代码不高亮）；
  * 视觉参照 https://docs.gtk.org/gtk4/ ：深色侧栏（#151515）+ 侧栏顶部胶囊搜索框
    + 白底正文（带投影）+ 右侧页内目录；
  * 侧栏按**用途**分组（入门 / 界面 / 布局 / 视觉 / 语法 / 性能 / API / 规范 / 设计），
    组可折叠（原生 <details>，不需要 JS）；
  * 搜索是纯前端：额外生成 `search-index.js`（页面标题 + 每个 h2/h3 + 首段），
    双击 file:// 也能用；
  * 每个 Markdown 文件自动生成右侧目录（h2/h3）。

目录结构约定：
    doc/tutorial/*.md            教程（按 TUTORIAL_SECTIONS 分组）
    doc/手册/<模块>/*.md          手册（阅读指南 → API 手册）
    doc/手册/ui/{CSS3,HTML5}*.md  规范与清单（单独成组，不混进 QLUI API）
    doc/<名字>.md                设计文档（按 DESIGN_DOCS 的顺序，只收根目录）

**不移动任何源文件**：输出路径与旧版完全一致，历史链接不会断。
"""
import argparse
import html
import os
import re
import urllib.parse
import sys

try:
    import markdown
except ImportError:
    print("需要 python3-markdown：pip install markdown", file=sys.stderr)
    raise

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# ---------------------------------------------------------------- 分组定义

# 模块的显示名与顺序（API 手册组）
MODULES = [
    ("object", "QLObject", "对象系统：类、属性、信号"),
    ("text", "QLText", "文本：字体、字形、UTF-8"),
    ("ui", "QLUI", "界面：HTML/CSS → 静态数据"),
    ("uitext", "QLUIText", "运行期 HTML/CSS 解析"),
    ("render", "QLRender", "渲染：画布、GPU 后端、脏区"),
    ("anim", "QLAnim", "动效：帧时钟、缓动、补间"),
    ("input", "QLInput", "输入：事件队列、指针、键盘"),
]

# 教程分组：(侧栏标题, 一句话说明, [文件名后缀])
# 每个 doc/tutorial/*.md 必须**恰好**出现在一组里（漏了会在生成时报出来）。
TUTORIAL_SECTIONS = [
    ("入门", "从零写第一个界面：结构、文本、布局、交互、动画、换肤", [
        "01_basics", "02_text", "03_layout", "04_interaction",
        "05_animation", "06_theme", "07_advanced",
    ]),
    ("界面与控件", "标签语义、滚动、输入控件、表单、触摸", [
        "08_tags", "09_scroll", "11_input", "13_form", "26_touch",
    ]),
    ("布局与文本", "空白与上下标、Grid、表格、多栏、文本栈", [
        "12_text", "19_grid", "22_table", "23_columns", "24_text_stack",
    ]),
    ("视觉与动效", "毛玻璃、滤镜图层、变换、裁剪、效果检阅", [
        "15_backdrop", "20_filter", "21_transform", "25_clip_filter",
        "16_css_gallery", "27_liquid_glass",
    ]),
    ("语法与兼容", "真实页面写法补全、与 Web 的差别", [
        "10_vs_web", "14_css3", "18_css3_more",
    ]),
    ("性能", "帧时间拆分、掉帧案例、MCU 基准", [
        "17_perf",
    ]),
]

# 规范与清单：在 doc/手册/ui/ 下，但不属于 QLUI API
SPEC_STEMS = [
    ("CSS3覆盖与裁剪", "CSS：逐条能力状态（全量清单）"),
    ("CSS3进度规划", "CSS：已完成 / 待做与批次排期"),
    ("CSS3开发日志", "CSS：每一批落地时怎么做的"),
    ("HTML5标签覆盖与裁剪", "HTML5：114 个标签逐条状态"),
    ("HTML5标签进度规划", "HTML5：分期与工作量"),
    ("HTML5标签开发日志", "HTML5：占位元素的落地记录"),
]

# 根目录的设计文档：按这里的顺序出现在侧栏（不在表里的根目录 .md 不收）
DESIGN_DOCS = [
    ("图像管线设计", "图像管线：解码、绘制、<img>"),
    ("EMBED后端设计", "EMBED：占位与后端路线（未排期）"),
    ("底层实现设计", "底层实现：对象、绘制、字体"),
    ("第三方依赖", "第三方依赖：用了什么、为什么、什么许可"),
    ("迁移指南", "从 Web 迁到 QLGL：写法对照与坑"),
    ("Web兼容设计", "与 Web 的兼容边界"),
    ("对比与取舍", "与主流框架对比：站在哪一格"),
    ("开发规划", "开发规划与分期"),
    ("bench", "性能基准"),
    ("性能诊断", "性能诊断：qlperf、帧时间拆分、掉帧案例"),
]

# ---------------------------------------------------------------- 样式

CSS = r"""
:root {
  --text: #333;
  --text-muted: rgba(51, 51, 51, .65);
  --primary: #1c76e4;
  --bg: #fff;
  --side-bg: #151515;
  --side-fg: #fafafa;
  --side-primary: rgb(144, 194, 255);
  --side-hover: rgba(127, 127, 127, .2);
  --side-sel: #1c76e4;
  --side-search: rgba(127, 127, 127, .25);
  --side-search-focus: rgba(127, 127, 127, .38);
  --box-bg: rgba(135, 135, 135, .085);
  --box-radius: .35rem;
  --line: rgba(51, 51, 51, .12);
  --side-w: 17.5rem;
  --toc-w: 15.5rem;
  --font: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Noto Sans CJK SC",
          "Source Han Sans SC", "Microsoft YaHei", sans-serif;
  --mono: "Source Code Pro", "SF Mono", Menlo, Consolas, "Liberation Mono", monospace;
}
* { box-sizing: border-box; }
html { scroll-behavior: smooth; }
body {
  margin: 0;
  font: 15px/1.6 var(--font);
  color: var(--text);
  background: var(--bg);
  -webkit-font-smoothing: antialiased;
}
a { color: var(--primary); text-decoration: none; }
a:hover { text-decoration: underline; }

.shell { display: flex; align-items: flex-start; }

/* ---------------- 左侧深色导航（GTK 风格） ---------------- */
.sidebar {
  position: sticky; top: 0;
  flex: 0 0 var(--side-w); width: var(--side-w);
  height: 100vh; overflow-y: auto;
  padding: 1.2rem 1rem 3rem;
  background: var(--side-bg);
  color: var(--side-fg);
  color-scheme: dark;
  font-size: 13.5px;
  scrollbar-width: thin;
}
.sidebar::-webkit-scrollbar { width: 9px; }
.sidebar::-webkit-scrollbar-thumb { background: #3a3a3a; border-radius: 9px; }
.sidebar .brand { display: block; padding: .2rem .5rem 1rem; }
.sidebar .brand b {
  display: block; font-size: 1.45rem; letter-spacing: .02em; color: #fff;
  font-weight: 900;
}
.sidebar .brand span { font-size: .8rem; opacity: .7; }
.sidebar a { text-decoration: none; }

.search { position: relative; margin: 0 0 1rem; }
.search input {
  width: 100%; border: 0; border-radius: 50px;
  padding: 8px 14px; font-size: 13.5px; outline: 0;
  background: var(--side-search); color: var(--side-fg);
  transition: background-color .15s ease;
}
.search input::placeholder { color: rgba(250, 250, 250, .55); }
.search input:hover, .search input:focus { background: var(--side-search-focus); }
.results {
  position: absolute; left: 0; right: 0; top: calc(100% + 6px);
  max-height: 60vh; overflow-y: auto; z-index: 50;
  background: #232323; border: 1px solid #3a3a3a; border-radius: 10px;
  box-shadow: 0 16px 40px rgba(0, 0, 0, .5); padding: 6px;
}
.results .hit {
  display: block; padding: 7px 10px; border-radius: 7px; cursor: pointer;
  color: var(--side-fg);
}
.results .hit:hover, .results .hit.sel { background: var(--side-sel); }
.results .hit .t { display: block; font-size: 13.5px; }
.results .hit .c { display: block; font-size: 11.5px; opacity: .62; }
.results .hit.sel .c { opacity: .85; }
.results .empty { padding: 10px; font-size: 12.5px; opacity: .6; }

.sec { border-top: 1px solid rgba(255, 255, 255, .07); padding: .5rem 0 .2rem; }
.sec > summary {
  list-style: none; cursor: pointer; user-select: none;
  display: flex; align-items: center; gap: 6px;
  padding: 4px 6px; font-weight: 700; font-size: 12px;
  letter-spacing: .04em; text-transform: uppercase;
  color: rgba(250, 250, 250, .55);
}
.sec > summary::-webkit-details-marker { display: none; }
.sec > summary::before {
  content: "▸"; font-size: 10px; transition: transform .15s ease; opacity: .8;
}
.sec[open] > summary::before { transform: rotate(90deg); }
.sec > summary:hover { color: rgba(250, 250, 250, .85); }
.sec a {
  display: block; padding: .25rem .55rem; margin-bottom: .1rem;
  border-radius: var(--box-radius); color: var(--side-primary);
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
  transition: background-color .15s ease, color .15s ease;
}
.sec a:hover { background: var(--side-hover); color: var(--side-fg); }
.sec a.current { background: var(--side-sel); color: #fff; font-weight: 600; }
.sec .mod {
  display: block; padding: .55rem .55rem .15rem;
  font-size: 11px; letter-spacing: .05em; text-transform: uppercase;
  color: rgba(250, 250, 250, .4);
}
.sec .mod:first-child { padding-top: .2rem; }
.sidebar .gen {
  margin: 1.4rem .5rem 0; font-size: 11px; opacity: .45; line-height: 1.5;
}

/* ---------------- 正文 ---------------- */
main {
  flex: 1 1 auto; min-width: 0;
  padding: 3rem 3rem 6rem;
  box-shadow: 0 0 134px rgba(0, 0, 0, .07);
}
main .inner { max-width: 54rem; margin: 0 auto; }
main h1, main h2, main h3, main h4, main h5 {
  font-weight: 800; letter-spacing: -.01em; line-height: 1.25;
}
main h1 { font-size: 2.05rem; margin: 0 0 .3rem; }
main .eyebrow {
  font-size: 12.5px; letter-spacing: .05em; text-transform: uppercase;
  color: var(--text-muted); margin: 0 0 .15rem;
}
main .lead { color: var(--text-muted); font-size: 1rem; margin: .6rem 0 2rem; }
main h2 {
  font-size: 1.45rem; margin: 2.4rem 0 .7rem;
  padding-bottom: .35rem; border-bottom: 1px solid var(--line);
}
main h3 { font-size: 1.18rem; margin: 1.8rem 0 .5rem; }
main h4 { font-size: 1.02rem; margin: 1.4rem 0 .4rem; color: var(--text-muted); }
main p { margin: .7rem 0; }
main ul, main ol { margin: .7rem 0; padding-left: 1.5rem; }
main li { margin: .25rem 0; }
main hr { border: 0; border-top: 1px solid var(--line); margin: 2.2rem 0; }
main blockquote {
  margin: 1rem 0; padding: .5rem 1rem;
  border-left: 3px solid var(--primary);
  background: var(--box-bg); border-radius: 0 var(--box-radius) var(--box-radius) 0;
  color: var(--text-muted);
}
main blockquote p { margin: .3rem 0; }

code {
  font: .86em/1.5 var(--mono);
  background: var(--box-bg); padding: .12em .35em;
  border-radius: 4px;
}
pre {
  margin: 1rem 0; padding: .8rem 1rem; overflow-x: auto;
  background: var(--box-bg); border-radius: var(--box-radius);
}
pre code { background: none; padding: 0; font-size: .86rem; }

table { border-collapse: collapse; margin: 1rem 0; width: 100%; font-size: .9rem; }
th, td { border: 1px solid var(--line); padding: .4rem .65rem; text-align: left; }
th { background: var(--box-bg); font-weight: 650; }
tbody tr:nth-child(even) { background: rgba(135, 135, 135, .04); }

/* 代码高亮（pygments） */
.highlight .k, .highlight .kd, .highlight .kn { color: #a626a4; }
.highlight .s, .highlight .s1, .highlight .s2 { color: #4078f2; }
.highlight .c, .highlight .c1, .highlight .cm { color: #a0a1a7; font-style: italic; }
.highlight .mi, .highlight .mf { color: #986801; }
.highlight .nf, .highlight .n { color: #383a42; }
.highlight .cp, .highlight .cpf { color: #a0a1a7; }

main img {
  max-width: 100%; height: auto; display: block; margin: 1.2rem 0;
  border: 1px solid var(--line); border-radius: var(--box-radius);
}

/* ---------------- 右侧页内目录 ---------------- */
.toc {
  position: sticky; top: 0;
  flex: 0 0 var(--toc-w); width: var(--toc-w);
  max-height: 100vh; overflow-y: auto;
  padding: 3rem .8rem 3rem .4rem;
  font-size: 12.5px;
}
.toc h5 {
  margin: 0 0 .5rem; font-size: 10.5px; letter-spacing: .09em;
  text-transform: uppercase; color: var(--text-muted);
}
.toc ul { list-style: none; margin: 0; padding: 0; }
.toc li { margin: .15rem 0; }
.toc a {
  display: block; padding: .16rem .4rem; border-radius: 5px;
  color: var(--text-muted); line-height: 1.4;
}
.toc a:hover { background: var(--box-bg); color: var(--primary); text-decoration: none; }
.toc .lv3 { padding-left: 1rem; font-size: 12px; }

/* 翻页 */
.pager {
  display: flex; gap: .8rem; margin-top: 3rem; padding-top: 1.2rem;
  border-top: 1px solid var(--line);
}
.pager a {
  flex: 1 1 0; padding: .6rem .9rem; border: 1px solid var(--line);
  border-radius: var(--box-radius); color: var(--text);
}
.pager a:hover { background: var(--box-bg); text-decoration: none; }
.pager .dir { display: block; font-size: 11px; color: var(--text-muted); }
.pager .next { text-align: right; }

/* 首页卡片 */
.cards { display: grid; grid-template-columns: repeat(auto-fill, minmax(15rem, 1fr));
         gap: .7rem; margin: 1rem 0 2rem; }
.cards a {
  display: block; padding: .8rem 1rem; color: var(--text);
  background: var(--box-bg); border-radius: var(--box-radius);
  border: 1px solid transparent;
}
.cards a:hover { border-color: var(--primary); text-decoration: none; }
.cards b { display: block; font-size: .95rem; margin-bottom: .15rem; }
.cards span { font-size: .82rem; color: var(--text-muted); }
.sec-title { font-size: 1.2rem; font-weight: 800; margin: 2rem 0 .1rem; }
.sec-desc { color: var(--text-muted); font-size: .88rem; margin: 0 0 .6rem; }

@media (max-width: 1180px) {
  .toc { display: none; }
}
@media (max-width: 820px) {
  .shell { display: block; }
  .sidebar { position: static; height: auto; width: auto; flex: none; }
  main { padding: 1.6rem 1.1rem 4rem; box-shadow: none; }
}
"""

# ---------------------------------------------------------------- Markdown

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


def rewrite_page_links(body, page, out_map):
    """把指向别的文档的链接改写成站点里能点开的相对路径。

    源码里常见的两种写法在生成的站点上都点不开：
      - 带 `.md`（源目录里能点，站点里没有 .md）
      - 相对 `doc/` 根写（如 `手册/ui/QLUI阅读指南.md`），而站点里手册在 `manual/` 下
    这里按"源文件相对"和"doc/ 根相对"两种解释去查页面表，查到就换成相对当前页的路径。
    """
    src_dir = os.path.dirname(page.src)
    out_dir = os.path.dirname(page.out)

    def fix(m):
        attr, href = m.group(1), m.group(2)
        if re.match(r"^(https?:|mailto:|data:|#|/)", href):
            return m.group(0)
        raw, sep, frag = href.partition("#")
        if not raw:
            return m.group(0)
        stem = os.path.splitext(urllib.parse.unquote(raw))[0]
        for base in (src_dir, os.path.join(ROOT, "doc")):
            key = os.path.normpath(os.path.join(base, stem))
            if key in out_map:
                rel = os.path.relpath(out_map[key], out_dir or ".")
                return '%s"%s%s%s"' % (attr, rel, sep, frag)
        return m.group(0)

    return re.sub(r'(href=)"([^"]*)"', fix, body)


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
    """第一段非标题正文，用作副标题 / 搜索摘要。"""
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


def find_lead(text):
    """挑一段有信息量的正文当导语：跳过 h1、图片、代码块、表格、样板行。

    返回 (导语文本, 原文段落)；调用方要把这个段落在 Markdown 里删掉，
    否则页面上会出现两遍。
    """
    blocks = re.split(r"\n\s*\n", text)
    for b in blocks:
        b = b.strip()
        if not b or b.startswith(("#", "```", "|", "---", ">", "![", "<")):
            continue
        if "](img/" in b or b.startswith("完整程序") or "example/" in b:
            continue
        flat = " ".join(ln.strip() for ln in b.splitlines())
        flat = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", flat)
        flat = re.sub(r"[`*_]", "", flat).strip()
        if len(flat) >= 12:
            return flat[:150], b
    return "", ""


def blurb_of(text, limit=95):
    """首页卡片的一句话说明：跳过图片/样板行，取第一段有信息量的正文。"""
    lead, _block = find_lead(text)
    if not lead:
        lead = first_para(text)
    return lead[:limit]


def plain(text):
    """显示用标题：去掉 Markdown 的行内标记（反引号 / 星号）。"""
    return re.sub(r"[`*]", "", text or "")


def short_label(title):
    """'QLUI API 手册（v0.1）' → 'API 手册'；'QLUI 阅读指南' → '阅读指南'"""
    if "API" in title:
        return "API 手册"
    if "阅读指南" in title:
        return "阅读指南"
    return title


class Page:
    def __init__(self, src, out, title, group, group_label, blurb=""):
        self.src = src
        self.out = out
        self.title = plain(title)
        self.group = group          # tut / api / spec / design
        self.group_label = group_label
        self.blurb = blurb
        self.toc = []


# ---------------------------------------------------------------- 收集

def collect():
    pages = []
    tutorials_dir = os.path.join(ROOT, "doc", "tutorial")
    seen = set()

    for label, _desc, stems in TUTORIAL_SECTIONS:
        for stem in stems:
            path = os.path.join(tutorials_dir, stem + ".md")
            if not os.path.isfile(path):
                print("警告：教程分组里的 %s.md 不存在" % stem, file=sys.stderr)
                continue
            text = open(path, encoding="utf-8").read()
            pages.append(Page(path, "tutorial/" + stem + ".html",
                              first_heading(text) or stem, "tut", label,
                              blurb_of(text)))
            seen.add(stem)

    if os.path.isdir(tutorials_dir):
        for name in sorted(os.listdir(tutorials_dir)):
            if name.endswith(".md") and name[:-3] not in seen:
                print("警告：%s 没有归到任何教程分组（把它加进 TUTORIAL_SECTIONS）"
                      % name, file=sys.stderr)

    for key, disp, _desc in MODULES:
        d = os.path.join(ROOT, "doc", "手册", key)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith(".md"):
                continue
            if key == "ui" and name[:-3] in [s for s, _ in SPEC_STEMS]:
                continue
            path = os.path.join(d, name)
            text = open(path, encoding="utf-8").read()
            pages.append(Page(path, "manual/%s/%s.html" % (key, name[:-3]),
                              first_heading(text) or name, "api:" + disp, disp,
                              blurb_of(text)))

    for stem, desc in SPEC_STEMS:
        path = os.path.join(ROOT, "doc", "手册", "ui", stem + ".md")
        if not os.path.isfile(path):
            print("警告：规范文档 %s.md 不存在" % stem, file=sys.stderr)
            continue
        text = open(path, encoding="utf-8").read()
        pages.append(Page(path, "manual/ui/" + stem + ".html",
                          first_heading(text) or stem, "spec", "规范与清单", desc))

    for name, sub in DESIGN_DOCS:
        path = os.path.join(ROOT, "doc", name + ".md")
        if not os.path.isfile(path):
            continue
        text = open(path, encoding="utf-8").read()
        pages.append(Page(path, name + ".html",
                          first_heading(text) or name, "design", "设计文档",
                          blurb_of(text)))

    return pages


# ---------------------------------------------------------------- 侧栏

def sidebar_groups(pages):
    """→ [(label, desc, [('a', href, text) | ('s', text)])]"""
    groups = []
    for label, desc, _stems in TUTORIAL_SECTIONS:
        items = [("a", p.out, p.title) for p in pages
                 if p.group == "tut" and p.group_label == label]
        if items:
            groups.append((label, desc, items))

    api_items = []
    for key, disp, desc in MODULES:
        sub = [p for p in pages if p.group == "api:" + disp]
        if not sub:
            continue
        api_items.append(("s", "%s —— %s" % (disp, desc)))
        for p in sorted(sub, key=lambda q: (0 if "阅读指南" in q.title else 1)):
            api_items.append(("a", p.out, short_label(p.title)))
    if api_items:
        groups.append(("API 手册", "按模块：阅读指南（为什么）→ API 手册（怎么用）", api_items))

    spec = [("a", p.out, p.title) for p in pages if p.group == "spec"]
    if spec:
        groups.append(("规范与清单", "HTML / CSS 支持到哪一步、还差什么、怎么排期", spec))

    design = [("a", p.out, p.title) for p in pages if p.group == "design"]
    if design:
        groups.append(("设计文档", "架构、取舍、依赖、规划与基准", design))
    return groups


SEARCH_JS = r"""
(function () {
  var IDX = window.QLGL_SEARCH || [];
  var box = document.getElementById('q');
  var panel = document.getElementById('qres');
  if (!box || !panel) return;
  var sel = -1, hits = [];

  function esc(s) { return s; }

  function render() {
    panel.textContent = '';
    if (!hits.length) {
      var d = document.createElement('div');
      d.className = 'empty';
      d.textContent = '没有匹配的页面';
      panel.appendChild(d);
      panel.hidden = false;
      return;
    }
    hits.forEach(function (h, i) {
      var a = document.createElement('a');
      a.className = 'hit' + (i === sel ? ' sel' : '');
      a.href = PREFIX + h.u;
      var t = document.createElement('span');
      t.className = 't';
      t.textContent = h.t;
      var c = document.createElement('span');
      c.className = 'c';
      c.textContent = h.b;
      a.appendChild(t); a.appendChild(c);
      panel.appendChild(a);
    });
    panel.hidden = false;
  }

  function search(q) {
    q = q.trim().toLowerCase();
    if (!q) { panel.hidden = true; hits = []; return; }
    var out = [];
    for (var i = 0; i < IDX.length; i++) {
      var e = IDX[i], t = e.t.toLowerCase(), s = 0;
      if (t === q) s = 100;
      else if (t.indexOf(q) === 0) s = 90;
      else if (t.indexOf(q) >= 0) s = 70;
      else if (e.k && e.b && e.b.toLowerCase().indexOf(q) >= 0) s = 30;
      if (s > 0) out.push({ s: s + (e.k ? 0 : 8), e: e });
    }
    out.sort(function (x, y) { return y.s - x.s || x.e.t.length - y.e.t.length; });
    hits = out.slice(0, 18).map(function (o) { return o.e; });
    sel = hits.length ? 0 : -1;
    render();
  }

  box.addEventListener('input', function () { search(box.value); });
  box.addEventListener('focus', function () { if (box.value.trim()) search(box.value); });
  box.addEventListener('keydown', function (ev) {
    if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
      ev.preventDefault();
      if (!hits.length) return;
      sel = (sel + (ev.key === 'ArrowDown' ? 1 : hits.length - 1)) % hits.length;
      render();
    } else if (ev.key === 'Enter') {
      if (sel >= 0 && hits[sel]) window.location.href = PREFIX + hits[sel].u;
    } else if (ev.key === 'Escape') {
      panel.hidden = true; box.blur();
    }
  });
  document.addEventListener('click', function (ev) {
    if (!panel.contains(ev.target) && ev.target !== box) panel.hidden = true;
  });
  document.addEventListener('keydown', function (ev) {
    if (ev.key === '/' && document.activeElement !== box &&
        !/^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement.tagName)) {
      ev.preventDefault(); box.focus(); box.select();
    }
  });
})();
"""


def build(pages, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    title_of = {p.out: p.title for p in pages}
    groups = sidebar_groups(pages)
    link_text = {}
    for _label, _desc, items in groups:
        for it in items:
            if it[0] == "a":
                link_text[it[1]] = it[2]

    def sidebar(active, prefix):
        out = ["<div class='brand'>"
               "<a href='%sindex.html' style='color:inherit'><b>QLGL</b>"
               "<span>C11 界面库 · 文档</span></a></div>" % prefix,
               "<div class='search'>"
               "<input id='q' type='text' placeholder='搜索页面与章节…' "
               "autocomplete='off' spellcheck='false'>"
               "<div id='qres' class='results' hidden></div></div>"]
        active_group = None
        for label, desc, items in groups:
            if any(it[0] == "a" and it[1] == active for it in items):
                active_group = label
        if active_group is None and groups:
            active_group = groups[0][0]

        for label, desc, items in groups:
            out.append("<details class='sec'%s><summary>%s</summary>"
                       % (" open" if label == active_group else "",
                          html.escape(label)))
            for it in items:
                if it[0] == "s":
                    out.append("<span class='mod'>%s</span>" % html.escape(it[1]))
                    continue
                href, text = it[1], it[2]
                cls = " class='current'" if href == active else ""
                out.append("<a href='%s%s'%s title='%s'>%s</a>"
                           % (prefix, href, cls, html.escape(text),
                              html.escape(text)))
            out.append("</details>")
        out.append("<p class='gen'>由 <code>tools/doc/gen_html.py</code> 生成<br>"
                   "搜索快捷键：<code>/</code></p>")
        return "\n".join(out)

    def toc_html(p):
        if not p.toc:
            return ""
        out = ["<aside class='toc'><h5>本页内容</h5><ul>"]
        for level, ident, text in p.toc:
            cls = " class='lv3'" if level == 3 else ""
            out.append("<li%s><a href='#%s'>%s</a></li>"
                       % (cls, ident, html.escape(text)))
        out.append("</ul></aside>")
        return "\n".join(out)

    def shell(active, prefix, page_title, body, toc="", pager="", lead=""):
        head = (
            "<!DOCTYPE html>\n<html lang='zh-CN'>\n<head>\n"
            "<meta charset='utf-8'>\n"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
            "<title>%s · QLGL</title>\n<style>%s</style>\n</head>\n<body>\n"
            "<div class='shell'>\n<aside class='sidebar'>\n%s\n</aside>\n"
            "<main><div class='inner'>\n" % (html.escape(page_title), CSS,
                                             sidebar(active, prefix))
        )
        tail = (
            "%s\n</div></main>\n%s\n</div>\n"
            "<script src='%ssearch-index.js'></script>\n"
            "<script>var PREFIX='%s';%s</script>\n"
            "</body>\n</html>\n" % (pager, toc, prefix, prefix, SEARCH_JS)
        )
        return head + body + tail

    order = [p.out for p in pages]
    out_map = {os.path.normpath(os.path.splitext(p.src)[0]): p.out for p in pages}
    search = []

    for p in pages:
        text = open(p.src, encoding="utf-8").read()
        # h1 由生成器渲染（要加分组眉标与导语），正文只转换 h1 之后的部分
        hm = re.match(r"\s*#\s+(.+?)\s*\n", text)
        rest_md = text[hm.end():] if hm else text
        lead, lead_block = find_lead(rest_md)
        if lead_block:
            rest_md = rest_md.replace(lead_block, "", 1)
        body = md_to_html(rest_md)
        body = rewrite_page_links(body, p, out_map)
        body, p.toc = add_anchors(body)

        depth = p.out.count("/")
        prefix = "../" * (0 if depth == 0 else depth)

        head_html = ("<p class='eyebrow'>%s</p>\n<h1>%s</h1>\n"
                     % (html.escape(p.group_label), html.escape(p.title)))
        if lead:
            head_html += "<p class='lead'>%s</p>\n" % html.escape(lead)
        body = head_html + body

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

        out = shell(p.out, prefix, p.title, body, toc_html(p), "\n".join(pager))
        dst = os.path.join(out_dir, p.out)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w", encoding="utf-8") as f:
            f.write(out)

        search.append({"t": p.title, "u": p.out, "b": p.group_label, "k": 0})
        for level, ident, htext in p.toc:
            search.append({"t": htext, "u": p.out + "#" + ident,
                           "b": p.title, "k": 1})

    # 首页：按分组铺卡片
    def cards(items):
        return "".join(
            "<a href='%s'><b>%s</b><span>%s</span></a>"
            % (p.out, html.escape(p.title), html.escape(p.blurb))
            for p in items)

    idx = ["<h1>QLGL 文档</h1>",
           "<p class='lead'>C11 写界面：HTML/CSS 描述结构，构建期或运行期编译成静态数据，"
           "CPU / GPU 双后端绘制。左侧按用途分组；<code>/</code> 或点搜索框找页面与章节。</p>"]
    for label, desc, _items in groups:
        if label == "API 手册":
            group_pages = [p for p in pages if p.group.startswith("api:")]
        else:
            group_pages = [p for p in pages if p.group_label == label]
        if not group_pages:
            continue
        idx.append("<h2 class='sec-title'>%s</h2><p class='sec-desc'>%s</p>"
                   % (html.escape(label), html.escape(desc)))
        idx.append("<div class='cards'>%s</div>" % cards(group_pages))

    with open(os.path.join(out_dir, "index.html"), "w", encoding="utf-8") as f:
        f.write(shell("index.html", "", "首页", "\n".join(idx)))

    import json
    with open(os.path.join(out_dir, "search-index.js"), "w", encoding="utf-8") as f:
        f.write("window.QLGL_SEARCH=")
        json.dump(search, f, ensure_ascii=False, separators=(",", ":"))
        f.write(";\n")

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
