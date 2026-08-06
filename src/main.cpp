// psdgen - generate PSD files from a layout text file.
//
// Layout file format (see README "txt 格式"):
//   <GroupName> --- (psd group name for group N)      (one line per group)
//   >>>>>>>>>>[image.jpg]<<<<<<<<<<
//   ----------------[1]----------------[x,y,group]    (x,y in 0..1 of image)
//   text line 1
//   text line 2
//
// This file is the CLI orchestrator only: argument handling, file pickers,
// and assembling the psdw::Document model. Parsing lives in layout.*, style
// config in style.*, GDI+ image work in image.*, and text conversion in
// textcodec.*.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "image.hpp"
#include "layout.hpp"
#include "minijson.hpp"
#include "psd_writer.hpp"
#include "style.hpp"
#include "textcodec.hpp"
#include "windows_ui.hpp"

#pragma comment(lib, "shell32.lib")

namespace {

using textcodec::utf8_to_wide;
using textcodec::wide_to_utf8;

// ===========================================================================
// Path helpers
// ===========================================================================
std::string dir_of(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? "" : path.substr(0, p + 1);
}

std::string stem_of(const std::string& name) {
    size_t p = name.find_last_of('.');
    return p == std::string::npos ? name : name.substr(0, p);
}

std::string group_name_of(const Layout& layout, int g) {
    for (const auto& kv : layout.groups)
        if (kv.first == g) return kv.second;
    return "Group " + std::to_string(g);
}

// ===========================================================================
// Document building
// ===========================================================================

// Returns the GDI-loadable family name for the configured font. The config
// uses Photoshop PostScript names (e.g. "YWHeiTI-Medium"), which GDI cannot
// load directly; DirectWrite maps them to the Windows family name. An empty
// result means the font is not installed and the Txt2 block must be skipped.
std::wstring resolve_gdi_font(const Style& style) {
    static std::wstring cached_ps;
    static std::wstring cached_family;
    std::wstring ps = utf8_to_wide(style.font_ps);
    if (cached_ps == ps) return cached_family;
    std::wstring family;
    bool ok = resolve_font_family(ps, family);
    if (!ok) {
        family = style.font_name;
        ok = font_family_available(family);
    }
    cached_ps = ps;
    cached_family = ok ? family : L"";
    return cached_family;
}

// Photoshop renders ASCII letters/digits in vertical CJK manga fonts (e.g.
// YW HeiTI) with the full-width glyphs. Convert the glyph lookup text
// accordingly; the text stored in the PSD stays half-width.
bool vertical_fullwidth_font(const std::wstring& family) {
    std::wstring n;
    for (wchar_t c : family) {
        if (c >= L'A' && c <= L'Z') c += 32;
        if (c != L' ' && c != L'-') n += c;
    }
    return n.find(L"ywheit") != std::wstring::npos;
}

// A txt line may contain literal "\r" / "\n" escape sequences to force extra
// line breaks inside one physical line. Returns the resulting logical lines.
std::vector<std::string> split_escaped_lines(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == '\\' && i + 1 < line.size() &&
            (line[i + 1] == 'r' || line[i + 1] == 'n')) {
            out.push_back(cur);
            cur.clear();
            i++;
        } else {
            cur += line[i];
        }
    }
    out.push_back(cur);
    return out;
}

// Rough text width estimation (CJK glyphs = 1.0 em, Latin = 0.55 em).
double line_units(const std::string& line) {
    double units = 0;
    size_t i = 0;
    while (i < line.size()) {
        uint8_t c = (uint8_t)line[i];
        if (c < 0x80) {
            units += 0.55;
            i++;
        } else if ((c & 0xE0) == 0xC0) i += 2, units += 0.55;
        else if ((c & 0xF0) == 0xE0) i += 3, units += 1.0;
        else i += 4, units += 1.0;
    }
    return units;
}

