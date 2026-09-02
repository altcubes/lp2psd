# config.json 配置文档

config.json 是 lp2psd 的样式与功能配置文件。程序按以下顺序查找:`--config` 参数指定 → exe 同目录的 `config.json` → 都不存在时首次运行自动生成模板。仓库根的 [config.json](../config.json) 是**全参数示例**,与本文件一一对应。

JSON 不支持注释,本文档即字段说明。未写的字段取内置默认;非法/越界值一律回落默认,不会报错。

## 顶层参数

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `dpi` | 数字或字符串 | `"original"` | 画布分辨率(像素/英寸)。`"original"`/`"auto"`/`"image"`/`"原图"`/`"自动"` = 使用原图内嵌 DPI(JPEG JFIF/EXIF,无则 96);数字(1~10000)= 所有图片统一用该分辨率。**影响字号换算**:EngineData 里字号 = pt × dpi ÷ 72,Photoshop 按 `pt = 值 × 72 ÷ dpi` 显示 |
| `bgCopy` | 对象 | 见下 | 可选:在 `bg` 图层正上方、`whites` 涂白图层下方生成一份原图拷贝图层(默认图层名 `bg 拷贝`),用于 PS 中对比/备份原图 |

## `layers` — 图层不透明度与锁定

作用于**所有生成图层**(背景 `bg`、`bg 拷贝`、涂白 `whites`、检测框 `dbnet_boxes` 与文本图层)。

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `opacity` | 数字 | `100` | 图层不透明度(%),范围 0~100。100 = 完全不透明。写入图层记录的不透明度字节(0~255)与 PS 图层面板一致。对 `whites` 图层会与 `whiten.transparency` 相乘(如 `layers.opacity: 50` + `transparency: 20` ⇒ 最终不透明度 40%) |

## `font` — 文字样式

所有样式写入 PSD 文本图层(TySh + EngineData),PS 中可继续修改。

| 字段 | 类型 | 内置默认 | 模板值 | 说明 |
|---|---|---|---|---|
| `name` | 字符串 | `"Microsoft YaHei"` | 同左 | 字体显示名。① 供涂白预览的 GDI+ 渲染(缺字体时自动回退 微软雅黑→宋体→Arial);② 自动解析 PostScript 名(内置中文映射表:宋体→SimSun、微软雅黑→MicrosoftYaHei 等 25 项;其余去掉空格,如 `Source Han Sans SC` → `SourceHanSansSC`) |
| `postScript` | 字符串 | `""` | `""` | 显式指定写入 PSD 的 PostScript 字体名,**非空时优先于** `name` 的自动解析。PS 字体列表里的名称可用 PS 脚本或字体工具查询 |
| `fontSize` | 数字/数字字符串 | `24` | `24` | 字号(pt),合法范围 (0, 10000)。文档像素大小 = pt × dpi ÷ 72 |
| `color` | `[R,G,B]` | `[255,255,255]` | `[0,0,0]` | 文字颜色,各分量 0~255 |
| `antiAlias` | 数字或字符串 | `2` | `"smooth"` | 抗锯齿:`0`/`"none"`/`"无"`、`1`/`"crisp"`/`"犀利"`、`2`/`"strong"`/`"浑厚"`、`3`/`"smooth"`/`"平滑"`、`4`/`"sharp"`/`"锐利"`、`6`/`"lcd"` |
| `orientation` | 数字或字符串 | `0` | `"vertical"` | 排版方向:`"horizontal"`/`"h"`/`"横排"`/`0`、`"vertical"`/`"v"`/`"竖排"`/`1` |
| `justification` | 数字或字符串 | `0` | `"center"` | 对齐:`0`/`"left"`/`"左"`、`1`/`"right"`/`"右"`、`2`/`"center"`/`"居中"`、`3`/`"justifylastleft"`、`4`/`"justifylastright"`、`5`/`"justifylastcenter"`、`6`/`"justifyall"`(3~6 有中文别名"两端对齐末行×") |
| `autoLeading` | 布尔 | `true` | `true` | 自动行距开关 |
| `autoLeadingSize` | 数字 | `1.2` | `1.2` | 自动行距倍数(行距 = fontSize × 此值) |
| `leading` | 数字 | `0` | `0` | 手动行距(pt),仅 `autoLeading: false` 时生效;≤0 回落自动行距 |
| `discretionaryLigatures` | 布尔 | `false` | `true` | 自由连字(EngineData /DLigatures) |
| `standardVerticalRomanAlignment` | 布尔 | `true` | `true` | 标准垂直罗马对齐(仅竖排生效,EngineData /BaselineDirection) |
| `script` | 字符串或数字 | `"auto"`(`-1`) | `"auto"` | EngineData FontSet Script:`"auto"`/`"自动"` = 按字体名推断(西文 0,简中 3,命中日/韩/繁体关键词取对应值);也可数字 `0` 罗马、`1` 日文、`2` 繁体中文、`3` 简体中文、`4` 韩文。Script 错误是 PS"字体需要重排"提示的来源之一,一般保持 auto |

## `dbnet` — 日文检测与自动涂白(可选)

