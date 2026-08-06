#pragma once
// style.hpp - style configuration model and JSON loading.

#include <cstdint>
#include <string>

#include "minijson.hpp"

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
    std::string output_dir;
    std::string prefix;
    std::string suffix;
};

// Builds a Style from a parsed JSON config value (a bare `null` yields the
// built-in defaults).
Style load_style(const mjson::Value& cfg);