std::shared_ptr<psdw::TextLayer> make_text_layer(const TextEntry& e, int iw,
                                                 int ih, double font_size,
                                                 const Style& style) {
    auto tl = std::make_shared<psdw::TextLayer>();
    std::vector<std::string> lines;
    for (const auto& ln : e.lines) {
        auto parts = split_escaped_lines(ln);
        lines.insert(lines.end(), parts.begin(), parts.end());
    }
    std::string text;
    double max_units = 1.0;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
        max_units = std::max(max_units, line_units(lines[i]));
    }
    if (text.empty()) return nullptr;

    double x = e.x * iw;
    double y = e.y * ih;
    // Photoshop lays type layers out in a fixed 72-dpi space (1 pt == 1 px
    // in the file), independent of the document resolution, so geometry and
    // previews use the point size directly.
    double font_size_px = font_size;
    double leading_px = style.auto_leading ? font_size * style.auto_leading_size
                                           : (style.leading > 0.0 ? style.leading
                                                                  : font_size * style.auto_leading_size);
    // Longest line in UTF-16 code units (vertical columns stack glyphs).
    int max_chars = 1;
    for (const auto& ln : lines)
        max_chars = std::max(max_chars, (int)utf8_to_wide(ln).size());
    double box_w, box_h;
    if (style.orientation == 1) {
        // vertical: each line is a column, columns advance right-to-left
        // Vertical: one em column per line, columns advance right-to-left;
        // column height = glyph count * font size (real PS behavior).
        box_w = std::max((double)lines.size() * font_size_px * 1.35, font_size_px * 1.35);
        box_h = std::max((double)max_chars * font_size_px, font_size_px);
    } else {
        box_w = std::max(max_units * font_size_px, font_size_px);
        box_h = std::max((double)lines.size() * leading_px, leading_px);
    }

    tl->name = lines[0].substr(0, 60);
    tl->text.text = text;
    tl->text.font = style.font_ps;
    tl->text.font_size = font_size;
    tl->text.color[0] = style.color[0];
    tl->text.color[1] = style.color[1];
    tl->text.color[2] = style.color[2];
    tl->text.box_x = x;
    tl->text.box_y = y;
    tl->text.box_w = box_w;
    tl->text.box_h = box_h;
    tl->text.anti_alias = style.anti_alias;
    tl->text.orientation = style.orientation;
    tl->text.justification = style.justification;
    tl->text.auto_leading = style.auto_leading;
    tl->text.auto_leading_size = style.auto_leading_size;
    tl->text.leading = style.leading;
    tl->text.discretionary_ligatures = style.discretionary_ligatures;
    tl->text.standard_vertical_roman = style.standard_vertical_roman;
    tl->text.script = style.script;

    // Layer geometry must match the record rectangle exactly:
    // w = round(x+w) - round(x)
    tl->left = (int)std::llround(x);
    tl->top = (int)std::llround(y);
    tl->right = (int)std::llround(x + box_w);
    tl->bottom = (int)std::llround(y + box_h);
    int pw = tl->right - tl->left;
    int ph = tl->bottom - tl->top;
    if (pw > 0 && ph > 0) {
        std::vector<std::wstring> wlines;
        for (const auto& ln : lines) wlines.push_back(utf8_to_wide(ln));
        std::vector<uint8_t> prev;
        if (render_text_preview(style.font_name, font_size_px, pw, ph, style.color,
                                style.orientation, style.justification, leading_px,
                                wlines, prev)) {
            tl->text.preview = std::move(prev);
            // Ink bounding box of the rendered glyphs, relative to the box.
            int il = pw, it = ph, ir = -1, ib = -1;
            for (int py = 0; py < ph; py++) {
                for (int px_ = 0; px_ < pw; px_++) {
                    if (tl->text.preview[((size_t)py * pw + px_) * 4 + 3] > 8) {
                        il = std::min(il, px_);
                        it = std::min(it, py);
                        ir = std::max(ir, px_);
                        ib = std::max(ib, py);
                    }
                }
            }
            if (ir >= il && ib >= it) {
                tl->text.ink_l = il;
                tl->text.ink_t = it;
                tl->text.ink_r = ir + 1;
                tl->text.ink_b = ib + 1;
            }
        }
    }

    // Glyph indices for the Txt2 text-engine-data block. The glyph text uses
    // a space for every line break plus a trailing space for the line-break
    // glyph, matching the engine text (newlines -> \r) unit for unit.
    // Photoshop validates these against the layer text on open.
    std::wstring gdi_family = resolve_gdi_font(style);
    if (!gdi_family.empty()) {
        std::wstring gtext;
        for (const auto& ln : lines) {
            gtext += utf8_to_wide(ln);
            gtext += L' ';
        }
        if (style.orientation == 1 && vertical_fullwidth_font(gdi_family)) {
            for (wchar_t& c : gtext)
                if (c >= 0x21 && c <= 0x7E) c += 0xFEE0;
        }
        std::vector<uint16_t> glyphs;
        if (get_glyph_indices(gdi_family, gtext, glyphs)) {
            tl->text.glyphs = std::move(glyphs);
            fprintf(stderr, "[dbg] family=%ls glyphs=%d\n", gdi_family.c_str(),
                    (int)tl->text.glyphs.size());
        } else {
            fprintf(stderr, "[dbg] get_glyph_indices FAILED family=%ls\n",
                    gdi_family.c_str());
        }
    }

    // Text runs (split at spaces) and layout metrics for the Txt2 block.
    // Photoshop stores one glyph run per text run; a space becomes its own
    // run with the font's space glyph.
    if (!tl->text.glyphs.empty()) {
        std::wstring chars;
        for (const auto& ln : lines) {
            chars += utf8_to_wide(ln);
            chars += L' ';
        }
        if (!chars.empty()) chars.pop_back();  // trailing break glyph
        if (style.orientation == 1 && vertical_fullwidth_font(gdi_family)) {
            for (wchar_t& c : chars)
                if (c >= 0x21 && c <= 0x7E) c += 0xFEE0;
        }

        std::vector<int> ends;
        int start = 0;
        for (int i = 0; i < (int)chars.size(); i++) {
            if (chars[i] == L' ') {
                if (i > start) ends.push_back(i);
                ends.push_back(i + 1);
                start = i + 1;
            }
        }
        if (start < (int)chars.size()) ends.push_back((int)chars.size());
        if (ends.empty()) ends.push_back((int)chars.size());
        tl->text.run_ends = std::move(ends);

        double asc_em = 0.8, desc_em = 0.2, space_adv = 0.3;
        get_font_layout_metrics(gdi_family, asc_em, desc_em, space_adv);
        tl->text.ascent_em = asc_em;
        tl->text.descent_em = desc_em;
        tl->text.space_advance_em = space_adv;

        const auto& ends_r = tl->text.run_ends;
        std::vector<double> extents;
        std::vector<bool> is_space;
        int prev = 0;
        for (size_t ri = 0; ri < ends_r.size(); ri++) {
            int end = ends_r[ri];
            int count = end - prev;
            bool sp = count == 1 && chars[prev] == L' ';
            is_space.push_back(sp);
            if (style.orientation == 1) {
                extents.push_back(sp ? space_adv : (double)count);
            } else {
                double adv = 0.0;
                get_text_advance_em(gdi_family, chars.substr(prev, count),
                                    adv);
                extents.push_back(adv);
            }
            prev = end;
        }
        tl->text.run_extent_em = std::move(extents);
        tl->text.run_is_space = std::move(is_space);
    }

    return tl;
}

