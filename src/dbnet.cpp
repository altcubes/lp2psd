// dbnet.cpp - Text-region detection for whitening, using the m-i-t DBNet
// detector (ResNet34 + DB head) exported to ONNX (scripts/export_dbnet_onnx.py).
// This is the same detector yakuyomi-engine runs through NCNN; here it runs on
// ONNX Runtime, dynamically loaded at runtime (exe starts fine without the
// DLL).
//
// Preprocessing follows yakuyomi ImageOps.detectorChwDbnet: resize so the long
// side is <= limit_side_len while keeping aspect, pad right/bottom to a
// multiple of 256 (black), scale to [-1,1] (x/127.5 - 1), RGB channel order.
//
// Model outputs (see export script docstring):
//   out0 db [1,2,H,W]  full-res: ch0 = shrink-map RAW LOGITS, ch1 = threshold
//   out1 mask [1,1,h,w] per-pixel stroke mask, already sigmoid (half-res for
//                       this export; actual size read from the tensor).
//
// Postprocessing (ported from yakuyomi Detector.kt @ Geometry.kt):
//   sigmoid(ch0) -> binarize -> 8-connected components -> boundary points ->
//   minAreaRect (rotating calipers on convex hull) -> DB unclip -> rotated
//   quadrilateral in original coords; score = component-mean prob.
//   mask: crop valid region -> bilinear resize to original -> threshold ->
//   per-pixel stroke mask.

#ifdef LP2PSD_WITH_dbnet

#include "dbnet.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <onnxruntime_c_api.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>

#include "textcodec.hpp"

namespace {

const OrtApi* g_ort = nullptr;
bool g_ort_tried = false;
HMODULE g_ort_dll = nullptr;
OrtEnv* g_env = nullptr;
OrtSession* g_session = nullptr;
std::string g_model;

// Loads onnxruntime.dll (exe directory or PATH) and resolves the C API.
bool load_runtime() {
    if (g_ort_tried) return g_ort != nullptr;
    g_ort_tried = true;
    g_ort_dll = LoadLibraryW(L"onnxruntime.dll");
    if (!g_ort_dll) return false;
    auto base =
        (const OrtApiBase * (*)(void))GetProcAddress(g_ort_dll, "OrtGetApiBase");
    if (!base) {
        FreeLibrary(g_ort_dll);
        g_ort_dll = nullptr;
        return false;
    }
    g_ort = base()->GetApi(ORT_API_VERSION);
    if (!g_ort) {
        // Keep the DLL loaded once the API resolves: the session holds
        // runtime pointers into it for the process lifetime.
        FreeLibrary(g_ort_dll);
        g_ort_dll = nullptr;
    }
    return g_ort != nullptr;
}

bool ensure_session(const std::string& model_path, std::string* err) {
    auto check = [&](OrtStatus* st, const char* what) -> bool {
        if (!st) return true;
        if (err) *err = std::string("onnxruntime: ") + what +
                        g_ort->GetErrorMessage(st);
        g_ort->ReleaseStatus(st);
        return false;
    };
    if (!g_env && !check(
                      g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "lp2psd",
                                       &g_env),
                      ""))
        return false;

    if (g_session && g_model != model_path) {
        g_ort->ReleaseSession(g_session);
        g_session = nullptr;
        g_model.clear();
    }
    if (g_session) return true;

    std::wstring wpath = textcodec::utf8_to_wide(model_path);
    if (GetFileAttributesW(wpath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = "model not found: " + model_path;
        return false;
    }

    OrtSessionOptions* so = nullptr;
    if (!check(g_ort->CreateSessionOptions(&so), "")) return false;
    unsigned hw = std::thread::hardware_concurrency();
    if (!check(g_ort->SetIntraOpNumThreads(so, hw ? (int)hw : 1), "")) {
        g_ort->ReleaseSessionOptions(so);
        return false;
    }
    if (!check(g_ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL),
               "")) {
        g_ort->ReleaseSessionOptions(so);
        return false;
    }
    OrtStatus* st = g_ort->CreateSession(g_env, wpath.c_str(), so, &g_session);
    g_ort->ReleaseSessionOptions(so);
    if (st) {
        if (err) *err = std::string("create session: ") + g_ort->GetErrorMessage(st);
        g_ort->ReleaseStatus(st);
        g_session = nullptr;
        return false;
    }
    g_model = model_path;
    return true;
}

