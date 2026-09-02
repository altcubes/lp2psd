# 引入 CTD det+seg(+blk) 后端,双引擎对比

## 调研结论(已核实)

CTD(Comic Text Detector,dmMaze,GPL-3.0)是漫画专用检测器,YOLOv5 blk 头 + UNet seg 头 + DBNet det 头三合一:

- **模型**:`comictextdetector.pt.onnx`(~90MB),下载:`https://github.com/zyddnys/manga-image-translator/releases/download/beta-0.2.1/comictextdetector.pt.onnx`(已确认存在)
- **输入**:1024×1024 letterbox(stride 64)、RGB、/255 归一化(CHW float32)
- **输出 3 个张量**(onnxruntime 运行时按名字/形状识别):`blks`(YOLOv5 检测张量,需自实现 NMS:conf 0.4 / IoU 0.35,坐标 ×letterbox ratio)、`mask`(UNet 文字分割图)、`lines_map`(DBNet 概率图;两者可能顺序颠倒,按 shape 校验)
- **后处理阈值**:det mask @0.3,DB 线图 thresh 0.3/box 0.6——与现有管线兼容
- 对我们的意义:CTD 的 det/seg 掩码是**漫画专项训练**的,假名/竖排/网点背景的覆盖预期比通用 PP-OCR det 好;seg 掩码接近逐字 mask,天然适合单字格字号估计;blk 直接给气泡文本块框

## 实现步骤

1. **scripts/setup_ocr.bat**:追加可选参数 `ctd`(`setup_ocr.bat ctd` 时下载 CTD 模型到仓库根,90MB,默认不下载)
2. **src/ocr.hpp / ocr.cpp**:
   - `OcrOptions` 增加 `engine`("paddle" | "ctd")与 `ctd_model_path`
   - CTD 路径:letterbox 预处理 → 单次推理 → 按 shape 识别 3 个输出(blk=[1,N,6] 末维、mask/lines_map=[1,1or2,1024,1024])→ det 掩码 @0.3 二值化 → **复用现有管线**(膨胀 CC → 投影切分 → 涂白框;半径 1 CC → 低墨切分 → 单字格字号框)
   - blk 头:C++ 实现精简 YOLOv5 NMS(conf 0.4/IoU 0.35,cxcywh→xyxy),blk 框仅进调试图(第三种颜色)供人工评估,暂不参与涂白
   - session 缓存按 (engine, model_path) 区分,paddle/CTD 会话共存
3. **src/style.hpp/cpp + 模板 config**:`ocr.engine`("paddle" 默认 | "ctd")、`ocr.ctdModel`("comictextdetector.pt.onnx");engine 无效值回落 paddle
4. **src/main.cpp**:`--debug-ocr` 时若两个模型文件都存在,额外生成 `<stem>_paddle.png` / `<stem>_ctd.png` **对比图**(临时切换 engine 各跑一遍,互不影响当前 engine 的正式输出);`ocr_detect` 调用处按 engine 传参
5. **实测对比**:用 `31—34.txt` + testfile 分别生成两引擎的调试图与 PSD,逐条核对字号、观察涂白覆盖(假名/竖排/网点背景),给出**书面对比分析**
6. **决策交付**:基于实测给出"混用 vs 选优"建议及理由,**最终由你定夺**;若定混用,后续按建议实现混合策略(如 CTD 涂白 + Paddle 字号,或按区域置信度选优)——本计划不含混合实现

## 验证

- 无 CTD 模型时 engine=ctd 报清晰错误并回落 paddle;engine=paddle 行为与现状完全一致(回归 testfile)
- 两引擎调试图并排人工比对(31—34 四页重点:假名覆盖、竖排列、网点背景误检)
- psd-tools 抽查两引擎 PSD 的字号分布

## 文档

docs/ocr.md 增 CTD 章节:engine 配置、模型下载、预处理/后处理差异、GPL-3.0 许可注明、性能预期(90MB 模型,1024² 推理慢于 Paddle);modules.md、根 README 同步;setup_ocr.bat 用法更新。