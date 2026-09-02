#include "layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "strutil.hpp"
#include "textcodec.hpp"

namespace {

// "----[N]----[x,y,group]": parses the entry header line. Returns false when
// the line is not an entry header. If the line has the header shape
// (bracketed numeric index + second bracket group) but its fields are not
// valid numbers, sets *why so the caller can report and skip the line
// instead of silently shifting the remaining fields.
bool parse_entry_line(const std::string& line, int& idx, double& x,
                      double& y, int& group, std::string* why = nullptr) {
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
    auto parse_num = [](const std::string& t, double& v) {
        if (t.empty()) return false;
        char* end = nullptr;
        v = std::strtod(t.c_str(), &end);
        return end != t.c_str() && *end == '\0' && std::isfinite(v);
    };
    auto fail = [&](const std::string& msg) {
        if (why) *why = msg;
        return false;
    };
    for (char c : fields) {
        if (c == ',') {
            double v = 0;
            if (!cur.empty()) {
                if (!parse_num(cur, v))
                    return fail("field '" + cur + "' is not a number");
                nums.push_back(v);
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    double v = 0;
    if (!cur.empty()) {
        if (!parse_num(cur, v))
            return fail("field '" + cur + "' is not a number");
        nums.push_back(v);
    }
    if (nums.size() < 3)
        return fail("expected 3 comma-separated numbers [x,y,group]");

    idx = std::atoi(index_str.c_str());
    // Normalized coordinates are documented as 0..1 of the image size.
    x = std::max(0.0, std::min(1.0, nums[0]));
    y = std::max(0.0, std::min(1.0, nums[1]));
    group = (int)nums[2];
    return true;
}

}  // namespace

bool parse_layout(const std::wstring& path, Layout& out, std::string* err) {
    std::string text = textcodec::read_text_file(path, err);
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
    size_t line_no = 0;

    for (const std::string& raw : lines) {
        line_no++;
        std::string line = strutil::trim(raw);
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
                std::string name = strutil::trim(line.substr(0, dash));
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
                std::string lower = strutil::lower_ascii(line);
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
            std::string why;
            if (parse_entry_line(line, idx, x, y, g, &why)) {
                TextEntry ne;
                ne.index = idx;
                ne.x = x;
                ne.y = y;
                ne.group = g;
                block->entries.push_back(std::move(ne));
                entry = &block->entries.back();
                continue;
            }
            if (!why.empty()) {
                // Malformed entry header: report it and skip the line; the
                // rest of the file keeps parsing so later entries still
                // generate.
                fprintf(stderr,
                        "layout: %s: line %zu: skipping malformed entry "
                        "header (%s): %s\n",
                        textcodec::wide_to_utf8(path).c_str(),
                        line_no, why.c_str(), line.c_str());
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