// Bilinear RGBA resize (same output layout as cv2.resize INTER_LINEAR).
void resize_rgba(const std::vector<uint8_t>& src, int sw, int sh,
                 std::vector<uint8_t>& dst, int dw, int dh) {
    dst.assign((size_t)dw * dh * 4, 0);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (sw == dw && sh == dh) { dst = src; return; }
    const double sx = (double)sw / dw, sy = (double)sh / dh;
    for (int y = 0; y < dh; y++) {
        double fy = (y + 0.5) * sy - 0.5;
        int y0 = (int)std::floor(fy);
        double wy = fy - y0;
        y0 = std::max(0, std::min(y0, sh - 1));
        int y1 = std::min(y0 + 1, sh - 1);
        for (int x = 0; x < dw; x++) {
            double fx = (x + 0.5) * sx - 0.5;
            int x0 = (int)std::floor(fx);
            double wx = fx - x0;
            x0 = std::max(0, std::min(x0, sw - 1));
            int x1 = std::min(x0 + 1, sw - 1);
            const uint8_t* p00 = &src[((size_t)y0 * sw + x0) * 4];
            const uint8_t* p01 = &src[((size_t)y0 * sw + x1) * 4];
            const uint8_t* p10 = &src[((size_t)y1 * sw + x0) * 4];
            const uint8_t* p11 = &src[((size_t)y1 * sw + x1) * 4];
            uint8_t* d = &dst[((size_t)y * dw + x) * 4];
            for (int c = 0; c < 4; c++) {
                double v = p00[c] * (1 - wx) * (1 - wy) + p01[c] * wx * (1 - wy) +
                           p10[c] * (1 - wx) * wy + p11[c] * wx * wy;
                d[c] = (uint8_t)(v + 0.5);
            }
        }
    }
}

// Bilinear grayscale resize (float in, float out; matches cv2.resize on the
// mask plane, see segToMask in yakuyomi Detector.kt).
void resize_gray(const std::vector<float>& src, int sw, int sh,
                 std::vector<float>& dst, int dw, int dh) {
    dst.assign((size_t)dw * dh, 0.0f);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (sw == dw && sh == dh) { dst = src; return; }
    const double sx = (double)sw / dw, sy = (double)sh / dh;
    for (int y = 0; y < dh; y++) {
        double fy = (y + 0.5) * sy - 0.5;
        int y0 = (int)std::floor(fy);
        double wy = fy - y0;
        y0 = std::max(0, std::min(y0, sh - 1));
        int y1 = std::min(y0 + 1, sh - 1);
        for (int x = 0; x < dw; x++) {
            double fx = (x + 0.5) * sx - 0.5;
            int x0 = (int)std::floor(fx);
            double wx = fx - x0;
            x0 = std::max(0, std::min(x0, sw - 1));
            int x1 = std::min(x0 + 1, sw - 1);
            double v = src[(size_t)y0 * sw + x0] * (1 - wx) * (1 - wy) +
                       src[(size_t)y0 * sw + x1] * wx * (1 - wy) +
                       src[(size_t)y1 * sw + x0] * (1 - wx) * wy +
                       src[(size_t)y1 * sw + x1] * wx * wy;
            dst[(size_t)y * dw + x] = (float)v;
        }
    }
}

struct Pt { float x, y; };

struct RotRect {
    float cx = 0, cy = 0, ux = 1, uy = 0, w = 0, h = 0;

    // DB unclip: d = area*ratio/perimeter; w/h grow by 2d (db_utils.unclip).
    RotRect unclip(float ratio) const {
        float peri = 2.f * (w + h);
        float d = peri > 1e-6f ? (w * h) * ratio / peri : 0.f;
        RotRect r = *this;
        r.w += 2.f * d;
        r.h += 2.f * d;
        return r;
    }

    // Four corners (same order as yakuyomi RotRect.corners()).
    void corners(Pt out[4]) const {
        float hw = w / 2.f, hh = h / 2.f;
        out[0] = {cx - hw * ux - hh * -uy, cy - hw * uy - hh * ux};
        out[1] = {cx + hw * ux - hh * -uy, cy + hw * uy - hh * ux};
        out[2] = {cx + hw * ux + hh * -uy, cy + hw * uy + hh * ux};
        out[3] = {cx - hw * ux + hh * -uy, cy - hw * uy + hh * ux};
    }
};

