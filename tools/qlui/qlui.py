#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 HTML + CSS 编译成 C 文件（相当于 Qt 的 uic）。

用法：
    python3 tools/qlui/qlui.py --html ui/index.html --css ui/index.css --out build/

产物：
    <out>/ui_<name>.c   静态节点表 + 样式表 + 文本池
    <out>/ui_<name>.h   extern const QLUIDoc ui_<name>_doc;

设计约束（见 doc/Web兼容设计.md §12）：
  * 运行期不做任何 CSS 解析与选择器匹配，全部在这里算完；
  * 不支持的选择器/属性/单位 → 报错并给出行号，绝不静默降级；
  * 文本被折叠成"元素的一段文本"，不支持一个元素里的混合内容。
"""

import argparse
import os
import re
import sys
from html.parser import HTMLParser

# ---------------------------------------------------------------- 常量表

TAGS = {
    "div": "QL_UI_TAG_DIV",
    "section": "QL_UI_TAG_SECTION",
    "p": "QL_UI_TAG_P",
    "span": "QL_UI_TAG_SPAN",
    "b": "QL_UI_TAG_B",
    "i": "QL_UI_TAG_I",
    "button": "QL_UI_TAG_BUTTON",
    "h1": "QL_UI_TAG_H1",
    "h2": "QL_UI_TAG_H2",
    "h3": "QL_UI_TAG_H3",
}

NAMED_COLORS = {
    "transparent": None,
    "black": 0x000000,
    "white": 0xFFFFFF,
    "red": 0xFF0000,
    "green": 0x008000,
    "lime": 0x00FF00,
    "blue": 0x0000FF,
    "gray": 0x808080,
    "grey": 0x808080,
    "silver": 0xC0C0C0,
    "yellow": 0xFFFF00,
    "orange": 0xFFA500,
    "purple": 0x800080,
    "navy": 0x000080,
    "teal": 0x008080,
}

DISPLAYS = {
    "block": "QL_UI_DISPLAY_BLOCK",
    "inline": "QL_UI_DISPLAY_INLINE",
    "inline-block": "QL_UI_DISPLAY_INLINE_BLOCK",
    "none": "QL_UI_DISPLAY_NONE",
}

ALIGNS = {
    "left": "QL_TEXT_ALIGN_LEFT",
    "center": "QL_TEXT_ALIGN_CENTER",
    "right": "QL_TEXT_ALIGN_RIGHT",
}

# 每个标签的 UA 默认样式（相当于浏览器的内置样式表）
UA_DEFAULTS = {
    "div":     {"display": "block"},
    "section": {"display": "block"},
    "p":       {"display": "block", "margin": "8px 0"},
    "span":    {"display": "inline"},
    "b":       {"display": "inline"},
    "i":       {"display": "inline"},
    "button":  {"display": "inline-block", "padding": "4px 10px",
                "border-width": "1px", "border-color": "#555",
                "background-color": "#d8d8d8", "color": "#000"},
    "h1":      {"display": "block", "font-size": "16px", "margin": "6px 0"},
    "h2":      {"display": "block", "font-size": "16px", "margin": "5px 0"},
    "h3":      {"display": "block", "font-size": "12px", "margin": "4px 0"},
}

INHERITED = ("color", "font-size", "text-align")

SIDES = ("top", "right", "bottom", "left")

SUPPORTED_PROPS = {
    "display", "width", "height", "margin", "padding", "border", "border-width",
    "border-color", "border-radius", "background", "background-color", "color",
    "font-size", "text-align", "box-sizing",
} | {"%s-%s" % (base, side) for base in ("margin", "padding") for side in SIDES}


class CssError(Exception):
    pass


# ---------------------------------------------------------------- 颜色的

def parse_color(text, where):
    t = text.strip().lower()
    if t in NAMED_COLORS:
        v = NAMED_COLORS[t]
        return 0, 0 if v is None else 255, (v or 0)
    if t.startswith("#"):
        h = t[1:]
        if len(h) == 3:
            h = "".join(c * 2 for c in h)
        if len(h) != 6 or not re.fullmatch(r"[0-9a-f]{6}", h):
            raise CssError("%s: 非法颜色 %r" % (where, text))
        return int(h, 16), 255, int(h, 16)
    m = re.fullmatch(r"rgb\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)", t)
    if m:
        r, g, b = (int(x) for x in m.groups())
        if max(r, g, b) > 255:
            raise CssError("%s: rgb() 分量超范围 %r" % (where, text))
        return (r << 16) | (g << 8) | b, 255, (r << 16) | (g << 8) | b
    raise CssError("%s: 不支持的颜色写法 %r（支持 #rgb/#rrggbb/rgb()/命名色）"
                   % (where, text))


def parse_length(text, where, allow_percent=True):
    """返回 ('px'|'percent'|'auto', 数值)"""
    t = text.strip().lower()
    if t == "auto":
        return "auto", 0
    m = re.fullmatch(r"(-?\d+(?:\.\d+)?)px", t)
    if m:
        return "px", int(round(float(m.group(1))))
    m = re.fullmatch(r"(-?\d+(?:\.\d+)?)%", t)
    if m:
        if not allow_percent:
            raise CssError("%s: 此处不支持百分比 %r" % (where, text))
        return "percent", int(round(float(m.group(1))))
    m = re.fullmatch(r"0", t)
    if m:
        return "px", 0
    raise CssError("%s: 不支持的长度 %r（只支持 px 与 %%）" % (where, text))


def parse_edges(text, where):
    """1/2/4 值语法，返回 (上,右,下,左) 的 px 元组"""
    parts = text.split()
    vals = []
    for p in parts:
        unit, num = parse_length(p, where, allow_percent=False)
        if unit != "px":
            raise CssError("%s: margin/padding 只支持 px" % where)
        vals.append(num)
    if len(vals) == 1:
        return (vals[0],) * 4
    if len(vals) == 2:
        return (vals[0], vals[1], vals[0], vals[1])
    if len(vals) == 4:
        return tuple(vals)
    raise CssError("%s: margin/padding 需要 1/2/4 个值" % where)


# ---------------------------------------------------------------- CSS 解析

class Rule:
    __slots__ = ("selector", "specificity", "decls", "order")

    def __init__(self, selector, specificity, decls, order):
        self.selector = selector
        self.specificity = specificity
        self.decls = decls
        self.order = order


def strip_comments(css):
    return re.sub(r"/\*.*?\*/", "", css, flags=re.S)


def split_selector(sel):
    """把 'div.box > span' 拆成 [(comb, [简单选择器...]), ...]

    返回 (简单选择器列表, specificity)。
    简单选择器用 dict 表示：{'tag':..,'id':..,'class':..}
    """
    # 先按组合符切开，保留组合符
    tokens = re.split(r"\s*([>+~])\s*|\s+", sel.strip())
    tokens = [t for t in tokens if t is not None and t.strip() != ""]

    parts = []       # [(kind, simple)]
    kind = "descendant"
    ids = classes = tags = 0

    for tok in tokens:
        if tok in (">", "+", "~"):
            if tok in ("+", "~"):
                raise CssError("不支持相邻/兄弟选择器 %r" % tok)
            kind = "child"
            continue
        if tok == "":
            continue

        simple = {}
        m = re.fullmatch(r"([a-zA-Z][\w-]*)?((?:\.[\w-]+)*)?(#[\w-]+)?", tok)
        if not m:
            raise CssError("不支持的简单选择器：%r" % tok)
        tag, cls, ident = m.group(1), m.group(2), m.group(3)
        if tag:
            if tag.lower() not in TAGS:
                raise CssError("不支持的标签选择器：%r" % tag)
            simple["tag"] = tag.lower()
            tags += 1
        if cls:
            simple["classes"] = [c for c in cls.split(".") if c]
            classes += len(simple["classes"])
        if ident:
            simple["id"] = ident[1:]
            ids += 1
        if not simple:
            raise CssError("空选择器片段：%r" % tok)
        parts.append((kind, simple))
        kind = "descendant"

    specificity = (ids, classes, tags)
    return parts, specificity


def parse_css(text, origin):
    text = strip_comments(text)
    rules = []
    order = 0
    pos = 0

    while pos < len(text):
        brace = text.find("{", pos)
        if brace < 0:
            break
        prelude = text[pos:brace].strip()
        end = text.find("}", brace)
        if end < 0:
            line = text[:brace].count("\n") + 1
            raise CssError("%s:%d: 规则缺少右花括号" % (origin, line))
        body = text[brace + 1:end]
        line = text[:brace].count("\n") + 1
        where = "%s:%d" % (origin, line)

        if prelude.startswith("@"):
            raise CssError("%s: 不支持 at-rule %r" % (where, prelude.split()[0]))

        decls = {}
        for piece in body.split(";"):
            piece = piece.strip()
            if not piece:
                continue
            if ":" not in piece:
                raise CssError("%s: 声明缺少冒号：%r" % (where, piece))
            prop, _, value = piece.partition(":")
            prop = prop.strip().lower()
            value = value.strip()
            if prop not in SUPPORTED_PROPS:
                raise CssError("%s: 不支持的属性 %r" % (where, prop))
            decls[prop] = value

        for sel in prelude.split(","):
            sel = sel.strip()
            if not sel:
                continue
            parts, spec = split_selector(sel)
            rules.append(Rule(parts, spec, dict(decls), order))
            order += 1

        pos = end + 1

    return rules


# ---------------------------------------------------------------- HTML 解析

class Node:
    __slots__ = ("tag", "classes", "id", "attrs", "children", "parent", "text")

    def __init__(self, tag, attrs=None, parent=None):
        self.tag = tag
        self.classes = []
        self.id = None
        self.attrs = attrs or {}
        self.children = []
        self.parent = parent
        self.text = None

    def add_text(self, chunk):
        chunk = re.sub(r"\s+", " ", chunk)
        if not chunk.strip():
            return
        if self.text is None:
            self.text = ""
        self.text += chunk.strip()


class TreeBuilder(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = Node("#document")
        self.stack = [self.root]
        self.line = 1

    def handle_starttag(self, tag, attrs):
        tag = tag.lower()
        if tag not in TAGS:
            raise CssError("<input> 第 %d 行：不支持的标签 <%s>" % (self.getpos()[0], tag))
        a = dict(attrs)
        node = Node(tag, a, self.stack[-1])
        if "class" in a:
            node.classes = [c for c in a["class"].split() if c]
        if "id" in a:
            node.id = a["id"]
        self.stack[-1].children.append(node)
        self.stack.append(node)

    def handle_endtag(self, tag):
        tag = tag.lower()
        if len(self.stack) == 1:
            raise CssError("第 %d 行：多余的结束标签 </%s>" % (self.getpos()[0], tag))
        if self.stack[-1].tag != tag:
            raise CssError("第 %d 行：结束标签 </%s> 与 <%s> 不匹配"
                           % (self.getpos()[0], tag, self.stack[-1].tag))
        self.stack.pop()

    def handle_data(self, data):
        self.stack[-1].add_text(data)

    def finish(self):
        if len(self.stack) != 1:
            raise CssError("标签未闭合：<%s>" % self.stack[-1].tag)
        self._check_mixed(self.root)
        return self.root

    def _check_mixed(self, node):
        if node.tag != "#document" and node.text is not None and node.children:
            raise CssError("<%s> 同时包含文本和子元素（混合内容）——v0 不支持，"
                           "请把文本放进独立的 <span>/<p>" % node.tag)
        for c in node.children:
            self._check_mixed(c)


# ---------------------------------------------------------------- 选择器匹配

def matches_simple(node, simple):
    if "tag" in simple and node.tag != simple["tag"]:
        return False
    if "id" in simple and node.id != simple["id"]:
        return False
    for c in simple.get("classes", []):
        if c not in node.classes:
            return False
    return True


def matches(node, parts):
    """从右往左匹配，parts = [(comb, simple), ...]"""
    def rec(n, idx, comb):
        if not matches_simple(n, parts[idx][1]):
            return False
        if idx == 0:
            return True
        nxt_comb = parts[idx][0]
        p = n.parent
        if nxt_comb == "child":
            return p is not None and p.tag != "#document" and rec(p, idx - 1, "child")
        while p is not None and p.tag != "#document":
            if rec(p, idx - 1, "descendant"):
                return True
            p = p.parent
        return False

    return rec(node, len(parts) - 1, "descendant")


# ---------------------------------------------------------------- 样式计算

def font_id_for(size_px):
    return 1 if size_px >= 14 else 0


def build_style(node, rules, inherited):
    """返回 dict（已展开的最终样式）"""
    # UA 默认
    decls = {}
    ua = UA_DEFAULTS.get(node.tag, {})
    decls.update(ua)

    # 匹配规则，按 (specificity, order) 升序覆盖
    matched = []
    for r in rules:
        if matches(node, r.selector):
            matched.append(r)
    matched.sort(key=lambda r: (r.specificity, r.order))
    for r in matched:
        for k, v in r.decls.items():
            decls[k] = v

    # 行内 style 优先
    if "style" in node.attrs:
        for piece in node.attrs["style"].split(";"):
            if not piece.strip():
                continue
            prop, _, value = piece.partition(":")
            prop = prop.strip().lower()
            if prop not in SUPPORTED_PROPS:
                raise CssError("行内 style 不支持属性 %r" % prop)
            decls[prop] = value.strip()

    style = dict(inherited)      # 继承
    style.update(decls)

    # 展开 shorthand
    if "background" in style and "background-color" not in style:
        style["background-color"] = style["background"]
    if "border" in style and "border-width" not in style:
        # border: 1px solid #333
        bits = style["border"].split()
        for b in bits:
            if re.fullmatch(r"-?\d+(\.\d+)?px", b):
                style["border-width"] = b
            elif b in ("solid", "none", "dashed", "dotted"):
                if b != "solid":
                    raise CssError("只支持 solid 边框，收到 %r" % b)
            else:
                style["border-color"] = b
    return style


# ---------------------------------------------------------------- 生成 C

def emit(doc_root, rules, name, out_dir):
    # 文本池
    text_pool = bytearray()
    text_offsets = []
    text_index = {}

    def intern_text(text):
        if text is None:
            return 0xFFFF
        if text in text_index:
            return text_index[text]
        off = len(text_pool)
        text_pool.extend(text.encode("utf-8"))
        text_pool.append(0)
        idx = len(text_offsets)
        text_offsets.append(off)
        text_index[text] = idx
        return idx

    style_src = []
    style_index = {}

    def add_style(st):
        key = repr(sorted(st.items()))
        if key in style_index:
            return style_index[key]
        style_src.append(st)
        style_index[key] = len(style_src) - 1
        return style_index[key]

    # 深度优先展开，保证 parent 下标 < 子节点下标
    flat = []

    def walk(n, parent_idx):
        my = len(flat)
        entry = {"node": n, "parent": parent_idx,
                 "first_child": 0xFFFF, "next_sibling": 0xFFFF}
        flat.append(entry)
        prev = None
        for c in n.children:
            ci = walk(c, my)
            if entry["first_child"] == 0xFFFF:
                entry["first_child"] = ci
            if prev is not None:
                flat[prev]["next_sibling"] = ci
            prev = ci
        return my

    if len(doc_root.children) != 1:
        raise CssError("文档必须有且只有一个根元素，当前 %d 个" % len(doc_root.children))
    walk(doc_root.children[0], 0xFFFF)

    # 排好序后逐节点建样式（继承依赖父节点，flat 顺序天然父先子后）
    for entry in flat:
        n = entry["node"]
        parent_style = {}
        if entry["parent"] != 0xFFFF:
            parent_style = flat[entry["parent"]]["style"]
        inherited = {k: parent_style[k] for k in INHERITED if k in parent_style}
        entry["style"] = build_style(n, rules, inherited)
        entry["style_index"] = add_style(entry["style"])
        entry["text_index"] = intern_text(n.text)

    def resolve_edges(st, base, where):
        """简写 + 长写（margin-top 等）合并"""
        values = list(parse_edges(st[base], where)) if base in st else [0, 0, 0, 0]
        for i, side in enumerate(SIDES):
            key = "%s-%s" % (base, side)
            if key in st:
                unit, num = parse_length(st[key], where, allow_percent=False)
                if unit != "px":
                    raise CssError("%s: %s 只支持 px" % (where, key))
                values[i] = num
        return tuple(values)

    def length_of(value, where, default_auto=True):
        if value is None:
            return ("auto", 0)
        return parse_length(value, where)

    # 生成 C
    lines = []
    ident = name if name.startswith("ui_") else "ui_" + name
    guard = "QLUI_%s_H" % name.upper()

    lines.append("/* 本文件由 tools/qlui/qlui.py 生成，请勿手工修改。 */")
    lines.append("/* 源：%s */" % name)
    lines.append("")
    lines.append('#include "%s.h"' % ident)
    lines.append("")

    lines.append("static const QLUINode %s_nodes[] = {" % ident)
    lines.append("    /* tag                     parent first  next   style  text */")
    for i, entry in enumerate(flat):
        n = entry["node"]
        lines.append(
            "    { %-22s, %5s, %5s, %5s, %5d, %5s },   /* <%s%s%s> */"
            % (TAGS[n.tag],
               "0xFFFF" if entry["parent"] == 0xFFFF else str(entry["parent"]),
               "0xFFFF" if entry["first_child"] == 0xFFFF else str(entry["first_child"]),
               "0xFFFF" if entry["next_sibling"] == 0xFFFF else str(entry["next_sibling"]),
               entry["style_index"],
               "0xFFFF" if entry["text_index"] == 0xFFFF else str(entry["text_index"]),
               n.tag,
               ("#" + n.id) if n.id else "",
               ("." + ".".join(n.classes)) if n.classes else "")
        )
    lines.append("};")
    lines.append("")

    lines.append("static const QLUIStyle %s_styles[] = {" % ident)
    for i, st in enumerate(style_src):
        where = "%s:style[%d]" % (name, i)
        bg, bg_a, _ = parse_color(st.get("background-color", "transparent"), where)
        fg, _, _ = parse_color(st.get("color", "black"), where)
        bd, _, _ = parse_color(st.get("border-color", "black"), where)
        wu, wv = length_of(st.get("width"), where)
        hu, hv = length_of(st.get("height"), where)
        margin = resolve_edges(st, "margin", where)
        padding = resolve_edges(st, "padding", where)
        disp = DISPLAYS.get(st.get("display", "block"))
        if disp is None:
            raise CssError("%s: 不支持的 display %r" % (where, st.get("display")))
        align = ALIGNS.get(st.get("text-align", "left"))
        if align is None:
            raise CssError("%s: 不支持的 text-align %r" % (where, st.get("text-align")))
        size_txt = st.get("font-size", "16px")
        _, size_px = parse_length(size_txt, where)
        bw_txt = st.get("border-width", "0")
        _, bw = parse_length(bw_txt, where)
        radius_txt = st.get("border-radius", "0")
        _, radius = parse_length(radius_txt, where)

        lines.append(
            "    { .bg_color = 0x%06X, .text_color = 0x%06X, .border_color = 0x%06X,"
            % (bg, fg, bd))
        lines.append(
            "      .width = { %d, %s }, .height = { %d, %s },"
            % (wv, "QL_UI_UNIT_PX" if wu == "px" else
               ("QL_UI_UNIT_PERCENT" if wu == "percent" else "QL_UI_UNIT_AUTO"),
               hv, "QL_UI_UNIT_PX" if hu == "px" else
               ("QL_UI_UNIT_PERCENT" if hu == "percent" else "QL_UI_UNIT_AUTO")))
        lines.append(
            "      .margin = { %d, %d, %d, %d }, .padding = { %d, %d, %d, %d },"
            % (margin[0], margin[1], margin[2], margin[3],
               padding[0], padding[1], padding[2], padding[3]))
        lines.append(
            "      .display = %s, .bg_alpha = %d, .border_width = %d,"
            % (disp, bg_a, bw))
        box_sizing = st.get("box-sizing", "content-box")
        if box_sizing not in ("content-box", "border-box"):
            raise CssError("%s: 不支持的 box-sizing %r" % (where, box_sizing))
        lines.append(
            "      .border_radius = %d, .text_align = %s, .font_id = %d,"
            " .box_sizing = %d },"
            % (radius, align, font_id_for(size_px), 1 if box_sizing == "border-box" else 0))
    lines.append("};")
    lines.append("")

    # 文本池
    if text_pool:
        lines.append("static const char %s_text[] = {" % ident)
        data = list(text_pool)
        for i in range(0, len(data), 16):
            lines.append("    " + ", ".join(str(b) for b in data[i:i + 16]) + ",")
        lines.append("};")
        lines.append("static const uint16_t %s_text_ofs[] = { %s };"
                     % (ident, ", ".join(str(o) for o in text_offsets)))
    else:
        lines.append('static const char %s_text[] = { 0 };' % ident)
        lines.append('static const uint16_t %s_text_ofs[] = { 0 };' % ident)
    lines.append("")

    lines.append("const QLUIDoc %s_doc = {" % ident)
    lines.append('    .name         = "%s",' % name)
    lines.append("    .node_count   = (uint16_t)(sizeof(%s_nodes) / sizeof(%s_nodes[0])),"
                 % (ident, ident))
    lines.append("    .text_count   = (uint16_t)(sizeof(%s_text_ofs) / sizeof(%s_text_ofs[0])),"
                 % (ident, ident))
    lines.append("    .nodes        = %s_nodes," % ident)
    lines.append("    .styles       = %s_styles," % ident)
    lines.append("    .text_pool    = %s_text," % ident)
    lines.append("    .text_offsets = %s_text_ofs," % ident)
    lines.append("};")
    lines.append("")

    header = [
        "/* 本文件由 tools/qlui/qlui.py 生成，请勿手工修改。 */",
        "#ifndef %s" % guard,
        "#define %s" % guard,
        "",
        "#include <qlgl/qlgl.h>",
        "",
        "extern const QLUIDoc %s_doc;" % ident,
        "",
        "#endif /* %s */" % guard,
        "",
    ]

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "%s.c" % ident), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    with open(os.path.join(out_dir, "%s.h" % ident), "w", encoding="utf-8") as f:
        f.write("\n".join(header))
    return len(flat), len(style_src), len(text_pool)


def main():
    ap = argparse.ArgumentParser(description="HTML+CSS → C")
    ap.add_argument("--html", required=True)
    ap.add_argument("--css", required=True)
    ap.add_argument("--name", help="输出文件名（默认取 html 的文件名）")
    ap.add_argument("--out", required=True, help="输出目录")
    args = ap.parse_args()

    name = args.name or os.path.splitext(os.path.basename(args.html))[0]
    name = re.sub(r"[^\w]", "_", name)

    try:
        with open(args.html, encoding="utf-8") as f:
            html = f.read()
        with open(args.css, encoding="utf-8") as f:
            css = f.read()

        builder = TreeBuilder()
        builder.feed(html)
        doc_root = builder.finish()
        rules = parse_css(css, os.path.basename(args.css))

        nodes, styles, text_bytes = emit(doc_root, rules, name, args.out)
        out_name = name if name.startswith("ui_") else "ui_" + name
        print("qlui: %s -> %s/%s.c（%d 节点，%d 样式，%d 字节文本）"
              % (name, args.out, out_name, nodes, styles, text_bytes))
        return 0
    except CssError as e:
        print("qlui: 错误：%s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
