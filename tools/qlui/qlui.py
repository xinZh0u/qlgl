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
    "flex": "QL_UI_DISPLAY_FLEX",
    "inline-flex": "QL_UI_DISPLAY_INLINE_FLEX",
}

FLEX_DIRECTIONS = {
    "row": "QL_UI_FLEX_ROW",
    "row-reverse": "QL_UI_FLEX_ROW_REVERSE",
    "column": "QL_UI_FLEX_COLUMN",
    "column-reverse": "QL_UI_FLEX_COLUMN_REVERSE",
}

JUSTIFY = {
    "flex-start": "QL_UI_JUSTIFY_START",
    "start": "QL_UI_JUSTIFY_START",
    "center": "QL_UI_JUSTIFY_CENTER",
    "flex-end": "QL_UI_JUSTIFY_END",
    "end": "QL_UI_JUSTIFY_END",
    "space-between": "QL_UI_JUSTIFY_SPACE_BETWEEN",
    "space-around": "QL_UI_JUSTIFY_SPACE_AROUND",
    "space-evenly": "QL_UI_JUSTIFY_SPACE_EVENLY",
}

ALIGN_ITEMS = {
    "stretch": "QL_UI_ALIGN_STRETCH",
    "flex-start": "QL_UI_ALIGN_START",
    "start": "QL_UI_ALIGN_START",
    "center": "QL_UI_ALIGN_CENTER",
    "flex-end": "QL_UI_ALIGN_END",
    "end": "QL_UI_ALIGN_END",
}

POSITIONS = {
    "static": "QL_UI_POSITION_STATIC",
    "relative": "QL_UI_POSITION_RELATIVE",
    "absolute": "QL_UI_POSITION_ABSOLUTE",
    "fixed": "QL_UI_POSITION_FIXED",
}

OVERFLOWS = {
    "visible": "QL_UI_OVERFLOW_VISIBLE",
    "hidden": "QL_UI_OVERFLOW_HIDDEN",
    "auto": "QL_UI_OVERFLOW_AUTO",
    "scroll": "QL_UI_OVERFLOW_AUTO",
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
    "font-size", "text-align", "box-sizing", "box-shadow",
    "flex", "flex-direction", "flex-wrap", "flex-grow", "flex-shrink", "flex-basis",
    "justify-content", "align-items", "gap", "row-gap", "column-gap",
    "position", "top", "right", "bottom", "left", "z-index", "overflow",
    "transform", "clip-path", "backdrop-filter",
    "transition", "transition-duration", "transition-timing-function",
    "transition-property",
    "animation", "animation-name", "animation-duration",
    "animation-timing-function", "animation-iteration-count",
    "animation-direction", "animation-fill-mode", "animation-delay",
    "animation-play-state",
} | {"%s-%s" % (base, side) for base in ("margin", "padding") for side in SIDES}


EASING_NAMES = {
    "linear": "QL_EASE_LINEAR",
    "ease-in": "QL_EASE_IN",
    "ease-out": "QL_EASE_OUT",
    "ease-in-out": "QL_EASE_IN_OUT",
    "ease": "QL_EASE_CSS",
}


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
    m = re.fullmatch(
        r"rgba\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([0-9.]+)\s*\)", t)
    if m:
        r, g, b = (int(m.group(i)) for i in (1, 2, 3))
        a = float(m.group(4))
        if max(r, g, b) > 255 or a < 0 or a > 1:
            raise CssError("%s: rgba() 分量超范围 %r" % (where, text))
        return (r << 16) | (g << 8) | b, int(round(a * 255)), (r << 16) | (g << 8) | b
    raise CssError("%s: 不支持的颜色写法 %r（支持 #rgb/#rrggbb/rgb()/rgba()/命名色）"
                   % (where, text))


GRADIENTS = {
    "to bottom": 1, "180deg": 1, "to top": 1,
    "to right": 2, "90deg": 2, "to left": 2,
}


def parse_gradient(text, where):
    """linear-gradient(<方向>, <起色>, <止色>)
       radial-gradient(<位置>, <起色>, <止色>)  → (起色, 止色, 方向)

    径向渐变只支持"圆心 = 元素中心"（这是 UI 里 99% 的用法：光晕、暗角）。
    写 at 偏移、ellipse、color stop 位置一律报错，不静默降级。
    """
    t = text.strip()
    m = re.fullmatch(r"radial-gradient\((.*)\)", t, re.S)
    if m:
        parts = [p.strip() for p in m.group(1).split(",")]
        if len(parts) != 3:
            raise CssError("%s: radial-gradient 需要 3 个参数（位置, 起色, 止色）"
                           % where)
        centre = parts[0].lower().replace(" ", "")
        if centre not in ("circle", "circleatcenter", "ellipse", "50%", "center"):
            raise CssError("%s: radial-gradient 只支持圆心在元素中心（收到 %r）；"
                           "at 偏移/自定义半径请改用 linear-gradient"
                           % (where, parts[0]))
        c0, _, _ = parse_color(parts[1], where)
        c1, _, _ = parse_color(parts[2], where)
        return c0, c1, 3          # 3 → QL_GRADIENT_RADIAL（见 qlui.h 的 bg_grad_dir）
    m = re.fullmatch(r"linear-gradient\((.*)\)", t, re.S)
    if not m:
        raise CssError("%s: 只支持 linear-gradient() / radial-gradient()，收到 %r"
                       % (where, text))
    parts = [p.strip() for p in m.group(1).split(",")]
    if len(parts) != 3:
        raise CssError("%s: linear-gradient 需要 3 个参数（方向, 起色, 止色）" % where)
    direction = GRADIENTS.get(parts[0].lower())
    if direction is None:
        raise CssError("%s: 不支持的渐变方向 %r（支持 to bottom/right、180deg/90deg）"
                       % (where, parts[0]))
    c0, _, _ = parse_color(parts[1], where)
    c1, _, _ = parse_color(parts[2], where)
    return c0, c1, direction


def parse_shadow(text, where):
    """box-shadow: <dx> <dy> [blur] <color> → (dx, dy, blur, rgb, alpha)

    注意 rgba(...) 里有空格，不能简单 split()，先把颜色整体抠出来。
    """
    t = " ".join(text.split())
    if t == "none":
        return 0, 0, 0, 0, 0

    color_txt = None
    for pat in (r"rgba?\([^)]*\)", r"#[0-9a-fA-F]{3,8}", r"[a-zA-Z]+"):
        m = re.search(pat, t)
        if m:
            color_txt = m.group(0)
            t = (t[:m.start()] + " " + t[m.end():]).strip()
            break
    if color_txt is None:
        raise CssError("%s: box-shadow 缺少颜色（不支持默认色）" % where)

    nums = []
    for piece in t.split():
        m = re.fullmatch(r"(-?\d+(?:\.\d+)?)px", piece)
        if m:
            nums.append(int(round(float(m.group(1)))))
        elif piece == "0":
            nums.append(0)
        else:
            raise CssError("%s: box-shadow 里无法识别 %r" % (where, piece))

    if len(nums) == 2:
        dx, dy = nums
        blur = 0
    elif len(nums) == 3:
        dx, dy, blur = nums
    else:
        raise CssError("%s: box-shadow 需要 2 或 3 个长度，收到 %d 个" % (where, len(nums)))
    if blur < 0:
        raise CssError("%s: box-shadow 的模糊半径不能为负" % where)

    rgb, a, _ = parse_color(color_txt, where)
    return dx, dy, blur, rgb, a


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


