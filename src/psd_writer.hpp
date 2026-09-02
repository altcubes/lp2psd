#pragma once
// psd_writer.hpp - Dependency-free PSD (Photoshop) file writer.
//
// Generates PSD version 1, RGB 8-bit files with:
//   - pixel (image) layers
//   - text layers (editable, via TySh / EngineData)
//   - groups (layer section dividers)
//   - legacy layer effects lrFX (drop shadow, color overlay, glow, bevel...)
//
// This header is the public document model only. The byte-level machinery
// (Buffer, descriptor / EngineData serialization, internal tagged-block
// builders) lives in psd_writer_internal.hpp + psd_writer.cpp and should not
// be included by callers.
//
// The writer mirrors the byte layout produced/consumed by psd-tools and
// Adobe Photoshop (big-endian PSD structures, descriptor-based TySh data,
// engine-data markup language, PackBits RLE channel compression).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace psdw {

// ---------------------------------------------------------------------------
// Layer effects (legacy lrFX format, understood by Photoshop)
// ---------------------------------------------------------------------------
enum class BlendMode : uint32_t {
    Normal = 0, Multiply, Screen, Overlay, SoftLight, HardLight,
    Darken, Lighten, ColorDodge, ColorBurn, LinearBurn, LinearDodge,
    Difference, Exclusion, Hue, Saturation, Color, Luminosity,
};

struct DropShadow {
    bool enabled = true;
    uint8_t color[3] = {0, 0, 0};
    int opacity = 75;        // 0-100
    int angle = 120;         // degrees (global angle when use_global_angle)
    int distance = 3;        // px
    int size = 4;            // px (blur)
    bool use_global_angle = true;
    BlendMode blend = BlendMode::Normal;
};

struct InnerShadow {
    bool enabled = false;
    uint8_t color[3] = {0, 0, 0};
    int opacity = 75;
    int angle = 120;
    int distance = 3;
    int size = 4;
    bool use_global_angle = true;
    BlendMode blend = BlendMode::Multiply;
};

struct OuterGlow {
    bool enabled = false;
    uint8_t color[3] = {255, 255, 255};
    int opacity = 75;
    int size = 4;
    BlendMode blend = BlendMode::Screen;
};

struct InnerGlow {
    bool enabled = false;
    uint8_t color[3] = {255, 255, 255};
    int opacity = 50;
    int size = 4;
    bool invert = true;
    BlendMode blend = BlendMode::Screen;
};

struct Bevel {
    bool enabled = false;
    int depth = 100;
    int size = 4;
    int angle = 120;
    bool use_global_angle = true;
    // 0 = outer bevel, 1 = inner bevel, 2 = emboss, 3 = pillow emboss
    int style = 0;
    uint8_t highlight_color[3] = {255, 255, 255};
    int highlight_opacity = 75;
    uint8_t shadow_color[3] = {0, 0, 0};
    int shadow_opacity = 75;
};

struct ColorOverlay {
    bool enabled = false;
    uint8_t color[3] = {255, 255, 255};
    int opacity = 100;
    BlendMode blend = BlendMode::Normal;
};

struct Effects {
    std::optional<DropShadow> drop_shadow;
    std::optional<InnerShadow> inner_shadow;
    std::optional<OuterGlow> outer_glow;
    std::optional<InnerGlow> inner_glow;
    std::optional<Bevel> bevel;
    std::optional<ColorOverlay> color_overlay;
};

// ---------------------------------------------------------------------------
// Text layer content
// ---------------------------------------------------------------------------
struct TextLayerData {
    std::string text;          // UTF-8; '\n' is converted to Photoshop '\r'
    std::string font;          // PostScript font name, e.g. "MicrosoftYaHei"
    double font_size = 24.0;
    uint8_t color[3] = {255, 255, 255};
    double box_x = 0, box_y = 0, box_w = 100, box_h = 30; // document px
    std::vector<uint8_t> preview;  // optional RGBA8 preview w*h
    // Document resolution in pixels/inch. Type sizes (font_size, leading,
    // TySh bounds) stay in the 72-dpi point space; the layer rectangle,
    // preview raster and ink box are document pixels scaled by dpi/72.
    double dpi = 96.0;
    // Ink bounding box of the rendered preview, in px relative to the box
    // top-left corner. Zero width/height means "unknown" (no preview).
    double ink_l = 0.0, ink_t = 0.0, ink_r = 0.0, ink_b = 0.0;

    // ---- Text style (TySh descriptor + EngineData) ----
    // EngineData /AntiAlias (TySh AntA enumerated value derived from this):
    //   0 = None, 1 = Crisp, 2 = Strong, 3 = Smooth, 4 = Sharp, 6 = LCD
    int anti_alias = 2;
    // 0 = horizontal, 1 = vertical (TySh Ornt=Vrtc, WritingDirection=2)
    int orientation = 0;
    // Paragraph justification: 0=Left 1=Right 2=Center 3=JustifyLastLeft
    // 4=JustifyLastRight 5=JustifyLastCenter 6=JustifyAll
    int justification = 0;
    bool auto_leading = true;      // StyleSheetData /AutoLeading
    double auto_leading_size = 1.2;  // paragraph /AutoLeading multiplier
    double leading = 0.0;          // explicit leading (px); 0 => font_size * auto_leading_size
    bool discretionary_ligatures = false;  // StyleSheetData /DLigatures
    bool standard_vertical_roman = true;   // StyleSheetData /BaselineDirection: true=2, false=0
    // EngineData FontSet Script: -1 = auto-detect from font name,
    // 0 = Roman, 1 = Japanese, 2 = Traditional Chinese,
    // 3 = Simplified Chinese, 4 = Korean.
    int script = -1;
};

// ---------------------------------------------------------------------------
// Document model
// ---------------------------------------------------------------------------
struct LayerBase {
    virtual ~LayerBase() = default;
    std::string name;
    int top = 0, left = 0, bottom = 0, right = 0;
    bool visible = true;
    // Layer opacity, 0-255 (255 = fully opaque, 0 = fully transparent).
    uint8_t opacity = 255;
    // Lock state. transparency_locked also sets the legacy layer-flags bit;
    // all three map to the lspf protected-settings bitmask
    // (1 = transparency, 2 = composite/edit, 4 = position).
    bool transparency_locked = false;
    bool composite_locked = false;
    bool position_locked = false;
    Effects effects;
};

struct PixelLayer : LayerBase {
    int x = 0, y = 0, w = 0, h = 0;
    std::vector<uint8_t> rgba;   // RGBA8, w*h*4
};

struct TextLayer : LayerBase {
    TextLayerData text;
};

struct Group : LayerBase {
    bool open = true;
    std::vector<std::shared_ptr<LayerBase>> children;  // bottom-to-top
};

class Document {
public:
    int width = 0;
    int height = 0;
    // Document resolution in pixels/inch (PSD image resource 0x1000).
    // Defaults to 96; set from the source image DPI when known.
    double res_h = 96.0;
    double res_v = 96.0;
    std::vector<std::shared_ptr<LayerBase>> layers;   // bottom-to-top

    bool write(const std::string& path, std::string* error = nullptr) const;
#ifdef _WIN32
    bool write_wide(const std::wstring& path, std::string* error = nullptr) const;
#endif
};

}  // namespace psdw
