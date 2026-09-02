# PSD 内部机制:TySh / EngineData / lrFX

本文深入 `src/psd_writer.cpp` 的字节级实现。写入器不依赖任何第三方库,字节布局以 psd-tools(解析库)与真实 Photoshop 生成的 PSD 为对照逐字节校准。阅读本文建议对照 [Adobe Photoshop File Formats Specification](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/)。

## PSD v1 文件整体结构

`build_document_bytes`(psd_writer.cpp:1280)按 Adobe 规范写出:

```
File header          8BPS, version 1, RGB, 8 bit/channel, channels 3 或 4
Color mode data      长度 0
Image resources      仅一块:0x03ED 分辨率信息(write_image_resources :1253)
Layer & mask info    图层记录 + 通道数据(collect_records :1207 展开)
Merged image data    全文档合成图,PackBits RLE
```

要点:

- **通道数**:合成图扫描 alpha,存在半透明像素写 4 通道,否则 3 通道(:1343-1346)。合成图由各图层 `paste_over`(:1220)alpha 混叠而成——文本图层贡献它的预览栅格。
- **图像资源 0x03ED**(:1253):16.16 定点数写画布 DPI,unit=1(像素/英寸)。Photoshop 靠它显示画布分辨率;没有这一块,文档 DPI 恒为 72,字号换算全错。
- **分组**:`Group` 展开为三条记录——`lsct=3` 分隔符(`</Layer group>`)、子图层(递归)、`lsct=1/2` 打开/闭合文件夹(:1207-1218)。分隔符记录用 Photoshop 规范要求的占位名与 `norm` 混合模式(:1410、:1422)。
- **图层矩形**:PixelLayer 直接用 x/y/w/h;TextLayer 用 `llround(box)` 重算(:1382-1387),与 main.cpp 的 `tl->left/top/right/bottom` 一致。
- **tagged blocks**:每条记录的 extra data 里按需写入 `lsct`、`TySh`、`lrFX`、`luni`(Unicode 层名)、`lnsr`、`lyid`(自增层 id)、`clbl`、`infx`、`knko`、`lspf`、`lclr`,文本层另有 `shmd`(固定字节的 layerTime 元数据,:1466)与 `fxrp`(参考点 = 文本框中心,:1480)。这套组合是从真实 Photoshop 文件抄的"让 PS 满意"的最小集合。
- **通道压缩**:每通道独立 PackBits(:837 `write_rle_channel`:压缩方式 u16(1) + 每行压缩后长度表 + 行数据)。`packbits`(:808)按 ≥3 重复打包、字面量段 ≤128 的经典规则。

## Photoshop Descriptor 序列化(TySh 的骨架)

TySh 的文本数据是 Photoshop 描述符(pascal 风格键值树)。模型与序列化:

- `DValue`(psd_writer_internal.hpp:42)九种类型,`dtype4`(:194)给类型 ID:

  | 类型 | ID | 载荷 |
  |---|---|---|
  | Obj | `Objc` | 递归 Descriptor(name + classID + count + items) |
  | List | `VlLs` | count + 逐项(typeID + value) |
  | Double | `doub` | IEEE 754 big-endian f64 |
  | Unit | `UntF` | 4 字节单位 + f64(如 `#Pnt`、`#Pxl`) |
  | Text | `TEXT` | UTF-16BE,前置 u32 字符数 |
  | Enum | `enum` | 两个 length-prefixed 字符串(类型 ID、枚举 ID) |
  | Long | `long` | i32 |
  | Bool | `bool` | u8 |
  | Raw | `tdta` | u32 长度 + 原始字节(EngineData 就放这里) |

- 字符串统一 `u32 长度 + 字节`(`write_length_key` :215);UTF-8 → UTF-16BE 转换 `utf8_to_utf16be`(:91,含代理对与 U+FFFD 回退)。

## TySh tagged block

`build_tysh`(:1030)的字节序列:

```
u16 1                          版本
f64 ×4                         变换矩阵 (1,0,0,1)
f64 ×2                         文本锚点 = 文本框中心(文档像素)
u16 50                         描述符大小写版本
u32 16 + Descriptor(TxLr)      文本数据
u16 1  + u32 16 + Descriptor(warp)  变形(全部 warpNone)
i32 ×4                         保留(0)
pad 4
```

TxLr 描述符条目:

