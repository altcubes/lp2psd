# lp2psd

根据 [LabelPlus](https://github.com/LabelPlus/LabelPlus) 排版 txt 生成多图层 PSD文件的 Windows 命令行工具。另可选用 DBNet 检测日文原文，生成涂白图层，省去嵌字流程里手动擦字的重复劳动。

> ⚠️ **注意**：生成的 PSD 在 Photoshop 中打开后，需手动刷新文本排版：菜单 **文字(Y) → 更新所有文本图层(U)**。

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

## 快速上手

```bat
lp2psd.exe 排版.txt
```

`排版.txt` 为 LabelPlus 排版文件
程序为每个图片块生成一个 `<图片名>.psd`，默认输出到 txt 所在目录下的 `output\`（自动创建；`--out` 可指定其它目录）。不带参数运行时弹出文件选择框。

输入格式的完整规范见 [docs/labelplus-format.md](docs/labelplus-format.md)。

### 命令行参数

| 参数 | 说明 |
|---|---|
| `--out <目录>` | 输出目录；默认是 txt 文件所在目录下的 `output` |
| `--config <json>` | 样式配置文件；默认读取 exe 同目录的 `config.json`，不存在时自动生成模板并读取 |
| `--debug-dbnet <目录>` | 输出 DBNet 检测叠加图、遮罩与 JSON|
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


## 文档

用户文档：

- [config.json 配置](docs/config.md) — 全部可配置字段、默认值与取值范围
- [LabelPlus 排版 txt 格式](docs/labelplus-format.md) — 解析器支持的文件结构


## 许可证与致谢

本项目以 [GPL-2.0](LICENSE) 许可发布。

- 排版 txt 格式来自 [LabelPlus](https://github.com/LabelPlus/LabelPlus)（GPL-2.0）；lp2psd 为其格式的独立实现。
- 可选检测功能使用 [manga-image-translator](https://github.com/zyddnys/manga-image-translator) 的 DBNet 检测器方案（ResNet34 + DB head，Apache-2.0），导出接口与 yakuyomi-engine 一致。
- 开发调试使用 [psd-tools](https://psd-tools.readthedocs.io/)、[ag-psd](https://github.com/Agamnentzar/ag-psd) 与 [ONNX Runtime](https://github.com/microsoft/onnxruntime)。
