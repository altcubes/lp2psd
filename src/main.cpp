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
#include "dbnet.hpp"
#include "psd_writer.hpp"
#include "style.hpp"
#include "textmetrics.hpp"
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
        "  \"dbnet\": {\n"
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
        "      \"boxMarginX\": 3,\n"
        "      \"boxMarginY\": 3,\n"
        "      \"limitToBoxes\": true,\n"
        "      \"layerName\": \"whites\",\n"
        "      \"transparency\": 0\n"
        "    },\n"
        "    \"boxes\": {\n"
        "      \"enabled\": false,\n"
        "      \"color\": [255, 0, 0],\n"
        "      \"layerName\": \"dbnet_boxes\",\n"
        "      \"lock\": true\n"
        "    }\n"
        "  },\n"
        "  \"bgCopy\": {\n"
        "    \"enabled\": false,\n"
        "    \"layerName\": \"bg 拷贝\"\n"
        "  },\n"
        "  \"layers\": {\n"
        "    \"opacity\": 100\n"
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

// ===========================================================================
// dbnet-based whitening and font-size matching
// ===========================================================================

// Expands a quad about its centroid along the image axes: `pad_x` moves each
// vertex outward horizontally (x), `pad_y` vertically (y). For axis-aligned
// boxes this grows the width by 2*pad_x and the height by 2*pad_y; rotated
// quads keep their orientation while the vertices move along x/y.
void expand_quad(const double* quad, int pad_x, int pad_y, double out[8]) {
    double cx = 0, cy = 0;
    for (int k = 0; k < 4; k++) { cx += quad[k * 2]; cy += quad[k * 2 + 1]; }
    cx /= 4; cy /= 4;
    for (int k = 0; k < 4; k++) {
        double dx = quad[k * 2] - cx, dy = quad[k * 2 + 1] - cy;
        double sx = dx > 0 ? 1.0 : (dx < 0 ? -1.0 : 0.0);
        double sy = dy > 0 ? 1.0 : (dy < 0 ? -1.0 : 0.0);
        out[k * 2] = quad[k * 2] + sx * pad_x;
        out[k * 2 + 1] = quad[k * 2 + 1] + sy * pad_y;
    }
}

// Statistics for the box-limited whitening scheme (see make_whiten_layer).
struct WhitenStats {
    size_t mask_px = 0;     // stroke pixels
    size_t dilated_px = 0;  // after margin dilation
    size_t final_px = 0;    // after intersection with expanded boxes
};

// Rasterizes the quads (each expanded by pad_x/pad_y about its centroid)
// into a binary "inside any box" mask via even-odd scanline fill. Uses the
// same expansion as make_box_outline_layer, so the outlines and the allowed
// whitening area always agree.
void rasterize_quads(const std::vector<dbnetBox>& boxes, int iw, int ih,
                     int pad_x, int pad_y, std::vector<uint8_t>& out) {
    out.assign((size_t)iw * ih, 0);
    for (const auto& box : boxes) {
        double q[8];
        expand_quad(box.quad, pad_x, pad_y, q);
        // double min_y = std::floor(q[1]), max_y = std::ceil(q[7]);
        // for (int k = 1; k < 4; k++) {
        //     min_y = std::min(min_y, std::floor(q[k * 2 + 1]));
        //     max_y = std::max(max_y, std::ceil(q[k * 2 + 1]));
        // }
        double min_y = q[1], max_y = q[1];
        for (int k = 1; k < 4; k++) {
            min_y = std::min(min_y, q[k * 2 + 1]);
            max_y = std::max(max_y, q[k * 2 + 1]);
        }
        min_y = std::floor(min_y);
        max_y = std::ceil(max_y);
        for (int y = std::max(0, (int)min_y);
             y <= std::min(ih - 1, (int)max_y); y++) {
            double spans[4];
            int n = 0;
            for (int k = 0; k < 4; k++) {
                int k2 = (k + 1) % 4;
                double y0 = q[k * 2 + 1], y1 = q[k2 * 2 + 1];
                if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                    double x0 = q[k * 2], x1 = q[k2 * 2];
                    spans[n++] = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
                }
            }
            if (n < 2) continue;
            std::sort(spans, spans + n);
            uint8_t* row = out.data() + (size_t)y * iw;
            for (int i = 0; i + 1 < n; i += 2) {
                int x0 = std::max(0, (int)std::ceil(spans[i]));
                int x1 = std::min(iw, (int)std::ceil(spans[i + 1]));
                for (int x = x0; x < x1; x++) row[x] = 1;
            }
        }
    }
}

