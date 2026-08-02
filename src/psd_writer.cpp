#include "psd_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace psdw {

// ===========================================================================
// Buffer
// ===========================================================================
void Buffer::u8(uint8_t v) { b_.push_back(v); }
void Buffer::u16(uint16_t v) {
    b_.push_back((uint8_t)(v >> 8));
    b_.push_back((uint8_t)(v & 0xFF));
}
void Buffer::u32(uint32_t v) {
    b_.push_back((uint8_t)(v >> 24));
    b_.push_back((uint8_t)((v >> 16) & 0xFF));
    b_.push_back((uint8_t)((v >> 8) & 0xFF));
    b_.push_back((uint8_t)(v & 0xFF));
}
void Buffer::i16(int16_t v) { u16((uint16_t)v); }
void Buffer::i32(int32_t v) { u32((uint32_t)v); }
void Buffer::f64(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    for (int shift = 56; shift >= 0; shift -= 8)
        b_.push_back((uint8_t)(bits >> shift));
}
void Buffer::raw(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    b_.insert(b_.end(), p, p + n);
}
void Buffer::raw(const std::vector<uint8_t>& v) {
    b_.insert(b_.end(), v.begin(), v.end());
}
void Buffer::raw(const std::string& s) {
    b_.insert(b_.end(), s.begin(), s.end());
}
void Buffer::pad(size_t align) {
    while (align > 1 && (b_.size() % align) != 0) b_.push_back(0);
}
size_t Buffer::size() const { return b_.size(); }
const std::vector<uint8_t>& Buffer::data() const { return b_; }

// ===========================================================================
// UTF-8 helpers
// ===========================================================================
namespace {

void append_utf16be(std::vector<uint8_t>& out, uint32_t cp) {
    if (cp < 0x10000) {
        out.push_back((uint8_t)(cp >> 8));
        out.push_back((uint8_t)(cp & 0xFF));
    } else {
        cp -= 0x10000;
        uint32_t hi = 0xD800 + (cp >> 10);
        uint32_t lo = 0xDC00 + (cp & 0x3FF);
        out.push_back((uint8_t)(hi >> 8));
        out.push_back((uint8_t)(hi & 0xFF));
        out.push_back((uint8_t)(lo >> 8));
        out.push_back((uint8_t)(lo & 0xFF));
    }
}

std::vector<uint8_t> utf8_to_utf16be(const std::string& s) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp = 0;
        size_t extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F; extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F; extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07; extra = 3;
        } else {
            append_utf16be(out, 0xFFFD);
            i++;
            continue;
        }
        if (i + extra >= s.size()) {
            append_utf16be(out, 0xFFFD);
            break;
        }
        bool bad = false;
        for (size_t k = 1; k <= extra; k++) {
            uint8_t cc = (uint8_t)s[i + k];
            if ((cc & 0xC0) != 0x80) { bad = true; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (bad) {
            append_utf16be(out, 0xFFFD);
            i++;
            continue;
        }
        append_utf16be(out, cp);
        i += extra + 1;
    }
    return out;
}

int utf16_length(const std::string& s) {
    int n = 0;
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) { i += 1; n += 1; }
        else if ((c & 0xE0) == 0xC0) { i += 2; n += 1; }
        else if ((c & 0xF0) == 0xE0) { i += 3; n += 1; }
        else if ((c & 0xF8) == 0xF0) { i += 4; n += 2; }
        else { i += 1; n += 1; }
    }
    return n;
}

}  // namespace (UTF-8 helpers)

// ===========================================================================
// Descriptor value factories
// ===========================================================================
DValue dObj(const std::string& class_id) {
    DValue v;
    v.t = DValue::T::Obj;
    v.obj = std::make_shared<Descriptor>();
    v.obj->class_id = class_id;
    return v;
}
DValue dList(std::vector<DValue> items) {
    DValue v;
    v.t = DValue::T::List;
    v.list = std::move(items);
    return v;
}
DValue dDouble(double x) { DValue v; v.t = DValue::T::Double; v.d = x; return v; }
DValue dUnit(const std::string& unit, double x) {
    DValue v;
    v.t = DValue::T::Unit;
    v.unit = unit;
    v.d = x;
    return v;
}
DValue dText(const std::string& t) { DValue v; v.t = DValue::T::Text; v.text = t; return v; }
DValue dEnum(const std::string& type_id, const std::string& enum_id) {
    DValue v;
    v.t = DValue::T::Enum;
    v.enum_type = type_id;
    v.enum_val = enum_id;
    return v;
}
DValue dLong(int64_t x) { DValue v; v.t = DValue::T::Long; v.l = x; return v; }
DValue dBool(bool x) { DValue v; v.t = DValue::T::Bool; v.b = x; return v; }
DValue dRaw(std::vector<uint8_t> bytes) {
    DValue v;
    v.t = DValue::T::Raw;
    v.raw = std::move(bytes);
    return v;
}