// Andrew monotone chain convex hull.
std::vector<Pt> convex_hull(std::vector<Pt> pts) {
    if (pts.size() < 3) return pts;
    std::sort(pts.begin(), pts.end(), [](const Pt& a, const Pt& b) {
        return a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    auto cross = [](const Pt& o, const Pt& a, const Pt& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    std::vector<Pt> lower, upper;
    for (const Pt& p : pts) {
        while (lower.size() >= 2 &&
               cross(lower[lower.size() - 2], lower[lower.size() - 1], p) <= 0.f)
            lower.pop_back();
        lower.push_back(p);
    }
    for (int i = (int)pts.size() - 1; i >= 0; i--) {
        const Pt& p = pts[i];
        while (upper.size() >= 2 &&
               cross(upper[upper.size() - 2], upper[upper.size() - 1], p) <= 0.f)
            upper.pop_back();
        upper.push_back(p);
    }
    lower.pop_back();
    upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());
    return lower;
}

// Rotating calipers over the convex hull; matches cv2.minAreaRect /
// yakuyomi Geometry.minAreaRect. Returns false when degenerate.
bool min_area_rect(const std::vector<Pt>& points, RotRect& out) {
    std::vector<Pt> hull = convex_hull(points);
    if (hull.size() < 2) return false;
    float best_area = std::numeric_limits<float>::max();
    bool found = false;
    for (size_t i = 0; i < hull.size(); i++) {
        const Pt& a = hull[i];
        const Pt& b = hull[(i + 1) % hull.size()];
        float ex = b.x - a.x, ey = b.y - a.y;
        float len = std::hypot(ex, ey);
        if (len < 1e-6f) continue;
        ex /= len;
        ey /= len;
        float min_u = std::numeric_limits<float>::max();
        float max_u = -std::numeric_limits<float>::max();
        float min_v = std::numeric_limits<float>::max();
        float max_v = -std::numeric_limits<float>::max();
        for (const Pt& p : hull) {
            float dx = p.x - a.x, dy = p.y - a.y;
            float u = dx * ex + dy * ey;
            float v = -dx * ey + dy * ex;
            min_u = std::min(min_u, u); max_u = std::max(max_u, u);
            min_v = std::min(min_v, v); max_v = std::max(max_v, v);
        }
        float w = max_u - min_u, h = max_v - min_v;
        float area = w * h;
        if (area < best_area) {
            best_area = area;
            float cu = (min_u + max_u) / 2.f, cv = (min_v + max_v) / 2.f;
            out.cx = a.x + cu * ex - cv * ey;
            out.cy = a.y + cu * ey + cv * ex;
            out.ux = ex; out.uy = ey;
            out.w = w; out.h = h;
            found = true;
        }
    }
    return found;
}

// Connected components over the binarized probability map; each component
// yields boundary points (pixels with an orthogonal below-threshold / OOB
// neighbor), the mean probability, and a box via minAreaRect + unclip.
// Returns text lines as quads in original-image coordinates.
void lines_from_prob_map(const std::vector<float>& prob, int grid_w, int grid_h,
                         float ratio, int orig_w, int orig_h,
                         const dbnetOptions& opt, std::vector<dbnetBox>& out) {
    const size_t n = prob.size();
    std::vector<uint8_t> visited(n, 0);
    std::vector<int> stack;
    stack.reserve(8192);
    std::vector<Pt> boundary;

    for (size_t seed = 0; seed < n; seed++) {
        if (visited[seed] || prob[seed] <= opt.det_thresh) continue;
        stack.clear();
        boundary.clear();
        stack.push_back((int)seed);
        visited[seed] = 1;
        double sum = 0;
        size_t cnt = 0;
        while (!stack.empty()) {
            int idx = stack.back();
            stack.pop_back();
            int x = idx % grid_w, y = idx / grid_w;
            sum += prob[idx];
            cnt++;
            bool is_boundary = false;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < grid_w && ny >= 0 && ny < grid_h) {
                        size_t ni = (size_t)ny * grid_w + nx;
                        if (prob[ni] > opt.det_thresh) {
                            if (!visited[ni]) {
                                visited[ni] = 1;
                                stack.push_back((int)ni);
                            }
                        } else if (dx == 0 || dy == 0) {
                            is_boundary = true;
                        }
                    } else if (dx == 0 || dy == 0) {
                        is_boundary = true;
                    }
                }
            }
            if (is_boundary) boundary.push_back({(float)x, (float)y});
        }
        float score = cnt ? (float)(sum / (double)cnt) : 0.f;
        if (score < opt.box_thresh) continue;

        RotRect rect;
        if (!min_area_rect(boundary, rect)) continue;
        if (std::min(rect.w, rect.h) < opt.min_side) continue;

        RotRect wide = rect.unclip(opt.unclip_ratio);
        Pt c[4];
        wide.corners(c);
        dbnetBox box;
        box.score = score;
        box.x = orig_w; box.y = orig_h;
        for (int k = 0; k < 4; k++) {
            float qx = std::max(0.f, std::min(c[k].x / ratio, (float)orig_w));
            float qy = std::max(0.f, std::min(c[k].y / ratio, (float)orig_h));
            box.quad[k * 2] = qx;
            box.quad[k * 2 + 1] = qy;
            box.x = std::min(box.x, (double)qx);
            box.y = std::min(box.y, (double)qy);
            box.w = std::max(box.w, (double)qx);
            box.h = std::max(box.h, (double)qy);
        }
        box.w -= box.x;
        box.h -= box.y;
        if (box.w * box.h >= opt.min_box_area) out.push_back(box);
    }
}

