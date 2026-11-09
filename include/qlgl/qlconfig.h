/**
 * @file    qlconfig.h
 * @brief   QLGL 编译期配置。所有容量/裁剪开关集中在这里，目标平台可覆盖。
 */
#ifndef QLGL_CONFIG_H
#define QLGL_CONFIG_H

/* 类表容量（静态数组，不参与堆分配） */
#ifndef QL_CFG_MAX_CLASS
#define QL_CFG_MAX_CLASS            64
#endif

/* 继承链深度上限，注册超深类会被拒绝 */
#ifndef QL_CFG_MAX_INHERIT_DEPTH
#define QL_CFG_MAX_INHERIT_DEPTH    8
#endif

/* 每类属性 id 预留数量（规范要求"大余量"，见代码规范第 11 行） */
#ifndef QL_CFG_PROPERTY_CAPACITY
#define QL_CFG_PROPERTY_CAPACITY    100
#endif

/* 每类事件 id 预留数量 */
#ifndef QL_CFG_EVENT_CAPACITY
#define QL_CFG_EVENT_CAPACITY       100
#endif

/* 单次事件最多携带的参数个数 */
#ifndef QL_CFG_EVENT_ARG_MAX
#define QL_CFG_EVENT_ARG_MAX        4
#endif

/* 全局事件连接池容量（运行期零 malloc 的关键） */
#ifndef QL_CFG_EVENT_CONN_POOL
#define QL_CFG_EVENT_CONN_POOL      256
#endif

/* 事件传播路径上限（当前对象到根的最大层数） */
#ifndef QL_CFG_EVENT_PATH_MAX
#define QL_CFG_EVENT_PATH_MAX       8
#endif

/*
 * flex 容器一次布局能处理的最大子项数。
 * 超过则退回 block 流并告警（宁可版面退化，也不做不可预测的行为）。
 * 实现不分配数组，这个上限只用于"是否降级"的判断。
 */
#ifndef QL_CFG_UI_FLEX_MAX_ITEMS
#define QL_CFG_UI_FLEX_MAX_ITEMS    256
#endif

/*
 * .qlui blob 加载上限（防止把手工构造/损坏的文件当合法数据吃进来）。
 * 静态编译的 doc 不受这些限制影响——它们是编译器保证的常量。
 * MCU 上应该按实际页面调小，例如 256 / 256。
 */
#ifndef QL_CFG_UI_MAX_NODES
#define QL_CFG_UI_MAX_NODES         4096
#endif
#ifndef QL_CFG_UI_MAX_STYLES
#define QL_CFG_UI_MAX_STYLES        4096
#endif
#ifndef QL_CFG_UI_MAX_TEXT_BYTES
#define QL_CFG_UI_MAX_TEXT_BYTES    (64u * 1024u)   /* text_offsets 是 uint16 的自然上限 */
#endif
#ifndef QL_CFG_UI_MAX_FONT_SIZES
#define QL_CFG_UI_MAX_FONT_SIZES    256
#endif
#ifndef QL_CFG_UI_MAX_ANIMS
#define QL_CFG_UI_MAX_ANIMS         256     /* @keyframes 段数 */
#endif
#ifndef QL_CFG_UI_MAX_KEYFRAMES
#define QL_CFG_UI_MAX_KEYFRAMES     4096    /* 所有段的关键帧总数 */
#endif

/*
 * HTML/CSS 文本解析（`ql_ui_load_text`）的上限。
 * 这是全库**唯一**吃外部文本的地方，所有计数与递归都必须有界：
 * 一份恶意/损坏的 CSS 不该能把栈撑爆或把堆吃光。
 */