def _tf_px(token, where):
    """`-40px` 或 `0`（CSS 允许非零值省略单位以外的 0）"""
    t = token.strip()
    if re.fullmatch(r"-?\d+px", t):
        return int(t[:-2])
    if re.fullmatch(r"-?0", t):
        return 0
    raise CssError("%s: transform 的长度必须带 px 单位（拿到 %r）" % (where, token))


def _tf_scale(v, where, fn):
    """缩放倍数 → 千分比。0 合法（翻转动画的中间帧就是 0）。"""
    try:
        sc = int(round(float(v) * 1000.0))
    except ValueError:
        raise CssError("%s: %s 的参数不是数字（收到 %r）" % (where, fn, v))
    if sc < 0 or sc > 32767:
        raise CssError("%s: scale 倍数必须在 0~32.767（收到 %r）" % (where, fn))
    return sc


def parse_transform(text, where):
    """`transform` 支持 translate/scale/scaleX/scaleY，返回 (tx, ty, scx, scy)（千分比）。

    只做函数内部的整数像素平移与**轴对齐**缩放（绕元素中心）。
    scaleX/scaleY 单独出现时另一轴保持 1.0——这正是"假 3D 翻转"要的形状：
    `scaleX: 1 → 0 → 1`，元素在水平方向压扁到零再展开。
    非等比时圆角/描边/字号按 min(sx, sy) 近似（见 QLUI阅读指南 §5.4.3）。
    rotate/skew/matrix/3d 需要非轴对齐光栅化，仍然报错（不静默降级）。
    """
    t = re.sub(r"\s+", " ", text.strip())
    if t.lower() == "none":
        return 0, 0, 1000, 1000

    tx = ty = 0
    scx = scy = 1000
    seen = 0
    for fn in re.finditer(r"([a-zA-Z]+)\(([^()]*)\)", t):
        name = fn.group(1).lower()
        args = [a.strip() for a in fn.group(2).split(",") if a.strip()]
        if name == "translate":
            tx = _tf_px(args[0], where)
            ty = _tf_px(args[1], where) if len(args) > 1 else 0
        elif name == "translatex":
            tx = _tf_px(args[0], where)
        elif name == "translatey":
            ty = _tf_px(args[0], where)
        elif name == "scale":
            v0 = args[0]
            v1 = args[1] if len(args) > 1 else args[0]
            scx = _tf_scale(v0, where, fn.group(0))
            scy = _tf_scale(v1, where, fn.group(0))
        elif name == "scalex":
            scx = _tf_scale(args[0], where, fn.group(0))
        elif name == "scaley":
            scy = _tf_scale(args[0], where, fn.group(0))
        else:
            raise CssError("%s: transform 只支持 translate()/scale()/scaleX()/"
                           "scaleY()（收到 %r）" % (where, fn.group(0)))
        seen += 1

    if seen == 0 and t.lower() != "none":
        raise CssError("%s: 无法解析的 transform %r" % (where, text))
    return tx, ty, scx, scy


CLIP_NONE = 0
CLIP_CIRCLE = 1


def parse_clip_path(text, where):
    """`clip-path` 只支持 circle(<pct>%)，返回 (kind, 千分比)。

    圆心的语义固定为"元素中心"（与 radial-gradient 同一个决定）。
    inset()/polygon()/path() 一律报错——它们要 stencil 或逐像素采样。
    """
    t = re.sub(r"\s+", "", text.strip().lower())
    if t in ("none", ""):
        return CLIP_NONE, 0
    m = re.fullmatch(r"circle\(([0-9.]+)%\)", t)
    if not m:
        raise CssError("%s: clip-path 只支持 circle(<百分比>)（0%%~100%%），收到 %r"
                       % (where, text))
    pct = float(m.group(1))
    if pct < 0.0 or pct > 100.0:
        raise CssError("%s: clip-path circle() 的百分比必须在 0~100（收到 %r）"
                       % (where, text))
    return CLIP_CIRCLE, int(round(pct * 10))


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

# 伪类 → 状态位（与 QL_UI_RT_* 保持一致）
PSEUDO_STATES = {
    "hover": 1,
    "active": 2,
    "focus": 4,
}


class Rule:
    """一条 CSS 规则。states 是选择器要求的状态位图（0 = 普通规则）。"""

    __slots__ = ("selector", "specificity", "decls", "order", "states")

    def __init__(self, selector, specificity, decls, order, states=0):
        self.selector = selector
        self.specificity = specificity
        self.decls = decls
        self.order = order
        self.states = states


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
        m = re.fullmatch(
            r"([a-zA-Z][\w-]*)?((?:\.[\w-]+)*)?(#([\w-]+))?((?::[a-zA-Z-]+)*)", tok)
        if not m:
            raise CssError("不支持的简单选择器：%r" % tok)
        tag, cls, ident, pseudos = m.group(1), m.group(2), m.group(3), m.group(5)
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
        if pseudos:
            states = 0
            for name in pseudos.split(":")[1:]:
                name = name.lower()
                if name not in PSEUDO_STATES:
                    raise CssError("不支持的伪类 %r（只支持 :hover :active :focus）"
                                   % (":" + name))
                states |= PSEUDO_STATES[name]
                classes += 1          # 伪类与类选择器同级
            simple["states"] = states
        if not simple:
            raise CssError("空选择器片段：%r" % tok)
        parts.append((kind, simple))
        kind = "descendant"

    specificity = (ids, classes, tags)
    states = 0
    for _kind, simple in parts:
        states |= simple.get("states", 0)
    return parts, specificity, states