// ===========================================================================
// Descriptor serialization
// ===========================================================================
namespace {

const char* dtype4(const DValue& v) {
    switch (v.t) {
        case DValue::T::Obj:    return "Objc";
        case DValue::T::List:   return "VlLs";
        case DValue::T::Double: return "doub";
        case DValue::T::Unit:   return "UntF";
        case DValue::T::Text:   return "TEXT";
        case DValue::T::Enum:   return "enum";
        case DValue::T::Long:   return "long";
        case DValue::T::Bool:   return "bool";
        case DValue::T::Raw:    return "tdta";
    }
    return "long";
}

void write_unicode(Buffer& b, const std::string& utf8) {
    std::vector<uint8_t> u16 = utf8_to_utf16be(utf8);
    b.u32((uint32_t)(u16.size() / 2));
    b.raw(u16);
}

void write_length_key(Buffer& b, const std::string& s) {
    b.u32((uint32_t)s.size());
    b.raw(s);
}

void write_dvalue(Buffer& b, const DValue& v);

void write_descriptor(Buffer& b, const Descriptor& d) {
    write_unicode(b, d.name);
    write_length_key(b, d.class_id);
    b.u32((uint32_t)d.items.size());
    for (const auto& kv : d.items) {
        write_length_key(b, kv.first);
        b.raw(dtype4(kv.second));
        write_dvalue(b, kv.second);
    }
}

void write_dvalue(Buffer& b, const DValue& v) {
    switch (v.t) {
        case DValue::T::Obj:
            write_descriptor(b, *v.obj);
            break;
        case DValue::T::List:
            b.u32((uint32_t)v.list.size());
            for (const auto& item : v.list) {
                b.raw(dtype4(item));
                write_dvalue(b, item);
            }
            break;
        case DValue::T::Double:
            b.f64(v.d);
            break;
        case DValue::T::Unit:
            b.raw(v.unit);
            b.f64(v.d);
            break;
        case DValue::T::Text:
            write_unicode(b, v.text);
            break;
        case DValue::T::Enum:
            write_length_key(b, v.enum_type);
            write_length_key(b, v.enum_val);
            break;
        case DValue::T::Long:
            b.i32((int32_t)v.l);
            break;
        case DValue::T::Bool:
            b.u8(v.b ? 1 : 0);
            break;
        case DValue::T::Raw:
            b.u32((uint32_t)v.raw.size());
            b.raw(v.raw);
            break;
    }
}

// ===========================================================================
// Engine-data markup writer (mirrors psd-tools' EngineData serialization)
// ===========================================================================
std::string fmt_float(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.8f", v);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s += '0';
    if (std::fabs(v) > 0.0 && std::fabs(v) < 1.0) {
        size_t p = s.find("0.");
        while (p != std::string::npos) {
            s.replace(p, 2, ".");
            p = s.find("0.", p + 1);
        }
    }
    return s;
}

void write_estring(Buffer& b, const std::string& utf8) {
    std::vector<uint8_t> u16 = utf8_to_utf16be(utf8);
    // Byte-level escaping of '\\', '(' and ')' exactly like psd-tools.
    for (uint8_t c : {(uint8_t)0x5C, (uint8_t)0x28, (uint8_t)0x29}) {
        std::vector<uint8_t> esc;
        for (size_t i = 0; i < u16.size(); i++) {
            if (u16[i] == c) esc.push_back(0x5C);
            esc.push_back(u16[i]);
        }
        u16.swap(esc);
    }
    b.u8('(');
    b.u8(0xFE);
    b.u8(0xFF);
    b.raw(u16);
    b.u8(')');
}

void write_edict(Buffer& b, const EDict& d, int indent, bool container);

void write_escalar(Buffer& b, const EVal& v) {
    switch (v.t) {
        case EVal::T::Str:  write_estring(b, v.str); break;
        case EVal::T::Flt:  b.raw(fmt_float(v.flt)); break;
        case EVal::T::Int:  b.raw(std::to_string(v.integer)); break;
        case EVal::T::Bool: b.raw(v.boolean ? "true" : "false"); break;
        default: break;
    }
}

void write_elist(Buffer& b, const std::vector<std::shared_ptr<EVal>>& l, int indent) {
    b.u8('[');
    if (indent < 0) {
        for (const auto& item : l) {
            if (item->t == EVal::T::Dict) {
                write_edict(b, item->dict, -1, true);
            } else {
                b.u8(' ');
                write_escalar(b, *item);
            }
        }
        b.u8(' ');
    } else {
        for (const auto& item : l) write_edict(b, item->dict, indent, true);
        b.u8('\n');
        for (int i = 0; i < indent; i++) b.u8('\t');
    }
    b.u8(']');
}

void write_edict(Buffer& b, const EDict& d, int indent, bool container) {
    int inner = indent + 1;
    if (container) {
        if (indent == 0) b.u8('\n');
        if (indent >= 0) {
            b.u8('\n');
            for (int i = 0; i < indent; i++) b.u8('\t');
            b.raw("<<");
            b.u8('\n');
        } else {
            b.u8(' ');
            b.raw("<<");
        }
    }
    for (const auto& kv : d) {
        for (int i = 0; i < inner; i++) b.u8('\t');
        b.u8('/');
        b.raw(kv.first);
        const EVal* v = kv.second.get();
        if (v->t == EVal::T::Dict) {
            write_edict(b, v->dict, inner, true);
        } else {
            b.u8(' ');
            if (v->t == EVal::T::List) {
                if (!v->list.empty() && v->list[0]->t == EVal::T::Dict)
                    write_elist(b, v->list, inner);
                else
                    write_elist(b, v->list, -1);
            } else {
                write_escalar(b, *v);
            }
        }
        if (indent >= 0) b.u8('\n');
    }
    if (container) {
        if (indent >= 0) {
            for (int i = 0; i < indent; i++) b.u8('\t');
        } else {
            b.u8(' ');
        }
        b.raw(">>");
    }
}

std::vector<uint8_t> engine_bytes(const EDict& root) {
    Buffer b;
    write_edict(b, root, 0, true);
    return b.data();
}

// ===========================================================================
// Text-engine template (structure of a real Photoshop-generated TySh)
// ===========================================================================
std::shared_ptr<EVal> eDictFrom(EDict d) {
    auto v = eDict();
    v->dict = std::move(d);
    return v;
}

EDict make_fill_color(double r, double g, double bl) {
    EDict d;
    edict_set(d, "Type", eInt(1));
    edict_set(d, "Values", eList({eFlt(1.0), eFlt(r), eFlt(g), eFlt(bl)}));
    return d;
}

EDict make_stroke_color() {
    EDict d;
    edict_set(d, "Type", eInt(1));
    edict_set(d, "Values", eList({eFlt(1.0), eFlt(0.0), eFlt(0.0), eFlt(0.0)}));
    return d;
}

EDict make_grid_color() {
    EDict d;
    edict_set(d, "Type", eInt(1));
    edict_set(d, "Values", eList({eFlt(0.0), eFlt(0.0), eFlt(0.0), eFlt(1.0)}));
    return d;
}

// EngineData /AntiAlias int -> TySh AntA enumerated id
static const char* anti_alias_enum(int v) {
    switch (v) {
        case 0: return "AnNo";                    // None
        case 1: return "AnCr";                    // Crisp
        case 3: return "AnSm";                    // Smooth
        case 4: return "antiAliasSharp";
        case 6: return "antiAliasPlatformLCD";
        case 2:
        default: return "AnSt";                   // Strong
    }
}

// Leading actually written when auto leading is on: font_size * auto_leading_size.
static double effective_leading(const TextLayerData& t) {
    return t.auto_leading ? t.font_size * t.auto_leading_size
                          : (t.leading > 0.0 ? t.leading : t.font_size * t.auto_leading_size);
}

// EngineData FontSet/Script: 0=Roman, 1=Japanese, 2=Chinese, 3=Korean.
// Real Photoshop files mark CJK fonts with their script; Latin script 0 on a
// Chinese font makes Photoshop treat the text as non-CJK and is one source of
// the "update text layer" / "font needs re-layout" prompt.
static int font_script_of(const std::string& font) {
    std::string f;
    f.reserve(font.size());
    for (char c : font) {
        if (c >= 'A' && c <= 'Z') f.push_back((char)(c - 'A' + 'a'));
        else if (c >= 'a' && c <= 'z') f.push_back(c);
    }
    static const char* cjk[] = {
        "hei", "song", "ming", "kai", "fang", "yahei", "simsun", "simhei",
        "msyh", "simkai", "simfang", "dengxian", "pingfang", "sourcehansans",
        "sourcehanserif", "notosanscjk", "notoserifcjk", "notosanssc",
        "notoserifsc", "han", "cjk", "sc", "cn", "wqy", "uming", "ukai",
        "malgun", "gulim", "batang", "dotum", "applegothic", "hirakaku",
        "hiragino", "meiryo", "msmincho", "msgothic", "yumin", "ipaex",
    };
    for (const char* k : cjk) {
        if (f.find(k) != std::string::npos) {
            // Japanese-specific names get 1, Korean-specific get 3, the rest
            // (Chinese fonts like Microsoft YaHei) get 2.
            if (std::string("hirakakuhiraginomeiryomsminchomsgothicyuminipaex").find(k) != std::string::npos)
                return 1;
            if (std::string("malgungulimbatangdotumapplegothic").find(k) != std::string::npos)
                return 3;
            return 2;
        }
    }
    return 0;
}

EDict make_run_style(const TextLayerData& t, double r, double g, double bl) {
    EDict d;
    // FontSet[0] is the AdobeInvisFont placeholder, the real font is index 1.
    edict_set(d, "Font", eInt(1));
    edict_set(d, "FontSize", eFlt(t.font_size));
    edict_set(d, "AutoLeading", eBool(t.auto_leading));
    edict_set(d, "Leading", eFlt(effective_leading(t)));
    edict_set(d, "Tracking", eInt(0));
    edict_set(d, "AutoKerning", eBool(true));
    edict_set(d, "Kerning", eInt(0));
    edict_set(d, "BaselineShift", eFlt(0.0));
    edict_set(d, "DLigatures", eBool(t.discretionary_ligatures));
    // Standard vertical Roman alignment (Applies only to vertical text):
    //   enabled  -> StyleRun BaselineDirection = 1
    //   disabled -> field omitted entirely (real Photoshop CLOSE state)
    // StyleSheetSet keeps the constant default 2 (written in make_resource_dict).
    if (t.standard_vertical_roman) edict_set(d, "BaselineDirection", eInt(1));
    edict_set(d, "FillColor", eDictFrom(make_fill_color(r, g, bl)));
    edict_set(d, "StrokeColor", eDictFrom(make_stroke_color()));
    edict_set(d, "HindiNumbers", eBool(false));
    return d;
}

EDict make_full_style_data(const TextLayerData& t, double r, double g, double bl) {
    EDict d;
    // FontSet[0] is the AdobeInvisFont placeholder, the real font is index 1.
    edict_set(d, "Font", eInt(1));
    edict_set(d, "FontSize", eFlt(t.font_size));
    edict_set(d, "FauxBold", eBool(false));
    edict_set(d, "FauxItalic", eBool(false));
    edict_set(d, "AutoLeading", eBool(t.auto_leading));
    edict_set(d, "Leading", eFlt(effective_leading(t)));
    edict_set(d, "HorizontalScale", eFlt(1.0));
    edict_set(d, "VerticalScale", eFlt(1.0));
    edict_set(d, "Tracking", eInt(0));
    edict_set(d, "AutoKerning", eBool(true));
    edict_set(d, "Kerning", eInt(0));
    edict_set(d, "BaselineShift", eFlt(0.0));
    edict_set(d, "FontCaps", eInt(0));
    edict_set(d, "FontBaseline", eInt(0));
    edict_set(d, "Underline", eBool(false));
    edict_set(d, "Strikethrough", eBool(false));
    edict_set(d, "Ligatures", eBool(true));
    edict_set(d, "DLigatures", eBool(t.discretionary_ligatures));
    edict_set(d, "BaselineDirection", eInt(2));  // StyleSheetSet default, always 2
    edict_set(d, "Tsume", eFlt(0.0));
    edict_set(d, "StyleRunAlignment", eInt(2));
    edict_set(d, "Language", eInt(0));
    edict_set(d, "NoBreak", eBool(false));
    edict_set(d, "FillColor", eDictFrom(make_fill_color(r, g, bl)));
    edict_set(d, "StrokeColor", eDictFrom(make_stroke_color()));
    edict_set(d, "FillFlag", eBool(true));
    edict_set(d, "StrokeFlag", eBool(false));
    edict_set(d, "FillFirst", eBool(true));
    edict_set(d, "YUnderline", eInt(1));
    edict_set(d, "OutlineWidth", eFlt(1.0));
    edict_set(d, "CharacterDirection", eInt(0));
    edict_set(d, "HindiNumbers", eBool(false));
    edict_set(d, "Kashida", eInt(1));
    edict_set(d, "DiacriticPos", eInt(2));
    return d;
}

EDict make_para_props(bool sheet_style, int justification, double auto_leading_size) {
    EDict d;
    edict_set(d, "Justification", eInt(justification));
    edict_set(d, "FirstLineIndent", eFlt(0.0));
    edict_set(d, "StartIndent", eFlt(0.0));
    edict_set(d, "EndIndent", eFlt(0.0));
    edict_set(d, "SpaceBefore", eFlt(0.0));
    edict_set(d, "SpaceAfter", eFlt(0.0));
    edict_set(d, "AutoHyphenate", eBool(sheet_style));
    edict_set(d, "HyphenatedWordSize", eInt(6));
    edict_set(d, "PreHyphen", eInt(2));
    edict_set(d, "PostHyphen", eInt(2));
    edict_set(d, "ConsecutiveHyphens", eInt(8));
    edict_set(d, "Zone", eFlt(36.0));
    edict_set(d, "WordSpacing", eList({eFlt(0.8), eFlt(1.0), eFlt(1.33)}));
    edict_set(d, "LetterSpacing", eList({eFlt(0.0), eFlt(0.0), eFlt(0.0)}));
    edict_set(d, "GlyphSpacing", eList({eFlt(1.0), eFlt(1.0), eFlt(1.0)}));
    edict_set(d, "AutoLeading", eFlt(auto_leading_size));
    edict_set(d, "LeadingType", eInt(sheet_style ? 0 : 1));
    edict_set(d, "Hanging", eBool(false));
    edict_set(d, "Burasagari", eBool(false));
    edict_set(d, "KinsokuOrder", eInt(0));
    edict_set(d, "EveryLineComposer", eBool(false));
    return d;
}

EDict make_adjustments() {
    EDict d;
    edict_set(d, "Axis", eList({eFlt(1.0), eFlt(0.0), eFlt(1.0)}));
    edict_set(d, "XY", eList({eFlt(0.0), eFlt(0.0)}));
    return d;
}

EDict make_paragraph_sheet(const EDict& props, int default_sheet) {
    EDict d;
    edict_set(d, "DefaultStyleSheet", eInt(default_sheet));
    edict_set(d, "Properties", eDictFrom(props));
    return d;
}

EDict make_engine_dict(const TextLayerData& t) {
    std::string engine_text = t.text;
    std::replace(engine_text.begin(), engine_text.end(), '\n', '\r');
    engine_text += '\r';
    // One run per line, exactly like Photoshop's own EngineData files.
    std::vector<int> run_lengths;
    size_t seg_start = 0;
    for (size_t i = 0; i < engine_text.size(); i++) {
        if (engine_text[i] == '\r') {
            run_lengths.push_back(
                utf16_length(engine_text.substr(seg_start, i - seg_start + 1)));
            seg_start = i + 1;
        }
    }
    if (run_lengths.empty()) run_lengths.push_back(utf16_length(engine_text));
    double r = t.color[0] / 255.0, g = t.color[1] / 255.0, bl = t.color[2] / 255.0;

    EDict dict;

    EDict editor;
    edict_set(editor, "Text", eStr(engine_text));
    edict_set(dict, "Editor", eDictFrom(editor));

    EDict para_default;
    edict_set(para_default, "ParagraphSheet", eDictFrom(make_paragraph_sheet(EDict(), 0)));
    edict_set(para_default, "Adjustments", eDictFrom(make_adjustments()));
    EDict para_run_item;
    edict_set(para_run_item, "ParagraphSheet",
              eDictFrom(make_paragraph_sheet(make_para_props(false, t.justification,
                                                             t.auto_leading_size), 0)));
    edict_set(para_run_item, "Adjustments", eDictFrom(make_adjustments()));
    EDict para_run;
    edict_set(para_run, "DefaultRunData", eDictFrom(para_default));
    std::vector<std::shared_ptr<EVal>> para_items, para_lens;
    for (int len : run_lengths) {
        para_items.push_back(eDictFrom(para_run_item));
        para_lens.push_back(eInt(len));
    }
    edict_set(para_run, "RunArray", eList(para_items));
    edict_set(para_run, "RunLengthArray", eList(para_lens));
    edict_set(para_run, "IsJoinable", eInt(1));
    edict_set(dict, "ParagraphRun", eDictFrom(para_run));

    EDict sr_default_sheet;
    edict_set(sr_default_sheet, "StyleSheetData", eDictFrom(EDict()));
    EDict sr_default;
    edict_set(sr_default, "StyleSheet", eDictFrom(sr_default_sheet));
    EDict sr_item_sheet;
    edict_set(sr_item_sheet, "StyleSheetData", eDictFrom(make_run_style(t, r, g, bl)));
    EDict sr_item;
    edict_set(sr_item, "StyleSheet", eDictFrom(sr_item_sheet));
    EDict style_run;
    edict_set(style_run, "DefaultRunData", eDictFrom(sr_default));
    std::vector<std::shared_ptr<EVal>> style_items, style_lens;
    for (int len : run_lengths) {
        style_items.push_back(eDictFrom(sr_item));
        style_lens.push_back(eInt(len));
    }
    edict_set(style_run, "RunArray", eList(style_items));
    edict_set(style_run, "RunLengthArray", eList(style_lens));
    edict_set(style_run, "IsJoinable", eInt(2));
    edict_set(dict, "StyleRun", eDictFrom(style_run));

    EDict grid;
    edict_set(grid, "GridIsOn", eBool(false));
    edict_set(grid, "ShowGrid", eBool(false));
    edict_set(grid, "GridSize", eFlt(18.0));
    edict_set(grid, "GridLeading", eFlt(22.0));
    edict_set(grid, "GridColor", eDictFrom(make_grid_color()));
    edict_set(grid, "GridLeadingFillColor", eDictFrom(make_grid_color()));
    edict_set(grid, "AlignLineHeightToGridFlags", eBool(false));
    edict_set(dict, "GridInfo", eDictFrom(grid));

    edict_set(dict, "AntiAlias", eInt(t.anti_alias));
    edict_set(dict, "UseFractionalGlyphWidths", eBool(true));

    EDict base;
    edict_set(base, "ShapeType", eInt(0));
    edict_set(base, "TransformPoint0", eList({eFlt(1.0), eFlt(0.0)}));
    edict_set(base, "TransformPoint1", eList({eFlt(0.0), eFlt(1.0)}));
    edict_set(base, "TransformPoint2", eList({eFlt(0.0), eFlt(0.0)}));
    EDict ph;
    // Point text: ShapeType 0 + PointBase (no fixed box). Paragraph text would
    // use ShapeType 1 + BoxBounds instead. Photoshop decides point vs paragraph
    // text from this Cookie->Photoshop->ShapeType value.
    edict_set(ph, "ShapeType", eInt(0));
    edict_set(ph, "PointBase", eList({eFlt(0.0), eFlt(0.0)}));
    edict_set(ph, "Base", eDictFrom(base));
    EDict cookie;
    edict_set(cookie, "Photoshop", eDictFrom(ph));
    EDict lines;
    edict_set(lines, "WritingDirection", eInt(t.orientation ? 2 : 0));
    edict_set(lines, "Children", eList({}));
    EDict child;
    edict_set(child, "ShapeType", eInt(0));
    // Real Photoshop files write Procession=1 for vertical (tate) text and
    // Procession=0 for horizontal. Photoshop consults this when it rebuilds
    // the text layer (e.g. after an "update layer" / font re-layout), so
    // writing 0 for vertical text makes the direction fall back to horizontal.
    edict_set(child, "Procession", eInt(t.orientation ? 1 : 0));
    edict_set(child, "Lines", eDictFrom(lines));
    edict_set(child, "Cookie", eDictFrom(cookie));
    EDict shapes;
    edict_set(shapes, "WritingDirection", eInt(t.orientation ? 2 : 0));
    edict_set(shapes, "Children", eList({eDictFrom(child)}));
    EDict rendered;
    edict_set(rendered, "Version", eInt(1));
    edict_set(rendered, "Shapes", eDictFrom(shapes));
    edict_set(dict, "Rendered", eDictFrom(rendered));

    return dict;
}

EDict make_resource_dict(const TextLayerData& t) {
    double r = t.color[0] / 255.0, g = t.color[1] / 255.0, bl = t.color[2] / 255.0;
    EDict d;

    auto hard = eDict();
    edict_set(hard->dict, "Name", eStr("PhotoshopKinsokuHard"));
    edict_set(hard->dict, "NoStart", eStr("\u3001\u3002\uff0c\uff0e\uff65\uff1a\uff1b\uff1f\uff01\u30fc\u2015\u2019\uff09\u3011\uff3d\u3015\uff09\u3009\u300b\u300d\u3011\u3001\u30fd\u30fe\u3005\u3041\u3043\u3045\u3047\u3049\u3063\u3083\u3085\u3087\u308e\u30a1\u30a3\u30a5\u30a7\u30a9\u30c3\u30e3\u30e5\u30e7\u30f1\u30f2\u30f3\u309b\u309c?!)]},.:;\u2103\u2109\u00a2\uff05\u2030"));
    edict_set(hard->dict, "NoEnd", eStr("\u2018\u201c\uff08\u3014\uff3b\u3016\uff5b\u3008\u300a\u300c\u300e\u3010([{\uffe5\uff04\u00a3\u20ac\u00a7\u3012\uff03"));
    edict_set(hard->dict, "Keep", eStr("\u2015\u2025"));
    edict_set(hard->dict, "Hanging", eStr("\u3001\u3002.,"));
    auto soft = eDict();
    edict_set(soft->dict, "Name", eStr("PhotoshopKinsokuSoft"));
    edict_set(soft->dict, "NoStart", eStr("\u3001\u3002\uff0c\uff0e\uff65\uff1a\uff1b\uff1f\uff01\u2019\uff09\u3011\uff3d\u3015\uff09\u3009\u300b\u300d\u3011\u30fd\u30fe\u3005"));
    edict_set(soft->dict, "NoEnd", eStr("\u2018\u201c\uff08\u3014\uff3b\u3016\uff5b\u3008\u300a\u300c\u300e\u3010"));
    edict_set(soft->dict, "Keep", eStr("\u2015\u2025"));
    edict_set(soft->dict, "Hanging", eStr("\u3001\u3002.,"));
    edict_set(d, "KinsokuSet", eList({hard, soft}));

    auto mk = [](const char* name) {
        auto v = eDict();
        edict_set(v->dict, "InternalName", eStr(name));
        return v;
    };
    edict_set(d, "MojiKumiSet", eList({mk("Photoshop6MojiKumiSet1"),
                                       mk("Photoshop6MojiKumiSet2"),
                                       mk("Photoshop6MojiKumiSet3"),
                                       mk("Photoshop6MojiKumiSet4")}));

    edict_set(d, "TheNormalStyleSheet", eInt(0));
    edict_set(d, "TheNormalParagraphSheet", eInt(0));

    EDict pss_item;
    edict_set(pss_item, "Name", eStr("Normal RGB"));
    edict_set(pss_item, "DefaultStyleSheet", eInt(0));
    edict_set(pss_item, "Properties",
              eDictFrom(make_para_props(true, t.justification, t.auto_leading_size)));
    edict_set(d, "ParagraphSheetSet", eList({eDictFrom(pss_item)}));

    EDict sss_item;
    edict_set(sss_item, "Name", eStr("Normal RGB"));
    edict_set(sss_item, "StyleSheetData", eDictFrom(make_full_style_data(t, r, g, bl)));
    edict_set(d, "StyleSheetSet", eList({eDictFrom(sss_item)}));

    // Real Photoshop files always keep AdobeInvisFont as FontSet[0]; the
    // actual font follows. FontType 0 = invis, 1 = TrueType, 2 = CFF/OTF.
    EDict invis;
    edict_set(invis, "Name", eStr("AdobeInvisFont"));
    edict_set(invis, "Script", eInt(0));
    edict_set(invis, "FontType", eInt(0));
    edict_set(invis, "Synthetic", eInt(0));
    EDict font_item;
    edict_set(font_item, "Name", eStr(t.font.empty() ? "ArialMT" : t.font));
    edict_set(font_item, "Script", eInt(font_script_of(t.font)));
    edict_set(font_item, "FontType", eInt(1));
    edict_set(font_item, "Synthetic", eInt(0));
    edict_set(d, "FontSet", eList({eDictFrom(invis), eDictFrom(font_item)}));

    edict_set(d, "SuperscriptSize", eFlt(0.583));
    edict_set(d, "SuperscriptPosition", eFlt(0.333));
    edict_set(d, "SubscriptSize", eFlt(0.583));
    edict_set(d, "SubscriptPosition", eFlt(0.333));
    edict_set(d, "SmallCapSize", eFlt(0.7));
    return d;
}

std::vector<uint8_t> make_engine_bytes(const TextLayerData& t) {
    EDict root;
    edict_set(root, "EngineDict", eDictFrom(make_engine_dict(t)));
    EDict res = make_resource_dict(t);
    edict_set(root, "ResourceDict", eDictFrom(res));
    edict_set(root, "DocumentResources", eDictFrom(res));
    return engine_bytes(root);
}

// ===========================================================================
// PackBits RLE
// ===========================================================================
std::vector<uint8_t> packbits(const uint8_t* p, size_t n) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 128 && p[i + run] == p[i]) run++;
        if (run >= 3) {
            out.push_back((uint8_t)(257 - run));
            out.push_back(p[i]);
            i += run;
        } else {
            size_t start = i;
            size_t j = i;
            while (j < n && (j - start) < 128) {
                size_t r2 = 1;
                while (j + r2 < n && r2 < 128 && p[j + r2] == p[j]) r2++;
                if (r2 >= 3) break;
                j++;
            }
            size_t len = j - start;
            if (len == 0) len = std::min(n - start, (size_t)128);
            out.push_back((uint8_t)(len - 1));
            out.insert(out.end(), p + start, p + start + len);
            i = start + len;
        }
    }
    return out;
}

