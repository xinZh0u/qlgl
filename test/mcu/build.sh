#!/bin/sh
# 交叉编译 + 在 QEMU 里跑 MCU 基准。
#
#   ./test/mcu/build.sh              # 编译并运行（默认 mps2-an386 / Cortex-M4）
#   ./test/mcu/build.sh --no-run     # 只编译
#
# 参数通过环境变量覆盖：
#   CPU=cortex-m4  BOARD=mps2-an386  FREQ_MHZ=200
#   QL_FEAT_CSS_FILTER_LAYER=0|1   离屏图层（MCU 默认关：它要按元素尺寸吃 RAM）
#   QL_FEAT_CSS_TABLE=0|1          表格（MCU 默认关：B7 的数据表格不在小核上跑）
#   QL_FEAT_CSS_COLUMNS=0|1        多栏（MCU 默认关：B7 后半，与表格同一个大核包）
#   EXTRA_CFLAGS="..."             额外的 -D / 编译选项（量体积用）
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/build/mcu}
CPU=${CPU:-cortex-m4}
BOARD=${BOARD:-mps2-an386}
FREQ_MHZ=${FREQ_MHZ:-200}
CROSS=${CROSS:-arm-none-eabi-}
QLTEXT=${QLTEXT:-$ROOT/build/qltext}

mkdir -p "$OUT"

# 1) 页面 → 静态 C（构建期一次，运行期不解析 HTML/CSS）
"$QLTEXT" cc --html "$ROOT/test/mcu/bench.html" --css "$ROOT/test/mcu/bench.css" \
    --name mcu_bench --out "$OUT" >/dev/null

SRCS="$ROOT/test/mcu/bench.c $ROOT/test/mcu/startup.c $ROOT/test/mcu/syscalls.c \
      $OUT/ui_mcu_bench.c \
      $ROOT/src/core/qlmem.c $ROOT/src/core/qlgl.c $ROOT/src/core/qlanim.c \
      $ROOT/src/core/qlinput.c $ROOT/src/core/qlstr.c $ROOT/src/core/qlvalue.c \
      $ROOT/src/core/qlclass.c $ROOT/src/core/qlobject.c $ROOT/src/core/qlevent.c \
      $ROOT/src/core/qlprop.c \
      $ROOT/src/ui/qlui_blob.c $ROOT/src/generated/qlclass_gen.c \
      $ROOT/src/draw/qlcanvas.c $ROOT/src/draw/qldraw2d.c $ROOT/src/draw/qllayer.c \
      $ROOT/src/draw/qlgeom.c \
      $ROOT/src/draw/qldisp.c $ROOT/src/draw/qlimage.c $ROOT/src/draw/qlinflate.c \
      $ROOT/src/text/qlfont.c $ROOT/src/text/qlfont_ascii.c \
      $ROOT/src/ui/qlui.c"

CFLAGS="-mcpu=$CPU -mthumb -O2 -g -std=c11 -ffreestanding -fno-common
        -I$ROOT/include -I$OUT
        -DQL_CFG_UI_MAX_NODES=256 -DQL_CFG_UI_MAX_STYLES=256
        -DQL_CFG_UI_FLEX_MAX_ITEMS=64
        -DQL_FEAT_CSS_FILTER_LAYER=${QL_FEAT_CSS_FILTER_LAYER:-0}
        -DQL_FEAT_CSS_TABLE=${QL_FEAT_CSS_TABLE:-0}
        -DQL_FEAT_CSS_COLUMNS=${QL_FEAT_CSS_COLUMNS:-0}
        ${EXTRA_CFLAGS:-}"

# shellcheck disable=SC2086
"${CROSS}gcc" $CFLAGS -T "$ROOT/test/mcu/link.ld" \
    --specs=nano.specs --specs=nosys.specs \
    -Wl,--gc-sections -Wl,-Map="$OUT/bench.map" \
    -o "$OUT/bench.elf" $SRCS -lm

"${CROSS}size" "$OUT/bench.elf"

if [ "$1" = "--no-run" ]; then
    exit 0
fi

# 2) 跑：`-icount shift=0,align=off,sleep=off` 让虚拟时钟按"每条指令 1ns"推进。
#    于是 SYS_ELAPSED 返回的纳秒数 ≈ 指令数，也就是 1 IPC 参考机上的周期数。
#    `align=off,sleep=off` 是必须的：默认对齐会让 QEMU 等虚拟时钟，跑得极慢。
qemu-system-arm -M "$BOARD" -cpu "$CPU" -nographic \
    -semihosting-config enable=on,target=native \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/bench.elf"
