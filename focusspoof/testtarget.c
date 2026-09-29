/**
 * @file focusspoof/testtarget.c
 * @brief Win32 host app used to smoke-test focusspoof.dll injection.
 *
 * Creates a visible top-level window and pumps its message loop. Every 2 s it
 * writes a line to logs\focusspoof_target_<pid>.log (relative to the working
 * directory) recording whether the injected
 * focus spoof makes GetForegroundWindow() report *this* window ("me:1") even
 * when another window holds the real foreground - which is exactly the spoof
 * the game engine relies on - and whether the injected input blocker swallowed
 * a posted WM_KEYDOWN before the window's own WndProc saw it (key_blocked:1).
 *
 * Usage: testtarget.exe [suffix]
 * Build: gcc -municode -mwindows -O2 -s testtarget.c -o testtarget.exe
 * 32-bit (MINGW32 toolchain):
 *        gcc -m32 -municode -mwindows -O2 -s testtarget.c -o testtarget32.exe
 */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>

static HWND g_hwnd = 0;
static FILE* g_log = 0;
static char g_suffix[16] = "";
static volatile LONG g_sawKeydown = 0; ///< Set when WndProc actually receives WM_KEYDOWN.

typedef DWORD(WINAPI* TGetState)(DWORD, XINPUT_STATE*);
static TGetState g_xgs = 0; ///< XInputGetState resolved lazily from xinput1_4.dll.
static TGetState g_xgsEx = 0; ///< XInputGetStateEx resolved lazily (name, then ordinal 100).

/**
 * @brief Query the controller with the process's own XInputGetState.
 *
 * The import is resolved dynamically so the probe exercises the exact module
 * focusspoof.dll is expected to hook (xinput1_4 is loaded here first, which
 * makes the injected resolver prefer it).
 * @param btnOut Receives the gamepad button bits.
 * @return The HRESULT returned by XInputGetState.
 */
static DWORD ProbeXInput(DWORD* btnOut) {
  *btnOut = 0;
  if (!g_xgs) {
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (h)
      g_xgs = (TGetState)GetProcAddress(h, "XInputGetState");
  }
  if (!g_xgs)
    return 0xFFFFFFFF;
  XINPUT_STATE st;
  DWORD hr = g_xgs(0, &st);
  *btnOut = st.Gamepad.wButtons;
  return hr;
}

/**
 * @brief Query the controller through the undocumented extended entry.
 *
 * Many titles read the pad via XInputGetStateEx instead of plain
 * XInputGetState; probing it proves the injected rewrite serves that entry
 * too (the hook uses the same body). xinput1_4 publishes it by name and at
 * ordinal 100; some runtimes only expose a matching ordinal.
 * @param btnOut Receives the gamepad button bits.
 * @return The HRESULT returned by the extended entry.
 */
static DWORD ProbeXInputEx(DWORD* btnOut) {
  *btnOut = 0;
  if (!g_xgsEx) {
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (h) {
      g_xgsEx = (TGetState)GetProcAddress(h, "XInputGetStateEx");
      if (!g_xgsEx)
        g_xgsEx = (TGetState)GetProcAddress(h, (LPCSTR)MAKEWORD(100, 0));
    }
  }
  if (!g_xgsEx)
    return 0xFFFFFFFF;
  XINPUT_STATE st;
  DWORD hr = g_xgsEx(0, &st);
  *btnOut = st.Gamepad.wButtons;
  return hr;
}

static volatile LONG g_servedSaw = 0;    ///< Latched when a rewrite-served state was observed.
static volatile LONG g_servedLogged = 0; ///< Whether the SERVED marker was flushed.
static volatile LONG g_servedExSaw = 0;    ///< Latched when the extended entry was served.
static volatile LONG g_servedExLogged = 0; ///< Whether the SERVEDEX marker was flushed.

/**
 * @brief Fast XInput probe: sample the state every ~20 ms.
 *
 * The rewritten state is only valid for the slot freshness window (~120 ms),
 * so a 2 s log cadence would miss it. A classic real XInputGetState in this
 * environment (no controller attached) always returns
 * ERROR_DEVICE_NOT_CONNECTED, so observing hr==0 can only come from the
 * injected rewrite - the markers are flushed immediately when such an
 * observation happens, one for the plain entry and one for the extended one.
 */
