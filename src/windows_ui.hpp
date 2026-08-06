#pragma once
// windows_ui.hpp - small shared Win32 UI helpers (file dialog, exe dir).
//
// Shared by psdgen (CLI file-picker mode) and psdgen-gui so the two binaries
// keep identical dialog behavior.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commdlg.h>

#include <iterator>
#include <string>

#pragma comment(lib, "comdlg32.lib")

namespace ui {

// Directory of the running executable, without a trailing separator.
inline std::wstring exe_directory() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    if (n == 0 || n >= std::size(buf)) return L"";
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

// Modal "open file" dialog for the layout text file. Returns false on cancel.
inline bool pick_text_file(std::wstring& out_path, HWND owner = nullptr) {
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)std::size(file);
    ofn.lpstrTitle = L"选择排版文本文件";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return false;
    out_path = file;
    return true;
}

}  // namespace ui