// Binary dilation by a square of radius r, via separable sliding windows
// (O(w*h), pixel-identical to the per-pixel box fill used by the legacy
// whitening path).
std::vector<uint8_t> dilate_binary(const std::vector<uint8_t>& src, int w,
                                   int h, int r) {
    if (r <= 0) return src;
    std::vector<uint8_t> hd((size_t)w * h, 0);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = &src[(size_t)y * w];
        uint8_t* out = &hd[(size_t)y * w];
        int sum = 0;
        // Window for x is [x-r, x+r]; the left edge is clipped for x < r.
        for (int x = 0; x < w && x <= r; x++) sum += row[x];
        for (int x = 0; x < w; x++) {
            out[x] = sum > 0 ? 1 : 0;
            if (x - r >= 0) sum -= row[x - r];
            if (x + r + 1 < w) sum += row[x + r + 1];
        }
    }
    std::vector<uint8_t> out((size_t)w * h, 0);
    for (int x = 0; x < w; x++) {
        int sum = 0;
        for (int y = 0; y < h && y <= r; y++) sum += hd[(size_t)y * w + x];
        for (int y = 0; y < h; y++) {
            out[(size_t)y * w + x] = sum > 0 ? 1 : 0;
            if (y - r >= 0) sum -= hd[(size_t)(y - r) * w + x];
            if (y + r + 1 < h) sum += hd[(size_t)(y + r + 1) * w + x];
        }
    }
    return out;
}

// Builds one pixel layer from a binary region (1 = paint): tight layer
// bounds plus opaque runs of cfg.color. Returns null when region is empty.
std::shared_ptr<psdw::PixelLayer> make_layer_from_region(
    const std::vector<uint8_t>& region, int iw, int ih,
    const dbnetWhiten& cfg) {
    int l = iw, t = ih, r = 0, b = 0;
    for (int y = 0; y < ih; y++) {
        const uint8_t* row = &region[(size_t)y * iw];
        for (int x = 0; x < iw; x++) {
            if (!row[x]) continue;
            l = std::min(l, x);
            t = std::min(t, y);
            r = std::max(r, x + 1);
            b = std::max(b, y + 1);
        }
    }
    if (r <= l || b <= t) return nullptr;

    auto pl = std::make_shared<psdw::PixelLayer>();
    pl->name = cfg.layer_name;
    pl->x = l; pl->y = t;
    pl->w = r - l; pl->h = b - t;
    pl->rgba.assign((size_t)pl->w * pl->h * 4, 0);  // transparent
    for (int y = t; y < b; y++) {
        const uint8_t* row = &region[(size_t)y * iw];
        int x = l;
        while (x < r) {
            if (!row[x]) { x++; continue; }
            int x1 = x;
            while (x1 < r && row[x1]) x1++;
            uint8_t* p = pl->rgba.data() +
                         ((size_t)(y - t) * pl->w + (x - l)) * 4;
            for (int xx = x; xx < x1; xx++) {
                p[0] = cfg.color[0]; p[1] = cfg.color[1];
                p[2] = cfg.color[2]; p[3] = 255;
                p += 4;
            }
            x = x1;
        }
    }
    return pl;
}

