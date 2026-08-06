#include "image.hpp"

#include <gdiplus.h>
#include <dwrite.h>
#include <cstdio>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwrite.lib")

using namespace Gdiplus;

namespace {

// Reads JPEG JFIF/EXIF DPI from the raw file bytes (GDI+ does not expose the
// JFIF density through PropertyItem). Falls back to GDI+ properties for other
// formats (PNG etc.), and to 96 when nothing is embedded.
void read_image_dpi(const std::wstring& path, double& out_h, double& out_v) {
    out_h = out_v = 96.0;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::string bytes((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFF && (uint8_t)bytes[1] == 0xD8) {
        // JPEG: walk APPn segments. APP0/JFIF carries the density.
        size_t p = 2;
        while (p + 4 <= bytes.size()) {
            if ((uint8_t)bytes[p] != 0xFF) break;
            uint8_t marker = (uint8_t)bytes[p + 1];
            if (marker == 0xD8 || marker == 0xD9) break;      // SOI/EOI
            if (marker == 0xDA) break;                        // SOS: scan data
            size_t len = ((uint8_t)bytes[p + 2] << 8) | (uint8_t)bytes[p + 3];
            if (len < 2 || p + 2 + len > bytes.size()) break;
            if (marker == 0xE0 && len >= 16 &&
                bytes.compare(p + 4, 4, "JFIF", 0, 4) == 0) {
                uint8_t units = (uint8_t)bytes[p + 11];
                uint16_t xd = ((uint8_t)bytes[p + 12] << 8) | (uint8_t)bytes[p + 13];
                uint16_t yd = ((uint8_t)bytes[p + 14] << 8) | (uint8_t)bytes[p + 15];
                if (units == 1 && xd) out_h = xd;
                if (units == 1 && yd) out_v = yd;
                return;
            }
            p += 2 + len;
        }
    }

    // Fallback: GDI+ EXIF properties (PNG etc.).
    Bitmap bmp(path.c_str());
    if (bmp.GetLastStatus() != Ok) return;
    UINT sz = bmp.GetPropertyItemSize(0x011A);
    if (sz > 0) {
        std::vector<BYTE> buf(sz);
        if (bmp.GetPropertyItem(0x011A, sz, (PropertyItem*)buf.data()) == Ok) {
            PropertyItem* item = (PropertyItem*)buf.data();
            if (item->type == PropertyTagTypeRational && item->length >= 8) {
                LONG num = ((LONG*)item->value)[0];
                LONG den = ((LONG*)item->value)[1];
                if (den) out_h = (double)num / (double)den;
            }
        }
    }
    sz = bmp.GetPropertyItemSize(0x011B);
    if (sz > 0) {
        std::vector<BYTE> buf(sz);
        if (bmp.GetPropertyItem(0x011B, sz, (PropertyItem*)buf.data()) == Ok) {
            PropertyItem* item = (PropertyItem*)buf.data();
            if (item->type == PropertyTagTypeRational && item->length >= 8) {
                LONG num = ((LONG*)item->value)[0];
                LONG den = ((LONG*)item->value)[1];
                if (den) out_v = (double)num / (double)den;
            }
        }
    }
}

}  // namespace

GdiScope::GdiScope() {
    GdiplusStartupInput in;
    GdiplusStartup(&token_, &in, nullptr);
}

GdiScope::~GdiScope() {
    if (token_) GdiplusShutdown(token_);
}

bool load_image(const std::wstring& path, std::vector<uint8_t>& rgba, int& w,
                int& h, double* res_h, double* res_v) {
    Bitmap bmp(path.c_str());
    if (bmp.GetLastStatus() != Ok) return false;
    w = (int)bmp.GetWidth();
    h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return false;
    // Read the embedded horizontal/vertical resolution (DPI): JFIF density
    // for JPEG, EXIF XResolution/YResolution otherwise, defaulting to 96.
    double rh = 96.0, rv = 96.0;
    read_image_dpi(path, rh, rv);
    if (res_h) *res_h = rh;
    if (res_v) *res_v = rv;
    rgba.assign((size_t)w * h * 4, 0);
    BitmapData data;
    Rect rc(0, 0, w, h);
    if (bmp.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok)
        return false;
    const uint8_t* p = (const uint8_t*)data.Scan0;
    for (int y = 0; y < h; y++) {
        const uint8_t* row = p + (size_t)y * data.Stride;
        for (int x = 0; x < w; x++) {
            const uint8_t* s = row + (size_t)x * 4;
            uint8_t* d = rgba.data() + ((size_t)y * w + x) * 4;
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
        }
    }
    bmp.UnlockBits(&data);
    return true;
}

bool render_text_preview(const std::wstring& font_name, double size_px,
                         int w, int h, const uint8_t color[3], int orientation,
                         int justification, double line_advance,
                         const std::vector<std::wstring>& lines,
                         std::vector<uint8_t>& rgba) {
    if (w <= 0 || h <= 0) return false;
    Bitmap bmp(w, h, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.Clear(Color(0, 0, 0, 0));

    // Font not installed: fall back so the raster preview is not blank.
    std::unique_ptr<FontFamily> ffp = std::make_unique<FontFamily>(font_name.c_str());
    if (ffp->GetLastStatus() != Ok) {
        static const wchar_t* kFallbacks[] = {
            L"Microsoft YaHei", L"SimSun", L"Arial", L"Microsoft Sans Serif",
        };
        for (const wchar_t* fam : kFallbacks) {
            auto fb = std::make_unique<FontFamily>(fam);
            if (fb->GetLastStatus() == Ok) {
                ffp = std::move(fb);
                break;
            }
        }
    }
    Font font(ffp.get(), (REAL)size_px, FontStyleRegular, UnitPixel);

    // Draws one line per row (horizontal) or one column per line (vertical,
    // columns stack right-to-left like Photoshop's vertical text).
    auto draw_block = [&](REAL dx, REAL dy, const SolidBrush& brush) {
        if (orientation == 1) {
            REAL col_w = (REAL)(size_px * 1.35);
            StringFormat fmt;
            fmt.SetFormatFlags(StringFormatFlagsDirectionVertical);
            fmt.SetAlignment(StringAlignmentCenter);
            fmt.SetLineAlignment(StringAlignmentNear);
            for (size_t i = 0; i < lines.size(); i++) {
                if (lines[i].empty()) continue;
                REAL x0 = (REAL)w - (REAL)(i + 1) * col_w + dx;
                if (x0 + col_w <= 0.0f || x0 >= (REAL)w) continue;
                g.DrawString(lines[i].c_str(), -1, &font, RectF(x0, dy, col_w, (REAL)h),
                             &fmt, &brush);
            }
        } else {
            for (size_t i = 0; i < lines.size(); i++) {
                if (lines[i].empty()) continue;
                RectF ms;
                g.MeasureString(lines[i].c_str(), -1, &font, PointF(0.0f, 0.0f), &ms);
                REAL x0 = 0.0f;
                if (justification == 1) x0 = (REAL)w - ms.Width;
                else if (justification == 2) x0 = ((REAL)w - ms.Width) / 2.0f;
                REAL y0 = (REAL)(i * line_advance) + dy;
                g.DrawString(lines[i].c_str(), -1, &font, PointF(x0 + dx, y0), &brush);
            }
        }
    };

    SolidBrush br(Color(255, color[0], color[1], color[2]));
    draw_block(0.0f, 0.0f, br);

    BitmapData data;
    Rect rc(0, 0, w, h);
    if (bmp.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok)
        return false;
    rgba.assign((size_t)w * h * 4, 0);
    const uint8_t* p = (const uint8_t*)data.Scan0;
    for (int y = 0; y < h; y++) {
        const uint8_t* row = p + (size_t)y * data.Stride;
        for (int x = 0; x < w; x++) {
            const uint8_t* s = row + (size_t)x * 4;
            uint8_t* d = rgba.data() + ((size_t)y * w + x) * 4;
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
        }
    }
    bmp.UnlockBits(&data);
    return true;
}

bool get_glyph_indices(const std::wstring& font_name, const std::wstring& text,
                       std::vector<uint16_t>& glyphs) {
    if (text.empty()) return false;
    HDC dc = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
    if (!dc) return false;
    // The height is irrelevant for cmap lookup; -64 is a convenient size.
    HFONT font = CreateFontW(-64, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH,
                             font_name.c_str());
    if (!font) {
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ old = SelectObject(dc, font);
    glyphs.assign(text.size(), 0);
    DWORD r = GetGlyphIndicesW(dc, text.c_str(), (int)text.size(),
                               glyphs.data(), 0);
    SelectObject(dc, old);
    DeleteObject(font);
    DeleteDC(dc);
    return r != GDI_ERROR;
}

namespace {

std::wstring normalize_face(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L' ' || c == L'-' || c == L'_' || c == L'\t')
            continue;
        if (c >= L'A' && c <= L'Z') c += 32;
        out += c;
    }
    return out;
}

bool face_matches(const std::wstring& requested, const std::wstring& actual) {
    std::wstring a = normalize_face(requested);
    std::wstring b = normalize_face(actual);
    if (a == b) return true;
    // Some CJK faces report a localized name; accept a requested ASCII name
    // as a prefix of the reported face (e.g. "YW HeiTI Medium" vs the
    // Chinese family name).
    if (a.size() >= 4 && b.find(a) != std::wstring::npos) return true;
    if (b.size() >= 4 && a.find(b) != std::wstring::npos) return true;
    return false;
}

uint16_t be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

// Scans one font file (TTF/OTF/TTC) for a name-table record whose PostScript
// name (nameID 6) matches `target_ps`; on success returns the family name
// (nameID 1, preferring English) via `family`.
bool scan_font_file(const std::wstring& path, const std::wstring& target_ps,
                    std::wstring& family) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    uint8_t head[16] = {0};
    f.read(reinterpret_cast<char*>(head), 16);
    if (f.gcount() < 16) return false;
    std::vector<uint32_t> faces;
    if (std::memcmp(head, "ttcf", 4) == 0) {
        uint32_t n = be32(head + 8);
        if (n > 64) n = 64;
        std::vector<uint8_t> buf(4 * n);
        f.read(reinterpret_cast<char*>(buf.data()), buf.size());
        for (uint32_t i = 0; i < n; i++)
            faces.push_back(be32(buf.data() + 4 * i));
    } else {
        faces.push_back(0);
    }
    for (uint32_t base : faces) {
        std::vector<uint8_t> hdr(12);
        f.clear();
        f.seekg(base);
        f.read(reinterpret_cast<char*>(hdr.data()), 12);
        if (f.gcount() < 12) continue;
        uint16_t nt = be16(hdr.data() + 4);
        if (nt > 512) nt = 512;
        std::vector<uint8_t> tabs(16 * nt);
        f.read(reinterpret_cast<char*>(tabs.data()), tabs.size());
        uint32_t name_off = 0, name_len = 0;
        for (uint16_t i = 0; i < nt; i++) {
            const uint8_t* rec = tabs.data() + 16 * i;
            if (std::memcmp(rec, "name", 4) == 0) {
                name_off = be32(rec + 8);
                name_len = be32(rec + 12);
            }
        }
        if (!name_off || name_len == 0 || name_len > (1u << 20)) continue;
        std::vector<uint8_t> ntbl(name_len);
        f.clear();
        f.seekg(base + name_off);
        f.read(reinterpret_cast<char*>(ntbl.data()), name_len);
        if (f.gcount() < (std::streamsize)name_len) continue;
        uint16_t count = be16(ntbl.data() + 2);
        uint16_t stroff = be16(ntbl.data() + 4);
        if (count > 1024 || stroff >= name_len) continue;
        std::wstring ps, fam, fam_en;
        for (uint16_t i = 0; i < count; i++) {
            const uint8_t* rec = ntbl.data() + 6 + 12 * i;
            uint16_t pid = be16(rec), eid = be16(rec + 2), lid = be16(rec + 4);
            uint16_t nid = be16(rec + 6), ln = be16(rec + 8), off = be16(rec + 10);
            if ((uint32_t)stroff + off + ln > name_len) continue;
            std::wstring s;
            if (pid == 3 && eid == 1) {
                const uint8_t* p = ntbl.data() + stroff + off;
                s.assign(ln / 2, L'\0');
                for (uint16_t k = 0; k < ln / 2; k++)
                    s[k] = (wchar_t)((p[2 * k] << 8) | p[2 * k + 1]);
            } else if (pid == 1) {
                const uint8_t* p = ntbl.data() + stroff + off;
                s.assign(p, p + ln);
            } else {
                continue;
            }
            if (nid == 6 && ps.empty()) {
                ps = s;
            } else if (nid == 1) {
                if (pid == 3 && eid == 1 && lid == 1033 && fam_en.empty())
                    fam_en = s;
                else if (fam.empty()) fam = s;
            }
        }
        if (!ps.empty() && normalize_face(ps) == target_ps) {
            family = !fam_en.empty() ? fam_en : fam;
            return !family.empty();
        }
    }
    return false;
}

}  // namespace