#ifndef QL_CFG_UI_MAX_DEPTH
#define QL_CFG_UI_MAX_DEPTH         32      /* 元素嵌套深度 */
#endif
#ifndef QL_CFG_UI_MAX_CSS_RULES
#define QL_CFG_UI_MAX_CSS_RULES     2048
#endif
#ifndef QL_CFG_UI_MAX_DECLS
#define QL_CFG_UI_MAX_DECLS         128     /* 单条规则/单个元素的声明数 */
#endif
#ifndef QL_CFG_UI_MAX_SEL_PARTS
#define QL_CFG_UI_MAX_SEL_PARTS     16      /* 复合选择器的段数 */
#endif
#ifndef QL_CFG_UI_MAX_TEXT_NODES
#define QL_CFG_UI_MAX_TEXT_NODES    4096    /* 去重后的文本条数 */
#endif
/*
 * 输入框（`<input>`）：
 *   QL_CFG_UI_INPUT_MAX  一个有输入的界面上最多几个输入框；
 *   QL_CFG_UI_INPUT_LEN  单个输入框的长度上限（UTF-8 字节，含结尾 0）。
 * 池只在 doc 里真的出现 `<input>` 时才分配。
 */
/*
 * ---- HTML5 能力包开关（见 doc/手册/ui/HTML5标签覆盖与裁剪.md） ----
 *
 * 一个"包"= 一组标签 + 支撑它们的引擎部分。PC/移动默认全开，MCU 按需关掉。
 *
 * **硬约束**：关包只裁代码路径与错误文案，**不许改 QLUITag 枚举**
 * （枚举值写进 blob，一旦按平台变，同一份 .qlui 两边就解释成不同标签）。
 *
 * 目前只有 META 一个包落地，其余（TEXT/TABLE/IMAGE/WIDGET/EMBED）随实现追加。
 */
#ifndef QL_FEAT_UI_META
#define QL_FEAT_UI_META             1   /* base head link meta style title：解析期跳过 */
#endif

/*
 * 未知/未启用标签的处理方式：
 *   0（默认）= strict，明确报错并点名缺哪个包；
 *   1        = lenient，降级成 div/span（元数据直接丢掉），并计入 doc->degraded_tags。
 *
 * 默认 strict：降级只允许发生在"你明确说要降级"的时候。
 */
#ifndef QL_CFG_UI_LENIENT_TAGS
#define QL_CFG_UI_LENIENT_TAGS      0
#endif

#ifndef QL_CFG_UI_INPUT_MAX
#define QL_CFG_UI_INPUT_MAX         8
#endif
#ifndef QL_CFG_UI_INPUT_LEN
#define QL_CFG_UI_INPUT_LEN         96
#endif
/*
 * 多行输入（`<textarea>`）：最多几个、单个最多多少字节。
 * 与单行输入**分池**：单行框多、缓冲小；多行框少、缓冲大。混在一起会让
 * 8 个单行框各自多背 400 字节（MCU 上不能接受）。
 * 池只在 doc 里真的出现 `<textarea>` 时才分配。
 */
#ifndef QL_CFG_UI_TEXTAREA_MAX
#define QL_CFG_UI_TEXTAREA_MAX      2
#endif
/*
 * 滚轮"一格"滚多少像素。
 *
 * `QLInputEvent.dy` 是**格数**（X11 的 Button4/5 一次就是一格），不是像素 ——
 * 以前直接当成像素用了，于是转一格才动 1px，得把滚轮转飞才看得见一行。
 *
 * 默认 50 ≈ 3 行（16px 行高），与 Firefox 的 "3 lines per notch" 同量级；
 * Chrome 是 100px。想要更快/更慢改这一个数就行。
 */
#ifndef QL_CFG_UI_WHEEL_STEP_PX
#define QL_CFG_UI_WHEEL_STEP_PX      50
#endif

#ifndef QL_CFG_UI_TEXTAREA_LEN
#define QL_CFG_UI_TEXTAREA_LEN      512
#endif
/* 下拉框：最多几个、面板最高多少像素（超出就滚动） */
/*
 * ---- 图像（`<img>`）----
 *
 * **不分平台，只分开关**：每个能力一个宏、互不互斥、组合都合法。
 * `QLGL_IMG_PRESET=full|lean|raw-only` 只是把常用组合打包（见 CMake），
 * 单品的 -D 永远优先。设计见 doc/图像管线设计.md §2。
 */
