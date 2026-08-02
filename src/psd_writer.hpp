#pragma once
// psd_writer.hpp - Dependency-free PSD (Photoshop) file writer.
//
// Generates PSD version 1, RGB 8-bit files with:
//   - pixel (image) layers
//   - text layers (editable, via TySh / EngineData)
//   - groups (layer section dividers)
//   - legacy layer effects lrFX (drop shadow, color overlay, glow, bevel...)
//
// The writer mirrors the byte layout produced/consumed by psd-tools and
// Adobe Photoshop (big-endian PSD structures, descriptor-based TySh data,
// engine-data markup language, PackBits RLE channel compression).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace psdw {

// ---------------------------------------------------------------------------
// Big-endian byte buffer
// ---------------------------------------------------------------------------
class Buffer {
public:
    void u8(uint8_t v);
    void u16(uint16_t v);
    void u32(uint32_t v);
    void i16(int16_t v);
    void i32(int32_t v);
    void f64(double v);
    void raw(const void* data, size_t n);
    void raw(const std::vector<uint8_t>& v);
    void raw(const std::string& s);
    void pad(size_t align);          // zero-pad to a multiple of `align`
    size_t size() const;
    const std::vector<uint8_t>& data() const;
private:
    std::vector<uint8_t> b_;
};

// ---------------------------------------------------------------------------
// Photoshop descriptor (used by TySh, lfx2, ...)
// ---------------------------------------------------------------------------
struct Descriptor;

struct DValue {
    enum class T { Obj, List, Double, Unit, Text, Enum, Long, Bool, Raw };
    T t = T::Long;

    std::shared_ptr<Descriptor> obj;   // T::Obj
    std::vector<DValue> list;          // T::List
    std::string class_id;              // T::Obj
    std::string unit;                  // T::Unit (4 chars, e.g. "#Pxl")
    std::string enum_type;             // T::Enum
    std::string enum_val;
    std::string text;                  // T::Text (UTF-8)
    std::vector<uint8_t> raw;          // T::Raw
    double d = 0.0;
    int64_t l = 0;
    bool b = false;
};

struct Descriptor {
    std::string name;
    std::string class_id = "null";
    std::vector<std::pair<std::string, DValue>> items;

    void add(const std::string& key, DValue v) {
        items.emplace_back(key, std::move(v));
    }
};

DValue dObj(const std::string& class_id);
DValue dList(std::vector<DValue> items);
DValue dDouble(double v);
DValue dUnit(const std::string& unit, double v);
DValue dText(const std::string& utf8);
DValue dEnum(const std::string& type_id, const std::string& enum_id);
DValue dLong(int64_t v);
DValue dBool(bool v);
DValue dRaw(std::vector<uint8_t> bytes);

// ---------------------------------------------------------------------------
// Text-engine data markup (EngineData)
// ---------------------------------------------------------------------------
struct EVal;
using EDict = std::vector<std::pair<std::string, std::shared_ptr<EVal>>>;

struct EVal {
    enum class T { Dict, List, Str, Flt, Int, Bool } t = T::Int;
    EDict dict;                                  // T::Dict
    std::vector<std::shared_ptr<EVal>> list;     // T::List
    std::string str;                             // T::Str (UTF-8)
    double flt = 0.0;                            // T::Flt
    int64_t integer = 0;                         // T::Int
    bool boolean = false;                        // T::Bool
};

std::shared_ptr<EVal> eDict();
std::shared_ptr<EVal> eList(std::vector<std::shared_ptr<EVal>> items);
std::shared_ptr<EVal> eStr(const std::string& utf8);
std::shared_ptr<EVal> eFlt(double v);
std::shared_ptr<EVal> eInt(int64_t v);
std::shared_ptr<EVal> eBool(bool v);

void edict_set(EDict& d, const std::string& key, std::shared_ptr<EVal> v);

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
};

// Build the raw tagged-block payloads (used internally and for testing).
std::vector<uint8_t> build_tysh(const TextLayerData& text);
std::vector<uint8_t> build_lrfx(const Effects& effects);  // empty if disabled

// ---------------------------------------------------------------------------
// Document model
// ---------------------------------------------------------------------------
struct LayerBase {
    virtual ~LayerBase() = default;
    std::string name;
    int top = 0, left = 0, bottom = 0, right = 0;
    bool visible = true;
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