bool build_psd(const ImageBlock& blk, const std::wstring& txt_dir,
               const std::wstring& out_path, const Style& style,
               const Layout& layout, std::string* err) {
    std::wstring image_path = txt_dir + utf8_to_wide(blk.image);
    std::vector<uint8_t> img;
    int iw = 0, ih = 0;
    double res_h = 96.0, res_v = 96.0;
    if (!load_image(image_path, img, iw, ih, &res_h, &res_v)) {
        if (err) *err = "cannot load image: " + blk.image;
        return false;
    }

    psdw::Document doc;
    doc.width = iw;
    doc.height = ih;
    // Canvas DPI follows the source image (all images share the same DPI).
    if (res_h < 1.0) res_h = 96.0;
    if (res_v < 1.0) res_v = 96.0;
    doc.res_h = res_h;
    doc.res_v = res_v;

    auto bg = std::make_shared<psdw::PixelLayer>();
    bg->name = "bg";
    bg->x = 0; bg->y = 0; bg->w = iw; bg->h = ih;
    bg->rgba = std::move(img);
    doc.layers.push_back(bg);

    // Font size in points (Photoshop unit). The document carries no resolution
    // info, so Photoshop uses 72 PPI and 1 pt == 1 px on screen.
    double font_size = style.font_size_pt;

    // Group numbers used by this image's entries (mapping order, then entry order)
    std::vector<int> gnums;
    for (const auto& kv : layout.groups)
        if (std::find(gnums.begin(), gnums.end(), kv.first) == gnums.end())
            gnums.push_back(kv.first);
    for (const auto& e : blk.entries)
        if (e.group > 0 && std::find(gnums.begin(), gnums.end(), e.group) == gnums.end())
            gnums.push_back(e.group);

    // Groups are stacked bottom-to-top; the first mapped group ends up on top.
    for (auto it = gnums.rbegin(); it != gnums.rend(); ++it) {
        int g = *it;
        auto grp = std::make_shared<psdw::Group>();
        grp->name = group_name_of(layout, g);
        grp->open = true;
        for (const auto& e : blk.entries) {
            if (e.group != g) continue;
            auto tl = make_text_layer(e, iw, ih, font_size, style);
            if (tl) grp->children.push_back(tl);
        }
        if (!grp->children.empty()) doc.layers.push_back(grp);
    }

    // Entries without a group (group <= 0)
    for (const auto& e : blk.entries) {
        if (e.group > 0) continue;
        auto tl = make_text_layer(e, iw, ih, font_size, style);
        if (tl) doc.layers.push_back(tl);
    }

    return doc.write_wide(out_path, err);
}