void write_rle_channel(Buffer& b, const uint8_t* plane, int w, int h) {
    b.u16(1);
    std::vector<std::vector<uint8_t>> rows;
    rows.reserve((size_t)h);
    for (int y = 0; y < h; y++)
        rows.push_back(packbits(plane + (size_t)y * w, (size_t)w));
    for (int y = 0; y < h; y++) b.u16((uint16_t)rows[y].size());
    for (const auto& r : rows) b.raw(r);
}

void write_color(Buffer& b, const uint8_t c[3]) {
    b.u16(0);  // RGB color space
    b.u16((uint16_t)(c[0] * 257));
    b.u16((uint16_t)(c[1] * 257));
    b.u16((uint16_t)(c[2] * 257));
    b.u16(0);
}

const char* blend4(BlendMode m) {
    switch (m) {
        case BlendMode::Normal:      return "norm";
        case BlendMode::Multiply:    return "mul ";
        case BlendMode::Screen:      return "scrn";
        case BlendMode::Overlay:     return "over";
        case BlendMode::SoftLight:   return "sLit";
        case BlendMode::HardLight:   return "hLit";
        case BlendMode::Darken:      return "dark";
        case BlendMode::Lighten:     return "lite";
        case BlendMode::ColorDodge:  return "div ";
        case BlendMode::ColorBurn:   return "idiv";
        case BlendMode::LinearBurn:  return "lbrn";
        case BlendMode::LinearDodge: return "lddg";
        case BlendMode::Difference:  return "diff";
        case BlendMode::Exclusion:   return "smud";
        case BlendMode::Hue:         return "hue ";
        case BlendMode::Saturation:  return "sat ";
        case BlendMode::Color:       return "colr";
        case BlendMode::Luminosity:  return "lum ";
    }
    return "norm";
}