// mask plane (already sigmoid, half/full res platform-dependent) -> binary
// stroke mask at original size. Crops the valid region, bilinear-upscales,
// thresholds at seg_thresh (segToMask in yakuyomi Detector.kt).
void seg_to_mask(const std::vector<float>& mask, int src_w, int src_h,
                 float ratio, int orig_w, int orig_h,
                 const dbnetOptions& opt, std::vector<uint8_t>& stroke_mask) {
    int nw = std::max(1, std::min((int)std::llround(orig_w * ratio), src_w));
    int nh = std::max(1, std::min((int)std::llround(orig_h * ratio), src_h));
    std::vector<float> valid((size_t)nw * nh);
    for (int y = 0; y < nh; y++)
        std::memcpy(&valid[(size_t)y * nw], &mask[(size_t)y * src_w],
                    sizeof(float) * nw);
    std::vector<float> scaled;
    resize_gray(valid, nw, nh, scaled, orig_w, orig_h);
    stroke_mask.assign((size_t)orig_w * orig_h, 0);
    int th = (int)(opt.seg_thresh * 255.f);
    for (size_t i = 0; i < scaled.size(); i++) {
        int v = (int)(scaled[i] * 255.f + 0.5f);
        if (v > th) stroke_mask[i] = 1;
    }
}

}  // namespace

bool dbnet_available() { return load_runtime(); }

