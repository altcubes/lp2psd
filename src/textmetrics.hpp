#pragma once
// textmetrics.hpp - shared rough text layout estimation.
//
// Used by main.cpp (layer box geometry) and psd_writer.cpp (TySh em bounds)
// so the two can never drift apart again. The estimates are intentionally
// rough: CJK glyphs count as 1.0 em, Latin as 0.55 em.

#include <string>
#include <vector>

namespace textmetrics {

// Rough text width in em units (CJK glyphs = 1.0 em, Latin = 0.55 em).
double line_units(const std::string& line);

// UTF-16 code unit count of a UTF-8 string (4-byte chars count as 2).
int utf16_length(const std::string& s);

struct Box {
    double w = 0.0;
    double h = 0.0;
};

// Estimates the text-layer box (document px) for horizontal/vertical text.
// Vertical: one em column per line, columns advance right-to-left; the
// column width uses `vertical_col_scale`, kept in sync with the GDI+ preview
// renderer's column advance (image.cpp).
Box estimate_box(const std::vector<std::string>& lines, int orientation,
                 double font_size_px, double leading_px,
                 double vertical_col_scale = 1.35);

}  // namespace textmetrics
