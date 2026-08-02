// psdgen - generate PSD files from a layout text file.
//
// Layout file format (see examples/text1.txt):
//   <GroupName> --- (psd group name for group N)      (one line per group)
//   >>>>>>>>>>[image.jpg]<<<<<<<<<<
//   ----------------[1]----------------[x,y,group]    (x,y in 0..1 of image)
//   text line 1
//   text line 2

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "minijson.hpp"
#include "psd_writer.hpp"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

// ===========================================================================
// Encoding helpers
// ===========================================================================
static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr,
                        nullptr);
    return out;
}

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

static std::string read_text_file(const std::wstring& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open text file";
        return {};
    }
    std::string bytes((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    // BOM detection
    if (bytes.size() >= 3 && (uint8_t)bytes[0] == 0xEF && (uint8_t)bytes[1] == 0xBB &&
        (uint8_t)bytes[2] == 0xBF)
        return bytes.substr(3);                       // UTF-8
    if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFF && (uint8_t)bytes[1] == 0xFE) {
        std::wstring w((bytes.size() - 2) / 2, L'\0');
        for (size_t i = 0; i < w.size(); i++) {
            w[i] = (wchar_t)((uint8_t)bytes[2 + i * 2] | ((uint8_t)bytes[3 + i * 2] << 8));
        }
        return wide_to_utf8(w);                       // UTF-16LE
    }
    if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFE && (uint8_t)bytes[1] == 0xFF) {
        std::wstring w((bytes.size() - 2) / 2, L'\0');
        for (size_t i = 0; i < w.size(); i++) {
            w[i] = (wchar_t)(((uint8_t)bytes[2 + i * 2] << 8) | (uint8_t)bytes[3 + i * 2]);
        }
        return wide_to_utf8(w);                       // UTF-16BE
    }

    // Validate UTF-8; fall back to system ANSI (e.g. GBK on Chinese Windows).
    bool valid = true;
    for (size_t i = 0; i < bytes.size(); i++) {
        uint8_t c = (uint8_t)bytes[i];
        size_t extra = 0;
        if (c >= 0x80) {
            if ((c & 0xE0) == 0xC0) extra = 1;
            else if ((c & 0xF0) == 0xE0) extra = 2;
            else if ((c & 0xF8) == 0xF0) extra = 3;
            else { valid = false; break; }
            for (size_t k = 1; k <= extra; k++) {
                if (i + k >= bytes.size() || ((uint8_t)bytes[i + k] & 0xC0) != 0x80) {
                    valid = false; break;
                }
            }
            if (!valid) break;
            i += extra;
        }
    }
    if (valid) return bytes;

    int n = MultiByteToWideChar(CP_ACP, 0, bytes.c_str(), (int)bytes.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.c_str(), (int)bytes.size(), &w[0], n);
    return wide_to_utf8(w);
}

// ===========================================================================
// Layout text parser
// ===========================================================================
struct TextEntry {
    int index = 0;
    double x = 0.0, y = 0.0;
    int group = 0;
    std::vector<std::string> lines;
};

struct ImageBlock {
    std::string image;
    std::vector<TextEntry> entries;
};

