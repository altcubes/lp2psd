# 已知限制与改进方向

## 已知限制

### 1. PS 打开后需手动"更新所有文本图层"(README 顶部标注的核心限制)

原因:EngineData 不含 Photoshop 原生排版引擎的字形网格数据(每个字形的字体索引、位置、宽度),PS 打开时文本引擎缓存过期,提示"文字(Y) → 更新所有文本图层(U)"后自行重排。显示层不受影响(靠 GDI+ 预览栅格),更新后通常只有微小位置差。

详见 [psd-internals.md](psd-internals.md) 的"为什么 PS 需要'更新所有文本图层'"一节。

### 2. 无自动化测试

`build_tysh` / `build_lrfx` 已在 `psd_writer_internal.hpp:107-108` 为测试预留,但测试从未编写。目前回归全靠 `testfile/` 手工样例 + psd-tools 人工对比。

### 3. 行宽估算与 PS 排版有偏差

文本框尺寸、TySh em box 用粗估(CJK=1.0em、Latin=0.55em),不含字距/标点压缩;**两处实现必须同步修改**(main.cpp:142 `line_units` 与 psd_writer.cpp:936 `est_line_units`),这是改代码时最容易踩的坑。

### 4. config 的 `outputDir` 字段无效

`style.cpp:192` 会读取 `outputDir` 到 `Style::output_dir`,但 `main.cpp` 从不使用它(输出目录只由 `--out` / 默认规则决定)。要么实现,要么从文档与代码中移除。

### 5. 平台绑定 Windows

`wmain`、GDI+、Win32 对话框、`CP_ACP` 回退均为 Windows 专属。若要跨平台,需要替换的点是:图片加载/预览渲染(GDI+ → stb_image 或 Skia)、文件对话框、文本编码回退(GBK → iconv)。

### 6. 仅点文本、仅 RGB 8-bit

EngineData `ShapeType` 固定为 0(点文本);不写段落文本(ShapeType 1 + BoxBounds)。文件头固定 RGB 8-bit。对 LabelPlus 排版场景足够,扩展时需动 `build_tysh` / `build_document_bytes`。

### 7. OCR 涂白的固有局限

OCR 涂白([ocr.md](ocr.md))已改为 m-i-t DBNet 的**逐像素笔画遮罩**(只填笔画,不再整块白矩形),深色背景上不再是显眼白块;但纯白填充在深色背景上仍是白笔画,复杂网点/花纹背景仍可能留边。改进方向:背景色取样填充、遮罩形态学优化。漏检的原文不会被涂白(宁漏勿错,`bg` 图层始终未被修改)。

## 历史:Txt2 路线的引入与回退

理解这段历史有助于避免重蹈覆辙(为何 README 说"需要手动更新文本图层"——这正是回退 Txt2 后接受的状态):

| 提交 | 内容 |
|---|---|
| `5f8f1ce` checkpoint: Txt2 fix | 引入 `src/txt2_templates.hpp`(**1311 行**):用截取自真实 PS 文件的二进制模板 + 补丁方式,伪造带完整字形数据的 EngineData,试图让 PS 免手动更新。同批还加了 GUI(`gui_main.cpp`、`windows_ui.hpp`)和两个 Photoshop JSX 脚本。 |
| `1f5a1eb` revert Txt2 | 整体回退(−2022 行):模板路线对字体/字号/文本变化的适配维护成本过高。**保留了文本预览渲染**(`render_text_preview` 及其在 make_text_layer 里的 ink bbox 扫描),这是本次回退的净收益。 |
| `61d867c` Rebrand as lp2psd | 删除 GUI 与 JSX,定位为 CLI-only;修正 DPI 字号换算(pt 空间 vs 文档像素)。 |
| `8109cc2` DPI configurable(HEAD) | config `dpi` 字段(0/"original"=跟随图片,数值=固定),落在 `style.*` 与 `main.cpp:280-289`。 |

`build/diag/` 下的大量脚本(`txt2_*.bin` 模板切片、`make_variants.py` 等)就是那轮实验的遗物,可参照 [building.md](building.md)。

## 改进方向(按性价比排序)

1. **最小单元测试**:先给 `build_tysh`/`build_lrfx` 写字节级断言(输入固定 `TextLayerData` → 输出 golden bytes,或用 psd-tools 回读断言),锁定最脆弱的 EngineData 序列化。接口已预留,不依赖 GDI+。
2. **回归脚本化**:把"生成 testfile 样例 → psd-tools 断言图层树/文本/EngineData 关键字段"写成 pytest 脚本,替代人工对比。
3. **决策 `outputDir` 去留**(见限制 4)。
4. **行宽估算改进**:接入真实字形宽度(GDI+ `MeasureString` 已可用于预览,同源数据喂给 box 估算可消除两处估算不一致的风险)。
5. **重新评估"免手动更新"**:若重启该方向,优先评估[最小合法字形数据](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/)只需覆盖 PS 检查的字段(而非 Txt2 的全量模板),或干脆预置 `TextIndex`/`Rendered.Children` 的精简变体。
6. **跨平台**:见限制 5 的替换点清单。

## 文档维护约定

- 行号引用(`file:line`)会随重构失效,改代码时顺手更新本文与 [modules.md](modules.md)。
- 取值表(枚举/单位)以代码注释为准:psd_writer.hpp:108-141 与 psd_writer.cpp 各 `make_*` 函数。