uint8_t pct(int pct255) { return (uint8_t)(pct255 * 255 / 100); }

}  // namespace

// ===========================================================================
// EVal factories
// ===========================================================================
std::shared_ptr<EVal> eDict() {
    auto v = std::make_shared<EVal>();
    v->t = EVal::T::Dict;
    return v;
}
std::shared_ptr<EVal> eList(std::vector<std::shared_ptr<EVal>> items) {
    auto v = std::make_shared<EVal>();
    v->t = EVal::T::List;
    v->list = std::move(items);
    return v;
}
std::shared_ptr<EVal> eStr(const std::string& s) {
    auto v = std::make_shared<EVal>();
    v->t = EVal::T::Str;
    v->str = s;
    return v;
}
std::shared_ptr<EVal> eFlt(double x) {
    auto v = std::make_shared<EVal>();
    v->t = EVal::T::Flt;
    v->flt = x;
    return v;
}
std::shared_ptr<EVal> eInt(int64_t x) {
    auto v = std::make_shared<EVal>();
    v->t = EVal::T::Int;
    v->integer = x;
    return v;
}
std::shared_ptr<EVal> eBool(bool x) {
    auto v = std::make_shared<EVal>();
    v->t = EVal::T::Bool;
    v->boolean = x;
    return v;
}
void edict_set(EDict& d, const std::string& key, std::shared_ptr<EVal> v) {
    for (auto& kv : d) {
        if (kv.first == key) { kv.second = std::move(v); return; }
    }
    d.emplace_back(key, std::move(v));
}

