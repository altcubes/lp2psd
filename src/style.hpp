#pragma once
// style.hpp - style configuration model and JSON loading.

#include <cstdint>
#include <string>

#include "minijson.hpp"

// dbnet feature settings (config "dbnet" node). Disabled by default; enabling
// requires onnxruntime.dll + the detection model (see docs/dbnet.md).
struct dbnetWhiten {
    bool enabled = true;
    uint8_t color[3] = {255, 255, 255};
    // Stroke (mask) expansion before painting, px: swallows anti-aliasing
    // edges around the detected strokes.
    int margin = 3;
    // Detection-box expansion along the image axes, px (horizontal/vertical).
    // Shared with the dbnet_boxes outline layer; when limit_to_boxes is on,
    // whitening may only extend this far from a detected quad.
    int box_margin_x = 3;
    int box_margin_y = 3;
    // true (default): whiten = dilate(stroke_mask, margin) ∩
    // expand(boxes, box_margin_x/box_margin_y). false: legacy full-mask
    // dilation only.
    bool limit_to_boxes = true;
    std::string layer_name = "whites";
    double opacity = 100.0;        // layer opacity, 0-100 percent
};

// Outline layer of the detection boxes (top of the layer stack) for quickly
// locating whitened regions in Photoshop. Disabled by default. When `lock`
// is set, the layer is fully locked (transparency + composite + position),
// equivalent to Photoshop's "Lock all".
struct dbnetBoxes {
    bool enabled = false;
    uint8_t color[3] = {255, 0, 0};
    std::string layer_name = "dbnet_boxes";
    bool lock = false;             // full lock (Lock all) on this layer only
};

// Optional copy of the background ("bg 拷贝") inserted directly above "bg"
// and below the "whites" whitening layer.
struct BgCopy {
    bool enabled = false;
    std::string layer_name = "bg 拷贝";
};

struct dbnetConfig {
    bool enabled = false;
    std::string model = "dbnet_detect.onnx";  // relative to the exe directory
    int limit_side_len = 1024;  // long-side resize limit (detect input size)
    double det_thresh = 0.5;    // sigmoid(db) binarization (dbBinThreshold)
    double box_thresh = 0.7;    // drop lines with mean prob below this
    double unclip_ratio = 2.3;  // DB unclip (area*ratio/perimeter)
    double min_side = 3.0;      // drop lines with a model-grid side < this
    double seg_thresh = 0.12;   // stroke-mask binarization threshold
    int min_box_area = 64;
    dbnetWhiten whiten;
    dbnetBoxes boxes;
};

// Layer-wide settings (config "layers" node) applied to every generated
// layer: the background, its copy, whitening / outline helper layers and
// text layers.
struct LayerSettings {
    double opacity = 100.0;  // 0-100 percent, 100 = fully opaque
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
    dbnetConfig dbnet;
    BgCopy bg_copy;
    LayerSettings layers;
};

// Builds a Style from a parsed JSON config value (a bare `null` yields the
// built-in defaults).
Style load_style(const mjson::Value& cfg);