bool dbnet_detect(const std::vector<uint8_t>& rgba, int w, int h,
                const dbnetOptions& opt, std::vector<dbnetBox>& out,
                std::vector<uint8_t>& stroke_mask, std::string* err) {
    out.clear();
    stroke_mask.clear();
    if (!load_runtime()) {
        if (err) *err = "onnxruntime.dll not found (place it next to lp2psd.exe)";
        return false;
    }
    if ((int)rgba.size() != w * h * 4 || w <= 0 || h <= 0) {
        if (err) *err = "invalid image buffer";
        return false;
    }
    if (!ensure_session(opt.model_path, err)) return false;
    OrtSession* session = g_session;

    // ---- Preprocess: resize_aspect + pad to 256 multiples + [-1,1] RGB ----
    const int mult = 256;
    const int size = std::max(1, opt.limit_side_len);
    float ratio = (float)size / (float)std::max(w, h);
    int tw = std::max(1, (int)std::llround(w * ratio));
    int th = std::max(1, (int)std::llround(h * ratio));
    int in_w = tw + (mult - tw % mult) % mult;
    int in_h = th + (mult - th % mult) % mult;

    std::vector<uint8_t> resized;
    resize_rgba(rgba, w, h, resized, tw, th);

    const size_t area = (size_t)in_w * in_h;
    std::vector<float> input(3 * area, -1.0f);  // pad area = black = -1
    float* planes[3] = {input.data(), input.data() + area,
                        input.data() + 2 * area};
    for (int y = 0; y < th; y++) {
        const uint8_t* row = &resized[(size_t)y * tw * 4];
        for (int x = 0; x < tw; x++) {
            size_t i = (size_t)y * in_w + x;
            planes[0][i] = row[x * 4 + 0] / 127.5f - 1.0f;  // R
            planes[1][i] = row[x * 4 + 1] / 127.5f - 1.0f;  // G
            planes[2][i] = row[x * 4 + 2] / 127.5f - 1.0f;  // B
        }
    }

    // ---- Inference ---------------------------------------------------------
    auto check = [&](OrtStatus* st, const char* what) -> bool {
        if (!st) return true;
        if (err) *err = std::string("onnxruntime: ") + what +
                        g_ort->GetErrorMessage(st);
        g_ort->ReleaseStatus(st);
        return false;
    };
    OrtAllocator* al = nullptr;
    char* in_name = nullptr;
    char* out0_name = nullptr;
    char* out1_name = nullptr;
    OrtMemoryInfo* mi = nullptr;
    OrtValue* in_t = nullptr;
    OrtValue* db_t = nullptr;
    OrtValue* mask_t = nullptr;
    bool ok = false;
    do {
        if (!check(g_ort->GetAllocatorWithDefaultOptions(&al), "")) break;
        if (!check(g_ort->SessionGetInputName(session, 0, al, &in_name), ""))
            break;
        if (!check(g_ort->SessionGetOutputName(session, 0, al, &out0_name),
                   ""))
            break;
        if (!check(g_ort->SessionGetOutputName(session, 1, al, &out1_name),
                   ""))
            break;
        if (!check(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator,
                                              OrtMemTypeDefault, &mi),
                   ""))
            break;
        int64_t shape[4] = {1, 3, in_h, in_w};
        if (!check(g_ort->CreateTensorWithDataAsOrtValue(
                       mi, input.data(), input.size() * sizeof(float), shape, 4,
                       ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_t),
                   ""))
            break;

        const char* ins[] = {in_name};
        const char* outs[] = {out0_name, out1_name};
        OrtValue* out_vals[2] = {nullptr, nullptr};
        if (!check(g_ort->Run(session, nullptr, ins, &in_t, 1, outs, 2,
                              out_vals),
                   ""))
            break;
        db_t = out_vals[0];
        mask_t = out_vals[1];

        auto read_dims = [&](OrtValue* v, std::vector<int64_t>& dims) -> bool {
            OrtTensorTypeAndShapeInfo* ti = nullptr;
            if (!check(g_ort->GetTensorTypeAndShape(v, &ti), "")) return false;
            size_t nd = 0;
            g_ort->GetDimensionsCount(ti, &nd);
            dims.assign(std::max<size_t>(nd, 1), 1);
            if (nd) g_ort->GetDimensions(ti, dims.data(), nd);
            g_ort->ReleaseTensorTypeAndShapeInfo(ti);
            return true;
        };

        std::vector<int64_t> db_dims, mask_dims;
        if (!read_dims(db_t, db_dims) || !read_dims(mask_t, mask_dims)) break;
        if ((int)db_dims.size() != 4 || (int)mask_dims.size() != 4) {
            if (err) *err = "unexpected model output rank";
            break;
        }
        int db_h = (int)db_dims[2], db_w = (int)db_dims[3];
        int mh = (int)mask_dims[2], mw = (int)mask_dims[3];
        if (db_h < in_h || db_w < in_w || db_dims[1] < 2) {
            if (err) *err = "unexpected model output shape";
            break;
        }
        
        if (mask_dims[0] < 1 || mask_dims[1] < 1 || mw <= 0 || mh <= 0) {
            if (err) *err = "unexpected mask output shape";
            break;
        }

        float* db = nullptr;
        float* mask = nullptr;
        if (!check(g_ort->GetTensorMutableData(db_t, (void**)&db), "")) break;
        if (!check(g_ort->GetTensorMutableData(mask_t, (void**)&mask), ""))
            break;

        // ---- Postprocess: lines (rotated quads) ----------------------------
        std::vector<float> prob(area);
        for (size_t i = 0; i < area; i++)
            prob[i] = 1.f / (1.f + std::exp(-db[i]));  // ch0 raw logits
        lines_from_prob_map(prob, in_w, in_h, ratio, w, h, opt, out);

        // ---- Postprocess: per-pixel stroke mask ----------------------------
        std::vector<float> mask_plane((size_t)mw * mh);
        std::memcpy(mask_plane.data(), mask, sizeof(float) * mw * mh);
        float mask_ratio = ratio * (float)mw / (float)in_w;
        seg_to_mask(mask_plane, mw, mh, mask_ratio, w, h, opt, stroke_mask);
        ok = true;
    } while (false);

    if (in_t) g_ort->ReleaseValue(in_t);
    if (db_t) g_ort->ReleaseValue(db_t);
    if (mask_t) g_ort->ReleaseValue(mask_t);
    if (mi) g_ort->ReleaseMemoryInfo(mi);
    if (al) {
        if (in_name) al->Free(al, in_name);
        if (out0_name) al->Free(al, out0_name);
        if (out1_name) al->Free(al, out1_name);
    }
    return ok;
}

#endif  // LP2PSD_WITH_dbnet
