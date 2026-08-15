# lp2psd — 根据 txt 排版文件生成多图层 PSD

用 C++（无第三方依赖）生成 Photoshop PSD 文件，支持：

- 图片图层（读取同目录 JPG/PNG/BMP/GIF/TIFF）
- 可编辑文本图层（TySh + EngineData，Photoshop 中可直接改字）
- 图层分组（如 `框内` / `框外`）
- 文本图层样式（抗锯齿、对齐、行距、自由连字等，写入 EngineData）
- 所有文本图层均为**点文本**（无固定文本框，双击可编辑）
- 图层预览与合成图（GDI+ 渲染文本，保证任何查看器可见）
- 画布 DPI 取自第一张图片（JPEG 读取 JFIF/EXIF 分辨率，默认 96），
  写入 PSD 图像资源，Photoshop 中显示正确分辨率

## 构建（Windows）

需要 Visual Studio 2022（含 C++ 工具集）和 CMake：

```bat
build.bat
```

或手动：

```bat
cmake -S . -B build
cmake --build build --config Release
```

生成的可执行文件：`build\Release\lp2psd.exe`

> 如果不想用 CMake，也可以直接用 MSVC 编译器：
>
> ```bat
> call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
> cl /nologo /std:c++17 /utf-8 /EHsc /O2 src\main.cpp src\layout.cpp src\style.cpp src\image.cpp src\psd_writer.cpp /Fe:lp2psd.exe /link gdiplus.lib comdlg32.lib shell32.lib
> ```

## 使用

```bat
lp2psd.exe text1.txt
```

双击 `lp2psd.exe`（不带参数）会弹出 Windows 文件选择器，选择一个排版
文本文件即可开始生成。

参数：

- `--out <目录>`：输出目录（默认是**文本文件所在目录下的 `output`**，
  不存在会自动创建）
- `--config <style.json>`：样式配置（字体、字号、颜色、文本样式）；
  默认读取**程序同目录下的 `config.json`**（不存在时自动生成一份模板
  `config.json` 并读取，修改后重新运行即可生效）
- `--help`：帮助

每个 `>>>>>>>>[图片名]<<<<<<<<` 块生成一个 `<图片名>.psd`。

> 配置文件中的 `outputDir` 已不再控制输出位置，输出始终为
> 文本文件目录下的 `output` 文件夹；如确有需要，可用 `--out` 覆盖。

全部文件生成成功后，会自动在资源管理器中打开输出目录，方便直接查看
结果。

## 编码说明（重要）

- **路径**：命令行参数、txt 路径、图片文件名、输出目录均支持中文
  （内部统一使用 Unicode API，不再依赖系统代码页）
- **txt 文件编码**：自动识别 UTF-8（带/不带 BOM）、UTF-16 LE/BE、
  以及中文 Windows 常见的 GBK/ANSI，无需手动转换
- **style.json**：使用 UTF-8 编码（自动跳过 BOM）
- 源码内部统一使用 UTF-8 字符串；编译时请保留 `/utf-8` 选项
  （CMakeLists.txt 与 README 中的编译命令均已包含）

## txt 格式

```text
框内 --- (psd 分组名称 对应 1)
框外 --- (psd 分组名称 对应 2)

>>>>>>>>[01.jpg]<<<<<<<<
----------------[1]----------------[0.203,0.043,2]
文字第一行
文字第二行

----------------[2]----------------[0.649,0.128,1]
另一段文字
```

- `>>>>>>>>[xxx.jpg]<<<<<<<<`：图片文件名（与 txt 同目录）
- `----[编号]----[x,y,分组]`：文本位置（x、y 为图片宽高的 0~1 比例）与分组编号
- 后续非空行：该文本图层的内容（支持多行）
- 分组编号对应开头 `名称 --- (对应 N)` 的映射

文本内容中可以用 `\r`（或 `\n`）在**同一行内**强制换行，例如：

```text
----------------[1]----------------[0.2,0.1,1]
第一行\r第二行\r第三行
```

等价于三行文本（在 Photoshop 中以 `\r` 分段，即硬换行）。