#ifndef QL_FEAT_UI_IMAGE
#define QL_FEAT_UI_IMAGE            1   /* <img> 标签与整条图像管线 */
#endif
/* 预解码位图（RAW）永久可用：它是零成本路径，关不掉 */
#ifndef QL_FEAT_IMG_BMP
#define QL_FEAT_IMG_BMP             1
#endif
#ifndef QL_FEAT_IMG_QOI
#define QL_FEAT_IMG_QOI             1
#endif
#ifndef QL_FEAT_IMG_PNG
#define QL_FEAT_IMG_PNG             1
#endif
#ifndef QL_FEAT_IMG_INFLATE
#define QL_FEAT_IMG_INFLATE         QL_FEAT_IMG_PNG
#endif
#ifndef QL_FEAT_IMG_GIF
#define QL_FEAT_IMG_GIF             1   /* GIF **首帧**（自有 LZW，不解动画） */
#endif
#ifndef QL_FEAT_IMG_PLATFORM
#define QL_FEAT_IMG_PLATFORM        1   /* 平台后端接口（JPEG/GIF…）：有就给，没有就报错 */
#endif
#ifndef QL_FEAT_IMG_LIBJPEG
#define QL_FEAT_IMG_LIBJPEG         QL_FEAT_IMG_PLATFORM   /* libjpeg-turbo 参考实现 */
#endif

#ifndef QL_CFG_UI_IMAGE_MAX
#define QL_CFG_UI_IMAGE_MAX         8       /* 一个界面最多几张图 */
#endif
#ifndef QL_CFG_UI_AREA_MAX
#define QL_CFG_UI_AREA_MAX          16      /* 一个界面最多几个 <area> 热区（0 = 整套裁掉） */
#endif
#ifndef QL_CFG_IMAGE_MAX_BYTES
#define QL_CFG_IMAGE_MAX_BYTES      (512u * 1024u)   /* 单张编码字节上限 */
#endif
#ifndef QL_CFG_IMAGE_MAX_PIXELS
#define QL_CFG_IMAGE_MAX_PIXELS     (4096u * 4096u)  /* 解码后像素数上限 */
#endif
#ifndef QL_CFG_IMAGE_BILINEAR
#define QL_CFG_IMAGE_BILINEAR       0       /* 0 = 最近邻（省），1 = 双线性 */
#endif
#ifndef QL_CFG_IMAGE_PLACEHOLDER
#define QL_CFG_IMAGE_PLACEHOLDER    1       /* 解码失败时画个占位框 */
#endif

#ifndef QL_CFG_UI_PROGRESS_MAX
#define QL_CFG_UI_PROGRESS_MAX      4       /* 一个界面上最多几个 <progress>/<meter> */
#endif

#ifndef QL_CFG_UI_SELECT_MAX
#define QL_CFG_UI_SELECT_MAX        8
#endif
#ifndef QL_CFG_UI_SELECT_PANEL_H
#define QL_CFG_UI_SELECT_PANEL_H    160
#endif

/*
 * 一次绘制最多排多少行（`white-space: normal` 折行后）。
 * 绘制时在栈上开 `QL_CFG_TEXT_MAX_LINES` 条行记录，超出的行不画——
 * 布局统计总行数时不受这个上限影响。64 行 ≈ 45 行/屏的两屏多。
 */
#ifndef QL_CFG_TEXT_MAX_LINES
#define QL_CFG_TEXT_MAX_LINES       64
#endif

/* 输入事件队列容量（后端只入队，主循环统一派发；静态分配不 malloc） */
#ifndef QL_CFG_INPUT_QUEUE
#define QL_CFG_INPUT_QUEUE          64
#endif

/* 渲染器同时跟踪的脏矩形数量上限；满了会合并成一块 */
#ifndef QL_CFG_DIRTY_RECT_MAX
#define QL_CFG_DIRTY_RECT_MAX       16
#endif

/* 内存统计与越界校验：MCU 发布版可关掉以省 8 字节/次分配 */
#ifndef QL_CFG_MEM_TRACK
#define QL_CFG_MEM_TRACK            1
#endif

/* 日志级别：0=关闭 1=错误 2=警告 3=信息 4=调试 */
#ifndef QL_CFG_LOG_LEVEL
#define QL_CFG_LOG_LEVEL            2
#endif

#endif /* QLGL_CONFIG_H */
