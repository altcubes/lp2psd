#include "textmetrics.hpp"

#include <algorithm>
#include <cstdint>

namespace textmetrics {

double line_units(const std::string& line) {
    double units = 0;
    size_t i = 0;
    while (i < line.size()) {
        uint8_t c = (uint8_t)line[i];
        if (c < 0x80) {
            units += 0.55;
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
            units += 0.55;
        } else if ((c & 0xF0) == 0xE0) {
            i += 3;
            units += 1.0;
        } else {
            i += 4;
            units += 1.0;
        }
    }
    return units;
}

int utf16_length(const std::string& s) {
    int n = 0;
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) {
            i += 1;
            n += 1;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
            n += 1;
        } else if ((c & 0xF0) == 0xE0) {
            i += 3;
            n += 1;
        } else if ((c & 0xF8) == 0xF0) {
            i += 4;
            n += 2;
        } else {
            i += 1;
            n += 1;
        }
    }
    return n;
}

Box estimate_box(const std::vector<std::string>& lines, int orientation,
                 double font_size_px, double leading_px,
                 double vertical_col_scale) {
    Box b;
    double max_units = 1.0;
    int max_chars = 1;
    for (const auto& ln : lines) {
        max_units = std::max(max_units, line_units(ln));
        max_chars = std::max(max_chars, utf16_length(ln));
    }
    if (orientation == 1) {
        b.w = std::max((double)lines.size() * font_size_px * vertical_col_scale,
                       font_size_px * vertical_col_scale);
        b.h = std::max((double)max_chars * font_size_px, font_size_px);
    } else {
        b.w = std::max(max_units * font_size_px, font_size_px);
        b.h = std::max((double)lines.size() * leading_px, leading_px);
    }
    return b;
}

}  // namespace textmetrics