## 样式配置（可选 style.json）

```json
{
  "outputDir": ".",
  "font": {
    "name": "Microsoft YaHei",
    "fontSize": 24,
    "color": [255, 255, 255],
    "antiAlias": "strong",
    "orientation": "horizontal",
    "justification": "left",
    "autoLeading": true,
    "autoLeadingSize": 1.2,
    "leading": 0,
    "discretionaryLigatures": false,
    "standardVerticalRomanAlignment": true
  }
}
```

`font` 中文本样式字段说明：

| 字段 | 说明 | 取值 |
|---|---|---|
| `fontSize` | 字体大小，单位为**点（pt）**，与 Photoshop 字符面板一致 | 数字，如 `24`。写入 PSD 时按文档 DPI 换算：文件值 = pt × DPI / 72，Photoshop 字符面板再按 ×72/DPI 显示回 pt，因此**与图像 DPI 无关，面板始终显示配置值** |
| `antiAlias` | 抗锯齿 | `none`/`crisp`/`strong`/`smooth`/`sharp`/`lcd`（或数字 0/1/2/3/4/6，对应 Photoshop 的无/犀利/浑厚/平滑/锐利/LCD） |
| `orientation` | 文本方向 | `horizontal`/`vertical`（或 0/1；竖排时每行文本成一列，列从右向左排列） |
| `justification` | 文本对齐 | `left`/`right`/`center`/`justifyLastLeft`/`justifyLastRight`/`justifyLastCenter`/`justifyAll`（或数字 0~6） |
| `autoLeading` | 是否自动行距 | `true`/`false` |
| `autoLeadingSize` | 自动行距倍数 | 数字，默认 `1.2`（自动行距时的行距 = 字号 × 该倍数） |
| `leading` | 手动行距（pt） | `autoLeading` 为 `false` 时生效；`0` 表示用 `autoLeadingSize` 计算 |
| `discretionaryLigatures` | 自由连字（Discretionary Ligatures） | `true`/`false` |
| `standardVerticalRomanAlignment` | 标准垂直罗马对齐（竖排时生效） | `true`/`false`（写入 EngineData `/BaselineDirection` 2/0） |
| `postScript` | 可选：显式指定写入 PSD 的 PostScript 字体名 | 字符串，如 `"SimSun"`；不填时自动从 `name` 生成 |
| `script` | 可选：EngineData 字体 Script | `auto`/`0`~`4`（0=罗马、1=日文、2=繁体中文、3=简体中文、4=韩文），默认 `auto` 按字体名自动判断 |

`antiAlias`、`orientation`、`justification` 的值也支持中文写法
（如 `"antiAlias": "平滑"`、`"orientation": "竖排"`、`"justification": "居中"`）。

`font.name` 填 Windows 显示名或 PostScript 名均可。常见中文显示名
（宋体、黑体、楷体、仿宋、微软雅黑、等线、隶书、幼圆、华文系列等）
会自动映射为对应的 PostScript 名；若 PS 仍提示“无法找到字体”，可用
`postScript` 直接指定（如 `"SimSun"`、`"KaiTi"`）。中文文本建议确认
`script` 为 `auto`（中文名会自动判定为 3，即简体中文），或手动指定 `3`。

## 代码结构

- `src/psd_writer.hpp/.cpp` + `src/psd_writer_internal.hpp`：PSD 写入库
  （公共文档模型在 `psd_writer.hpp`；字节序、descriptor、EngineData 等内部
  机制在 `psd_writer_internal.hpp`），纯 C++17，可独立复用
- `src/layout.hpp/.cpp`：排版 txt 解析（图片块、文本条目、分组）
- `src/style.hpp/.cpp`：样式配置解析（字体、字号、文本样式）
- `src/image.hpp/.cpp`：GDI+ 图片加载（含 DPI 读取）与文本预览渲染
- `src/textcodec.hpp`：UTF-8/UTF-16/ANSI 文本转换与文本文件读取
- `src/minijson.hpp`：极简 JSON 解析器
- `src/main.cpp`：CLI 编排（参数解析、文档装配、输出）
