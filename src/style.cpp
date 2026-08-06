#include "style.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "textcodec.hpp"

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// Maps common Chinese font *display* names to their PostScript names (the
// names Photoshop resolves in EngineData FontSet). Falls back to stripping
// spaces/tabs, which matches most Latin PostScript names ("Microsoft YaHei"
// -> "MicrosoftYaHei", "Source Han Sans SC" -> "SourceHanSansSC", ...).
std::string resolve_font_ps(const std::string& display) {
    const std::string name = trim(display);
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
    std::string s = lower_ascii(v.str_or(""));
    if (s == "auto" || s == "自动") return -1;
    if (s == "roman" || s == "latin" || s == "罗马" || s == "拉丁") return 0;
    if (s == "japanese" || s == "日文" || s == "日语") return 1;
    if (s == "traditional" || s == "繁体") return 2;
    if (s == "chinese" || s == "中文" || s == "汉语" || s == "简体") return 3;
    if (s == "korean" || s == "韩文" || s == "韩语") return 4;
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
    std::string s = lower_ascii(v.str_or(""));
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
    std::string s = lower_ascii(v.str_or(""));
    if (s == "vertical" || s == "竖排" || s == "v") return 1;
    if (s == "horizontal" || s == "横排" || s == "h") return 0;
    return dflt;
}

int parse_justification(const mjson::Value& v, int dflt) {
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

}  // namespace

Style load_style(const mjson::Value& cfg) {
    Style s;
    if (cfg.is_null()) return s;
    if (const mjson::Value* f = cfg.get("font")) {
        if (const mjson::Value* n = f->get("name"))
            if (n->t == mjson::Value::T::Str) {
                s.font_name = textcodec::utf8_to_wide(n->str);
                s.font_ps = resolve_font_ps(n->str);
            }
        // Explicit PostScript name wins over display-name resolution.
        if (const mjson::Value* ps = f->get("postScript"))
            if (ps->t == mjson::Value::T::Str && !ps->str.empty())
                s.font_ps = ps->str;
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
        if (const mjson::Value* sc = f->get("script"))
            s.script = parse_script(*sc, s.script);
    }
    s.output_dir = cfg.get("outputDir") ? cfg.get("outputDir")->str_or("") : "";
    s.prefix = cfg.get("prefix") ? cfg.get("prefix")->str_or("") : "";
    s.suffix = cfg.get("suffix") ? cfg.get("suffix")->str_or("") : "";
    return s;
}
