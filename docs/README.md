# lp2psd 开发文档

lp2psd:根据 [LabelPlus](https://github.com/LabelPlus/LabelPlus) 排版 txt 生成多图层 Photoshop PSD(可编辑文本图层 TySh + EngineData、分组、GDI+ 预览)的 Windows 命令行工具。C++17,零第三方依赖。

用户向的使用说明(config.json 字段、命令行参数)见根目录 [README.md](../README.md);本目录面向开发者。

## 阅读顺序建议

| 顺序 | 文档 | 内容 |
|---|---|---|
| 1 | [architecture.md](architecture.md) | 架构总览:数据流、模块分层、坐标/单位模型(**先读这篇**) |
| 2 | [modules.md](modules.md) | 模块参考:逐文件职责、关键函数、file:line 速查 |
| 3 | [config.md](config.md) | config.json 全参数配置文档(字段、默认值、取值) |
| 4 | [labelplus-format.md](labelplus-format.md) | LabelPlus 排版 txt 格式规范(解析器行为) |
| 5 | [psd-internals.md](psd-internals.md) | PSD 字节级机制:TySh、EngineData、lrFX、取值映射表 |
| 6 | [ocr.md](ocr.md) | OCR 日文检测:自动涂白、依赖与调参 |
| 7 | [building.md](building.md) | 构建、冒烟测试、psd-tools 调试方法 |
| 8 | [roadmap.md](roadmap.md) | 已知限制、Txt2 历史教训、改进方向 |

只想快速改一行代码:读 architecture.md 的"坐标与单位模型"和"关键代码位置"即可;要动 `psd_writer.cpp` 或排版逻辑,务必再读 psd-internals.md 与 modules.md 的对应节。

## 开发者速记

- 构建:`build.bat`(或 `cmake -S . -B build && cmake --build build --config Release`)。
- 运行:`lp2psd.exe <layout.txt> [--out <dir>] [--config <style.json>]`。
- 测试素材在本机 `testfile/`(git 忽略);调试脚本在 `build/diag/`(git 忽略),重建方法见 building.md。
- 改排版估算逻辑时,`line_units`(main.cpp)与 `est_line_units`(psd_writer.cpp)**必须同步改**。
- 源码与注释为 UTF-8 + 中英混合,`/utf-8` 编译选项不可移除。
