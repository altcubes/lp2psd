// lp2psd - generate PSD files from a layout text file.
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
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "image.hpp"
#include "layout.hpp"
#include "minijson.hpp"
#include "ocr.hpp"
#include "psd_writer.hpp"
#include "style.hpp"
#include "textcodec.hpp"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

using textcodec::utf8_to_wide;
using textcodec::wide_to_utf8;

// Directory of the running executable, without a trailing separator.
std::wstring exe_directory() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    if (n == 0 || n >= std::size(buf)) return L"";
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

// Modal "open file" dialog for the layout text file. Returns false on cancel.
bool pick_text_file(std::wstring& out_path) {
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)std::size(file);
    ofn.lpstrTitle = L"选择排版文本文件";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return false;
    out_path = file;
    return true;
}

// Writes the default template config.json (UTF-8, no BOM) — the full
// parameter set with manga-friendly defaults. Returns false if the file
// already exists or cannot be created.
bool write_template_config(const std::wstring& path) {
    const char templ[] =
        "{\n"
        "  \"dpi\": \"original\",\n"
        "  \"outputDir\": \"\",\n"
        "  \"prefix\": \"\",\n"
        "  \"suffix\": \"\",\n"
        "  \"font\": {\n"
        "    \"name\": \"Microsoft YaHei\",\n"
        "    \"postScript\": \"\",\n"
        "    \"fontSize\": 24,\n"
        "    \"color\": [0, 0, 0],\n"
        "    \"antiAlias\": \"smooth\",\n"
        "    \"orientation\": \"vertical\",\n"
        "    \"justification\": \"center\",\n"
        "    \"autoLeading\": true,\n"
        "    \"autoLeadingSize\": 1.2,\n"
        "    \"leading\": 0,\n"
        "    \"discretionaryLigatures\": true,\n"
        "    \"standardVerticalRomanAlignment\": true,\n"
        "    \"script\": \"auto\"\n"
        "  },\n"
        "  \"ocr\": {\n"
        "    \"enabled\": false,\n"
        "    \"model\": \"dbnet_detect.onnx\",\n"
        "    \"limitSideLen\": 1024,\n"
        "    \"dbBinThreshold\": 0.5,\n"
        "    \"dbBoxThreshold\": 0.7,\n"
        "    \"dbUnclipRatio\": 2.3,\n"
        "    \"minSide\": 3.0,\n"
        "    \"segThreshold\": 0.12,\n"
        "    \"minBoxArea\": 64,\n"
        "    \"whiten\": {\n"
        "      \"enabled\": true,\n"
        "      \"color\": [255, 255, 255],\n"
        "      \"margin\": 3,\n"
        "      \"layerName\": \"whites\"\n"
        "    },\n"
        "    \"boxes\": {\n"
        "      \"enabled\": false,\n"
        "      \"color\": [255, 0, 0],\n"
        "      \"layerName\": \"ocr_boxes\"\n"
        "    }\n"
        "  }\n"
        "}\n";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, templ, (DWORD)(sizeof(templ) - 1), &written, nullptr);
    CloseHandle(h);
    return ok && written == sizeof(templ) - 1;
}

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

// ===========================================================================
// OCR-based whitening and font-size matching
// ===========================================================================

