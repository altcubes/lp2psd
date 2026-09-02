#pragma once
// dbnet.hpp - optional text-region detection for the whitening layer, using the
// manga-image-translator DBNet detector (ResNet34 + DB head) exported to ONNX
// (the same detector yakuyomi-engine runs, see scripts/export_dbnet_onnx.py).
// Detection only: rotated text-line quadrilaterals + a per-pixel stroke mask;
// no content recognition.
//
// Compiled only when LP2PSD_WITH_dbnet is defined (CMake sets it when
// third_party/onnxruntime is present). Without it the stubs below keep the
// rest of the program unchanged: dbnet reports "unavailable" and callers fall
// back to config defaults.

#include <string>
#include <vector>

// A detected text region, in original image pixels. `quad` holds the four
// corners of the rotated quadrilateral (x0,y0, x1,y1, x2,y2, x3,y3);
// x/y/w/h is the axis-aligned bounding box (derived, for convenience).
struct dbnetBox {
    double quad[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    double x = 0, y = 0, w = 0, h = 0;
    float score = 0;  // mean probability inside the region (DB box_score)
};

struct dbnetOptions {
    std::string model_path;   // ONNX DBNet det model, resolved to abs path
    int limit_side_len = 1024;  // long-side resize limit before inference
    float det_thresh = 0.5f;  // sigmoid(db) binarization (dbBinThreshold)
    float box_thresh = 0.7f;  // drop lines with mean prob below this
    float unclip_ratio = 2.3f;// DB unclip: d = area*ratio/perimeter
    float min_side = 3.0f;    // drop lines with a side < this (model grid px)
    float seg_thresh = 0.12f; // stroke-mask binarization threshold
    int min_box_area = 64;    // drop boxes smaller than this (original px^2)
};

#ifdef LP2PSD_WITH_dbnet

bool dbnet_available();

// Detects text regions. `out` receives one rotated quad per text line;
// `stroke_mask` (w*h bytes, 1 = text stroke) is the per-pixel stroke mask in
// original image pixels, used for precise whitening. Both are cleared first.
bool dbnet_detect(const std::vector<uint8_t>& rgba, int w, int h,
                const dbnetOptions& opt, std::vector<dbnetBox>& out,
                std::vector<uint8_t>& stroke_mask, std::string* err);

#else  // !LP2PSD_WITH_dbnet

inline bool dbnet_available() { return false; }
inline bool dbnet_detect(const std::vector<uint8_t>&, int, int,
                       const dbnetOptions&, std::vector<dbnetBox>&,
                       std::vector<uint8_t>&, std::string* err) {
    if (err) *err = "built without dbnet support (third_party/onnxruntime missing)";
    return false;
}

#endif  // LP2PSD_WITH_dbnet