// ===========================================================================
// CLI
// ===========================================================================
void print_usage() {
    printf("psdgen - generate PSD files from a layout text file\n\n");
    printf("usage: psdgen <layout.txt> [--out <dir>] [--config <style.json>]\n\n");
    printf("With no arguments a file dialog asks for the layout text file.\n");
    printf("The layout file references images in the same folder (one block per\n");
    printf("image) and places text layers at normalized positions with optional\n");
    printf("group numbers. Each image produces <image>.psd.\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    GdiScope gdi;

    std::string txt_path, out_dir, cfg_path;
    for (int i = 1; i < argc; i++) {
        std::string a = wide_to_utf8(std::wstring(argv[i]));
        if (a == "--out" && i + 1 < argc) out_dir = wide_to_utf8(std::wstring(argv[++i]));
        else if (a == "--config" && i + 1 < argc)
            cfg_path = wide_to_utf8(std::wstring(argv[++i]));
        else if (a == "--help" || a == "-h") { print_usage(); return 0; }
        else if (txt_path.empty()) txt_path = a;
        else {
            fprintf(stderr, "unknown argument: %s\n", a.c_str());
            print_usage();
            return 2;
        }
    }
    if (txt_path.empty()) {
        // Interactive mode: double-click -> file picker.
        std::wstring picked;
        if (!ui::pick_text_file(picked)) {
            printf("no file selected.\n");
            return 2;
        }
        txt_path = wide_to_utf8(picked);
    }

    // Font config: default to config.json next to the exe.
    if (cfg_path.empty()) {
        std::wstring def = ui::exe_directory() + L"\\config.json";
        if (GetFileAttributesW(def.c_str()) != INVALID_FILE_ATTRIBUTES)
            cfg_path = wide_to_utf8(def);
    }

    std::string err;
    Layout layout;
    if (!parse_layout(utf8_to_wide(txt_path), layout, &err)) {
        fprintf(stderr, "layout parse error: %s\n", err.c_str());
        return 1;
    }

    Style style;
    if (!cfg_path.empty()) {
        std::wstring wcfg = utf8_to_wide(cfg_path);
        std::string text = textcodec::read_text_file(wcfg, &err);
        mjson::Value v;
        if (!mjson::parse(text, v, &err)) {
            fprintf(stderr, "config parse error: %s\n", err.c_str());
            return 1;
        }
        style = load_style(v);
    }

    std::string txt_dir = dir_of(txt_path);
    // Output always goes to "<text file dir>\output" (created if missing),
    // unless --out was given explicitly on the command line.
    if (out_dir.empty())
        out_dir = txt_dir.empty() ? "output" : txt_dir + "output";
    std::wstring wout_dir = utf8_to_wide(out_dir);
    CreateDirectoryW(wout_dir.c_str(), nullptr);
    std::wstring wtxt_dir = utf8_to_wide(txt_dir);

    printf("layout: %s (%d image block(s))\n", txt_path.c_str(),
           (int)layout.images.size());
    printf("output: %s\n", out_dir.c_str());
    printf("font  : %s (%.1f pt)\n", wide_to_utf8(style.font_name).c_str(),
           style.font_size_pt);

    int ok = 0;
    for (const ImageBlock& blk : layout.images) {
        std::string out_name =
            style.prefix + stem_of(blk.image) + style.suffix + ".psd";
        std::wstring out_path = wout_dir + L"\\" + utf8_to_wide(out_name);
        if (build_psd(blk, wtxt_dir, out_path, style, layout, &err)) {
            printf("OK   %s -> %s\n", blk.image.c_str(), out_name.c_str());
            ok++;
        } else {
            fprintf(stderr, "FAIL %s: %s\n", blk.image.c_str(), err.c_str());
        }
    }
    printf("done: %d/%d\n", ok, (int)layout.images.size());
    if (ok == (int)layout.images.size()) {
        // All files generated: open the output folder in Explorer.
        ShellExecuteW(nullptr, L"open", L"explorer.exe", wout_dir.c_str(), nullptr,
                      SW_SHOWNORMAL);
        return 0;
    }
    return 1;
}