// Builds one pixel layer covering all detected text strokes: the per-pixel
// stroke mask dilated by cfg.margin (to swallow anti-aliasing edges), filled
// with opaque pixels. Returns null when there is nothing to draw.
std::shared_ptr<psdw::PixelLayer> make_whiten_layer(
    const std::vector<uint8_t>& stroke_mask, int iw, int ih,
    const OcrWhiten& cfg) {
    if (stroke_mask.size() != (size_t)iw * ih) return nullptr;
    int l = iw, t = ih, r = 0, b = 0;
    for (int y = 0; y < ih; y++) {
        const uint8_t* row = &stroke_mask[(size_t)y * iw];
        for (int x = 0; x < iw; x++) {
            if (!row[x]) continue;
            l = std::min(l, std::max(0, x - cfg.margin));
            t = std::min(t, std::max(0, y - cfg.margin));
            r = std::max(r, std::min(iw, x + cfg.margin + 1));
            b = std::max(b, std::min(ih, y + cfg.margin + 1));
        }
    }
    if (r <= l || b <= t) return nullptr;

    auto pl = std::make_shared<psdw::PixelLayer>();
    pl->name = cfg.layer_name;
    pl->x = l; pl->y = t;
    pl->w = r - l; pl->h = b - t;
    pl->rgba.assign((size_t)pl->w * pl->h * 4, 0);  // transparent
    const int m = std::max(0, cfg.margin);
    for (int y = 0; y < ih; y++) {
        const uint8_t* row = &stroke_mask[(size_t)y * iw];
        for (int x = 0; x < iw; x++) {
            if (!row[x]) continue;
            int x0 = std::max(l, x - m), x1 = std::min(r, x + m + 1);
            int y0 = std::max(t, y - m), y1 = std::min(b, y + m + 1);
            for (int yy = y0; yy < y1; yy++) {
                uint8_t* p = pl->rgba.data() +
                             ((size_t)(yy - t) * pl->w + (x0 - l)) * 4;
                for (int xx = x0; xx < x1; xx++) {
                    p[0] = cfg.color[0]; p[1] = cfg.color[1];
                    p[2] = cfg.color[2]; p[3] = 255;
                    p += 4;
                }
            }
        }
    }
    return pl;
}

// Expands a quad uniformly by `pad` pixels about its centroid.
void expand_quad(const double* quad, int pad, double out[8]) {
    double cx = 0, cy = 0;
    for (int k = 0; k < 4; k++) { cx += quad[k * 2]; cy += quad[k * 2 + 1]; }
    cx /= 4; cy /= 4;
    for (int k = 0; k < 4; k++) {
        double dx = quad[k * 2] - cx, dy = quad[k * 2 + 1] - cy;
        double len = std::max(1.0, std::hypot(dx, dy));
        out[k * 2] = quad[k * 2] + dx / len * pad;
        out[k * 2 + 1] = quad[k * 2 + 1] + dy / len * pad;
    }
}

