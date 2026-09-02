# 构建与调试指南

## 构建

依赖:Windows 10+、Visual Studio 2022(MSVC,C++17)、CMake ≥ 3.15。无任何第三方 C++ 依赖,不需要 vcpkg/Conan。

```bat
:: 一键构建
build.bat

:: 等价手动步骤
cmake -S . -B build
cmake --build build --config Release
```

产物:`build\Release\lp2psd.exe`。编译选项见 `CMakeLists.txt`(`/utf-8 /W3`;链接 `gdiplus`、`comdlg32`、`shell32`)。

> 注意源码大量使用 UTF-8 中文注释与字符串字面量(如 Kinsoku 表、字体映射),`/utf-8` 不可移除。

### 可选 dbnet 支持

CMake 自动探测 `third_party/onnxruntime/include/onnxruntime_c_api.h`:存在则定义 `LP2PSD_WITH_dbnet` 并编译 `src/dbnet.cpp`,否则静默跳过(dbnet 相关代码以桩函数降级)。运行期 `onnxruntime.dll` 与检测模型也缺失时功能自动禁用——三层降级,构建和基础功能永远可用。依赖获取与原理见 [dbnet.md](dbnet.md)。

## 运行与冒烟测试

```bat
lp2psd.exe path\to\layout.txt            :: 输出到 txt 同目录 output\
lp2psd.exe layout.txt --out D:\tmp --config my.json
lp2psd.exe                               :: 无参数弹文件选择框
```

仓库没有入库的测试素材;`testfile/`(被 .gitignore 忽略)在本机存放手工测试用例:

| 文件 | 用途 |
|---|---|
| `text1.txt` + `01.jpg`/`02.jpg` | 标准样例:两个图片块、分组 1/2、横竖排混合 |
| `test_edge.txt`、`e1_blank_pixels.psd`、`e2_old_bbox.psd`、`e3_old_glyphs.psd` | 边界案例:空白像素、旧版 bbox、旧版字形数据 |
| `ps_ref_*.psd` | Photoshop 生成的**参照文件**(字节对照基准) |
| `gen_*.psd` | 本工具的历史输出 |

冒烟流程:`lp2psd.exe testfile\text1.txt` → 确认输出目录出现 `01.psd`、`02.psd` → 用 Photoshop(或 psd-tools)检查图层结构与文本。

## 自动化测试现状

**没有正式测试框架**。`psd_writer_internal.hpp:107-108` 预留了 `build_tysh` / `build_lrfx` 两个纯函数构建器(注释 "exposed for writer tests"),意图是为字节级单元测试提供入口——新增测试时建议从这两个函数起步(给定 `TextLayerData` → 断言字节/解析结果),无需启动 GDI+ 或文件 IO。

## 调试:psd-tools 对比法

核心调试手段是用 Python 库 [psd-tools](https://psd-tools.readthedocs.io/) 解析本工具输出与 Photoshop 参照,逐字段对比:

```bash
pip install psd-tools
```

常用手法:

```python
from psd_tools import PSDImage

psd = PSDImage.open("output/01.psd")
for group in psd:
    print(group.name, group.kind)
    for layer in group:
        print(" ", layer.name, layer.offset, layer.size)
        if layer.kind == "type":
            print(layer.text)          # Txt / bounds / boundingBox
            # engine_data 里是完整 EngineDict / ResourceDict
```

另可用 [ag-psd](https://github.com/Agamnentzar/ag-psd)(npm 包,本地留存 `build/diag/ag-psd-28.4.1.tgz`)交叉验证。

### build/diag/ 脚本

`build/diag/` 是开发期累积的**一次性诊断脚本集**(被 .gitignore 忽略,不入库;以下是当时的用途,文件可能随清理变动):

| 类别 | 脚本 | 用途 |
|---|---|---|
| 导出 | `decode_engine.py`、`dump_tysh.py`、`dump_layers.py`、`dump_one.py` | 从 PSD 解出 EngineData 文本 / TySh / 图层树 |
| 对比 | `diff_psd.py`、`diff2.py`、`diff3.py`、`skeleton_diff.py` | 本工具输出 vs 参照 PSD 的结构化 diff |
| 探针 | `probe_*.py` | 探测 psd-tools 对各类型/类 ID 的解析行为 |
| 构造 | `make_variants.py`、`patch_text_*.py`、`add_txt2_to_gen.py` | 生成变体 PSD / 往 PSD 里打补丁做实验 |

因为不入库,重建调试环境的路径是:写一个最小 dump 脚本(上面 10 行即可)→ 对比 `ps_ref_*.psd` 与新生成文件的同名 tagged block。

### 调试技巧

- **EngineData 文本 diff 最有效**:TySh 的 `EngineData`(tdta)是纯文本标记语言,直接提取后与参照文件 diff;本工具的序列化刻意与 psd-tools 的输出字节级一致,所以差异即语义差异。
- **字段对照表**在 [psd-internals.md](psd-internals.md)(抗锯齿/对齐/Script/Procession 等)。
- **DPI 相关问题**先核对三处换算是否一致:main.cpp `px_scale`、EngineData FontSize(pt×dpi/72)、图像资源 0x03ED。
- **图层矩形对不上**时检查 `llround` 链路:main.cpp:227-230 与 psd_writer.cpp:1382-1387 必须给出相同矩形。
- **预览与文本错位**:预览由 GDI+ 渲染,与 PS 排版引擎结果本就有细差;若预览本身错,查 `render_text_preview`(竖排列宽 1.35 系数、回退字体)与 `make_text_layer` 的框尺寸估算。

## 版本发布注意

- 产物仅 `lp2psd.exe` + 可选 `config.json`;exe 同目录无 config.json 时首次运行自动生成模板。
- `.gitignore` 忽略 `build/`、`testfile/`、`out/`、`output/`、`testout/`——发布前确认别把测试素材带进去。