// ===========================================================================
// Public builders
// ===========================================================================
std::vector<uint8_t> build_tysh(const TextLayerData& t) {
    std::vector<uint8_t> engine = make_engine_bytes(t);
    Descriptor text_data;
    text_data.class_id = "null";
    std::string text0 = t.text;
    std::replace(text0.begin(), text0.end(), '\n', '\r');
    text0 += '\0';
    text_data.add("Txt ", dText(text0));
    text_data.add("textGridding", dEnum("textGridding", "None"));
    text_data.add("Ornt", dEnum("Ornt", t.orientation ? "Vrtc" : "Hrzn"));
    text_data.add("AntA", dEnum("Annt", anti_alias_enum(t.anti_alias)));
    DValue bounds = dObj("bounds");
    bounds.obj->add("Left", dDouble(0.0));
    bounds.obj->add("Top ", dDouble(0.0));
    bounds.obj->add("Rght", dDouble(t.box_w));
    bounds.obj->add("Btom", dDouble(t.box_h));
    text_data.add("bounds", std::move(bounds));
    DValue bbox = dObj("boundingBox");
    bbox.obj->add("Left", dDouble(0.0));
    bbox.obj->add("Top ", dDouble(0.0));
    bbox.obj->add("Rght", dDouble(t.box_w));
    bbox.obj->add("Btom", dDouble(t.box_h));
    text_data.add("boundingBox", std::move(bbox));
    text_data.add("TextIndex", dLong(0));
    text_data.add("EngineData", dRaw(engine));

    Descriptor warp;
    warp.class_id = "warp";
    warp.add("warpStyle", dEnum("warpStyle", "warpNone"));
    warp.add("warpValue", dDouble(0.0));
    warp.add("warpPerspective", dDouble(0.0));
    warp.add("warpPerspectiveOther", dDouble(0.0));
    warp.add("warpRotate", dEnum("Ornt", "Hrzn"));

    Buffer b;
    b.u16(1);
    b.f64(1.0); b.f64(0.0); b.f64(0.0); b.f64(1.0);
    b.f64(t.box_x); b.f64(t.box_y);
    b.u16(50);
    b.u32(16);
    write_descriptor(b, text_data);
    b.u16(1);
    b.u32(16);
    write_descriptor(b, warp);
    b.i32(0); b.i32(0); b.i32(0); b.i32(0);
    b.pad(4);
    return b.data();
}