// Draws a 1px line (Bresenham) into a pixel layer, clamped to its bounds.
void draw_layer_line(std::vector<uint8_t>& rgba, int lw, int lh, int ox, int oy,
                     int x0, int y0, int x1, int y1,
                     uint8_t r, uint8_t g, uint8_t b) {
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        int px = x0 - ox, py = y0 - oy;
        if (px >= 0 && px < lw && py >= 0 && py < lh) {
            uint8_t* p = rgba.data() + ((size_t)py * lw + px) * 4;
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Builds a transparent layer with a 1px outline around every detection quad
// (expanded by the same margin used for whitening, so it marks roughly where
// white was painted). Intended for the top of the layer stack. Returns null
// when there is nothing to draw.
std::shared_ptr<psdw::PixelLayer> make_box_outline_layer(
    const std::vector<OcrBox>& boxes, int iw, int ih, int margin,
    const OcrBoxes& cfg) {
    int l = iw, t = ih, r = 0, b = 0;
    for (const auto& box : boxes) {
        double q[8];
        expand_quad(box.quad, margin, q);
        for (int k = 0; k < 4; k++) {
            l = std::min(l, (int)std::floor(q[k * 2]));
            t = std::min(t, (int)std::floor(q[k * 2 + 1]));
            r = std::max(r, (int)std::ceil(q[k * 2]));
            b = std::max(b, (int)std::ceil(q[k * 2 + 1]));
        }
    }
    l = std::max(0, l); t = std::max(0, t);
    r = std::min(iw, r); b = std::min(ih, b);
    if (r <= l || b <= t) return nullptr;

    auto pl = std::make_shared<psdw::PixelLayer>();
    pl->name = cfg.layer_name;
    pl->x = l; pl->y = t;
    pl->w = r - l; pl->h = b - t;
    pl->rgba.assign((size_t)pl->w * pl->h * 4, 0);  // transparent
    for (const auto& box : boxes) {
        double q[8];
        expand_quad(box.quad, margin, q);
        for (int k = 0; k < 4; k++) {
            int k2 = (k + 1) % 4;
            draw_layer_line(pl->rgba, pl->w, pl->h, l, t,
                            (int)std::llround(q[k * 2]),
                            (int)std::llround(q[k * 2 + 1]),
                            (int)std::llround(q[k2 * 2]),
                            (int)std::llround(q[k2 * 2 + 1]),
                            cfg.color[0], cfg.color[1], cfg.color[2]);
        }
    }
    return pl;
}

// Draws a quad outline onto an RGBA image (debug overlay).
void draw_quad_outline(std::vector<uint8_t>& img, int iw, int ih,
                       const double* quad,
                       uint8_t r, uint8_t g, uint8_t b) {
    for (int k = 0; k < 4; k++) {
        int k2 = (k + 1) % 4;
        draw_layer_line(img, iw, ih, 0, 0,
                        (int)std::llround(quad[k * 2]),
                        (int)std::llround(quad[k * 2 + 1]),
                        (int)std::llround(quad[k2 * 2]),
                        (int)std::llround(quad[k2 * 2 + 1]), r, g, b);
    }
}

// Saves "<image stem>_ocr.png" with the stroke mask (green) and the rotated
// detection quads (red) drawn, into debug_dir.
void save_ocr_debug(const std::vector<uint8_t>& img, int iw, int ih,
                    const std::vector<OcrBox>& boxes,
                    const std::vector<uint8_t>& stroke_mask,
                    const std::wstring& debug_dir, const std::string& stem) {
    std::vector<uint8_t> overlay = img;
    if (stroke_mask.size() == (size_t)iw * ih) {
        for (size_t i = 0; i < stroke_mask.size(); i++) {
            if (!stroke_mask[i]) continue;
            uint8_t* p = overlay.data() + i * 4;
            p[0] = (uint8_t)std::min(255, p[0] / 2 + 128);  // tint green
            p[1] = (uint8_t)std::min(255, p[1] / 2 + 160);
            p[2] = (uint8_t)std::min(255, p[2] / 2 + 64);
        }
    }
    for (const auto& box : boxes)
        draw_quad_outline(overlay, iw, ih, box.quad, 255, 0, 0);
    save_image_png(debug_dir + L"\\" + utf8_to_wide(stem) + L"_ocr.png",
                   overlay, iw, ih);

    // Grayscale stroke mask + machine-readable quads, for parity checking
    // against scripts/parity_detect.py.
    std::vector<uint8_t> mask_rgba(stroke_mask.size() * 4, 0);
    for (size_t i = 0; i < stroke_mask.size(); i++) {
        uint8_t v = stroke_mask[i] ? 255 : 0;
        uint8_t* p = &mask_rgba[i * 4];
        p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
    }
    save_image_png(debug_dir + L"\\" + utf8_to_wide(stem) + L"_mask.png",
                   mask_rgba, iw, ih);

    std::string js = "{\"boxes\":[";
    for (size_t i = 0; i < boxes.size(); i++) {
        if (i) js += ",";
        const auto& box = boxes[i];
        js += "{\"quad\":[";
        for (int k = 0; k < 8; k++) {
            if (k) js += ",";
            char buf[32];
            snprintf(buf, sizeof(buf), "%.3f", box.quad[k]);
            js += buf;
        }
        js += "],\"score\":";
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6f", box.score);
        js += buf;
        js += "}";
    }
    js += "]}";
    HANDLE f = CreateFileW((debug_dir + L"\\" + utf8_to_wide(stem) +
                            L"_quads.json").c_str(),
                           GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(f, js.data(), (DWORD)js.size(), &written, nullptr);
        CloseHandle(f);
    }
}

std::shared_ptr<psdw::TextLayer> make_text_layer(const TextEntry& e, int iw,
                                                 int ih, double font_size,
                                                 const Style& style,
                                                 double dpi) {
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
    // Type sizes live in a 72-dpi point space, but the layer rectangle, the
    // preview raster and the TySh anchor are document pixels. Scale by
    // dpi/72 so a 20 pt layer occupies 20 * dpi/72 px in the document
    // (e.g. 26.7 px at 96 dpi) and reads back as 20 pt.
    const double px_scale = dpi / 72.0;
    double font_size_px = font_size * px_scale;
    double leading_px = (style.auto_leading ? font_size * style.auto_leading_size
                                            : (style.leading > 0.0 ? style.leading
                                                                   : font_size * style.auto_leading_size)) * px_scale;
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
    tl->text.dpi = dpi;

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

    return tl;
}

bool build_psd(const ImageBlock& blk, const std::wstring& txt_dir,
               const std::wstring& out_path, const Style& style,
               const Layout& layout, const std::wstring& debug_dir,
               std::string* err) {
    std::wstring image_path = txt_dir + utf8_to_wide(blk.image);
    std::vector<uint8_t> img;
    int iw = 0, ih = 0;
    double res_h = 96.0, res_v = 96.0;
    if (!load_image(image_path, img, iw, ih, &res_h, &res_v)) {
        if (err) *err = "cannot load image: " + blk.image;
        return false;
    }

    // OCR text-region detection (optional, config "ocr"). Runs on the raw
    // image; the rotated quads and the per-pixel stroke mask feed the
    // whitening layer below.
    std::vector<OcrBox> boxes;
    std::vector<uint8_t> stroke_mask;
    if (style.ocr.enabled) {
        std::string oerr;
        if (ocr_available()) {
            OcrOptions opt;
            opt.model_path = style.ocr.model;
            opt.limit_side_len = style.ocr.limit_side_len;
            opt.det_thresh = (float)style.ocr.det_thresh;
            opt.box_thresh = (float)style.ocr.box_thresh;
            opt.unclip_ratio = (float)style.ocr.unclip_ratio;
            opt.min_side = (float)style.ocr.min_side;
            opt.seg_thresh = (float)style.ocr.seg_thresh;
            opt.min_box_area = style.ocr.min_box_area;
            if (!ocr_detect(img, iw, ih, opt, boxes, stroke_mask, &oerr)) {
                fprintf(stderr, "ocr: %s\n", oerr.c_str());
            } else {
                size_t strokes = 0;
                for (uint8_t v : stroke_mask) strokes += v;
                printf("ocr  : %s -> %d region(s), %zu stroke pixel(s)\n",
                       blk.image.c_str(), (int)boxes.size(), strokes);
            }
            if (!debug_dir.empty())
                save_ocr_debug(img, iw, ih, boxes, stroke_mask, debug_dir,
                               stem_of(blk.image));
        } else {
            fprintf(stderr,
                    "ocr: onnxruntime.dll unavailable, skipping detection\n");
        }
    }

    psdw::Document doc;
    doc.width = iw;
    doc.height = ih;
    // Canvas DPI follows the source image (all images share the same DPI)
    // unless config "dpi" overrides it with a fixed value.
    if (res_h < 1.0) res_h = 96.0;
    if (res_v < 1.0) res_v = 96.0;
    if (style.dpi > 0.0) {
        res_h = style.dpi;
        res_v = style.dpi;
    }
    doc.res_h = res_h;
    doc.res_v = res_v;

    auto bg = std::make_shared<psdw::PixelLayer>();
    bg->name = "bg";
    bg->x = 0; bg->y = 0; bg->w = iw; bg->h = ih;
    bg->rgba = std::move(img);
    doc.layers.push_back(bg);

    // Whitening layer directly above the background: covers the OCR-detected
    // text regions without touching the "bg" pixels.
    if (style.ocr.enabled && style.ocr.whiten.enabled) {
        if (auto wl = make_whiten_layer(stroke_mask, iw, ih, style.ocr.whiten))
            doc.layers.push_back(wl);
    }

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
            auto tl = make_text_layer(e, iw, ih, font_size, style, res_h);
            if (tl) grp->children.push_back(tl);
        }
        if (!grp->children.empty()) doc.layers.push_back(grp);
    }

    // Entries without a group (group <= 0)
    for (const auto& e : blk.entries) {
        if (e.group > 0) continue;
        auto tl = make_text_layer(e, iw, ih, font_size, style, res_h);
        if (tl) doc.layers.push_back(tl);
    }

    // Detection-box outline layer on top: shows where white was painted.
    if (style.ocr.enabled && style.ocr.boxes.enabled) {
        if (auto bl = make_box_outline_layer(boxes, iw, ih,
                                             style.ocr.whiten.margin,
                                             style.ocr.boxes))
            doc.layers.push_back(bl);
    }

    return doc.write_wide(out_path, err);
}

