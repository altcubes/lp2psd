#include "style.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

#include "strutil.hpp"
#include "textcodec.hpp"

namespace {

// Maps common Chinese font *display* names to their PostScript names (the
// names Photoshop resolves in EngineData FontSet). Falls back to stripping
// spaces/tabs, which matches most Latin PostScript names ("Microsoft YaHei"
// -> "MicrosoftYaHei", "Source Han Sans SC" -> "SourceHanSansSC", ...).
std::string resolve_font_ps(const std::string& display) {
    const std::string name = strutil::trim(display);
    static const std::pair<const char*, const char*> kMap[] = {
        {"宋体", "SimSun"},
        {"新宋体", "NSimSun"},
        {"黑体", "SimHei"},
        {"楷体", "KaiTi"},
        {"仿宋", "FangSong"},
        {"仿宋_GB2312", "FangSong_GB2312"},
        {"楷体_GB2312", "KaiTi_GB2312"},
        {"微软雅黑", "MicrosoftYaHei"},
        {"微软雅黑 Light", "MicrosoftYaHeiLight"},
        {"微软雅黑Light", "MicrosoftYaHeiLight"},
        {"等线", "DengXian"},
        {"隶书", "LiSu"},
        {"幼圆", "YouYuan"},
        {"华文黑体", "STHeiti"},
        {"华文宋体", "STSong"},
        {"华文楷体", "STKaiti"},
        {"华文仿宋", "STFangsong"},
        {"华文中宋", "STZhongsong"},
        {"华文细黑", "STXihei"},
        {"华文琥珀", "STHupo"},
        {"华文隶书", "STLiti"},
        {"华文彩云", "STCaiyun"},
        {"华文新魏", "STXinwei"},
        {"方正舒体", "FZShuTi"},
        {"方正姚体", "FZYaoti"},
    };
    for (const auto& kv : kMap)
        if (name == kv.first) return kv.second;

    std::string out = name;
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](char c) { return c == ' ' || c == '\t'; }),
              out.end());
    return out;
}

// Accepts a number (0-3), "auto", or localized names; -1 means auto.
int parse_script(const mjson::Value& v, int dflt) {
    if (v.t == mjson::Value::T::Num) return (int)v.num_or(dflt);
    if (v.t != mjson::Value::T::Str) return dflt;
    std::string s = strutil::lower_ascii(v.str_or(""));
    if (s == "auto" || s == "自动") return -1;
    if (s == "roman" || s == "latin" || s == "罗马" || s == "拉丁") return 0;
    if (s == "japanese" || s == "日文" || s == "日语") return 1;
    if (s == "traditional" || s == "繁体") return 2;
    if (s == "chinese" || s == "中文" || s == "汉语" || s == "简体") return 3;
    if (s == "korean" || s == "韩文" || s == "韩语") return 4;
    return dflt;
}

// Accepts a number or a numeric string (e.g. "fontSize": "30"); returns the
// default for anything invalid or out of range.
double parse_font_size(const mjson::Value& v, double dflt) {
    if (v.t == mjson::Value::T::Num) {
        double d = v.num_or(dflt);
        return d > 0.0 && d < 10000.0 ? d : dflt;
    }
    if (v.t == mjson::Value::T::Str) {
        const char* p = v.str.c_str();
        char* end = nullptr;
        double d = std::strtod(p, &end);
        if (end != p && d > 0.0 && d < 10000.0) return d;
    }
    return dflt;
}

void read_color(const mjson::Value& v, uint8_t out[3], const uint8_t dflt[3]) {
    out[0] = dflt[0]; out[1] = dflt[1]; out[2] = dflt[2];
    if (v.t != mjson::Value::T::Arr || v.arr.size() < 3) return;
    for (int i = 0; i < 3; i++)
        out[i] = (uint8_t)std::max(0.0, std::min(255.0, v.arr[i].num_or(dflt[i])));
}

// Accepts a number or a name; falls back to `dflt` when unknown.
int parse_anti_alias(const mjson::Value& v, int dflt) {
    if (v.t == mjson::Value::T::Num) return (int)v.num_or(dflt);
    if (v.t != mjson::Value::T::Str) return dflt;
    std::string s = strutil::lower_ascii(v.str_or(""));
    if (s == "none" || s == "无") return 0;
    if (s == "crisp" || s == "犀利") return 1;
    if (s == "strong" || s == "浑厚") return 2;
    if (s == "smooth" || s == "平滑") return 3;
    if (s == "sharp" || s == "锐利") return 4;
    if (s == "lcd") return 6;
    return dflt;
}