bool resolve_font_family(const std::wstring& ps_name, std::wstring& family) {
    if (ps_name.empty()) return false;
    std::wstring target = normalize_face(ps_name);
    const wchar_t* dirs[] = {
        L"C:\\Windows\\Fonts",
        L"",
    };
    std::wstring user = L"";
    const wchar_t* env = _wgetenv(L"LOCALAPPDATA");
    if (env && *env) user = std::wstring(env) + L"\\Microsoft\\Windows\\Fonts";
    dirs[1] = user.c_str();
    for (const wchar_t* dir : dirs) {
        if (!dir || !*dir) continue;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((std::wstring(dir) + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring fn = fd.cFileName;
            std::wstring ext;
            size_t dot = fn.find_last_of(L'.');
            if (dot != std::wstring::npos) ext = fn.substr(dot);
            if (ext != L".ttf" && ext != L".ttc" && ext != L".otf") continue;
            std::wstring path = std::wstring(dir) + L"\\" + fn;
            if (scan_font_file(path, target, family)) {
                FindClose(h);
                return true;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return false;
}

bool font_family_available(const std::wstring& family) {
    if (family.empty()) return false;
    HDC dc = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
    if (!dc) return false;
    HFONT font = CreateFontW(-64, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH,
                             family.c_str());
    if (!font) {
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ old = SelectObject(dc, font);
    wchar_t face[LF_FACESIZE] = {0};
    GetTextFaceW(dc, LF_FACESIZE, face);
    SelectObject(dc, old);
    DeleteObject(font);
    DeleteDC(dc);
    return face_matches(family, face);
}

namespace {

bool make_metrics_dc(const std::wstring& font_name, int size, HDC& dc,
                     HFONT& font, HGDIOBJ& old) {
    dc = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
    if (!dc) return false;
    font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH, font_name.c_str());
    if (!font) {
        DeleteDC(dc);
        return false;
    }
    old = SelectObject(dc, font);
    return true;
}

void release_metrics_dc(HDC dc, HFONT font, HGDIOBJ old) {
    SelectObject(dc, old);
    DeleteObject(font);
    DeleteDC(dc);
}

}  // namespace

bool get_font_layout_metrics(const std::wstring& font_name, double& ascent_em,
                             double& descent_em, double& space_advance_em) {
    // Photoshop's line box for Microsoft Ya Hei, measured from PS-saved
    // reference files (fresh.psd / ps_ref_htest.psd). Other fonts fall back
    // to GDI text metrics; the exact values only affect the layout numbers
    // written into Txt2, not the text content or glyphs.
    if (_wcsicmp(font_name.c_str(), L"Microsoft YaHei") == 0 ||
        _wcsicmp(font_name.c_str(), L"MicrosoftYaHei") == 0) {
        ascent_em = 0.85742;
        descent_em = 0.25537;
    } else {
        ascent_em = 0.8;
        descent_em = 0.2;
    }
    // Photoshop's vertical space column uses the font's space advance
    // (hmtx): measured as 12.3291 px at 41.66667 pt (= 0.2959 em) in the
    // ps_ref_space_yh.psd reference. Refine it with GDI below.
    space_advance_em = 0.296;
    HDC dc;
    HFONT font;
    HGDIOBJ old;
    if (make_metrics_dc(font_name, 1000, dc, font, old)) {
        SIZE sz;
        if (GetTextExtentPoint32W(dc, L" ", 1, &sz) && sz.cx > 0)
            space_advance_em = (double)sz.cx / 1000.0;
        release_metrics_dc(dc, font, old);
    }
    return true;
}

bool get_text_advance_em(const std::wstring& font_name,
                         const std::wstring& text, double& advance_em) {
    if (text.empty()) return false;
    HDC dc;
    HFONT font;
    HGDIOBJ old;
    if (!make_metrics_dc(font_name, 1000, dc, font, old)) return false;
    SIZE sz;
    bool ok = GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz) != 0;
    if (ok && sz.cx > 0) advance_em = (double)sz.cx / 1000.0;
    release_metrics_dc(dc, font, old);
    return ok;
}
