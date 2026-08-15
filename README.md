# lp2psd

根据 [LabelPlus](https://github.com/LabelPlus/LabelPlus) 生成的txt文件快速生成多图层 Photoshop PSD 的命令行工具
输出可编辑文本图层（TySh + EngineData）、图层分组、GDI+ 预览合成图。

**⚠️**该程序生成的psd文件需要在ps中手动更新文本图层:文字(Y)->更新所有文本图层(U)
**⚠️**LabelPlus生成的txt文件应与图片文件在同一文件夹中

## 构建说明

### 依赖

- Windows + Visual Studio 2022
- CMake ≥ 3.15

### 一键构建

```bat
build.bat
```

### 手动构建（CMake）

```bat
cmake -S . -B build
cmake --build build --config Release
```

## 使用说明

### 基本用法

```bat
lp2psd.exe text.txt
```

### 命令行参数

| 参数 | 说明 |
|---|---|
| `--out <目录>` | 输出目录；默认是 txt 文件所在目录下的 `output`（不存在会自动创建） |
| `--config <json>` | 样式配置文件；默认读取程序同目录的 `config.json`，不存在时自动生成模板并读取 |
| `--help` / `-h` | 显示帮助 |

### 样式配置（config.json）

程序同目录的 `config.json`（也可用 `--config` 指定其它文件）。不存在时首次运行
会自动生成一份模板，修改后重新运行即可生效。示例：

```json
{
  "dpi": "original",
  "font": {
    "name": "Microsoft YaHei",
    "fontSize": 24,
    "color": [0, 0, 0],
    "antiAlias": "smooth",
    "orientation": "vertical",
    "justification": "center",
    "autoLeading": true,
    "autoLeadingSize": 1.2,
    "leading": 0,
    "discretionaryLigatures": true,
    "standardVerticalRomanAlignment": true
  }
}
```

常用字段：

| 字段 | 说明 |
|---|---|
| `dpi` | 画布分辨率；`"original"` 使用原图 DPI（默认），也可填固定数值（如 `96`），所有图片统一使用该分辨率 |
| `font.name` | 字体名；字体 PostScript 名 |
| `font.fontSize` | 字号（pt） |
| `font.color` | RGB 颜色数组 |
| `font.antiAlias` | 抗锯齿：`none`/`crisp`/`strong`/`smooth`/`sharp`/`lcd`（或 0/1/2/3/4/6） |
| `font.orientation` | 文本方向：`horizontal` / `vertical` |
| `font.justification` | 对齐：`left`/`right`/`center`/`justifyAll` 等（或数字 0~6） |
| `font.autoLeading` | 是否自动行距：`true`/`false` |
| `font.autoLeadingSize` | 自动行距倍数 |
| `font.leading` | 手动行距（pt），`autoLeading` 为 `false` 时生效 |
| `font.discretionaryLigatures` | 自由连字：`true`/`false` |
| `font.standardVerticalRomanAlignment` | 标准垂直罗马对齐（竖排时生效）：`true`/`false` |
| `font.postScript` | 指定写入 PSD 的 PostScript 字体名 |
| `font.script` | 可选，EngineData 字体 Script：`auto` 或 0~4（罗马/日文/繁体中文/简体中文/韩文） |
