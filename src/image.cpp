#include "image.hpp"

#include <gdiplus.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

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
