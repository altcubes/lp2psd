# DBNet 检测与自动涂白

lp2psd 的可选 dbnet 功能用 **manga-image-translator (m-i-t) 的 DBNet 检测器**（ResNet34 + DB head，即 yakuyomi-engine 的 Detector 方案）定位图片中的日文文本，生成**涂白图层**覆盖原文：

1. **逐像素笔画遮罩**——检测器额外输出文字笔画 mask，涂白图层只覆盖笔画本身（外扩 `whiten.margin` 像素盖住抗锯齿边缘），不再是一整块白色矩形。深色背景上的文字涂白后也不再是显眼白块。
2. **旋转四边形文字行**——每条文字行给四个角点（`dbnetBox.quad`），支持横排/竖排/倾斜文本；`dbnet.boxes` 描边图层按四边形描边，`--debug-dbnet` 调试图也画四边形。

只做**检测**，不做内容识别；因此只需一个 DBNet 检测模型，没有识别模型和解码管线。DBNet 是语言无关的行/列区域检测，对日文竖排（漫画主流排版）与横排均有效。

## 依赖安装

```bat
scripts\setup_dbnet.bat
```

脚本下载两样东西（gitignore，不入库）：

| 依赖 | 位置 | 说明 |
|---|---|---|
| onnxruntime 预编译包(1.19.2) | `third_party\onnxruntime\` | 只用头文件；`onnxruntime.dll` 由脚本复制到 `build\Release\` |
| DBNet 检测模型(ONNX) | 仓库根 `dbnet_detect.onnx` | m-i-t `detect-20241225.ckpt` 导出（fp32，约 306MB） |

模型不是直接下载的，而是由 [scripts/export_dbnet_onnx.py](../scripts/export_dbnet_onnx.py) 从上游权重导出（sha256 校验后转 ONNX），需要 Python 3.10+ 与 torch/torchvision：

```bat
pip install torch torchvision --index-url https://download.pytorch.org/whl/cpu
python scripts\export_dbnet_onnx.py dbnet_detect.onnx
```

导出脚本会自动克隆/复用 `third_party/manga-image-translator` 并下载 `detect-20241225.ckpt`（约 294MB，来自 m-i-t beta-0.3 release，sha256 `67ce1c4e…dc502e`）。模型接口与 yakuyomi-engine 的 NCNN 版完全一致：

```
in0  [1,3,H,W]  RGB、NCHW、normalize (x/127.5 - 1)
out0 [1,2,H,W]  db（全分辨率）：ch0 = shrink_map 原始 logits（未 sigmoid）、
                ch1 = threshold_map（已 sigmoid）
out1 [1,1,H/2,W/2] mask：逐像素文字笔画遮罩（已 sigmoid）
```

⚠️ 不要用仓库里旧的 `db_resnet34-*.onnx`（单通道 logits、固定 1024×1024 输入）：那是已退役的 comic-text-detector 模型，没有笔画 mask 输出。旧 `dbnet_det.onnx`（PP-dbnet DB）也已不再使用。

## config.json 配置

```json
"dbnet": {
  "enabled": false,                  // 总开关,默认关闭
  "model": "dbnet_detect.onnx",      // 相对 exe 目录,找不到时回落当前目录;绝对路径直接用
  "whiten": {
    "enabled": true,                 // 涂白图层开关
    "color": [255, 255, 255],        // 涂白颜色(RGB)
    "margin": 3,                     // 笔画遮罩外扩像素(盖抗锯齿)
    "layerName": "whites"
  },
  "boxes": {
    "enabled": false,                // 四边形描边图层开关(图层栈最顶部)
    "color": [255, 0, 0],
    "layerName": "dbnet_boxes"
  }
}
```

调参项（写在 `dbnet` 节，一般不用动）：

| 字段 | 默认 | 说明 |
|---|---|---|
| `limitSideLen` | 1024 | 推理前长边缩放目标（保持纵横比，pad 到 256 倍数）。调大更精细、更慢 |
| `dbBinThreshold` | 0.5 | sigmoid(db ch0) 二值化阈值（m-i-t `text_threshold`）。调高 → 框更保守 |
| `dbBoxThreshold` | 0.7 | 连通域平均概率低于此值丢弃（m-i-t `box_threshold`） |
| `dbUnclipRatio` | 2.3 | DB unclip：d = area×ratio/perimeter，框外扩比例（m-i-t `unclip_ratio`） |
| `minSide` | 3.0 | 模型网格空间内短边小于此值的框丢弃 |
| `segThreshold` | 0.12 | 笔画遮罩二值化阈值（mask 概率 > 此值算笔画） |
| `minBoxArea` | 64 | 原图像素面积小于此值的框丢弃，过滤小误检 |

旧字段兼容：`detThresh` 仍可作为 `dbBinThreshold` 的别名读取；`unclipPx`（固定像素膨胀）已由 `dbUnclipRatio`（比例膨胀）取代，不再读取。

## 检测管线（src/dbnet.cpp）

```
RGBA 原图
 → 等比缩放(长边 = limitSideLen)+ pad 右/下到 256 倍数(黑)
 → float32 CHW, (x/127.5 - 1), RGB 通道序
 → ONNX Runtime CPU 推理
   ├─ out0 db → sigmoid(ch0) → 二值化(dbBinThreshold)
   │    → 8 连通域 → 边界点 → 凸包旋转卡尺 minAreaRect
   │    → unclip(dbUnclipRatio) → 旋转四边形(原图坐标) → 过滤 → dbnetBox.quad
   └─ out1 mask → 裁有效区 → 双线性放大回原图 → 阈值(segThreshold)
        → 逐像素笔画遮罩 stroke_mask
```

后处理逐行对照 yakuyomi-engine 的 `Detector.kt` / `Geometry.kt`（它们又 port 自 manga_translator `default_utils`），可运行 [scripts/detect_parity.py](../scripts/detect_parity.py) 验证 C++ 输出与 Python 参考一致。Session 按进程缓存（同一模型只加载一次）；每张 1351×1920 图片推理约 1-2s（CPU，含模型首次加载）。

## 调试

```bat
lp2psd.exe text.txt --config config.json --debug-dbnet build\dbnetdbg
```

`--debug-dbnet <dir>` 对每张图输出三个文件：

- `<图片名>_dbnet.png`：**绿 = 笔画遮罩、红 = 旋转四边形框**——调 `dbBinThreshold`/`dbBoxThreshold`/`segThreshold` 的主要手段；
- `<图片名>_mask.png`：灰度笔画遮罩；
- `<图片名>_quads.json`：机器可读的四边形与分数（供 parity 对比）。

## 已知限制

- 深色背景上的文字涂白后是白色笔画（纯白填充的固有局限）；比整块白矩形自然得多，但仍可在 PS 中改色或取样背景色。
- 遮罩偶尔覆盖到网点/气泡等小图案，会造成小面积涂白；可调大 `minBoxArea` 或调高 `segThreshold` 过滤。
- 手写体/艺术字可能漏检或遮罩不完整；漏检的文字不会被涂白（宁漏勿错，原图层始终未被修改，可在 PS 中手动补）。
