# 液态玻璃：`blur` + `saturate` + `refract` 的玻璃圆盘

<img src="../img/38-liquid-glass.png" alt="水彩背景上的玻璃圆盘：边缘一圈折射，中间一个白色加号" width="630">

完整程序：`example/38_liquid_glass/`（对照稿：codepen.io/Mikhail-Bespalov/pen/MYwrMNy）

> 直接看效果：`./build/example/example_38_liquid_glass`
>
> - **悬停**圆盘：折射半径 14px → 26px（边缘那圈"厚度"变厚），同时整体放大 2%、白底变亮
> - **点一下**圆盘：下面那行字换一次

---

## 1. 参考稿的全部秘密就四个数值

| # | 数值 | 作用 |
|---|---|---|
| ① | `background-color: rgba(255,255,255,.08)` | 玻璃本身几乎不挡光 —— **这一个 alpha 决定"透不透"** |
| ② | `border: 2px solid rgba(255,255,255,.6)` | 一圈亮边，玻璃的"截面" |
| ③ | `box-shadow: 0 16px 32px rgba(0,0,0,.12)` | 很淡的投影，玻璃于是"浮"在背景上 |
| ④ | `backdrop-filter: blur(6px) saturate(140%) refract(14px)` | 毛面 + 提色 + **边缘折射** |

```css
.glass {
  background-color: rgba(255, 255, 255, 0.08);
  border: 2px solid rgba(255, 255, 255, 0.6);
  box-shadow: 0 16px 32px rgba(0, 0, 0, 0.12);
  backdrop-filter: blur(6px) saturate(140%) refract(14px);
}
```

> ① 是最容易写错的地方：alpha 拉到 0.2 以上立刻变成"白塑料"，0.08 才是玻璃。

## 2. `refract()`：参考稿里那个 `feDisplacementMap`

参考稿的滤镜链是 **`feGaussianBlur` + `feDisplacementMap`**：先轻度模糊，再按一张法线
图把像素**位移**。位移集中在轮廓附近，于是玻璃边缘看起来像一块有厚度的透镜。

本库把这两步写成两个函数，语义一一对应：

| 参考稿 | 本库 |
|---|---|
| `feGaussianBlur stdDeviation≈4` | `blur(<N>px)` |
| `feDisplacementMap` | `refract(<N>px)` |
| 饱和调整 | `saturate(<N>%)` |

**`refract(N)` 的语义**：越靠近轮廓，采样点沿**法线方向往外推**，最多 N px；圆心处
完全不推。所以圆盘看起来是"把外面的颜色折进来了"。

- 半径怎么选：大约是**亮边宽度的 5~10 倍**。例程的圆盘 300px，取 14px；悬停时 26px。
- 只写 `refract()` 也可以 —— 那就是一块干净的放大镜；只写 `blur()` 就是普通毛玻璃。
  两者一起写才是"液态"。
- 三个函数任意顺序、可以只写其中一部分；写其它函数（`brightness()` 等）**构建期报错**。

## 3. 为什么 `blur()` 只有 6px

模糊的目的是"看不清细节"，不是"看不见东西"。半径一大，背景就被糊成一团均值色，
玻璃立刻变灰塑料。参考稿只有 ~4px，这里取 6px。

**厚度感来自 `refract()`，不是来自模糊半径。**

## 4. 背后那张"水彩"是程序算出来的

```css
#root { background-image: url(watercolor); background-size: cover; }
```

`url(watercolor)` 不是文件，而是 `ql_ui_register_image()` 在运行期注册的一张位图
（`main.c` 的 `build_watercolor()`）：

- 11 个色斑（暖橙 → 紫蓝）+ 18 个小花瓣（白/黄/粉）+ 一层纸纹；
- 纸纹用整数 LCG，**不依赖 `rand()`**，跨平台逐像素一致；
- 花瓣边缘只有 ~1.5px 的过渡 —— 它们就是**模糊与折射的试纸**：糊多了会消失，
  折射不够会看不出弯折。

`.scene` 里那 5 团慢慢漂移的薄雾是 `radial-gradient` + `transform` 动画，
**它们不模糊** —— 糊不糊是玻璃自己的事（图层数只有一层）。

## 5. 实现上必须做对的三件事

1. **采样范围要比盒子大一圈**（`radius + refract`）。模糊核本来就要读盒子外面：
   只糊盒内、边缘靠钳位补的话，得到的是"这块方形的均值色"，半径一大就发灰，
   四边还会留一圈方形的边。
2. **结果只写进轮廓之内**（按圆角覆盖率与原像素混合），轮廓外一个像素都不动 ——
   否则四个圆角缺口里会渗出"方的模糊块"。
3. **三段盒式卷积近似高斯**。两段是三角核，边缘有一道可见的折线；三段才接近钟形。

CPU 与 GL 两个后端走同一套参数与顺序，`test_gl_vs_cpu` 对着色器做逐像素对拍。

## 6. 限制（都是明确报错，不静默降级）

| 限制 | 说明 |
|---|---|
| 函数集合 | 只有 `blur()` / `saturate()` / `refract()`，其它函数构建期报错 |
| 不可过渡 | `backdrop-filter` **不在**可过渡属性列表里 → 悬停时折射半径是**瞬变**，不是渐变 |
| 平台档 | 属于 `QL_FEAT_CSS_BLUR`（PC 档特性）；MCU 档恒关 |
| 层数 | 玻璃套玻璃天然可用（先采样"已画好的内容"，再画自己），但每多一层就多一次模糊 |

## 7. 性能（本机 720×460、GL 后端、240 帧）

| 项 | 数值 |
|---|---|
| 帧时间 p50 / p95 | **16.62 ms** / 17.68 ms |
| 其中等 swap | 16.33 ms（几乎全是等垂直同步） |
| 实际渲染 | 约 **0.3 ms** |

也就是：一帧的预算几乎全花在 vsync 上，真正画一张"水彩 + 折射玻璃"只要零点几毫秒。
成本主要随**玻璃元素数量**线性增长（每块玻璃一次模糊 + 一次折射位移）。

## 8. 写玻璃时踩过的三个坑（都已修 / 都已记录）

### 8.1 渐变的两个色标共用一个 alpha（**已修**）

以前一个背景只有**一个** alpha：`rgba()` 写在渐变色标里会被忽略，于是"上亮下透"
的玻璃做不出来。现在两个色标各带自己的 alpha，插值时"颜色 + alpha"一起插。
兼容规则：两个色标都不写 alpha 时沿用 `background-color` 的 alpha。

### 8.2 `box-shadow: inset` + 圆角=半边长 → 四角被涂满（**已修**）

正圆/药丸的圆角正好等于半边长，矩形四角落在轮廓**外面**，而内阴影的覆盖率公式
在那一片恒等于 1 —— 四个角被整片涂成阴影色，看着像"圆外面套了个方框"。
修法是片元里再乘一个"在轮廓内"的覆盖率；回归测试在 `test_gl_vs_cpu`。

### 8.3 `transition` 只认这七个属性

`background-color` / `opacity` / `color` / `border-color` / `border-radius` /
`transform` / `clip-path`。写别的会在**构建期报错**（不静默忽略）。

所以例程的悬停过渡只写了两个：

```css
transition: background-color 220ms ease-out, transform 220ms ease-out;
```