// Builds the whitening pixel layer.
//
// limit_to_boxes = true (default): every stroke pixel is dilated by
// cfg.margin, then the result is intersected with the detection quads
// expanded by cfg.box_margin_x/cfg.box_margin_y. Whitening therefore never
// leaves the dbnet_boxes outlines, and margin keeps swallowing AA edges.
// limit_to_boxes = false: legacy behavior - the whole stroke mask is
// dilated by cfg.margin with no box restriction.
//
// `final_mask_out` receives the final binary region (1 = painted) and
// `stats_out` the pixel counts, both for debug output/statistics.
std::shared_ptr<psdw::PixelLayer> make_whiten_layer(
    const std::vector<uint8_t>& stroke_mask, const std::vector<dbnetBox>& boxes,
    int iw, int ih, const dbnetWhiten& cfg,
    std::vector<uint8_t>* final_mask_out = nullptr,
    WhitenStats* stats_out = nullptr) {
    if (stroke_mask.size() != (size_t)iw * ih) return nullptr;

    if (!cfg.limit_to_boxes) {
        // Legacy scheme, kept intact for zero behavior change.
        const int m = std::max(0, cfg.margin);
        int l = iw, t = ih, r = 0, b = 0;
        for (int y = 0; y < ih; y++) {
            const uint8_t* row = &stroke_mask[(size_t)y * iw];
            for (int x = 0; x < iw; x++) {
                if (!row[x]) continue;
                l = std::min(l, std::max(0, x - m));
                t = std::min(t, std::max(0, y - m));
                r = std::max(r, std::min(iw, x + m + 1));
                b = std::max(b, std::min(ih, y + m + 1));
            }
        }
        if (r <= l || b <= t) return nullptr;

        auto pl = std::make_shared<psdw::PixelLayer>();
        pl->name = cfg.layer_name;
        pl->x = l; pl->y = t;
        pl->w = r - l; pl->h = b - t;
        pl->rgba.assign((size_t)pl->w * pl->h * 4, 0);  // transparent
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

    const int m = std::max(0, cfg.margin);
    const int bm_x = std::max(0, cfg.box_margin_x);
    const int bm_y = std::max(0, cfg.box_margin_y);
    std::vector<uint8_t> dilated = dilate_binary(stroke_mask, iw, ih, m);
    std::vector<uint8_t> allowed;
    rasterize_quads(boxes, iw, ih, bm_x, bm_y, allowed);
    std::vector<uint8_t> region((size_t)iw * ih, 0);
    size_t final_px = 0;
    for (size_t i = 0; i < region.size(); i++) {
        if (dilated[i] && allowed[i]) {
            region[i] = 1;
            final_px++;
        }
    }
    if (stats_out) {
        size_t mask_px = 0, dilated_px = 0;
        for (size_t i = 0; i < region.size(); i++) {
            mask_px += stroke_mask[i];
            dilated_px += dilated[i];
        }
        stats_out->mask_px = mask_px;
        stats_out->dilated_px = dilated_px;
        stats_out->final_px = final_px;
    }
    if (final_mask_out) *final_mask_out = region;
    return make_layer_from_region(region, iw, ih, cfg);
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
// (expanded by margin_x/margin_y, the whiten boxMarginX/boxMarginY in the new
// scheme), so it marks exactly where the box-limited whitening may reach.
// Intended for the top of the layer stack. Returns null when nothing to draw.
std::shared_ptr<psdw::PixelLayer> make_box_outline_layer(
    const std::vector<dbnetBox>& boxes, int iw, int ih, int margin_x,
    int margin_y,
    const dbnetBoxes& cfg) {
    int l = iw, t = ih, r = 0, b = 0;
    for (const auto& box : boxes) {
        double q[8];
        expand_quad(box.quad, margin_x, margin_y, q);
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
        expand_quad(box.quad, margin_x, margin_y, q);
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

// Saves a binary mask (1 = set) as a grayscale PNG "<stem><suffix>.png".
void save_mask_png(const std::wstring& debug_dir, const std::string& stem,
                   const char* suffix, const std::vector<uint8_t>& mask,
                   int iw, int ih) {
    if (mask.size() != (size_t)iw * ih) return;
    std::vector<uint8_t> rgba(mask.size() * 4, 0);
    for (size_t i = 0; i < mask.size(); i++) {
        uint8_t v = mask[i] ? 255 : 0;
        uint8_t* p = &rgba[i * 4];
        p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
    }
    save_image_png(debug_dir + L"\\" + utf8_to_wide(stem) +
                       utf8_to_wide(suffix) + L".png",
                   rgba, iw, ih);
}

// Saves "<image stem>_dbnet.png" with the stroke mask (green) and the rotated
// detection quads (red) drawn, into debug_dir.
void save_dbnet_debug(const std::vector<uint8_t>& img, int iw, int ih,
                    const std::vector<dbnetBox>& boxes,
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
    save_image_png(debug_dir + L"\\" + utf8_to_wide(stem) + L"_dbnet.png",
                   overlay, iw, ih);

    // Grayscale stroke mask + machine-readable quads, for parity checking
    // against scripts/parity_detect.py.
    if (stroke_mask.size() == (size_t)iw * ih)
        save_mask_png(debug_dir, stem, "_mask", stroke_mask, iw, ih);

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

// Applies the config "layers" section (opacity) to a layer.
void apply_layer_settings(psdw::LayerBase* l, const Style& style) {
    if (!l) return;
    l->opacity = (uint8_t)std::llround(style.layers.opacity * 255.0 / 100.0);
}

// Full lock: protects transparent pixels, image pixels and position
// (lspf = 0x07, Photoshop "Lock all").
void apply_full_lock(psdw::LayerBase* l) {
    if (!l) return;
    l->transparency_locked = true;
    l->composite_locked = true;
    l->position_locked = true;
}

// Renders the raster preview for a text layer and measures its ink box
// (relative to the layer box top-left). Leaves the preview empty on failure.
void render_text_preview_and_ink(psdw::TextLayer& tl,
                                 const std::vector<std::string>& lines,
                                 double font_size_px, int pw, int ph,
                                 const Style& style, double leading_px) {
    if (pw <= 0 || ph <= 0) return;
    std::vector<std::wstring> wlines;
    for (const auto& ln : lines) wlines.push_back(utf8_to_wide(ln));
    std::vector<uint8_t> prev;
    if (!render_text_preview(style.font_name, font_size_px, pw, ph,
                             style.color, style.orientation,
                             style.justification, leading_px, wlines, prev))
        return;
    tl.text.preview = std::move(prev);

    // Ink bounding box of the rendered glyphs, relative to the box.
    int il = pw, it = ph, ir = -1, ib = -1;
    for (int py = 0; py < ph; py++) {
        for (int px_ = 0; px_ < pw; px_++) {
            if (tl.text.preview[((size_t)py * pw + px_) * 4 + 3] > 8) {
                il = std::min(il, px_);
                it = std::min(it, py);
                ir = std::max(ir, px_);
                ib = std::max(ib, py);
            }
        }
    }
    if (ir >= il && ib >= it) {
        tl.text.ink_l = il;
        tl.text.ink_t = it;
        tl.text.ink_r = ir + 1;
        tl.text.ink_b = ib + 1;
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
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
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
    // Shared with psd_writer.cpp (TySh em bounds) so layer geometry and the
    // text engine stay in agreement.
    textmetrics::Box box =
        textmetrics::estimate_box(lines, style.orientation, font_size_px,
                                  leading_px);
    const double box_w = box.w, box_h = box.h;

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
    apply_layer_settings(tl.get(), style);

    // Layer geometry must match the record rectangle exactly:
    // w = round(x+w) - round(x)
    tl->left = (int)std::llround(x);
    tl->top = (int)std::llround(y);
    tl->right = (int)std::llround(x + box_w);
    tl->bottom = (int)std::llround(y + box_h);
    int pw = tl->right - tl->left;
    int ph = tl->bottom - tl->top;
    render_text_preview_and_ink(*tl, lines, font_size_px, pw, ph, style,
                                leading_px);

    return tl;
}

// Adds the background and its optional copy.
void add_background_layers(psdw::Document& doc, std::vector<uint8_t>& img,
                           int iw, int ih, const Style& style) {
    auto bg = std::make_shared<psdw::PixelLayer>();
    bg->name = "bg";
    bg->x = 0; bg->y = 0; bg->w = iw; bg->h = ih;
    bg->rgba = std::move(img);
    apply_layer_settings(bg.get(), style);
    doc.layers.push_back(bg);

    // Optional copy of the background, directly above "bg" and below the
    // "whites" whitening layer.
    if (style.bg_copy.enabled) {
        auto bgc = std::make_shared<psdw::PixelLayer>();
        bgc->name = style.bg_copy.layer_name;
        bgc->x = 0; bgc->y = 0; bgc->w = iw; bgc->h = ih;
        bgc->rgba = bg->rgba;
        apply_layer_settings(bgc.get(), style);
        doc.layers.push_back(bgc);
    }

}

// Adds the whitening layer directly above the background (below the text
// layers), and prints/debug-dumps the box-limited scheme when enabled.
void add_whiten_layer(psdw::Document& doc,
                      const std::vector<uint8_t>& stroke_mask,
                      const std::vector<dbnetBox>& boxes, int iw, int ih,
                      const Style& style, const std::wstring& debug_dir,
                      const std::string& stem) {
    if (!style.dbnet.enabled || !style.dbnet.whiten.enabled) return;

    WhitenStats stats;
    std::vector<uint8_t> final_mask;
    auto wl = make_whiten_layer(stroke_mask, boxes, iw, ih, style.dbnet.whiten,
                                &final_mask, &stats);
    if (style.dbnet.whiten.limit_to_boxes) {
        // Debug dump: final whiten region (gray = painted) for tuning
        // without opening the PSD.
        if (!debug_dir.empty())
            save_mask_png(debug_dir, stem, "_whiten", final_mask, iw, ih);
        double kept = stats.dilated_px
                          ? (double)stats.final_px * 100.0 / stats.dilated_px
                          : 0.0;
        printf("dbnet  : %s whiten mask=%zu dilated=%zu boxed=%zu "
               "(kept %.1f%%, dropped %.1f%%)\n",
               stem.c_str(), stats.mask_px, stats.dilated_px, stats.final_px,
               kept, 100.0 - kept);
    }
    if (!wl) return;

    // Global layer opacity first; the whiten-specific transparency setting
    // (both 0-100 percent) multiplies it.
    apply_layer_settings(wl.get(), style);
    wl->opacity =
        (uint8_t)std::llround((double)wl->opacity *
                              style.dbnet.whiten.opacity / 100.0);
    doc.layers.push_back(wl);
}

// Adds the text layers (grouped, then ungrouped) from the layout entries.
void add_text_layers(psdw::Document& doc, const ImageBlock& blk, int iw,
                     int ih, const Style& style, const Layout& layout,
                     double dpi) {
    const double font_size = style.font_size_pt;

    // Group numbers used by this image's entries (mapping order, then entry
    // order).
    std::vector<int> gnums;
    for (const auto& kv : layout.groups)
        if (std::find(gnums.begin(), gnums.end(), kv.first) == gnums.end())
            gnums.push_back(kv.first);
    for (const auto& e : blk.entries)
        if (e.group > 0 &&
            std::find(gnums.begin(), gnums.end(), e.group) == gnums.end())
            gnums.push_back(e.group);

    // Groups are stacked bottom-to-top; the first mapped group ends up on top.
    for (auto it = gnums.rbegin(); it != gnums.rend(); ++it) {
        int g = *it;
        auto grp = std::make_shared<psdw::Group>();
        grp->name = group_name_of(layout, g);
        grp->open = true;
        for (const auto& e : blk.entries) {
            if (e.group != g) continue;
            auto tl = make_text_layer(e, iw, ih, font_size, style, dpi);
            if (tl) grp->children.push_back(tl);
        }
        if (!grp->children.empty()) doc.layers.push_back(grp);
    }

    // Entries without a group (group <= 0)
    for (const auto& e : blk.entries) {
        if (e.group > 0) continue;
        auto tl = make_text_layer(e, iw, ih, font_size, style, dpi);
        if (tl) doc.layers.push_back(tl);
    }
}

// Optional detection-box outline layer on top of the stack.
void add_box_outline_layer(psdw::Document& doc,
                           const std::vector<dbnetBox>& boxes, int iw, int ih,
                           const Style& style) {
    if (!style.dbnet.enabled || !style.dbnet.boxes.enabled) return;
    if (auto bl = make_box_outline_layer(boxes, iw, ih,
                                         style.dbnet.whiten.box_margin_x,
                                         style.dbnet.whiten.box_margin_y,
                                         style.dbnet.boxes)) {
        apply_layer_settings(bl.get(), style);
        if (style.dbnet.boxes.lock) apply_full_lock(bl.get());
        doc.layers.push_back(bl);
    }
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

    // dbnet text-region detection (optional, config "dbnet"). Runs on the raw
    // image; the rotated quads and the per-pixel stroke mask feed the
    // whitening layer below.
    std::vector<dbnetBox> boxes;
    std::vector<uint8_t> stroke_mask;
    if (style.dbnet.enabled) {
        std::string oerr;
        if (dbnet_available()) {
            dbnetOptions opt;
            opt.model_path = style.dbnet.model;
            opt.limit_side_len = style.dbnet.limit_side_len;
            opt.det_thresh = (float)style.dbnet.det_thresh;
            opt.box_thresh = (float)style.dbnet.box_thresh;
            opt.unclip_ratio = (float)style.dbnet.unclip_ratio;
            opt.min_side = (float)style.dbnet.min_side;
            opt.seg_thresh = (float)style.dbnet.seg_thresh;
            opt.min_box_area = style.dbnet.min_box_area;
            if (!dbnet_detect(img, iw, ih, opt, boxes, stroke_mask, &oerr)) {
                fprintf(stderr, "dbnet: %s\n", oerr.c_str());
            } else {
                size_t strokes = 0;
                for (uint8_t v : stroke_mask) strokes += v;
                printf("dbnet  : %s -> %d region(s), %zu stroke pixel(s)\n",
                       blk.image.c_str(), (int)boxes.size(), strokes);
            }
            if (!debug_dir.empty())
                save_dbnet_debug(img, iw, ih, boxes, stroke_mask, debug_dir,
                               stem_of(blk.image));
        } else {
            fprintf(stderr,
                    "dbnet: onnxruntime.dll unavailable, skipping detection\n");
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

    add_background_layers(doc, img, iw, ih, style);
    add_whiten_layer(doc, stroke_mask, boxes, iw, ih, style, debug_dir,
                     stem_of(blk.image));
    add_text_layers(doc, blk, iw, ih, style, layout, res_h);
    add_box_outline_layer(doc, boxes, iw, ih, style);

    return doc.write_wide(out_path, err);
}

// ===========================================================================
// CLI
// ===========================================================================
void print_usage() {
    printf("lp2psd - generate PSD files from a layout text file\n\n");
    printf("usage: lp2psd <layout.txt> [--out <dir>] [--config <style.json>]\n");
    printf("                     [--debug-dbnet <dir>]\n\n");
    printf("With no arguments a file dialog asks for the layout text file.\n");
    printf("The layout file references images in the same folder (one block per\n");
    printf("image) and places text layers at normalized positions with optional\n");
    printf("group numbers. Each image produces <image>.psd.\n\n");
    printf("Optional dbnet (config \"dbnet\"): detects Japanese text regions for\n");
    printf("whitening (rotated quads + per-pixel stroke mask). Requires\n");
    printf("onnxruntime.dll + the DBNet detection model next to the exe.\n");
    printf("  --debug-dbnet <dir>   save detection overlay PNGs/JSON to <dir>\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    GdiScope gdi;

    std::string txt_path, out_dir, cfg_path, debug_dir;
    for (int i = 1; i < argc; i++) {
        std::string a = wide_to_utf8(std::wstring(argv[i]));
        auto need_value = [&](const char* flag) {
            if (i + 1 < argc) return true;
            fprintf(stderr, "missing value for %s\n", flag);
            print_usage();
            return false;
        };
        if (a == "--out") {
            if (!need_value("--out")) return 2;
            out_dir = wide_to_utf8(std::wstring(argv[++i]));
        } else if (a == "--config") {
            if (!need_value("--config")) return 2;
            cfg_path = wide_to_utf8(std::wstring(argv[++i]));
        } else if (a == "--debug-dbnet") {
            if (!need_value("--debug-dbnet")) return 2;
            debug_dir = wide_to_utf8(std::wstring(argv[++i]));
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else if (txt_path.empty()) txt_path = a;
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

    // dbnet model paths: absolute as-is; otherwise exe directory, then the
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
    resolve_model_path(style.dbnet.model);
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
        std::string out_name = stem_of(blk.image) + ".psd";
        // Layout files may reference images in subfolders; flatten the name
        // so the output stays a single file inside the output directory.
        std::replace(out_name.begin(), out_name.end(), '/', '_');
        std::replace(out_name.begin(), out_name.end(), '\\', '_');
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