int parse_orientation(const mjson::Value& v, int dflt) {
    if (v.t == mjson::Value::T::Num) return v.num_or(dflt) != 0.0 ? 1 : 0;
    if (v.t != mjson::Value::T::Str) return dflt;
    std::string s = strutil::lower_ascii(v.str_or(""));
    if (s == "vertical" || s == "竖排" || s == "v") return 1;
    if (s == "horizontal" || s == "横排" || s == "h") return 0;
    return dflt;
}

int parse_justification(const mjson::Value& v, int dflt) {
    if (v.t == mjson::Value::T::Num) return (int)v.num_or(dflt);
    if (v.t != mjson::Value::T::Str) return dflt;
    std::string s = strutil::lower_ascii(v.str_or(""));
    if (s == "left" || s == "左") return 0;
    if (s == "right" || s == "右") return 1;
    if (s == "center" || s == "居中" || s == "centre") return 2;
    if (s == "justifylastleft" || s == "两端对齐末行左对齐") return 3;
    if (s == "justifylastright" || s == "两端对齐末行右对齐") return 4;
    if (s == "justifylastcenter" || s == "两端对齐末行居中") return 5;
    if (s == "justifyall" || s == "全部两端对齐") return 6;
    return dflt;
}

}  // namespace

void load_font_style(const mjson::Value& f, Style& s) {
    if (const mjson::Value* n = f.get("name"))
        if (n->t == mjson::Value::T::Str) {
            s.font_name = textcodec::utf8_to_wide(n->str);
            s.font_ps = resolve_font_ps(n->str);
        }
    // Explicit PostScript name wins over display-name resolution.
    if (const mjson::Value* ps = f.get("postScript"))
        if (ps->t == mjson::Value::T::Str && !ps->str.empty())
            s.font_ps = ps->str;
    if (const mjson::Value* fs = f.get("fontSize"))
        s.font_size_pt = parse_font_size(*fs, s.font_size_pt);
    if (const mjson::Value* c = f.get("color")) read_color(*c, s.color, s.color);
    if (const mjson::Value* a = f.get("antiAlias"))
        s.anti_alias = parse_anti_alias(*a, s.anti_alias);
    if (const mjson::Value* o = f.get("orientation"))
        s.orientation = parse_orientation(*o, s.orientation);
    if (const mjson::Value* j = f.get("justification"))
        s.justification = parse_justification(*j, s.justification);
    if (const mjson::Value* al = f.get("autoLeading"))
        s.auto_leading = al->bool_or(s.auto_leading);
    if (const mjson::Value* als = f.get("autoLeadingSize"))
        s.auto_leading_size = als->num_or(s.auto_leading_size);
    if (const mjson::Value* ld = f.get("leading"))
        s.leading = ld->num_or(s.leading);
    if (const mjson::Value* dl = f.get("discretionaryLigatures"))
        s.discretionary_ligatures = dl->bool_or(s.discretionary_ligatures);
    if (const mjson::Value* sv = f.get("standardVerticalRomanAlignment"))
        s.standard_vertical_roman = sv->bool_or(s.standard_vertical_roman);
    if (const mjson::Value* sc = f.get("script"))
        s.script = parse_script(*sc, s.script);
}

void load_dpi(const mjson::Value& d, Style& s) {
    if (d.t == mjson::Value::T::Num) {
        double v = d.num_or(0.0);
        if (v >= 1.0 && v <= 10000.0) s.dpi = v;
    } else if (d.t == mjson::Value::T::Str) {
        // "original" / "auto" / 原图 均表示使用图片自身的 DPI。
        std::string x = strutil::lower_ascii(d.str_or(""));
        if (x == "original" || x == "auto" || x == "image" ||
            x == "原图" || x == "自动")
            s.dpi = 0.0;
    }
}

void load_bg_copy(const mjson::Value& bc, Style& s) {
    if (const mjson::Value* v = bc.get("enabled"))
        s.bg_copy.enabled = v->bool_or(s.bg_copy.enabled);
    if (const mjson::Value* v = bc.get("layerName"))
        s.bg_copy.layer_name = v->str_or(s.bg_copy.layer_name);
}

