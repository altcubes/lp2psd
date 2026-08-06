#pragma once
// layout.hpp - layout text file model and parser.
//
// The layout file (see README "txt 格式") describes image blocks and text
// entries placed at normalized coordinates, optionally assigned to groups.

#include <string>
#include <utility>
#include <vector>

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

// Parses `path` into `out`. On failure returns false and fills `err`.
bool parse_layout(const std::wstring& path, Layout& out, std::string* err);
