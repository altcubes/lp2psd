#pragma once
// textcodec.hpp - UTF-8 / UTF-16 / system-ANSI text conversion helpers.
//
// Windows-only, header-only text conversion helpers for lp2psd.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

namespace textcodec {

// Wide string -> UTF-8 (empty in, empty out).
inline std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr,
                        nullptr);
    return out;
}

// UTF-8 -> wide string (empty in, empty out).
inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

// Reads a text file and returns its content as UTF-8. Detects UTF-8 (with or
// without BOM), UTF-16LE/BE (BOM); anything else is treated as the system
// ANSI codepage (e.g. GBK on Chinese Windows). Fills `err` when the file
// cannot be opened.
inline std::string read_text_file(const std::wstring& path, std::string* err) {
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

}  // namespace textcodec