def _match_brace(text, open_pos, where):
    """从 open_pos 的 '{' 找到配对的 '}'（要跳过嵌套块，@keyframes 里有嵌套）"""
    depth = 0
    for i in range(open_pos, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    raise CssError("%s: 规则缺少右花括号" % where)


def parse_decls(body, where, allowed=None):
    """解析 `a: b; c: d` 形式的声明块"""
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
        if allowed is not None:
            if prop not in allowed:
                raise CssError("%s: @keyframes 里不支持的属性 %r（可用 %s）"
                               % (where, prop, " / ".join(sorted(allowed))))
        elif prop not in SUPPORTED_PROPS:
            raise CssError("%s: 不支持的属性 %r" % (where, prop))
        decls[prop] = value
    # 在**规则级**展开 shorthand：级联合并之后再展开会覆盖后出现的长写
    return expand_shorthands(decls) if allowed is None else decls


# @keyframes 里能写的属性。只允许"绘制期能插值"的那些——与 transition 同一套通道，
# 别的一律报错，避免"写上了但不生效"这种半正确状态。
KEYFRAME_PROPS = {"background", "background-color", "background-image",
                  "opacity", "transform", "color", "border-color", "border-radius",
                  "clip-path", "animation-timing-function"}


def _kf_offset(sel, where):
    """`from` / `to` / `N%` → 0..1000 千分比"""
    t = sel.strip().lower()
    if t == "from":
        return 0
    if t == "to":
        return 1000
    m = re.fullmatch(r"(\d+(?:\.\d+)?)%", t)
    if not m:
        raise CssError("%s: @keyframes 里的选择器只能是 from / to / N%%（收到 %r）"
                       % (where, sel))
    v = float(m.group(1))
    if v < 0 or v > 100:
        raise CssError("%s: @keyframes 的百分比必须在 0%%~100%%（收到 %r）"
                       % (where, sel))
    return int(round(v * 10))


def parse_keyframes(body, where, frames):
    """解析 `0% { ... } 50% { ... } to { ... }`，追加到 frames"""
    pos = 0
    while True:
        brace = body.find("{", pos)
        if brace < 0:
            break
        sel = body[pos:brace].strip()
        if not sel:
            raise CssError("%s: @keyframes 里有空选择器" % where)
        end = _match_brace(body, brace, where)
        decls = parse_decls(body[brace + 1:end], where, KEYFRAME_PROPS)
        for one in sel.split(","):
            frames.append({"offset": _kf_offset(one, where), "decls": decls})
        pos = end + 1


def parse_css(text, origin):
    text = strip_comments(text)
    rules = []
    keyframes = {}
    order = 0
    pos = 0

    while pos < len(text):
        brace = text.find("{", pos)
        if brace < 0:
            break
        prelude = text[pos:brace].strip()
        line = text[:brace].count("\n") + 1
        where = "%s:%d" % (origin, line)
        end = _match_brace(text, brace, where)
        body = text[brace + 1:end]

        if prelude.startswith("@"):
            m = re.fullmatch(r"@keyframes\s+([-\w]+)", prelude)
            if not m:
                raise CssError("%s: 不支持 at-rule %r" % (where, prelude.split()[0]))
            kf_name = m.group(1)
            if kf_name in ("none", "initial", "inherit", "infinite"):
                raise CssError("%s: @keyframes 名字 %r 与 CSS 关键字冲突"
                               % (where, kf_name))
            if kf_name in keyframes:
                raise CssError("%s: @keyframes %r 重复定义" % (where, kf_name))
            frames = []
            parse_keyframes(body, where, frames)
            if not frames:
                raise CssError("%s: @keyframes %r 没有任何关键帧" % (where, kf_name))
            keyframes[kf_name] = frames
            pos = end + 1
            continue

        decls = parse_decls(body, where)
        for sel in prelude.split(","):
            sel = sel.strip()
            if not sel:
                continue
            parts, spec, states = split_selector(sel)
            rules.append(Rule(parts, spec, dict(decls), order, states))
            order += 1

        pos = end + 1

    return rules, keyframes


# ---------------------------------------------------------------- HTML 解析

class Node:
    """元素节点。

    items 按**文档顺序**保存内容：str（文本段）或 Node（子元素）。
    只有文本时 → text 直接放文本（快路径，绝大多数节点走这条）；
    文本与元素混排时 → 由 _flatten_mixed 把文本段包成匿名 <span> 子节点，
    这样运行期数据模型完全不用改（混合内容在**生成期**就消解掉了）。
    """

    __slots__ = ("tag", "classes", "id", "attrs", "children", "parent", "text",
                 "items")

    def __init__(self, tag, attrs=None, parent=None):
        self.tag = tag
        self.classes = []
        self.id = None
        self.attrs = attrs or {}
        self.children = []
        self.parent = parent
        self.text = None
        self.items = []

    def add_text(self, chunk):
        chunk = re.sub(r"\s+", " ", chunk)
        if not chunk:
            return
        self.items.append(chunk)

    def add_child(self, node):
        self.children.append(node)
        self.items.append(node)




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
        self.stack[-1].add_child(node)
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
        self._flatten(self.root)
        return self.root

    def _flatten(self, node):
        """把"文本与元素混排"在生成期消解成匿名的 inline 子节点。

        <p>Hello <b>world</b>!</p>
          → <p><span>Hello </span><b>world</b><span>!</span></p>

        这样运行期只认识"元素树"，混合内容不需要任何新的数据模型与布局代码。
        纯文本节点（无子元素）保持 text 快路径不变。
        """
        # 先递归处理子节点
        for c in list(node.children):
            self._flatten(c)

        if node.tag == "#document":
            return

        has_text = any(isinstance(it, str) for it in node.items)
        has_elem = any(isinstance(it, Node) for it in node.items)

        if not has_text:
            node.items = []
            return
        if not has_elem:
            node.text = "".join(it for it in node.items if isinstance(it, str)).strip()
            node.text = re.sub(r"\s+", " ", node.text) or None
            node.items = []
            return

        # 混排：按文档顺序重建 children，每个文本段包一个匿名 span
        str_positions = [i for i, it in enumerate(node.items) if isinstance(it, str)]
        first_i = str_positions[0] if str_positions else -1
        last_i = str_positions[-1] if str_positions else -1

        ordered = []
        for i, it in enumerate(node.items):
            if isinstance(it, str):
                txt = it
                if i == first_i:
                    txt = txt.lstrip()
                if i == last_i:
                    txt = txt.rstrip()
                # 元素之间的纯空白不产生盒子（HTML 的空白折叠规则）——
                # 否则每个换行缩进都会变成一个 4px 宽的匿名 span，撑高整页。
                if not txt.strip():
                    continue
                span = Node("span", {}, node)
                span.text = txt
                ordered.append(span)
            else:
                ordered.append(it)

        node.children = ordered
        node.items = []


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

# 文档里出现的所有 font-size 会汇总成一张表，节点的 font_id 是表下标
FONT_SIZES = []


def parse_number(text, where, allow_percent=False, allow_auto=False):
    """返回 ('px'|'percent'|'auto', 值)"""
    t = text.strip().lower()
    if allow_auto and t == "auto":
        return "auto", 0
    m = re.fullmatch(r"(-?\d+(?:\.\d+)?)px", t)
    if m:
        return "px", int(round(float(m.group(1))))
    if allow_percent:
        m = re.fullmatch(r"(-?\d+(?:\.\d+)?)%", t)
        if m:
            return "percent", int(round(float(m.group(1))))
    if re.fullmatch(r"-?\d+(?:\.\d+)?", t):
        return "number", float(t)
    if t == "0":
        return "px", 0
    raise CssError("%s: 无法解析数值 %r" % (where, text))


def parse_flex_shorthand(text, where):
    """flex: 1 / 1 0 auto / 0 0 100px / none / auto → (grow, shrink, basis)

    与 CSS 的展开规则一致（单值 = grow，双值 = grow shrink，三值 = 全写）。
    """
    t = text.strip().lower()
    if t == "none":
        return 0, 0, ("auto", 0)
    if t == "auto":
        return 1, 1, ("auto", 0)
    parts = t.split()
    if not 1 <= len(parts) <= 3:
        raise CssError("%s: flex 只能是 1~3 个值，收到 %r" % (where, text))

    def num(p):
        try:
            return float(p)
        except ValueError:
            raise CssError("%s: flex 里的 %r 不是数字" % (where, p))

    if len(parts) == 1:
        kind, val = parse_number(parts[0], where, allow_percent=True, allow_auto=True)
        if kind in ("px", "percent") or parts[0] == "auto":
            return 1, 1, (kind, val)          # flex: <basis>
        return num(parts[0]), 1, ("auto", 0)
    if len(parts) == 2:
        kind, val = parse_number(parts[1], where, allow_percent=True, allow_auto=True)
        if kind in ("px", "percent") or parts[1] == "auto":
            return num(parts[0]), 1, (kind, val)
        return num(parts[0]), num(parts[1]), ("auto", 0)

    kind, val = parse_number(parts[2], where, allow_percent=True, allow_auto=True)
    return num(parts[0]), num(parts[1]), (kind, val)


def parse_gap(text, where):
    """gap: <row> [<col>] → (row_px, col_px)"""
    parts = text.split()
    if len(parts) == 1:
        kind, v = parse_number(parts[0], where)
        if kind == "percent":
            raise CssError("%s: gap 不支持百分比" % where)
        return v, v
    if len(parts) == 2:
        k1, v1 = parse_number(parts[0], where)
        k2, v2 = parse_number(parts[1], where)
        if k1 == "percent" or k2 == "percent":
            raise CssError("%s: gap 不支持百分比" % where)
        return v1, v2
    raise CssError("%s: gap 只支持 1~2 个值，收到 %r" % (where, text))


def font_id_for(size_px):
    if size_px not in FONT_SIZES:
        if len(FONT_SIZES) >= 255:
            raise CssError("font-size 种类过多（上限 255）")
        FONT_SIZES.append(size_px)
    return FONT_SIZES.index(size_px)


def collect_decls(node, rules, states_active):
    """按 (specificity, order) 覆盖出某个「状态组合」下的声明集合。

    states_active 为 0 时只算普通规则；为某状态位时把该状态的规则也算进来。
    """
    decls = {}
    decls.update(UA_DEFAULTS.get(node.tag, {}))

    matched = []
    for r in rules:
        if (r.states & ~states_active) != 0:
            continue
        if matches(node, r.selector):
            matched.append(r)
    matched.sort(key=lambda r: (r.specificity, r.order))
    for r in matched:
        for k, v in r.decls.items():
            decls[k] = v
    return decls


def build_style(node, rules, inherited):
    """返回 (基础样式 dict, {状态位: 该状态下的样式 dict})

    伪类规则只会改变"外观类"属性；布局仍按基础样式算（见 doc §M1 说明），
    因此这里不区分继承来源，各状态用同一份 inherited。
    """
    base = finalize_style(collect_decls(node, rules, 0), node, inherited)
    variants = {}
    for bit in PSEUDO_STATES.values():
        variant = finalize_style(collect_decls(node, rules, bit), node, inherited)
        # 动画在 ql_ui_build 时按**基础样式**起播，写在伪类里永远不会被触发。
        # 与其静默失效，不如明确报错。
        if (variant.get("animation-name", "none") != "none" and
                variant.get("animation-name") != base.get("animation-name", "none")):
            raise CssError("<%s%s> 的状态规则里不支持 animation："
                           "关键帧动画只按基础样式起播"
                           % (node.tag,
                              ("." + ".".join(node.classes)) if node.classes else ""))
        variants[bit] = variant
    return base, variants


def expand_shorthands(decls):
    """把一条规则里的 shorthand 展开成对应的长写（原地修改并返回）。

    必须在**单条规则内**展开，不能等到级联合并之后再做：
    `animation: pulse 1s` 与后出现的 `animation-name: other` 属于两条规则，
    级联合并后 `animation-name` 已经是 `other`，这时候再展开 shorthand
    就会把它覆盖回 `pulse`——`.a{} .b{}` 这种写法会全部退化成第一条规则的值
    （踩过：10 张卡片用了同一段关键帧）。同理适用于 transition / flex / gap /
    background / border。
    """
    # transition 简写：`transition: 200ms ease` / `200ms` / `200ms ease 50ms`
    if "transition" in decls:
        parts = decls["transition"].replace(",", " ").split()
        dur = None
        tf = None
        for part in parts:
            pl = part.strip().lower()
            if pl in ("all", "background-color", "opacity", "color", "border-color",
                      "border-radius", "transform", "clip-path"):
                decls["transition-property"] = pl
            elif pl in EASING_NAMES:
                tf = pl
            elif re.fullmatch(r"-?[0-9.]+m?s", pl):
                if dur is None:
                    dur = pl
                # 第二个时长是 delay；本实现不支持延迟，静默忽略会违反"不静默降级"
                else:
                    raise CssError("transition: 暂不支持 delay（%r）" % part)
            else:
                raise CssError("transition: 无法识别的部分 %r" % part)
        if dur is not None:
            decls["transition-duration"] = dur
        if tf is not None:
            decls["transition-timing-function"] = tf
        del decls["transition"]

    # animation 简写：`animation: pulse 1s ease-out infinite alternate forwards`
    if "animation" in decls:
        if "steps(" in decls["animation"] or "cubic-bezier" in decls["animation"]:
            raise CssError("animation: 暂不支持 steps()/cubic-bezier()，"
                           "请用 linear / ease / ease-in / ease-out / ease-in-out")
        for part in decls["animation"].split():
            pl = part.strip().lower()
            if re.fullmatch(r"-?[0-9.]+m?s", pl):
                if "animation-duration" not in decls:
                    decls["animation-duration"] = pl
                elif "animation-delay" not in decls:
                    decls["animation-delay"] = pl     # CSS：第一个时长是 duration，第二个是 delay
                else:
                    raise CssError("animation: 时长写了三个（%r）" % part)
            elif pl == "infinite" or re.fullmatch(r"\d+", pl):
                decls["animation-iteration-count"] = pl
            elif pl in EASING_NAMES:
                decls["animation-timing-function"] = pl
            elif pl in ("normal", "reverse", "alternate", "alternate-reverse"):
                decls["animation-direction"] = pl
            elif pl in ("forwards", "backwards", "both"):
                decls["animation-fill-mode"] = pl
            elif pl in ("running", "paused"):
                if pl == "paused":
                    raise CssError("animation-play-state: 暂不支持 paused")
            elif pl == "none":
                decls["animation-name"] = "none"
            else:
                decls["animation-name"] = pl
        del decls["animation"]

    if "flex" in decls:
        g, sh, (bk, bv) = parse_flex_shorthand(decls["flex"], "flex")
        decls["flex-grow"] = ("%g" % g)
        decls["flex-shrink"] = ("%g" % sh)
        if bk == "auto":
            decls["flex-basis"] = "auto"
        elif bk == "percent":
            decls["flex-basis"] = "%d%%" % bv
        else:
            decls["flex-basis"] = "%dpx" % bv
        del decls["flex"]

    if "gap" in decls:
        gr, gc = parse_gap(decls["gap"], "gap")
        decls["row-gap"] = "%dpx" % gr
        decls["column-gap"] = "%dpx" % gc
        del decls["gap"]

    if "background" in decls:
        bg = decls.pop("background")
        if "gradient(" in bg:
            decls["background-image"] = bg
        else:
            decls["background-color"] = bg

    if "border" in decls:
        bits = decls.pop("border").split()
        for b in bits:
            if re.fullmatch(r"-?\d+(\.\d+)?px", b):
                decls["border-width"] = b
            elif b in ("solid", "none", "dashed", "dotted"):
                if b != "solid":
                    raise CssError("只支持 solid 边框，收到 %r" % b)
            else:
                decls["border-color"] = b

    return decls


def finalize_style(decls, node, inherited):
    """合并行内样式与继承，返回最终 dict（shorthand 已在规则级展开过）"""
    # 行内 style 优先；它自己也是一条"规则"，单独展开
    if "style" in node.attrs:
        inline = {}
        for piece in node.attrs["style"].split(";"):
            if not piece.strip():
                continue
            prop, _, value = piece.partition(":")
            prop = prop.strip().lower()
            if prop not in SUPPORTED_PROPS:
                raise CssError("行内 style 不支持属性 %r" % prop)
            inline[prop] = value.strip()
        decls.update(expand_shorthands(inline))

    style = dict(inherited)      # 继承
    style.update(decls)
    return style


# ---------------------------------------------------------------- 生成 C

ANIM_DIRECTIONS = {
    "normal": "QL_UI_ANIM_NORMAL",
    "reverse": "QL_UI_ANIM_REVERSE",
    "alternate": "QL_UI_ANIM_ALTERNATE",
    "alternate-reverse": "QL_UI_ANIM_ALTERNATE_REVERSE",
}

ANIM_FILLS = {
    "none": "QL_UI_ANIM_FILL_NONE",
    "forwards": "QL_UI_ANIM_FILL_FORWARDS",
    "backwards": "QL_UI_ANIM_FILL_BACKWARDS",
    "both": "QL_UI_ANIM_FILL_BOTH",
}

KF_EASE_DEFAULT = 0xFF      # 关键帧没写 timing function → 用动画的默认缓动


def parse_duration(text, where, prop):
    """`200ms` / `1.5s` → 毫秒整数"""
    m = re.fullmatch(r"([0-9.]+)(ms|s)", text.strip().lower())
    if not m:
        raise CssError("%s: %s 需要 ms/s 单位（%r）" % (where, prop, text))
    val = float(m.group(1)) * (1000.0 if m.group(2) == "s" else 1.0)
    ms = int(round(val))
    if ms < 0 or ms > 65535:
        raise CssError("%s: %s 超出范围（%r）" % (where, prop, text))
    return ms


def compile_keyframes(keyframes, cur_ease_of):
    """把解析出的 @keyframes 编译成静态帧表。

    cur_ease_of(frame) → QLEasing 名字；关键帧自带 timing function 时用它，
    否则返回 KF_EASE_DEFAULT（运行期回落到动画的默认缓动）。

    返回 (frames, ranges)：frames 是 C 侧 QLUIKeyframe 的 dict 列表，
    ranges[name] = (first_frame, frame_count)。
    """
    frames = []
    ranges = {}

    for name, raw in keyframes.items():
        where = "@keyframes %s" % name
        offsets = [f["offset"] for f in raw]
        if len(set(offsets)) != len(offsets):
            raise CssError("%s: 有重复的百分比" % where)

        # 每一帧的通道集合必须一致——否则"这一段插的是颜色、下一段插的是 alpha"，
        # 运行期要额外记录隐式值，容易做出半正确的效果。宁可报错。
        props = None
        for f in raw:
            p = 0
            if any(k in f["decls"] for k in ("background", "background-color",
                                             "background-image")):
                p |= 1
            if "opacity" in f["decls"]:
                p |= 2
            if "transform" in f["decls"]:
                p |= 4
            if "color" in f["decls"]:
                p |= 8
            if "border-color" in f["decls"]:
                p |= 16
            if "border-radius" in f["decls"]:
                p |= 32
            if "clip-path" in f["decls"]:
                p |= 64
            if p == 0:
                raise CssError("%s: %d%% 这一帧没有可动画的属性"
                               % (where, f["offset"] // 10))
            if props is None:
                props = p
            elif p != props:
                raise CssError("%s: 各帧写的属性不一致（%d%% 这帧少了别的帧有的属性）"
                               % (where, f["offset"] // 10))

        ordered = sorted(raw, key=lambda f: f["offset"])
        # 缺 0% / 100% 时补"取元素基础样式"的隐式帧（CSS 的隐式关键帧语义）
        if ordered[0]["offset"] != 0:
            ordered.insert(0, {"offset": 0, "decls": None})
        if ordered[-1]["offset"] != 1000:
            ordered.append({"offset": 1000, "decls": None})

        first = len(frames)
        for f in ordered:
            if f["decls"] is None:
                frames.append({"offset": f["offset"], "ease": KF_EASE_DEFAULT,
                               "props": props | 0x80, "bg": 0, "bg2": 0, "alpha": 0,
                               "tf_x": 0, "tf_y": 0, "sc_x": -1, "sc_y": -1, "text": 0, "border": 0,
                               "radius": -1, "clip": -1})
                continue
            bg = bg2 = 0
            alpha = 255
            tfx = tfy = 0
            tfscx = tfscy = -1
            text_c = border_c = 0
            if (props & 4) != 0:
                tfx, tfy, tfscx, tfscy = parse_transform(
                    f["decls"]["transform"], where)
            if (props & 8) != 0:
                text_c, _, _ = parse_color(f["decls"]["color"], where)
            if (props & 16) != 0:
                border_c, _, _ = parse_color(f["decls"]["border-color"], where)
            clipr = -1                                # -1 = 用元素基础值
            if (props & 64) != 0:
                _, clipr = parse_clip_path(f["decls"]["clip-path"], where)
                if clipr == 0 and "none" in f["decls"]["clip-path"].lower():
                    clipr = 0
            rad = -1                                  # -1 = 用元素基础值
            if (props & 32) != 0:
                _, rad = parse_length(f["decls"]["border-radius"], where)
                if rad < 0 or rad > 32767:
                    raise CssError("%s: border-radius 超出 0~32767px" % where)
            if (props & 1) != 0:
                if "background-image" in f["decls"]:
                    bg, bg2, _dir = parse_gradient(f["decls"]["background-image"],
                                                   where)
                else:
                    txt = f["decls"].get("background-color",
                                         f["decls"].get("background", "transparent"))
                    bg, alpha, _ = parse_color(txt, where)
                    bg2 = bg
            if (props & 2) != 0:
                try:
                    op = float(f["decls"]["opacity"])
                except ValueError:
                    raise CssError("%s: opacity 必须是 0~1 的数字" % where)
                if op < 0.0 or op > 1.0:
                    raise CssError("%s: opacity 超出 0~1" % where)
                alpha = int(round(op * 255.0))
            tf = f["decls"].get("animation-timing-function")
            if tf is None:
                ease = KF_EASE_DEFAULT
            elif tf.strip().lower() in EASING_NAMES:
                ease = EASING_NAMES[tf.strip().lower()]
            else:
                raise CssError("%s: 暂不支持的 animation-timing-function %r"
                               % (where, tf))
            frames.append({"offset": f["offset"], "ease": ease, "props": props,
                           "bg": bg, "bg2": bg2, "alpha": alpha,
                           "tf_x": tfx, "tf_y": tfy,
                           "sc_x": tfscx, "sc_y": tfscy,
                           "text": text_c, "border": border_c, "radius": rad,
                           "clip": clipr})
        ranges[name] = (first, len(frames) - first)

    return frames, ranges


def emit(doc_root, rules, name, out_dir, keyframes):
    FONT_SIZES.clear()
    kf_frames, kf_ranges = compile_keyframes(keyframes, None)

    anim_src = []
    anim_index = {}

    def add_anim(name_, dur, ease, iters, direction, fill, delay):
        key = (name_, dur, ease, iters, direction, fill, delay)
        if key in anim_index:
            return anim_index[key]
        first, count = kf_ranges[name_]
        anim_src.append({"first": first, "count": count, "dur": dur, "ease": ease,
                         "iters": iters, "direction": direction, "fill": fill,
                         "delay": delay})
        anim_index[key] = len(anim_src) - 1
        return anim_index[key]

    def anim_index_of(st, where):
        """样式里的 animation-* → doc->anims 下标（0xFFFF = 没有）"""
        an = st.get("animation-name", "none").strip().lower()
        if an in ("none", "", "initial"):
            return 0xFFFF
        if an not in kf_ranges:
            raise CssError("%s: animation-name %r 没有对应的 @keyframes"
                           % (where, an))
        dur_txt = st.get("animation-duration")
        if dur_txt is None:
            raise CssError("%s: 有 animation-name 但没写 animation-duration"
                           % where)
        dur = parse_duration(dur_txt, where, "animation-duration")
        if dur == 0:
            raise CssError("%s: animation-duration 不能为 0（那等于不播）" % where)
        tf = st.get("animation-timing-function", "ease").strip().lower()
        if tf not in EASING_NAMES:
            raise CssError("%s: 暂不支持的 animation-timing-function %r"
                           % (where, tf))
        ic = st.get("animation-iteration-count", "1").strip().lower()
        if ic == "infinite":
            iters = 0
        else:
            try:
                iters = int(ic)
            except ValueError:
                raise CssError("%s: animation-iteration-count 必须是整数或 infinite"
                               % where)
            if iters < 1 or iters > 255:
                raise CssError("%s: animation-iteration-count 超出 1~255" % where)
        dr = st.get("animation-direction", "normal").strip().lower()
        if dr not in ANIM_DIRECTIONS:
            raise CssError("%s: 不支持的 animation-direction %r" % (where, dr))
        fl = st.get("animation-fill-mode", "none").strip().lower()
        if fl not in ANIM_FILLS:
            raise CssError("%s: 不支持的 animation-fill-mode %r" % (where, fl))
        dly = parse_duration(st.get("animation-delay", "0ms"), where,
                             "animation-delay")
        return add_anim(an, dur, EASING_NAMES[tf], iters, ANIM_DIRECTIONS[dr],
                        ANIM_FILLS[fl], dly)

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
        entry["style"], entry["variants"] = build_style(n, rules, inherited)
        entry["style_index"] = add_style(entry["style"])
        # 只有真正不同才生成变体，没写 :hover 的节点就少 3 个下标
        entry["state_index"] = {}
        for bit, variant in entry["variants"].items():
            entry["state_index"][bit] = (0xFFFF if variant == entry["style"]
                                         else add_style(variant))
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

    def hex16(v):
        return "0xFFFF" if v == 0xFFFF else str(v)

    lines.append("static const QLUINode %s_nodes[] = {" % ident)
    lines.append("    /* tag                     parent first  next   style  text"
                 "   hover  active focus */")
    for entry in flat:
        n = entry["node"]
        st = entry["state_index"]
        lines.append(
            "    { %-22s, %5s, %5s, %5s, %5s, %5s, %5s, %5s, %5s },"
            "   /* <%s%s%s> */"
            % (TAGS[n.tag],
               hex16(entry["parent"]),
               hex16(entry["first_child"]),
               hex16(entry["next_sibling"]),
               hex16(entry["style_index"]),
               hex16(entry["text_index"]),
               hex16(st[1]), hex16(st[2]), hex16(st[4]),
               n.tag,
               ("#" + n.id) if n.id else "",
               ("." + ".".join(n.classes)) if n.classes else "")
        )
    lines.append("};")
    lines.append("")

    font_sizes_line = len(lines)      # 占位：样式表生成之后才知道用到哪些字号
    lines.append("")
    lines.append("static const QLUIStyle %s_styles[] = {" % ident)
    for i, st in enumerate(style_src):
        where = "%s:style[%d]" % (name, i)
        bg, bg_a, _ = parse_color(st.get("background-color", "transparent"), where)
        fg, _, _ = parse_color(st.get("color", "black"), where)
        bd, _, _ = parse_color(st.get("border-color", "black"), where)

        # 渐变背景：背景色取起始色，终止色与方向单列
        bg2, grad_dir = bg, 0
        if "background-image" in st:
            g0, g1, grad_dir = parse_gradient(st["background-image"], where)
            bg = g0
            bg2 = g1
            if bg_a == 0:
                bg_a = 255

        # 阴影
        sh_dx = sh_dy = sh_blur = sh_a = sh_color = 0
        if "box-shadow" in st:
            sh_dx, sh_dy, sh_blur, sh_color, sh_a = parse_shadow(st["box-shadow"], where)
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
            "      .bg_color2 = 0x%06X, .shadow_color = 0x%06X,"
            % (bg2, sh_color))
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
            "      .shadow_dx = %d, .shadow_dy = %d," % (sh_dx, sh_dy))
        lines.append(
            "      .border_radius = %d, .text_align = %s, .font_id = %d,"
            " .box_sizing = %d," % (radius, align, font_id_for(size_px),
                                    1 if box_sizing == "border-box" else 0))
        # ---- flex 容器 ----
        fdir = FLEX_DIRECTIONS.get(st.get("flex-direction", "row"))
        if fdir is None:
            raise CssError("%s: 不支持的 flex-direction %r"
                           % (where, st.get("flex-direction")))
        jc = JUSTIFY.get(st.get("justify-content", "flex-start"))
        if jc is None:
            raise CssError("%s: 不支持的 justify-content %r"
                           % (where, st.get("justify-content")))
        ai = ALIGN_ITEMS.get(st.get("align-items", "stretch"))
        if ai is None:
            raise CssError("%s: 不支持的 align-items %r" % (where, st.get("align-items")))
        wrap_txt = st.get("flex-wrap", "nowrap")
        if wrap_txt not in ("nowrap", "wrap"):
            raise CssError("%s: 不支持的 flex-wrap %r（只支持 nowrap/wrap）"
                           % (where, wrap_txt))
        wrap = 1 if wrap_txt == "wrap" else 0
        _, gap_r = parse_number(st.get("row-gap", "0"), where)
        _, gap_c = parse_number(st.get("column-gap", "0"), where)

        # ---- flex 项 ----
        fg_txt = st.get("flex-grow", "0")
        fs_txt = st.get("flex-shrink", "1")
        try:
            grow = int(round(float(fg_txt) * 100))
            shrink = int(round(float(fs_txt) * 100))
        except ValueError:
            raise CssError("%s: flex-grow/flex-shrink 必须是数字" % where)
        if grow < 0 or shrink < 0:
            raise CssError("%s: flex-grow/flex-shrink 不能为负" % where)

        basis_txt = st.get("flex-basis", "auto")
        bk, bv = parse_number(basis_txt, where, allow_percent=True, allow_auto=True)
        if bk == "auto":
            basis_px, basis_pct = -1, 0
        elif bk == "percent":
            basis_px, basis_pct = 0, max(1, bv)
        else:
            basis_px, basis_pct = bv, 0

        # ---- 定位 ----
        pos = POSITIONS.get(st.get("position", "static"))
        if pos is None:
            raise CssError("%s: 不支持的 position %r" % (where, st.get("position")))
        ov = OVERFLOWS.get(st.get("overflow", "visible"))
        if ov is None:
            raise CssError("%s: 不支持的 overflow %r" % (where, st.get("overflow")))
        try:
            zidx = int(st.get("z-index", "0"))
        except ValueError:
            raise CssError("%s: z-index 必须是整数" % where)
        if zidx < -32768 or zidx > 32767:
            raise CssError("%s: z-index 超出 int16 范围" % where)

        inset = []
        for side in SIDES:          # top, right, bottom, left
            key = st.get(side, "auto")
            k, v = parse_number(key, where, allow_percent=True, allow_auto=True)
            if k == "auto":
                inset.append((0, "QL_UI_UNIT_AUTO"))
            elif k == "percent":
                inset.append((max(0, v), "QL_UI_UNIT_PERCENT"))
            else:
                inset.append((v, "QL_UI_UNIT_PX"))

        lines.append(
            "      .bg_grad_dir = %d, .shadow_blur = %d, .shadow_alpha = %d,"
            % (grad_dir, sh_blur, sh_a))
        lines.append(
            "      .flex_direction = %s, .justify_content = %s,"
            % (fdir, jc))
        lines.append(
            "      .align_items = %s, .flex_wrap = %d, .gap_row = %d, .gap_col = %d,"
            % (ai, wrap, gap_r, gap_c))
        lines.append(
            "      .flex_grow = %d, .flex_shrink = %d,"
            " .flex_basis = %d, .flex_basis_pct = %d,"
            % (grow, shrink, basis_px, basis_pct))
        lines.append(
            "      .position = %s, .overflow = %s, .z_index = %d,"
            % (pos, ov, zidx))
        # ---- 过渡 ----
        trans_ms = 0
        trans_ease = "QL_EASE_LINEAR"
        trans_props = 0
        tdur = st.get("transition-duration")
        if tdur is not None:
            m = re.fullmatch(r"([0-9.]+)(ms|s)", tdur.strip().lower())
            if not m:
                raise CssError("%s: transition-duration 需要 ms/s 单位（%r）"
                               % (where, tdur))
            val = float(m.group(1))
            trans_ms = int(round(val * (1000.0 if m.group(2) == "s" else 1.0)))
            if trans_ms < 0 or trans_ms > 65535:
                raise CssError("%s: transition-duration 超出范围（%r）" % (where, tdur))
            tprop = st.get("transition-property", "all").strip().lower()
            tprops = {"all": 127, "background-color": 1, "opacity": 2,
                      "color": 4, "border-color": 8, "border-radius": 16,
                      "transform": 32, "clip-path": 64}
            if tprop not in tprops:
                raise CssError("%s: 暂不支持的 transition-property %r"
                               "（可用 all / background-color / opacity / color /"
                               " border-color / border-radius / transform /"
                               " clip-path）" % (where, tprop))
            trans_props = tprops[tprop]
            ttf = st.get("transition-timing-function", "ease").strip().lower()
            if ttf not in EASING_NAMES:
                raise CssError("%s: 暂不支持的 transition-timing-function %r"
                               % (where, ttf))
            trans_ease = EASING_NAMES[ttf]

        lines.append(
            "      .trans_ms = %d, .trans_ease = %s, .trans_props = %d,"
            % (trans_ms, trans_ease, trans_props))
        # 毛玻璃：backdrop-filter: blur(Npx) / none
        backdrop_blur = 0
        bf = st.get("backdrop-filter")
        if bf is not None:
            bft = re.sub(r"\s+", " ", bf.strip().lower())
            if bft != "none":
                m = re.fullmatch(r"blur\(([0-9.]+)px\)", bft)
                if not m:
                    raise CssError("%s: backdrop-filter 只支持 blur(<N>px)（收到 %r）"
                                   % (where, bf))
                backdrop_blur = int(round(float(m.group(1))))
                if backdrop_blur < 1 or backdrop_blur > 64:
                    raise CssError("%s: backdrop-filter: blur 半径必须在 1~64px"
                                   % where)
        lines.append("      .backdrop_blur = %d," % backdrop_blur)
        clip_kind, clip_r = parse_clip_path(st.get("clip-path", "none"), where)
        tf_x, tf_y, tf_scx, tf_scy = parse_transform(
            st.get("transform", "none"), where)
        anim_idx = anim_index_of(st, where)

        # 动画里动 clip-path 时，基础样式也要把 clip_kind 打开，
        # 否则运行期根本不会去建遮罩（clip_r 由每一帧写进 run->style）。
        if clip_kind == 0 and anim_idx != 0xFFFF:
            a = anim_src[anim_idx]
            for k in range(a["first"], a["first"] + a["count"]):
                if (kf_frames[k]["props"] & 0x40) != 0:
                    clip_kind = 1
                    clip_r = kf_frames[k]["clip"] if kf_frames[k]["clip"] > 0 else 1000
                    break
        lines.append("      .clip_kind = %d, .clip_r = %d," % (clip_kind, clip_r))
        lines.append("      .tf_x = %d, .tf_y = %d, .sc_x = %d, .sc_y = %d,"
                     % (tf_x, tf_y, tf_scx, tf_scy))
        lines.append("      .anim = %s," % hex16(anim_idx))
        lines.append(
            "      .inset = { { %d, %s }, { %d, %s }, { %d, %s }, { %d, %s } } },"
            % (inset[0][0], inset[0][1], inset[1][0], inset[1][1],
               inset[2][0], inset[2][1], inset[3][0], inset[3][1]))
    lines.append("};")
    lines.append("")
    lines.insert(font_sizes_line,
                 "static const uint16_t %s_font_sizes[] = { %s };"
                 % (ident, ", ".join(str(v) for v in FONT_SIZES) or "0"))

    # 文本池：直接生成 C 字符串字面量。
    # 非 ASCII 字节一律用三位八进制转义（\ooo），既有固定长度不会吃掉后面的
    # 字符，也不会触发 -Wconversion（数字数组写法会把 >127 的字节当成 int）。
    def c_string(data):
        out = []
        for b in data:
            if b == 0x22:
                out.append('\\"')
            elif b == 0x5C:
                out.append('\\\\')
            elif 0x20 <= b < 0x7F:
                out.append(chr(b))
            else:
                out.append("\\%03o" % b)
        return "".join(out)

    if text_pool:
        lines.append('static const char %s_text[] = "%s";'
                     % (ident, c_string(text_pool)))
        lines.append("static const uint16_t %s_text_ofs[] = { %s };"
                     % (ident, ", ".join(str(o) for o in text_offsets)))
    else:
        lines.append('static const char %s_text[] = "";' % ident)
        lines.append('static const uint16_t %s_text_ofs[] = { 0 };' % ident)
    lines.append("")

    # 关键帧表。空表也要给一条占位（C 不允许零长数组），count 单独写 0
    if kf_frames:
        lines.append("static const QLUIKeyframe %s_keyframes[] = {" % ident)
        lines.append("    /* offset  ease                  props bg_color bg_color2 text_c"
                     " border_c  tf_x tf_y   scx  scy radius  clip alpha */")
        for f in kf_frames:
            ease = ("QL_UI_KF_EASE_INHERIT" if f["ease"] == KF_EASE_DEFAULT
                    else f["ease"])
            lines.append("    { %4d, %-21s, 0x%02X, 0x%06X, 0x%06X, 0x%06X, 0x%06X,"
                         " %4d, %4d, %5d, %5d, %5d, %5d, %3d, 0 },"
                         % (f["offset"], ease, f["props"], f["bg"], f["bg2"],
                            f["text"], f["border"], f["tf_x"], f["tf_y"], f["sc_x"], f["sc_y"],
                            f["radius"], f["clip"], f["alpha"]))
        lines.append("};")
    else:
        lines.append("static const QLUIKeyframe %s_keyframes[1] = {"
                     " { 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, 0, 0, 0 } };" % ident)
    lines.append("")

    if anim_src:
        lines.append("static const QLUIAnimation %s_anims[] = {" % ident)
        lines.append("    /* first count dur_ms ease        iterations direction"
                     "                    fill                     delay */")
        for a in anim_src:
            lines.append("    { %5d, %5d, %6d, %-11s, %10d, %-26s, %-22s, %5d },"
                         % (a["first"], a["count"], a["dur"], a["ease"], a["iters"],
                            a["direction"], a["fill"], a["delay"]))
        lines.append("};")
    else:
        lines.append("static const QLUIAnimation %s_anims[1] = {"
                     " { 0, 0, 0, 0, 0, 0, 0, 0 } };" % ident)
    lines.append("")

    lines.append("const QLUIDoc %s_doc = {" % ident)
    lines.append('    .name         = "%s",' % name)
    lines.append("    .node_count   = (uint16_t)(sizeof(%s_nodes) / sizeof(%s_nodes[0])),"
                 % (ident, ident))
    lines.append("    .style_count  = (uint16_t)(sizeof(%s_styles) / sizeof(%s_styles[0])),"
                 % (ident, ident))
    lines.append("    .text_count   = (uint16_t)(sizeof(%s_text_ofs) / sizeof(%s_text_ofs[0])),"
                 % (ident, ident))
    lines.append("    .font_size_count = (uint16_t)(sizeof(%s_font_sizes) / sizeof(%s_font_sizes[0])),"
                 % (ident, ident))
    lines.append("    .anim_count   = %d," % len(anim_src))
    lines.append("    .keyframe_count = %d," % len(kf_frames))
    lines.append("    .font_sizes   = %s_font_sizes," % ident)
    lines.append("    .anims        = %s_anims," % ident)
    lines.append("    .keyframes    = %s_keyframes," % ident)
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

    # 给宿主工具 qlblob 用的访问器：blob 的序列化格式只在 C 侧实现一次，
    # Python 这里只负责"把生成的 doc 交给工具"，不重复实现二进制布局。
    blob_helper = [
        "/* 本文件由 tools/qlui/qlui.py 生成，请勿手工修改。 */",
        "/* 仅供宿主工具 tools/qlblob 使用，不参与目标机构建。 */",
        '#include "%s.h"' % ident,
        "",
        "const QLUIDoc *qlui_blob_doc(void);",
        "",
        "const QLUIDoc *qlui_blob_doc(void)",
        "{",
        "    return &%s_doc;" % ident,
        "}",
        "",
    ]

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "%s.c" % ident), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    with open(os.path.join(out_dir, "%s.h" % ident), "w", encoding="utf-8") as f:
        f.write("\n".join(header))
    with open(os.path.join(out_dir, "%s_blob.c" % ident), "w", encoding="utf-8") as f:
        f.write("\n".join(blob_helper))
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
        rules, keyframes = parse_css(css, os.path.basename(args.css))

        nodes, styles, text_bytes = emit(doc_root, rules, name, args.out, keyframes)
        out_name = name if name.startswith("ui_") else "ui_" + name
        print("qlui: %s -> %s/%s.c（%d 节点，%d 样式，%d 字节文本）"
              % (name, args.out, out_name, nodes, styles, text_bytes))
        return 0
    except CssError as e:
        print("qlui: 错误：%s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
