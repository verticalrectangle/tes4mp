#include "../include/obse_types.h"
#include "scriptfuncs.h"
#include "network.h"
#include "game_hooks.h"
#include "ui.h"
#include <windows.h>

static PluginHandle g_pluginHandle = 0;
static UINT_PTR     g_timerId      = 0;
static HWND         g_gameHwnd     = nullptr;

static void CALLBACK GameThreadTimer(HWND, UINT, UINT_PTR, DWORD) {
    UI_Tick();
}

// Find the main visible window belonging to this process.
struct FindWndData { DWORD pid; HWND result; };
static BOOL CALLBACK FindWndProc(HWND hwnd, LPARAM lp) {
    auto* d = (FindWndData*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == d->pid && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
        d->result = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND FindGameWindow() {
    FindWndData d = { GetCurrentProcessId(), nullptr };
    EnumWindows(FindWndProc, (LPARAM)&d);
    return d.result;
}

// Background thread: waits for Oblivion's window to exist, then installs the timer on it.
// The timer fires on the game thread (the thread that owns and pumps messages for g_gameHwnd).
static DWORD WINAPI TimerSetupThread(LPVOID) {
    for (int i = 0; i < 100; ++i) {   // try for up to 10 s
        HWND hwnd = FindGameWindow();
        if (hwnd) {
            g_gameHwnd = hwnd;
            g_timerId  = SetTimer(hwnd, 1, 200, GameThreadTimer);
            return 0;
        }
        Sleep(100);
    }
    return 1; // gave up
}

extern "C" {

__declspec(dllexport) bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info) {
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name        = "TES4MP";
    info->version     = 1;

    if (obse->isEditor) return false;

    return true;
}

__declspec(dllexport) bool OBSEPlugin_Load(const OBSEInterface* obse) {
    g_pluginHandle = obse->GetPluginHandle();

    RegisterScriptFunctions(const_cast<OBSEInterface*>(obse));
    GameHooks_Init(const_cast<OBSEInterface*>(obse), g_pluginHandle);
    UI_Init();

    // Find Oblivion's window and attach the 200 ms tick timer to it so the
    // callback runs on the game thread (which owns and pumps that window).
    CloseHandle(CreateThread(nullptr, 0, TimerSetupThread, nullptr, 0, nullptr));

    return true;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        if (g_timerId && g_gameHwnd) KillTimer(g_gameHwnd, g_timerId);
        UI_Shutdown();
        GameHooks_Shutdown();
        g_network.disconnect();
    }
    return TRUE;
}