std::vector<uint8_t> build_lrfx(const Effects& fx) {
    auto add_block = [](Buffer& b, const char* key, const std::vector<uint8_t>& data) {
        b.raw("8BIM");
        b.raw(key);
        b.u32((uint32_t)data.size());
        b.raw(data);
    };

    std::vector<std::pair<std::string, std::vector<uint8_t>>> blocks;

    Buffer c;
    c.u32(0); c.u8(1); c.u8(0); c.u8(0);  // cmnS: version 0, visible 1
    blocks.emplace_back("cmnS", c.data());

    if (fx.drop_shadow) {
        const DropShadow& s = *fx.drop_shadow;
        Buffer d;
        d.u32(2);
        d.u32((uint32_t)(s.size * 65536));
        d.u32(0);
        d.i32((int32_t)(s.angle * 65536));
        d.u32((uint32_t)(s.distance * 65536));
        write_color(d, s.color);
        d.raw("8BIM");
        d.raw(blend4(s.blend));
        d.u8(s.enabled ? 1 : 0);
        d.u8(s.use_global_angle ? 1 : 0);
        d.u8(pct(s.opacity));
        write_color(d, s.color);
        blocks.emplace_back("dsdw", d.data());
    }
    if (fx.inner_shadow) {
        const InnerShadow& s = *fx.inner_shadow;
        Buffer d;
        d.u32(2);
        d.u32((uint32_t)(s.size * 65536));
        d.u32(0);
        d.i32((int32_t)(s.angle * 65536));
        d.u32((uint32_t)(s.distance * 65536));
        write_color(d, s.color);
        d.raw("8BIM");
        d.raw(blend4(s.blend));
        d.u8(s.enabled ? 1 : 0);
        d.u8(s.use_global_angle ? 1 : 0);
        d.u8(pct(s.opacity));
        write_color(d, s.color);
        blocks.emplace_back("isdw", d.data());
    }
    if (fx.outer_glow) {
        const OuterGlow& s = *fx.outer_glow;
        Buffer d;
        d.u32(2);
        d.u32((uint32_t)(s.size * 65536));
        d.u32(0);
        write_color(d, s.color);
        d.raw("8BIM");
        d.raw(blend4(s.blend));
        d.u8(s.enabled ? 1 : 0);
        d.u8(pct(s.opacity));
        write_color(d, s.color);
        blocks.emplace_back("oglw", d.data());
    }
    if (fx.inner_glow) {
        const InnerGlow& s = *fx.inner_glow;
        Buffer d;
        d.u32(2);
        d.u32((uint32_t)(s.size * 65536));
        d.u32(0);
        write_color(d, s.color);
        d.raw("8BIM");
        d.raw(blend4(s.blend));
        d.u8(s.enabled ? 1 : 0);
        d.u8(pct(s.opacity));
        d.u8(s.invert ? 1 : 0);
        write_color(d, s.color);
        blocks.emplace_back("iglw", d.data());
    }
    if (fx.bevel) {
        const Bevel& s = *fx.bevel;
        Buffer d;
        d.i32(2);
        d.i32((int32_t)(s.angle * 65536));
        d.u32((uint32_t)(s.depth * 65536));
        d.u32((uint32_t)(s.size * 65536));
        d.raw("8BIM");
        d.raw("scrn");
        d.raw("8BIM");
        d.raw("mul ");
        write_color(d, s.highlight_color);
        write_color(d, s.shadow_color);
        d.u8((uint8_t)s.style);
        d.u8(pct(s.highlight_opacity));
        d.u8(pct(s.shadow_opacity));
        d.u8(s.enabled ? 1 : 0);
        d.u8(s.use_global_angle ? 1 : 0);
        d.u8(0);  // direction
        write_color(d, s.highlight_color);
        write_color(d, s.shadow_color);
        blocks.emplace_back("bevl", d.data());
    }
    if (fx.color_overlay) {
        const ColorOverlay& s = *fx.color_overlay;
        Buffer d;
        d.u32(2);
        d.raw("8BIM");
        d.raw(blend4(s.blend));
        write_color(d, s.color);
        d.u8(pct(s.opacity));
        d.u8(s.enabled ? 1 : 0);
        write_color(d, s.color);
        blocks.emplace_back("sofi", d.data());
    }

    if (blocks.size() <= 1) return {};  // no enabled effect

    Buffer b;
    b.u16(0);
    b.u16((uint16_t)blocks.size());
    for (const auto& blk : blocks) add_block(b, blk.first.c_str(), blk.second);
    b.pad(4);
    return b.data();
}

