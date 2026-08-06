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

// Resolves the glyph indices (font cmap) of `text` using the Windows GDI
// font engine. Returns false when the DC/font could not be created or the
// call failed. Glyph indices are what Photoshop stores in the Txt2 block
// (the text engine data) and must match the layer text exactly, otherwise
// Photoshop asks to update the text layer on open.
bool get_glyph_indices(const std::wstring& font_name, const std::wstring& text,
                       std::vector<uint16_t>& glyphs);

// Resolves the Windows font family name for a font given its PostScript
// name (for example "YWHeiTI-Medium" -> "YW HeiTI Medium"). Uses the
// DirectWrite system font collection, which also covers per-user fonts.
// Returns false when no installed font carries that PostScript name.
bool resolve_font_family(const std::wstring& ps_name, std::wstring& family);

// Checks whether GDI can load `family` without falling back to another face
// (compares the selected face name with the requested one, ignoring spaces,
// hyphens and case).
bool font_family_available(const std::wstring& family);

// Returns font layout metrics as fractions of the em square:
//   ascent_em, descent_em  - the line box used by Photoshop for this font
//                            (taken from Photoshop-saved reference files for
//                            Microsoft YaHei; approximated for other fonts)
//   space_advance_em       - advance of the space glyph
bool get_font_layout_metrics(const std::wstring& font_name, double& ascent_em,
                             double& descent_em, double& space_advance_em);

// Horizontal advance of `text` in em units, measured with the GDI font
// engine at a large size to limit rounding.
bool get_text_advance_em(const std::wstring& font_name,
                         const std::wstring& text, double& advance_em);