void load_whiten(const mjson::Value& wh, Style& s) {
    if (const mjson::Value* v = wh.get("enabled"))
        s.dbnet.whiten.enabled = v->bool_or(s.dbnet.whiten.enabled);
    if (const mjson::Value* v = wh.get("color"))
        read_color(*v, s.dbnet.whiten.color, s.dbnet.whiten.color);
    if (const mjson::Value* v = wh.get("margin"))
        s.dbnet.whiten.margin = (int)v->num_or(s.dbnet.whiten.margin);
    if (const mjson::Value* v = wh.get("boxMarginX"))
        s.dbnet.whiten.box_margin_x =
            (int)v->num_or(s.dbnet.whiten.box_margin_x);
    if (const mjson::Value* v = wh.get("boxMarginY"))
        s.dbnet.whiten.box_margin_y =
            (int)v->num_or(s.dbnet.whiten.box_margin_y);
    if (const mjson::Value* v = wh.get("limitToBoxes"))
        s.dbnet.whiten.limit_to_boxes =
            v->bool_or(s.dbnet.whiten.limit_to_boxes);
    if (const mjson::Value* v = wh.get("layerName"))
        s.dbnet.whiten.layer_name = v->str_or(s.dbnet.whiten.layer_name);
    if (const mjson::Value* v = wh.get("transparency")) {
        double t = v->num_or(s.dbnet.whiten.opacity);
        if (t >= 0.0 && t <= 100.0)
            s.dbnet.whiten.opacity = 100.0 - t;
    }
}

void load_boxes(const mjson::Value& bx, Style& s) {
    if (const mjson::Value* v = bx.get("enabled"))
        s.dbnet.boxes.enabled = v->bool_or(s.dbnet.boxes.enabled);
    if (const mjson::Value* v = bx.get("color"))
        read_color(*v, s.dbnet.boxes.color, s.dbnet.boxes.color);
    if (const mjson::Value* v = bx.get("layerName"))
        s.dbnet.boxes.layer_name = v->str_or(s.dbnet.boxes.layer_name);
    if (const mjson::Value* v = bx.get("lock"))
        s.dbnet.boxes.lock = v->bool_or(s.dbnet.boxes.lock);
}

void load_dbnet(const mjson::Value& o, Style& s) {
    if (const mjson::Value* en = o.get("enabled"))
        s.dbnet.enabled = en->bool_or(false);
    if (const mjson::Value* m = o.get("model"))
        s.dbnet.model = m->str_or(s.dbnet.model);
    if (const mjson::Value* v = o.get("limitSideLen"))
        s.dbnet.limit_side_len = (int)v->num_or(s.dbnet.limit_side_len);
    if (const mjson::Value* v = o.get("dbBinThreshold"))
        s.dbnet.det_thresh = v->num_or(s.dbnet.det_thresh);
    else if (const mjson::Value* v = o.get("detThresh"))  // legacy alias
        s.dbnet.det_thresh = v->num_or(s.dbnet.det_thresh);
    if (const mjson::Value* v = o.get("dbBoxThreshold"))
        s.dbnet.box_thresh = v->num_or(s.dbnet.box_thresh);
    if (const mjson::Value* v = o.get("dbUnclipRatio"))
        s.dbnet.unclip_ratio = v->num_or(s.dbnet.unclip_ratio);
    if (const mjson::Value* v = o.get("minSide"))
        s.dbnet.min_side = v->num_or(s.dbnet.min_side);
    if (const mjson::Value* v = o.get("segThreshold"))
        s.dbnet.seg_thresh = v->num_or(s.dbnet.seg_thresh);
    if (const mjson::Value* v = o.get("minBoxArea"))
        s.dbnet.min_box_area = (int)v->num_or(s.dbnet.min_box_area);
    if (const mjson::Value* wh = o.get("whiten")) load_whiten(*wh, s);
    if (const mjson::Value* bx = o.get("boxes")) load_boxes(*bx, s);
}

void load_layer_settings(const mjson::Value& ly, Style& s) {
    if (const mjson::Value* v = ly.get("opacity")) {
        double o = v->num_or(s.layers.opacity);
        if (o >= 0.0 && o <= 100.0) s.layers.opacity = o;
    }
}

Style load_style(const mjson::Value& cfg) {
    Style s;
    if (cfg.is_null()) return s;
    if (const mjson::Value* f = cfg.get("font")) load_font_style(*f, s);
    if (const mjson::Value* d = cfg.get("dpi")) load_dpi(*d, s);
    if (const mjson::Value* bc = cfg.get("bgCopy")) load_bg_copy(*bc, s);
    if (const mjson::Value* o = cfg.get("dbnet")) load_dbnet(*o, s);
    if (const mjson::Value* ly = cfg.get("layers")) load_layer_settings(*ly, s);
    return s;
}