// ===========================================================================
// Document
// ===========================================================================
namespace {

struct Rec {
    const LayerBase* layer = nullptr;
    int lsct = 0;  // 0 = normal, 1 = open folder, 2 = closed folder, 3 = divider
};

void collect_records(const std::vector<std::shared_ptr<LayerBase>>& items,
                     std::vector<Rec>& out) {
    for (const auto& l : items) {
        if (auto g = std::dynamic_pointer_cast<Group>(l)) {
            out.push_back({l.get(), 3});
            collect_records(g->children, out);
            out.push_back({l.get(), g->open ? 1 : 2});
        } else {
            out.push_back({l.get(), 0});
        }
    }
}

void paste_over(std::vector<uint8_t>& dst, int dw, int dh,
                const uint8_t* src, int sw, int sh, int ox, int oy) {
    for (int y = 0; y < sh; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= dh) continue;
        for (int x = 0; x < sw; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= dw) continue;
            const uint8_t* sp = src + ((size_t)y * sw + x) * 4;
            uint8_t* dp = dst.data() + ((size_t)dy * dw + dx) * 4;
            uint8_t a = sp[3];
            if (a == 0) continue;
            if (a == 255) {
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
            } else {
                int ia = 255 - a;
                dp[0] = (uint8_t)((sp[0] * a + dp[0] * ia) / 255);
                dp[1] = (uint8_t)((sp[1] * a + dp[1] * ia) / 255);
                dp[2] = (uint8_t)((sp[2] * a + dp[2] * ia) / 255);
                dp[3] = (uint8_t)(a + dp[3] * ia / 255);
            }
        }
    }
}

struct Chan {
    int16_t id;
    std::vector<uint8_t> bytes;
};

void write_tagged(Buffer& b, const char* key, const std::vector<uint8_t>& data) {
    b.raw("8BIM");
    b.raw(key);
    b.u32((uint32_t)data.size());
    b.raw(data);
}

// Image resources: resolution info (ID 0x03ED, as written by real
// Photoshop). The 16.16 fixed-point values carry the document DPI so
// Photoshop reports the canvas resolution correctly.
static void write_image_resources(Buffer& b, double res_h, double res_v) {
    auto fixed16 = [](double v) -> uint32_t {
        if (v < 0) v = 0;
        if (v > 65535.0) v = 65535.0;
        return (uint32_t)(v * 65536.0 + 0.5);
    };
    Buffer data;
    data.u32(fixed16(res_h));   // horizontal resolution
    data.u16(1);                // horizontal resolution unit: pixels/inch
    data.u16(2);                // width unit: centimeters
    data.u32(fixed16(res_v));   // vertical resolution
    data.u16(1);                // vertical resolution unit: pixels/inch
    data.u16(2);                // height unit: centimeters

    Buffer res;
    res.raw("8BIM");
    res.u16(0x03ED);
    res.u8(0);                  // Pascal name: empty
    res.u8(0);                  // name padded to even length (2 bytes)
    res.u32((uint32_t)data.size());
    res.raw(data.data());
    if (data.size() % 2) res.u8(0);  // even padding
    b.raw(res.data());
}

}  // namespace

