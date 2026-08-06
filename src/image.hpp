#pragma once
// image.hpp - GDI+ image loading and text-preview rendering.
//
// psdgen renders a raster preview of every text layer so any viewer shows the
// text; the editable text itself lives in the TySh/EngineData blocks.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// RAII wrapper around GdiplusStartup/GdiplusShutdown.
class GdiScope {
public:
    GdiScope();
    ~GdiScope();
    GdiScope(const GdiScope&) = delete;
    GdiScope& operator=(const GdiScope&) = delete;
private:
    ULONG_PTR token_ = 0;
};

// Loads an image as RGBA8 (row-major). Also reports the embedded resolution
// (DPI) when requested; defaults to 96.
bool load_image(const std::wstring& path, std::vector<uint8_t>& rgba, int& w,
                int& h, double* res_h = nullptr, double* res_v = nullptr);

// Renders `lines` into an RGBA8 buffer the size of the layer record.
// Returns false when the size is invalid.
bool render_text_preview(const std::wstring& font_name, double size_px,
                         int w, int h, const uint8_t color[3], int orientation,
                         int justification, double line_advance,
                         const std::vector<std::wstring>& lines,
                         std::vector<uint8_t>& rgba);
