# 模块参考

逐文件说明 `src/` 下各模块的职责、关键函数与注意事项。行号以当前代码为准,重构后请随代码更新。

## main.cpp — CLI 编排与文档组装

入口 `wmain`(src/main.cpp:348)。流程:

1. **参数解析**(:353-365):位置参数 = 排版 txt;`--out`、`--config`、`--help`。多余参数报错退出码 2。
2. **交互兜底**(:366-374):没有 txt 时弹文件选择对话框 `pick_text_file`(:56),取消则退出码 2。
3. **配置定位**(:377-392):`--config` 优先;否则找 exe 同目录 `config.json`;都不存在则写出模板 `write_template_config`(:72,UTF-8 无 BOM)再读——即"首次运行生成模板"。
4. **解析**:`parse_layout` → `load_style`(minijson 解析失败直接退出码 1)。
5. **输出目录**:默认 `<txt 目录>\output`,`CreateDirectoryW` 自动创建(:416-419)。
6. **逐图片块生成**(:430-440):文件名 = `<stem>.psd`;单个失败只打印 FAIL,不影响后续;全部成功返回 0 并打开 Explorer,否则返回 1。

### make_text_layer(:157)— 一个 TextEntry → 一个文本图层

- `split_escaped_lines`(:124):把一行里的字面 `\r` / `\n` 转义拆成多个逻辑行。
- `line_units`(:142):行宽估算,CJK 字符 1.0em、Latin 0.55em(按 UTF-8 首字节判断长度)。
- 文本为空返回 `nullptr`(不生成图层)。
- **DPI 换算**(:178-186):`px_scale = dpi / 72`;`font_size_px = pt * px_scale`;行距同乘。自动行距 = `fontSize * autoLeadingSize`;手动行距 `leading <= 0` 时回落到自动。
- 文本框尺寸(:191-201):
  - 横排:`box_w = max(最长行 em, 1em) * font_size_px`,`box_h = 行数 * leading_px`;
  - 竖排:每行一列,`box_w = 行数 * font_size_px * 1.35`(列宽留 1.35 系数),`box_h = 最长列字数 * font_size_px`。
- 图层名取第一行前 60 字节(:203)。
- 图层矩形用 `llround(x + w) - llround(x)` 语义(:227-230),必须与 record 矩形精确一致(build_document_bytes 里 TextLayer 的矩形也是从 box 重新 llround 的,见 psd_writer.cpp:1382-1387)。
- 预览与 ink bbox(:233-259):调 `render_text_preview` 渲染 `pw×ph` 栅格,然后逐像素扫描 alpha > 8 得到墨迹外接框(相对图层矩形左上角,右/下为 +1 的开区间端点)。无预览时 ink box 全 0,表示"未知"。

### build_psd(:265)— 一个 ImageBlock → 一个 PSD

- `load_image` 失败 → 返回 false,错误信息 "cannot load image: ..."。
- 画布 DPI(:280-289):图片 DPI < 1 时回落 96;config `dpi > 0` 时覆盖。
- 底层 `bg` 像素图层(整幅图)。
- **分组堆叠顺序**(:301-322):分组号按"映射顺序优先、条目出现顺序补充"收集到 `gnums`,然后**逆序**压入 `doc.layers`(模型是 bottom-to-top),因此分组映射表中先出现的组在 PS 里位于上层。组名为空映射时用 `"Group N"` 兜底(:112)。
- `group <= 0` 的条目不进组,直接放顶层(:325-329)。
- **dbnet 集成**:dbnet 检测在 `build_psd` 开头(`style.dbnet.enabled` 时调 `dbnet_detect`,失败打印警告继续),随后:
  - `make_whiten_layer`:默认(`whiten.limitToBoxes=true`)把笔画遮罩按 `whiten.margin` 外扩(`dilate_binary`,滑动窗口 O(n)),再与按 `whiten.boxMarginX`/`boxMarginY` 外扩的检测框栅格化区域取交集,合成白色像素图层,插入 `bg` 之上;框外涂白为 0。`false` 时走旧分支(全 mask 按 `margin` 外扩,无框限制);
  - `make_box_outline_layer`:可选(`dbnet.boxes.enabled`)在图层栈**最顶部**按旋转四边形(外扩 `whiten.boxMarginX`/`boxMarginY`)生成 1px 描边透明图层,与交集允许区域共用同一外扩;
  - `--debug-dbnet <dir>`:`save_dbnet_debug` + `image.cpp` 的 `save_image_png` 输出调试图 `<图片名>_dbnet.png`(绿=笔画遮罩、红=四边形)+ `_mask.png` + `_quads.json`;新方案另输出 `_whiten.png`(最终涂白区域)并在控制台打印 mask→外扩→框内交集统计。

