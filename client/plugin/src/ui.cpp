#include "ui.h"
#include "game_hooks.h"
#include "network.h"
#include "json_util.h"
#include <windows.h>
#include <string>

static HWND   g_hwnd   = nullptr;
static UINT   g_hotkey = 1;

// ── F10 command-input dialog ───────────────────────────────────────────────────

static std::string Win32InputBox(const char* title, const char* prompt) {
    struct DD { const char* prompt; std::string result; };
    static auto proc = [](HWND hw, UINT m, WPARAM w, LPARAM l) -> INT_PTR {
        if (m == WM_INITDIALOG) {
            auto* d = (DD*)l;
            SetWindowLongPtrA(hw, DWLP_USER, (LONG_PTR)d);
            SetDlgItemTextA(hw, 100, d->prompt);
            SetFocus(GetDlgItem(hw, 101));
            return FALSE;
        }
        if (m == WM_COMMAND && LOWORD(w) == IDOK) {
            auto* d = (DD*)GetWindowLongPtrA(hw, DWLP_USER);
            char buf[512] = {};
            GetDlgItemTextA(hw, 101, buf, 512);
            if (d) d->result = buf;
            EndDialog(hw, IDOK);
            return TRUE;
        }
        if (m == WM_COMMAND && LOWORD(w) == IDCANCEL) {
            EndDialog(hw, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    };

    static unsigned char tpl[512];
    memset(tpl, 0, sizeof(tpl));
    WORD* p = (WORD*)tpl;

    auto ws = [&](const wchar_t* s) { while (*s) *p++ = *s++; *p++ = 0; };
    auto a4 = [&]() { auto off = ((ULONG_PTR)p) & 3; if (off) p = (WORD*)((ULONG_PTR)p + (4 - off)); };
    auto item = [&](DWORD sty, short x, short y, short cx, short cy, WORD id,
                    const wchar_t* cls, const wchar_t* txt) {
        a4();
        auto* it = (DLGITEMTEMPLATE*)p;
        it->style = sty | WS_CHILD | WS_VISIBLE;
        it->dwExtendedStyle = 0;
        it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
        p = (WORD*)(it + 1); ws(cls); ws(txt); *p++ = 0;
    };

    wchar_t wt[64] = {}, wp[256] = {};
    MultiByteToWideChar(CP_ACP, 0, title,  -1, wt, 64);
    MultiByteToWideChar(CP_ACP, 0, prompt, -1, wp, 256);

    auto* hdr = (DLGTEMPLATE*)p;
    hdr->style = DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    hdr->cdit = 4; hdr->x = hdr->y = 0; hdr->cx = 265; hdr->cy = 62;
    p = (WORD*)(hdr + 1); *p++ = 0; *p++ = 0; ws(wt); *p++ = 9; ws(L"Segoe UI");

    item(SS_LEFT,                          5,  5, 250, 12, 100, L"STATIC", wp);
    item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 5, 20, 250, 14, 101, L"EDIT", L"");
    item(BS_DEFPUSHBUTTON | WS_TABSTOP,  195, 40,  60, 14, IDOK,     L"BUTTON", L"OK");
    item(WS_TABSTOP,                     130, 40,  60, 14, IDCANCEL, L"BUTTON", L"Cancel");

    DD dd; dd.prompt = prompt;
    INT_PTR r = DialogBoxIndirectParamA(nullptr, (DLGTEMPLATE*)tpl, nullptr,
                                        (DLGPROC)(void*)+proc, (LPARAM)&dd);
    return (r == IDOK) ? dd.result : std::string{};
}

// ── Hotkey window (F10 → send /command) ──────────────────────────────────────

static LRESULT CALLBACK HotkeyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_HOTKEY && wp == g_hotkey) {
        if (!g_network.isConnected()) return 0;
        std::string text = Win32InputBox("TES4MP Chat", "Say something (prefix / for commands):");
        if (!text.empty()) {
            // No leading slash → wrap as /say so the server routes it as chat.
            std::string send = (text[0] == '/') ? text : "/say " + text;
            g_network.send(json::obj({
                json::str("type", "COMMAND"),
                json::str("text", send),
            }));
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static DWORD WINAPI UIThread(LPVOID) {
    WNDCLASSA wc    = {};
    wc.lpfnWndProc  = HotkeyWndProc;
    wc.lpszClassName = "TES4MPHotkey";
    wc.hInstance    = GetModuleHandleA(nullptr);
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(0, "TES4MPHotkey", "",
                              0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return 1;

    RegisterHotKey(g_hwnd, g_hotkey, 0, VkKeyScanA('/') & 0xFF);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

static HANDLE g_uiThread = nullptr;

void UI_Init() {
    g_uiThread = CreateThread(nullptr, 0, UIThread, nullptr, 0, nullptr);
}

void UI_Tick() {
    GameHooks_Tick();
}

void UI_Shutdown() {
    if (g_hwnd) {
        UnregisterHotKey(g_hwnd, g_hotkey);
        PostMessageA(g_hwnd, WM_QUIT, 0, 0);
    }
    if (g_uiThread) {
        WaitForSingleObject(g_uiThread, 2000);
        CloseHandle(g_uiThread);
        g_uiThread = nullptr;
    }
}
