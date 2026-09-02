#pragma once
// style.hpp - style configuration model and JSON loading.

#include <cstdint>
#include <string>

#include "minijson.hpp"

// OCR feature settings (config "ocr" node). Disabled by default; enabling
// requires onnxruntime.dll + the detection model (see docs/ocr.md).
struct OcrWhiten {
    bool enabled = true;
    uint8_t color[3] = {255, 255, 255};
    int margin = 3;                // box expansion before painting, px
    std::string layer_name = "whites";
};

// Outline layer of the detection boxes (top of the layer stack) for quickly
// locating whitened regions in Photoshop. Disabled by default.
struct OcrBoxes {
    bool enabled = false;
    uint8_t color[3] = {255, 0, 0};
    std::string layer_name = "ocr_boxes";
};

struct OcrConfig {
    bool enabled = false;
    std::string model = "dbnet_detect.onnx";  // relative to the exe directory
    int limit_side_len = 1024;  // long-side resize limit (detect input size)
    double det_thresh = 0.5;    // sigmoid(db) binarization (dbBinThreshold)
    double box_thresh = 0.7;    // drop lines with mean prob below this
    double unclip_ratio = 2.3;  // DB unclip (area*ratio/perimeter)
    double min_side = 3.0;      // drop lines with a model-grid side < this
    double seg_thresh = 0.12;   // stroke-mask binarization threshold
    int min_box_area = 64;
    OcrWhiten whiten;
    OcrBoxes boxes;
};

struct Style {
    std::wstring font_name = L"Microsoft YaHei";
    std::string font_ps = "MicrosoftYaHei";
    double font_size_pt = 24.0;  // font size in points, as in Photoshop
    uint8_t color[3] = {255, 255, 255};
    // Text style settings (written into TySh / EngineData)
    int anti_alias = 2;            // 0=None 1=Crisp 2=Strong 3=Smooth 4=Sharp 6=LCD
    int orientation = 0;           // 0=horizontal 1=vertical
    int justification = 0;         // 0=Left 1=Right 2=Center ... 6=JustifyAll
    bool auto_leading = true;      // auto leading on/off
    double auto_leading_size = 1.2; // auto leading multiplier
    double leading = 0.0;          // explicit leading in points when auto leading is off
    bool discretionary_ligatures = false;
    bool standard_vertical_roman = true;
    // EngineData FontSet Script: -1 = auto-detect from font name,
    // 0 = Roman, 1 = Japanese, 2 = Traditional Chinese,
    // 3 = Simplified Chinese, 4 = Korean.
    int script = -1;
    // Document DPI: 0 = keep the source image's own resolution (原图值);
    // > 0 = fixed document resolution (pixels/inch) overriding the image.
    double dpi = 0.0;
    std::string output_dir;
    std::string prefix;
    std::string suffix;
    OcrConfig ocr;
};

// Builds a Style from a parsed JSON config value (a bare `null` yields the
// built-in defaults).
Style load_style(const mjson::Value& cfg);