static void ProbeFast(void) {
  static DWORD last = 0;
  DWORD now = GetTickCount();
  if (now - last < 20)
    return;
  last = now;
  DWORD xbtn = 0;
  DWORD hr = ProbeXInput(&xbtn);
  if (hr == 0 && InterlockedCompareExchange(&g_servedSaw, 1, 0) == 0 &&
      g_servedLogged == 0) {
    InterlockedExchange(&g_servedLogged, 1);
    if (g_log) {
      fprintf(g_log, "SERVED t=%lu xbtn=%u\n", now, xbtn);
      fflush(g_log);
    }
  }
  DWORD xbtnEx = 0;
  DWORD hrEx = ProbeXInputEx(&xbtnEx);
  if (hrEx == 0 && InterlockedCompareExchange(&g_servedExSaw, 1, 0) == 0 &&
      g_servedExLogged == 0) {
    InterlockedExchange(&g_servedExLogged, 1);
    if (g_log) {
      fprintf(g_log, "SERVEDEX t=%lu xbtn=%u\n", now, xbtnEx);
      fflush(g_log);
    }
  }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_CLOSE || m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  if (m == WM_KEYDOWN) {
    InterlockedExchange(&g_sawKeydown, 1);
    return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE h, HINSTANCE, LPWSTR cmd, int nCmdShow) {
  WNDCLASSEXW wc;
  ZeroMemory(&wc, sizeof wc);
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = h;
  wc.lpszClassName = L"FsTestTarget";
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  RegisterClassExW(&wc);

  if (cmd && cmd[0]) {
    /* cmd is the whole wide command line; the suffix is the first token. */
    LPCWSTR p = cmd;
    while (*p && *p != L' ' && *p != L'\t')
      p++;
    char narrow[32];
    int n = WideCharToMultiByte(CP_ACP, 0, cmd, (int)(p - cmd), narrow, 31, 0, 0);
    narrow[n > 0 ? n : 0] = 0;
    strcpy(g_suffix, narrow);
  }

  /* The suffix (if any) only goes into the log file; the title is fixed. */
  const wchar_t* title = L"focusspoof-target";

  g_hwnd = CreateWindowExW(0, wc.lpszClassName, title,
      WS_OVERLAPPEDWINDOW, 100, 100, 640, 480,
      NULL, NULL, h, NULL);
  ShowWindow(g_hwnd, SW_SHOWNORMAL);
  UpdateWindow(g_hwnd);

  CreateDirectoryA("logs", NULL);
  char logPath[64];
  sprintf(logPath, "logs\\focusspoof_target_%lu.log", GetCurrentProcessId());
  g_log = fopen(logPath, "w");

  DWORD last = 0;
  MSG m;
  while (1) {
    ProbeFast(); /* sample every ~20 ms so the rewrite window is never missed */
    if (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE) > 0) {
      if (m.message == WM_QUIT)
        break;
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }

    DWORD now = GetTickCount();
    if (now - last >= 2000) {
      last = now;
      if (g_log) {
        HWND fg = GetForegroundWindow();
        /*
         * key_blocked is a black-box check of the injected input blocker:
         * we post a WM_KEYDOWN to our own queue, let the message pump
         * dispatch it, and see whether WndProc ever received it. When the
         * blocker is live the subclassed SwallowProc eats the message
         * (key_blocked=1); without it WndProc sees the keydown.
         */
        InterlockedExchange(&g_sawKeydown, 0);
        PostMessageW(g_hwnd, WM_KEYDOWN, (WPARAM)'A', 0);
        Sleep(60);  /* give the pump time to dispatch/for the swallow */
        int key_blocked = !g_sawKeydown;
        DWORD xbtn = 0;
        DWORD xhr = ProbeXInput(&xbtn);
        fprintf(g_log,
                "suffix=%s t=%lu me:%d key_blocked:%d fg=%p target=%p "
                "xstate:%lu xbtn:%u\n",
                g_suffix, now, (int)(fg == g_hwnd), key_blocked,
                (void*)fg, (void*)g_hwnd, xhr, xbtn);
        fflush(g_log);
      }
    }
    Sleep(10);
  }

  if (g_log)
    fclose(g_log);
  return 0;
}