| key | 值 | 说明 |
|---|---|---|
| `Txt ` | TEXT | 文本,`\n` → `\r`(Photoshop 用 CR),末尾加 `\0` |
| `textGridding` | enum `None` | |
| `Ornt` | enum `Hrzn` / `Vrtc` | 横排 / 竖排 |
| `AntA` | enum `Annt` | 见下方取值表 |
| `TxMg` | enum `TxMg`/`TxNM` | 文本网格(无),真实 PS 恒有此键 |
| `bounds` | Obj(#Pnt ×4) | em box(点),锚点居中 |
| `boundingBox` | Obj(#Pnt ×4) | 渲染墨迹框(点),锚点居中 |
| `TextIndex` | long 0 | |
| `EngineData` | tdta | 引擎数据(下节) |

与真实 Photoshop 一致,所有描述符(TxLr、bounds、boundingBox、warp)的 **name 都是 1 个 `\0`**(`write_unicode` 写出 u32 长度 1 + `00 00`),不是空串(:1033-1062)。

### TySh 几何:`tysh_layout`(:962)

- anchor = `(box_x + box_w/2, box_y + box_h/2)`(文档像素)。
- em box:横排 `宽 = max(最长行 em,1) × fs_px`、`高 = 行数 × leading_px`;竖排每行一列、列高 = 最长列字数 × fs_px。`fs_px = font_size × dpi / 72`——**TySh 几何全部以文档像素存储**,与真实 Photoshop 一致(:987-990 注释)。
- ink box:预览存在时由 ink bbox 相对锚点换算(`ink - box_w/2`);无预览时回落 em box(:1010-1022)。`bounds`/`boundingBox` 虽然单位是 `#Pnt`,但数值与像素相同——Photoshop 在 72dpi 点空间解释它们,文档 DPI 信息放在 0x03ED 里。

### 取值映射表

**抗锯齿** `anti_alias`(:426 `anti_alias_enum`):

| int | EngineData /AntiAlias | TySh AntA 枚举 |
|---|---|---|
| 0 | 0 | `AnNo`(无) |
| 1 | 1 | `AnCr`(犀利) |
| 2 | 2 | `AnSt`(浑厚,默认) |
| 3 | 3 | `AnSm`(平滑) |
| 4 | 4 | `antiAliasSharp`(锐利) |
| 6 | 6 | `antiAliasPlatformLCD`(LCD) |

**对齐** `justification`(EngineData /Justification):0=Left 1=Right 2=Center 3=JustifyLastLeft 4=JustifyLastRight 5=JustifyLastCenter 6=JustifyAll。

**书写方向**:EngineData `WritingDirection` 0=横 / 2=竖;TySh `Ornt` Hrzn/Vrtc;**子形状 `Procession` 竖排必须写 1**(:702-706)——真实 PS 文件竖排写 Procession=1,写 0 会让 PS 重建图层时方向回落横排。

## EngineData(文本引擎数据)

`make_engine_bytes`(:794)产出三大段:`EngineDict` + `ResourceDict` + `DocumentResources`(后两段内容相同)。

序列化(`write_edict` :343 / `write_elist` :323 / `write_estring` :293 / `fmt_float` :275)仿 psd-tools 的文本格式:`/Key value` 缩进用 tab、字典 `<<` `>>`、数组 `[]`、字符串 `(\xFE\xFF + UTF-16BE)`(转义 `\` `(` `)`)、浮点 5 位小数去尾零、`0.x` 去前导零。与 psd-tools 字节级一致是刻意目标——便于调试对比。

### EngineDict(:607)

- **Editor.Text**:全文(`\n`→`\r`,**末尾补一个 `\r`**)。
- **ParagraphRun**:每个 `\r` 段一个 run;每段长度按 UTF-16 code units 计(`utf16_length` :132,代理对算 1)。段落样式携带 `ParagraphSheet`(对齐、避头尾等,:567)。
- **StyleRun**:同样每段一个 run,`StyleSheetData` = `make_run_style`(:494):
  - `Font` = 0(FontSet[0] 即配置的字体);
  - **`FontSize` = pt × dpi / 72**——Photoshop 按文档像素存储字号,读回时 `pt = 值 × 72 / dpi`,这样字符面板显示的就是配置的 pt 值;
  - 只写**非默认**样式(PS 自己也这样):手动行距才写 `AutoLeading:false` + `Leading`;竖排 + 开启标准垂直罗马对齐才写 `BaselineDirection:1`;非黑色才写 `FillColor`。
  - 真实 PS 的 run 在 `BaselineDirection` 后恒写两条非默认属性:白色描边 `StrokeColor << /Type 1 /Values [ 1.0 1.0 1.0 1.0 ] >>` 与 `HindiNumbers false`(与默认样式表的 `[1,0,0,0]` 描边区分)。
- **GridInfo**:固定关闭的网格。
- **/AntiAlias**:int,见上表。
- **Rendered.Shapes**:点文本(`ShapeType 0` + `PointBase`;段落文本是 ShapeType 1 + BoxBounds,本工具只用点文本,:688-694)+ `Procession`(方向,见上)。

### ResourceDict(:720)

- `KinsokuSet` / `MojiKumiSet`:Photoshop 标准避头尾/注音规则集(固定内容)。
- `ParagraphSheetSet` / `StyleSheetSet`:一个名为"正常 RGB"的默认样式表;`StyleSheetSet` 的 `StyleSheetData` 用 `make_full_style_data`(:524)写**全量默认样式**,其中 `Font`=3(指向 CJK 回退字体)、`FontSize`=**固定 12.0**(PS 恒写 12pt,图层自身样式由 StyleRun 提供)、`BaselineDirection`=2。
- **FontSet**(:767-790),当前 Photoshop 的四槽布局:
  - `[0]` 配置字体,`FontType` 1(TrueType),`Script` = config 指定,否则 `font_script_of`(:449)按字体名关键词推断(默认西文 0;命中 CJK 关键词后按日/韩/繁体关键词细分,否则简体 3);
  - `[1]` `AdobeInvisFont`(FontType 0);
  - `[2]` `MyriadPro-Regular`(FontType 0,Script 0);
  - `[3]` `AdobeHeitiStd-Regular`(FontType 2,Script 3)——默认样式表指向它。

### 为什么 PS 需要"更新所有文本图层"

本工具写出的 EngineData 只描述样式与文本,**不包含 PS 原生排版引擎计算的字形网格(glyph run / 每字形位置)**。PS 打开时按预览栅格显示,但文本引擎缓存为空/过期,于是提示"更新所有文本图层";更新后 PS 自己排版,通常仅位置有微小偏差。历史上(提交 `5f8f1ce`)曾尝试自带 1300 行模板伪造完整字形数据(`txt2_templates.hpp` 路线),因维护成本与兼容问题在 `1f5a1eb` 整体回退,详见 [roadmap.md](roadmap.md)。

## lrFX(legacy 图层样式)

`build_lrfx`(:1081):`u16 0`(版本)+ `u16 子块数` + 若干 `8BIM` tagged block;无启用效果时返回空(不写 lrFX)。公共首块 `cmnS`(可见标志),随后按效果类型:

| 效果 | key | 数值编码 |
|---|---|---|
| 投影 | `dsdw` | 角度/距离/尺寸均为 16.16 定点(×65536) |
| 内阴影 | `isdw` | 同上 |
| 外发光 | `oglw` | 尺寸定点 |
| 内发光 | `iglw` | 尺寸定点 + invert |
| 斜面 | `bevl` | 高光/阴影两组混合模式与颜色 |
| 颜色叠加 | `sofi` | 混合模式 + 颜色 + 不透明度 |

颜色统一 `write_color`(:847):`u16 0`(RGB 空间)+ 三个 `u16(c*257)`(0-255 → 0-65535);不透明度 `pct_to_byte`(:880)0-100 → 0-255。混合模式 4 字符 ID 映射见 `blend4`(:855)。

## 图层不透明度与锁定

每条图层记录写三处(:1394-1402、:1462-1470):

- **不透明度字节**:blend mode 后 1 字节 0-255,config `layers.opacity`(0-100%)换算;255 = 完全不透明。
- **flags 字节**:bit3 = PS5+ 标志(恒置),bit0 = 透明像素锁定(`transparency_locked`),bit1 = 隐藏(`!visible`)。对照真实 PS:普通图层 `0x08`,锁定透明像素后 `0x09`。
- **lspf 块**:u32 位掩码,bit0 = 透明像素锁定、bit1 = 图像像素/编辑锁定、bit2 = 位置锁定。config `dbnet.boxes.lock` 开启时对 `dbnet_boxes` 图层三者全开 = `0x07`(PS"锁定全部")。老版本 PS 全锁另写 `0x80000000`(官方 sample 的 `bg 拷贝` 即如此),本工具按新格式写 `0x07`,解析器(ag-psd 等)均能识别。

## 与 psd-tools 对调调试

写入器历史上就是对着 psd-tools 的解析结果逐字段校准的,调试方法(脚本在 `build/diag/`,不入库,详见 [building.md](building.md)):

```python
from psd_tools import PSDImage
psd = PSDImage.open("gen.psd")
layer = psd[0][0]          # 分组内的文本层
print(layer.engine_data)   # EngineDict / ResourceDict 原始解析
print(layer.text)          # Txt / bounds / boundingBox
```

怀疑字节级问题时,把本工具输出与 Photoshop 参照 PSD(`testfile/ps_ref_*.psd`)的同名 tagged block 做十六进制对比;EngineData 文本差异直接 diff 两个 block 的 tdta。
