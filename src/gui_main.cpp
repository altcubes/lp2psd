// psdgen-gui - native Win32 GUI wrapper around psdgen.exe.
//
// Built with the Windows SDK only (user32/comdlg32/shell32), no resource
// files, no third-party UI framework. Lets the user pick a layout txt and
// runs psdgen.exe in the background while streaming its console output into
// the log box.

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
#include <shellapi.h>

#include <string>
#include <thread>
#include <vector>

#include "textcodec.hpp"
#include "windows_ui.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr UINT WM_APP_RUN_DONE = WM_APP + 1;
constexpr UINT WM_APP_RUN_LOG = WM_APP + 2;

// ---------------- helpers ----------------

bool file_exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Reads stdout/stderr from a child process until it exits. Returns exit code
// and captures all output (UTF-8, converted to wide for display).
int run_process(const std::wstring& exe, const std::wstring& args,
                std::wstring& out_log) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hOutRead = nullptr, hOutWrite = nullptr;
    if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0)) return -1;
    SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hOutWrite;
    si.hStdError = hOutWrite;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hOutWrite);
    if (!ok) {
        CloseHandle(hOutRead);
        return -1;
    }

    std::string acc;
    char buf[4096];
    DWORD rd = 0;
    while (ReadFile(hOutRead, buf, sizeof(buf), &rd, nullptr) && rd > 0)
        acc.append(buf, rd);
    CloseHandle(hOutRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // psdgen prints UTF-8; convert best-effort.
    if (!acc.empty()) out_log = textcodec::utf8_to_wide(acc);
    return (int)code;
}

// ---------------- window state ----------------

struct AppState {
    HWND hwnd = nullptr;
    HWND edit_path = nullptr;
    HWND btn_browse = nullptr;
    HWND btn_run = nullptr;
    HWND edit_log = nullptr;
    std::wstring exe_dir;
    bool running = false;
};

AppState g_state;

void append_log(const std::wstring& text) {
    if (!g_state.edit_log) return;
    int len = GetWindowTextLengthW(g_state.edit_log);
    SendMessageW(g_state.edit_log, EM_SETSEL, len, len);
    SendMessageW(g_state.edit_log, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    SendMessageW(g_state.edit_log, EM_SCROLLCARET, 0, 0);
}

void set_running(bool running) {
    g_state.running = running;
    EnableWindow(g_state.btn_run, !running);
    EnableWindow(g_state.btn_browse, !running);
    EnableWindow(g_state.edit_path, !running);
}

// Runs in a background thread; posts results to the GUI thread.
void run_generation(const std::wstring& txt_path) {
    std::wstring exe = g_state.exe_dir + L"\\psdgen.exe";
    if (!file_exists(exe)) {
        PostMessageW(g_state.hwnd, WM_APP_RUN_DONE, 2,
                     (LPARAM)(new std::wstring(L"未找到 psdgen.exe（应与本程序同目录）")));
        return;
    }
    std::wstring args = L"\"" + txt_path + L"\"";
    std::wstring log;
    int code = run_process(exe, args, log);
    std::wstring msg;
    if (code == -1) {
        msg = L"启动 psdgen.exe 失败";
    } else if (code == 0) {
        msg = L"\r\n✓ 生成完成，已自动打开输出目录\r\n";
    } else {
        msg = L"\r\n✗ 生成失败（退出码 " + std::to_wstring(code) + L"）\r\n";
    }
    if (!log.empty()) msg = log + msg;
    PostMessageW(g_state.hwnd, WM_APP_RUN_DONE, (WPARAM)code,
                 (LPARAM)(new std::wstring(msg)));
}

void on_run() {
    if (g_state.running) return;
    wchar_t buf[MAX_PATH * 4] = L"";
    GetWindowTextW(g_state.edit_path, buf, (int)std::size(buf));
    std::wstring path = buf;
    if (path.empty()) {
        MessageBoxW(g_state.hwnd, L"请先选择排版文本文件", L"提示", MB_ICONINFORMATION);
        return;
    }
    if (!file_exists(path)) {
        MessageBoxW(g_state.hwnd, L"文件不存在，请重新选择", L"提示", MB_ICONWARNING);
        return;
    }
    set_running(true);
    append_log(L"\r\n—— 开始生成：" + path + L" ——\r\n");
    std::thread(run_generation, path).detach();
}

// ---------------- window & dialog ----------------

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_state.hwnd = hwnd;
            g_state.exe_dir = ui::exe_directory();
            CreateWindowW(L"STATIC", L"排版文本文件：", WS_CHILD | WS_VISIBLE,
                          12, 14, 110, 20, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            g_state.edit_path = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE |
                                              WS_BORDER | ES_AUTOHSCROLL,
                                              122, 12, 500, 22, hwnd, nullptr,
                                              GetModuleHandleW(nullptr), nullptr);
            g_state.btn_browse = CreateWindowW(L"BUTTON", L"浏览…", WS_CHILD |
                                               WS_VISIBLE | BS_PUSHBUTTON,
                                               628, 11, 72, 24, hwnd,
                                               (HMENU)101, GetModuleHandleW(nullptr), nullptr);
            g_state.btn_run = CreateWindowW(L"BUTTON", L"生成 PSD", WS_CHILD |
                                            WS_VISIBLE | BS_PUSHBUTTON,
                                            706, 11, 100, 24, hwnd,
                                            (HMENU)102, GetModuleHandleW(nullptr), nullptr);
            g_state.edit_log = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE |
                                             WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL |
                                             ES_READONLY | WS_VSCROLL,
                                             12, 44, 794, 380, hwnd, nullptr,
                                             GetModuleHandleW(nullptr), nullptr);
            SendMessageW(g_state.edit_log, WM_SETFONT,
                         (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            append_log(L"psdgen GUI\r\n请选择排版文本文件（txt），然后点击“生成 PSD”。\r\n");
            return 0;
        }
        case WM_COMMAND: {
            if (HIWORD(wp) == BN_CLICKED) {
                if (LOWORD(wp) == 101) {
                    std::wstring f;
                    if (ui::pick_text_file(f, hwnd))
                        SetWindowTextW(g_state.edit_path, f.c_str());
                } else if (LOWORD(wp) == 102) {
                    on_run();
                }
            }
            return 0;
        }
        case WM_APP_RUN_DONE: {
            std::wstring* msg = (std::wstring*)lp;
            if (msg) {
                append_log(*msg);
                delete msg;
            }
            set_running(false);
            return 0;
        }
        case WM_APP_RUN_LOG: {
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    // Single instance: bring the existing window forward instead.
    {
        HWND existing = FindWindowW(L"psdgen_gui_window", nullptr);
        if (existing) {
            SetForegroundWindow(existing);
            return 0;
        }
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"psdgen_gui_window";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&wc)) return 1;

    HWND hwnd = CreateWindowW(L"psdgen_gui_window", L"PSD 生成器",
                              WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 830, 470,
                              nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
