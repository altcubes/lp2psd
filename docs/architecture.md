# 架构总览

lp2psd 是一个 Windows 命令行工具:读取 [LabelPlus](https://github.com/LabelPlus/LabelPlus) 导出的排版 txt,为其中引用的每张图片生成一个多图层 Photoshop PSD 文件——背景像素图层 + 可编辑文本图层(TySh + EngineData)+ 图层分组 + 文本预览栅格。

## 设计原则

- **零第三方 C++ 依赖**:JSON 解析(`src/minijson.hpp`)、文本编码转换(`src/textcodec.hpp`)、PSD 写入(`src/psd_writer.*`)全部自行实现,只链接系统库 `gdiplus`、`comdlg32`、`shell32`。不需要包管理器。
- **CLI-only**:无 GUI。不带参数运行时弹一次文件选择对话框(双击运行场景),其余全走命令行。
- **业务层 / 字节层分离**:`psd_writer.*` 是通用的 PSD 写入库,不知道 LabelPlus 的存在;`main.cpp` 只负责把业务数据组装成 `psdw::Document` 模型。
- **Windows-only**:使用 `wmain`、GDI+、Win32 文件对话框;路径处理全程走宽字符 API 以支持中文路径。

## 数据流

```
LabelPlus txt (UTF-8/UTF-16/GBK)
        │
        ▼
parse_layout()                    src/layout.cpp:63
        │  Layout { groups, images[] { image, entries[] { index, x, y, group, lines } } }
        ▼                          (坐标为图片宽高的归一化值 0..1)
┌── 对每个 ImageBlock:build_psd()   src/main.cpp:265 ──────────────────────────┐
│                                                                              │
│  load_image()                        src/image.cpp:91                        │
│      → RGBA8 像素 + 图片内嵌 DPI(JPEG JFIF / EXIF)                          │
│                                                                              │
│  确定画布 DPI:config "dpi" 覆盖图片自带 DPI(src/main.cpp:280-289)          │
│                                                                              │
│  [可选] dbnet 检测(config dbnet.enabled):dbnet_detect() 得到旋转四边形 +          │
│      逐像素笔画遮罩 → make_whiten_layer() 默认"笔画外扩∩框外扩"填白       │
│      (limitToBoxes=false 时回退为全 mask 外扩;bg 之上)                     │
│                                                                              │
│  对每个 TextEntry:make_text_layer()  src/main.cpp:157                        │
│      • 展开字面 "\r"/"\n" 转义换行                                            │
│      • px_scale = dpi / 72:字号在 72dpi 点空间,几何在文档像素空间            │
│      • textmetrics::estimate_box() 估算文本框尺寸(与 TySh bounds 共用)      │
│      • render_text_preview() 用 GDI+ 渲染 RGBA 预览并扫描 ink bbox            │
│                                                                              │
│  组装 psdw::Document:                                                        │
│      "bg" 像素图层(整图) → 按分组号建 Group(自下而上堆叠)                   │
│      → 无分组条目直接放顶层                                                   │
│                                                                              │
│  doc.write_wide(out_path)            src/psd_writer.cpp:1573                 │
│      → build_document_bytes():合成图、图层记录、TySh/lrFX 等 tagged blocks、  │
│        PackBits RLE 压缩、图像资源(0x03ED 分辨率)                            │
└──────────────────────────────────────────────────────────────────────────────┘
        │
        ▼
<输出目录>/<图片名去扩展名>.psd
```

## 模块分层

```
┌─────────────────────────────────────────────────────────┐
│ CLI 编排        src/main.cpp                             │
│   参数解析、文件对话框、Document 组装、输出命名            │
├─────────────────────────────────────────────────────────┤
│ 业务模型                                                 │
│   layout.*   LabelPlus txt 解析                          │
│   style.*    config.json 样式配置(含 dbnetConfig)          │
│   image.*    GDI+ 图片加载 / 文本预览渲染 / DPI 读取      │
│   dbnet.*      可选:日文文本区域检测(m-i-t DBNet,ONNX Runtime)│
│              → 旋转四边形 + 笔画遮罩涂白;缺依赖时桩函数降级  │
│              (src/dbnet.hpp)                                │
├─────────────────────────────────────────────────────────┤
│ 通用 PSD 写入库   src/psd_writer.*                       │
│   公开模型:Document / Group / PixelLayer / TextLayer     │
│   字节机器:Buffer / Descriptor / EngineData / RLE        │
│   (public API 在 psd_writer.hpp,内部机制在               │
│    psd_writer_internal.hpp + psd_writer.cpp,             │
│    业务代码不得 include internal 头)                      │
├─────────────────────────────────────────────────────────┤
│ 基础工具                                                 │
│   minijson.hpp  极简 JSON 解析(~175 行)                 │
│   textcodec.hpp UTF-8 / UTF-16 / ANSI(GBK) 转换          │
└─────────────────────────────────────────────────────────┘
```

依赖方向严格自上而下:`main.cpp` 依赖下面所有层;`psd_writer.*` 不依赖任何业务模块;`minijson`/`textcodec` 不被反向依赖。

## 关键坐标与单位模型

这是理解代码最重要的一点,`src/main.cpp:178-186` 有注释:

| 量 | 单位 | 说明 |
|---|---|---|
| 字号、行距 | pt(72dpi 点空间) | 与 Photoshop 字符面板一致的排版单位 |
| 图层矩形、预览栅格、TySh anchor、ink bbox | 文档像素 | `px_scale = dpi / 72`,如 20pt 在 96dpi 下占 26.7px |
| EngineData 里的 FontSize / Leading / bounds | 文档像素(pt × dpi/72) | Photoshop 按文档分辨率读回时显示为原 pt 值(`src/psd_writer.cpp:494-501`) |
| txt 条目坐标 x, y | 归一化 0..1 | 乘以图片宽高得到文档像素 |

三处独立实现但必须保持一致的行宽估算(CJK=1.0em, Latin=0.55em):

- 文本框尺寸:`line_units`,src/main.cpp:142
- TySh em box:`est_line_units`,src/psd_writer.cpp:936(注释明确要求与 main.cpp 同步)
- 竖排列数:按 UTF-16 code units 数字符(src/main.cpp:189-190、src/psd_writer.cpp:132 的 `utf16_length`)

改动排版估算逻辑时,这两份实现必须同步修改,否则 TySh bounds 与图层矩形会错位。

## 输出文件布局

- 输出目录:`<txt 所在目录>\output`,或 `--out` 指定(src/main.cpp:414-419)。
- 输出文件名:`<图片名去扩展名>.psd`(src/main.cpp:433-434),不再支持 config 改名。
- 全部成功后自动用 Explorer 打开输出目录(src/main.cpp:442-446)。