需先运行 `scripts\setup_dbnet.bat`(onnxruntime + 导出 DBNet 检测模型)获取依赖,见 [dbnet.md](dbnet.md);缺依赖时自动跳过,不影响生成。

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `enabled` | 布尔 | `false` | 总开关。开启后为每张图片生成涂白图层(覆盖检测到的日文原文,不修改原图层) |
| `model` | 字符串 | `"dbnet_detect.onnx"` | 检测模型(m-i-t DBNet,ONNX)路径:绝对路径直接用;相对路径先找 exe 同目录,再找当前工作目录 |
| `limitSideLen` | 数字 | `1024` | 推理前长边缩放目标(保持纵横比,pad 到 256 倍数)。调大更精细、更慢 |
| `dbBinThreshold` | 数字 | `0.5` | sigmoid(db) 二值化阈值。调高 → 框更保守(漏检多),调低 → 更激进(误检多) |
| `dbBoxThreshold` | 数字 | `0.7` | 连通域平均概率低于此值丢弃(过滤弱置信区域) |
| `dbUnclipRatio` | 数字 | `2.3` | DB unclip 外扩比例(d = area×ratio/perimeter),影响框的贴合与相邻列粘连 |
| `minSide` | 数字 | `3.0` | 模型网格空间短边小于此值的框丢弃 |
| `segThreshold` | 数字 | `0.12` | 笔画遮罩二值化阈值(mask 概率 > 此值算笔画) |
| `minBoxArea` | 数字 | `64` | 丢弃面积(像素²)小于此值的框,过滤小误检 |
| `whiten.enabled` | 布尔 | `true` | 涂白图层开关(`enabled` 为 false 时无效果) |
| `whiten.color` | `[R,G,B]` | `[255,255,255]` | 涂白填充颜色 |
| `whiten.margin` | 数字 | `3` | 笔画遮罩外扩像素(盖住抗锯齿边缘) |
| `whiten.boxMarginX` | 数字 | `3` | 检测框**横向**外扩像素(图像 x 轴),与 `dbnet_boxes` 描边共用同一外扩;`limitToBoxes` 开启时涂白横向最多延伸到此处 |
| `whiten.boxMarginY` | 数字 | `3` | 检测框**竖向**外扩像素(图像 y 轴),与 `dbnet_boxes` 描边共用同一外扩;`limitToBoxes` 开启时涂白竖向最多延伸到此处 |
| `whiten.limitToBoxes` | 布尔 | `true` | `true`(默认):涂白 = (mask 按 `margin` 外扩) ∩ (检测框按 `boxMarginX`/`boxMarginY` 外扩),框外涂白为 0。`false`:旧方案,全 mask 按 `margin` 外扩,不限制在框内 |
| `whiten.layerName` | 字符串 | `"whites"` | 涂白图层名(图层紧贴原图层之上) |
| `whiten.transparency` | 数字 | `0` | 涂白图层的透明程度(%),范围 0~100。0 = 完全不透明,100 = 完全透明(等价于图层不透明度 = 100 − transparency)。与 `layers.opacity` 相乘生效 |

`limitToBoxes` 下,被 `dbBoxThreshold`/`minBoxArea` 过滤掉(无框)的弱文字 mask 区域不再涂白——若想保留弱文字,优先下调这两个框阈值,而不是直接关闭 `limitToBoxes`。`--debug-dbnet` 会额外输出 `<图片名>_whiten.png`(白 = 最终涂白像素)便于目检。
| `boxes.enabled` | 布尔 | `false` | 检测框描边图层开关:在图层栈**最顶部**生成 1px 红框透明图层,按旋转四边形描边,标记每处涂白位置 |
| `boxes.color` | `[R,G,B]` | `[255,0,0]` | 描边颜色 |
| `boxes.layerName` | 字符串 | `"dbnet_boxes"` | 描边图层名 |
| `boxes.lock` | 布尔 | `false` | **仅对** `dbnet_boxes` 图层生效:为 `true` 时锁定全部(透明像素 + 图像像素 + 位置,`lspf = 0x07`,PS"锁定全部"),防止误编辑 |

## `bgCopy` — 背景拷贝图层(可选)

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `enabled` | 布尔 | `false` | 是否生成 `bg` 的拷贝图层 |
| `layerName` | 字符串 | `"bg 拷贝"` | 拷贝图层名(位于 `bg` 之上、`whites` 之下,与 `bg` 像素完全一致) |

旧字段:`detThresh` 仍作为 `dbBinThreshold` 的别名读取;`unclipPx`(固定像素膨胀)已由 `dbUnclipRatio`(比例膨胀)取代,不再读取。

调试:`--debug-dbnet <目录>` 输出 `<图片名>_dbnet.png`(绿=笔画遮罩、红=四边形)与 `<图片名>_mask.png`、`<图片名>_quads.json`,用于核查涂白位置(详见 [dbnet.md](dbnet.md))。

## 生成结果对照

以上参数如何落到 PSD:

| config 项 | 落点 |
|---|---|
| `dpi` | 图像资源 0x03ED(画布分辨率)+ EngineData 字号/行距换算 |
| `font.*` | 文本图层 TySh / EngineData(字体、字号、颜色、对齐、行距、连字、Script)与涂白预览渲染 |
| `dbnet.*` | `whites` 涂白图层与可选 `dbnet_boxes` 描边图层 |
| `bgCopy.*` | 可选的 `bg 拷贝` 图层 |
| `layers.*` | 每个图层记录的不透明度字节、flags 位与 `lspf` 保护设置块 |