static bool build_document_bytes(const Document& doc, std::vector<uint8_t>& bytes,
                                 std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (doc.width <= 0 || doc.height <= 0) return fail("invalid document size");

    std::vector<Rec> recs;
    collect_records(doc.layers, recs);
    if (recs.empty()) return fail("document has no layers");

    // Layer pixel sources
    struct Px {
        std::vector<uint8_t> owned;
        const uint8_t* data = nullptr;
        int w = 0, h = 0;
    };
    std::vector<Px> px(recs.size());
    for (size_t i = 0; i < recs.size(); i++) {
        if (recs[i].lsct != 0) continue;
        int lw = 0, lh = 0;
        const uint8_t* src = nullptr;
        if (auto pl = dynamic_cast<const PixelLayer*>(recs[i].layer)) {
            lw = pl->w; lh = pl->h;
            if (pl->rgba.size() != (size_t)lw * lh * 4)
                return fail("pixel layer '" + pl->name + "' has wrong rgba size");
            src = pl->rgba.data();
        } else if (auto tl = dynamic_cast<const TextLayer*>(recs[i].layer)) {
            lw = tl->right - tl->left;
            lh = tl->bottom - tl->top;
            if (!tl->text.preview.empty()) {
                if (tl->text.preview.size() != (size_t)lw * lh * 4)
                    return fail("text layer '" + tl->name + "' preview size mismatch");
                src = tl->text.preview.data();
            }
        }
        if (lw <= 0 || lh <= 0) continue;
        if (!src) {
            px[i].owned.assign((size_t)lw * lh * 4, 0);
            src = px[i].owned.data();
        }
        px[i].data = src;
        px[i].w = lw;
        px[i].h = lh;
    }

    // Composite
    std::vector<uint8_t> comp((size_t)doc.width * doc.height * 4, 0);
    for (size_t i = 0; i < recs.size(); i++) {
        if (recs[i].lsct != 0 || !px[i].data) continue;
        const LayerBase* l = recs[i].layer;
        int ox = l->left, oy = l->top;
        if (auto pl = dynamic_cast<const PixelLayer*>(l)) {
            ox = pl->x;
            oy = pl->y;
        } else if (auto tl = dynamic_cast<const TextLayer*>(l)) {
            ox = (int)std::llround(tl->text.box_x);
            oy = (int)std::llround(tl->text.box_y);
        }
        paste_over(comp, doc.width, doc.height, px[i].data, px[i].w, px[i].h,
                   ox, oy);
    }
    bool has_alpha = false;
    for (size_t k = 3; k < comp.size(); k += 4)
        if (comp[k] != 255) { has_alpha = true; break; }
    int channels = has_alpha ? 4 : 3;

    // Channel data per record (alpha, R, G, B)
    std::vector<std::vector<Chan>> chan(recs.size());
    for (size_t i = 0; i < recs.size(); i++) {
        if (recs[i].lsct != 0 || !px[i].data) {
            for (int16_t id : {int16_t(-1), int16_t(0), int16_t(1), int16_t(2)}) {
                Buffer e;
                e.u16(0);  // RAW, no data
                chan[i].push_back({id, e.data()});
            }
            continue;
        }
        const uint8_t* src = px[i].data;
        int lw = px[i].w, lh = px[i].h;
        const int16_t ids[4] = {-1, 0, 1, 2};
        for (int c = 0; c < 4; c++) {
            std::vector<uint8_t> plane((size_t)lw * lh);
            for (size_t k = 0; k < plane.size(); k++)
                plane[k] = src[k * 4 + (c == 0 ? 3 : c - 1)];
            Buffer cb;
            write_rle_channel(cb, plane.data(), lw, lh);
            chan[i].push_back({ids[c], cb.data()});
        }
    }

    // Assemble layer info (layer records + channel image data)
    Buffer lm;
    lm.i16((int16_t)recs.size());

    int id_counter = 1;
    for (size_t i = 0; i < recs.size(); i++) {
        const LayerBase* l = recs[i].layer;
        int top = l->top, left = l->left, bottom = l->bottom, right = l->right;
        if (auto pl = dynamic_cast<const PixelLayer*>(l)) {
            top = pl->y; left = pl->x; bottom = pl->y + pl->h; right = pl->x + pl->w;
        } else if (auto tl = dynamic_cast<const TextLayer*>(l)) {
            top = (int)std::llround(tl->text.box_y);
            left = (int)std::llround(tl->text.box_x);
            bottom = (int)std::llround(tl->text.box_y + tl->text.box_h);
            right = (int)std::llround(tl->text.box_x + tl->text.box_w);
        }

        lm.i32(top); lm.i32(left); lm.i32(bottom); lm.i32(right);
        lm.u16((uint16_t)chan[i].size());
        for (const auto& c : chan[i]) {
            lm.i16(c.id);
            lm.u32((uint32_t)c.bytes.size());
        }
        lm.raw("8BIM");
        lm.raw("norm");
        lm.u8(255);   // opacity
        lm.u8(0);     // clipping
        lm.u8(l->visible ? 0x08 : 0x0A);  // flags (bit3 = PS5+)
        lm.u8(0);     // filler

        // Extra data
        Buffer extra;
        extra.u32(0);  // no layer mask
        // Blending ranges: 4-byte length + composite + 4 channel ranges
        extra.u32(40);
        // 2 composite pairs + 4 channels x 2 pairs = 10 pairs of 16-bit values
        for (int k = 0; k < 10; k++) { extra.u16(0); extra.u16(65535); }
        // Layer name (pascal string, UTF-8, padded to 4)
        const std::string& nm = (recs[i].lsct == 3) ? "</Layer group>" : l->name;
        std::string name8 = nm.substr(0, 254);
        extra.u8((uint8_t)name8.size());
        extra.raw(name8);
        size_t name_pad = (4 - (1 + name8.size()) % 4) % 4;
        for (size_t k = 0; k < name_pad; k++) extra.u8(0);

        // Tagged blocks
        if (recs[i].lsct != 0) {
            Buffer ls;
            ls.u32((uint32_t)recs[i].lsct);
            ls.raw("8BIM");
            ls.raw(recs[i].lsct == 3 ? "norm" : "pass");
            ls.u32(0);
            write_tagged(extra, "lsct", ls.data());
        }
        if (auto tl = dynamic_cast<const TextLayer*>(l)) {
            write_tagged(extra, "TySh", build_tysh(tl->text));
            std::vector<uint8_t> fx = build_lrfx(l->effects);
            if (!fx.empty()) write_tagged(extra, "lrFX", fx);
        } else {
            std::vector<uint8_t> fx = build_lrfx(l->effects);
            if (!fx.empty()) write_tagged(extra, "lrFX", fx);
        }
        {
            Buffer v;
            v.u32((uint32_t)nm.size());
            v.raw(utf8_to_utf16be(nm));
            write_tagged(extra, "luni", v.data());
        }
        write_tagged(extra, "lnsr",
                     std::vector<uint8_t>{0x72, 0x65, 0x6E, 0x64});
        {
            Buffer v;
            v.u32((uint32_t)id_counter++);
            write_tagged(extra, "lyid", v.data());
        }
        {
            Buffer v;
            v.u8(1); v.u8(0); v.u8(0); v.u8(0);
            write_tagged(extra, "clbl", v.data());
        }
        {
            Buffer v;
            v.u8(0); v.u8(0); v.u8(0); v.u8(0);
            write_tagged(extra, "infx", v.data());
            write_tagged(extra, "knko", v.data());
        }
        extra.pad(2);

        lm.u32((uint32_t)extra.size());
        lm.raw(extra.data());
    }

    // Channel image data
    for (const auto& chans : chan)
        for (const auto& c : chans) lm.raw(c.bytes);

    lm.pad(4);

    // Wrap into layer & mask information (length + layer info)
    Buffer lm_outer;
    lm_outer.u32((uint32_t)lm.size());
    lm_outer.raw(lm.data());

    Buffer out;
    out.raw("8BPS");
    out.u16(1);
    out.u32(0);      // reserved
    out.u16(0);      // reserved
    out.u16((uint16_t)channels);
    out.u32((uint32_t)doc.height);
    out.u32((uint32_t)doc.width);
    out.u16(8);      // 8 bits/channel
    out.u16(3);      // RGB
    out.u32(0);      // color mode data
    // Image resources (resolution block). Resources must be present for
    // Photoshop to display the intended canvas DPI.
    Buffer res;
    write_image_resources(res, doc.res_h, doc.res_v);
    out.u32((uint32_t)res.size());
    out.raw(res.data());
    out.u32((uint32_t)lm_outer.size());
    out.raw(lm_outer.data());

    // Merged image data (RLE)
    Buffer img;
    img.u16(1);
    std::vector<int> chan_idx;
    if (channels >= 1) chan_idx.push_back(0);
    if (channels >= 2) chan_idx.push_back(1);
    if (channels >= 3) chan_idx.push_back(2);
    if (channels >= 4) chan_idx.push_back(3);
    std::vector<std::vector<uint8_t>> rows;
    for (int ci : chan_idx) {
        for (int y = 0; y < doc.height; y++) {
            std::vector<uint8_t> row((size_t)doc.width);
            for (int x = 0; x < doc.width; x++)
                row[x] = comp[((size_t)y * doc.width + x) * 4 + ci];
            rows.push_back(packbits(row.data(), row.size()));
        }
    }
    for (size_t r = 0; r < rows.size(); r++) img.u16((uint16_t)rows[r].size());
    for (const auto& r : rows) img.raw(r);
    out.raw(img.data());

    bytes = out.data();
    return true;
}

bool Document::write(const std::string& path, std::string* error) const {
#ifdef _WIN32
    // std::ofstream uses the ANSI codepage for narrow paths, which breaks
    // non-ASCII paths (e.g. Chinese). Route through the wide-char API.
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0);
    std::wstring wpath((size_t)n, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), &wpath[0], n);
    return write_wide(wpath, error);
#else
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    std::vector<uint8_t> bytes;
    if (!build_document_bytes(*this, bytes, error)) return false;

    std::ofstream f(path, std::ios::binary);
    if (!f) return fail("cannot open output file: " + path);
    f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    if (!f) return fail("write failed: " + path);
    return true;
#endif
}

#ifdef _WIN32
bool Document::write_wide(const std::wstring& path, std::string* error) const {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    std::vector<uint8_t> bytes;
    if (!build_document_bytes(*this, bytes, error)) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("cannot open output file (wide)");
    DWORD written = 0;
    BOOL ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != bytes.size()) return fail("write failed (wide)");
    return true;
}
#endif

}  // namespace psdw