## layout.cpp / layout.hpp — LabelPlus txt 解析

数据模型(layout.hpp):`TextEntry{index, x, y, group, lines}`、`ImageBlock{image, entries}`、`Layout{groups(序号→名称的有序 pair 列表), images}`。

`parse_layout`(:63)逐行状态机,`block`/`entry` 两个指针表示当前位置:

| 行形态 | 识别 | 处理 |
|---|---|---|
| `>>>>>>>>[01.jpg]<<<<<<<<` | 前缀 `>>>>>>>>[`(:83,兼容 9 个 `>`) | 开启新 ImageBlock |
| 头部区(`block == nullptr`) | 见下 | 分组名映射 |
| `----------------[N]--------[x,y,g]` | 行首 `-` 且 `parse_entry_line` 成功(:177) | 新 TextEntry |
| 其它 | 当前 entry 存在 | 追加为一行文本 |

**头部分组名映射**( :95-174)同时接受两种写法:

- 样例注释式:`框内 --- (psd 分组名称 对应 1)` — 含 `---` 时取前半为组名,"对应"后面的数字为组号(:109-131,"对应"两字用十六进制字节字面量写死,避免源码编码问题)。
- 真实导出式(纯行):
  ```
  1,0        ← "数字,数字" 视作文件头跳过
  -          ← 分隔行跳过
  框内       ← 依次分配 next_group_num = 1, 2, ...
  框外
  -
  Default Comment / You can edit me   ← 已知注释行跳过
  ```
  以 `-`、`=`、`#` 开头的行也跳过(:136-137)。

`parse_entry_line`(:25):两个 `[...]`,第一个内必须是纯数字(条目序号),第二个内逗号分隔至少 3 个数(x、y、group)。没有图片块时解析失败("no image blocks found in text file")。

## style.cpp / style.hpp — 样式配置

`Style` 结构(style.hpp:9-33),全部字段与 `config.json` 对应,字段语义见根 README 的配置表。

`load_style`(style.cpp:145)的容错逻辑:

- **字体**(:148-157):`font.name` 同时供 GDI+ 预览(`font_name`,宽字符)与 PostScript 名解析;`resolve_font_ps`(:30)内置中文显示名 → PostScript 名映射表(宋体→SimSun 等 25 项),其余走"去空格/Tab"规则(`Microsoft YaHei` → `MicrosoftYaHei`);`font.postScript` 显式指定时**优先于**自动解析。
- **字号** `parse_font_size`(:85):接受数字或数字字符串,合法范围 (0, 10000)。
- **枚举解析**均接受数字或名称(含中文):`parse_anti_alias`(:107)、`parse_orientation`(:120)、`parse_justification`(:129)、`parse_script`(:70,`auto`/`自动` → -1 表示按字体名自动判断)。
- **DPI**(:180-191):数字且 ∈ [1, 10000] 为固定 DPI;字符串 `original`/`auto`/`image`/`原图`/`自动` → 0(跟随图片)。
- **dbnetConfig**(style.hpp):`dbnet.enabled`(默认 false)、`model`、`limitSideLen`/`dbBinThreshold`/`dbBoxThreshold`/`dbUnclipRatio`/`minSide`/`segThreshold`/`minBoxArea`(推理参数)、`whiten`(颜色/边距/图层名/透明度)、`boxes`(描边图层颜色/图层名/全锁)。另有顶层 `bgCopy`(可选 `bg 拷贝` 图层)。解析在 `load_style` 末尾,字段名 camelCase 与 config 对应;`detThresh` 作为 `dbBinThreshold` 的旧别名兼容。