// ===========================================================================
// CLI
// ===========================================================================
void print_usage() {
    printf("lp2psd - generate PSD files from a layout text file\n\n");
    printf("usage: lp2psd <layout.txt> [--out <dir>] [--config <style.json>]\n");
    printf("                     [--debug-ocr <dir>]\n\n");
    printf("With no arguments a file dialog asks for the layout text file.\n");
    printf("The layout file references images in the same folder (one block per\n");
    printf("image) and places text layers at normalized positions with optional\n");
    printf("group numbers. Each image produces <image>.psd.\n\n");
    printf("Optional OCR (config \"ocr\"): detects Japanese text regions for\n");
    printf("whitening (rotated quads + per-pixel stroke mask). Requires\n");
    printf("onnxruntime.dll + the DBNet detection model next to the exe.\n");
    printf("  --debug-ocr <dir>   save detection overlay PNGs/JSON to <dir>\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    GdiScope gdi;

    std::string txt_path, out_dir, cfg_path, debug_dir;
    for (int i = 1; i < argc; i++) {
        std::string a = wide_to_utf8(std::wstring(argv[i]));
        if (a == "--out" && i + 1 < argc) out_dir = wide_to_utf8(std::wstring(argv[++i]));
        else if (a == "--config" && i + 1 < argc)
            cfg_path = wide_to_utf8(std::wstring(argv[++i]));
        else if (a == "--debug-ocr" && i + 1 < argc)
            debug_dir = wide_to_utf8(std::wstring(argv[++i]));
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
        if (!pick_text_file(picked)) {
            printf("no file selected.\n");
            return 2;
        }
        txt_path = wide_to_utf8(picked);
    }

    // Font config: default to config.json next to the exe.
    if (cfg_path.empty()) {
        std::wstring def = exe_directory() + L"\\config.json";
        if (GetFileAttributesW(def.c_str()) != INVALID_FILE_ATTRIBUTES)
            cfg_path = wide_to_utf8(def);
    }
    // No config anywhere: create the template config.json next to the exe so
    // the user can edit it, then load it (this run keeps the defaults).
    if (cfg_path.empty()) {
        std::wstring def = exe_directory() + L"\\config.json";
        if (write_template_config(def)) {
            cfg_path = wide_to_utf8(def);
            printf("config: 未找到 config.json，已生成模板 %s\n", cfg_path.c_str());
        } else {
            printf("config: 未找到 config.json，且无法创建模板，使用内置默认样式\n");
        }
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

    // OCR model paths: absolute as-is; otherwise exe directory, then the
    // current working directory.
    auto resolve_model_path = [](std::string& path) {
        if (path.empty()) return;
        bool absolute = path.size() > 1 &&
                        (path[1] == ':' || path[0] == '/' || path[0] == '\\');
        if (absolute) return;
        std::wstring model_w = utf8_to_wide(path);
        std::wstring cand = exe_directory() + L"\\" + model_w;
        if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES)
            cand = model_w;
        path = wide_to_utf8(cand);
    };
    resolve_model_path(style.ocr.model);
    std::wstring wdebug_dir;
    if (!debug_dir.empty()) {
        wdebug_dir = utf8_to_wide(debug_dir);
        CreateDirectoryW(wdebug_dir.c_str(), nullptr);
    }

    printf("layout: %s (%d image block(s))\n", txt_path.c_str(),
           (int)layout.images.size());
    printf("output: %s\n", out_dir.c_str());
    printf("config: %s\n", cfg_path.empty() ? "(none, built-in defaults)" : cfg_path.c_str());
    printf("font  : %s (%.1f pt)\n", wide_to_utf8(style.font_name).c_str(),
           style.font_size_pt);

    int ok = 0;
    for (const ImageBlock& blk : layout.images) {
        std::string out_name =
            style.prefix + stem_of(blk.image) + style.suffix + ".psd";
        std::wstring out_path = wout_dir + L"\\" + utf8_to_wide(out_name);
        if (build_psd(blk, wtxt_dir, out_path, style, layout, wdebug_dir, &err)) {
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