struct Layout {
    std::vector<std::pair<int, std::string>> groups;  // number -> name
    std::vector<ImageBlock> images;
};

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static bool parse_entry_line(const std::string& line, int& idx, double& x,
                             double& y, int& group) {
    size_t b1 = line.find('[');
    if (b1 == std::string::npos) return false;
    size_t e1 = line.find(']', b1);
    if (e1 == std::string::npos) return false;
    std::string index_str = line.substr(b1 + 1, e1 - b1 - 1);
    if (index_str.empty() ||
        !std::all_of(index_str.begin(), index_str.end(),
                     [](char c) { return c >= '0' && c <= '9'; }))
        return false;

    size_t b2 = line.find('[', e1);
    if (b2 == std::string::npos) return false;
    size_t e2 = line.find(']', b2);
    if (e2 == std::string::npos) return false;
    std::string fields = line.substr(b2 + 1, e2 - b2 - 1);
    std::vector<double> nums;
    std::string cur;
    for (char c : fields) {
        if (c == ',') {
            if (!cur.empty()) { nums.push_back(std::atof(cur.c_str())); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) nums.push_back(std::atof(cur.c_str()));
    if (nums.size() < 3) return false;

    idx = std::atoi(index_str.c_str());
    x = nums[0];
    y = nums[1];
    group = (int)nums[2];
    return true;
}

static bool parse_layout(const std::wstring& path, Layout& out, std::string* err) {
    std::string text = read_text_file(path, err);
    if (text.empty() && err && !err->empty()) return false;

    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);

    ImageBlock* block = nullptr;
    TextEntry* entry = nullptr;
    int next_group_num = 1;

    for (const std::string& raw : lines) {
        std::string line = trim(raw);
        if (line.empty()) continue;

        if (line.rfind(">>>>>>>>[", 0) == 0 || line.rfind(">>>>>>>>>[", 0) == 0) {
            size_t b = line.find('[');
            size_t e = line.find(']', b);
            if (e == std::string::npos) continue;
            ImageBlock nb;
            nb.image = line.substr(b + 1, e - b - 1);
            out.images.push_back(std::move(nb));
            block = &out.images.back();
            entry = nullptr;
            continue;
        }

        if (block == nullptr) {
            // Header section: group name mapping lines. Real layout files list
            // group names as plain lines before the first image block:
            //     1,0
            //     -
            //     框内
            //     框外
            //     -
            //     Default Comment
            //     You can edit me
            // The example text1.txt annotates them as "框内 --- (psd 分组名称
            // 对应 1)" instead; both forms are accepted below. Lines starting
            // with "-" and the known comment lines are skipped.
            bool handled = false;
            size_t dash = line.find("---");
            if (dash != std::string::npos && dash > 0) {
                std::string name = trim(line.substr(0, dash));
                if (name.empty()) continue;
                handled = true;
                int num = next_group_num++;
                // "对应 N" comment. The needle is UTF-8 bytes spelled out
                // with hex escapes so the build is encoding-independent.
                const std::string duiying = "\xE5\xAF\xB9\xE5\xBA\x94";  // 对应
                size_t p = line.find(duiying);
                if (p != std::string::npos) {
                    std::string tail = line.substr(p);
                    for (size_t i = 0; i < tail.size(); i++) {
                        if (tail[i] >= '0' && tail[i] <= '9') {
                            num = 0;
                            while (i < tail.size() && tail[i] >= '0' && tail[i] <= '9') {
                                num = num * 10 + (tail[i] - '0');
                                i++;
                            }
                            break;
                        }
                    }
                }
                bool found = false;
                for (auto& g : out.groups)
                    if (g.first == num) { g.second = name; found = true; }
                if (!found) out.groups.emplace_back(num, name);
            } else if (!handled && line != "-" && !line.empty() && line[0] != '-' &&
                       line[0] != '=' && line[0] != '#') {
                // Plain group-name line (real-world format). Anything that is
                // not a separator/comment/header line is treated as a group
                // name; the count is capped by the actual entries' group ids.
                // "1,0" style header lines (document id, page id) are skipped.
                bool coord_header = false;
                {
                    size_t comma = line.find(',');
                    if (comma != std::string::npos) {
                        auto is_digits = [](const std::string& s) {
                            return !s.empty() &&
                                   std::all_of(s.begin(), s.end(),
                                               [](char c) { return c >= '0' && c <= '9'; });
                        };
                        coord_header = is_digits(line.substr(0, comma)) &&
                                       is_digits(line.substr(comma + 1));
                    }
                }
                const std::string skip_prefixes[] = {
                    "default comment", "you can edit me",
                };
                bool skip = coord_header;
                std::string lower = lower_ascii(line);
                for (const auto& p : skip_prefixes)
                    if (lower.rfind(p, 0) == 0) { skip = true; break; }
                if (!skip) {
                    bool found = false;
                    for (auto& g : out.groups)
                        if (g.first == next_group_num) {
                            if (g.second.empty()) g.second = line;
                            found = true;
                        }
                    if (!found) out.groups.emplace_back(next_group_num, line);
                    next_group_num++;
                }
            }
            continue;
        }

        // Entry header line: ----------------[N]----------------[x,y,g]
        if (line.size() > 2 && line[0] == '-') {
            int idx; double x, y; int g;
            if (parse_entry_line(line, idx, x, y, g)) {
                TextEntry ne;
                ne.index = idx;
                ne.x = x;
                ne.y = y;
                ne.group = g;
                block->entries.push_back(std::move(ne));
                entry = &block->entries.back();
                continue;
            }
        }

        // Plain text line -> current entry
        if (entry) entry->lines.push_back(line);
    }

    if (out.images.empty()) {
        if (err) *err = "no image blocks found in text file";
        return false;
    }
    return true;
}

// ===========================================================================
// Style configuration
// ===========================================================================
struct Style {
    std::wstring font_name = L"Microsoft YaHei";
    std::string font_ps = "MicrosoftYaHei";
    double font_size_pt = 24.0;  // font size in points, as in Photoshop
    uint8_t color[3] = {255, 255, 255};
    // Text style settings (written into TySh / EngineData)
    int anti_alias = 2;            // 0=None 1=Crisp 2=Strong 3=Smooth 4=Sharp 6=LCD
    int orientation = 0;           // 0=horizontal 1=vertical
    int justification = 0;         // 0=Left 1=Right 2=Center ... 6=JustifyAll
    bool auto_leading = true;      // auto leading on/off
    double auto_leading_size = 1.2; // auto leading multiplier
    double leading = 0.0;          // explicit leading in px when auto leading is off
    bool discretionary_ligatures = false;
    bool standard_vertical_roman = true;
    std::string output_dir;
    std::string prefix;
    std::string suffix;
};

static void read_color(const mjson::Value& v, uint8_t out[3], const uint8_t dflt[3]) {
    out[0] = dflt[0]; out[1] = dflt[1]; out[2] = dflt[2];
    if (v.t != mjson::Value::T::Arr || v.arr.size() < 3) return;
    for (int i = 0; i < 3; i++)
        out[i] = (uint8_t)std::max(0.0, std::min(255.0, v.arr[i].num_or(dflt[i])));
}

// Accepts a number or a name; falls back to `dflt` when unknown.
static int parse_anti_alias(const mjson::Value& v, int dflt) {
    if (v.t == mjson::Value::T::Num) return (int)v.num_or(dflt);
    if (v.t != mjson::Value::T::Str) return dflt;
    std::string s = lower_ascii(v.str_or(""));
    if (s == "none" || s == "无") return 0;
    if (s == "crisp" || s == "犀利") return 1;
    if (s == "strong" || s == "浑厚") return 2;
    if (s == "smooth" || s == "平滑") return 3;
    if (s == "sharp" || s == "锐利") return 4;
    if (s == "lcd") return 6;
    return dflt;
}

static int parse_orientation(const mjson::Value& v, int dflt) {
    if (v.t == mjson::Value::T::Num) return v.num_or(dflt) != 0.0 ? 1 : 0;
    if (v.t != mjson::Value::T::Str) return dflt;
    std::string s = lower_ascii(v.str_or(""));
    if (s == "vertical" || s == "竖排" || s == "v") return 1;
    if (s == "horizontal" || s == "横排" || s == "h") return 0;
    return dflt;
}

static int parse_justification(const mjson::Value& v, int dflt) {
    if (v.t == mjson::Value::T::Num) return (int)v.num_or(dflt);
    if (v.t != mjson::Value::T::Str) return dflt;
    std::string s = lower_ascii(v.str_or(""));
    if (s == "left" || s == "左") return 0;
    if (s == "right" || s == "右") return 1;
    if (s == "center" || s == "居中" || s == "centre") return 2;
    if (s == "justifylastleft" || s == "两端对齐末行左对齐") return 3;
    if (s == "justifylastright" || s == "两端对齐末行右对齐") return 4;
    if (s == "justifylastcenter" || s == "两端对齐末行居中") return 5;
    if (s == "justifyall" || s == "全部两端对齐") return 6;
    return dflt;
}

static Style load_style(const mjson::Value& cfg) {
    Style s;
    if (cfg.is_null()) return s;
    if (const mjson::Value* f = cfg.get("font")) {
        if (const mjson::Value* n = f->get("name"))
            if (n->t == mjson::Value::T::Str) {
                s.font_name = utf8_to_wide(n->str);
                s.font_ps = n->str;
                s.font_ps.erase(std::remove_if(s.font_ps.begin(), s.font_ps.end(),
                                               [](char c) { return c == ' ' || c == '\t'; }),
                                s.font_ps.end());
            }
        s.font_size_pt =
            f->get("fontSize") ? f->get("fontSize")->num_or(s.font_size_pt) : s.font_size_pt;
        if (const mjson::Value* c = f->get("color")) read_color(*c, s.color, s.color);
        if (const mjson::Value* a = f->get("antiAlias"))
            s.anti_alias = parse_anti_alias(*a, s.anti_alias);
        if (const mjson::Value* o = f->get("orientation"))
            s.orientation = parse_orientation(*o, s.orientation);
        if (const mjson::Value* j = f->get("justification"))
            s.justification = parse_justification(*j, s.justification);
        if (const mjson::Value* al = f->get("autoLeading"))
            s.auto_leading = al->bool_or(s.auto_leading);
        if (const mjson::Value* als = f->get("autoLeadingSize"))
            s.auto_leading_size = als->num_or(s.auto_leading_size);
        if (const mjson::Value* ld = f->get("leading"))
            s.leading = ld->num_or(s.leading);
        if (const mjson::Value* dl = f->get("discretionaryLigatures"))
            s.discretionary_ligatures = dl->bool_or(s.discretionary_ligatures);
        if (const mjson::Value* sv = f->get("standardVerticalRomanAlignment"))
            s.standard_vertical_roman = sv->bool_or(s.standard_vertical_roman);
    }
    s.output_dir = cfg.get("outputDir") ? cfg.get("outputDir")->str_or("") : "";
    s.prefix = cfg.get("prefix") ? cfg.get("prefix")->str_or("") : "";
    s.suffix = cfg.get("suffix") ? cfg.get("suffix")->str_or("") : "";
    return s;
}

// ===========================================================================
// GDI+ image loading and text preview rendering
// ===========================================================================
class GdiScope {
public:
    GdiScope() {
        GdiplusStartupInput in;
        GdiplusStartup(&token_, &in, nullptr);
    }
    ~GdiScope() {
        if (token_) GdiplusShutdown(token_);
    }
private:
    ULONG_PTR token_ = 0;
};

// Reads JPEG JFIF/EXIF DPI from the raw file bytes (GDI+ does not expose the
// JFIF density through PropertyItem). Falls back to GDI+ properties for other
// formats (PNG etc.), and to 96 when nothing is embedded.
static void read_image_dpi(const std::wstring& path, double& out_h, double& out_v) {
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

static bool load_image(const std::wstring& path, std::vector<uint8_t>& rgba, int& w,
                       int& h, double* res_h = nullptr, double* res_v = nullptr) {
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

static bool render_text_preview(const std::wstring& font_name, double size_px,
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

    FontFamily ff(font_name.c_str());
    Font font(&ff, (REAL)size_px, FontStyleRegular, UnitPixel);

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

// Rough text width estimation (CJK glyphs = 1.0 em, Latin = 0.55 em).
static double line_units(const std::string& line) {
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
// Document building
// ===========================================================================
static std::string dir_of(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? "" : path.substr(0, p + 1);
}

static std::string stem_of(const std::string& name) {
    size_t p = name.find_last_of('.');
    return p == std::string::npos ? name : name.substr(0, p);
}

static std::string group_name_of(const Layout& layout, int g) {
    for (const auto& kv : layout.groups)
        if (kv.first == g) return kv.second;
    return "Group " + std::to_string(g);
}

// A txt line may contain literal "\r" / "\n" escape sequences to force extra
// line breaks inside one physical line. Returns the resulting logical lines.
static std::vector<std::string> split_escaped_lines(const std::string& line) {
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

static std::shared_ptr<psdw::TextLayer> make_text_layer(const TextEntry& e, int iw,
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
    double leading_px = style.auto_leading ? font_size * style.auto_leading_size
                                           : (style.leading > 0.0 ? style.leading
                                                                  : font_size * style.auto_leading_size);
    double box_w, box_h;
    if (style.orientation == 1) {
        // vertical: each line is a column, columns advance right-to-left
        box_w = std::max((double)lines.size() * font_size * 1.35, font_size * 1.35);
        box_h = std::max(max_units * font_size, font_size);
    } else {
        box_w = std::max(max_units * font_size, font_size);
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
        if (render_text_preview(style.font_name, font_size, pw, ph, style.color,
                                style.orientation, style.justification, leading_px,
                                wlines, prev))
            tl->text.preview = std::move(prev);
    }

    return tl;
}

static bool build_psd(const ImageBlock& blk, const std::wstring& txt_dir,
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

static void print_usage() {
    printf("psdgen - generate PSD files from a layout text file\n\n");
    printf("usage: psdgen <layout.txt> [--out <dir>] [--config <style.json>]\n\n");
    printf("With no arguments a file dialog asks for the layout text file.\n");
    printf("The layout file references images in the same folder (one block per\n");
    printf("image) and places text layers at normalized positions with optional\n");
    printf("group numbers. Each image produces <image>.psd.\n");
}

// Full directory of the running executable (trailing backslash).
static std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    if (n == 0 || n >= std::size(buf)) return L"";
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s + 1);
}

// Modal "open file" dialog for the layout text file. Returns false on cancel.
static bool pick_text_file(std::wstring& out_path) {
    wchar_t file[MAX_PATH * 4] = L"";
    wchar_t dir[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)std::size(file);
    ofn.lpstrInitialDir = dir;
    ofn.lpstrTitle = L"选择排版文本文件";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return false;
    out_path = file;
    return true;
}

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
        if (!pick_text_file(picked)) {
            printf("no file selected.\n");
            return 2;
        }
        txt_path = wide_to_utf8(picked);
    }

    // Font config: default to config.json next to the exe.
    if (cfg_path.empty()) {
        std::wstring def = exe_dir() + L"config.json";
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
        std::ifstream f(wcfg, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        mjson::Value v;
        if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB &&
            (uint8_t)text[2] == 0xBF)
            text = text.substr(3);  // strip UTF-8 BOM
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