## dbnet.cpp / dbnet.hpp — 可选 dbnet 检测(编译开关 `LP2PSD_WITH_dbnet`)

用 ONNX Runtime(C API,`LoadLibraryW("onnxruntime.dll")` 动态加载,**不链接 .lib**)+ m-i-t DBNet 检测模型(ONNX,与 yakuyomi-engine 同款检测器)做日文文本区域检测,输出旋转四边形 + 逐像素笔画遮罩(供涂白图层)。头文件在未定义宏时提供内联桩(`dbnet_available()` 恒 false),调用方无需条件编译。管线与依赖安装见 [dbnet.md](dbnet.md)。

- `load_runtime`:`GetProcAddress("OrtGetApiBase")` → `GetApi(ORT_API_VERSION)`,结果缓存。
- `ensure_session`:模型存在性检查;`OrtSession` 按进程缓存,同一模型只加载一次。
- `dbnet_detect`:双线性 RGBA 缩放(长边 = `limit_side_len`)→ pad 到 256 倍数(黑)→ RGB 归一化 (x/127.5-1)→ `OrtApi::Run`(in0 → out0 db / out1 mask)→ sigmoid(ch0) 二值化(`det_thresh`)→ 8 连通 BFS 连通域收集边界点 → 凸包旋转卡尺 `min_area_rect` → DB unclip(`unclip_ratio`)→ 旋转四边形映射回原图坐标 → `min_box_area`/`min_side` 过滤;mask 裁有效区 → 双线性放大回原图 → `seg_thresh` 阈值 → 笔画遮罩。后处理与 yakuyomi-engine `Detector.kt` 逐行一致。

## image.cpp / image.hpp — GDI+ 图像与文本预览

- `GdiScope`(image.hpp:17):`GdiplusStartup/Shutdown` 的 RAII 包装,wmain 开头构造。
- `load_image`(:91):`Bitmap` 加载 → `LockBits(PixelFormat32bppARGB)` → 手动 BGRA→RGBA 逐像素翻转。同时读内嵌 DPI。
- `read_image_dpi`(:21):JPEG 直接走原始字节解析 APP0/JFIF 段取密度单位(仅 units==1,即像素/英寸);其它格式回落 GDI+ EXIF 属性 `0x011A/0x011B`(X/YResolution,Rational);全都没有则 96。GDI+ 不暴露 JFIF 密度,所以 JPEG 必须手工解析。
- `render_text_preview`(:122):在图层矩形大小的 ARGB 位图上渲染文本,输出 RGBA8。
  - 抗锯齿:`TextRenderingHintAntiAlias`;字体缺失时按 微软雅黑 → SimSun → Arial → Microsoft Sans Serif 顺序回退(:136-148),保证预览不空白(EngineData 里的字体不受影响)。
  - **竖排**(:154-166):每行一列,列宽 `size_px * 1.35`,从右往左排(`x0 = w - (i+1)*col_w`),`StringFormatFlagsDirectionVertical`;与 Photoshop 竖排行为一致。
  - **横排**(:167-178):按 justification 左/中/右定位(仅 1、2 生效),行高 `line_advance`。
  - 颜色 BRGA 翻转与 load_image 相同(:189-197)。
- `save_image_png`:RGBA8 → PNG(`GetImageEncoders` 找 PNG CLSID),dbnet 调试图输出用。

预览栅格有两个用途:① 写入 PSD 作为文本图层的合成像素,让任何查看器(不解析 EngineData 的)都能显示文字;② 扫描出 ink bbox 供 TySh `boundingBox` 用。

