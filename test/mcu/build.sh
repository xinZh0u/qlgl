#!/bin/sh
# 交叉编译 + 在 QEMU 里跑 MCU 程序。
#
#   ./test/mcu/build.sh              # 基准（默认）：编译并运行
#   ./test/mcu/build.sh window       # QLWindow 验收：板级平台 + 触摸 + 脏区刷屏
#   ./test/mcu/build.sh --no-run     # 只编译（看 ROM/RAM 占用）
#   ./test/mcu/build.sh window --no-run
#
# 参数通过环境变量覆盖：
#   CPU=cortex-m4  BOARD=mps2-an386  FREQ_MHZ=200
#   QL_FEAT_CSS_FILTER_LAYER=0|1   离屏图层（MCU 默认关：它要按元素尺寸吃 RAM）
#   QL_FEAT_CSS_TABLE=0|1          表格（MCU 默认关：B7 的数据表格不在小核上跑）
#   QL_FEAT_CSS_COLUMNS=0|1        多栏（MCU 默认关：B7 后半，与表格同一个大核包）
#   QL_FEAT_CSS_FONT_STACK=0|1     文本栈 @font-face / 家族列表（MCU 恒关：要整份 TTF）
#   QL_FEAT_CSS_TEXT_SHAPE=0|1     整形 HarfBuzz（MCU 恒关：库比整块固件还大）
#   QL_FEAT_CSS_BIDI=0|1           双向 fribidi（MCU 恒关）
#   EXTRA_CFLAGS="..."             额外的 -D / 编译选项（量体积用）
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/build/mcu}
CPU=${CPU:-cortex-m4}
BOARD=${BOARD:-mps2-an386}
FREQ_MHZ=${FREQ_MHZ:-200}
CROSS=${CROSS:-arm-none-eabi-}
QLTEXT=${QLTEXT:-$ROOT/build/qltext}

# 目标程序：bench（默认，量布局/光栅）或 window（QLWindow 验收）
TARGET=bench
NO_RUN=0
for arg in "$@"; do
    case "$arg" in
        --no-run) NO_RUN=1 ;;
        bench|window) TARGET=$arg ;;
        *) echo "未知参数：$arg" >&2; exit 2 ;;
    esac
done

case "$TARGET" in
bench)
    PAGE_HTML=$ROOT/test/mcu/bench.html
    PAGE_CSS=$ROOT/test/mcu/bench.css
    PAGE_NAME=mcu_bench
    APP_SRCS="$ROOT/test/mcu/bench.c"
    PORT_SRCS=""
    ELF=bench.elf
    ;;
window)
    PAGE_HTML=$ROOT/test/mcu/window.html
    PAGE_CSS=$ROOT/test/mcu/window.css
    PAGE_NAME=mcu_window
    APP_SRCS="$ROOT/test/mcu/window.c $ROOT/test/mcu/qlplatform_board.c"
    # 窗口层 + 平台注册表：这两个文件就是"PC 上 QLWindow 的 MCU 版"
    PORT_SRCS="$ROOT/src/port/qlplatform.c $ROOT/src/port/qlwindow.c"
    ELF=window.elf
    ;;
esac

mkdir -p "$OUT"

# 1) 页面 → 静态 C（构建期一次，运行期不解析 HTML/CSS）
"$QLTEXT" cc --html "$PAGE_HTML" --css "$PAGE_CSS" \
    --name "$PAGE_NAME" --out "$OUT" >/dev/null

SRCS="$APP_SRCS $PORT_SRCS $ROOT/test/mcu/startup.c $ROOT/test/mcu/syscalls.c \
      $OUT/ui_${PAGE_NAME}.c \
      $ROOT/src/core/qlmem.c $ROOT/src/core/qlgl.c $ROOT/src/core/qlanim.c \
      $ROOT/src/core/qlinput.c $ROOT/src/core/qlstr.c $ROOT/src/core/qlvalue.c \
      $ROOT/src/core/qlclass.c $ROOT/src/core/qlobject.c $ROOT/src/core/qlevent.c \
      $ROOT/src/core/qlprop.c \
      $ROOT/src/ui/qlui_blob.c $ROOT/src/generated/qlclass_gen.c \
      $ROOT/src/draw/qlcanvas.c $ROOT/src/draw/qldraw2d.c $ROOT/src/draw/qllayer.c \
      $ROOT/src/draw/qlgeom.c \
      $ROOT/src/draw/qldisp.c $ROOT/src/draw/qlimage.c $ROOT/src/draw/qlinflate.c \
      $ROOT/src/text/qlfont.c $ROOT/src/text/qlfont_ascii.c \
      $ROOT/src/text/qltextshape.c $ROOT/src/text/qltextbidi.c \
      $ROOT/src/core/qldl.c \
      $ROOT/src/ui/qlui.c"

CFLAGS="-mcpu=$CPU -mthumb -O2 -g -std=c11 -ffreestanding -fno-common
        -ffunction-sections -fdata-sections
        -I$ROOT/include -I$ROOT/test/mcu -I$OUT
        -DQL_CFG_UI_MAX_NODES=256 -DQL_CFG_UI_MAX_STYLES=256
        -DQL_CFG_UI_FLEX_MAX_ITEMS=64
        -DQL_FEAT_CSS_FILTER_LAYER=${QL_FEAT_CSS_FILTER_LAYER:-0}
        -DQL_FEAT_CSS_TABLE=${QL_FEAT_CSS_TABLE:-0}
        -DQL_FEAT_CSS_COLUMNS=${QL_FEAT_CSS_COLUMNS:-0}
        -DQL_FEAT_CSS_FONT_STACK=${QL_FEAT_CSS_FONT_STACK:-0}
        -DQL_FEAT_CSS_TEXT_SHAPE=${QL_FEAT_CSS_TEXT_SHAPE:-0}
        -DQL_FEAT_CSS_BIDI=${QL_FEAT_CSS_BIDI:-0}
        ${EXTRA_CFLAGS:-}"

# shellcheck disable=SC2086
"${CROSS}gcc" $CFLAGS -T "$ROOT/test/mcu/link.ld" \
    --specs=nano.specs --specs=nosys.specs \
    -Wl,--gc-sections -Wl,-Map="$OUT/${ELF%.elf}.map" \
    -o "$OUT/$ELF" $SRCS -lm

"${CROSS}size" "$OUT/$ELF"

if [ "$NO_RUN" = 1 ]; then
    exit 0
fi

# 2) 跑：`-icount shift=0,align=off,sleep=off` 让虚拟时钟按"每条指令 1ns"推进。
#    于是 SYS_ELAPSED 返回的纳秒数 ≈ 指令数，也就是 1 IPC 参考机上的周期数。
#    `align=off,sleep=off` 是必须的：默认对齐会让 QEMU 等虚拟时钟，跑得极慢。
qemu-system-arm -M "$BOARD" -cpu "$CPU" -nographic \
    -semihosting-config enable=on,target=native \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/$ELF"
