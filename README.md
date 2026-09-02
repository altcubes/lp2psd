# lp2psd

根据 [LabelPlus](https://github.com/LabelPlus/LabelPlus) 排版 txt 生成多图层 PSD文件的 Windows 命令行工具。排版 txt 中每引用一张图片，程序就输出一个 PSD，包含完整背景、图层分组与**可编辑文本图层**；另可选用 DBNet 检测日文原文，按笔画生成自动涂白图层，省去嵌字流程里手动擦字的重复劳动。

> ⚠️ **重要提示**：生成的 PSD 在 Photoshop 中打开后，需手动刷新文本排版：菜单 **文字(Y) → 更新所有文本图层(U)**。原因见 [已知限制](#已知限制)。

## 主要特性

- **真正可编辑的文本图层**：文字以 TySh + EngineData 写入，打开后仍可在 Photoshop 中修改字体、字号、颜色、行距、对齐等，不用重新打字。
- **图层结构即排版结构**：每条排版记录按分组号归入图层组，未分组记录放在顶层；文本图层内同时写入 GDI+ 渲染的像素预览，不支持 EngineData 的查看器也能看到文字效果。
- **DPI 正确的字号换算**：字号按 Photoshop 的点空间（pt × dpi / 72）换算，画布 DPI 跟随原图（JPEG JFIF/EXIF）或由配置统一指定。
- **可选 DBNet 自动涂白**：只做日文文本区域检测（旋转四边形 + 逐像素笔画遮罩），生成覆盖原字的涂白图层，不修改背景原图。
- **零第三方 C++ 依赖**：基础构建不需要 vcpkg/Conan 或任何第三方 C++ 库；PSD 写入、JSON 解析、文本编码转换全部内置。
- **中文路径友好**：支持 UTF-8 / UTF-16 / GBK 编码的排版 txt，路径处理全程走宽字符 API。

## 系统要求与构建

依赖：Windows 10+、Visual Studio 2022（MSVC，C++17）、CMake ≥ 3.15。

```bat
:: 一键构建
build.bat

:: 等价手动步骤
cmake -S . -B build
cmake --build build --config Release
```

产物：`build\Release\lp2psd.exe`。

### 可选：启用 DBNet 检测涂白

先按上面的步骤构建出 `lp2psd.exe`，再运行：

```bat
scripts\setup_dbnet.bat
build.bat
```

脚本会下载 onnxruntime（1.19.2，预编译包）并生成 DBNet 检测模型 `dbnet_detect.onnx`（约 306 MB，导出需要 Python 3.10+ 与 torch，详见 [docs/dbnet.md](docs/dbnet.md)），然后把 `onnxruntime.dll` 复制到 `build\Release\`。随后重跑 `build.bat` 让 CMake 探测到 onnxruntime 并编入 dbnet 支持（若先运行脚本后构建，构建完再运行一次脚本补齐 DLL 即可）。

缺少任一层依赖（编译宏 / `onnxruntime.dll` / 检测模型）时程序会自动降级跳过检测，**不影响正常 PSD 生成**。

## 快速上手

```bat
lp2psd.exe 排版.txt
```

`排版.txt` 为 LabelPlus 排版文件，由若干“图片块”组成：

- `>>>>>>>>[01.jpg]<<<<<<<<` 开启一个图片块，方括号内是相对 txt 所在目录的图片文件名（可带子目录）；
- 块内每条排版记录形如 `----------------[1]----------------[0.203,0.043,2]`（条目序号、归一化坐标 x/y、分组号），记录后跟若干行文本。

程序为每个图片块生成一个 `<图片名>.psd`，默认输出到 txt 所在目录下的 `output\`（自动创建；`--out` 可指定其它目录）。全部成功后会打开输出目录。不带参数运行时弹出文件选择框。

输入格式的完整规范见 [docs/labelplus-format.md](docs/labelplus-format.md)。

### 命令行参数

| 参数 | 说明 |
|---|---|
| `--out <目录>` | 输出目录；默认是 txt 文件所在目录下的 `output` |
| `--config <json>` | 样式配置文件；默认读取 exe 同目录的 `config.json`，不存在时自动生成模板并读取 |
| `--debug-dbnet <目录>` | 输出 DBNet 检测叠加图、遮罩与 JSON（开启 dbnet 后用于调参） |
| `--help` / `-h` | 显示帮助 |

退出码：全部成功为 `0`；有图片生成失败为 `1`；参数错误或取消选择文件为 `2`。

## 生成的 PSD 图层结构

每个输出文件自下而上依次为：

```text
bg              整幅背景图（像素图层）
bg 拷贝         可选：背景备份图层（config 的 bgCopy 节）
whites          可选：涂白图层（dbnet 检测到日文时生成）
组 1 / 组 2 …   文本图层按分组归组，组内顺序与排版记录一致
（无分组记录）    直接放在顶层
dbnet_boxes     可选：检测框描边图层（config 的 dbnet.boxes 节，位于最顶）
```

## 样式配置

程序按以下顺序寻找配置：`--config` 指定的文件 → exe 同目录的 `config.json` → 都不存在时首次运行自动生成一份模板并读取。

配置可控制画布 DPI、字体、字号、颜色、抗锯齿、排版方向、对齐、行距、自由连字、图层不透明度，以及 dbnet 涂白、描边与背景拷贝图层等。仓库根的 [config.json](config.json) 是全参数示例；全部字段的默认值与取值见 [docs/config.md](docs/config.md)。

## 已知限制

- **PS 打开后需手动“更新所有文本图层”**：写入的 EngineData 不含 Photoshop 排版引擎使用的字形网格数据，打开时文本引擎缓存过期，需用 文字(Y) → 更新所有文本图层(U) 自行重排（预览像素不受影响）。
- **Windows-only**：依赖 wmain、GDI+、Win32 对话框与系统 ANSI 代码页。
- 仅生成点文本图层与 RGB 8-bit PSD。

更多限制与改进方向见 [docs/roadmap.md](docs/roadmap.md)。

## 文档

用户文档：

- [config.json 配置](docs/config.md) — 全部字段、默认值与取值范围
- [LabelPlus 排版 txt 格式](docs/labelplus-format.md) — 解析器支持的文件结构
- [DBNet 检测与自动涂白](docs/dbnet.md) — 原理、依赖安装与调参

开发文档：[docs/README.md](docs/README.md) 

## 许可证与致谢

本项目以 [GPL-2.0](LICENSE) 许可发布。

- 排版 txt 格式来自 [LabelPlus](https://github.com/LabelPlus/LabelPlus)（GPL-2.0）；lp2psd 为其格式的独立实现。
- 可选检测功能使用 [manga-image-translator](https://github.com/zyddnys/manga-image-translator) 的 DBNet 检测器方案（ResNet34 + DB head，Apache-2.0），导出接口与 yakuyomi-engine 一致。
- 开发调试使用 [psd-tools](https://psd-tools.readthedocs.io/)、[ag-psd](https://github.com/Agamnentzar/ag-psd) 与 [ONNX Runtime](https://github.com/microsoft/onnxruntime)。