## psd_writer.hpp / _internal.hpp / psd_writer.cpp — PSD 写入库

公开模型(psd_writer.hpp):

- `Effects`(:96):六种 legacy lrFX 图层样式的可选值(投影/内阴影/外发光/内发光/斜面/颜色叠加),字段语义见头文件注释;`BlendMode`(:30)19 种混合模式。
- `TextLayerData`(:108):文本图层全部内容,字段注释即规范——尤其注意 `dpi`(:118)与 ink box(:121)的坐标系约定,详见 [psd-internals.md](psd-internals.md)。
- `Document`(:168):宽高、`res_h/res_v`(写进图像资源 0x03ED)、`layers`(bottom-to-top)。`write` / `write_wide`:narrow 版在 Windows 上内部转宽字符转发,避免 ANSI 代码页路径乱码(psd_writer.cpp:1547-1555)。

内部机制(psd_writer_internal.hpp,仅 psd_writer.cpp 与未来的 writer 测试可 include):

- `Buffer`(:19):大端字节缓冲,`pad(align)` 补零对齐。
- `Descriptor`/`DValue`(:42-67):Photoshop pascal 描述符模型,9 种值类型(Objc/VlLs/doub/UntF/TEXT/enum/long/bool/tdta)。
- `EVal`/`EDict`(:85-102):EngineData 标记语言模型(Dict/List/Str/Flt/Int/Bool)。
- `build_tysh` / `build_lrfx`(:107-108):tagged-block 构建器,注释注明 "exposed for writer tests"——预留的单元测试入口,测试尚未落地。

psd_writer.cpp 关键函数(按数据流顺序):

| 函数 | 位置 | 职责 |
|---|---|---|
| `write_descriptor`/`write_dvalue` | :222/:233 | 描述符二进制序列化 |
| `fmt_float` | :275 | EngineData 浮点格式:5 位小数去尾零、0.x 去前导零 |
| `write_estring` | :293 | `(\xFE\xFF + UTF-16BE)` 字符串,转义 `\` `(` `)` |
| `write_edict`/`write_elist` | :343/:323 | EngineData 缩进排版(仿 psd-tools 字节级一致) |
| `make_engine_dict` | :607 | EngineDict:Editor/ParagraphRun/StyleRun/GridInfo/Rendered |
| `make_resource_dict` | :720 | ResourceDict:Kinsoku/MojiKumi/样式表/FontSet |
| `make_engine_bytes` | :794 | EngineDict + ResourceDict + DocumentResources 三段 |
| `packbits` | :808 | PackBits RLE(≥3 重复打包,其余字面量,≤128 分段) |
| `tysh_layout` | :962 | TySh 几何:anchor(文本框中心)、em box、ink box |
| `build_tysh` | :1030 | TySh tagged block 完整字节 |
| `build_lrfx` | :1081 | lrFX tagged block(无启用效果返回空) |
| `collect_records` | :1207 | 展开分组为 lsct 记录序列(divider→children→folder) |
| `build_document_bytes` | :1280 | 组装整个 PSD v1 文件 |
| `Document::write_wide` | :1573 | CreateFileW 写文件 |

## minijson.hpp — 极简 JSON

`mjson::Value`(minijson.hpp:12):Null/Bool/Num/Str/Arr/Obj 六型,Obj 用 `std::map` 存储(键有序)。`parse`(:168)。支持 `\uXXXX`(含基本多文种平面内转 UTF-8;**不支持代理对**——config 里的中文用不到,注意即可)。数字直接 `strtod`。

## textcodec.hpp — 文本编码

- `wide_to_utf8` / `utf8_to_wide`(:19/:30):CP_UTF8 往返。
- `read_text_file`(:42):**编码探测顺序**:UTF-8 BOM → UTF-16LE BOM → UTF-16BE BOM → 严格 UTF-8 校验 → 回落系统 ANSI(`CP_ACP`,中文 Windows 即 GBK)。返回统一 UTF-8。
