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
#include <stdio.h>

static HWND g_hwnd = 0;
static FILE* g_log = 0;
static char g_suffix[16] = "";
static volatile LONG g_sawKeydown = 0; ///< Set when WndProc actually receives WM_KEYDOWN.

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
        fprintf(g_log,
                "suffix=%s t=%lu me:%d key_blocked:%d fg=%p target=%p\n",
                g_suffix, now, (int)(fg == g_hwnd), key_blocked,
                (void*)fg, (void*)g_hwnd);
        fflush(g_log);
      }
    }
    Sleep(10);
  }

  if (g_log)
    fclose(g_log);
  return 0;
}