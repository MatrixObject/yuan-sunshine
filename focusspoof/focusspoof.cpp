/**
 * @file focusspoof/focusspoof.cpp
 * @brief Injected into the game process: keeps a designated window
 *        "focused" without ever taking the real foreground, so the game
 *        keeps rendering and consuming XInput gamepad input even when
 *        another window has the keyboard focus.
 *
 * Build (MinGW-w64/MSYS2; UCRT64 for the 64-bit DLL, MINGW32 for the
 * 32-bit one):
 *   g++ -shared -O2 -s focusspoof.cpp -o focusspoof.dll \
 *       -static -lws2_32 -lole32 -loleaut32
 *   32-bit (WOW64 games such as Doom 3 BFG):
 *     g++ -m32 -shared -O2 -s focusspoof.cpp -o focusspoof32.dll \
 *         -static -Wl,--kill-at -lws2_32 -lole32 -loleaut32
 *   -static is REQUIRED, not cosmetic: without it the DLL hard-imports
 *   MinGW runtime DLLs (libstdc++-6.dll via new(std::nothrow) in the WMI
 *   shim, and - once libstdc++ is static - libwinpthread-1.dll). Neither
 *   ships with the game or the controller, so the game process's
 *   LoadLibraryW fails (or binds a foreign copy found in the game folder):
 *   injection silently does nothing for every user whose machine is not
 *   the build machine. -static folds libgcc, libstdc++ and libwinpthread
 *   into the DLL so it imports only system DLLs (KERNEL32/USER32/WS2_32/
 *   ole32/OLEAUT32 plus the UCRT api-sets). --kill-at drops the i686 '@0'
 *   stdcall decoration so GetProcAddress("SpoofStart") resolves.
 *
 * Settings all come from the controller's config file, never the registry:
 *   focus_options.txt next to this DLL (the controller ships it in the
 *   same directory and writes it there)
 * each line "Name=Number". Missing keys fall back to the DLL defaults
 * listed here, so a bare injection follows the same behavior as the UI.
 *
 * Per-game overrides: when focus_options.txt next to this DLL contains a
 * line "OptionsFile=<absolute path>", this DLL reads that file instead for
 * every setting. The pointer is captured once at DLL attach (concurrent
 * games each lock their own options file, so simultaneous injections can
 * never overwrite each other's focus options). GameArchives writes the
 * pointer right before each injection and materialises the per-game file
 * next to the DLL as focus_options_<id>.txt. Without the pointer the DLL
 * keeps reading the single shared file as before.
 *
 * The option switches (mirroring the WPF controller's focus-spoof panel):
 *   SuspendThreadsOnPatch (default 1) - stop the world while entry bytes
 *                                     are patched.
 *   EnableSubclass        (default 1) - subclass the target window; powers
 *                                     both the activation spoofing and the
 *                                     WM_INPUT drop.
 *   BlockPollingApis      (default 1) - GetKeyState / GetAsyncKeyState /
 *                                     GetKeyboardState report nothing
 *                                     pressed.
 *   BlockWmInput          (default 1) - real WM_INPUT is dropped in the
 *                                     subclassed wndproc (needs
 *                                     EnableSubclass).
 *   BlockRawInputApis     (default 1) - GetRawInputData /
 *                                     GetRawInputBuffer answer no data
 *                                     (no subclass needed).
 *   BlockCursorHide       (default 1) - arms the ShowCursor pass-through
 *                                     control-point hook (blocking a hide
 *                                     dead-loops the game).
 *   BlockCursorLock       (default 1) - ClipCursor confinement is refused,
 *                                     GetClipCursor reports the full
 *                                     virtual screen.
 *   XInputRewrite         (default 0) - serve XInputGetState and the
 *                                     extended XInputGetStateEx (by name or
 *                                     ordinal 100/103 when the module exports
 *                                     it) from loopback UDP snapshots pushed
 *                                     by the host; the virtual pad stays
 *                                     attached, so only the state read is
 *                                     overridden; XInputSetState is hooked as
 *                                     a pure pass-through to keep ViGEm
 *                                     rumble feedback. The host neutralizes
 *                                     the virtual pad while delivery is armed,
 *                                     so only this process receives the
 *                                     controls.
 *   XInputUdpMode         (default 0) - informational flag kept for
 *                                     compatibility; both modes listen on the
 *                                     fixed XInputUdpPort (no PID-derived
 *                                     destination).
 *   XInputUdpPort         (default 45690) - loopback port for the snapshot stream.
 *   XInputFakePad         (default 0) - hook the XInput presence APIs as well
 *                                     (GetCapabilities / GetBatteryInformation /
 *                                     GetKeystroke / GetState / GetStateEx /
 *                                     SetState) so the game sees ONE connected,
 *                                     idle Xbox-style pad on slot 0 even when
 *                                     no controller exists anywhere behind the
 *                                     OS (no physical pad, no virtual pad, no
 *                                     host). A real device always wins wherever
 *                                     the OS reports one; the fake only
 *                                     materializes when the OS says there is no
 *                                     controller. Works standalone or combined
 *                                     with XInputRewrite, where it also keeps a
 *                                     stale UDP delivery from blanking out to
 *                                     "disconnected". The same flag also arms
 *                                     the device-presence illusion: the
 *                                     DirectInput enumeration reports a wired
 *                                     Xbox 360 pad (name, product GUID
 *                                     VID 045E / PID 028E) and the raw-input/
 *                                     HID tree reports one RIM_TYPEHID device
 *                                     with the same identity, and the hot-plug
 *                                     nudges post DBT_DEVICECHANGE plus
 *                                     DBT_DEVICEARRIVAL with HID/USB interface
 *                                     payloads, so an engine that rescans on
 *                                     hot-plug finds a controller to bind
 *                                     XInput against.
 *   XInputFakeRescan      (default 1) - use the reference-style two-stage
 *                                     activation driver instead of the legacy
 *                                     immediate arm + fixed hot-plug nudges.
 *                                     The driver waits until the game is idle
 *                                     (XInputFakeIdleProbes consecutive fast
 *                                     probes answered within XInputFakeIdleMaxMs),
 *                                     arms the presentation fakes, posts the
 *                                     DBT_DEVNODES_CHANGED rescan sequence, then
 *                                     watches for WMI Win32_PNPEntity adoption
 *                                     (success) or a DirectInput-only bind
 *                                     (retract, rotate the device identity and
 *                                     retry, up to XInputFakeRetries times).
 *                                     With 0 the fakes arm immediately at
 *                                     install time and the legacy
 *                                     HotPlugNudgeThread posts the fixed
 *                                     nudge rounds.
 *   XInputFakeIdleProbes  (default 120) - consecutive fast idle probes that
 *                                     must be answered before arming.
 *   XInputFakeIdleMaxMs   (default 30000) - deadline for the idle probe
 *                                     window; the driver arms anyway once the
 *                                     probe count is reached.
 *   XInputFakeRetries     (default 8) - maximum identity rotations in the
 *                                     retry loop (1..16).
 *   XInputFakeWgi         (default 0) - present the fake pad through
 *                                     Windows.Gaming.Input as well: UE5
 *                                     titles (Split Fiction among them)
 *                                     read the controller through the
 *                                     WinRT Gamepad statics instead of
 *                                     XInput, so the XInput-side hooks
 *                                     never fire for them. The layer
 *                                     detours combase!RoGetActivationFactory
 *                                     for Windows.Gaming.Input.Gamepad and
 *                                     serves a fake statics object whose
 *                                     get_Gamepads passes the real system
 *                                     list through whenever the OS has a
 *                                     controller, and otherwise presents
 *                                     one connected fake pad fed from the
 *                                     same UDP snapshots as the XInput
 *                                     rewrite. Not armed-gated, exactly
 *                                     like the XInput serve family: a
 *                                     game enumerates the pad once at
 *                                     startup and never looks again.
 *   DisableFocusSpoof     (default 0) - load the DLL but do nothing.
 *
 * How it works, all in-process, on a delayed worker thread (loader-lock
 * safe):
 *   1. Finds the game's main window (largest visible top-level window of
 *      this process).
 *   2. Installs an inline detour (trampoline) on user32!GetForegroundWindow,
 *      user32!GetActiveWindow and user32!GetFocus. Every call site -
 *      whether it reaches the API through the import table, through a PLT
 *      thunk, or through a register-cached pointer the compiler hoisted
 *      loop-invariantly - physically enters the patched function entry, so
 *      it is caught no matter how the engine was built. The original
 *      prologues are relocated to per-process trampolines (allocated
 *      PAGE_READWRITE, then hardened to PAGE_EXECUTE_READ) and the hooks
 *      call those for the fall-through path. The entry bytes are only ever
 *      written while every other thread in the process is suspended (each
 *      thread id recorded and resumed exactly), which matches the
 *      stop-the-world guarantee Detours/EasyHook provide.
 *   3. Optionally blocks keyboard/mouse input for the game so it only
 *      renders and keeps accepting XInput gamepad input. The gamepad
 *      (XInput) polling is never touched. Blocking is a single install,
 *      armed by per-layer flag switches, never by re-patching. It is
 *      implemented ONLY at the API layer, exactly like the capture-hook
 *      reference - the window procedure is never used to block input, so
 *      the game window stays a completely normal, movable, resizable
 *      host-desktop window:
 *        a. Window-procedure layer: SwallowProc (installed by the subclass
 *           when EnableSubclass=1, DEFAULT ON) is a line-for-line mirror of
 *           the battle-tested capture-hook reference
 *           (Capture.Hook.WindowSubClass WindowSubClass.cs) with the SAME
 *           message set and NOTHING else: WM_INPUT (real, wParam !=
 *           SRI_WPARAM, dropped while blocking), WM_ACTIVATE /
 *           WM_ACTIVATEAPP / WM_NCACTIVATE forced active, WM_MOUSEACTIVATE
 *           forwarded with the target handle, WM_MOUSELEAVE / WM_KILLFOCUS
 *           acknowledged, WM_SIZE / WM_MOUSEHOVER / WM_NCHITTEST pass
 *           through. The reference handles NO legacy mouse/keyboard button
 *           messages, no WM_SETCURSOR, no non-client button messages - and
 *           neither do we; every other message (including ALL legacy
 *           key/mouse messages) is forwarded untouched by the single
 *           CallWindowProcW, so the OS keeps full window management
 *           (dragging, resizing, closing). Only the SINGLE main window is
 *           subclassed; nothing else - no child-window or
 *           message-only-window subclassing, no periodic re-scan (both of
 *           those wedged RE-engine titles). Never uses GetMessageW /
 *           PeekMessageW detours.
 *        b. Real raw input is NEVER forwarded to the engine's raw path: field
 *           evidence proved a WGC-captured RE-engine title wedges its UI
 *           thread within ~1 second of receiving real raw input (its raw
 *           handler + capture present path deadlock; the next physical
 *           click then pops the Windows "not responding" force-close
 *           dialog). So while blocking, MyGetRawInputData /
 *           MyGetRawInputBuffer answer real handles with no data (size
 *           probes honored per the SDK, so no mis-sized heap allocations /
 *           "Heap allocation failed" dialog), exactly like the reference's
 *           GetRawInputDataHook/GetRawInputBufferHook.
 *        c. Polled state APIs: while blocking, GetAsyncKeyState /
 *           GetKeyState / GetKeyboardState report nothing pressed - the
 *           keyboard and mouse are simply disabled for the game. Not
 *           blocking forwards the real state untouched.
 *      The hooks are only installed when their explicit switch
 *      ("BlockPollingApis" / "BlockWmInput" / "BlockRawInputApis", all
 *      default 0) is set, so a fresh install stays on the
 *      previously-known-good baseline.
 *   4. Cursor hooks. The GAME owns cursor visibility: ShowCursor is hooked
 *      as a strict pass-through (the reference's ShowCursorHook always
 *      calls the original). Interfering with a hide - either rewriting
 *      ShowCursor(FALSE) into TRUE or swallowing it - desyncs the game's
 *      display-count belief from the real count and dead-loops games whose
 *      show path is "while (ShowCursor(TRUE) != 0) {}", so "BlockCursorHide"
 *      (default 1) only arms the pass-through hook as the config's control
 *      point. Cursor CONFINEMENT is refused ("BlockCursorLock", default 1):
 *        a. ClipCursor / GetClipCursor: a game request to confine the cursor
 *           to a region is refused, and any read-back reports the full
 *           virtual screen, so FPS-style center-locking never traps the
 *           remote cursor.
 *      These are separate from keyboard/mouse input blocking, so each title
 *      can enable only the pieces it needs.
 *
 * No SetForegroundWindow is ever called and the target window is never
 * pushed to the front of the z-order.
 *
 * Exported API (extern "C"):
 *   SpoofStart(void) -> BOOL
 *   SpoofStop (void) -> BOOL
 *
 * Risk note: modifies a game process (window subclass + inline detours).
 * Use at your own risk on titles with anti-cheat.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <xinput.h>
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <oleauto.h>
#include <oaidl.h>
#include <wbemidl.h>
#include <cstdint>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>
#include <string.h>

/* The tag the capture-hook reference uses as the wParam of its own
   simulated raw input packets (its dev-input "simulated raw input"
   protocol): WM_INPUT messages whose wParam != SRI_WPARAM are real input
   and are swallowed while blocking. A direct-play client never posts
   SRI_WPARAM-tagged packets, so every WM_INPUT this window sees while
   blocking is real and is dropped; the forwarding branch is kept only to
   mirror the reference's protocol exactly. */
#define SRI_WPARAM 21760

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static HWND g_target;            ///< The window we keep looking "focused".
static WNDPROC g_prev;           ///< Original wndproc of g_target.

static volatile LONG g_state = 0;/* 0 = off, 1 = on */
static volatile LONG g_stop = 0; /* 1 = worker must bail out */

/* Diagnostic: number of WM_INPUT messages that reached THIS subclassed
   window. If RE9 registers raw input on the main window, this counter grows
   on every real input event even though the engine's GetRawInputData reads
   are zeroed; a counter that stays 0 while the game still freezes means the
   raw input is landing on ANOTHER window (child render surface or
   RIDEV_INPUTSINK), which the reference's single-window subclass never saw
   either. */
static volatile LONG64 g_wmInputSeen = 0;
/* Diagnostic: number of valid XInput UDP snapshots received from the
   Sunshine host on the loopback port. The heartbeat reports this so a test
   can prove whether client pad input keeps flowing to the rewrite while the
   pad is held (a counter stuck at 1 = only the initial idle snapshot arrived,
   i.e. the client never sent input, not a rewrite problem). */
static volatile LONG64 g_udpDatagrams = 0;
/* Diagnostic: how many times the game called the hooked GetState/GetStateEx
   and how many of those were answered from a FRESH UDP snapshot. A nonzero
   served count while the game still ignores the pad proves the game reads
   controller input somewhere other than XInputGetState (raw input /
   direct input), which no XInput-level rewrite can satisfy. */
static volatile LONG64 g_xinputGetStateCalls = 0;
static volatile LONG64 g_xinputServedFresh = 0;
/* Diagnostic: how many times the game called each remaining hooked XInput
   API after injection. Together with the first-call markers these show
   exactly which surface the title's input layer touches (presence, rumble,
   battery, keystroke) even when it never polls state. */
static volatile LONG64 g_xinputCapsCalls = 0;     ///< GetCapabilities calls.
static volatile LONG64 g_xinputCapsExCalls = 0;   ///< GetCapabilitiesEx calls.
static volatile LONG64 g_xinputSetCalls = 0;      ///< SetState calls.
static volatile LONG64 g_xinputBatteryCalls = 0;  ///< GetBatteryInformation calls.
static volatile LONG64 g_xinputKeystrokeCalls = 0; ///< GetKeystroke calls.
static volatile LONG64 g_xinputEnableCalls = 0;   ///< XInputEnable calls.
static volatile LONG64 g_xinputDsoundCalls = 0;   ///< GetDSoundAudioDeviceGuids calls.
static volatile LONG64 g_xinputAudioIdsCalls = 0; ///< GetAudioDeviceIds calls.
/* Focus-hook diagnostics. The three focus APIs are detoured so the game keeps
   believing its own window is foreground/active/focused. The spoof can only
   work if the ENGINE actually asks - a title that reads focus from raw
   WM_ACTIVATE/WM_SETFOCUS messages, from DXGI, or that simply never checks
   gets the same answer with or without the detours. These six counters prove
   which case a live title is in: "calls" counts every entry into the hooked
   API, "spoofed" counts the ones answered with g_target. calls==0 means the
   engine never consults the focus APIs at all (so any focus-related symptom
   has a different cause), while calls>0 && spoofed==0 means the hook is
   installed but g_state/g_target were not usable at call time. */
static volatile LONG64 g_fgwCalls = 0;    ///< GetForegroundWindow calls after injection.
static volatile LONG64 g_fgwSpoofed = 0;  ///< GetForegroundWindow calls answered with the target.
static volatile LONG64 g_awCalls = 0;     ///< GetActiveWindow calls after injection.
static volatile LONG64 g_awSpoofed = 0;   ///< GetActiveWindow calls answered with the target.
static volatile LONG64 g_focusCalls = 0;  ///< GetFocus calls after injection.
static volatile LONG64 g_focusSpoofed = 0; ///< GetFocus calls answered with the target.
static HANDLE g_worker = 0;      ///< The setup worker thread.
static HMODULE g_host = 0;       ///< This DLL's module handle.
/* Keyboard/mouse input is disabled for the GAME at the API layer only -
   exactly like the capture-hook reference - and the three layers are armed
   INDEPENDENTLY by the controller's option list, so each features a single
   self-explanatory toggle in the UI (no master/combination option):
   - g_blockPolls:  GetAsyncKeyState / GetKeyState / GetKeyboardState report
                    nothing pressed (the classic key/mouse polled-state cut).
   - g_blockWmInput: real WM_INPUT (wParam != SRI_WPARAM) is dropped in the
                    SwallowProc. Requires the window subclass (EnableSubclass)
                    to be active, because the drop happens in the subclassed
                    wndproc.
   - g_blockRawApis: GetRawInputData / GetRawInputBuffer answer real handles
                    with no data (size probes honored per the SDK). Works
                    without a subclass, exactly like the reference's raw-API
                    hooks.
   The window procedure stays a plain reference mirror (it never eats legacy
   messages), so the window remains a normal host-desktop window. The game
   keeps rendering and XInput gamepad input is never touched. */
static volatile LONG g_blockPolls = 0;    ///< 1 while polled key state is zeroed.
static volatile LONG g_blockWmInput = 0;  ///< 1 while real WM_INPUT is dropped.
static volatile LONG g_blockRawApis = 0;  ///< 1 while raw-API reads are zeroed.

// ===== 前向声明（用于钩子安装等函数）=====
typedef SHORT(WINAPI* FnKeyState)(int);
typedef BOOL(WINAPI* FnKeybState)(PBYTE);
typedef UINT(WINAPI* FnRawInput)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
typedef int(WINAPI* FnShowCursor)(BOOL);
typedef BOOL(WINAPI* FnClipCursor)(const RECT*);
typedef BOOL(WINAPI* FnGetClipCursor)(LPRECT);
static FnKeyState g_exportGetAsyncKeyState;
static FnKeyState g_exportGetKeyState;
static FnKeybState g_exportGetKeyboardState;
static FnRawInput g_exportGetRawInputData;
static FnShowCursor g_exportShowCursor;
static FnClipCursor g_exportClipCursor;
static FnGetClipCursor g_exportGetClipCursor;
static SHORT WINAPI MyGetAsyncKeyState(int vKey);
static SHORT WINAPI MyGetKeyState(int nVirtKey);
static BOOL WINAPI MyGetKeyboardState(PBYTE lpKeyState);
static UINT WINAPI MyGetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
static int WINAPI MyShowCursor(BOOL bShow);
static BOOL WINAPI MyClipCursor(const RECT* lpRect);
static BOOL WINAPI MyGetClipCursor(LPRECT lpRect);
static bool InputBlockEnabled(void);
static DWORD FileDword(const char* name, DWORD def);
static bool SuspendThreadsOnPatchEnabled(void);
static volatile LONG g_enableCursorHide; ///< 1 while blocking the game from hiding the cursor.
static volatile LONG g_enableCursorLock; ///< 1 while blocking the game from locking the cursor.

/* Keyboard/mouse blocking runs at two complementary layers. The raw-API
   layer (GetRawInputData/GetRawInputBuffer) is a raw-input cut that needs no
   subclass: WM_INPUT is always forwarded to the engine's registered window,
   so the handle stays valid, but every read of the raw data returns zero
   bytes. The window-procedure layer (only active when the subclass is
   installed) drops the real WM_INPUT message itself. The whole design avoids
   touching user32 entry bytes beyond the four thin detours above and avoids
   touching the message queue itself. */
// ===== 前向声明结束 =====

/* ------------------------------------------------------------------ */
/* Debug tracing (cold paths only; never inside the wndproc)           */
/* ------------------------------------------------------------------ */

static volatile LONG g_workerLog = 0; ///< 1 after worker logging is open.
static volatile LONG g_diagEvents = 0; ///< 1 when wndproc message tracing is on.

/**
 * @brief Emit a debug trace line via OutputDebugStringA.
 *
 * This is cheap (no disk I/O) so it is safe to call anywhere except in
 * high-frequency paths where a per-message trace would flood. Intended
 * for use with DebugView / cdb.
 *
 * @param fmt printf-style format string.
 */
static void Trace(const char* fmt, ...) {
  char buf[1024];
  va_list va;
  va_start(va, fmt);
  vsnprintf(buf, sizeof(buf), fmt, va);
  va_end(va);
  char full[1040];
  snprintf(full, sizeof(full), "focusspoof: %s\n", buf);
  OutputDebugStringA(full);
}

/**
 * @brief Build the absolute diagnostic log path for this process.
 *
 * All log files collect in a "logs" subdirectory next to the DLL itself, so
 * every injected process writes into one stable, predictable folder (e.g.
 * <controller dir>\logs) regardless of the host's working directory; the
 * process id is only recorded inside the file content. When the DLL module
 * path cannot be resolved the fallback is ".\logs" relative to the process
 * working directory. The directory is created on demand and logging stays
 * best-effort: a failed fopen simply skips the line.
 *
 * @param[out] path Buffer receiving the full path.
 * @param size Size of the buffer.
 * @param stem File name stem (e.g. "focusspoof_worker").
 */
static void LogPath(char* path, size_t size, const char* stem) {
  char dir[MAX_PATH];
  HMODULE hmod = NULL;
  DWORD n = 0;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCSTR) &LogPath, &hmod))
    n = GetModuleFileNameA(hmod, dir, (DWORD) sizeof(dir));
  char* slash = (n > 0 && n < sizeof(dir)) ? strrchr(dir, '\\') : NULL;
  if (!slash) {
    CreateDirectoryA(".\\logs", NULL);
    snprintf(path, size, ".\\logs\\%s.log", stem);
    return;
  }
  *slash = '\0';
  char logdir[MAX_PATH];
  snprintf(logdir, sizeof(logdir), "%s\\logs", dir);
  CreateDirectoryA(logdir, NULL);
  snprintf(path, size, "%s\\logs\\%s.log", dir, stem);
}

/**
 * @brief Truncate the worker diagnostics file so a run always starts fresh.
 *
 * Called at SpoofStartImpl entry (even when the spoof is already active, so
 * re-injecting or re-enabling into a still-running game rewrites the log
 * instead of appending) and at worker thread start. Dlog/DiagEvent then
 * append only within the current run; the file never accumulates across
 * runs.
 */
static void ResetWorkerLog(void) {
  char path[MAX_PATH];
  LogPath(path, sizeof(path), "focusspoof_worker");
  FILE* f = fopen(path, "w");
  if (f)
    fclose(f);
}

/**
 * @brief Append a line to the worker's diagnostics file.
 *
 * Only ever called on the cold worker / stop paths, never inside the
 * wndproc. Uses min-write flushes so the smoke test can read progress.
 * Every line is prefixed with the process-uptime in milliseconds so the
 * smoke test and, more importantly, a live game capture, can show exactly
 * where a hang happens (last timestamped line = the step that wedged).
 *
 * @param fmt printf-style format string.
 */
static void Dlog(const char* fmt, ...) {
  char buf[2048];
  va_list va;
  va_start(va, fmt);
  vsnprintf(buf, sizeof(buf), fmt, va);
  va_end(va);
  char path[MAX_PATH];
  LogPath(path, sizeof(path), "focusspoof_worker");
  if (g_workerLog != 1)
    return;
  FILE* f = fopen(path, "a");
  if (!f)
    return;
  fprintf(f, "[%llu] %s\n", (unsigned long long)GetTickCount64(), buf);
  fclose(f);
}

/**
 * @brief Whether a wndproc message belongs to the click/activation trace
 * set.
 *
 * Everything here maps to a real mouse click or a focus transition, so at
 * click time the trace is small (tens of lines) even when the game then
 * hangs - the last traced line names the exact message whose handler
 * wedged. High-frequency messages (WM_PAINT, WM_TIMER, WM_MOUSEMOVE,
 * engine-private ids) are deliberately excluded.
 *
 * @param m Message id.
 * @return True when the message should be traced.
 */
static bool IsDiagEvent(UINT m) {
  switch (m) {
  case WM_MOUSEACTIVATE:
  case WM_MOUSELEAVE:
  case WM_MOUSEHOVER:
  case WM_ACTIVATE:
  case WM_ACTIVATEAPP:
  case WM_NCACTIVATE:
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
  case WM_NCHITTEST:
    return true;
  default:
    return false;
  }
}

/**
 * @brief Trace one wndproc message entry while DiagFocusEvents is enabled.
 *
 * Off by default (DiagFocusEvents=1 in the settings file turns it on at
 * worker start). This deliberately writes from inside the wndproc, unlike
 * Dlog, because the click sequence being diagnosed only materialises in
 * the wndproc; the trace stays small because IsDiagEvent bounds the set.
 *
 * @param h The target window.
 * @param m Message id.
 * @param w Message wParam.
 * @param l Message lParam.
 * @param block Current keyboard/mouse block flag.
 */
static void DiagEvent(HWND h, UINT m, WPARAM w, LPARAM l, bool block) {
  if (g_diagEvents != 1)
    return;
  char path[MAX_PATH];
  LogPath(path, sizeof(path), "focusspoof_worker");
  FILE* f = fopen(path, "a");
  if (!f)
    return;
  fprintf(f,
          "[%llu] wndproc: h=%p msg=0x%04X w=%p l=%p block=%d state=%ld "
          "prev=%p\n",
          (unsigned long long)GetTickCount64(), (void*)h, (unsigned)m,
          (void*)w, (void*)l, block ? 1 : 0, g_state, (void*)g_prev);
  fclose(f);
}

/* ------------------------------------------------------------------ */
/* Hooked focus APIs (inline-detour targets)                           */
/* ------------------------------------------------------------------ */

typedef HWND(WINAPI* FnGetWindow)(void);

/**
 * @brief Resolved export entry for each focus API. Used as the detour
 * target. Constant for the process lifetime.
 */
static FnGetWindow g_exportGetForegroundWindow;
static FnGetWindow g_exportGetActiveWindow;
static FnGetWindow g_exportGetFocus;

/**
 * @brief The "real" implementation the hooks fall through to. Initially
 * the resolved export; once the inline detour is installed this is
 * redirected to the per-process trampoline so the fall-through path never
 * re-enters the hooked entry.
 */
static FnGetWindow g_realGetForegroundWindow;
static FnGetWindow g_realGetActiveWindow;
static FnGetWindow g_realGetFocus;

/**
 * @brief Hooked GetForegroundWindow: pretend the target has the
 * foreground while spoofing is active.
 *
 * Deliberately performs ZERO system calls on the hot path besides the two
 * hooks themselves: the engine calls this every frame, and any file or
 * heap activity here both contends with the engine's own heap locks and
 * looks like tampering to in-game anti-tamper (observed as a spurious
 * "Heap allocation failed" dialog and instant termination).
 *
 * @return HWND of the target window while active, otherwise the real one.
 */
static HWND WINAPI MyGetForegroundWindow(void) {
  InterlockedIncrement64(&g_fgwCalls);
  if (g_state == 1 && g_target && IsWindow(g_target)) {
    InterlockedIncrement64(&g_fgwSpoofed);
    return g_target;
  }
  return g_realGetForegroundWindow();
}

/**
 * @brief Hooked GetActiveWindow: pretend the target is the active window.
 * @return HWND of the target window while active, otherwise the real one.
 */
static HWND WINAPI MyGetActiveWindow(void) {
  InterlockedIncrement64(&g_awCalls);
  if (g_state == 1 && g_target && IsWindow(g_target)) {
    InterlockedIncrement64(&g_awSpoofed);
    return g_target;
  }
  return g_realGetActiveWindow();
}

/**
 * @brief Hooked GetFocus: pretend the target holds keyboard focus.
 * @return HWND of the target window while active, otherwise the real one.
 */
static HWND WINAPI MyGetFocus(void) {
  InterlockedIncrement64(&g_focusCalls);
  if (g_state == 1 && g_target && IsWindow(g_target)) {
    InterlockedIncrement64(&g_focusSpoofed);
    return g_target;
  }
  return g_realGetFocus();
}

/* ------------------------------------------------------------------ */
/* Inline detour (trampoline) for the focus APIs                       */
/* ------------------------------------------------------------------ */

/* Forward declarations: defined with the settings helpers further below
   but used by DetourInstall, which lives earlier in the file. */
static DWORD FileDword(const char* name, DWORD def);
static bool SuspendThreadsOnPatchEnabled(void);

/**
 * @brief Thread ids suspended by SuspendAllThreads, for exact resume.
 *
 * Grown to the full candidate count on every suspend pass. A fixed-size
 * array would silently drop every thread id beyond its capacity:
 * SuspendAllThreads suspends the whole process, so any id not stored can
 * never be resumed - RE-engine titles (RE9 observed at ~3900 threads)
 * leave thousands of threads frozen and the process dies a permanent
 * 假死 that even SpoofStop cannot recover. Kept process-lifetime.
 */
static DWORD* g_suspendTids = 0;
static int g_suspendCap = 0;
static int g_suspendCount = 0;

/**
 * @brief Temporarily stop every other thread in the process.
 *
 * Writing a multi-byte patch to a function entry that other threads may be
 * executing at that instant is a data race: a second core can decode a
 * half-written branch and crash or wedge the process. Mirroring what
 * Detours and EasyHook do, we suspend the whole process for the duration of
 * the write. The suspended thread ids are recorded so the resume pass can
 * only ever unpick exactly what this pass suspended: re-enumerating at
 * resume time would otherwise hit threads that started meanwhile, and a
 * single ResumeThread on a never-suspended thread would permanently wedge
 * it (its suspend count wraps). The record array is sized to the snapshot's
 * candidate count so a heavily threaded engine (RE9: thousands of
 * threadpool threads) is always fully resumable - the previous fixed 512
 * slots left every thread beyond the 512th permanently frozen. Once called
 * this function MUST be balanced by ResumeAllThreads().
 *
 * @return Number of threads suspended and recorded (0 if no candidate was
 *         found or the array allocation failed; only recorded ids are ever
 *         resumed, so no thread can be left suspended).
 */
static int SuspendAllThreads(void) {
  DWORD self = GetCurrentThreadId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return 0;

  int suspended = 0;
  THREADENTRY32 te;
  te.dwSize = sizeof(te);

  /* Pass 1: count the candidate threads (exactly what pass 2 suspends) so
     the record array can never overflow. */
  int total = 0;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID == GetCurrentProcessId() &&
        te.th32ThreadID != self)
      total++;
  }
  if (total <= 0) {
    CloseHandle(snap);
    return 0;
  }

  DWORD* tids = (DWORD*)malloc((size_t)total * sizeof(DWORD));
  if (!tids) {
    CloseHandle(snap);
    return 0;
  }

  /* Pass 2: suspend and record exactly what we suspend. Threads can exit
     between the two passes or refuse OpenThread; only the recorded ids are
     ever resumed, so no thread is left suspended even when the pass is not
     exhaustive (a busy engine at thousands of threads constantly spawns and
     reaps). The array is sized to pass 1's count, which is always >= what
     pass 2 actually suspends. */
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != GetCurrentProcessId() ||
        te.th32ThreadID == self)
      continue;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
    if (h) {
      /* Suspend once; ignore repeat suspend counts. */
      if (SuspendThread(h) != (DWORD)-1)
        tids[suspended++] = te.th32ThreadID;
      CloseHandle(h);
    }
  }
  CloseHandle(snap);

  if (g_suspendTids)
    free(g_suspendTids);
  g_suspendTids = tids;
  g_suspendCap = total;
  g_suspendCount = suspended;
  return suspended;
}

/**
 * @brief Resume exactly the threads SuspendAllThreads suspended.
 */
static void ResumeAllThreads(void) {
  int n = g_suspendCount;
  g_suspendCount = 0;
  for (int i = 0; i < n; i++) {
    HANDLE h =
        OpenThread(THREAD_SUSPEND_RESUME, FALSE, g_suspendTids[i]);
    if (h) {
      ResumeThread(h);
      CloseHandle(h);
    }
  }
}

/**
 * @brief Minimum number of bytes the detour patch occupies at the entry
 * of a hooked function. On x64, 12 bytes fit `mov rax, imm64; jmp rax`;
 * on x86, 7 bytes fit `mov eax, imm32; jmp eax`.
 */
#if defined(_WIN64)
#define DETOUR_MIN_PATCH 12
#else
#define DETOUR_MIN_PATCH 7
#endif

/**
 * @brief Maximum number of original prologue bytes a trampoline may
 * relocate. Real-world user32 prologues stay far below this.
 */
#define DETOUR_MAX_PATCH 32

/** @brief One decoded x86-64 instruction. */
struct DetourInsn {
  int len;      /**< Total instruction length in bytes, prefixes included. */
  int rel;      /**< 0 = none, 1 = rel8, 2 = rel32, 3 = rip-relative disp32. */
  int opoff;    /**< Byte offset of the opcode within the instruction. */
  bool isJmp;   /**< True when the instruction is an unconditional jmp. */
  bool seg;     ///< True when a segment-override prefix was seen.
  bool jcc;     ///< True for a conditional jump (0F 8x rel32 or 7x rel8).
  bool jccRel8; ///< True when jcc carries a rel8 displacement instead of rel32.
  int cc;       ///< Condition code (0..15) when jcc is true.
};

/**
 * @brief Decode the length of one x86-64 instruction.
 *
 * Covers the instruction forms that commonly appear in the prologue of a
 * Windows API export (moves, logic ops, sub/add, FF group, call/jmp, push,
 * int3 padding). Anything outside that set returns false and the caller
 * skips detouring that function.
 *
 * @param p Start of the instruction.
 * @param[out] out Decoded instruction bounds.
 * @return True on success.
 */
static bool DecodeInsn(const BYTE* p, DetourInsn* out) {
  const BYTE* q = p;
  out->seg = false;
  out->isJmp = false;
  out->jcc = false;
  out->jccRel8 = false;
  out->cc = 0;
  out->rel = 0;

  for (;;) {
    BYTE b = q[0];
    if (b >= 0x40 && b <= 0x4F)
      ; /* REX prefix: length contribution only. */
    else if (b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x66 || b == 0x67)
      ; /* lock / rep / operand-size / address-size: length contribution. */
    else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 ||
             b == 0x65)
      out->seg = true;
    else
      break;
    q++;
  }

  const int prlen = (int)(q - p);
  const BYTE op = q[0];
  out->opoff = prlen;

  /* Relative call/jmp: E8 rel32, E9 rel32, EB rel8. */
  if (op == 0xE8 || op == 0xE9 || op == 0xEB) {
    out->len = prlen + 1 + ((op == 0xEB) ? 1 : 4);
    out->rel = (op == 0xEB) ? 1 : 2;
    out->isJmp = (op != 0xE8);
    return true;
  }

  /* 0F 8x rel32: near conditional jump. hid.dll's HidP_GetCaps opens with
     `test rcx,rcx; 0F 84 ...` (a validity check that bails to a shared
     failure stub), so this form must decode or that export cannot be
     detoured. Other 0F escapes (movzx/SSE/...) are still rejected. */
  if (op == 0x0F && q[1] >= 0x80 && q[1] <= 0x8F) {
    out->len = prlen + 6;
    out->jcc = true;
    out->cc = q[1] & 0x0F;
    return true;
  }

  /* 0x70-0x7F rel8: short conditional jump. Relocated exactly like the near
     form, only the displacement width differs. The compiler emits this in
     compact validity checks, so a hid.dll (or user32) export can open with it
     on one Windows build and with the 0F 8x form on another; both must decode
     or the export silently stays unhooked. */
  if (op >= 0x70 && op <= 0x7F) {
    out->len = prlen + 2;
    out->jcc = true;
    out->jccRel8 = true;
    out->cc = op & 0x0F;
    return true;
  }
  /* 0xE3 jcxz/jrcxz: never emitted by MSVC; reject. */
  if (op == 0xE3)
    return false;

  /* Single-byte leaves. */
  if (op == 0xCC) { /* int3 padding */
    out->len = prlen + 1;
    return true;
  }
  if ((op >= 0x50 && op <= 0x5F) || op == 0xC3 || op == 0x90) {
    out->len = prlen + 1;
    return true;
  }
  if (op == 0xC2 || op == 0xCA) {
    out->len = prlen + 3;
    return true;
  }

  /* ModRM instruction (possibly a RIP-relative memory operand). */
  if (op == 0x8B || op == 0x89 || op == 0x8D || op == 0x33 || op == 0x3B ||
      op == 0x85 || op == 0x83 || op == 0x81 || op == 0x29 || op == 0x01 ||
      op == 0x03 || op == 0x39 || op == 0x2B || op == 0x0B || op == 0x21 ||
      op == 0x23 || op == 0x31 || op == 0x84 || op == 0xC7 || op == 0x88 ||
      op == 0x8A || op == 0x38 || op == 0x3A || op == 0x32) {
    const BYTE modrm = q[1];
    const int mod = (modrm >> 6) & 3;
    const int rm = modrm & 7;
    int extra = 2; /* opcode + modrm */

    if (mod != 3) {
      if (rm == 4) { /* SIB byte follows */
        extra += 1;
        const BYTE sib = q[2];
        const int base = sib & 7;
        if (mod == 1)
          extra += 1;
        else if (mod == 2)
          extra += 4;
        else if (mod == 0 && base == 5)
          extra += 4; /* disp32 with no base */
      } else if (mod == 0 && rm == 5) {
        extra += 4; /* [rip + disp32]; on x86 this is [disp32] absolute */
#if defined(_WIN64)
        if (!out->seg)
          out->rel = 3;
#endif
      } else if (mod == 1) {
        extra += 1;
      } else {
        extra += 4; /* mod == 2 */
      }
    }
    if (op == 0x83)
      extra += 1; /* group 1 imm8 */
    else if (op == 0x81 || op == 0xC7)
      extra += 4; /* group 1 imm32 / mov r/m64, imm32 */

    out->len = prlen + extra;
    return true;
  }

  /* FF group (inc/dec/call/jmp/push via ModRM). */
  if (op == 0xFF) {
    const BYTE modrm = q[1];
    const int mod = (modrm >> 6) & 3;
    const int rm = modrm & 7;
    const int reg = (modrm >> 3) & 7;
    int extra = 2;
    if (mod != 3) {
      if (rm == 4) {
        extra += 1;
        const BYTE sib = q[2];
        const int base = sib & 7;
        if (mod == 1)
          extra += 1;
        else if (mod == 2)
          extra += 4;
        else if (mod == 0 && base == 5)
          extra += 4;
      } else if (mod == 0 && rm == 5) {
        extra += 4; /* [rip + disp32]; on x86 this is [disp32] absolute */
#if defined(_WIN64)
        if (!out->seg)
          out->rel = 3;
#endif
      } else if (mod == 1) {
        extra += 1;
      } else {
        extra += 4;
      }
    }
    out->len = prlen + extra;
    out->isJmp = (reg == 4);
    return true;
  }

  return false;
}

/**
 * @brief A single relocated instruction inside a trampoline.
 */
struct RelocatedInsn {
  BYTE bytes[32]; /**< Relocated encoding. */
  int len;        /**< Bytes used. */
};

/**
 * @brief Relocate one original instruction into its trampoline encoding.
 *
 * Direct relative call/jmp (rel8/rel32) is rewritten as
 * `mov rax, <target>; <op> rax`, which preserves the original op semantics
 * independent of the trampoline's position. A near conditional jump is
 * rewritten as a short inverted branch over a register-free absolute jump to
 * its target. A `jmp/call [rip+disp32]`
 * indirection is rewritten as `mov rax, <slot>; <op> [rax]`, preserving
 * the memory indirection (the slot address, not the pointer value, is
 * loaded). Any other rip-relative memory operand (a `mov eax,[rip+disp32]`
 * prologue load, for example) is rewritten the same way, keeping the opcode
 * prefix bytes and switching the ModRM r/m term to [rax]. Everything else is
 * copied verbatim (register/base-pointer moves carry no relative addressing
 * and stay position-independent).
 *
 * @param src Original instruction bytes.
 * @param insn Decoded instruction.
 * @param[out] out Result buffer.
 * @return True on success.
 */
static bool RelocateInsn(const BYTE* src, const DetourInsn* insn,
                         RelocatedInsn* out) {
  int n = 0;

  /* Conditional jump (0F 8x rel32 or 7x rel8): the target is an arbitrary
     address, so the branch must keep working from the trampoline's arbitrary
     address. Invert the condition into a SHORT (2-byte `7x cb`) jump that skips
     a register- and flag-free absolute jump to the target. There is no
     `0F 8x rel8` encoding: emitting `0F 8x <one byte>` makes the CPU decode
     that byte plus the following stub bytes as a rel32 near branch and jump
     to a garbage address (observed as the first real hid!HidP_GetCaps call in
     the game jumping into the heap and killing the process). Taken -> the
     inverted short test is not taken and the absolute jump runs; not taken ->
     the short branch skips the absolute jump. */
  if (insn->jcc) {
    const int32_t rel = insn->jccRel8 ? (int32_t)(int8_t)src[insn->len - 1]
                                      : *(int32_t*)(src + insn->len - 4);
    const uintptr_t target = (uintptr_t)(src + insn->len) + rel;
    const int at = n;
    out->bytes[n++] = (BYTE)(0x70 | (insn->cc ^ 1)); /* inverted, short form */
    out->bytes[n++] = 0x00;                          /* rel8, patched below */
#if defined(_WIN64)
    out->bytes[n++] = 0xFF; /* jmp qword ptr [rip+0] */
    out->bytes[n++] = 0x25;
    out->bytes[n++] = 0x00;
    out->bytes[n++] = 0x00;
    out->bytes[n++] = 0x00;
    out->bytes[n++] = 0x00;
    memcpy(out->bytes + n, &target, sizeof(target));
    n += (int)sizeof(target);
#else
    out->bytes[n++] = 0x68; /* push imm32; ret nets the stack to zero */
    memcpy(out->bytes + n, &target, sizeof(target));
    n += (int)sizeof(target);
    out->bytes[n++] = 0xC3;
#endif
    out->bytes[at + 1] = (BYTE)(n - (at + 2)); /* rel8 base is the 2-byte jcc */
    out->len = n;
    return true;
  }

  if (insn->rel == 0) {
    /* Position-independent: copy as-is. */
    memcpy(out->bytes, src, (size_t)insn->len);
    out->len = insn->len;
    return true;
  }

  const BYTE* opc = src + insn->opoff;

  /* Direct relative call/jmp: mov rax/rax, target; jmp/call rax|eax. */
  if (insn->rel == 1 || insn->rel == 2) {
    const int64_t imm = (insn->rel == 1) ? (int64_t)(int8_t)opc[1]
                                         : *(int32_t*)(opc + 1);
    const uintptr_t target = (uintptr_t)(src + insn->len) + imm;

#if defined(_WIN64)
    out->bytes[n++] = 0x48; /* mov rax, imm64 */
    out->bytes[n++] = 0xB8;
#else
    out->bytes[n++] = 0xB8; /* mov eax, imm32 */
#endif
    memcpy(out->bytes + n, &target, sizeof(target));
    n += (int)sizeof(target);
    out->bytes[n++] = 0xFF;
    out->bytes[n++] = insn->isJmp ? 0xE0 : 0xD0; /* jmp/call rax|eax */
    out->len = n;
    return true;
  }

#if defined(_WIN64)
  /* [rip + disp32]: mov rax, <slot address>; <op> [rax]. */
  const int32_t disp = *(int32_t*)(src + insn->len - 4);
  const uintptr_t slot = (uintptr_t)(src + insn->len) + disp;

  out->bytes[n++] = 0x48;
  out->bytes[n++] = 0xB8;
  memcpy(out->bytes + n, &slot, sizeof(slot));
  n += (int)sizeof(slot);

  if (*opc == 0xFF && !insn->seg) {
    const int reg = (opc[1] >> 3) & 7;
    if (reg == 2 || reg == 4) {
      /* call [rax] / jmp [rax] */
      out->bytes[n++] = 0xFF;
      out->bytes[n++] = (reg == 4) ? 0x20 : 0x10;
      out->len = n;
      return true;
    }
    /* Other FF /r (inc/dec/push) over a rip-relative operand: unsupported. */
    return false;
  }

  /* Generic rip-relative memory operand: re-emit every prefix and opcode
     byte (REX.W included - dropping it silently downgrades a 64-bit load
     such as `mov rax,[rip+__security_cookie]` to a 32-bit one, and the
     caller's security-cookie check then fast-fails), then rewrite the ModRM
     so the r/m term is [rax]. Only the no-immediate forms are supported: for
     83 /r imm8, 81 /r imm32 and C7 /0 imm32 the immediate follows the
     disp32, so the ModRM byte is not at len-5 and they are rejected. */
  if (insn->len - insn->opoff >= 6 && !insn->seg) {
    const BYTE opcFirst = src[insn->opoff];
    if (opcFirst == 0x83 || opcFirst == 0x81 || opcFirst == 0xC7)
      return false;
    const BYTE modrm = src[insn->len - 5];
    if ((modrm & 0xC7) == 0x05) { /* mod == 00, rm == 101 */
      /* rax is the scratch register. Refuse instructions that also READ rax
         as their other operand: `mov rax,[rip]` and `lea r64,[rip]` only
         write the reg field, but a store or an ALU op with reg==rax (for
         example `add rax,[rip]`) would read the clobbered scratch instead of
         the original value. */
      const int reg = (modrm >> 3) & 7;
      if (reg == 0 && opcFirst != 0x8B && opcFirst != 0x8D)
        return false;
      const int headLen = insn->len - 5; /* prefixes + opcode, before ModRM */
      const int rexIdx = insn->opoff - 1;
      memcpy(out->bytes + n, src, (size_t)headLen);
      if (rexIdx >= 0 && src[rexIdx] >= 0x40 && src[rexIdx] <= 0x4F)
        out->bytes[n + rexIdx] &= (BYTE)0xFE; /* clear REX.B: base is rax */
      n += headLen;
      out->bytes[n++] = (BYTE)((modrm >> 3) << 3); /* r/m := [rax] */
      out->len = n;
      return true;
    }
  }
#endif

  /* Conservative: generic relative-address rewrite is not implemented. */
  return false;
}

/**
 * @brief One installed detour (per hooked API).
 */
struct Detour {
  BYTE* entry;      /**< Export entry being patched. */
  BYTE saved[DETOUR_MAX_PATCH]; /**< Original bytes. */
  int savedLen;     /**< Bytes actually replaced. */
  BYTE* tramp;      /**< Trampoline code (EXECUTE_READWRITE). */
  bool active;      /**< True while installed. */
};

/**
 * @brief Install a 12-byte `mov rax, hook; jmp rax` detour at a function
 * entry, relocating the overwritten prologue into a trampoline.
 *
 * @param entry Export address to hook.
 * @param hook Replacement implementation.
 * @param[out] realOut Real implementation to call for the fall-through path.
 * @param[out] det Detour record filled in on success.
 * @return True when the detour is live.
 */
static bool DetourInstall(void* entry, void* hook, void** realOut,
                          Detour* det) {
  int off = 0;
  DetourInsn insn;
  RelocatedInsn rel;

  BYTE trampBuf[DETOUR_MAX_PATCH * 8];
  int tOff = 0;

  while (off < DETOUR_MAX_PATCH) {
    if (!DecodeInsn((BYTE*)entry + off, &insn)) {
      Dlog("detour %p: decode failed at byte %d", (void*)entry, off);
      break; /* unsupported: stop copying, bail below. */
    }
    if (!RelocateInsn((BYTE*)entry + off, &insn, &rel)) {
      Dlog("detour %p: relocate failed at byte %d", (void*)entry, off);
      break;
    }
    if (tOff + rel.len > (int)sizeof(trampBuf))
      break;
    memcpy(trampBuf + tOff, rel.bytes, (size_t)rel.len);
    tOff += rel.len;
    off += insn.len;
    if (off >= DETOUR_MIN_PATCH)
      break;
  }
  if (off < DETOUR_MIN_PATCH) {
    Dlog("detour %p: could not cover %d bytes", (void*)entry, off);
    return false; /* could not cover enough bytes */
  }
  if (tOff + 16 > (int)sizeof(trampBuf)) {
    Dlog("detour %p: trampoline buffer overflow", (void*)entry);
    return false;
  }

  /* Append a register- and flag-free jump to the first unpatched original
     byte. `mov rax, cont; jmp rax` would clobber rax, which is live across
     the boundary whenever the covered prologue ends by loading a value into
     rax - the classic stack-cookie prologue
     (`mov rax, __security_cookie; xor rax, rsp; mov [rsp+58h], rax`) does
     exactly that, and clobbering it makes the function's security check
     fast-fail with STATUS_STACK_BUFFER_OVERRUN. x86-64 uses an indirect
     `jmp [rip+0]` over an inline pointer; x86 uses `push imm32; ret`, whose
     transient stack use nets to zero and leaves every GPR intact. */
  const uintptr_t cont = (uintptr_t)entry + off;
#if defined(_WIN64)
  memcpy(trampBuf + tOff, "\xFF\x25\x00\x00\x00\x00", 6); /* jmp [rip+0] */
  tOff += 6;
  memcpy(trampBuf + tOff, &cont, sizeof(cont));
  tOff += (int)sizeof(cont);
#else
  trampBuf[tOff++] = 0x68; /* push imm32 */
  memcpy(trampBuf + tOff, &cont, sizeof(cont));
  tOff += (int)sizeof(cont);
  trampBuf[tOff++] = 0xC3; /* ret */
#endif

  /* Executable trampoline: write page as RW, then harden to RX. */
  BYTE* tramp = (BYTE*)VirtualAlloc(NULL, (SIZE_T)tOff, MEM_COMMIT,
                                    PAGE_READWRITE);
  if (!tramp) {
    Dlog("detour %p: VirtualAlloc failed %lu", (void*)entry,
         GetLastError());
    return false;
  }
  memcpy(tramp, trampBuf, (size_t)tOff);
  DWORD tp = 0;
  if (!VirtualProtect(tramp, (SIZE_T)tOff, PAGE_EXECUTE_READ, &tp)) {
    VirtualFree(tramp, 0, MEM_RELEASE);
    Dlog("detour %p: trampoline protect failed %lu", (void*)entry,
         GetLastError());
    return false;
  }

  /* Save original bytes, then patch the entry. */
  memset(det, 0, sizeof(*det));
  det->entry = (BYTE*)entry;
  det->savedLen = off;
  det->tramp = tramp;
  memcpy(det->saved, (BYTE*)entry, (size_t)off);

  BYTE patch[DETOUR_MIN_PATCH];
  int n = 0;
#if defined(_WIN64)
  patch[n++] = 0x48; /* mov rax, imm64 */
  patch[n++] = 0xB8;
#else
  patch[n++] = 0xB8; /* mov eax, imm32 */
#endif
  memcpy(patch + n, &hook, sizeof(hook));
  n += (int)sizeof(hook);
  patch[n++] = 0xFF; /* jmp rax|eax */
  patch[n++] = 0xE0;

  DWORD oldProt = 0;
  if (!VirtualProtect((LPVOID)entry, DETOUR_MIN_PATCH, PAGE_EXECUTE_READWRITE,
                      &oldProt)) {
    VirtualFree(tramp, 0, MEM_RELEASE);
    Dlog("detour %p: entry protect failed %lu", (void*)entry,
         GetLastError());
    return false;
  }

  /* Publish the trampoline and mark the record live BEFORE the entry is
     patched. Installing a detour on a function the stop-the-world machinery
     itself calls (kernel32!CloseHandle is called by Suspend/ResumeAllThreads
     to close the thread handles) otherwise re-enters the fresh hook during
     ResumeAllThreads while the fall-through pointer is still null. */
  det->active = true;
  *realOut = tramp;

  /* Stop the world so no thread can execute a half-written entry (unless
     the user opted out of suspending threads for this title). */
  bool suspend = SuspendThreadsOnPatchEnabled();
  int frozen = suspend ? SuspendAllThreads() : 0;
  memcpy((void*)entry, patch, DETOUR_MIN_PATCH);
  FlushInstructionCache(GetCurrentProcess(), (LPCVOID)entry,
                        DETOUR_MIN_PATCH);
  if (suspend)
    ResumeAllThreads();

  VirtualProtect((LPVOID)entry, DETOUR_MIN_PATCH, oldProt, &oldProt);

  Dlog("detour: %p hook installed (covered %d bytes, tramp %p, %d frozen)",
       (void*)entry, off, (void*)tramp, frozen);
  return true;
}

/**
 * @brief Restore a previously installed detour (restores the original
 * entry bytes). The trampoline is deliberately NOT released here.
 *
 * A busy engine (RE-engine titles in particular) calls the hooked focus
 * APIs every frame, so the entry-bytes write must be guarded by the same
 * stop-the-world SuspendAllThreads as the install: a second core decoding
 * a half-written branch while the game is running at 60 fps tears the
 * restore, and the game can never recover (SpoofStop appears to "do
 * nothing"). The trampoline is left allocated for the process lifetime:
 * freeing it the instant the entry is restored races with a thread that
 * entered the trampoline a few nanoseconds earlier and would then execute
 * a freed page. The leak is a handful of pages per API per stop/start
 * cycle, which is acceptable versus a crash.
 *
 * @param det Detour record.
 * @param real Where the fall-through pointer lives; reset to the export.
 * @param entry The export address.
 */
static void DetourRestore(Detour* det, void** real, void* entry) {
  if (!det->active)
    return;
  DWORD oldProt = 0;
  bool suspend = SuspendThreadsOnPatchEnabled();
  int frozen = suspend ? SuspendAllThreads() : 0;
  if (VirtualProtect(det->entry, det->savedLen, PAGE_EXECUTE_READWRITE,
                     &oldProt)) {
    memcpy(det->entry, det->saved, (size_t)det->savedLen);
    FlushInstructionCache(GetCurrentProcess(), det->entry,
                          (size_t)det->savedLen);
    VirtualProtect(det->entry, det->savedLen, oldProt, &oldProt);
  }
  if (suspend)
    ResumeAllThreads();
  /* Trampoline intentionally leaked; see the function doc comment. */
  det->active = false;
  *real = entry;
  Dlog("restore: %p unhooked (%d bytes, tramp %p kept, %d frozen)",
       (void*)entry, det->savedLen, (void*)det->tramp, frozen);
}

static Detour g_detourFgw; ///< Detour record for GetForegroundWindow.
static Detour g_detourAsw; ///< Detour record for GetActiveWindow.
static Detour g_detourFoc; ///< Detour record for GetFocus.

/**
 * @brief Install inline detours on the three focus APIs. On success the
 * fall-through pointers (g_real*) are redirected to the trampolines so
 * the hooks never re-enter the patched entry.
 */
static void DetourHookInstall(void) {
  bool ok1 = DetourInstall((void*)g_exportGetForegroundWindow,
                           (void*)MyGetForegroundWindow,
                           (void**)&g_realGetForegroundWindow, &g_detourFgw);
  if (ok1)
    Dlog("detour: GetForegroundWindow -> trampoline %p",
         (void*)g_realGetForegroundWindow);
  else
    Trace("detour GetForegroundWindow: skipped");

  bool ok2 = DetourInstall((void*)g_exportGetActiveWindow,
                           (void*)MyGetActiveWindow,
                           (void**)&g_realGetActiveWindow, &g_detourAsw);
  if (ok2)
    Dlog("detour: GetActiveWindow -> trampoline %p",
         (void*)g_realGetActiveWindow);
  else
    Trace("detour GetActiveWindow: skipped");

  bool ok3 = DetourInstall((void*)g_exportGetFocus, (void*)MyGetFocus,
                           (void**)&g_realGetFocus, &g_detourFoc);
  if (ok3)
    Dlog("detour: GetFocus -> trampoline %p", (void*)g_realGetFocus);
  else
    Trace("detour GetFocus: skipped");
}

/**
 * @brief Restore all inline detours and reset the fall-through pointers.
 */
static void DetourHookRestore(void) {
  DetourRestore(&g_detourFgw, (void**)&g_realGetForegroundWindow,
                (void*)g_exportGetForegroundWindow);
  DetourRestore(&g_detourAsw, (void**)&g_realGetActiveWindow,
                (void*)g_exportGetActiveWindow);
  DetourRestore(&g_detourFoc, (void**)&g_realGetFocus, (void*)g_exportGetFocus);
}

/* ------------------------------------------------------------------ */
/* Keyboard / mouse input blocking                                      */
/* ------------------------------------------------------------------ */

typedef SHORT(WINAPI* FnKeyState)(int);
typedef BOOL(WINAPI* FnKeybState)(PBYTE);
typedef UINT(WINAPI* FnRawBuf)(PRAWINPUT, PUINT, UINT);
typedef UINT(WINAPI* FnRawList)(PRAWINPUTDEVICELIST, PUINT, UINT);
typedef UINT(WINAPI* FnRawInfo)(HANDLE, UINT, LPVOID, PUINT);
typedef BOOL(WINAPI* FnRawReg)(PCRAWINPUTDEVICE, UINT, UINT);
static FnKeyState g_realGetAsyncKeyState; ///< GetAsyncKeyState fall-through.
static FnKeyState g_realGetKeyState; ///< GetKeyState fall-through.
static FnKeybState g_realGetKeyboardState; ///< GetKeyboardState fall-through.
static FnRawInput g_realGetRawInputData; ///< GetRawInputData fall-through.
static FnRawBuf g_realGetRawInputBuffer; ///< GetRawInputBuffer fall-through.

static Detour g_detourAsk; ///< Detour record for GetAsyncKeyState.
static Detour g_detourKst; ///< Detour record for GetKeyState.
static Detour g_detourKb;  ///< Detour record for GetKeyboardState.
static Detour g_detourRin; ///< Detour record for GetRawInputData.
static Detour g_detourRbuf; ///< Detour record for GetRawInputBuffer.

static bool SubClassInstall(void);

static FnRawBuf g_exportGetRawInputBuffer; ///< GetRawInputBuffer export.
/* Raw-input PnP device-presentation layer (see the fake-HID section): the
   exports are resolved in DllMain and the fall-through trampolines are filled
   by DetectRawFakeInstall(). */
static FnRawList g_exportGetRawInputDeviceList;    ///< user32!GetRawInputDeviceList export.
static FnRawInfo g_exportGetRawInputDeviceInfoW;   ///< user32!GetRawInputDeviceInfoW export.
static FnRawInfo g_exportGetRawInputDeviceInfoA;   ///< user32!GetRawInputDeviceInfoA export.
static FnRawReg g_exportRegisterRawInputDevices;   ///< user32!RegisterRawInputDevices export.
static FnRawList g_realGetRawInputDeviceList;      ///< GetRawInputDeviceList fall-through.
static FnRawInfo g_realGetRawInputDeviceInfoW;     ///< GetRawInputDeviceInfoW fall-through.
static FnRawInfo g_realGetRawInputDeviceInfoA;     ///< GetRawInputDeviceInfoA fall-through.
static FnRawReg g_realRegisterRawInputDevices;     ///< RegisterRawInputDevices fall-through.
static Detour g_detourRawList;   ///< Detour record for GetRawInputDeviceList.
static Detour g_detourRawInfoW;  ///< Detour record for GetRawInputDeviceInfoW.
static Detour g_detourRawInfoA;  ///< Detour record for GetRawInputDeviceInfoA.
static Detour g_detourRawReg;    ///< Detour record for RegisterRawInputDevices.
static void RawFakeInstall(void); ///< Forward: install the fake-HID layer.

/**
 * @brief Whether the activation-message window subclass is enabled via
 * the settings file. Defaults to ON (matches the battle-tested capture-
 * hook reference: subclass the target window and forward every message
 * through the original wndproc). Set "EnableSubclass"=0 in
 * focus_options.txt to turn it off.
 * @return True when the window subclass should be installed.
 */
static bool SubclassEnabled(void) {
  return FileDword("EnableSubclass", 1) != 0;
}

/**
 * @brief Whether inline detour installs suspend the whole process while the
 * entry bytes are written. Defaults to ON (safe against a second core
 * decoding a half-written branch). The controller's focus_options.txt switch
 * "SuspendThreadsOnPatch" set to 0 skips the stop-the-world: helps titles
 * whose threads can permanently wedge while suspended (spin-lock holders),
 * at the cost of a tiny race window during the few-byte patch.
 * @return True when SuspendAllThreads should guard the write.
 */
static bool SuspendThreadsOnPatchEnabled(void) {
  return FileDword("SuspendThreadsOnPatch", 1) != 0;
}

/**
 * @brief Whether a function pointer still points into committed, executable
 * memory. Guards CallWindowProcW against a stored wndproc that dangles
 * because the owning module was unloaded or the window procedure slot was
 * torn down.
 * @param p The candidate function pointer (may be NULL).
 * @return True when safe to jump to, false otherwise.
 */
static bool IsSafeProc(const void* p) {
  if (!p)
    return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
    return false;
  if (mbi.State != MEM_COMMIT || mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
    return false;
  return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

/**
 * @brief Absolute path of this process's per-game options file, if any.
 *
 * GameArchives launches several games concurrently; each injected DLL
 * locks onto its own options file at attach time so one game's settings
 * can never be overwritten by another game's launch. Empty means the DLL
 * reads focus_options.txt next to itself (the controller's layout).
 */
static char g_options_file_override[MAX_PATH] = "";

/**
 * @brief Resolve the absolute path of the controller's focus_options.txt.
 *
 * The file lives next to this DLL itself (the controller ships in the same
 * directory and writes the file there), so resolving the DLL module path
 * works for the portable package and the dev layout alike, independent of
 * the host process's working directory and of any %APPDATA% state. When a
 * per-game override was captured at attach (OptionsFile pointer), the
 * override path is returned instead.
 *
 * @param[out] path Buffer receiving the full file path.
 * @param size Size of the buffer.
 * @return True when the path was resolved, false when the DLL module path
 *         cannot be determined (callers then fall back to defaults).
 */
static bool ConfigFilePath(char* path, size_t size) {
  if (g_options_file_override[0] != '\0') {
    snprintf(path, size, "%s", g_options_file_override);
    return true;
  }
  char dir[MAX_PATH];
  HMODULE hmod = NULL;
  DWORD n = 0;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCSTR) &ConfigFilePath, &hmod))
    n = GetModuleFileNameA(hmod, dir, (DWORD) sizeof(dir));
  char* slash = (n > 0 && n < sizeof(dir)) ? strrchr(dir, '\\') : NULL;
  if (!slash)
    return false;
  *slash = '\0';
  snprintf(path, size, "%s\\focus_options.txt", dir);
  return true;
}

/**
 * @brief Capture the per-game "OptionsFile=" pointer at attach.
 *
 * Runs once from DllMain (DLL_PROCESS_ATTACH) before the worker starts:
 * looks inside the default focus_options.txt (next to this DLL) for a line
 * "OptionsFile=<absolute path>" and, when present, redirects every later
 * config read to that file. Only plain file I/O is used, which is safe
 * under the loader lock (no LoadLibrary/GetProcAddress here). Without the
 * pointer the DLL keeps reading the shared file, preserving the
 * controller's single-file workflow.
 */
static void ReadOptionsFileOverride(void) {
  char dir[MAX_PATH];
  HMODULE hmod = NULL;
  DWORD n = 0;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCSTR) &ReadOptionsFileOverride, &hmod))
    n = GetModuleFileNameA(hmod, dir, (DWORD) sizeof(dir));
  char* slash = (n > 0 && n < sizeof(dir)) ? strrchr(dir, '\\') : NULL;
  if (!slash)
    return;
  *slash = '\0';
  snprintf(dir, sizeof(dir), "%s\\focus_options.txt", dir);

  FILE* f = fopen(dir, "r");
  if (!f)
    return;

  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "OptionsFile=", 12) != 0)
      continue;

    char* value = line + 12;
    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == '\r' || value[len - 1] == '\n' ||
                       value[len - 1] == ' ' || value[len - 1] == '\t'))
      value[--len] = '\0';

    if (len > 0 && len < sizeof(g_options_file_override))
      memcpy(g_options_file_override, value, len + 1);
    break;
  }
  fclose(f);
}

/**
 * @brief Read one "name=value" switch from the controller's settings file
 * (focus_options.txt next to this DLL). The companion WPF controller writes
 * this file; the registry is deliberately never touched so the feature needs
 * no registry permissions anywhere.
 * @param name Switch name, e.g. "BlockPollingApis".
 * @param def Value returned when the file or line is missing.
 * @return The stored value, or the default on any failure.
 */
static DWORD FileDword(const char* name, DWORD def) {
  char path[MAX_PATH];
  if (!ConfigFilePath(path, sizeof(path)))
    return def;
  FILE* f = fopen(path, "r");
  if (!f)
    return def;
  DWORD val = def;
  bool found = false;
  char line[256];
  size_t nl = strlen(name);
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, name, nl) == 0 && line[nl] == '=') {
      val = (DWORD)strtoul(line + nl + 1, NULL, 10);
      found = true;
      break;
    }
  }
  fclose(f);
  return found ? val : def;
}

/**
 * @brief Read a 64-bit "name=value" switch from the controller's settings
 * file. Used for window handles (HWND is 64-bit on x64) that the controller
 * passes through "TargetWindow".
 * @param name Switch name.
 * @param def Value returned when the file or line is missing.
 * @return The stored value, or the default on any failure.
 */
static ULONG_PTR FileUintPtr(const char* name, ULONG_PTR def) {
  char path[MAX_PATH];
  if (!ConfigFilePath(path, sizeof(path)))
    return def;
  FILE* f = fopen(path, "r");
  if (!f)
    return def;
  ULONG_PTR val = def;
  bool found = false;
  char line[256];
  size_t nl = strlen(name);
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, name, nl) == 0 && line[nl] == '=') {
      val = (ULONG_PTR)strtoull(line + nl + 1, NULL, 10);
      found = true;
      break;
    }
  }
  fclose(f);
  return found ? val : def;
}

/**
 * @brief Whether polled key/mouse state is zeroed (GetAsyncKeyState /
 * GetKeyState / GetKeyboardState). Armed only by the explicit
 * "BlockPollingApis" switch; defaults to ON. XInput gamepad input is never
 * affected.
 * @return True when the poll hooks should be installed.
 */
static bool PollBlockEnabled(void) {
  return FileDword("BlockPollingApis", 1) != 0;
}

/**
 * @brief Whether real WM_INPUT messages are dropped in the subclassed
 * wndproc. Armed only by the explicit "BlockWmInput" switch; defaults to
 * ON. Only has an effect when the window subclass is installed
 * (EnableSubclass=1), because the drop happens inside SwallowProc.
 * @return True when real WM_INPUT should be swallowed.
 */
static bool WmInputBlockEnabled(void) {
  return FileDword("BlockWmInput", 1) != 0;
}

/**
 * @brief Whether the raw-API reads (GetRawInputData / GetRawInputBuffer) are
 * zeroed. Armed only by the explicit "BlockRawInputApis" switch; defaults to
 * ON. Works with or without a window subclass.
 * @return True when the raw-API hooks should be installed.
 */
static bool RawApisBlockEnabled(void) {
  return FileDword("BlockRawInputApis", 1) != 0;
}

/**
 * @brief Hooked GetAsyncKeyState: reports nothing pressed while key/mouse
 * polling is blocked (BlockPollingApis).
 * @param vKey Virtual-key code.
 * @return 0 while blocking, otherwise the real state.
 */
static SHORT WINAPI MyGetAsyncKeyState(int vKey) {
  if (g_realGetAsyncKeyState == 0) return 0;
  if (g_blockPolls == 1)
    return 0;
  return g_realGetAsyncKeyState(vKey);
}

/**
 * @brief Hooked GetKeyState: reports nothing toggled/pressed while key/mouse
 * polling is blocked (BlockPollingApis).
 * @param nVirtKey Virtual-key code.
 * @return 0 while blocking, otherwise the real state.
 */
static SHORT WINAPI MyGetKeyState(int nVirtKey) {
  if (g_realGetKeyState == 0) return 0;
  if (g_blockPolls == 1)
    return 0;
  return g_realGetKeyState(nVirtKey);
}

/**
 * @brief Hooked GetKeyboardState: zeroes the whole state table while key/mouse
 * polling is blocked (BlockPollingApis).
 * @param lpKeyState 256-byte output buffer.
 * @return TRUE.
 */
static BOOL WINAPI MyGetKeyboardState(PBYTE lpKeyState) {
  if (g_realGetKeyboardState == 0) {
    memset(lpKeyState, 0, 256);
    return TRUE;
  }
  if (g_blockPolls == 1) {
    memset(lpKeyState, 0, 256);
    return TRUE;
  }
  return g_realGetKeyboardState(lpKeyState);
}

/**
 * @brief Hooked GetRawInputData.
 *
 * While raw-API reads are blocked (BlockRawInputApis), raw input is
 * invisible to every reader: the query returns 0 bytes so the engine's raw
 * path never runs on real host input (measured: a WGC-captured RE-engine
 * title wedges its UI thread within ~1 second of receiving real raw input,
 * then the next click forces the "not responding" dialog). Unlike the
 * WM_INPUT message drop this layer needs NO window subclass, exactly like
 * the reference's GetRawInputDataHook. The size probe (pData == NULL) is
 * honored with the real required size so an engine that allocates its
 * RAWINPUT buffer from the probe never sees a bogus 0-length allocation; the
 * subsequent data query is then satisfied with 0 bytes. Per the SDK,
 * pData==NULL success returns 0, pData!=NULL success returns bytes copied,
 * error returns (UINT)-1.
 * @param hRawInput Raw input handle.
 * @param uiCommand RID_INPUT / RID_HEADER.
 * @param pData Destination buffer, NULL when querying the size.
 * @param pcbSize In/out buffer size.
 * @param cbSizeHeader Size of RAWINPUTHEADER.
 * @return Bytes written (0 while blocking), like the original API.
 */
static UINT WINAPI MyGetRawInputData(HRAWINPUT hRawInput, UINT uiCommand,
                                     LPVOID pData, PUINT pcbSize,
                                     UINT cbSizeHeader) {
  if (g_realGetRawInputData == 0)
    return 0;
  if (g_blockRawApis == 1) {
    if (pData == NULL && pcbSize)
      return g_realGetRawInputData(hRawInput, uiCommand, NULL, pcbSize,
                                   cbSizeHeader);
    if (pcbSize)
      *pcbSize = 0;
    return 0;
  }
  return g_realGetRawInputData(hRawInput, uiCommand, pData, pcbSize,
                               cbSizeHeader);
}

/**
 * @brief Hooked GetRawInputBuffer: while raw-API reads are blocked
 * (BlockRawInputApis), no RAWINPUT records are written so an engine that
 * drains raw input in batches (the pattern of GetRawInputBuffer-based loops)
 * sees no keyboard/mouse events. The buffer-size probe (pData == NULL) is
 * honored with the real first-record size so the engine's allocation never
 * mis-sizes. Per the SDK, pData==NULL success returns 0 (with the required
 * size in *pcbSize), pData!=NULL success returns the number of records
 * written, error returns (UINT)-1.
 * @param pData RAWINPUT output buffer, NULL when probing the size.
 * @param pcbSize In/out buffer size in bytes.
 * @param cbSizeHeader Size of RAWINPUTHEADER.
 * @return Records written (0 while blocking), like the original API.
 */
static UINT WINAPI MyGetRawInputBuffer(PRAWINPUT pData, PUINT pcbSize,
                                       UINT cbSizeHeader) {
  if (g_realGetRawInputBuffer == 0)
    return 0;
  if (g_blockRawApis == 1) {
    if (pData == NULL && pcbSize)
      return g_realGetRawInputBuffer(NULL, pcbSize, cbSizeHeader);
    if (pcbSize)
      *pcbSize = 0;
    return 0;
  }
  return g_realGetRawInputBuffer(pData, pcbSize, cbSizeHeader);
}

/**
 * @brief Install the keyboard/mouse blocking hooks (inline detours) for
 * each independently-armed layer. Failure of any single hook is non-fatal
 * and logged; the game keeps the remaining behavior.
 */
static void InputBlockHookInstall(void) {
  bool polls = PollBlockEnabled();
  bool wmInput = WmInputBlockEnabled();
  bool rawApis = RawApisBlockEnabled();
  bool any = polls || wmInput || rawApis;
  Dlog("InputBlockHookInstall: BlockPollingApis=%d BlockWmInput=%d "
       "BlockRawInputApis=%d",
       polls ? 1 : 0, wmInput ? 1 : 0, rawApis ? 1 : 0);

  if (!any) {
    Dlog("keyboard/mouse blocking disabled by config");
    return;
  }

  InterlockedExchange(&g_blockWmInput, wmInput ? 1 : 0);

  {
    Dlog("Installing detours for keyboard/mouse blocking");

    if (polls) {
      bool ok = DetourInstall((void*)g_exportGetAsyncKeyState,
                         (void*)MyGetAsyncKeyState,
                         (void**)&g_realGetAsyncKeyState, &g_detourAsk);
      Dlog(ok ? "block: GetAsyncKeyState hooked"
              : "block: GetAsyncKeyState skipped");
      ok = DetourInstall((void*)g_exportGetKeyState, (void*)MyGetKeyState,
                         (void**)&g_realGetKeyState, &g_detourKst);
      Dlog(ok ? "block: GetKeyState hooked" : "block: GetKeyState skipped");
      ok = DetourInstall((void*)g_exportGetKeyboardState,
                         (void*)MyGetKeyboardState,
                         (void**)&g_realGetKeyboardState, &g_detourKb);
      Dlog(ok ? "block: GetKeyboardState hooked"
              : "block: GetKeyboardState skipped");
    } else {
      Dlog("block: polled-state hooks skipped (BlockPollingApis=0)");
    }

    /* Raw-API layer: independent of subclass and of the WM_INPUT drop. Both
       GetRawInputData and GetRawInputBuffer are hooked and implement the SDK
       semantics (size probe preserved). */
    if (rawApis) {
      bool ok = DetourInstall((void*)g_exportGetRawInputData,
                         (void*)MyGetRawInputData,
                         (void**)&g_realGetRawInputData, &g_detourRin);
      Dlog(ok ? "block: GetRawInputData hooked"
              : "block: GetRawInputData skipped");
      ok = DetourInstall((void*)g_exportGetRawInputBuffer,
                         (void*)MyGetRawInputBuffer,
                         (void**)&g_realGetRawInputBuffer, &g_detourRbuf);
      Dlog(ok ? "block: GetRawInputBuffer hooked"
              : "block: GetRawInputBuffer skipped");
    } else {
      Dlog("block: raw-API hooks skipped (BlockRawInputApis=0)");
    }
  }

  if (wmInput && g_prev) {
    Dlog("block: WM_INPUT drop armed on subclassed window");
  } else if (wmInput) {
    Dlog("block: WM_INPUT drop requested but no subclass present (blocked = "
         "raw-API guard only)");
  }

  InterlockedExchange(&g_blockPolls, polls ? 1 : 0);
  InterlockedExchange(&g_blockRawApis, rawApis ? 1 : 0);
  Dlog("keyboard/mouse input blocked for the enabled layers");
}

/**
 * @brief Restore all keyboard/mouse blocking hooks and their fall-through
 * pointers.
 */
static void InputBlockHookRestore(void) {
  InterlockedExchange(&g_blockPolls, 0);
  InterlockedExchange(&g_blockWmInput, 0);
  InterlockedExchange(&g_blockRawApis, 0);
  DetourRestore(&g_detourAsk, (void**)&g_realGetAsyncKeyState,
                (void*)g_exportGetAsyncKeyState);
  DetourRestore(&g_detourKst, (void**)&g_realGetKeyState,
                (void*)g_exportGetKeyState);
  DetourRestore(&g_detourKb, (void**)&g_realGetKeyboardState,
                (void*)g_exportGetKeyboardState);
  DetourRestore(&g_detourRin, (void**)&g_realGetRawInputData,
                (void*)g_exportGetRawInputData);
  DetourRestore(&g_detourRbuf, (void**)&g_realGetRawInputBuffer,
                (void*)g_exportGetRawInputBuffer);
}

/* ------------------------------------------------------------------ */
/* Cursor control blocking: keep the cursor visible and unconfined     */
/* ------------------------------------------------------------------ */

static FnShowCursor g_realShowCursor;   ///< ShowCursor fall-through.
static FnClipCursor g_realClipCursor;   ///< ClipCursor fall-through.
static FnGetClipCursor g_realGetClipCursor; ///< GetClipCursor fall-through.

static Detour g_detourShowCursor;  ///< Detour record for ShowCursor.
static Detour g_detourClipCursor;  ///< Detour record for ClipCursor.
static Detour g_detourGetClipCursor; ///< Detour record for GetClipCursor.

/**
 * @brief Whether the ShowCursor control-point hook is armed via
 * "BlockCursorHide" (default ON). The hook itself is a STRICT pass-through:
 * blocking a hide desyncs the game's display-count belief from the real one
 * and dead-loops games whose show path is
 * "while (ShowCursor(TRUE) != 0) {}", so the hook only stays installed as
 * the config's control point (see MyShowCursor).
 * @return True when the ShowCursor hook should be installed.
 */
static bool CursorHideEnabled(void) {
  return FileDword("BlockCursorHide", 1) != 0;
}

/**
 * @brief Whether the game is prevented from locking the cursor into a
 * window/region via "BlockCursorLock" (default ON). With this set,
 * ClipCursor(rect) and SetCursorPos clamping never confine the cursor
 * (FPS titles lock the cursor to the center, which breaks the remote
 * cursor).
 * @return True when the cursor should stay unconfined.
 */
static bool CursorLockEnabled(void) {
  return FileDword("BlockCursorLock", 1) != 0;
}

/**
 * @brief Hooked ShowCursor: STRICT pass-through - the game owns cursor
 * visibility.
 *
 * The reference's ShowCursorHook always calls the original, and this must
 * stay that way: the ShowCursor display count is part of the game's own
 * state machine. Interfering with a hide desyncs the game's belief about
 * the count from the real one. Field-tested failure modes:
 *   - Rewriting the request (ShowCursor(FALSE) -> ShowCursor(TRUE)) wedged
 *     an RE-engine title's cursor/input logic the moment it was enabled
 *     (reproduced every run).
 *   - Swallowing the hide (returning a mirrored count without calling the
 *     original) leaves the real count at 0 while the game believes it is
 *     -1; a game whose show path is "while (ShowCursor(TRUE) != 0) {}"
 *     then spins forever (dead-loop).
 * The hook stays installed purely as the config's control point, which
 * keeps the WPF controller toggle wired and safe.
 * @param bShow TRUE to show, FALSE to hide.
 * @return The new display count, from the original function.
 */
static int WINAPI MyShowCursor(BOOL bShow) {
  if (g_realShowCursor == 0)
    return 0;
  return g_realShowCursor(bShow);
}

/**
 * @brief Hooked ClipCursor: while blocking is active the requested region
 * is ignored so the cursor is never confined; NULL (unconfine) is passed
 * through unchanged.
 * @param lpRect Region to confine the cursor to, or NULL.
 * @return TRUE on success.
 */
static BOOL WINAPI MyClipCursor(const RECT* lpRect) {
  if (g_realClipCursor == 0)
    return FALSE;
  if (g_enableCursorLock == 1 && lpRect != NULL)
    return TRUE; /* refuse to confine */
  return g_realClipCursor(lpRect);
}

/**
 * @brief Hooked GetClipCursor: while blocking is active the reported clip
 * region is widened to the full virtual screen so the game never believes
 * the cursor is confined (engines that read back the clip rect to render
 * crosshairs etc. keep working).
 * @param lpRect Receives the current clip region.
 * @return TRUE on success.
 */
static BOOL WINAPI MyGetClipCursor(LPRECT lpRect) {
  if (g_realGetClipCursor == 0)
    return FALSE;
  if (g_enableCursorLock == 1) {
    if (lpRect) {
      lpRect->left = GetSystemMetrics(SM_XVIRTUALSCREEN);
      lpRect->top = GetSystemMetrics(SM_YVIRTUALSCREEN);
      lpRect->right = lpRect->left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
      lpRect->bottom = lpRect->top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    return TRUE;
  }
  return g_realGetClipCursor(lpRect);
}

/**
 * @brief Install the cursor-control blocking hooks (inline detours)
 * according to the settings file. Each hook is independent so a title
 * that breaks on one stays usable with the others.
 */
static void CursorBlockHookInstall(void) {
  bool hide = CursorHideEnabled();
  bool lock = CursorLockEnabled();
  InterlockedExchange(&g_enableCursorHide, hide ? 1 : 0);
  InterlockedExchange(&g_enableCursorLock, lock ? 1 : 0);

  if (hide) {
    InterlockedExchange(&g_enableCursorHide, 1);
    bool ok = DetourInstall((void*)g_exportShowCursor, (void*)MyShowCursor,
                            (void**)&g_realShowCursor, &g_detourShowCursor);
    Dlog(ok ? "cursor: ShowCursor hooked" : "cursor: ShowCursor skipped");
  }
  if (lock) {
    InterlockedExchange(&g_enableCursorLock, 1);
    bool ok = DetourInstall((void*)g_exportClipCursor, (void*)MyClipCursor,
                            (void**)&g_realClipCursor, &g_detourClipCursor);
    Dlog(ok ? "cursor: ClipCursor hooked" : "cursor: ClipCursor skipped");
    ok = DetourInstall((void*)g_exportGetClipCursor,
                       (void*)MyGetClipCursor,
                       (void**)&g_realGetClipCursor, &g_detourGetClipCursor);
    Dlog(ok ? "cursor: GetClipCursor hooked" : "cursor: GetClipCursor skipped");
  }
  if (!hide && !lock)
    Dlog("cursor controls left unchanged by config");
}

/**
 * @brief Restore all cursor-control blocking hooks and their fall-through
 * pointers.
 */
static void CursorBlockHookRestore(void) {
  InterlockedExchange(&g_enableCursorHide, 0);
  InterlockedExchange(&g_enableCursorLock, 0);
  DetourRestore(&g_detourShowCursor, (void**)&g_realShowCursor,
                (void*)g_exportShowCursor);
  DetourRestore(&g_detourClipCursor, (void**)&g_realClipCursor,
                (void*)g_exportClipCursor);
  DetourRestore(&g_detourGetClipCursor, (void**)&g_realGetClipCursor,
                (void*)g_exportGetClipCursor);
}

/* ------------------------------------------------------------------ */
/* XInput API rewriting (gamepad delivered over loopback UDP)          */
/* ------------------------------------------------------------------ */

/* When "XInputRewrite"=1 the streaming host forwards the session's gamepad
   state to THIS process over UDP instead of relying solely on the virtual
   gamepad. The virtual pad stays attached (per the design decision: the game
   keeps seeing a controller through the OS for every other XInput API), but
   while delivery is armed the host writes only neutral state to it, so no
   other host-side application receives the controls. We therefore hook the
   APIs that carry state / feedback:
   - XInputGetState: fresh UDP state is served from a per-controller slot;
     the undocumented extended entry XInputGetStateEx (name or ordinal 100/103)
     shares the same body, since many titles read the pad through it.
     While the slot is stale the call falls through to the virtual pad so
     gameplay never blanks out.
   - XInputSetState: pass-through only - the virtual pad is still there, so
     rumble keeps flowing back to the client via the existing ViGEm
     notification path; no reverse UDP channel is needed.
   The virtual pad must run for this mode (it answers XInputGetCapabilities /
   XInputGetBatteryInformation / XInputEnable for every other consumer), which
   the host guarantees by never detaching it while a session is live.

   XInputFakePad=1 extends the same hook set to the presence side, so the game
   sees a connected pad even when NOTHING is attached (no physical pad, no
   virtual pad, no host):
   - XInputGetCapabilities: the real call runs first; only when the OS reports
     ERROR_DEVICE_NOT_CONNECTED is a standard Xbox gamepad descriptor served
     for slot 0 (XINPUT_DEVTYPE_GAMEPAD / XINPUT_DEVSUBTYPE_GAMEPAD, full
     button / trigger / thumbstick capability mask and rumble). Other slots
     keep reporting disconnected, matching a real one-pad machine.
   - XInputGetBatteryInformation: wired + full for slot 0 while faking.
   - XInputGetKeystroke: ERROR_EMPTY for slot 0 while faking ("connected,
     nothing buffered").
   - XInputGetState / XInputGetStateEx: the real pad's state when the OS has a
     device; a neutral (all-released) pad with a stable packet number when it
     does not.
   - XInputSetState: consumed and acknowledged on slot 0 - never relayed to a
     real device, so a game's rumble cannot move a local pad that happens to
     be attached while the fake layer is live (the fake pad has no motor).
   A real device ALWAYS wins: every fake answer is served only after the
   underlying API reported not-connected, so plugging a real pad in mid-session
   is transparent. The fake needs no virtual pad and no host, and it works
   standalone or combined with XInputRewrite (where a stale delivered snapshot
   no longer falls through to "disconnected" - the neutral pad takes over).

   UDP layout: one datagram per controller snapshot, last-wins. The host
   always targets the fixed loopback port (XInputUdpPort, default 45690) for
   every delivery mode; the per-process aspect comes from this process being
   the only one whose injection listens on that port, never from a PID-derived
   destination address.
   All fields are little-endian host integers (identical on x86/x64), so the
   packed struct travels as-is. */

constexpr std::uint32_t XINPUT_UDP_MAGIC = 0x31504958u;  ///< "XIP1" little-endian tag.
constexpr unsigned XINPUT_UDP_BASE_PORT = 45690;         ///< Global-mode default port.
constexpr unsigned XINPUT_UDP_SLOTS = 4;                 ///< XInput supports 4 controllers.
constexpr LONG64 XINPUT_UDP_FRESH_MS = 120;              ///< Slot stays "connected" this long.

#pragma pack(push, 1)
/**
 * @brief One controller snapshot as transported over loopback UDP.
 */
struct XInputUdpPacket {
  std::uint32_t magic;        ///< XINPUT_UDP_MAGIC.
  std::uint32_t index;        ///< XInput slot 0-3.
  std::uint32_t packetNumber; ///< Host-side monotonic counter.
  XINPUT_GAMEPAD gamepad;     ///< Button / trigger / stick state.
};
static_assert(sizeof(XInputUdpPacket) == 24, "wire layout must stay 24 bytes");
#pragma pack(pop)

/**
 * @brief Latest known state for one XInput slot, protected by g_xinputLock.
 */
struct XInputSlot {
  LONG64 timestamp;    ///< GetTickCount64() at receive time; 0 = never.
  std::uint32_t packetNumber; ///< From the last received packet.
  XINPUT_GAMEPAD gamepad;     ///< Last received state.
};

static volatile LONG g_xinputEnabled = 0;  ///< 1 while rewriting is live.
static XInputSlot g_xinputSlots[XINPUT_UDP_SLOTS];  ///< Latest state per slot.
static CRITICAL_SECTION g_xinputLock;    ///< Guards g_xinputSlots.
static volatile LONG g_xinputLockInit = 0; ///< 1 after InitializeCriticalSection.
static volatile LONG g_firstPkt = 0;      ///< 1 once the first datagram landed.
static HANDLE g_xinputThread = 0;        ///< The UDP receive thread.

typedef DWORD(WINAPI* FnXInputGetState)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* FnXInputSetState)(DWORD, XINPUT_VIBRATION*);
/* The undocumented extended XInput exports (ordinal 100 / 108 on the legacy
   runtimes, name on xinput1_4) use their own structs. The EX state carries a
   trailing DWORD some engines (the RE Engine among them) validate: serving
   only the plain XINPUT_GAMEPAD bytes leaves that field as caller garbage and
   the engine discards the whole snapshot, so every EX path clears it. The EX
   capabilities extend the standard descriptor with the console identity. */
struct XinputGamepadEx {
  std::uint16_t wButtons;        ///< XINPUT button mask.
  std::uint8_t bLeftTrigger;     ///< Left trigger 0-255.
  std::uint8_t bRightTrigger;    ///< Right trigger 0-255.
  std::int16_t sThumbLX;         ///< Left stick X axis.
  std::int16_t sThumbLY;         ///< Left stick Y axis.
  std::int16_t sThumbRX;         ///< Right stick X axis.
  std::int16_t sThumbRY;         ///< Right stick Y axis.
  std::uint32_t dwUnknown;       ///< Validation field; must stay zero.
};
struct XinputStateEx {
  std::uint32_t dwPacketNumber;  ///< Monotonic change counter.
  XinputGamepadEx Gamepad;       ///< Extended gamepad state.
};
struct XinputCapabilitiesEx {
  XINPUT_CAPABILITIES Capabilities; ///< Standard descriptor.
  std::uint16_t vendorId;        ///< Microsoft VID (0x045E).
  std::uint16_t productId;       ///< Xbox 360 controller PID (0x028E).
  std::uint16_t versionNumber;   ///< Firmware version (0x0114).
  std::uint16_t unk1;            ///< Reserved; must stay zero.
};
typedef DWORD(WINAPI* FnXInputGetStateEx)(DWORD, XinputStateEx*);
typedef DWORD(WINAPI* FnXInputGetCapabilitiesEx)(DWORD, DWORD, DWORD, XinputCapabilitiesEx*);
typedef DWORD(WINAPI* FnXInputGetCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES*);
typedef DWORD(WINAPI* FnXInputGetBatteryInformation)(DWORD, BYTE,
                                                     XINPUT_BATTERY_INFORMATION*);
typedef DWORD(WINAPI* FnXInputGetKeystroke)(DWORD, DWORD, XINPUT_KEYSTROKE*);
typedef void(WINAPI* FnXInputEnable)(BOOL);
typedef DWORD(WINAPI* FnXInputGetDSoundAudioDeviceGuids)(DWORD, GUID*, GUID*);
typedef DWORD(WINAPI* FnXInputGetAudioDeviceIds)(DWORD, WCHAR*, UINT*, WCHAR*,
                                                 UINT*);

/** Maximum XInput runtimes this build will hook in one process. */
static const int XINPUT_RUNTIME_MAX = 8;

/**
 * @brief One resolved XInput runtime (a loaded xinput DLL) with its exports,
 * per-module trampolines and detour records.
 *
 * Titles commonly load BOTH their own xinput stub from the game directory
 * (typically xinput1_3.dll) and the system xinput1_4.dll, and may poll
 * either; the reference capture-hook therefore hooks every loaded variant
 * with its own trampoline. Each runtime carries its own export set, detour
 * records and a diagnostic call counter so a live session shows exactly
 * which module the game reads. Records below g_xinputRuntimeCount stay valid
 * for the process lifetime.
 */
struct XInputRuntime {
  const char* name;                     ///< DLL file name, for logs.
  HMODULE module;                       ///< Loaded module, kept alive.
  FnXInputGetState exportGetState;      ///< Original export entry.
  FnXInputGetState realGetState;        ///< Trampoline fall-through.
  FnXInputSetState exportSetState;      ///< Original export entry.
  FnXInputSetState realSetState;        ///< Trampoline fall-through.
  FnXInputGetStateEx exportGetStateEx;  ///< Original GetStateEx entry (0 when absent).
  FnXInputGetStateEx realGetStateEx;    ///< Trampoline fall-through for GetStateEx.
  FnXInputGetCapabilities exportGetCaps; ///< Original GetCapabilities entry.
  FnXInputGetCapabilities realGetCaps;  ///< Trampoline fall-through for GetCapabilities.
  FnXInputGetCapabilitiesEx exportGetCapsEx; ///< Original GetCapabilitiesEx entry.
  FnXInputGetCapabilitiesEx realGetCapsEx;   ///< Trampoline fall-through for GetCapabilitiesEx.
  FnXInputGetBatteryInformation exportGetBattery; ///< Original GetBatteryInformation entry.
  FnXInputGetBatteryInformation realGetBattery;   ///< Trampoline fall-through for GetBatteryInformation.
  FnXInputGetKeystroke exportGetKeystroke;   ///< Original GetKeystroke entry.
  FnXInputGetKeystroke realGetKeystroke;     ///< Trampoline fall-through for GetKeystroke.
  FnXInputEnable exportGetEnable;            ///< Original XInputEnable entry (0 when absent).
  FnXInputEnable realGetEnable;              ///< Trampoline fall-through for XInputEnable.
  FnXInputGetDSoundAudioDeviceGuids exportGetDsound; ///< Original GetDSoundAudioDeviceGuids entry.
  FnXInputGetDSoundAudioDeviceGuids realGetDsound;   ///< Trampoline fall-through for DSound.
  FnXInputGetAudioDeviceIds exportGetAudioIds;       ///< Original GetAudioDeviceIds entry.
  FnXInputGetAudioDeviceIds realGetAudioIds;         ///< Trampoline fall-through for AudioIds.
  Detour dGetState;     ///< XInputGetState detour record.
  Detour dGetStateEx;   ///< XInputGetStateEx detour record.
  Detour dSetState;     ///< XInputSetState detour record.
  Detour dGetCaps;      ///< XInputGetCapabilities detour record.
  Detour dGetCapsEx;    ///< XInputGetCapabilitiesEx detour record.
  Detour dBattery;      ///< XInputGetBatteryInformation detour record.
  Detour dKeystroke;    ///< XInputGetKeystroke detour record.
  Detour dEnable;       ///< XInputEnable detour record.
  Detour dDsound;       ///< XInputGetDSoundAudioDeviceGuids detour record.
  Detour dAudioIds;     ///< XInputGetAudioDeviceIds detour record.
  char path[MAX_PATH];  ///< Full module path, for the diagnostics.
  volatile LONG64 callsGetState;  ///< GetState/GetStateEx calls through this runtime.
  volatile LONG sawEnable;    ///< First XInputEnable call marker through this runtime.
};
static XInputRuntime g_xinputRuntimes[XINPUT_RUNTIME_MAX];
static int g_xinputRuntimeCount = 0;
static volatile LONG g_xinputFakeOn = 0;  ///< 1 while the fake-pad presence layer is live.

/** Neutral (fully released) state the fake pad reports on every poll. */
static const XINPUT_GAMEPAD g_xinputFakeNeutralPad = {0, 0, 0, 0, 0, 0, 0};

/** Standard Xbox gamepad descriptor served when the OS reports no device. */
static const XINPUT_CAPABILITIES g_xinputFakeCaps = {
    XINPUT_DEVTYPE_GAMEPAD, XINPUT_DEVSUBTYPE_GAMEPAD, 0,
    {XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
         XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK |
         XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB |
         XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER |
         XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y,
     0xFF, 0xFF, (SHORT)0xFFFF, (SHORT)0xFFFF, (SHORT)0xFFFF, (SHORT)0xFFFF},
    {0xFFFF, 0xFFFF}};

/**
 * @brief Whether XInput API rewriting is enabled via the settings file
 * ("XInputRewrite", default 0).
 * @return True when the rewrite mode should be installed.
 */
static bool XInputRewriteEnabled(void) {
  return FileDword("XInputRewrite", 0) != 0;
}

/**
 * @brief Whether the fake-pad presence layer is enabled via the settings
 * file ("XInputFakePad", default 0).
 *
 * When enabled the game sees ONE connected, idle Xbox-style pad on slot 0
 * even when no controller exists behind the OS; every fake answer is only
 * served after the underlying API reported not-connected, so a real device
 * keeps winning.
 * @return True when the fake pad should be presented.
 */
static bool XInputFakePadEnabled(void) {
  return FileDword("XInputFakePad", 0) != 0;
}

/**
 * @brief The UDP base port from the settings file ("XInputUdpPort", default
 * XINPUT_UDP_BASE_PORT).
 * @return The configured base port.
 */
static DWORD XInputUdpPort(void) {
  return FileDword("XInputUdpPort", XINPUT_UDP_BASE_PORT);
}

/**
 * @brief Whether the host delivers per-process ("XInputUdpMode" 1 = process,
 * 0 = global; default 0).
 *
 * Informational only: the mode no longer changes the binding port. Both modes
 * listen on the fixed XInputUdpPort; per-process routing is achieved by only
 * the target game's injection listening on that port.
 * @return True for per-process delivery.
 */
static bool XInputUdpProcessMode(void) {
  return FileDword("XInputUdpMode", 0) != 0;
}

/**
 * @brief Compute the local UDP port this process listens on.
 *
 * Always the fixed configured port (XInputUdpPort, default 45690). The port
 * never derives from the process ID; the host targets the same fixed port for
 * every delivery mode.
 * @return The local port number.
 */
static unsigned short XInputLocalPort(void) {
  return (unsigned short)XInputUdpPort();
}

/**
 * @brief Resolve the extended GetState entries (XInputGetStateEx).
 *
 * Many titles read the controller through the undocumented extended export
 * instead of plain XInputGetState. It is published by name on xinput1_4 and
 * by ordinal (100/103/108) on the older runtimes and the game-shipped
 * stubs; returns the first one that resolves and is distinct from the plain
 * XInputGetState entry.
 * @param h Module to probe (already loaded).
 * @param plainGetState The module's plain XInputGetState entry, so the
 * extended entry is only accepted when it is a DIFFERENT function.
 * @return The GetStateEx entry, or nullptr when the module has none.
 */
static FnXInputGetStateEx ResolveGetStateEx(HMODULE h,
                                            FnXInputGetState plainGetState) {
  static const LPCSTR ordinals[] = {(LPCSTR)MAKEWORD(100, 0),
                                    (LPCSTR)MAKEWORD(103, 0),
                                    (LPCSTR)MAKEWORD(108, 0)};
  FnXInputGetStateEx fx = (FnXInputGetStateEx)GetProcAddress(h, "XInputGetStateEx");
  if (fx && (void*)fx != (void*)plainGetState)
    return fx;
  for (LPCSTR ord : ordinals) {
    fx = (FnXInputGetStateEx)GetProcAddress(h, ord);
    if (fx && (void*)fx != (void*)plainGetState)
      return fx;
  }
  return nullptr;
}

/**
 * @brief Resolve one XInput export by name, falling back to a legacy
 * ordinal (the pre-xinput1_4 runtimes publish some entries only that way;
 * xinput1_4 exports everything by name).
 *
 * @param h Module to probe (already loaded).
 * @param name Export name on xinput1_4.
 * @param ordinal Legacy ordinal on xinput1_1/1_2/1_3/9_1_0 (0 = skip).
 * @return The entry, or nullptr when the module has none.
 */
static FARPROC ResolveExport(HMODULE h, const char* name, WORD ordinal) {
  FARPROC p = GetProcAddress(h, name);
  if (p || ordinal == 0)
    return p;
  /* An address in the 0..65535 range is interpreted by GetProcAddress as a
     function ordinal; go through INT_PTR so the cast never truncates on 64-bit. */
  return GetProcAddress(h, (LPCSTR)(INT_PTR)ordinal);
}

/**
 * @brief Bind the XInput exports of a module handle into a new runtime
 * record.
 *
 * The module is kept loaded by the caller for the process lifetime so the
 * detours' trampolines and pass-through pointers stay valid. Missing entries
 * (a legacy stub without the extended exports, for example) are recorded as
 * zero and simply skipped by the installer.
 * @param h Module to probe (already loaded).
 * @param who Which DLL it is, for the log.
 * @return True when XInputGetState and XInputSetState exports were found.
 */
static bool ResolveFromHandle(HMODULE h, const char* who) {
  FnXInputGetState gs =
      (FnXInputGetState)GetProcAddress(h, "XInputGetState");
  FnXInputSetState ss =
      (FnXInputSetState)GetProcAddress(h, "XInputSetState");
  if (!gs || !ss)
    return false;
  /* Dedup by MODULE HANDLE, never by name. Windows resolves some of the
     standard names to an already-loaded mapping (the shipped xinput1_1/1_2/1_3
     stubs and the system forwarders can share one module), so a manual
     GetModuleHandleA + LoadLibraryA pass records the SAME mapping several
     times. The by-name check in ResolveXInput's second pass only guards that
     one loop; the already-loaded pass is unguarded, so a table that was 5/8
     full ballooned to 8/8 with r5..r7 duplicating r0..r2 and then refused the
     genuinely NEW xinput1_4.dll / xinput9_1_0.dll ("runtime table full") -
     i.e. the cap silently dropped runtimes instead of merely being idle
     headroom. Deduping here also makes every repeated resolution cheap. */
  for (int i = 0; i < g_xinputRuntimeCount; ++i) {
    if (g_xinputRuntimes[i].module == h) {
      Dlog("xinput: %s already hooked as %s (same module %p), skipping duplicate",
           who, g_xinputRuntimes[i].name ? g_xinputRuntimes[i].name : "?", (void*)h);
      return true;
    }
  }
  if (g_xinputRuntimeCount >= XINPUT_RUNTIME_MAX) {
    Dlog("xinput: runtime table full, skipping %s", who);
    return true;
  }
  XInputRuntime& r = g_xinputRuntimes[g_xinputRuntimeCount];
  memset(&r, 0, sizeof(r));
  r.name = who;
  r.module = h;
  if (!GetModuleFileNameA(h, r.path, MAX_PATH))
    snprintf(r.path, sizeof(r.path), "?(%s)", who);
  r.exportGetState = gs;
  r.exportSetState = ss;
  r.exportGetStateEx = ResolveGetStateEx(h, gs);
  r.exportGetCaps =
      (FnXInputGetCapabilities)ResolveExport(h, "XInputGetCapabilities", 102);
  r.exportGetCapsEx = (FnXInputGetCapabilitiesEx)ResolveExport(
      h, "XInputGetCapabilitiesEx", 108);
  r.exportGetBattery = (FnXInputGetBatteryInformation)ResolveExport(
      h, "XInputGetBatteryInformation", 105);
  r.exportGetKeystroke =
      (FnXInputGetKeystroke)ResolveExport(h, "XInputGetKeystroke", 106);
  r.exportGetEnable = (FnXInputEnable)ResolveExport(h, "XInputEnable", 106);
  r.exportGetDsound = (FnXInputGetDSoundAudioDeviceGuids)ResolveExport(
      h, "XInputGetDSoundAudioDeviceGuids", 103);
  r.exportGetAudioIds = (FnXInputGetAudioDeviceIds)ResolveExport(
      h, "XInputGetAudioDeviceIds", 107);
  g_xinputRuntimeCount++;
  return true;
}

/**
 * @brief Resolve every XInput runtime available to the host process.
 *
 * Two passes:
 *   1. Any XInput DLL the process has ALREADY loaded is hooked - that is the
 *      exact instance the game's import table points at (a game-shipped stub
 *      is typically xinput1_3, modern titles xinput1_4, UWP xinput9_1_0). A
 *      title may load and poll MORE than one, so unlike the old single-module
 *      resolver this pass does NOT stop at the first hit: every loaded
 *      variant is recorded. GetModuleHandleA never re-loads a mapping.
 *   2. Any standard name not already recorded is loaded and hooked too, so a
 *      runtime the game initializes lazily after we inject is still the
 *      hooked instance. Loading a system xinput DLL has no side effects.
 * Every recorded module is kept loaded for the process lifetime.
 */
static void ResolveXInput(void) {
  static const char* const names[] = {
    "xinput1_1.dll", "xinput1_2.dll", "xinput1_3.dll", "xinput1_4.dll",
    "xinput9_1_0.dll",
  };
  for (const char* name : names) {
    HMODULE h = GetModuleHandleA(name);
    if (h)
      ResolveFromHandle(h, name);
  }
  for (const char* name : names) {
    bool seen = false;
    for (int i = 0; i < g_xinputRuntimeCount; ++i) {
      if (strcmp(g_xinputRuntimes[i].name, name) == 0) {
        seen = true;
        break;
      }
    }
    if (seen)
      continue;
    HMODULE h = LoadLibraryA(name);
    if (!h)
      continue;
    int before = g_xinputRuntimeCount;
    if (ResolveFromHandle(h, name)) {
      if (g_xinputRuntimeCount > before)
        Dlog("xinput: loaded %s and hooked", name);
      else
        Dlog("xinput: loaded %s but it maps to an already-hooked module", name);
    } else {
      FreeLibrary(h);
    }
  }
  if (g_xinputRuntimeCount == 0)
    Dlog("xinput: no XInput runtime found");
}

/**
 * @brief UDP receive thread: drains controller snapshots into the slots.
 *
 * A blocking socket with a 250 ms select timeout keeps the thread stoppable
 * (g_stop) without losing packets. Loopback datagrams addressed to a port
 * nobody listens on are dropped by the OS, so the thread may also run with no
 * host sending.
 * @param param Unused.
 * @return 0.
 */
static DWORD WINAPI XInputUdpThread(LPVOID param) {
  (void)param;
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    Dlog("xinput: WSAStartup failed %d", WSAGetLastError());
    return 0;
  }
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    Dlog("xinput: socket failed %d", WSAGetLastError());
    WSACleanup();
    return 0;
  }

  SOCKADDR_IN addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(XInputLocalPort());
  if (bind(s, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR) {
    Dlog("xinput: bind 127.0.0.1:%u failed %d", (unsigned)XInputLocalPort(),
         WSAGetLastError());
    closesocket(s);
    WSACleanup();
    return 0;
  }
  Dlog("xinput: listening on 127.0.0.1:%u", (unsigned)XInputLocalPort());

  while (InterlockedExchangeAdd(&g_stop, 0) == 0) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 250000; /* 250 ms so the stop flag is polled regularly */
    int r = select(0, &rd, NULL, NULL, &tv);
    if (r == SOCKET_ERROR) {
      if (WSAGetLastError() != WSAEINTR)
        break;
      continue;
    }
    if (r == 0)
      continue; /* timeout: re-check g_stop */
    if (!FD_ISSET(s, &rd))
      continue;

    SOCKADDR_IN from;
    memset(&from, 0, sizeof(from));
    int fromLen = sizeof(from);
    XInputUdpPacket pkt;
    int n = recvfrom(s, (char*)&pkt, sizeof(pkt), 0, (SOCKADDR*)&from,
                     &fromLen);
    if (n == (int)sizeof(pkt) && pkt.magic == XINPUT_UDP_MAGIC &&
        pkt.index < XINPUT_UDP_SLOTS) {
      if (g_xinputLockInit == 1) {
        EnterCriticalSection(&g_xinputLock);
        g_xinputSlots[pkt.index].packetNumber = pkt.packetNumber;
        g_xinputSlots[pkt.index].gamepad = pkt.gamepad;
        g_xinputSlots[pkt.index].timestamp = (LONG64)GetTickCount64();
        LeaveCriticalSection(&g_xinputLock);
      }
      InterlockedIncrement64(&g_udpDatagrams);
      if (InterlockedExchange(&g_firstPkt, 1) == 0)
        Dlog("xinput: first datagram slot=%u btn=%04x", pkt.index,
             (unsigned)pkt.gamepad.wButtons);
    }
  }

  closesocket(s);
  WSACleanup();
  Dlog("xinput: UDP thread exiting");
  return 0;
}

/**
 * @brief Shared implementation of XInputGetState for any runtime.
 *
 * While rewriting is live and the requested slot holds fresh state, the
 * caller's XINPUT_STATE is filled from the last received snapshot and
 * ERROR_SUCCESS returned — regardless of which runtime the call arrived
 * through. Otherwise the fake-pad layer takes over when armed: a real OS
 * device keeps its own state (its answer is returned as-is), and slot 0 with
 * no device behind it presents a connected, neutral pad instead of
 * ERROR_DEVICE_NOT_CONNECTED. When neither layer applies the call falls
 * through to the runtime's own trampoline.
 * @param rt The runtime the call arrived through.
 * @param dwUserIndex Controller slot 0-3.
 * @param pState Receives the state.
 * @return ERROR_SUCCESS on fresh data / a connected pad, otherwise the
 * original API's result.
 */
static DWORD MyGetStateImpl(XInputRuntime& rt, DWORD dwUserIndex,
                            XINPUT_STATE* pState) {
  if (g_xinputEnabled == 1 && g_xinputLockInit == 1 && pState &&
      dwUserIndex < XINPUT_UDP_SLOTS) {
    EnterCriticalSection(&g_xinputLock);
    LONG64 ts = g_xinputSlots[dwUserIndex].timestamp;
    bool fresh = ts != 0 && (GetTickCount64() - ts) < XINPUT_UDP_FRESH_MS;
    if (fresh) {
      pState->dwPacketNumber = g_xinputSlots[dwUserIndex].packetNumber;
      pState->Gamepad = g_xinputSlots[dwUserIndex].gamepad;
      LeaveCriticalSection(&g_xinputLock);
      InterlockedIncrement64(&g_xinputServedFresh);
      return ERROR_SUCCESS;
    }
    LeaveCriticalSection(&g_xinputLock);
  }
  /* Fake-pad layer: the real device's own state wins when one exists; slot 0
     with nothing behind it reports a connected, idle pad. */
  if (g_xinputFakeOn == 1 && pState && dwUserIndex == 0) {
    DWORD r = rt.realGetState
                  ? rt.realGetState(dwUserIndex, pState)
                  : ERROR_DEVICE_NOT_CONNECTED;
    if (r == ERROR_DEVICE_NOT_CONNECTED) {
      pState->dwPacketNumber = 0;
      pState->Gamepad = g_xinputFakeNeutralPad;
      return ERROR_SUCCESS;
    }
    return r;
  }
  if (!rt.realGetState)
    return ERROR_DEVICE_NOT_CONNECTED;
  return rt.realGetState(dwUserIndex, pState);
}

/**
 * @brief Shared implementation of XInputGetStateEx, the undocumented
 * extended state entry, for any runtime.
 *
 * The extended view is what several engines (the RE Engine among them)
 * actually poll. It carries the SAME snapshot as the public entry but through
 * the EX layout, whose trailing dwUnknown field is validated by the caller:
 * serving only the plain XINPUT_GAMEPAD bytes would leave that field as caller
 * garbage and the engine discards the whole snapshot as invalid. Every path
 * here clears it, mirroring MyGetStateImpl exactly otherwise.
 * @param rt The runtime the call arrived through.
 * @param dwUserIndex Controller slot 0-3.
 * @param pState Receives the extended state.
 * @return ERROR_SUCCESS on fresh data or a connected pad, otherwise the
 * original API's result.
 */
static DWORD MyGetStateExImpl(XInputRuntime& rt, DWORD dwUserIndex,
                              XinputStateEx* pState) {
  if (g_xinputEnabled == 1 && g_xinputLockInit == 1 && pState &&
      dwUserIndex < XINPUT_UDP_SLOTS) {
    EnterCriticalSection(&g_xinputLock);
    LONG64 ts = g_xinputSlots[dwUserIndex].timestamp;
    bool fresh = ts != 0 && (GetTickCount64() - ts) < XINPUT_UDP_FRESH_MS;
    if (fresh) {
      const XINPUT_GAMEPAD& g = g_xinputSlots[dwUserIndex].gamepad;
      pState->dwPacketNumber = g_xinputSlots[dwUserIndex].packetNumber;
      pState->Gamepad.wButtons = g.wButtons;
      pState->Gamepad.bLeftTrigger = g.bLeftTrigger;
      pState->Gamepad.bRightTrigger = g.bRightTrigger;
      pState->Gamepad.sThumbLX = g.sThumbLX;
      pState->Gamepad.sThumbLY = g.sThumbLY;
      pState->Gamepad.sThumbRX = g.sThumbRX;
      pState->Gamepad.sThumbRY = g.sThumbRY;
      pState->Gamepad.dwUnknown = 0;
      LeaveCriticalSection(&g_xinputLock);
      InterlockedIncrement64(&g_xinputServedFresh);
      return ERROR_SUCCESS;
    }
    LeaveCriticalSection(&g_xinputLock);
  }
  /* Fake-pad layer: same policy as the public entry, through the EX layout
     with the validation field cleared. */
  if (g_xinputFakeOn == 1 && pState && dwUserIndex == 0) {
    DWORD r = rt.realGetStateEx
                  ? rt.realGetStateEx(dwUserIndex, pState)
                  : ERROR_DEVICE_NOT_CONNECTED;
    if (r == ERROR_DEVICE_NOT_CONNECTED) {
      const XINPUT_GAMEPAD& n = g_xinputFakeNeutralPad;
      pState->dwPacketNumber = 0;
      pState->Gamepad.wButtons = n.wButtons;
      pState->Gamepad.bLeftTrigger = n.bLeftTrigger;
      pState->Gamepad.bRightTrigger = n.bRightTrigger;
      pState->Gamepad.sThumbLX = n.sThumbLX;
      pState->Gamepad.sThumbLY = n.sThumbLY;
      pState->Gamepad.sThumbRX = n.sThumbRX;
      pState->Gamepad.sThumbRY = n.sThumbRY;
      pState->Gamepad.dwUnknown = 0;
      return ERROR_SUCCESS;
    }
    return r;
  }
  if (!rt.realGetStateEx)
    return ERROR_DEVICE_NOT_CONNECTED;
  return rt.realGetStateEx(dwUserIndex, pState);
}

/**
 * @brief Shared implementation of XInputSetState for any runtime.
 *
 * In fake-pad mode the write NEVER reaches a real device: the fake pad has no
 * motor behind it, and relaying the game's rumble to a local pad that happens
 * to exist would be visible to its user. The write is consumed and
 * acknowledged on slot 0 (returning ERROR_DEVICE_NOT_CONNECTED would make the
 * game drop the controller). The rewrite-only path keeps the original
 * pass-through to the still-attached virtual pad, which relays rumble back to
 * the streaming client through the OS.
 * @param rt The runtime the call arrived through.
 * @param dwUserIndex Controller slot 0-3.
 * @param pVibration Motor levels.
 * @return ERROR_SUCCESS for an accepted fake write, otherwise the original
 * API's result.
 */
static DWORD MySetStateImpl(XInputRuntime& rt, DWORD dwUserIndex,
                            XINPUT_VIBRATION* pVibration) {
  InterlockedIncrement64(&g_xinputSetCalls);
  if (g_xinputFakeOn == 1)
    return (dwUserIndex == 0) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
  if (!rt.realSetState)
    return ERROR_DEVICE_NOT_CONNECTED;
  return rt.realSetState(dwUserIndex, pVibration);
}

/**
 * @brief Shared implementation of XInputGetCapabilities for any runtime.
 *
 * The real call runs first and anything other than "device not connected" is
 * authoritative (a real pad, a virtual pad, or an error that must surface).
 * Only when the OS reports no device does the fake layer serve a standard
 * Xbox gamepad descriptor for slot 0, so the game sees a controller exists —
 * on EVERY hooked runtime, which is exactly what lets a title that probes
 * caps through a second (system) DLL proceed to poll state.
 * @param rt The runtime the call arrived through.
 * @param dwUserIndex Controller slot 0-3.
 * @param dwFlags Any combination of XINPUT_FLAG_GAMEPAD.
 * @param pCapabilities Receives the device descriptor.
 * @return ERROR_SUCCESS on a real or fake connected device, otherwise the
 * original API's result.
 */
static DWORD MyGetCapsImpl(XInputRuntime& rt, DWORD dwUserIndex, DWORD dwFlags,
                           XINPUT_CAPABILITIES* pCapabilities) {
  InterlockedIncrement64(&g_xinputCapsCalls);
  if (rt.realGetCaps) {
    DWORD r = rt.realGetCaps(dwUserIndex, dwFlags, pCapabilities);
    if (r != ERROR_DEVICE_NOT_CONNECTED)
      return r;
  }
  if (g_xinputFakeOn == 1 && pCapabilities && dwUserIndex == 0) {
    *pCapabilities = g_xinputFakeCaps;
    return ERROR_SUCCESS;
  }
  return ERROR_DEVICE_NOT_CONNECTED;
}

/**
 * @brief Shared implementation of XInputGetCapabilitiesEx, the undocumented
 * extended descriptor entry some engines probe before they will poll the pad,
 * for any runtime.
 *
 * Mirrors MyGetCapsImpl: the real call is authoritative unless the OS reports
 * no device; the fake layer then serves the same standard Xbox gamepad
 * descriptor extended with the console identity (Microsoft VID 045E, Xbox 360
 * PID 028E) that these engines compare against.
 * @param rt The runtime the call arrived through.
 * @param a1 First extended argument, passed through verbatim.
 * @param dwUserIndex Controller slot 0-3.
 * @param dwFlags Any combination of XINPUT_FLAG_GAMEPAD.
 * @param pCapabilities Receives the extended device descriptor.
 * @return ERROR_SUCCESS on a real or fake connected device, otherwise the
 * original API's result.
 */
static DWORD MyGetCapsExImpl(XInputRuntime& rt, DWORD a1, DWORD dwUserIndex,
                             DWORD dwFlags,
                             XinputCapabilitiesEx* pCapabilities) {
  InterlockedIncrement64(&g_xinputCapsExCalls);
  if (pCapabilities == nullptr)
    return ERROR_INVALID_PARAMETER;
  if (rt.realGetCapsEx) {
    DWORD r = rt.realGetCapsEx(a1, dwUserIndex, dwFlags, pCapabilities);
    /* The real entry answers the a1=0 identity query with ERROR_INVALID_DATA
       even when no device is attached; both that and "device not connected"
       mean there is no real device to describe, so both fall through to the
       fake layer. */
    if (r != ERROR_DEVICE_NOT_CONNECTED && r != ERROR_INVALID_DATA)
      return r;
  }
  if (g_xinputFakeOn == 1 && dwUserIndex == 0) {
    pCapabilities->Capabilities = g_xinputFakeCaps;
    pCapabilities->vendorId = 0x045E;
    pCapabilities->productId = 0x028E;
    pCapabilities->versionNumber = 0x0114;
    pCapabilities->unk1 = 0;
    return ERROR_SUCCESS;
  }
  return ERROR_DEVICE_NOT_CONNECTED;
}

/**
 * @brief Shared implementation of XInputGetBatteryInformation for any runtime.
 *
 * Fake layer: wired + full for slot 0 when the OS reports no device, so the
 * game sees a powered controller instead of a disconnected one.
 * @param rt The runtime the call arrived through.
 * @param dwUserIndex Controller slot 0-3.
 * @param devType 0 = gamepad, 1 = headset.
 * @param pBatteryInformation Receives the battery report.
 * @return ERROR_SUCCESS on a real or fake report, otherwise the original
 * API's result.
 */
static DWORD MyBatteryImpl(XInputRuntime& rt, DWORD dwUserIndex, BYTE devType,
                           XINPUT_BATTERY_INFORMATION* pBatteryInformation) {
  InterlockedIncrement64(&g_xinputBatteryCalls);
  if (rt.realGetBattery) {
    DWORD r = rt.realGetBattery(dwUserIndex, devType, pBatteryInformation);
    if (r != ERROR_DEVICE_NOT_CONNECTED)
      return r;
  }
  if (g_xinputFakeOn == 1 && pBatteryInformation && dwUserIndex == 0) {
    pBatteryInformation->BatteryType = BATTERY_TYPE_WIRED;
    pBatteryInformation->BatteryLevel = BATTERY_LEVEL_FULL;
    return ERROR_SUCCESS;
  }
  return ERROR_DEVICE_NOT_CONNECTED;
}

/**
 * @brief Shared implementation of XInputGetKeystroke for any runtime.
 *
 * Fake layer: a connected-but-idle pad reports ERROR_EMPTY (nothing
 * buffered) on slot 0 when the OS reports no device, which is the state a
 * real, fully released pad reports.
 * @param rt The runtime the call arrived through.
 * @param dwUserIndex Controller slot 0-3.
 * @param dwReserved Reserved, must be 0.
 * @param pKeystroke Receives the next buffered keystroke on ERROR_SUCCESS.
 * @return ERROR_EMPTY on a fake idle pad, otherwise the original API's result.
 */
static DWORD MyKeystrokeImpl(XInputRuntime& rt, DWORD dwUserIndex,
                             DWORD dwReserved, XINPUT_KEYSTROKE* pKeystroke) {
  InterlockedIncrement64(&g_xinputKeystrokeCalls);
  if (rt.realGetKeystroke) {
    DWORD r = rt.realGetKeystroke(dwUserIndex, dwReserved, pKeystroke);
    if (r == ERROR_SUCCESS || r == ERROR_EMPTY || r != ERROR_DEVICE_NOT_CONNECTED)
      return r;
  }
  if (g_xinputFakeOn == 1 && dwUserIndex == 0)
    return ERROR_EMPTY;
  return ERROR_DEVICE_NOT_CONNECTED;
}

/**
 * @brief Shared implementation of XInputEnable for any runtime.
 *
 * The engine calls this to gate gamepad input while it switches UI focus or
 * pauses. Passing through keeps a real device behaving exactly as before;
 * with no real device it is a harmless no-op, and the call is still counted
 * so the diagnostics show whether the title touches this surface at all.
 * @param rt The runtime the call arrived through.
 * @param enable TRUE to enable gamepad input, FALSE to suspend it.
 */
static void MyEnableImpl(XInputRuntime& rt, BOOL enable) {
  if (InterlockedCompareExchange(&rt.sawEnable, 1, 0) == 0)
    Dlog("xinput: [%s] Enable called (first) enable=%d", rt.name,
         enable ? 1 : 0);
  InterlockedIncrement64(&g_xinputEnableCalls);
  if (rt.realGetEnable)
    rt.realGetEnable(enable);
}

/**
 * @brief Shared implementation of XInputGetDSoundAudioDeviceGuids for any
 * runtime.
 *
 * Merely a hook surface: the engine probes it to pair the pad with a headset
 * audio endpoint. The fake pad has no audio endpoint, so when the OS reports
 * no device the presence layer still answers ERROR_SUCCESS with zero GUIDs
 * (treated by engines as "pad present, no headset") instead of a
 * "not connected" that would make the whole probe look like no pad exists.
 * @param rt The runtime the call arrived through.
 * @param dwUserIndex Controller slot 0-3.
 * @param pRender Receives the render (headset) GUID; zero when absent.
 * @param pCapture Receives the capture (mic) GUID; zero when absent.
 * @return ERROR_SUCCESS when the pad exists (zero GUIDs), otherwise the
 * original API's result.
 */
static DWORD MyDsoundImpl(XInputRuntime& rt, DWORD dwUserIndex, GUID* pRender,
                          GUID* pCapture) {
  InterlockedIncrement64(&g_xinputDsoundCalls);
  if (rt.realGetDsound) {
    DWORD r = rt.realGetDsound(dwUserIndex, pRender, pCapture);
    if (r != ERROR_DEVICE_NOT_CONNECTED)
      return r;
  }
  if (g_xinputFakeOn == 1 && dwUserIndex == 0) {
    if (pRender)
      memset(pRender, 0, sizeof(*pRender));
    if (pCapture)
      memset(pCapture, 0, sizeof(*pCapture));
    return ERROR_SUCCESS;
  }
  return ERROR_DEVICE_NOT_CONNECTED;
}

/**
 * @brief Shared implementation of XInputGetAudioDeviceIds for any runtime.
 *
 * The Unicode counterpart of GetDSoundAudioDeviceGuids introduced with
 * xinput1_4. Same policy: a fake pad answers "present, no audio endpoint"
 * with two zero-length ID strings so a detection probe sees a connected pad.
 * @param rt The runtime the call arrived through.
 * @param dwUserIndex Controller slot 0-3.
 * @param pRender Receives the render device ID buffer when non-null.
 * @param pRenderCount On failure the required buffer size in characters;
 * on success the number written (0 for the fake pad's no-audio answer).
 * @param pCapture Receives the capture device ID buffer when non-null.
 * @param pCaptureCount On failure the required buffer size in characters;
 * on success the number written (0 for the fake pad's no-audio answer).
 * @return ERROR_SUCCESS when the pad exists, otherwise the original API's
 * result.
 */
static DWORD MyAudioIdsImpl(XInputRuntime& rt, DWORD dwUserIndex,
                            WCHAR* pRender, UINT* pRenderCount, WCHAR* pCapture,
                            UINT* pCaptureCount) {
  InterlockedIncrement64(&g_xinputAudioIdsCalls);
  if (rt.realGetAudioIds) {
    DWORD r =
        rt.realGetAudioIds(dwUserIndex, pRender, pRenderCount, pCapture,
                           pCaptureCount);
    if (r != ERROR_DEVICE_NOT_CONNECTED)
      return r;
  }
  if (g_xinputFakeOn == 1 && dwUserIndex == 0) {
    if (pRenderCount)
      *pRenderCount = 0;
    if (pCaptureCount)
      *pCaptureCount = 0;
    return ERROR_SUCCESS;
  }
  return ERROR_DEVICE_NOT_CONNECTED;
}

/**
 * @brief Generate the stdcall wrapper set for one runtime slot.
 *
 * A shared implementation cannot know WHICH module it was called through and
 * therefore which trampoline to fall through on, so each runtime gets its own
 * thin wrappers that bind the per-module record. The state wrappers also feed
 * the global diagnostic counter and their runtime's own call counter so a
 * live session shows exactly which DLL the game polls.
 * @param IDX Index into g_xinputRuntimes (0..XINPUT_RUNTIME_MAX-1).
 */
#define DEFINE_XINPUT_WRAPPERS(IDX)                                          \
  static DWORD WINAPI XGetState##IDX(DWORD u, XINPUT_STATE* s) {             \
    InterlockedIncrement64(&g_xinputGetStateCalls);                          \
    InterlockedIncrement64(&g_xinputRuntimes[IDX].callsGetState);            \
    return MyGetStateImpl(g_xinputRuntimes[IDX], u, s);                      \
  }                                                                          \
  static DWORD WINAPI XGetStateEx##IDX(DWORD u, XinputStateEx* s) {          \
    InterlockedIncrement64(&g_xinputGetStateCalls);                          \
    InterlockedIncrement64(&g_xinputRuntimes[IDX].callsGetState);            \
    return MyGetStateExImpl(g_xinputRuntimes[IDX], u, s);                    \
  }                                                                          \
  static DWORD WINAPI XSetState##IDX(DWORD u, XINPUT_VIBRATION* v) {         \
    return MySetStateImpl(g_xinputRuntimes[IDX], u, v);                      \
  }                                                                          \
  static DWORD WINAPI XGetCaps##IDX(DWORD u, DWORD f,                        \
                                    XINPUT_CAPABILITIES* c) {                \
    return MyGetCapsImpl(g_xinputRuntimes[IDX], u, f, c);                    \
  }                                                                          \
  static DWORD WINAPI XGetCapsEx##IDX(DWORD a, DWORD u, DWORD f,             \
                                      XinputCapabilitiesEx* c) {             \
    return MyGetCapsExImpl(g_xinputRuntimes[IDX], a, u, f, c);               \
  }                                                                          \
  static DWORD WINAPI XGetBattery##IDX(DWORD u, BYTE t,                      \
                                       XINPUT_BATTERY_INFORMATION* b) {      \
    return MyBatteryImpl(g_xinputRuntimes[IDX], u, t, b);                    \
  }                                                                          \
  static DWORD WINAPI XGetKeystroke##IDX(DWORD u, DWORD rsv,                 \
                                         XINPUT_KEYSTROKE* k) {              \
    return MyKeystrokeImpl(g_xinputRuntimes[IDX], u, rsv, k);                \
  }                                                                          \
  static void WINAPI XEnable##IDX(BOOL e) {                                  \
    MyEnableImpl(g_xinputRuntimes[IDX], e);                                  \
  }                                                                          \
  static DWORD WINAPI XDsound##IDX(DWORD u, GUID* r, GUID* c) {              \
    return MyDsoundImpl(g_xinputRuntimes[IDX], u, r, c);                     \
  }                                                                          \
  static DWORD WINAPI XAudioIds##IDX(DWORD u, WCHAR* r, UINT* rc,            \
                                     WCHAR* c, UINT* cc) {                   \
    return MyAudioIdsImpl(g_xinputRuntimes[IDX], u, r, rc, c, cc);           \
  }

DEFINE_XINPUT_WRAPPERS(0)
DEFINE_XINPUT_WRAPPERS(1)
DEFINE_XINPUT_WRAPPERS(2)
DEFINE_XINPUT_WRAPPERS(3)
DEFINE_XINPUT_WRAPPERS(4)
DEFINE_XINPUT_WRAPPERS(5)
DEFINE_XINPUT_WRAPPERS(6)
DEFINE_XINPUT_WRAPPERS(7)

/**
 * @brief Install one runtime's detours.
 *
 * GetState / GetStateEx / SetState and the input-system family (XInputEnable /
 * GetDSoundAudioDeviceGuids / GetAudioDeviceIds) are hooked on every runtime
 * regardless of the active layers so a game's device DETECTION always lands in
 * the diagnostics (the presence set - GetCapabilities / GetCapabilitiesEx /
 * GetBatteryInformation / GetKeystroke - is added when the fake pad is armed).
 * A legacy stub without an extended export is common: the missing entry is
 * logged and skipped. The macro expands inside XInputHookInstall, whose local
 * "fake" flag selects the presence set.
 * @param IDX Index into g_xinputRuntimes; guarded against the resolved count.
 */
#define HOOK_XINPUT_RUNTIME(IDX)                                              \
  if (IDX < g_xinputRuntimeCount)                                             \
  {                                                                           \
    XInputRuntime& rt = g_xinputRuntimes[IDX];                                \
    bool ok = DetourInstall((void*)rt.exportGetState, (void*)XGetState##IDX,  \
                            (void**)&rt.realGetState, &rt.dGetState);         \
    if (rt.exportGetStateEx) {                                                \
      ok = DetourInstall((void*)rt.exportGetStateEx,                          \
                         (void*)XGetStateEx##IDX,                             \
                         (void**)&rt.realGetStateEx, &rt.dGetStateEx);        \
    } else {                                                                  \
    }                                                                         \
    ok = DetourInstall((void*)rt.exportSetState, (void*)XSetState##IDX,       \
                       (void**)&rt.realSetState, &rt.dSetState);              \
    if (rt.exportGetEnable) {                                                 \
      ok = DetourInstall((void*)rt.exportGetEnable, (void*)XEnable##IDX,      \
                         (void**)&rt.realGetEnable, &rt.dEnable);             \
    } else {                                                                  \
    }                                                                         \
    if (rt.exportGetDsound) {                                                 \
      ok = DetourInstall((void*)rt.exportGetDsound, (void*)XDsound##IDX,      \
                         (void**)&rt.realGetDsound, &rt.dDsound);             \
    } else {                                                                  \
    }                                                                         \
    if (rt.exportGetAudioIds) {                                               \
      ok = DetourInstall((void*)rt.exportGetAudioIds, (void*)XAudioIds##IDX,  \
                         (void**)&rt.realGetAudioIds, &rt.dAudioIds);         \
    } else {                                                                  \
    }                                                                         \
    if (fake) {                                                               \
      if (rt.exportGetCaps) {                                                 \
        ok = DetourInstall((void*)rt.exportGetCaps, (void*)XGetCaps##IDX,     \
                           (void**)&rt.realGetCaps, &rt.dGetCaps);            \
      } else {                                                                \
      }                                                                       \
      if (rt.exportGetCapsEx) {                                               \
        ok = DetourInstall((void*)rt.exportGetCapsEx,                         \
                           (void*)XGetCapsEx##IDX,                            \
                           (void**)&rt.realGetCapsEx, &rt.dGetCapsEx);        \
      } else {                                                                \
      }                                                                       \
      if (rt.exportGetBattery) {                                              \
        ok = DetourInstall((void*)rt.exportGetBattery,                        \
                           (void*)XGetBattery##IDX,                           \
                           (void**)&rt.realGetBattery, &rt.dBattery);         \
      } else {                                                                \
      }                                                                       \
      if (rt.exportGetKeystroke) {                                            \
        ok = DetourInstall((void*)rt.exportGetKeystroke,                      \
                           (void*)XGetKeystroke##IDX,                         \
                           (void**)&rt.realGetKeystroke, &rt.dKeystroke);     \
      } else {                                                                \
      }                                                                       \
    }                                                                         \
  }

/**
 * @brief Install the XInput hooks (when XInputRewrite and/or XInputFakePad is
 * on): resolve EVERY XInput runtime, hook GetState/GetStateEx/SetState on
 * each, hook the presence APIs in fake-pad mode and start the UDP listener in
 * rewrite mode. Any missing piece is non-fatal and logged, so the game keeps
 * its normal pad behavior.
 */
static void XInputHookInstall(void) {
  bool rewrite = XInputRewriteEnabled();
  bool fake = XInputFakePadEnabled();
  if (!rewrite && !fake) {
    Dlog("xinput: rewriting and fake pad disabled by config");
    return;
  }

  ResolveXInput();
  if (g_xinputRuntimeCount == 0) {
    Dlog("xinput: install skipped (no runtime)");
    return;
  }

  if (g_xinputLockInit != 1) {
    InitializeCriticalSection(&g_xinputLock);
    InterlockedExchange(&g_xinputLockInit, 1);
  }
  memset(g_xinputSlots, 0, sizeof(g_xinputSlots));
  InterlockedExchange(&g_firstPkt, 0);

  if (fake)
    InterlockedExchange(&g_xinputFakeOn, 1);

  Dlog("xinput: hooking %d runtime(s)", g_xinputRuntimeCount);
  HOOK_XINPUT_RUNTIME(0)
  HOOK_XINPUT_RUNTIME(1)
  HOOK_XINPUT_RUNTIME(2)
  HOOK_XINPUT_RUNTIME(3)
  HOOK_XINPUT_RUNTIME(4)
  HOOK_XINPUT_RUNTIME(5)
  HOOK_XINPUT_RUNTIME(6)
  HOOK_XINPUT_RUNTIME(7)

  if (fake) {
    int caps = 0, capsex = 0, battery = 0, keystroke = 0;
    int enable = 0, dsound = 0, audioids = 0;
    for (int i = 0; i < g_xinputRuntimeCount; ++i) {
      const XInputRuntime& rt = g_xinputRuntimes[i];
      caps += rt.exportGetCaps ? 1 : 0;
      capsex += rt.exportGetCapsEx ? 1 : 0;
      battery += rt.exportGetBattery ? 1 : 0;
      keystroke += rt.exportGetKeystroke ? 1 : 0;
      enable += rt.exportGetEnable ? 1 : 0;
      dsound += rt.exportGetDsound ? 1 : 0;
      audioids += rt.exportGetAudioIds ? 1 : 0;
    }
    Dlog("xinput: fake pad armed (runtimes=%d caps=%d capsex=%d battery=%d "
         "keystroke=%d enable=%d dsound=%d audioids=%d)",
         g_xinputRuntimeCount, caps, capsex, battery, keystroke, enable,
         dsound, audioids);
  }

  if (rewrite) {
    g_xinputThread = CreateThread(NULL, 0, XInputUdpThread, 0, 0, NULL);
    if (!g_xinputThread)
      Dlog("xinput: UDP thread failed %lu", GetLastError());
    InterlockedExchange(&g_xinputEnabled, 1);
    Dlog("xinput: rewrite armed (mode=%d port=%u)",
         XInputUdpProcessMode() ? 1 : 0, (unsigned)XInputLocalPort());
  }
}

/**
 * @brief Restore one runtime's detours.
 * @param IDX Index into g_xinputRuntimes; guarded against the resolved count.
 */
#define UNHOOK_XINPUT_RUNTIME(IDX)                                            \
  if (IDX < g_xinputRuntimeCount)                                             \
  {                                                                           \
    XInputRuntime& rt = g_xinputRuntimes[IDX];                                \
    DetourRestore(&rt.dGetState, (void**)&rt.realGetState,                    \
                  (void*)rt.exportGetState);                                  \
    if (rt.exportGetStateEx)                                                  \
      DetourRestore(&rt.dGetStateEx, (void**)&rt.realGetStateEx,              \
                    (void*)rt.exportGetStateEx);                              \
    DetourRestore(&rt.dSetState, (void**)&rt.realSetState,                    \
                  (void*)rt.exportSetState);                                  \
    if (rt.exportGetCaps)                                                     \
      DetourRestore(&rt.dGetCaps, (void**)&rt.realGetCaps,                    \
                    (void*)rt.exportGetCaps);                                 \
    if (rt.exportGetCapsEx)                                                   \
      DetourRestore(&rt.dGetCapsEx, (void**)&rt.realGetCapsEx,                \
                    (void*)rt.exportGetCapsEx);                               \
    if (rt.exportGetBattery)                                                  \
      DetourRestore(&rt.dBattery, (void**)&rt.realGetBattery,                 \
                    (void*)rt.exportGetBattery);                              \
    if (rt.exportGetKeystroke)                                                \
      DetourRestore(&rt.dKeystroke, (void**)&rt.realGetKeystroke,             \
                    (void*)rt.exportGetKeystroke);                            \
    if (rt.exportGetEnable)                                                   \
      DetourRestore(&rt.dEnable, (void**)&rt.realGetEnable,                   \
                    (void*)rt.exportGetEnable);                               \
    if (rt.exportGetDsound)                                                   \
      DetourRestore(&rt.dDsound, (void**)&rt.realGetDsound,                   \
                    (void*)rt.exportGetDsound);                               \
    if (rt.exportGetAudioIds)                                                 \
      DetourRestore(&rt.dAudioIds, (void**)&rt.realGetAudioIds,               \
                    (void*)rt.exportGetAudioIds);                             \
  }

/**
 * @brief Restore the XInput hooks: stop the UDP thread, restore every
 * runtime's detours (state, extended state, set-state, plus the fake-pad
 * presence set) and drop the arm flags. The resolved xinput modules stay
 * loaded for the process lifetime.
 */
static void XInputHookRestore(void) {
  InterlockedExchange(&g_xinputEnabled, 0);
  InterlockedExchange(&g_xinputFakeOn, 0);
  if (g_xinputThread) {
    WaitForSingleObject(g_xinputThread, 2000);
    CloseHandle(g_xinputThread);
    g_xinputThread = 0;
  }
  UNHOOK_XINPUT_RUNTIME(0)
  UNHOOK_XINPUT_RUNTIME(1)
  UNHOOK_XINPUT_RUNTIME(2)
  UNHOOK_XINPUT_RUNTIME(3)
  UNHOOK_XINPUT_RUNTIME(4)
  UNHOOK_XINPUT_RUNTIME(5)
  UNHOOK_XINPUT_RUNTIME(6)
  UNHOOK_XINPUT_RUNTIME(7)
  Dlog("xinput: restore done");
}

/* ------------------------------------------------------------------ */
/* XInput fake activation (two-stage arming + device identity)         */
/* ------------------------------------------------------------------ */

// The presence layers (DirectInput fake pad, raw-input fake HID, the
// SetupDi/HidD virtual HID device and the WMI Win32_PNPEntity fake) are only
// served while the pad is *armed*. The XInput serve family (GetState /
// GetCaps / Battery / Keystroke / SetState / Enable / DSound / AudioIds)
// is intentionally NOT armed-gated: a game probes XInput once at startup and
// never looks again, so a delayed arm would leave the fake pad permanently
// invisible to it.
static volatile LONG g_fakeArmed = 0; ///< 1 while the pad presentation fakes are live.
static volatile LONG64 g_wmiPnpQueried = 0; ///< 1 once the game ran the WMI XInput filter while armed.
static volatile LONG64 g_fakeDiBound = 0;   ///< 1 once the game bound the pad as plain DirectInput while armed.
static volatile LONG g_fakeIdentityGen = 0; ///< Identity generation; 0 = original instance GUID.
static volatile LONG g_autoRescanFired = 0; ///< 1 once the auto-rescan driver finished a cycle.

/**
 * @brief Whether the pad presentation fakes are live.
 * @return True when g_fakeArmed is set.
 */
static bool FakeArmed(void) {
  return InterlockedExchangeAdd(&g_fakeArmed, 0) == 1;
}

/**
 * @brief Current device-identity generation.
 * @return The generation (0 = the original verified identity).
 */
static unsigned FakeIdentityGen(void) {
  return (unsigned)InterlockedExchangeAdd(&g_fakeIdentityGen, 0);
}

/**
 * @brief Whether the reference-style auto-rescan driver is enabled.
 *
 * XInputFakeRescan, default 1: the driver arms the fakes only once the game
 * is idle, then posts the WM_DEVICECHANGE rescan sequence and observes the
 * adoption. With 0 the legacy path arms the fakes immediately at install and
 * uses the fixed HotPlugNudgeThread rounds instead.
 * @return True when the auto-rescan driver owns the arming flow.
 */
static bool XInputFakeRescanEnabled(void) {
  return FileDword("XInputFakeRescan", 1) != 0;
}

/**
 * @brief Number of consecutive fast idle probes the driver needs before
 * arming the fakes (XInputFakeIdleProbes, default 120).
 * @return The configured probe count.
 */
static DWORD XInputFakeIdleProbes(void) {
  return FileDword("XInputFakeIdleProbes", 120);
}

/**
 * @brief Deadline in ms for the idle probe window (XInputFakeIdleMaxMs,
 * default 30000). The driver arms once the probe count is reached or the
 * deadline passes.
 * @return The configured deadline.
 */
static DWORD XInputFakeIdleMaxMs(void) {
  return FileDword("XInputFakeIdleMaxMs", 30000);
}

/**
 * @brief Maximum identity rotations of the auto-rescan retry loop
 * (XInputFakeRetries, default 8, clamped to 1..16).
 * @return The configured retry count.
 */
static DWORD XInputFakeRetries(void) {
  DWORD v = FileDword("XInputFakeRetries", 8);
  if (v < 1)
    v = 1;
  if (v > 16)
    v = 16;
  return v;
}

/**
 * @brief Forward: WMI Win32_PNPEntity fake installer.
 */
static void WmiPnpInstall(void);

/**
 * @brief Forward: WMI fake restorer.
 */
static void WmiPnpRestore(void);

/**
 * @brief Forward: SetupDi/HidD virtual HID installer.
 */
static void HidSetupDiHookInstall(void);

/**
 * @brief Forward: SetupDi/HidD virtual HID restorer.
 */
static void HidSetupDiHookRestore(void);

/**
 * @brief Forward: auto-rescan driver entry point.
 * @param param Unused.
 * @return 0 (unused).
 */
static DWORD WINAPI AutoRescanThreadProc(LPVOID param);

/* ------------------------------------------------------------------ */
/* DirectInput8 fake gamepad (no-driver device-presence layer)         */
/* ------------------------------------------------------------------ */

/**
 * @brief Standard DirectInput object GUIDs copied from dinput.h.
 *
 * dinput.h declares the standard GUIDs as extern symbols owned by the
 * dxguid import library; copying the values into local constants keeps
 * this single-file DLL free of that extra link dependency while still
 * letting the fake device report the exact object identities a real
 * wired Xbox 360 pad exposes through DirectInput.
 */
/// X axis object GUID (0xA36D02E0).
static const GUID kDiGuidXAxis = {0xA36D02E0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
/// Y axis object GUID (0xA36D02E1).
static const GUID kDiGuidYAxis = {0xA36D02E1, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
/// Z axis object GUID (0xA36D02E2).
static const GUID kDiGuidZAxis = {0xA36D02E2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
/// X rotation object GUID (0xA36D02F4).
static const GUID kDiGuidRxAxis = {0xA36D02F4, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
/// Y rotation object GUID (0xA36D02F5).
static const GUID kDiGuidRyAxis = {0xA36D02F5, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
/// Z rotation object GUID (0xA36D02E3).
static const GUID kDiGuidRzAxis = {0xA36D02E3, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
/// Button object GUID (0xA36D02F0).
static const GUID kDiGuidButton = {0xA36D02F0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
/// Point-of-view hat object GUID (0xA36D02F2).
static const GUID kDiGuidPov = {0xA36D02F2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
/// All-zero GUID used for the fake pad's (absent) force-feedback driver.
static const GUID kDiGuidEmpty = {0x00000000, 0x0000, 0x0000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
/// Product GUID encoding VID 045E / PID 028E (wired Xbox 360 pad).
static const GUID kDiGuidFakeProduct = {0x028e045e, 0x0000, 0x0000, {0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44}};
/// Instance GUID that names our virtual pad everywhere.
static const GUID kDiGuidFakeInstance = {0xb1ade001, 0xca7e, 0x4a11, {0x9e, 0x60, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};

/**
 * @brief The DI instance GUID for the current identity generation.
 *
 * The high word of Data1 plus Data2..Data4 are the immutable family marker
 * (IsFakeDiGuid checks them); the low word of Data1 carries the generation so
 * each retry hot-plugs a distinct instance instead of re-binding the one the
 * game already locked as DirectInput. Generation 0 is byte-identical to the
 * original verified identity.
 * @return The current instance GUID.
 */
static GUID FakeDiInstanceGuid(void) {
  GUID g = kDiGuidFakeInstance;
  const unsigned gen = FakeIdentityGen();
  if (gen != 0)
    g.Data1 = (g.Data1 & 0xFFFF0000u) | (gen & 0xFFFFu);
  return g;
}
/// IID of the ANSI IDirectInput8 interface (DirectInput8Create selector).
static const GUID kIidDi8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
/// IID of the ANSI IDirectInput8 interface (DirectInput8Create selector).
static const GUID kIidDi8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};

/// DIDEVTYPE_HID | "gamepad standard" subtype | gamepad class.
static const DWORD kDiFakeDevType =
    DIDEVTYPE_HID | (DI8DEVTYPEGAMEPAD_STANDARD << 8) | DI8DEVTYPE_GAMEPAD;

/// Dev type a real X360 HID pad reports in EnumDevices instances (0x0215):
/// caps/GetDeviceInfo report the DIDEVTYPE_HID-composed 0x10215, so the fake
/// mirrors the same split between the instance and the capability layers.
static const DWORD kDiFakeInstanceDevType = 0x0215;

/// Offset of rgbButtons[0] in DIJOYSTATE2 (the layout we report).
static const DWORD kDiFakeButtons0 = 48;
/// Offset of rgdwPOV[0] in DIJOYSTATE2.
static const DWORD kDiFakePov0 = 32;
/// Number of buttons the fake pad reports.
static const int kDiFakeButtonCount = 16;
/// Number of axes the fake pad reports.
static const int kDiFakeAxisCount = 6;

/**
 * @brief One axis GUID paired with its DIJOYSTATE2 offset.
 */
struct DiFakeAxis {
  const GUID* guid; ///< Standard DirectInput axis GUID.
  DWORD ofs;        ///< DIJOYSTATE2 offset holding the axis value.
};

/// Axis table in X/Y/Z/Rx/Ry/Rz instance order.
static const DiFakeAxis kDiFakeAxes[kDiFakeAxisCount] = {
    {&kDiGuidXAxis, 0},  {&kDiGuidYAxis, 4},   {&kDiGuidZAxis, 8},
    {&kDiGuidRxAxis, 12}, {&kDiGuidRyAxis, 16}, {&kDiGuidRzAxis, 20},
};

static volatile LONG g_diFakeOn = 0;        ///< 1 while the DI layer is armed.
static volatile LONG g_diFakeWide = 1;      ///< 1 when the game's device is W flavour.
static volatile LONG64 g_diEnumCalls = 0;   ///< IDirectInput8::EnumDevices calls.
static volatile LONG64 g_diCreateCalls = 0; ///< IDirectInput8::CreateDevice calls.
static volatile LONG64 g_diStatusCalls = 0; ///< IDirectInput8::GetDeviceStatus calls.
static volatile LONG64 g_diStateCalls = 0;  ///< Fake device GetDeviceState calls.
static volatile LONG64 g_diFormatCalls = 0; ///< Fake device SetDataFormat calls.
static volatile LONG64 g_diObjCalls = 0;    ///< Fake device EnumObjects calls.
static volatile LONG64 g_diDataCalls = 0;      ///< Fake device GetDeviceData calls.
static volatile LONG64 g_diCapsCalls = 0;      ///< Fake device GetCapabilities calls.
static volatile LONG64 g_diInfoCalls = 0;      ///< Fake device GetDeviceInfo calls.
static volatile LONG64 g_diObjInfoCalls = 0;   ///< Fake device GetObjectInfo calls.
static volatile LONG64 g_diSetPropCalls = 0;   ///< Fake device SetProperty calls.
static volatile LONG64 g_diEventCalls = 0;     ///< Fake device SetEventNotification calls.
static volatile LONG64 g_diPollCalls = 0;      ///< Fake device Poll calls.
static volatile LONG64 g_diAcquireCalls = 0;   ///< Fake device Acquire calls.
static volatile LONG64 g_diUnacquireCalls = 0; ///< Fake device Unacquire calls.
static volatile LONG64 g_diCoopCalls = 0;      ///< Fake device SetCooperativeLevel calls.

/* Raw-input device-presentation layer: a real X360 pad appears in the
   Windows raw-input/HID tree (GetRawInputDeviceList / GetRawInputDeviceInfo).
   A DI-only fake is invisible there, so an engine whose hot-plug rescan asks
   "is there a controller anywhere?" sees nothing and never activates its
   XInput slot - the observed 18:02 report (game polls the DI fake at 45 Hz
   and calls no XInput API). With this layer armed the game's rescan finds one
   wired Xbox 360 HID device and can bind XInput against it. */
static volatile LONG g_rawFakeOn = 0;      ///< 1 while the fake HID device is reported.
static volatile LONG64 g_rawListCalls = 0; ///< GetRawInputDeviceList calls.
static volatile LONG64 g_rawInfoCalls = 0; ///< GetRawInputDeviceInfo(A/W) calls.
static volatile LONG64 g_rawRegCalls = 0;  ///< RegisterRawInputDevices calls.

/// Handle of the fake wired Xbox 360 HID device (never collides with a real handle).
static const HANDLE kRawFakeDevice = (HANDLE)0xE8F00001;
/// HID device path of a wired Xbox 360 pad (GUID_DEVINTERFACE_HID suffix).
static const WCHAR kRawFakeDeviceNameW[] =
    L"\\\\?\\HID#VID_045E&PID_028E&IG_00#8&2b6e2a20&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}";
/// ANSI copy of the same device path.
static const char kRawFakeDeviceNameA[] =
    "\\\\?\\HID#VID_045E&PID_028E&IG_00#8&2b6e2a20&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}";

/// DIDATAFORMAT the game declared, used to scatter state at its offsets.
static DIDATAFORMAT g_diFakeFormat;
/// 1 once SetDataFormat stored a format.
static volatile LONG g_diFakeHasFormat = 0;

/**
 * @brief The single static COM object handed out for the fake pad.
 *
 * The vtable pointer MUST stay the first member: the game treats this
 * address as the IDirectInputDevice8 interface pointer.
 */
struct DiFakeDevice {
  void** vtable;      ///< Points at g_diFakeVtable.
  volatile LONG refs; ///< Reference count kept >= 1 (never destroyed).
};
static void* g_diFakeVtable[32]; ///< IDirectInputDevice8W method table.
static DiFakeDevice g_diFakeDevice = {g_diFakeVtable, 1};

/**
 * @brief Whether a GUID names the fake pad's instance (any identity
 * generation).
 *
 * The family marker is the high word of Data1 plus Data2..Data4; the low
 * word of Data1 carries the identity generation, so every rotated instance
 * still matches.
 * @param g Guid to test.
 * @return True when it is a fake-pad instance GUID.
 */
static bool IsFakeDiGuid(REFGUID g) {
  return (g.Data1 & 0xFFFF0000u) == (kDiGuidFakeInstance.Data1 & 0xFFFF0000u) &&
         memcmp(&g.Data2, &kDiGuidFakeInstance.Data2,
                sizeof(GUID) - offsetof(GUID, Data2)) == 0;
}

/**
 * @brief Snapshot the pad to expose: slot 0 while a datagram is fresh,
 *        otherwise the neutral (fully released) pad.
 * @return Current XINPUT_GAMEPAD to translate into DirectInput state.
 */
static XINPUT_GAMEPAD FakeDiCurrentPad(void) {
  XINPUT_GAMEPAD pad = g_xinputFakeNeutralPad;
  if (g_xinputLockInit == 1) {
    EnterCriticalSection(&g_xinputLock);
    if (g_xinputSlots[0].timestamp != 0 &&
        (LONG64)GetTickCount64() - g_xinputSlots[0].timestamp <=
            XINPUT_UDP_FRESH_MS)
      pad = g_xinputSlots[0].gamepad;
    LeaveCriticalSection(&g_xinputLock);
  }
  return pad;
}

/**
 * @brief Translate one axis GUID to its value, using the canonical
 *        Xbox-360-over-DirectInput convention a real pad presents:
 *        X/Y carry the left stick, Z the left trigger, Rx/Ry the right
 *        stick, Rz the right trigger.
 * @param g Axis GUID.
 * @param p Source pad state.
 * @return Signed 16-bit axis value (-32768..32767).
 */
static LONG FakeDiAxisValue(const GUID* g, const XINPUT_GAMEPAD& p) {
  if (memcmp(g, &kDiGuidXAxis, sizeof(GUID)) == 0)
    return p.sThumbLX;
  if (memcmp(g, &kDiGuidYAxis, sizeof(GUID)) == 0)
    return -(LONG)p.sThumbLY;
  if (memcmp(g, &kDiGuidZAxis, sizeof(GUID)) == 0)
    return (LONG)p.bLeftTrigger * 32767L / 255L;
  if (memcmp(g, &kDiGuidRzAxis, sizeof(GUID)) == 0)
    return (LONG)p.bRightTrigger * 32767L / 255L;
  if (memcmp(g, &kDiGuidRxAxis, sizeof(GUID)) == 0)
    return p.sThumbRX;
  if (memcmp(g, &kDiGuidRyAxis, sizeof(GUID)) == 0)
    return -(LONG)p.sThumbRY;
  return 0;
}

/**
 * @brief Whether DI button instance @p idx is pressed in @p p.
 *
 * The instance numbers follow the canonical Xbox-360-over-DirectInput
 * order a real pad exposes (A=0 B=1 X=2 Y=3 LB=4 RB=5 Back=6 Start=7
 * LS=8 RS=9). Instances 10/11 additionally report the analog triggers as
 * digital buttons (LT/RT past 0x40), which some engines read in the DI
 * path when no analog trigger object is bound; every other instance stays
 * unassigned.
 * @param idx DirectInput button instance (X360-over-DI layout).
 * @param p Source pad state.
 * @return True when pressed.
 */
static bool FakeDiButtonDown(int idx, const XINPUT_GAMEPAD& p) {
  switch (idx) {
  case 0:
    return (p.wButtons & XINPUT_GAMEPAD_A) != 0;
  case 1:
    return (p.wButtons & XINPUT_GAMEPAD_B) != 0;
  case 2:
    return (p.wButtons & XINPUT_GAMEPAD_X) != 0;
  case 3:
    return (p.wButtons & XINPUT_GAMEPAD_Y) != 0;
  case 4:
    return (p.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
  case 5:
    return (p.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
  case 6:
    return (p.wButtons & XINPUT_GAMEPAD_BACK) != 0;
  case 7:
    return (p.wButtons & XINPUT_GAMEPAD_START) != 0;
  case 8:
    return (p.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
  case 9:
    return (p.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
  case 10:
    return p.bLeftTrigger > 0x40;
  case 11:
    return p.bRightTrigger > 0x40;
  default:
    return false;
  }
}

/**
 * @brief Translate the d-pad bits to a DirectInput POV value.
 * @param p Source pad state.
 * @return 0/9000/18000/27000 (plus diagonals) or 0xFFFFFFFF when centred.
 */
static DWORD FakeDiPov(const XINPUT_GAMEPAD& p) {
  const bool u = (p.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
  const bool d = (p.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
  const bool l = (p.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
  const bool r = (p.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
  if (u && r)
    return 4500;
  if (r && d)
    return 13500;
  if (d && l)
    return 22500;
  if (l && u)
    return 31500;
  if (u)
    return 0;
  if (r)
    return 9000;
  if (d)
    return 18000;
  if (l)
    return 27000;
  return 0xFFFFFFFFu;
}

/**
 * @brief Invoke the game's EnumObjects/GetObjectInfo callback for one
 *        object, in the struct flavour matching the interface used.
 * @param cb Callback pointer.
 * @param ctx Callback context.
 * @param g Object GUID.
 * @param type DIDFT type word.
 * @param ofs Object offset advertised to the game.
 * @param nw Wide object name.
 * @param na ANSI object name.
 * @param wide True for the W flavour.
 * @return The callback's return value (DIENUM_STOP stops enumeration).
 */
static BOOL FakeDiPushObject(void* cb, LPVOID ctx, const GUID& g, DWORD type,
                             DWORD ofs, const wchar_t* nw, const char* na,
                             bool wide) {
  const DWORD flags =
      ((type & DIDFT_AXIS) != 0) ? (DIDOI_POLLED | DIDOI_ASPECTPOSITION) : 0;
  if (wide) {
    DIDEVICEOBJECTINSTANCEW o = {};
    o.dwSize = sizeof(o);
    o.guidType = g;
    o.dwOfs = ofs;
    o.dwType = type;
    o.dwFlags = flags;
    lstrcpyW(o.tszName, nw);
    return ((BOOL(WINAPI*)(const DIDEVICEOBJECTINSTANCEW*, LPVOID))cb)(&o, ctx);
  }
  DIDEVICEOBJECTINSTANCEA o = {};
  o.dwSize = sizeof(o);
  o.guidType = g;
  o.dwOfs = ofs;
  o.dwType = type;
  o.dwFlags = flags;
  lstrcpyA(o.tszName, na);
  return ((BOOL(WINAPI*)(const DIDEVICEOBJECTINSTANCEA*, LPVOID))cb)(&o, ctx);
}

/* ---- fake IDirectInputDevice8 vtable ---------------------------------- */

/**
 * @brief Fake QueryInterface: the object exposes only itself.
 * @param self The fake device.
 * @param riid Requested IID (ignored).
 * @param out Receives the interface pointer.
 * @return S_OK, or E_POINTER when @p out is null.
 */
static HRESULT STDMETHODCALLTYPE FakeDiQueryInterface(void* self, REFIID,
                                                      void** out) {
  if (!out)
    return E_POINTER;
  *out = self;
  InterlockedIncrement(&g_diFakeDevice.refs);
  return S_OK;
}

/**
 * @brief Fake AddRef.
 * @param self The fake device.
 * @return New reference count.
 */
static ULONG STDMETHODCALLTYPE FakeDiAddRef(void*) {
  return (ULONG)InterlockedIncrement(&g_diFakeDevice.refs);
}

/**
 * @brief Fake Release; the static singleton is never destroyed.
 * @param self The fake device.
 * @return New reference count, never below 1.
 */
static ULONG STDMETHODCALLTYPE FakeDiRelease(void*) {
  LONG refs = InterlockedDecrement(&g_diFakeDevice.refs);
  if (refs < 1) {
    InterlockedExchange(&g_diFakeDevice.refs, 1);
    refs = 1;
  }
  return (ULONG)refs;
}

/**
 * @brief Report the fake pad's capabilities (6 axes, 16 buttons, 1 POV).
 * @param self The fake device.
 * @param caps Caller DIDEVCAPS.
 * @return DI_OK, or DIERR_INVALIDPARAM.
 */
static HRESULT STDMETHODCALLTYPE FakeDiGetCapabilities(void*, DIDEVCAPS* caps) {
  if (!FakeArmed())
    return DIERR_INPUTLOST;
  if (!caps || caps->dwSize != sizeof(DIDEVCAPS))
    return DIERR_INVALIDPARAM;
  caps->dwFlags = DIDC_ATTACHED;
  caps->dwDevType = kDiFakeDevType;
  caps->dwAxes = kDiFakeAxisCount;
  caps->dwButtons = kDiFakeButtonCount;
  caps->dwPOVs = 1;
  InterlockedExchangeAdd64(&g_diCapsCalls, 1);
  return DI_OK;
}

/**
 * @brief Enumerate the fake pad's objects to the game's callback.
 * @param self The fake device.
 * @param cb Callback pointer.
 * @param ctx Callback context.
 * @param flags DIDFT filter.
 * @return DI_OK, or DIERR_INVALIDPARAM.
 */
static HRESULT STDMETHODCALLTYPE FakeDiEnumObjects(void*, void* cb, LPVOID ctx,
                                                   DWORD flags) {
  if (!cb)
    return DIERR_INVALIDPARAM;
  InterlockedExchangeAdd64(&g_diObjCalls, 1);
  const bool wide = g_diFakeWide == 1;
  const DWORD kind = flags & 0xFF;
  if (kind == 0 || (kind & DIDFT_AXIS) != 0) {
    for (int i = 0; i < kDiFakeAxisCount; ++i) {
      if (FakeDiPushObject(cb, ctx, *kDiFakeAxes[i].guid,
                           DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(i),
                           kDiFakeAxes[i].ofs, L"Axis", "Axis", wide) ==
          DIENUM_STOP)
        return DI_OK;
    }
  }
  if (kind == 0 || (kind & DIDFT_BUTTON) != 0) {
    for (int i = 0; i < kDiFakeButtonCount; ++i) {
      wchar_t nw[32];
      char na[32];
      snprintf(na, sizeof(na), "Button %d", i);
      for (int k = 0; k < 32; ++k)
        nw[k] = (wchar_t)(unsigned char)na[k];
      if (FakeDiPushObject(cb, ctx, kDiGuidButton,
                           DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i),
                           kDiFakeButtons0 + (DWORD)i, nw, na, wide) ==
          DIENUM_STOP)
        return DI_OK;
    }
  }
  if (kind == 0 || (kind & DIDFT_POV) != 0) {
    FakeDiPushObject(cb, ctx, kDiGuidPov, DIDFT_POV | DIDFT_MAKEINSTANCE(0),
                     kDiFakePov0, L"Hat Switch", "Hat Switch", wide);
  }
  return DI_OK;
}

/**
 * @brief Answer device properties (range, axis mode, centre) for the pad.
 * @param self The fake device.
 * @param rguid DIPROP_* pseudo-GUID (an integer cast to a pointer).
 * @param header DIPROPHEADER whose payload the caller fills.
 * @return DI_OK, or DIERR_*.
 */
static HRESULT STDMETHODCALLTYPE FakeDiGetProperty(void*, REFGUID rguid,
                                                   DIPROPHEADER* header) {
  if (!header)
    return DIERR_INVALIDPARAM;
  switch (reinterpret_cast<ULONG_PTR>(&rguid)) {
  case 1: /* DIPROP_BUFFERSIZE */
    if (header->dwSize != sizeof(DIPROPDWORD))
      return DIERR_INVALIDPARAM;
    reinterpret_cast<DIPROPDWORD*>(header)->dwData = 32;
    return DI_OK;
  case 2: /* DIPROP_AXISMODE */
    if (header->dwSize != sizeof(DIPROPDWORD))
      return DIERR_INVALIDPARAM;
    reinterpret_cast<DIPROPDWORD*>(header)->dwData = DIPROPAXISMODE_ABS;
    return DI_OK;
  case 3: /* DIPROP_GRANULARITY */
    if (header->dwSize != sizeof(DIPROPDWORD))
      return DIERR_INVALIDPARAM;
    reinterpret_cast<DIPROPDWORD*>(header)->dwData = 1;
    return DI_OK;
  case 4: /* DIPROP_RANGE: signed -32768..32767 like a real X360 pad */
    if (header->dwSize != sizeof(DIPROPRANGE))
      return DIERR_INVALIDPARAM;
    reinterpret_cast<DIPROPRANGE*>(header)->lMin = -32768;
    reinterpret_cast<DIPROPRANGE*>(header)->lMax = 32767;
    return DI_OK;
  case 5: /* DIPROP_DEADZONE */
    if (header->dwSize != sizeof(DIPROPDWORD))
      return DIERR_INVALIDPARAM;
    reinterpret_cast<DIPROPDWORD*>(header)->dwData = 0;
    return DI_OK;
  case 6: /* DIPROP_SATURATION */
    if (header->dwSize != sizeof(DIPROPDWORD))
      return DIERR_INVALIDPARAM;
    reinterpret_cast<DIPROPDWORD*>(header)->dwData = 10000;
    return DI_OK;
  default:
    return DIERR_UNSUPPORTED;
  }
}

/**
 * @brief Accept (and ignore) any device property write.
 * @param self The fake device.
 * @param rguid Property id.
 * @param header Property value.
 * @return DI_OK.
 */
static HRESULT STDMETHODCALLTYPE FakeDiSetProperty(void*, REFGUID pguid,
                                                   LPCDIPROPHEADER header) {
  InterlockedExchangeAdd64(&g_diSetPropCalls, 1);
  return DI_OK;
}

/**
 * @brief Fake Acquire (always succeeds).
 * @param self The fake device.
 * @return DI_OK.
 */
static HRESULT STDMETHODCALLTYPE FakeDiAcquire(void*) {
  if (!FakeArmed())
    return DIERR_INPUTLOST;
  InterlockedExchangeAdd64(&g_diAcquireCalls, 1);
  return DI_OK;
}

/**
 * @brief Fake Unacquire (always succeeds).
 * @param self The fake device.
 * @return DI_OK.
 */
static HRESULT STDMETHODCALLTYPE FakeDiUnacquire(void*) {
  if (!FakeArmed())
    return DIERR_INPUTLOST;
  InterlockedExchangeAdd64(&g_diUnacquireCalls, 1);
  return DI_OK;
}

/**
 * @brief Serve the current pad state in the layout the GAME declared.
 *
 * SetDataFormat stored the game's own DIDATAFORMAT; this walks its object
 * descriptors and writes each value at the offset the game asked for, so
 * the fake pad works for DIJOYSTATE2 and for the game's custom layouts
 * alike. Without a stored format the canonical DIJOYSTATE2 layout is used.
 *
 * @param self The fake device.
 * @param size Caller buffer size (bytes).
 * @param data Caller buffer.
 * @return DI_OK, or DIERR_INVALIDPARAM.
 */
static HRESULT STDMETHODCALLTYPE FakeDiGetDeviceState(void*, DWORD size,
                                                      LPVOID data) {
  if (!FakeArmed())
    return DIERR_INPUTLOST;
  if (!data || size == 0)
    return DIERR_INVALIDPARAM;
  InterlockedIncrement64(&g_diStateCalls);
  const XINPUT_GAMEPAD pad = FakeDiCurrentPad();
  BYTE* out = (BYTE*)data;

  if (g_diFakeHasFormat == 1 && g_diFakeFormat.rgodf) {
    DWORD clear = size < g_diFakeFormat.dwDataSize ? size
                                                   : g_diFakeFormat.dwDataSize;
    memset(out, 0, clear);
    for (DWORD i = 0; i < g_diFakeFormat.dwNumObjs; ++i) {
      const DIOBJECTDATAFORMAT& od = g_diFakeFormat.rgodf[i];
      if (od.dwOfs >= size)
        continue;
      const GUID* g = od.pguid;
      const DWORD type = od.dwType;
      bool isButton, isPov;
      if (g) {
        isButton = memcmp(g, &kDiGuidButton, sizeof(GUID)) == 0;
        isPov = memcmp(g, &kDiGuidPov, sizeof(GUID)) == 0;
      } else {
        isButton = (type & DIDFT_BUTTON) != 0;
        isPov = (type & DIDFT_POV) != 0;
      }
      if (isButton) {
        int idx = (int)DIDFT_GETINSTANCE(type);
        if (idx < 0 || idx >= kDiFakeButtonCount) {
          /* Some engines (DMC5's RE engine) bind button objects with a
             wildcard instance (DIDFT_OPTIONAL | DIDFT_ANYINSTANCE) and a
             null pguid, so the instance field cannot identify the button.
             Xbox-360-over-DirectInput devices report button i at
             DIJOYSTATE2 offset 48 + i, which is exactly the layout those
             formats are built for, so derive the number from the object
             offset instead. */
          if (od.dwOfs >= kDiFakeButtons0 &&
              od.dwOfs - kDiFakeButtons0 < (DWORD)kDiFakeButtonCount)
            idx = (int)(od.dwOfs - kDiFakeButtons0);
        }
        if (idx >= 0 && idx < kDiFakeButtonCount && od.dwOfs + 1 <= size)
          out[od.dwOfs] = FakeDiButtonDown(idx, pad) ? 0x80 : 0x00;
      } else if (isPov) {
        if (od.dwOfs + 4 <= size)
          *(DWORD*)(out + od.dwOfs) = FakeDiPov(pad);
      } else {
        LONG v = 0;
        if (g) {
          v = FakeDiAxisValue(g, pad);
        } else {
          const int inst = DIDFT_GETINSTANCE(type);
          if (inst >= 0 && inst < kDiFakeAxisCount)
            v = FakeDiAxisValue(kDiFakeAxes[inst].guid, pad);
        }
        if (od.dwOfs + 4 <= size)
          *(LONG*)(out + od.dwOfs) = v;
      }
    }
    return DI_OK;
  }

  /* No declared format: emit the canonical DIJOYSTATE2 layout with the same
     Xbox-360-over-DirectInput mapping as FakeDiAxisValue: X/Y carry the left
     stick, Z the left trigger, Rx/Ry the right stick, Rz the right trigger. */
  const DWORD clear = size < (DWORD)sizeof(DIJOYSTATE2) ? size
                                                        : (DWORD)sizeof(DIJOYSTATE2);
  memset(out, 0, clear);
  if (size >= 24) {
    *(LONG*)(out + 0) = pad.sThumbLX;
    *(LONG*)(out + 4) = -(LONG)pad.sThumbLY;
    *(LONG*)(out + 8) = (LONG)pad.bLeftTrigger * 32767L / 255L;
    *(LONG*)(out + 12) = pad.sThumbRX;
    *(LONG*)(out + 16) = -(LONG)pad.sThumbRY;
    *(LONG*)(out + 20) = (LONG)pad.bRightTrigger * 32767L / 255L;
  }
  if (size >= kDiFakePov0 + 4)
    *(DWORD*)(out + kDiFakePov0) = FakeDiPov(pad);
  for (int i = 0; i < kDiFakeButtonCount; ++i) {
    if (kDiFakeButtons0 + (DWORD)i + 1 <= size)
      out[kDiFakeButtons0 + i] = FakeDiButtonDown(i, pad) ? 0x80 : 0x00;
  }
  return DI_OK;
}

/**
 * @brief Buffered-read path: the fake pad never queues events.
 * @param self The fake device.
 * @param objectDataSize Size of one DIDEVICEOBJECTDATA.
 * @param objects Caller buffer.
 * @param inOut In: capacity; out: rows returned (always 0).
 * @param flags DIGDD_* flags.
 * @return DI_OK.
 */
static HRESULT STDMETHODCALLTYPE FakeDiGetDeviceData(void*, DWORD dataSize,
                                                     LPVOID, LPDWORD inOut,
                                                     DWORD flags) {
  if (!FakeArmed())
    return DIERR_INPUTLOST;
  InterlockedExchangeAdd64(&g_diDataCalls, 1);
  if (inOut)
    *inOut = 0;
  return DI_OK;
}

/**
 * @brief Store the game's DIDATAFORMAT so GetDeviceState can honour it.
 * @param self The fake device.
 * @param format The format the game is binding to this device.
 * @return DI_OK.
 */
static HRESULT STDMETHODCALLTYPE FakeDiSetDataFormat(void*,
                                                     LPCDIDATAFORMAT format) {
  if (!FakeArmed())
    return DIERR_INPUTLOST;
  /* The game bound the pad as a plain DirectInput device while armed: the
     auto-rescan observation loop treats this (with no WMI adoption) as a
     "stuck on DirectInput" retract signal. */
  InterlockedExchange64(&g_fakeDiBound, 1);
  InterlockedExchangeAdd64(&g_diFormatCalls, 1);
  if (format && format->rgodf && format->dwNumObjs) {
    g_diFakeFormat = *format;
    InterlockedExchange(&g_diFakeHasFormat, 1);
  }
  return DI_OK;
}

/**
 * @brief Accept (and ignore) an event-notification handle.
 * @param self The fake device.
 * @param h New event handle.
 * @return DI_OK.
 */
static HRESULT STDMETHODCALLTYPE FakeDiSetEventNotification(void*, HANDLE h) {
  InterlockedExchangeAdd64(&g_diEventCalls, 1);
  return DI_OK;
}

/**
 * @brief Accept (and ignore) the cooperative level.
 * @param self The fake device.
 * @param hwnd Foreground window.
 * @param flags DIEDFL cooperative flags.
 * @return DI_OK.
 */
static HRESULT STDMETHODCALLTYPE FakeDiSetCooperativeLevel(void*, HWND,
                                                           DWORD flags) {
  if (!FakeArmed())
    return DIERR_INPUTLOST;
  InterlockedExchangeAdd64(&g_diCoopCalls, 1);
  return DI_OK;
}

/**
 * @brief Report one object's details for a DIPH_BYID/BYOFFSET query.
 * @param self The fake device.
 * @param obj Caller DIDEVICEOBJECTINSTANCEW/A (size-prefixed).
 * @param how Object selector.
 * @param dwHow DIPH_* selector kind.
 * @return DI_OK, DIERR_INVALIDPARAM or DIERR_OBJECTNOTFOUND.
 */
static HRESULT STDMETHODCALLTYPE FakeDiGetObjectInfo(void* self, void* obj,
                                                     DWORD how, DWORD dwHow) {
  (void)self;
  if (!obj)
    return DIERR_INVALIDPARAM;
  const DWORD size = *(DWORD*)obj;
  const bool wide = size == sizeof(DIDEVICEOBJECTINSTANCEW);
  if (!wide && size != sizeof(DIDEVICEOBJECTINSTANCEA))
    return DIERR_INVALIDPARAM;
  const GUID* g = nullptr;
  DWORD type = 0, ofs = 0;
  wchar_t nw[32] = {};
  char na[32] = {};
  for (int i = 0; i < kDiFakeAxisCount && !g; ++i) {
    const DWORD t = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(i);
    if ((dwHow == DIPH_BYOFFSET && kDiFakeAxes[i].ofs == how) ||
        (dwHow == DIPH_BYID && t == how)) {
      g = kDiFakeAxes[i].guid;
      type = t;
      ofs = kDiFakeAxes[i].ofs;
      lstrcpyW(nw, L"Axis");
      lstrcpyA(na, "Axis");
    }
  }
  for (int i = 0; i < kDiFakeButtonCount && !g; ++i) {
    const DWORD t = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i);
    const DWORD o = kDiFakeButtons0 + (DWORD)i;
    if ((dwHow == DIPH_BYOFFSET && o == how) ||
        (dwHow == DIPH_BYID && t == how)) {
      g = &kDiGuidButton;
      type = t;
      ofs = o;
      snprintf(na, sizeof(na), "Button %d", i);
      for (int k = 0; k < 32; ++k)
        nw[k] = (wchar_t)(unsigned char)na[k];
    }
  }
  if (!g) {
    const DWORD t = DIDFT_POV | DIDFT_MAKEINSTANCE(0);
    if ((dwHow == DIPH_BYOFFSET && kDiFakePov0 == how) ||
        (dwHow == DIPH_BYID && t == how)) {
      g = &kDiGuidPov;
      type = t;
      ofs = kDiFakePov0;
      lstrcpyW(nw, L"Hat Switch");
      lstrcpyA(na, "Hat Switch");
    }
  }
  if (!g)
    return DIERR_OBJECTNOTFOUND;
  InterlockedExchangeAdd64(&g_diObjInfoCalls, 1);
  const DWORD flags =
      ((type & DIDFT_AXIS) != 0) ? (DIDOI_POLLED | DIDOI_ASPECTPOSITION) : 0;
  if (wide) {
    DIDEVICEOBJECTINSTANCEW* o = (DIDEVICEOBJECTINSTANCEW*)obj;
    o->guidType = *g;
    o->dwOfs = ofs;
    o->dwType = type;
    o->dwFlags = flags;
    lstrcpyW(o->tszName, nw);
  } else {
    DIDEVICEOBJECTINSTANCEA* o = (DIDEVICEOBJECTINSTANCEA*)obj;
    o->guidType = *g;
    o->dwOfs = ofs;
    o->dwType = type;
    o->dwFlags = flags;
    lstrcpyA(o->tszName, na);
  }
  return DI_OK;
}

/**
 * @brief Report the fake pad's instance identity.
 * @param self The fake device.
 * @param inst Caller DIDEVICEINSTANCEW/A (size-prefixed).
 * @return DI_OK or DIERR_INVALIDPARAM.
 */
static HRESULT STDMETHODCALLTYPE FakeDiGetDeviceInfo(void* self, void* inst) {
  (void)self;
  if (!FakeArmed())
    return DIERR_INPUTLOST;
  if (!inst)
    return DIERR_INVALIDPARAM;
  const DWORD size = *(DWORD*)inst;
  InterlockedExchangeAdd64(&g_diInfoCalls, 1);
  if (size == sizeof(DIDEVICEINSTANCEW)) {
    DIDEVICEINSTANCEW* d = (DIDEVICEINSTANCEW*)inst;
    d->guidInstance = FakeDiInstanceGuid();
    d->guidProduct = kDiGuidFakeProduct;
    d->dwDevType = kDiFakeDevType;
    lstrcpyW(d->tszInstanceName, L"Xbox 360 Controller");
    lstrcpyW(d->tszProductName, L"Xbox 360 Controller");
    d->guidFFDriver = kDiGuidEmpty;
    d->wUsagePage = 0x01;
    d->wUsage = 0x05;
    return DI_OK;
  }
  if (size == sizeof(DIDEVICEINSTANCEA)) {
    DIDEVICEINSTANCEA* d = (DIDEVICEINSTANCEA*)inst;
    d->guidInstance = FakeDiInstanceGuid();
    d->guidProduct = kDiGuidFakeProduct;
    d->dwDevType = kDiFakeDevType;
    lstrcpyA(d->tszInstanceName, "Xbox 360 Controller");
    lstrcpyA(d->tszProductName, "Xbox 360 Controller");
    d->guidFFDriver = kDiGuidEmpty;
    d->wUsagePage = 0x01;
    d->wUsage = 0x05;
    return DI_OK;
  }
  return DIERR_INVALIDPARAM;
}

/**
 * @brief Unsupported control-panel entry point.
 * @return DIERR_UNSUPPORTED.
 */
static HRESULT STDMETHODCALLTYPE FakeDiRunControlPanel(void*, HWND, DWORD) {
  return DIERR_UNSUPPORTED;
}

/**
 * @brief Fake device Initialize (always succeeds).
 * @return DI_OK.
 */
static HRESULT STDMETHODCALLTYPE FakeDiInitialize(void*, HINSTANCE, DWORD,
                                                  REFGUID) {
  return DI_OK;
}

/**
 * @brief Force-feedback effect creation is unsupported on the fake pad.
 * @return DIERR_UNSUPPORTED.
 */
static HRESULT STDMETHODCALLTYPE FakeDiCreateEffect(void*, REFGUID, LPCDIEFFECT,
                                                    void**, LPUNKNOWN) {
  return DIERR_UNSUPPORTED;
}

/**
 * @brief Force-feedback effect enumeration (empty).
 * @return DI_OK.
 */
static HRESULT STDMETHODCALLTYPE FakeDiEnumEffects(void*, void*, LPVOID,
                                                   DWORD) {
  return DI_OK;
}

/**
 * @brief Force-feedback effect info is unsupported.
 * @return DIERR_UNSUPPORTED.
 */
static HRESULT STDMETHODCALLTYPE FakeDiGetEffectInfo(void*, void*, REFGUID) {
  return DIERR_UNSUPPORTED;
}

/**
 * @brief Force-feedback state is unsupported.
 * @return DIERR_UNSUPPORTED.
 */
static HRESULT STDMETHODCALLTYPE FakeDiGetForceFeedbackState(void*, DWORD*) {
  return DIERR_UNSUPPORTED;
}

/**
 * @brief Force-feedback commands are unsupported.
 * @return DIERR_UNSUPPORTED.
 */
static HRESULT STDMETHODCALLTYPE FakeDiSendForceFeedbackCommand(void*, DWORD) {
  return DIERR_UNSUPPORTED;
}

/**
 * @brief Created-effect enumeration (empty).
 * @return DI_OK.
 */
static HRESULT STDMETHODCALLTYPE FakeDiEnumCreatedEffectObjects(void*, void*,
                                                                LPVOID, DWORD) {
  return DI_OK;
}

/**
 * @brief Escape IO is unsupported.
 * @return DIERR_UNSUPPORTED.
 */
static HRESULT STDMETHODCALLTYPE FakeDiEscape(void*, LPDIEFFESCAPE) {
  return DIERR_UNSUPPORTED;
}

/**
 * @brief Fake Poll (always succeeds; state is pushed, not polled).
 * @return DI_OK.
 */
static HRESULT STDMETHODCALLTYPE FakeDiPoll(void*) {
  if (!FakeArmed())
    return DIERR_INPUTLOST;
  InterlockedExchangeAdd64(&g_diPollCalls, 1);
  return DI_OK;
}

/**
 * @brief Fake SendDeviceData (accepted, ignored).
 * @return DI_OK.
 */
static HRESULT STDMETHODCALLTYPE FakeDiSendDeviceData(void*, DWORD, LPCDIDEVICEOBJECTDATA,
                                                      LPDWORD, DWORD) {
  return DI_OK;
}

/**
 * @brief Effect-file enumeration is unsupported.
 * @return DIERR_UNSUPPORTED.
 */
static HRESULT STDMETHODCALLTYPE FakeDiEnumEffectsInFile(void*, LPCWSTR, void*,
                                                         LPVOID, DWORD) {
  return DIERR_UNSUPPORTED;
}

/**
 * @brief Effect-file writing is unsupported.
 * @return DIERR_UNSUPPORTED.
 */
static HRESULT STDMETHODCALLTYPE FakeDiWriteEffectToFile(void*, LPCWSTR, DWORD,
                                                         LPCDIFILEEFFECT,
                                                         DWORD) {
  return DIERR_UNSUPPORTED;
}

/**
 * @brief Action-map building is unsupported.
 * @return DIERR_UNSUPPORTED.
 */
static HRESULT STDMETHODCALLTYPE FakeDiBuildActionMap(void*, LPDIACTIONFORMATW,
                                                      LPCWSTR, DWORD) {
  return DIERR_UNSUPPORTED;
}

/**
 * @brief Action-map setting is unsupported.
 * @return DIERR_UNSUPPORTED.
 */
static HRESULT STDMETHODCALLTYPE FakeDiSetActionMap(void*, LPDIACTIONFORMATW,
                                                    LPCWSTR, DWORD) {
  return DIERR_UNSUPPORTED;
}

/**
 * @brief Device image info is unsupported.
 * @return DIERR_UNSUPPORTED.
 */
static HRESULT STDMETHODCALLTYPE FakeDiGetImageInfo(void*,
                                                    LPDIDEVICEIMAGEINFOHEADERW) {
  return DIERR_UNSUPPORTED;
}

/**
 * @brief Build the fake device's 32-slot IDirectInputDevice8W vtable once.
 */
static void EnsureFakeDiDevice(void) {
  if (g_diFakeVtable[0])
    return;
  g_diFakeVtable[0] = (void*)&FakeDiQueryInterface;
  g_diFakeVtable[1] = (void*)&FakeDiAddRef;
  g_diFakeVtable[2] = (void*)&FakeDiRelease;
  g_diFakeVtable[3] = (void*)&FakeDiGetCapabilities;
  g_diFakeVtable[4] = (void*)&FakeDiEnumObjects;
  g_diFakeVtable[5] = (void*)&FakeDiGetProperty;
  g_diFakeVtable[6] = (void*)&FakeDiSetProperty;
  g_diFakeVtable[7] = (void*)&FakeDiAcquire;
  g_diFakeVtable[8] = (void*)&FakeDiUnacquire;
  g_diFakeVtable[9] = (void*)&FakeDiGetDeviceState;
  g_diFakeVtable[10] = (void*)&FakeDiGetDeviceData;
  g_diFakeVtable[11] = (void*)&FakeDiSetDataFormat;
  g_diFakeVtable[12] = (void*)&FakeDiSetEventNotification;
  g_diFakeVtable[13] = (void*)&FakeDiSetCooperativeLevel;
  g_diFakeVtable[14] = (void*)&FakeDiGetObjectInfo;
  g_diFakeVtable[15] = (void*)&FakeDiGetDeviceInfo;
  g_diFakeVtable[16] = (void*)&FakeDiRunControlPanel;
  g_diFakeVtable[17] = (void*)&FakeDiInitialize;
  g_diFakeVtable[18] = (void*)&FakeDiCreateEffect;
  g_diFakeVtable[19] = (void*)&FakeDiEnumEffects;
  g_diFakeVtable[20] = (void*)&FakeDiGetEffectInfo;
  g_diFakeVtable[21] = (void*)&FakeDiGetForceFeedbackState;
  g_diFakeVtable[22] = (void*)&FakeDiSendForceFeedbackCommand;
  g_diFakeVtable[23] = (void*)&FakeDiEnumCreatedEffectObjects;
  g_diFakeVtable[24] = (void*)&FakeDiEscape;
  g_diFakeVtable[25] = (void*)&FakeDiPoll;
  g_diFakeVtable[26] = (void*)&FakeDiSendDeviceData;
  g_diFakeVtable[27] = (void*)&FakeDiEnumEffectsInFile;
  g_diFakeVtable[28] = (void*)&FakeDiWriteEffectToFile;
  g_diFakeVtable[29] = (void*)&FakeDiBuildActionMap;
  g_diFakeVtable[30] = (void*)&FakeDiSetActionMap;
  g_diFakeVtable[31] = (void*)&FakeDiGetImageInfo;
}

/* ---- IDirectInput8 interface hooks ------------------------------------ */

/// IDirectInput8::CreateDevice signature.
typedef HRESULT(STDMETHODCALLTYPE* FnDiCreateDevice)(void*, REFGUID, void**,
                                                     void*);
/// IDirectInput8::EnumDevices signature.
typedef HRESULT(STDMETHODCALLTYPE* FnDiEnumDevices)(void*, DWORD, void*, LPVOID,
                                                    DWORD);
/// IDirectInput8::GetDeviceStatus signature.
typedef HRESULT(STDMETHODCALLTYPE* FnDiGetDeviceStatus)(void*, REFGUID);
/// COM Release signature.
typedef ULONG(STDMETHODCALLTYPE* FnDiRelease)(void*);

static FnDiCreateDevice g_realDiCreateDeviceW = nullptr; ///< W trampoline.
static FnDiCreateDevice g_realDiCreateDeviceA = nullptr; ///< A trampoline.
static FnDiEnumDevices g_realDiEnumDevicesW = nullptr;   ///< W trampoline.
static FnDiEnumDevices g_realDiEnumDevicesA = nullptr;   ///< A trampoline.
static FnDiGetDeviceStatus g_realDiGetStatusW = nullptr; ///< W trampoline.
static FnDiGetDeviceStatus g_realDiGetStatusA = nullptr; ///< A trampoline.
static Detour g_diDetCreateW, g_diDetCreateA;            ///< CreateDevice detours.
static Detour g_diDetEnumW, g_diDetEnumA;                ///< EnumDevices detours.
static Detour g_diDetStatusW, g_diDetStatusA;            ///< GetDeviceStatus detours.
static volatile LONG g_diHookedW = 0;                    ///< 1 once W slot set is hooked.
static volatile LONG g_diHookedA = 0;                    ///< 1 once A slot set is hooked.

/**
 * @brief Fill a DIDEVICEINSTANCEW/A for the fake pad.
 * @param inst Size-prefixed caller struct.
 * @param wide True for the W flavour.
 */
static void FakeDiFillInstance(void* inst, bool wide) {
  if (wide) {
    DIDEVICEINSTANCEW* d = (DIDEVICEINSTANCEW*)inst;
    d->guidInstance = FakeDiInstanceGuid();
    d->guidProduct = kDiGuidFakeProduct;
    d->dwDevType = kDiFakeInstanceDevType;
    lstrcpyW(d->tszInstanceName, L"Xbox 360 Controller");
    lstrcpyW(d->tszProductName, L"Xbox 360 Controller");
    d->guidFFDriver = kDiGuidEmpty;
    d->wUsagePage = 0x01;
    d->wUsage = 0x05;
  } else {
    DIDEVICEINSTANCEA* d = (DIDEVICEINSTANCEA*)inst;
    d->guidInstance = FakeDiInstanceGuid();
    d->guidProduct = kDiGuidFakeProduct;
    d->dwDevType = kDiFakeInstanceDevType;
    lstrcpyA(d->tszInstanceName, "Xbox 360 Controller");
    lstrcpyA(d->tszProductName, "Xbox 360 Controller");
    d->guidFFDriver = kDiGuidEmpty;
    d->wUsagePage = 0x01;
    d->wUsage = 0x05;
  }
}

/**
 * @brief W-flavour CreateDevice: hand out the fake pad for its GUID.
 * @param self IDirectInput8W.
 * @param guid Requested device GUID.
 * @param out Receives the device pointer.
 * @param outer Aggregation outer (unused).
 * @return DI_OK for the fake pad, otherwise the original result.
 */
static HRESULT STDMETHODCALLTYPE DiCreateDeviceW_Hook(void* self, REFGUID guid,
                                                      void** out, void* outer) {
  if (g_diFakeOn == 1 && FakeArmed() && out && IsFakeDiGuid(guid)) {
    InterlockedExchangeAdd64(&g_diCreateCalls, 1);
    InterlockedExchange(&g_diFakeWide, 1);
    EnsureFakeDiDevice();
    *out = &g_diFakeDevice;
    InterlockedIncrement(&g_diFakeDevice.refs);
    return DI_OK;
  }
  return g_realDiCreateDeviceW(self, guid, out, outer);
}

/**
 * @brief A-flavour CreateDevice: hand out the fake pad for its GUID.
 * @param self IDirectInput8A.
 * @param guid Requested device GUID.
 * @param out Receives the device pointer.
 * @param outer Aggregation outer (unused).
 * @return DI_OK for the fake pad, otherwise the original result.
 */
static HRESULT STDMETHODCALLTYPE DiCreateDeviceA_Hook(void* self, REFGUID guid,
                                                      void** out, void* outer) {
  if (g_diFakeOn == 1 && FakeArmed() && out && IsFakeDiGuid(guid)) {
    InterlockedExchangeAdd64(&g_diCreateCalls, 1);
    InterlockedExchange(&g_diFakeWide, 0);
    EnsureFakeDiDevice();
    *out = &g_diFakeDevice;
    InterlockedIncrement(&g_diFakeDevice.refs);
    return DI_OK;
  }
  return g_realDiCreateDeviceA(self, guid, out, outer);
}

/**
 * @brief W-flavour EnumDevices: append the fake pad to gamepad enumerations.
 * @param self IDirectInput8W.
 * @param devClass DI8DEVCLASS_* filter.
 * @param cb Game callback.
 * @param ctx Callback context.
 * @param flags Enumeration flags.
 * @return The original call's result.
 */
static HRESULT STDMETHODCALLTYPE DiEnumDevicesW_Hook(void* self, DWORD devClass,
                                                     void* cb, LPVOID ctx,
                                                     DWORD flags) {
  InterlockedExchangeAdd64(&g_diEnumCalls, 1);
  const HRESULT hr = g_realDiEnumDevicesW(self, devClass, cb, ctx, flags);
  if (g_diFakeOn != 1 || !FakeArmed() || !cb || FAILED(hr))
    return hr;
  if (devClass != DI8DEVCLASS_ALL && devClass != DI8DEVCLASS_GAMECTRL)
    return hr;
  DIDEVICEINSTANCEW inst = {};
  inst.dwSize = sizeof(inst);
  FakeDiFillInstance(&inst, true);
  ((BOOL(WINAPI*)(const DIDEVICEINSTANCEW*, LPVOID))cb)(&inst, ctx);
  return hr;
}

/**
 * @brief A-flavour EnumDevices: append the fake pad to gamepad enumerations.
 * @param self IDirectInput8A.
 * @param devClass DI8DEVCLASS_* filter.
 * @param cb Game callback.
 * @param ctx Callback context.
 * @param flags Enumeration flags.
 * @return The original call's result.
 */
static HRESULT STDMETHODCALLTYPE DiEnumDevicesA_Hook(void* self, DWORD devClass,
                                                     void* cb, LPVOID ctx,
                                                     DWORD flags) {
  InterlockedExchangeAdd64(&g_diEnumCalls, 1);
  const HRESULT hr = g_realDiEnumDevicesA(self, devClass, cb, ctx, flags);
  if (g_diFakeOn != 1 || !FakeArmed() || !cb || FAILED(hr))
    return hr;
  if (devClass != DI8DEVCLASS_ALL && devClass != DI8DEVCLASS_GAMECTRL)
    return hr;
  DIDEVICEINSTANCEA inst = {};
  inst.dwSize = sizeof(inst);
  FakeDiFillInstance(&inst, false);
  ((BOOL(WINAPI*)(const DIDEVICEINSTANCEA*, LPVOID))cb)(&inst, ctx);
  return hr;
}

/**
 * @brief W-flavour GetDeviceStatus: the fake pad is always "attached".
 * @param self IDirectInput8W.
 * @param guid Device GUID.
 * @return DI_OK for the fake pad, otherwise the original result.
 */
static HRESULT STDMETHODCALLTYPE DiGetStatusW_Hook(void* self, REFGUID guid) {
  InterlockedIncrement64(&g_diStatusCalls);
  if (g_diFakeOn == 1 && FakeArmed() && IsFakeDiGuid(guid))
    return DI_OK;
  return g_realDiGetStatusW(self, guid);
}

/**
 * @brief A-flavour GetDeviceStatus: the fake pad is always "attached".
 * @param self IDirectInput8A.
 * @param guid Device GUID.
 * @return DI_OK for the fake pad, otherwise the original result.
 */
static HRESULT STDMETHODCALLTYPE DiGetStatusA_Hook(void* self, REFGUID guid) {
  InterlockedIncrement64(&g_diStatusCalls);
  if (g_diFakeOn == 1 && FakeArmed() && IsFakeDiGuid(guid))
    return DI_OK;
  return g_realDiGetStatusA(self, guid);
}

/// IDirectInput8 vtable slot of CreateDevice.
static const int kDiSlotCreateDevice = 3;
/// IDirectInput8 vtable slot of EnumDevices.
static const int kDiSlotEnumDevices = 4;
/// IDirectInput8 vtable slot of GetDeviceStatus.
static const int kDiSlotGetDeviceStatus = 5;

/*
 * Translated from the reference capture-hook (HookInput DI install, the
 * reference's HookInput.cs): dinput8 builds every IDirectInput8 object with
 * the same method implementations, so patching the IMPLEMENTATION addresses
 * read out of a throw-away object covers the game's own objects without
 * touching any vtable. DirectInput8Create itself is deliberately not hooked.
 */
/**
 * @brief Create a throw-away IDirectInput8 to read its method addresses.
 * @param create DirectInput8Create pointer.
 * @param iid IID to request.
 * @return A live interface pointer, or null.
 */
typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*,
                                              LPUNKNOWN);

/**
 * @brief Install the DirectInput8 fake-pad layer (only with XInputFakePad).
 *
 * Loads dinput8, reads the IDirectInput8 implementation addresses from a
 * temporary object and detours CreateDevice/EnumDevices/GetDeviceStatus
 * (W and A). The fake pad is then appended to gamepad enumerations, handed
 * out by CreateDevice and reported attached by GetDeviceStatus. Just before
 * returning, a WM_DEVICECHANGE / DBT_DEVNODES_CHANGED is posted to the
 * target window so a running game re-scans DirectInput (the same signal a
 * real hot-plug produces) and finds the pad.
 */
static void Di8HookInstall(void) {
  if (g_xinputFakeOn != 1) {
    Dlog("di8: skipped (XInputFakePad not set)");
    return;
  }
  HMODULE dinput = LoadLibraryA("dinput8.dll");
  if (!dinput) {
    Dlog("di8: dinput8.dll unavailable");
    return;
  }
  DirectInput8Create_t create =
      (DirectInput8Create_t)GetProcAddress(dinput, "DirectInput8Create");
  if (!create) {
    Dlog("di8: no DirectInput8Create export");
    return;
  }
  EnsureFakeDiDevice();

  void* diW = nullptr;
  if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, kIidDi8W,
                    &diW, nullptr)) ||
      !diW) {
    Dlog("di8: temporary IDirectInput8W creation failed");
    return;
  }
  void** vtW = *(void***)diW;
  if (DetourInstall(vtW[kDiSlotCreateDevice], (void*)&DiCreateDeviceW_Hook,
                    (void**)&g_realDiCreateDeviceW, &g_diDetCreateW))
    Dlog("di8: [W] CreateDevice hooked");
  else
    Dlog("di8: [W] CreateDevice hook failed");
  if (DetourInstall(vtW[kDiSlotEnumDevices], (void*)&DiEnumDevicesW_Hook,
                    (void**)&g_realDiEnumDevicesW, &g_diDetEnumW))
    Dlog("di8: [W] EnumDevices hooked");
  else
    Dlog("di8: [W] EnumDevices hook failed");
  if (DetourInstall(vtW[kDiSlotGetDeviceStatus], (void*)&DiGetStatusW_Hook,
                    (void**)&g_realDiGetStatusW, &g_diDetStatusW))
    Dlog("di8: [W] GetDeviceStatus hooked");
  else
    Dlog("di8: [W] GetDeviceStatus hook failed");
  InterlockedExchange(&g_diHookedW, 1);

  void* diA = nullptr;
  if (SUCCEEDED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, kIidDi8A,
                       &diA, nullptr)) &&
      diA) {
    void** vtA = *(void***)diA;
    if (vtA[kDiSlotCreateDevice] != vtW[kDiSlotCreateDevice] &&
        DetourInstall(vtA[kDiSlotCreateDevice], (void*)&DiCreateDeviceA_Hook,
                      (void**)&g_realDiCreateDeviceA, &g_diDetCreateA))
      Dlog("di8: [A] CreateDevice hooked");
    if (vtA[kDiSlotEnumDevices] != vtW[kDiSlotEnumDevices] &&
        DetourInstall(vtA[kDiSlotEnumDevices], (void*)&DiEnumDevicesA_Hook,
                      (void**)&g_realDiEnumDevicesA, &g_diDetEnumA))
      Dlog("di8: [A] EnumDevices hooked");
    if (vtA[kDiSlotGetDeviceStatus] != vtW[kDiSlotGetDeviceStatus] &&
        DetourInstall(vtA[kDiSlotGetDeviceStatus], (void*)&DiGetStatusA_Hook,
                      (void**)&g_realDiGetStatusA, &g_diDetStatusA))
      Dlog("di8: [A] GetDeviceStatus hooked");
    InterlockedExchange(&g_diHookedA, 1);
    ((FnDiRelease)vtA[2])(diA);
  }

  ((FnDiRelease)vtW[2])(diW);
  InterlockedExchange(&g_diFakeOn, 1);
  Dlog("di8: fake gamepad armed (W=%d A=%d)", g_diHookedW, g_diHookedA);
  /* A real hot-plug announces itself by broadcasting
     WM_DEVICECHANGE/DBT_DEVNODES_CHANGED; without a driver there is no such
     event, so the worker asks HotPlugNudgeThread() to synthesise a sequence
     of them AFTER every hook layer is live - that is what makes a running
     game re-run its complete device scan (XInput + DirectInput) against the
     fake pad. */
}

/* ------------- Raw-input fake HID device ------------------------------- */

/**
 * @brief Serve a RIDI_DEVICENAME query for the fake HID device.
 *
 * The name is the canonical device path of a wired Xbox 360 controller; an
 * engine that parses VID/PID out of it (the classic RawInput HID flow) sees
 * 045E:028E, i.e. "this is an Xbox pad" - the step that lets it bind its
 * XInput slot afterwards.
 * @param data Output buffer, NULL when probing the size.
 * @param size In/out capacity in bytes.
 * @param wide Whether the caller used the Unicode flavour.
 * @return Bytes copied (0 = size probe), like the real API.
 */
static UINT RawFakeDeviceName(LPVOID data, PUINT size, bool wide) {
  const void* name = wide ? (const void*)kRawFakeDeviceNameW
                          : (const void*)kRawFakeDeviceNameA;
  const UINT need =
      (UINT)(wide ? (wcslen(kRawFakeDeviceNameW) + 1) * sizeof(WCHAR)
                  : (strlen(kRawFakeDeviceNameA) + 1));
  if (!data) {
    if (size)
      *size = need;
    return 0;
  }
  if (!size)
    return 0;
  if (*size < need) {
    *size = need;
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return (UINT)-1;
  }
  memcpy(data, name, need);
  *size = need;
  return need;
}

/**
 * @brief Serve a RIDI_DEVICEINFO query for the fake HID device.
 *
 * Reports a RIM_TYPEHID device with Microsoft's vendor ID 045E and the Xbox
 * 360 controller's product ID 028E and the gamepad usage page/usage, exactly
 * what the RIDI_DEVICEINFO-based "is this an Xbox controller?" check looks
 * at.
 * @param data Output buffer, NULL when probing the size.
 * @param size In/out capacity in bytes.
 * @return Bytes copied (0 = size probe), like the real API.
 */
static UINT RawFakeDeviceInfo(LPVOID data, PUINT size) {
  const UINT need = (UINT)sizeof(RID_DEVICE_INFO);
  if (!data) {
    if (size)
      *size = need;
    return 0;
  }
  if (!size)
    return 0;
  if (*size < need) {
    *size = need;
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return (UINT)-1;
  }
  RID_DEVICE_INFO rdi;
  memset(&rdi, 0, sizeof(rdi));
  rdi.cbSize = sizeof(rdi);
  rdi.dwType = RIM_TYPEHID;
  rdi.hid.dwVendorId = 0x045E;  /* Microsoft */
  rdi.hid.dwProductId = 0x028E; /* Xbox 360 Controller */
  rdi.hid.dwVersionNumber = 0x0112;
  rdi.hid.usUsagePage = 0x01; /* Generic Desktop */
  rdi.hid.usUsage = 0x05;     /* Game Pad */
  memcpy(data, &rdi, need);
  *size = need;
  return need;
}

/**
 * @brief Serve one GetRawInputDeviceInfo command for the fake device.
 * @param cmd The RIDI_ command.
 * @param data Output buffer, NULL when probing the size.
 * @param size In/out capacity in bytes.
 * @param wide Whether the caller used the Unicode flavour.
 * @return Bytes copied, 0 for a size probe, (UINT)-1 on error.
 */
static UINT RawFakeInfoReply(UINT cmd, LPVOID data, PUINT size, bool wide) {
  InterlockedIncrement64(&g_rawInfoCalls);
  switch (cmd) {
  case RIDI_DEVICENAME:
    return RawFakeDeviceName(data, size, wide);
  case RIDI_DEVICEINFO:
    return RawFakeDeviceInfo(data, size);
  case RIDI_PREPARSEDDATA:
    /* The HID report descriptor is deliberately not faked: an engine that
       reads pad state through the HID layer (rather than binding XInput)
       is out of scope, and inventing a descriptor would corrupt its reads. */
    Dlog("raw-input: PreparsedData requested for fake device (not faked)");
    if (size)
      *size = 0;
    SetLastError(ERROR_INVALID_DATA);
    return (UINT)-1;
  default:
    break;
  }
  return (UINT)-1;
}

/**
 * @brief Hooked GetRawInputDeviceInfoW: serve the fake device's identity.
 * @param hDev Device handle.
 * @param cmd RIDI_ command.
 * @param data Output buffer, NULL when probing the size.
 * @param size In/out capacity in bytes.
 * @return Like the real API.
 */
static UINT WINAPI MyGetRawInputDeviceInfoW(HANDLE hDev, UINT cmd, LPVOID data,
                                            PUINT size) {
  if (g_rawFakeOn == 1 && FakeArmed() && hDev == kRawFakeDevice)
    return RawFakeInfoReply(cmd, data, size, true);
  if (g_realGetRawInputDeviceInfoW)
    return g_realGetRawInputDeviceInfoW(hDev, cmd, data, size);
  return 0;
}

/**
 * @brief Hooked GetRawInputDeviceInfoA: serve the fake device's identity.
 * @param hDev Device handle.
 * @param cmd RIDI_ command.
 * @param data Output buffer, NULL when probing the size.
 * @param size In/out capacity in bytes.
 * @return Like the real API.
 */
static UINT WINAPI MyGetRawInputDeviceInfoA(HANDLE hDev, UINT cmd, LPVOID data,
                                            PUINT size) {
  if (g_rawFakeOn == 1 && FakeArmed() && hDev == kRawFakeDevice)
    return RawFakeInfoReply(cmd, data, size, false);
  if (g_realGetRawInputDeviceInfoA)
    return g_realGetRawInputDeviceInfoA(hDev, cmd, data, size);
  return 0;
}

/**
 * @brief Hooked GetRawInputDeviceList: append the fake HID device.
 *
 * The size-probe phase (NULL buffer) reports the real device count plus the
 * fake one; the fill phase passes the caller through to the real API and
 * appends the fake device after the real entries. Per the SDK, a NULL
 * buffer returns 0 (with the count in *num), a too-small buffer returns
 * (UINT)-1 with the required count, success returns the device count.
 * @param list Output array, NULL when probing the size.
 * @param num In/out device count.
 * @param cb Size of a RAWINPUTDEVICELIST entry.
 * @return Like the real API.
 */
static UINT WINAPI MyGetRawInputDeviceList(PRAWINPUTDEVICELIST list, PUINT num,
                                           UINT cb) {
  InterlockedIncrement64(&g_rawListCalls);
  if (g_realGetRawInputDeviceList == 0) {
    if (num)
      *num = 0;
    return (UINT)-1;
  }
  UINT realCount = 0;
  g_realGetRawInputDeviceList(NULL, &realCount, cb);
  const bool armed = FakeArmed();
  const UINT total = realCount + (g_rawFakeOn == 1 && armed ? 1u : 0u);
  if (!list) {
    if (num)
      *num = total;
    return 0;
  }
  if (!num)
    return (UINT)-1;
  if (*num < total) {
    *num = total;
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return (UINT)-1;
  }
  UINT got = g_realGetRawInputDeviceList(list, num, cb);
  if (got == (UINT)-1)
    return got;
  if (g_rawFakeOn == 1 && armed && cb >= sizeof(RAWINPUTDEVICELIST)) {
    RAWINPUTDEVICELIST fe;
    fe.hDevice = kRawFakeDevice;
    fe.dwType = RIM_TYPEHID;
    memcpy(&list[got], &fe, sizeof(fe));
    got++;
  }
  *num = got;
  return got;
}

/**
 * @brief Hooked RegisterRawInputDevices: pass through untouched.
 *
 * The game only registers the device classes it wants data for ("raw mouse",
 * "raw keyboard", "gamepad"), never specific fake handles, so a pass-through
 * is the correct behaviour - the fake device needs no registration because
 * it never produces real WM_INPUT records.
 * @param devs Registration array.
 * @param count Number of registrations.
 * @param cb Size of a RAWINPUTDEVICE entry.
 * @return The real API's result.
 */
static BOOL WINAPI MyRegisterRawInputDevices(PCRAWINPUTDEVICE devs, UINT count,
                                             UINT cb) {
  InterlockedIncrement64(&g_rawRegCalls);
  if (g_realRegisterRawInputDevices)
    return g_realRegisterRawInputDevices(devs, count, cb);
  SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
  return FALSE;
}

/**
 * @brief Install the raw-input fake-HID layer (only with XInputFakePad).
 *
 * Detours user32!GetRawInputDeviceList / GetRawInputDeviceInfoA/W so a
 * running engine that rescans the system device tree after a hot-plug nudge
 * sees one wired Xbox 360 HID device in addition to the real keyboard and
 * mouse. The DI-only fake is invisible to that scan, which is the gap this
 * layer closes: with the raw tree reporting an Xbox pad, the game finally
 * has a reason to bind its XInput slot, whose presence/state APIs are
 * already faked. The layer is flag-gated (g_rawFakeOn) like the DI layer and
 * never re-patched.
 */
static void RawFakeInstall(void) {
  if (XInputFakePadEnabled() != 1) {
    Dlog("raw-input: fake HID device skipped (XInputFakePad=0)");
    return;
  }
  InterlockedExchange(&g_rawFakeOn, 1);
  bool ok1 = DetourInstall((void*)g_exportGetRawInputDeviceList,
                           (void*)MyGetRawInputDeviceList,
                           (void**)&g_realGetRawInputDeviceList, &g_detourRawList);
  Dlog(ok1 ? "raw-input: GetRawInputDeviceList hooked"
           : "raw-input: GetRawInputDeviceList skipped");
  bool ok2 = DetourInstall((void*)g_exportGetRawInputDeviceInfoW,
                           (void*)MyGetRawInputDeviceInfoW,
                           (void**)&g_realGetRawInputDeviceInfoW, &g_detourRawInfoW);
  bool ok3 = DetourInstall((void*)g_exportGetRawInputDeviceInfoA,
                           (void*)MyGetRawInputDeviceInfoA,
                           (void**)&g_realGetRawInputDeviceInfoA, &g_detourRawInfoA);
  Dlog(ok2 && ok3 ? "raw-input: GetRawInputDeviceInfoW/A hooked"
                  : "raw-input: GetRawInputDeviceInfoW/A hook failed (W=%d A=%d)",
       ok2 ? 1 : 0, ok3 ? 1 : 0);
  bool ok4 = DetourInstall((void*)g_exportRegisterRawInputDevices,
                           (void*)MyRegisterRawInputDevices,
                           (void**)&g_realRegisterRawInputDevices, &g_detourRawReg);
  Dlog(ok4 ? "raw-input: RegisterRawInputDevices hooked"
           : "raw-input: RegisterRawInputDevices skipped");
  Dlog("raw-input: fake X360 HID device armed (vid=045e pid=028e)");
}

/* ------------------------------------------------------------------ */
/* WMI Win32_PNPEntity fake (the "is this an XInput pad?" filter)      */
/* ------------------------------------------------------------------ */
// A game that supports DirectInput and XInput side by side has to tell real
// XInput pads apart from legacy pads, because XInput pads also enumerate
// through DirectInput. The documented method is to enumerate every PNP device
// through WMI (Win32_PNPEntity), look for the "IG_" interface marker in
// DeviceID (e.g. "VID_045E&PID_028E&IG_00"), parse VID/PID and drop every
// DirectInput device whose guidProduct equals MAKELONG(VID, PID). DMC5 (RE
// Engine) does exactly that; without an "IG_" entry for our virtual pad the
// game keeps the pad on DirectInput and never calls our XInput serve hooks.
//
// So we append one synthetic Win32_PNPEntity object whose DeviceID is the
// Xbox 360 HID path. The game's filter then removes the virtual pad and it
// falls through to XInput. DirectInput-only titles never run this filter, so
// they keep getting the pad; nothing about the DirectInput path changes.

/// CLSID_WbemLocator (the in-proc WMI locator).
static const GUID kClsidWbemLocator = {0x4590F811, 0x1D3A, 0x11D0,
                                      {0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};
/// IID_IWbemLocator.
static const IID kIidWbemLocator = {0xDC12A687, 0x737F, 0x11CF,
                                    {0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};
/// IID_IWbemServices.
static const IID kIidWbemServices = {0x9556DC99, 0x828C, 0x11CF,
                                     {0xA3, 0x7E, 0x00, 0xAA, 0x00, 0x24, 0xAE, 0x2B}};
/// The WMI class the XInput filter enumerates.
static const wchar_t kFakePnpClass[] = L"Win32_PNPEntity";
/// The synthetic entity's DeviceID, carrying the IG_ XInput marker.
static const wchar_t kFakePnpDeviceId[] = L"HID\\VID_045E&PID_028E&IG_00\\7&1&0&0000";

/// Live WMI hook-hit counter (reported as "wmicalls" in the heartbeat).
static volatile LONG g_wmiCallLog = 0;

/**
 * @brief Log an install-time WMI message to the worker log.
 * @param fmt printf-style format.
 */
static void WmiLog(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Dlog("%s", buf);
}

/**
 * @brief Note a live WMI hook hit; counted, never logged.
 */
static void WmiNoteCall(void) {
  InterlockedIncrement(&g_wmiCallLog);
}

/// Minimal fake IWbemClassObject (vtable + refs).
struct FakeWbemObject {
  void** vtable;      ///< Points at g_fakeWbemVtable.
  volatile LONG refs; ///< Reference count.
};

static HRESULT STDMETHODCALLTYPE FakeWbem_QueryInterface(FakeWbemObject* self, REFIID,
                                                         void** out) {
  if (out == nullptr)
    return E_POINTER;
  *out = self;
  return S_OK;
}
static ULONG STDMETHODCALLTYPE FakeWbem_AddRef(FakeWbemObject* self) {
  return (ULONG)InterlockedIncrement(&self->refs);
}
static ULONG STDMETHODCALLTYPE FakeWbem_Release(FakeWbemObject* self) {
  return (ULONG)InterlockedDecrement(&self->refs);
}

/**
 * @brief Answer property reads on the synthetic PNP entity.
 *
 * The XInput filter reads the device instance path, which is what carries the
 * "IG_" marker. Answer both spellings WMI exposes it under.
 * @param name Property name.
 * @param value Receives the value when non-null.
 * @param type Receives the CIM type when non-null.
 * @param flavor Receives the flavor when non-null.
 * @return WBEM_S_NO_ERROR for DeviceID/PNPDeviceID, WBEM_E_NOT_FOUND otherwise.
 */
static HRESULT STDMETHODCALLTYPE FakeWbem_Get(FakeWbemObject*, LPCWSTR name, LONG,
                                              VARIANT* value, CIMTYPE* type,
                                              LONG* flavor) {
  if (name != nullptr && (_wcsicmp(name, L"DeviceID") == 0 ||
                          _wcsicmp(name, L"PNPDeviceID") == 0)) {
    if (value != nullptr) {
      VariantInit(value);
      value->vt = VT_BSTR;
      value->bstrVal = SysAllocString(kFakePnpDeviceId);
      if (value->bstrVal == nullptr)
        return E_OUTOFMEMORY;
    }
    if (type != nullptr)
      *type = CIM_STRING;
    if (flavor != nullptr)
      *flavor = 0;
    return WBEM_S_NO_ERROR;
  }
  if (value != nullptr)
    VariantInit(value);
  return WBEM_E_NOT_FOUND;
}

/// Generate one not-supported stub for the unused IWbemClassObject slots.
#define FAKE_WBEM_STUB(fn, ...)                                                      \
  static HRESULT STDMETHODCALLTYPE fn(__VA_ARGS__) {                                 \
    return WBEM_E_NOT_SUPPORTED;                                                     \
  }
FAKE_WBEM_STUB(FakeWbem_GetQualifierSet, IWbemQualifierSet**)
FAKE_WBEM_STUB(FakeWbem_Put, LPCWSTR, LONG, VARIANT*, CIMTYPE)
FAKE_WBEM_STUB(FakeWbem_Delete, LPCWSTR, LONG)
FAKE_WBEM_STUB(FakeWbem_GetNames, LPCWSTR, LONG, VARIANT*, SAFEARRAY**)
FAKE_WBEM_STUB(FakeWbem_BeginEnumeration, LONG)
FAKE_WBEM_STUB(FakeWbem_Next, LONG, BSTR*, VARIANT*, CIMTYPE*, LONG*)
FAKE_WBEM_STUB(FakeWbem_EndEnumeration, void)
FAKE_WBEM_STUB(FakeWbem_GetPropertyQualifierSet, LPCWSTR, IWbemQualifierSet**)
FAKE_WBEM_STUB(FakeWbem_Clone, IWbemClassObject**)
FAKE_WBEM_STUB(FakeWbem_GetObjectText, LONG, BSTR*)
FAKE_WBEM_STUB(FakeWbem_SpawnDerivedClass, LONG, IWbemClassObject**)
FAKE_WBEM_STUB(FakeWbem_SpawnInstance, LONG, IWbemClassObject**)
FAKE_WBEM_STUB(FakeWbem_CompareTo, LONG, IWbemClassObject*)
FAKE_WBEM_STUB(FakeWbem_GetPropertyOrigin, LPCWSTR, BSTR*)
FAKE_WBEM_STUB(FakeWbem_InheritsFrom, LPCWSTR)
FAKE_WBEM_STUB(FakeWbem_GetMethod, LPCWSTR, LONG, IWbemClassObject**, IWbemClassObject**)
FAKE_WBEM_STUB(FakeWbem_PutMethod, LPCWSTR, LONG, IWbemClassObject*, IWbemClassObject*)
FAKE_WBEM_STUB(FakeWbem_DeleteMethod, LPCWSTR)
FAKE_WBEM_STUB(FakeWbem_BeginMethodEnumeration, LONG)
FAKE_WBEM_STUB(FakeWbem_NextMethod, LONG, BSTR*, IWbemClassObject**, IWbemClassObject**)
FAKE_WBEM_STUB(FakeWbem_EndMethodEnumeration, void)
FAKE_WBEM_STUB(FakeWbem_GetMethodQualifierSet, LPCWSTR, IWbemQualifierSet**)
FAKE_WBEM_STUB(FakeWbem_GetMethodOrigin, LPCWSTR, BSTR*)
#undef FAKE_WBEM_STUB

/// IWbemClassObject method table (27 slots), built by hand.
static void* g_fakeWbemVtable[27] = {
    (void*)&FakeWbem_QueryInterface,          // 0
    (void*)&FakeWbem_AddRef,                  // 1
    (void*)&FakeWbem_Release,                 // 2
    (void*)&FakeWbem_GetQualifierSet,         // 3
    (void*)&FakeWbem_Get,                     // 4
    (void*)&FakeWbem_Put,                     // 5
    (void*)&FakeWbem_Delete,                  // 6
    (void*)&FakeWbem_GetNames,                // 7
    (void*)&FakeWbem_BeginEnumeration,        // 8
    (void*)&FakeWbem_Next,                    // 9
    (void*)&FakeWbem_EndEnumeration,          // 10
    (void*)&FakeWbem_GetPropertyQualifierSet, // 11
    (void*)&FakeWbem_Clone,                   // 12
    (void*)&FakeWbem_GetObjectText,           // 13
    (void*)&FakeWbem_SpawnDerivedClass,       // 14
    (void*)&FakeWbem_SpawnInstance,           // 15
    (void*)&FakeWbem_CompareTo,               // 16
    (void*)&FakeWbem_GetPropertyOrigin,       // 17
    (void*)&FakeWbem_InheritsFrom,            // 18
    (void*)&FakeWbem_GetMethod,               // 19
    (void*)&FakeWbem_PutMethod,               // 20
    (void*)&FakeWbem_DeleteMethod,            // 21
    (void*)&FakeWbem_BeginMethodEnumeration,  // 22
    (void*)&FakeWbem_NextMethod,              // 23
    (void*)&FakeWbem_EndMethodEnumeration,    // 24
    (void*)&FakeWbem_GetMethodQualifierSet,   // 25
    (void*)&FakeWbem_GetMethodOrigin,         // 26
};
/// The singleton synthetic entity, handed out by FakeEnum_Next.
static FakeWbemObject g_fakeWbemObject = {g_fakeWbemVtable, 1};

/// IEnumWbemClassObject wrapper; owns an optional real enumerator.
struct FakeWbemEnum {
  void** vtable;                       ///< Points at g_fakeWbemEnumVtable.
  IEnumWbemClassObject* real;          ///< Real enumerator to fall through to.
  volatile LONG refs;                  ///< Reference count.
  bool injected;                       ///< 1 once the synthetic entity was handed out.
};

static HRESULT STDMETHODCALLTYPE FakeEnum_QueryInterface(FakeWbemEnum* self, REFIID,
                                                         void** out) {
  if (out == nullptr)
    return E_POINTER;
  *out = self;
  return S_OK;
}
static ULONG STDMETHODCALLTYPE FakeEnum_AddRef(FakeWbemEnum* self) {
  return (ULONG)InterlockedIncrement(&self->refs);
}
static ULONG STDMETHODCALLTYPE FakeEnum_Release(FakeWbemEnum* self) {
  const LONG refs = InterlockedDecrement(&self->refs);
  if (refs == 0) {
    if (self->real != nullptr)
      self->real->Release();
    delete self;
  }
  return (ULONG)refs;
}
static HRESULT STDMETHODCALLTYPE FakeEnum_Reset(FakeWbemEnum* self) {
  if (self->real == nullptr)
    return WBEM_S_NO_ERROR;
  return self->real->Reset();
}

/**
 * @brief Enumerate: real rows first, then the synthetic PNP entity once the
 * real enumerator stops short of the requested count.
 * @param self The wrapper.
 * @param timeout Timeout hint forwarded to the real enumerator.
 * @param count Objects requested.
 * @param objects Caller array.
 * @param returned Receives the rows returned.
 * @return The real HRESULT, or WBEM_S_NO_ERROR after injecting.
 */
static HRESULT STDMETHODCALLTYPE FakeEnum_Next(FakeWbemEnum* self, LONG timeout,
                                               ULONG count,
                                               IWbemClassObject** objects,
                                               ULONG* returned) {
  if (returned == nullptr)
    return E_POINTER;
  ULONG got = 0;
  HRESULT hr = WBEM_S_FALSE;
  if (self->real != nullptr) {
    hr = self->real->Next(timeout, count, objects, &got);
    if (FAILED(hr)) {
      *returned = 0;
      return hr;
    }
  }
  if (objects == nullptr) {
    *returned = 0;
    return hr;
  }
  if (self->injected || got >= count) {
    *returned = got;
    return hr;
  }
  self->injected = true;
  InterlockedIncrement(&g_fakeWbemObject.refs);
  objects[got] = (IWbemClassObject*)&g_fakeWbemObject;
  *returned = got + 1;
  Dlog("WMI: injected virtual Win32_PNPEntity (VID_045E&PID_028E&IG_00)");
  return WBEM_S_NO_ERROR;
}
static HRESULT STDMETHODCALLTYPE FakeEnum_NextAsync(FakeWbemEnum*, ULONG, IWbemObjectSink*) {
  return WBEM_E_NOT_SUPPORTED;
}
static HRESULT STDMETHODCALLTYPE FakeEnum_Clone(FakeWbemEnum*, IEnumWbemClassObject**) {
  return WBEM_E_NOT_SUPPORTED;
}
static HRESULT STDMETHODCALLTYPE FakeEnum_Skip(FakeWbemEnum* self, LONG timeout, ULONG n) {
  if (self->real == nullptr)
    return WBEM_S_FALSE;
  return self->real->Skip(timeout, n);
}

/// IEnumWbemClassObject method table (8 slots).
static void* g_fakeWbemEnumVtable[8] = {
    (void*)&FakeEnum_QueryInterface, // 0
    (void*)&FakeEnum_AddRef,         // 1
    (void*)&FakeEnum_Release,        // 2
    (void*)&FakeEnum_Reset,          // 3
    (void*)&FakeEnum_Next,           // 4
    (void*)&FakeEnum_NextAsync,      // 5
    (void*)&FakeEnum_Clone,          // 6
    (void*)&FakeEnum_Skip,           // 7
};

/// IWbemServices::CreateInstanceEnum signature.
typedef HRESULT(STDMETHODCALLTYPE* FnWbemCreateEnum)(IWbemServices*, BSTR, LONG,
                                                     IWbemContext*, IEnumWbemClassObject**);
/// IWbemServices::CreateInstanceEnumAsync signature.
typedef HRESULT(STDMETHODCALLTYPE* FnWbemCreateEnumAsync)(IWbemServices*, BSTR, LONG,
                                                          IWbemContext*, IWbemObjectSink*);
/// IWbemServices::ExecQuery signature.
typedef HRESULT(STDMETHODCALLTYPE* FnWbemExecQuery)(IWbemServices*, BSTR, BSTR, LONG,
                                                    IWbemContext*, IEnumWbemClassObject**);
/// IWbemServices::ExecQueryAsync signature.
typedef HRESULT(STDMETHODCALLTYPE* FnWbemExecQueryAsync)(IWbemServices*, BSTR, BSTR, LONG,
                                                         IWbemContext*, IWbemObjectSink*);

/// One registered IWbemServices implementation whose slots are detoured.
struct WmiImpl {
  void* enumTarget;              ///< Original CreateInstanceEnum code (slot 18).
  void* enumAsyncTarget;         ///< Original CreateInstanceEnumAsync code (slot 19).
  void* queryTarget;             ///< Original ExecQuery code (slot 20).
  void* queryAsyncTarget;        ///< Original ExecQueryAsync code (slot 21).
  FnWbemCreateEnum enumOrig;     ///< Trampoline for slot 18.
  FnWbemCreateEnumAsync enumAsyncOrig; ///< Trampoline for slot 19.
  FnWbemExecQuery queryOrig;     ///< Trampoline for slot 20.
  FnWbemExecQueryAsync queryAsyncOrig; ///< Trampoline for slot 21.
  Detour detEnum;                ///< Detour for slot 18.
  Detour detEnumAsync;           ///< Detour for slot 19.
  Detour detQuery;               ///< Detour for slot 20.
  Detour detQueryAsync;          ///< Detour for slot 21.
};
static WmiImpl g_wmiImpls[8] = {};
static volatile LONG g_wmiImplCount = 0; ///< Registered implementations.

/// Locator vtable swap record (survives wbemprox reloads).
struct WbemLocatorSwap {
  void** copiedVtable; ///< Our private vtable copy.
  void* origVtable;    ///< The vtable it was copied from (for re-swaps).
  void* origConnect;   ///< The true ConnectServer code captured at swap time.
};
static WbemLocatorSwap g_locatorSwaps[8];
static volatile LONG g_locatorSwapCount = 0; ///< Live swap records.
static CRITICAL_SECTION g_wmiLock;           ///< Guards the swap registry.
static volatile LONG g_wmiLockInit = 0;      ///< 1 once g_wmiLock is valid.

/**
 * @brief Lazily initialise the WMI critical section.
 */
static void WmiLockInit(void) {
  if (InterlockedCompareExchange(&g_wmiLockInit, 1, 0) == 0)
    InitializeCriticalSection(&g_wmiLock);
}

/**
 * @brief Wrap an enumerator so the synthetic PNP entity shows up as an extra
 * result.
 * @param real Real enumerator to wrap (never released here).
 * @return The wrapper, or real when allocation failed.
 */
static IEnumWbemClassObject* WrapPnpEnumerator(IEnumWbemClassObject* real) {
  FakeWbemEnum* wrapper = new (std::nothrow) FakeWbemEnum{g_fakeWbemEnumVtable, real, 1, false};
  if (wrapper == nullptr) {
    Dlog("WMI: failed to allocate PNP enumerator wrapper");
    return real;
  }
  Dlog("WMI: Win32_PNPEntity enumerator wrapped for XInput detection");
  return (IEnumWbemClassObject*)wrapper;
}

/**
 * @brief Build an enumerator holding only the synthetic PNP entity.
 * @return The enumerator, or null on allocation failure.
 */
static IEnumWbemClassObject* MakeFakeOnlyEnumerator(void) {
  FakeWbemEnum* wrapper = new (std::nothrow) FakeWbemEnum{g_fakeWbemEnumVtable, nullptr, 1, false};
  if (wrapper == nullptr) {
    Dlog("WMI: failed to allocate synthetic-only PNP enumerator");
    return nullptr;
  }
  Dlog("WMI: real PNP query failed, returning synthetic-only enumerator");
  return (IEnumWbemClassObject*)wrapper;
}

/**
 * @brief Map a services object back to the registered implementation whose
 * slot code the caller's vtable points at.
 * @param self The services object; null-safe.
 * @param slot The vtable slot being serviced (18..21).
 * @return A registered WmiImpl, or null when none is registered.
 */
static WmiImpl* FindWmiImpl(IWbemServices* self, int slot) {
  if (InterlockedExchangeAdd(&g_wmiImplCount, 0) <= 0)
    return nullptr;
  void** vt = (self != nullptr) ? *reinterpret_cast<void***>(self) : nullptr;
  if (vt != nullptr) {
    void* target = nullptr;
    switch (slot) {
    case 18:
      target = vt[18];
      break;
    case 19:
      target = vt[19];
      break;
    case 20:
      target = vt[20];
      break;
    case 21:
      target = vt[21];
      break;
    default:
      return &g_wmiImpls[0];
    }
    const LONG count = InterlockedExchangeAdd(&g_wmiImplCount, 0);
    for (LONG i = 0; i < count; ++i) {
      void* known = nullptr;
      switch (slot) {
      case 18:
        known = g_wmiImpls[i].enumTarget;
        break;
      case 19:
        known = g_wmiImpls[i].enumAsyncTarget;
        break;
      case 20:
        known = g_wmiImpls[i].queryTarget;
        break;
      case 21:
        known = g_wmiImpls[i].queryAsyncTarget;
        break;
      }
      if (known != nullptr && known == target)
        return &g_wmiImpls[i];
    }
  }
  return &g_wmiImpls[0];
}

static HRESULT STDMETHODCALLTYPE WbemCreateInstanceEnum_Hook(IWbemServices* self,
                                                             BSTR className, LONG flags,
                                                             IWbemContext* context,
                                                             IEnumWbemClassObject** out) {
  WmiImpl* impl = FindWmiImpl(self, 18);
  if (impl == nullptr || impl->enumOrig == nullptr)
    return E_FAIL;
  WmiNoteCall();
  // The game's own "is this DI pad actually an XInput pad?" filter uses this
  // exact class. Seeing it while armed means the pad is being routed through
  // the XInput path - the auto-rescan retry loop's success signal.
  if (g_xinputFakeOn == 1 && FakeArmed() && className != nullptr &&
      _wcsicmp(className, kFakePnpClass) == 0) {
    InterlockedExchange64(&g_wmiPnpQueried, 1);
  }
  const HRESULT hr = impl->enumOrig(self, className, flags, context, out);
  if (g_xinputFakeOn != 1 || !FakeArmed() || className == nullptr ||
      _wcsicmp(className, kFakePnpClass) != 0) {
    return hr;
  }
  if (out == nullptr)
    return hr;
  if (SUCCEEDED(hr) && *out != nullptr) {
    *out = WrapPnpEnumerator(*out);
    return hr;
  }
  IEnumWbemClassObject* only = MakeFakeOnlyEnumerator();
  if (only == nullptr)
    return hr;
  *out = only;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE WbemExecQuery_Hook(IWbemServices* self, BSTR language,
                                                    BSTR query, LONG flags,
                                                    IWbemContext* context,
                                                    IEnumWbemClassObject** out) {
  WmiImpl* impl = FindWmiImpl(self, 20);
  if (impl == nullptr || impl->queryOrig == nullptr)
    return E_FAIL;
  WmiNoteCall();
  if (g_xinputFakeOn == 1 && FakeArmed() && query != nullptr &&
      wcsstr(query, kFakePnpClass) != nullptr) {
    InterlockedExchange64(&g_wmiPnpQueried, 1);
  }
  const HRESULT hr = impl->queryOrig(self, language, query, flags, context, out);
  if (g_xinputFakeOn != 1 || !FakeArmed() || query == nullptr ||
      wcsstr(query, kFakePnpClass) == nullptr) {
    return hr;
  }
  if (out == nullptr)
    return hr;
  if (SUCCEEDED(hr) && *out != nullptr) {
    *out = WrapPnpEnumerator(*out);
    return hr;
  }
  IEnumWbemClassObject* only = MakeFakeOnlyEnumerator();
  if (only == nullptr)
    return hr;
  *out = only;
  return S_OK;
}

// The async twins are counted, never wrapped: their results are delivered
// through IWbemObjectSink::Indicate, so injecting the synthetic entity would
// mean wrapping the sink too.
static HRESULT STDMETHODCALLTYPE WbemCreateInstanceEnumAsync_Hook(IWbemServices* self,
                                                                  BSTR className, LONG flags,
                                                                  IWbemContext* context,
                                                                  IWbemObjectSink* handler) {
  WmiImpl* impl = FindWmiImpl(self, 19);
  if (impl == nullptr || impl->enumAsyncOrig == nullptr)
    return E_FAIL;
  WmiNoteCall();
  return impl->enumAsyncOrig(self, className, flags, context, handler);
}
static HRESULT STDMETHODCALLTYPE WbemExecQueryAsync_Hook(IWbemServices* self, BSTR language,
                                                         BSTR query, LONG flags,
                                                         IWbemContext* context,
                                                         IWbemObjectSink* handler) {
  WmiImpl* impl = FindWmiImpl(self, 21);
  if (impl == nullptr || impl->queryAsyncOrig == nullptr)
    return E_FAIL;
  WmiNoteCall();
  return impl->queryAsyncOrig(self, language, query, flags, context, handler);
}

/**
 * @brief Detour the four enumeration slots behind a services vtable, once per
 * distinct code address.
 *
 * Both the install path (throwaway connection) and the ConnectServer observer
 * call this: if the game's WMI proxy resolves to different code than ours, the
 * install-time hooks alone would never see its query.
 * @param svcVtable The IWbemServices vtable pointer (may be null).
 * @param source Log tag naming the caller.
 */
static void RegisterWmiServices(void** svcVtable, const char* source) {
  if (svcVtable == nullptr) {
    WmiLog("WMI: RegisterWmiServices(%s): vtable is null", source);
    return;
  }
  void* targets[4] = {svcVtable[18], svcVtable[19], svcVtable[20], svcVtable[21]};
  const LONG count = InterlockedExchangeAdd(&g_wmiImplCount, 0);
  for (LONG i = 0; i < count; ++i) {
    if (g_wmiImpls[i].enumTarget == targets[0] &&
        g_wmiImpls[i].queryTarget == targets[2]) {
      WmiLog("WMI: RegisterWmiServices(%s): vtable=%p already hooked", source,
             (void*)svcVtable);
      return;
    }
  }
  if (InterlockedExchangeAdd(&g_wmiImplCount, 0) >= 8) {
    WmiLog("WMI: RegisterWmiServices(%s): registry full, vtable=%p NOT hooked", source,
           (void*)svcVtable);
    return;
  }
  WmiImpl impl = {};
  impl.enumTarget = targets[0];
  impl.enumAsyncTarget = targets[1];
  impl.queryTarget = targets[2];
  impl.queryAsyncTarget = targets[3];
  bool any = false;
  if (impl.enumTarget != nullptr) {
    void* tramp = impl.enumTarget;
    if (DetourInstall(tramp, (void*)&WbemCreateInstanceEnum_Hook, &tramp, &impl.detEnum)) {
      impl.enumOrig = (FnWbemCreateEnum)tramp;
      WmiLog("WMI: CreateInstanceEnum hook installed (%s) target=%p", source, impl.enumTarget);
      any = true;
    } else {
      WmiLog("WMI: CreateInstanceEnum hook failed (%s)", source);
    }
  }
  if (impl.queryTarget != nullptr) {
    void* tramp = impl.queryTarget;
    if (DetourInstall(tramp, (void*)&WbemExecQuery_Hook, &tramp, &impl.detQuery)) {
      impl.queryOrig = (FnWbemExecQuery)tramp;
      WmiLog("WMI: ExecQuery hook installed (%s) target=%p", source, impl.queryTarget);
      any = true;
    } else {
      WmiLog("WMI: ExecQuery hook failed (%s)", source);
    }
  }
  if (impl.enumAsyncTarget != nullptr) {
    void* tramp = impl.enumAsyncTarget;
    if (DetourInstall(tramp, (void*)&WbemCreateInstanceEnumAsync_Hook, &tramp, &impl.detEnumAsync)) {
      impl.enumAsyncOrig = (FnWbemCreateEnumAsync)tramp;
      WmiLog("WMI: CreateInstanceEnumAsync hook installed (%s) target=%p", source,
             impl.enumAsyncTarget);
      any = true;
    } else {
      WmiLog("WMI: CreateInstanceEnumAsync hook failed (%s)", source);
    }
  }
  if (impl.queryAsyncTarget != nullptr) {
    void* tramp = impl.queryAsyncTarget;
    if (DetourInstall(tramp, (void*)&WbemExecQueryAsync_Hook, &tramp, &impl.detQueryAsync)) {
      impl.queryAsyncOrig = (FnWbemExecQueryAsync)tramp;
      WmiLog("WMI: ExecQueryAsync hook installed (%s) target=%p", source, impl.queryAsyncTarget);
      any = true;
    } else {
      WmiLog("WMI: ExecQueryAsync hook failed (%s)", source);
    }
  }
  if (any) {
    g_wmiImpls[InterlockedExchangeAdd(&g_wmiImplCount, 0)] = impl;
    InterlockedIncrement(&g_wmiImplCount);
    WmiLog("WMI: registered services impl #%ld (%s) slot18=%p slot20=%p",
           InterlockedExchangeAdd(&g_wmiImplCount, 0), source, targets[0], targets[2]);
  }
}

/// IWbemLocator::ConnectServer signature (called through the copied vtable).
typedef HRESULT(STDMETHODCALLTYPE* FnWbemConnectServer)(void*, BSTR, BSTR, BSTR, BSTR, LONG,
                                                        BSTR, IWbemContext*, IWbemServices**);

/**
 * @brief Locator ConnectServer thunk: forward to the true implementation, then
 * detour every IWbemServices object that comes back.
 *
 * A code detour on the locator's ConnectServer would be erased when wbemprox.dll
 * reloads pristine; swapping the live locator's vtable slot survives that.
 */
static HRESULT STDMETHODCALLTYPE WbemConnectServer_Thunk(void* self, BSTR resource, BSTR user,
                                                         BSTR password, BSTR locale, LONG flags,
                                                         BSTR authority, IWbemContext* context,
                                                         IWbemServices** out) {
  void* orig = nullptr;
  {
    void** vt = (self != nullptr) ? *reinterpret_cast<void***>(self) : nullptr;
    WmiLockInit();
    EnterCriticalSection(&g_wmiLock);
    const LONG count = g_locatorSwapCount;
    for (LONG i = 0; i < count; ++i) {
      if (g_locatorSwaps[i].copiedVtable == vt) {
        orig = g_locatorSwaps[i].origConnect;
        break;
      }
    }
    LeaveCriticalSection(&g_wmiLock);
  }
  if (orig == nullptr)
    return E_FAIL;
  FnWbemConnectServer fn = (FnWbemConnectServer)orig;
  const HRESULT hr = fn(self, resource, user, password, locale, flags, authority, context, out);
  IWbemServices* svc = (out != nullptr) ? *out : nullptr;
  void** svcVt = (svc != nullptr) ? *reinterpret_cast<void***>(svc) : nullptr;
  WmiLog("WMI: ConnectServer ns=%ls hr=0x%08lX services=%p slot18=%p slot20=%p",
         resource != nullptr ? resource : L"(null)", (unsigned long)hr, (void*)svc,
         (svcVt != nullptr) ? svcVt[18] : nullptr, (svcVt != nullptr) ? svcVt[20] : nullptr);
  if (SUCCEEDED(hr))
    RegisterWmiServices(svcVt, "ConnectServer");
  return hr;
}

/**
 * @brief Swap a locator's vtable slot 3 for our ConnectServer thunk.
 * @param obj The locator object.
 * @param source Log tag naming the caller.
 */
static void SwapLocatorVtable(void* obj, const char* source) {
  if (obj == nullptr)
    return;
  void** vt = *reinterpret_cast<void***>(obj);
  if (vt == nullptr)
    return;
  WmiLockInit();
  EnterCriticalSection(&g_wmiLock);
  for (LONG i = 0; i < g_locatorSwapCount; ++i) {
    if (g_locatorSwaps[i].copiedVtable == vt) {
      LeaveCriticalSection(&g_wmiLock);
      return; // already ours
    }
    if (g_locatorSwaps[i].origVtable == (void*)vt) {
      // A locator whose vtable matches one we already copied: reuse the copy.
      *reinterpret_cast<void***>(obj) = g_locatorSwaps[i].copiedVtable;
      WmiLog("WMI: locator vtable re-swapped (%s)", source);
      LeaveCriticalSection(&g_wmiLock);
      return;
    }
  }
  if (g_locatorSwapCount >= 8) {
    WmiLog("WMI: locator swap registry full (%s)", source);
    LeaveCriticalSection(&g_wmiLock);
    return;
  }
  const int kSlots = 8;
  void** copy = (void**)malloc(sizeof(void*) * (size_t)kSlots);
  if (copy == nullptr) {
    LeaveCriticalSection(&g_wmiLock);
    return;
  }
  for (int i = 0; i < kSlots; ++i)
    copy[i] = vt[i];
  void* origConnect = vt[3];
  copy[3] = (void*)&WbemConnectServer_Thunk;
  g_locatorSwaps[g_locatorSwapCount].copiedVtable = copy;
  g_locatorSwaps[g_locatorSwapCount].origVtable = (void*)vt;
  g_locatorSwaps[g_locatorSwapCount].origConnect = origConnect;
  InterlockedIncrement(&g_locatorSwapCount);
  *reinterpret_cast<void***>(obj) = copy;
  WmiLog("WMI: locator vtable swapped (%s) obj=%p origConnect=%p", source, obj, origConnect);
  LeaveCriticalSection(&g_wmiLock);
}

typedef HRESULT(WINAPI* FnCoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static FnCoCreateInstance g_realCoCreateInstance = nullptr; ///< ole32 trampoline.
static Detour g_detourCoCreate;                            ///< ole32 CoCreateInstance detour.
static void* g_exportCoCreateInstance = nullptr;           ///< ole32 export address.

/**
 * @brief Hooked CoCreateInstance: redirect every WbemLocator that the game
 * creates onto the ConnectServer thunk (see SwapLocatorVtable).
 * @return The original call's result.
 */
static HRESULT WINAPI CoCreateInstance_Hook(REFCLSID clsid, LPUNKNOWN outer, DWORD clsContext,
                                            REFIID riid, LPVOID* out) {
  if (g_realCoCreateInstance == nullptr)
    return E_FAIL;
  if (IsEqualGUID(clsid, kClsidWbemLocator) == FALSE)
    return g_realCoCreateInstance(clsid, outer, clsContext, riid, out);
  const HRESULT hr = g_realCoCreateInstance(clsid, outer, clsContext, riid, out);
  if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
    SwapLocatorVtable(*out, "CoCreateInstance");
  return hr;
}

/// Pin WMI in-proc components so their detoured code is not reloaded pristine.
static void WmiPinModules(void) {
  const wchar_t* names[] = {L"wbemprox.dll", L"fastprox.dll"};
  for (int i = 0; i < 2; ++i) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, names[i], &mod))
      WmiLog("WMI: pinned %ls at %p", names[i], (void*)mod);
    else
      WmiLog("WMI: pin %ls failed err=%lu", names[i], GetLastError());
  }
}

/**
 * @brief Install the WMI Win32_PNPEntity fake.
 *
 * Only runs with XInputFakePad=1. Builds a live IWbemServices to borrow the
 * CreateInstanceEnum/ExecQuery code addresses from, hooks them, then attaches
 * the CoCreateInstance observer LAST so our own session runs through the
 * unhooked originals.
 */
static void WmiPnpInstall(void) {
  if (g_xinputFakeOn != 1) {
    Dlog("WMI: PNP fake skipped (XInputFakePad=0)");
    return;
  }
  WmiLockInit();
  InterlockedExchange(&g_wmiCallLog, 0);
  WmiLog("WMI: install begin");

  const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool coInit = (coHr == S_OK);
  WmiLog("WMI: CoInitializeEx hr=0x%08lX coInit=%d", (unsigned long)coHr, coInit ? 1 : 0);

  IWbemLocator* locator = nullptr;
  IWbemServices* services = nullptr;

  HRESULT hr = CoCreateInstance(kClsidWbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                kIidWbemLocator, (void**)&locator);
  WmiLog("WMI: CoCreateInstance(WbemLocator) hr=0x%08lX locator=%p", (unsigned long)hr,
         (void*)locator);
  if (SUCCEEDED(hr) && locator != nullptr) {
    BSTR ns = SysAllocString(L"ROOT\\CIMV2");
    hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    WmiLog("WMI: ConnectServer(ROOT\\CIMV2) hr=0x%08lX services=%p", (unsigned long)hr,
           (void*)services);
    SysFreeString(ns);
  }

  if (SUCCEEDED(hr) && services != nullptr) {
    void** svcVtable = *reinterpret_cast<void***>(services);
    // Diagnostic only: whether the throwaway connection may enumerate the
    // class tells us whether the game's filter will succeed or need the
    // synthetic-only fallback.
    {
      IEnumWbemClassObject* probe = nullptr;
      BSTR cls = SysAllocString(kFakePnpClass);
      const HRESULT pHr = services->CreateInstanceEnum(cls, 0, nullptr, &probe);
      WmiLog("WMI: probe CreateInstanceEnum hr=0x%08lX enum=%p", (unsigned long)pHr,
             (void*)probe);
      SysFreeString(cls);
      if (probe != nullptr)
        probe->Release();
    }
    if (svcVtable != nullptr) {
      // After the probe on purpose: the probe must run through the unhooked
      // originals so it never counts toward wmiCalls.
      RegisterWmiServices(svcVtable, "install");
    } else {
      WmiLog("WMI: IWbemServices vtable null - PNP fake skipped");
    }
    services->Release();
  } else {
    WmiLog("WMI: WbemLocator/ConnectServer unavailable, PNP fake skipped");
  }

  // Watch every future WMI session. Attached last: our own ConnectServer and
  // the probe above have to run through the unhooked originals.
  if (locator != nullptr) {
    SwapLocatorVtable(locator, "install");
    locator->Release();
  }
  if (g_realCoCreateInstance == nullptr && g_exportCoCreateInstance == nullptr) {
    HMODULE ole32 = GetModuleHandleW(L"ole32.dll");
    if (ole32 != nullptr) {
      g_exportCoCreateInstance = (void*)GetProcAddress(ole32, "CoCreateInstance");
      if (g_exportCoCreateInstance != nullptr) {
        void* real = g_exportCoCreateInstance;
        if (DetourInstall(g_exportCoCreateInstance, (void*)&CoCreateInstance_Hook, &real,
                          &g_detourCoCreate)) {
          g_realCoCreateInstance = (FnCoCreateInstance)real;
          WmiLog("WMI: CoCreateInstance observer installed");
        } else {
          g_exportCoCreateInstance = nullptr;
          WmiLog("WMI: CoCreateInstance observer failed");
        }
      }
    }
  }
  WmiPinModules();
  if (coInit)
    CoUninitialize();
  WmiLog("WMI: install end");
}

/**
 * @brief Whether at least one WMI services implementation is hooked.
 * @return True when the PNP fake is live.
 */
static bool WmiFakeInstalled(void) {
  return InterlockedExchangeAdd(&g_wmiImplCount, 0) > 0;
}

/**
 * @brief Number of WMI hook hits seen (the "game queried" signal).
 * @return The counter.
 */
static int WmiCallCount(void) {
  return (int)InterlockedExchangeAdd(&g_wmiCallLog, 0);
}

/**
 * @brief Undo the WMI detours (the vtable swaps stay process-live; they are
 * harmless and cannot be restored once the game holds the objects).
 */
static void WmiPnpRestore(void) {
  const LONG count = InterlockedExchangeAdd(&g_wmiImplCount, 0);
  for (LONG i = 0; i < count; ++i) {
    DetourRestore(&g_wmiImpls[i].detEnum, (void**)&g_wmiImpls[i].enumOrig, nullptr);
    DetourRestore(&g_wmiImpls[i].detEnumAsync, (void**)&g_wmiImpls[i].enumAsyncOrig, nullptr);
    DetourRestore(&g_wmiImpls[i].detQuery, (void**)&g_wmiImpls[i].queryOrig, nullptr);
    DetourRestore(&g_wmiImpls[i].detQueryAsync, (void**)&g_wmiImpls[i].queryAsyncOrig, nullptr);
  }
  InterlockedExchange(&g_wmiImplCount, 0);
  if (g_exportCoCreateInstance != nullptr) {
    DetourRestore(&g_detourCoCreate, (void**)&g_realCoCreateInstance, g_exportCoCreateInstance);
    g_exportCoCreateInstance = nullptr;
  }
  Dlog("wmi: restore done");
}

/* ------------------------------------------------------------------ */
/* SetupDi/HidD virtual HID device (the Xbox VID gate)                 */
/* ------------------------------------------------------------------ */
// DMC5 (RE Engine) does not probe XInputGetCapabilities to detect a gamepad;
// it enumerates HID interfaces through SetupDi, opens each one and reads
// VID/PID with HidD_GetAttributes. Only an Xbox VID (0x045E) makes it switch
// to the XInput code path. Without a physical pad SetupDi returns nothing, so
// the game falls back to DirectInput and never touches our XInput hooks.
//
// We inject a single fake HID device that reports Xbox 360 identifiers. The
// game opens it, sees VID 0x045E, and starts calling XInputGetState - which
// our XInput hooks serve from the streamed virtual pad state.

/// GUID_DEVINTERFACE_HID (hidclass.h) - the interface class the fake device
/// and the injected interface data both advertise.
static const GUID kGuidHidInterface = {0x4d1e55b2, 0xf16f, 0x11cf,
                                       {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

/// Xbox 360 HID device path (VID_045E&PID_028E, GUID_DEVINTERFACE_HID suffix).
static const wchar_t kFakeHidDevicePath[] =
    L"\\\\?\\HID#VID_045E&PID_028E&IG_00#7&1&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
/// Sentinel handle our hooks recognise as the virtual HID device ("HIDX").
static const HANDLE kFakeHidHandle = (HANDLE)0x48494458u;
/**
 * Pointer handed back as the virtual device's preparsed data.
 *
 * The real HidP_* parsers dereference whatever pointer HidD_GetPreparsedData
 * returns, so this MUST be a mapped address. A bogus unmapped constant (as was
 * used here before) faults the moment a parser this build never hooks - or
 * could not hook, because its prologue could not be relocated on that OS build
 * - receives it, which killed the game right after injection. A zeroed,
 * process-lifetime block instead makes those parsers fail validation and
 * return HIDP_STATUS_INVALID_PREPARSED_DATA.
 */
static PHIDP_PREPARSED_DATA g_fakePreparsedData = nullptr;

/**
 * @brief Allocate the mapped stand-in used as the virtual device's preparsed
 * data (idempotent, called once from HidSetupDiHookInstall before any hook is
 * live).
 */
static void FakePreparsedDataInit(void) {
  if (g_fakePreparsedData != nullptr)
    return;
  void* buf = calloc(1, 0x10000); // 64 KiB zeroed: validation fails, reads stay mapped
  if (buf != nullptr)
    g_fakePreparsedData = (PHIDP_PREPARSED_DATA)buf;
}

/// Shared per-process budget for the HID fake answer logs.
static volatile LONG g_hidFakeLogBudget = 24;

/**
 * @brief Log a HID fake answer, capped so a hot-plug storm cannot flood.
 * @param msg Message to log.
 */
static void HidLogFake(const char* msg) {
  if (InterlockedExchangeAdd(&g_hidFakeLogBudget, -1) > 0)
    Dlog("%s", msg);
}

typedef BOOLEAN(WINAPI* FnHidDAttributes)(HANDLE, PHIDD_ATTRIBUTES);
typedef BOOLEAN(WINAPI* FnHidDPrep)(HANDLE, PHIDP_PREPARSED_DATA*);
typedef BOOLEAN(WINAPI* FnHidDFreePrep)(PHIDP_PREPARSED_DATA);
typedef NTSTATUS(WINAPI* FnHidPCaps)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);
typedef NTSTATUS(WINAPI* FnHidPValueCaps)(HIDP_REPORT_TYPE, PHIDP_VALUE_CAPS, PUSHORT,
                                          PHIDP_PREPARSED_DATA);
typedef NTSTATUS(WINAPI* FnHidPButtonCaps)(HIDP_REPORT_TYPE, PHIDP_BUTTON_CAPS, PUSHORT,
                                           PHIDP_PREPARSED_DATA);
typedef BOOLEAN(WINAPI* FnHidDNumBuf)(HANDLE, PULONG);
typedef BOOLEAN(WINAPI* FnHidDSetNumBuf)(HANDLE, ULONG);
typedef BOOLEAN(WINAPI* FnHidDGetFeature)(HANDLE, PVOID, ULONG);
typedef BOOLEAN(WINAPI* FnHidDSetFeature)(HANDLE, PVOID, ULONG);
typedef BOOLEAN(WINAPI* FnHidDGetInputReport)(HANDLE, PVOID, ULONG);
typedef BOOLEAN(WINAPI* FnHidDGetOutputReport)(HANDLE, PVOID, ULONG);
typedef BOOLEAN(WINAPI* FnHidDGetProductString)(HANDLE, PVOID, ULONG);
typedef BOOLEAN(WINAPI* FnHidDGetSerial)(HANDLE, PVOID, ULONG);
typedef BOOLEAN(WINAPI* FnHidDGetIndexed)(HANDLE, ULONG, PVOID, ULONG);

typedef BOOL(WINAPI* FnSetupDiEnumDeviceInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID*,
                                                    DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL(WINAPI* FnSetupDiGetDeviceInterfaceDetailW)(
    HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD,
    PSP_DEVINFO_DATA);
typedef BOOL(WINAPI* FnSetupDiGetDeviceInstanceIdW)(HDEVINFO, PSP_DEVINFO_DATA, PWSTR, DWORD,
                                                    PDWORD);
typedef BOOL(WINAPI* FnSetupDiGetDeviceRegistryPropertyW)(HDEVINFO, PSP_DEVINFO_DATA, DWORD,
                                                          PDWORD, PBYTE, DWORD, PDWORD);
typedef BOOL(WINAPI* FnSetupDiGetDeviceRegistryPropertyA)(HDEVINFO, PSP_DEVINFO_DATA, DWORD,
                                                          PDWORD, PBYTE, DWORD, PDWORD);
typedef HDEVINFO(WINAPI* FnSetupDiGetClassDevsW)(const GUID*, PCWSTR, HWND, DWORD);
typedef HDEVINFO(WINAPI* FnSetupDiGetClassDevsA)(const GUID*, PCSTR, HWND, DWORD);
typedef BOOL(WINAPI* FnSetupDiDestroyDeviceInfoList)(HDEVINFO);

typedef HANDLE(WINAPI* FnCreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                      HANDLE);
typedef BOOL(WINAPI* FnCloseHandle)(HANDLE);
typedef BOOL(WINAPI* FnReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI* FnWriteFile)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI* FnGetOverlappedResult)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
typedef BOOL(WINAPI* FnDeviceIoControl)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD,
                                        LPOVERLAPPED);

static FnSetupDiEnumDeviceInterfaces g_realSetupDiEnumDeviceInterfaces = nullptr;
static FnSetupDiGetDeviceInterfaceDetailW g_realSetupDiGetDeviceInterfaceDetailW = nullptr;
static FnSetupDiGetDeviceInstanceIdW g_realSetupDiGetDeviceInstanceIdW = nullptr;
static FnSetupDiGetDeviceRegistryPropertyW g_realSetupDiGetDeviceRegistryPropertyW = nullptr;
static FnSetupDiGetDeviceRegistryPropertyA g_realSetupDiGetDeviceRegistryPropertyA = nullptr;
static FnSetupDiGetClassDevsW g_realSetupDiGetClassDevsW = nullptr;
static FnSetupDiGetClassDevsA g_realSetupDiGetClassDevsA = nullptr;
static FnSetupDiDestroyDeviceInfoList g_realSetupDiDestroyDeviceInfoList = nullptr;
static FnCreateFileW g_realCreateFileW = nullptr;
static FnCloseHandle g_realCloseHandle = nullptr;
static FnReadFile g_realReadFile = nullptr;
static FnWriteFile g_realWriteFile = nullptr;
static FnGetOverlappedResult g_realGetOverlappedResult = nullptr;
static FnDeviceIoControl g_realDeviceIoControl = nullptr;
static FnHidDAttributes g_realHidDGetAttributes = nullptr;
static FnHidDPrep g_realHidDGetPreparsedData = nullptr;
static FnHidDFreePrep g_realHidDFreePreparsedData = nullptr;
static FnHidPCaps g_realHidPGetCaps = nullptr;
static FnHidPValueCaps g_realHidPGetValueCaps = nullptr;
static FnHidPButtonCaps g_realHidPGetButtonCaps = nullptr;
static FnHidDNumBuf g_realHidDGetNumInputBuffers = nullptr;
static FnHidDSetNumBuf g_realHidDSetNumInputBuffers = nullptr;
static FnHidDGetFeature g_realHidDGetFeature = nullptr;
static FnHidDSetFeature g_realHidDSetFeature = nullptr;
static FnHidDGetInputReport g_realHidDGetInputReport = nullptr;
static FnHidDGetOutputReport g_realHidDGetOutputReport = nullptr;
static FnHidDGetProductString g_realHidDGetProductString = nullptr;
static FnHidDGetSerial g_realHidDGetSerialNumberString = nullptr;
static FnHidDGetIndexed g_realHidDGetIndexedString = nullptr;

/// SetupDi detour records.
static Detour g_hidDetEnumInterfaces, g_hidDetDetailW, g_hidDetInstanceId, g_hidDetRegPropW,
    g_hidDetRegPropA, g_hidDetClassDevsW, g_hidDetClassDevsA, g_hidDetDestroyList;
/// HidD/HidP detour records.
static Detour g_hidDetAttrib, g_hidDetPrep, g_hidDetFreePrep, g_hidDetCaps, g_hidDetValCaps,
    g_hidDetBtnCaps, g_hidDetNumBuf, g_hidDetSetNumBuf, g_hidDetFeature, g_hidDetSetFeature,
    g_hidDetInReport, g_hidDetOutReport, g_hidDetProduct, g_hidDetSerial, g_hidDetIndexed;
/// kernel32 detour records for the virtual handle answers.
static Detour g_hidDetCreateFileW, g_hidDetCloseHandle, g_hidDetReadFile, g_hidDetWriteFile,
    g_hidDetGetOverlapped, g_hidDetIoControl;
/// Original export addresses (for restore).
static void* g_exportSetupDiEnumDeviceInterfaces = nullptr;
static void* g_exportSetupDiGetDeviceInterfaceDetailW = nullptr;
static void* g_exportSetupDiGetDeviceInstanceIdW = nullptr;
static void* g_exportSetupDiGetDeviceRegistryPropertyW = nullptr;
static void* g_exportSetupDiGetDeviceRegistryPropertyA = nullptr;
static void* g_exportSetupDiGetClassDevsW = nullptr;
static void* g_exportSetupDiGetClassDevsA = nullptr;
static void* g_exportSetupDiDestroyDeviceInfoList = nullptr;
static void* g_exportCreateFileW = nullptr;
static void* g_exportCloseHandle = nullptr;
static void* g_exportReadFile = nullptr;
static void* g_exportWriteFile = nullptr;
static void* g_exportGetOverlappedResult = nullptr;
static void* g_exportDeviceIoControl = nullptr;
static void* g_exportHidDGetAttributes = nullptr;
static void* g_exportHidDGetPreparsedData = nullptr;
static void* g_exportHidDFreePreparsedData = nullptr;
static void* g_exportHidPGetCaps = nullptr;
static void* g_exportHidPGetValueCaps = nullptr;
static void* g_exportHidPGetButtonCaps = nullptr;
static void* g_exportHidDGetNumInputBuffers = nullptr;
static void* g_exportHidDSetNumInputBuffers = nullptr;
static void* g_exportHidDGetFeature = nullptr;
static void* g_exportHidDSetFeature = nullptr;
static void* g_exportHidDGetInputReport = nullptr;
static void* g_exportHidDGetOutputReport = nullptr;
static void* g_exportHidDGetProductString = nullptr;
static void* g_exportHidDGetSerialNumberString = nullptr;
static void* g_exportHidDGetIndexedString = nullptr;

/// Fix a setupapi export address and detour it.
static void HidAttachExport(HMODULE mod, const char* name, void* hook, void** real,
                            void** exportOut, Detour* det) {
  void* entry = (void*)GetProcAddress(mod, name);
  if (entry == nullptr) {
    Dlog("hid: %s skipped (not exported)", name);
    return;
  }
  *exportOut = entry;
  if (DetourInstall(entry, hook, real, det))
    Dlog("hid: %s hooked", name);
  else
    Dlog("hid: %s hook failed", name);
}

/// HDEVINFO sets that already received the injected fake interface.
static HDEVINFO g_fakeDevInfoSets[32];
static volatile LONG g_fakeDevInfoSetCount = 0; ///< Tracked sets.
static CRITICAL_SECTION g_fakeDevInfoLock;      ///< Guards the set tracking.
static volatile LONG g_fakeDevInfoLockInit = 0; ///< 1 once the lock is valid.

/**
 * @brief Lazily initialise the HID dev-info tracking lock.
 */
static void HidDevInfoLockInit(void) {
  if (InterlockedCompareExchange(&g_fakeDevInfoLockInit, 1, 0) == 0)
    InitializeCriticalSection(&g_fakeDevInfoLock);
}

/**
 * @brief Whether this HDEVINFO already received the fake interface.
 * @param devInfo The set handle.
 * @return True when already injected.
 */
static bool HidSetTracked(HDEVINFO devInfo) {
  const LONG count = InterlockedExchangeAdd(&g_fakeDevInfoSetCount, 0);
  for (LONG i = 0; i < count; ++i) {
    if (g_fakeDevInfoSets[i] == devInfo)
      return true;
  }
  return false;
}

/**
 * @brief Track a set as injected.
 * @param devInfo The set handle.
 */
static void HidSetTrack(HDEVINFO devInfo) {
  if (InterlockedExchangeAdd(&g_fakeDevInfoSetCount, 0) >= 32)
    return;
  g_fakeDevInfoSets[InterlockedExchangeAdd(&g_fakeDevInfoSetCount, 0)] = devInfo;
  InterlockedIncrement(&g_fakeDevInfoSetCount);
}

/**
 * @brief Untrack a destroyed set.
 * @param devInfo The set handle.
 */
static void HidSetUntrack(HDEVINFO devInfo) {
  const LONG count = InterlockedExchangeAdd(&g_fakeDevInfoSetCount, 0);
  for (LONG i = 0; i < count; ++i) {
    if (g_fakeDevInfoSets[i] == devInfo) {
      g_fakeDevInfoSets[i] = g_fakeDevInfoSets[count - 1];
      InterlockedDecrement(&g_fakeDevInfoSetCount);
      return;
    }
  }
}

/**
 * @brief Whether the interface data is our injected fake (cbSize marker).
 * @param data The interface data.
 * @return True for the fake interface.
 */
static bool IsFakeInterfaceData(const SP_DEVICE_INTERFACE_DATA* data) {
  if (data == nullptr)
    return false;
  return IsEqualGUID(data->InterfaceClassGuid, kGuidHidInterface) != FALSE &&
         data->cbSize == sizeof(SP_DEVICE_INTERFACE_DATA) + 1;
}

/**
 * @brief Whether the device info data refers to our fake device node.
 * @param devInfoData The device info data.
 * @return True for the fake node.
 */
static bool IsFakeDevInfo(const PSP_DEVINFO_DATA devInfoData) {
  return devInfoData != nullptr && devInfoData->DevInst == 0 &&
         IsEqualGUID(devInfoData->ClassGuid, kGuidHidInterface) != FALSE;
}

static BOOL WINAPI SetupDiEnumDeviceInterfaces_Hook(HDEVINFO devInfo, PSP_DEVINFO_DATA devInfoData,
                                                    const GUID* interfaceClassGuid,
                                                    DWORD memberIndex,
                                                    PSP_DEVICE_INTERFACE_DATA deviceInterfaceData) {
  const BOOL ret = g_realSetupDiEnumDeviceInterfaces(devInfo, devInfoData, interfaceClassGuid,
                                                     memberIndex, deviceInterfaceData);
  if (g_xinputFakeOn != 1 || !FakeArmed())
    return ret;
  // Only inject into HID enumerations that have run out of real devices, once
  // per set.
  if (ret || interfaceClassGuid == nullptr || deviceInterfaceData == nullptr)
    return ret;
  if (IsEqualGUID(*interfaceClassGuid, kGuidHidInterface) == FALSE)
    return ret;
  HidDevInfoLockInit();
  EnterCriticalSection(&g_fakeDevInfoLock);
  const bool tracked = HidSetTracked(devInfo);
  if (!tracked)
    HidSetTrack(devInfo);
  LeaveCriticalSection(&g_fakeDevInfoLock);
  if (tracked)
    return ret;
  deviceInterfaceData->cbSize = sizeof(SP_DEVICE_INTERFACE_DATA) + 1; // marker for us
  deviceInterfaceData->InterfaceClassGuid = kGuidHidInterface;
  deviceInterfaceData->Flags = SPINT_ACTIVE;
  SetLastError(ERROR_SUCCESS);
  HidLogFake("hid: injected virtual Xbox 360 HID device interface");
  return TRUE;
}

static BOOL WINAPI SetupDiGetDeviceInterfaceDetailW_Hook(
    HDEVINFO devInfo, PSP_DEVICE_INTERFACE_DATA deviceInterfaceData,
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W deviceInterfaceDetailData,
    DWORD deviceInterfaceDetailDataSize, PDWORD requiredSize, PSP_DEVINFO_DATA deviceInfoData) {
  if (IsFakeInterfaceData(deviceInterfaceData)) {
    const DWORD pathBytes = (DWORD)((wcslen(kFakeHidDevicePath) + 1) * sizeof(wchar_t));
    const DWORD total = (DWORD)offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath) + pathBytes;
    if (requiredSize != nullptr)
      *requiredSize = total;
    if (deviceInterfaceDetailData == nullptr) {
      SetLastError(ERROR_INSUFFICIENT_BUFFER);
      return FALSE;
    }
    if (deviceInterfaceDetailDataSize < total) {
      SetLastError(ERROR_INSUFFICIENT_BUFFER);
      return FALSE;
    }
    deviceInterfaceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    memcpy(deviceInterfaceDetailData->DevicePath, kFakeHidDevicePath, pathBytes);
    if (deviceInfoData != nullptr) {
      deviceInfoData->cbSize = sizeof(SP_DEVINFO_DATA);
      deviceInfoData->ClassGuid = kGuidHidInterface;
      deviceInfoData->DevInst = 0;
    }
    SetLastError(ERROR_SUCCESS);
    HidLogFake("hid: SetupDiGetDeviceInterfaceDetailW fake path served");
    return TRUE;
  }
  return g_realSetupDiGetDeviceInterfaceDetailW(devInfo, deviceInterfaceData,
                                                deviceInterfaceDetailData,
                                                deviceInterfaceDetailDataSize, requiredSize,
                                                deviceInfoData);
}

static BOOL WINAPI SetupDiGetDeviceInstanceIdW_Hook(HDEVINFO devInfo, PSP_DEVINFO_DATA devInfoData,
                                                    PWSTR instanceId, DWORD instanceIdSize,
                                                    PDWORD requiredSize) {
  if (g_xinputFakeOn == 1 && FakeArmed() && IsFakeDevInfo(devInfoData)) {
    // Real Xbox 360 pads carry the "IG_" interface marker in their instance
    // ID; without it the game's XInput detector never recognises the pad.
    static const wchar_t kFakeInstanceId[] = L"HID\\VID_045E&PID_028E&IG_00\\7&1&0&0000";
    const DWORD len = (DWORD)(wcslen(kFakeInstanceId) + 1);
    if (requiredSize != nullptr)
      *requiredSize = len;
    if (instanceId == nullptr || instanceIdSize < len) {
      SetLastError(ERROR_INSUFFICIENT_BUFFER);
      return FALSE;
    }
    memcpy(instanceId, kFakeInstanceId, len * sizeof(wchar_t));
    SetLastError(ERROR_SUCCESS);
    HidLogFake("hid: SetupDiGetDeviceInstanceIdW fake -> HID\\VID_045E&PID_028E&IG_00");
    return TRUE;
  }
  return g_realSetupDiGetDeviceInstanceIdW(devInfo, devInfoData, instanceId, instanceIdSize,
                                           requiredSize);
}

/// Builds a registry-property payload for the fake device node.
struct FakeRegPropPayload {
  BYTE buf[512]; ///< Payload bytes.
  size_t len;    ///< Payload length.
};

/// Append a string in the flavour the caller wants.
static void RegPropAppendString(FakeRegPropPayload* p, const wchar_t* text, bool wide) {
  if (wide) {
    const size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    if (p->len + bytes <= sizeof(p->buf)) {
      memcpy(p->buf + p->len, text, bytes);
      p->len += bytes;
    }
  } else {
    char buf[256];
    const int len = WideCharToMultiByte(CP_ACP, 0, text, -1, buf, (int)sizeof(buf), nullptr,
                                        nullptr);
    if (len > 0 && p->len + (size_t)len <= sizeof(p->buf)) {
      memcpy(p->buf + p->len, buf, (size_t)len);
      p->len += (size_t)len;
    }
  }
}

/**
 * @brief Fill a fake device-node registry payload for a property.
 * @param property SPDRP_* property.
 * @param dataType Receives the registry type.
 * @param p Receives the payload.
 * @param wide True for the W-flavour property reader.
 * @return True when the property is served.
 */
static bool BuildFakeRegProp(DWORD property, DWORD* dataType, FakeRegPropPayload* p, bool wide) {
  switch (property) {
  case SPDRP_DEVICEDESC:
    *dataType = REG_SZ;
    RegPropAppendString(p, L"Xbox 360 Controller", wide);
    return true;
  case SPDRP_HARDWAREID: {
    *dataType = REG_MULTI_SZ;
    RegPropAppendString(p, L"HID\\VID_045E&PID_028E&IG_00", wide);
    RegPropAppendString(p, L"HID\\VID_045E&PID_028E", wide);
    if (wide) {
      if (p->len + 2 <= sizeof(p->buf)) {
        p->buf[p->len++] = 0;
        p->buf[p->len++] = 0;
      }
    } else if (p->len + 1 <= sizeof(p->buf)) {
      p->buf[p->len++] = 0;
    }
    return true;
  }
  case SPDRP_MFG:
    *dataType = REG_SZ;
    RegPropAppendString(p, L"Microsoft", wide);
    return true;
  case SPDRP_FRIENDLYNAME:
    *dataType = REG_SZ;
    RegPropAppendString(p, L"Xbox 360 Controller for Windows", wide);
    return true;
  case SPDRP_CLASS:
    *dataType = REG_SZ;
    RegPropAppendString(p, L"HIDClass", wide);
    return true;
  case SPDRP_CLASSGUID:
    *dataType = REG_SZ;
    RegPropAppendString(p, L"{745a17a0-74d3-11d0-b6fe-00a0c90f57da}", wide);
    return true;
  case SPDRP_DRIVER:
    *dataType = REG_SZ;
    RegPropAppendString(p, L"{4d36e96b-e325-11ce-bfc1-08002be10318}\\0008", wide);
    return true;
  case SPDRP_SERVICE:
    *dataType = REG_SZ;
    RegPropAppendString(p, L"HidUsb", wide);
    return true;
  case SPDRP_ENUMERATOR_NAME:
    *dataType = REG_SZ;
    RegPropAppendString(p, L"HID", wide);
    return true;
  case SPDRP_INSTALL_STATE:
    *dataType = REG_DWORD;
    p->len = 4;
    return true;
  default:
    return false;
  }
}

/**
 * @brief Serve one fake registry-property read (W or A flavour).
 * @param property SPDRP_* property.
 * @param regType Receives the registry type.
 * @param buffer Output buffer.
 * @param bufferSize Capacity.
 * @param requiredSize Receives the needed size.
 * @param wide True for the W flavour.
 * @return TRUE on success, FALSE with SetLastError on failure.
 */
static BOOL ServeFakeRegProp(DWORD property, DWORD* regType, PBYTE buffer, DWORD bufferSize,
                             DWORD* requiredSize, bool wide) {
  DWORD type = 0;
  FakeRegPropPayload payload = {};
  if (!BuildFakeRegProp(property, &type, &payload, wide)) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (requiredSize != nullptr)
    *requiredSize = (DWORD)payload.len;
  if (regType != nullptr)
    *regType = type;
  if (buffer == nullptr || bufferSize < payload.len) {
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return FALSE;
  }
  memcpy(buffer, payload.buf, payload.len);
  SetLastError(ERROR_SUCCESS);
  return TRUE;
}

static BOOL WINAPI SetupDiGetDeviceRegistryPropertyW_Hook(
    HDEVINFO devInfo, PSP_DEVINFO_DATA devInfoData, DWORD property, DWORD* regType, PBYTE buffer,
    DWORD bufferSize, DWORD* requiredSize) {
  if (g_xinputFakeOn == 1 && FakeArmed() && IsFakeDevInfo(devInfoData)) {
    HidLogFake("hid: SetupDiGetDeviceRegistryPropertyW fake");
    return ServeFakeRegProp(property, regType, buffer, bufferSize, requiredSize, true);
  }
  return g_realSetupDiGetDeviceRegistryPropertyW(devInfo, devInfoData, property, regType, buffer,
                                                 bufferSize, requiredSize);
}

static BOOL WINAPI SetupDiGetDeviceRegistryPropertyA_Hook(
    HDEVINFO devInfo, PSP_DEVINFO_DATA devInfoData, DWORD property, DWORD* regType, PBYTE buffer,
    DWORD bufferSize, DWORD* requiredSize) {
  if (g_xinputFakeOn == 1 && FakeArmed() && IsFakeDevInfo(devInfoData)) {
    HidLogFake("hid: SetupDiGetDeviceRegistryPropertyA fake");
    return ServeFakeRegProp(property, regType, buffer, bufferSize, requiredSize, false);
  }
  return g_realSetupDiGetDeviceRegistryPropertyA(devInfo, devInfoData, property, regType, buffer,
                                                 bufferSize, requiredSize);
}

static HDEVINFO WINAPI SetupDiGetClassDevsW_Hook(const GUID* classGuid, PCWSTR enumerator,
                                                 HWND hwnd, DWORD flags) {
  return g_realSetupDiGetClassDevsW(classGuid, enumerator, hwnd, flags);
}

static HDEVINFO WINAPI SetupDiGetClassDevsA_Hook(const GUID* classGuid, PCSTR enumerator,
                                                 HWND hwnd, DWORD flags) {
  return g_realSetupDiGetClassDevsA(classGuid, enumerator, hwnd, flags);
}

static BOOL WINAPI SetupDiDestroyDeviceInfoList_Hook(HDEVINFO devInfo) {
  HidDevInfoLockInit();
  EnterCriticalSection(&g_fakeDevInfoLock);
  HidSetUntrack(devInfo);
  LeaveCriticalSection(&g_fakeDevInfoLock);
  return g_realSetupDiDestroyDeviceInfoList(devInfo);
}

static BOOLEAN WINAPI HidD_GetAttributes_Hook(HANDLE hidDeviceObject, PHIDD_ATTRIBUTES attributes) {
  if (hidDeviceObject == kFakeHidHandle && attributes != nullptr) {
    attributes->Size = sizeof(HIDD_ATTRIBUTES);
    attributes->VendorID = 0x045E;   // Microsoft
    attributes->ProductID = 0x028E;  // Xbox 360 Controller
    attributes->VersionNumber = 0x0114;
    HidLogFake("hid: HidD_GetAttributes fake -> VID=045E PID=028E");
    return TRUE;
  }
  return g_realHidDGetAttributes(hidDeviceObject, attributes);
}

static BOOLEAN WINAPI HidD_GetPreparsedData_Hook(HANDLE hidDeviceObject,
                                                 PHIDP_PREPARSED_DATA* preparsedData) {
  if (hidDeviceObject == kFakeHidHandle && preparsedData != nullptr &&
      g_fakePreparsedData != nullptr) {
    *preparsedData = g_fakePreparsedData;
    HidLogFake("hid: HidD_GetPreparsedData fake -> mapped stand-in");
    return TRUE;
  }
  return g_realHidDGetPreparsedData(hidDeviceObject, preparsedData);
}

static BOOLEAN WINAPI HidD_FreePreparsedData_Hook(PHIDP_PREPARSED_DATA preparsedData) {
  if (preparsedData != nullptr && preparsedData == g_fakePreparsedData)
    return TRUE;
  return g_realHidDFreePreparsedData(preparsedData);
}

// A genuine wired "Xbox 360 Controller" parses to 10 buttons + 6 values
// (X, Y, Rx, Ry = 16-bit sticks; Z, Rz = 8-bit triggers), 20-byte input report,
// 8-byte LED output report. If DMC5 checks any cap counts this must look real.
static NTSTATUS WINAPI HidP_GetCaps_Hook(PHIDP_PREPARSED_DATA preparsedData, PHIDP_CAPS caps) {
  if (preparsedData != nullptr && preparsedData == g_fakePreparsedData && caps != nullptr) {
    memset(caps, 0, sizeof(HIDP_CAPS));
    caps->Usage = 0x05; // Generic Desktop / Game Pad
    caps->UsagePage = 0x01;
    caps->InputReportByteLength = 20;
    caps->OutputReportByteLength = 8;
    caps->FeatureReportByteLength = 0;
    caps->NumberLinkCollectionNodes = 1;
    caps->NumberInputButtonCaps = 10;
    caps->NumberInputValueCaps = 6;
    caps->NumberInputDataIndices = 16;
    caps->NumberOutputButtonCaps = 1; // LED
    caps->NumberOutputValueCaps = 0;
    caps->NumberOutputDataIndices = 1;
    caps->NumberFeatureButtonCaps = 0;
    caps->NumberFeatureValueCaps = 0;
    caps->NumberFeatureDataIndices = 0;
    HidLogFake("hid: HidP_GetCaps fake -> page=01 usage=05 btn=10 val=6 in=20 out=8");
    return HIDP_STATUS_SUCCESS;
  }
  return g_realHidPGetCaps(preparsedData, caps);
}

static NTSTATUS WINAPI HidP_GetValueCaps_Hook(HIDP_REPORT_TYPE reportType,
                                              PHIDP_VALUE_CAPS valueCaps, PUSHORT valueCapsLength,
                                              PHIDP_PREPARSED_DATA preparsedData) {
  if (preparsedData != nullptr && preparsedData == g_fakePreparsedData) {
    if (reportType != HidP_Input || valueCapsLength == nullptr) {
      if (valueCapsLength != nullptr)
        *valueCapsLength = 0;
      return HIDP_STATUS_SUCCESS;
    }
    const USHORT need = 6;
    if (*valueCapsLength < need) {
      *valueCapsLength = need;
      return HIDP_STATUS_BUFFER_TOO_SMALL;
    }
    static const struct {
      USHORT usage;
      USHORT bitSize;
      LONG lo;
      LONG hi;
    } kVal[6] = {
        {0x30, 16, -32768, 32767}, // X
        {0x31, 16, -32768, 32767}, // Y
        {0x33, 16, -32768, 32767}, // Rx
        {0x34, 16, -32768, 32767}, // Ry
        {0x32, 8, 0, 255},         // Z (LT)
        {0x35, 8, 0, 255},         // Rz (RT)
    };
    for (ULONG i = 0; i < need; ++i) {
      HIDP_VALUE_CAPS c = {};
      c.UsagePage = 0x01;
      c.LinkUsagePage = 0x01;
      c.LinkUsage = 0x05;
      c.IsRange = FALSE;
      c.IsAbsolute = TRUE;
      c.HasNull = FALSE;
      c.BitSize = kVal[i].bitSize;
      c.ReportCount = 1;
      c.LogicalMin = kVal[i].lo;
      c.LogicalMax = kVal[i].hi;
      c.PhysicalMin = kVal[i].lo;
      c.PhysicalMax = kVal[i].hi;
      c.NotRange.Usage = kVal[i].usage;
      c.NotRange.DataIndex = (USHORT)(10 + i);
      valueCaps[i] = c;
    }
    *valueCapsLength = need;
    HidLogFake("hid: HidP_GetValueCaps fake -> 6 values");
    return HIDP_STATUS_SUCCESS;
  }
  return g_realHidPGetValueCaps(reportType, valueCaps, valueCapsLength, preparsedData);
}

static NTSTATUS WINAPI HidP_GetButtonCaps_Hook(HIDP_REPORT_TYPE reportType,
                                               PHIDP_BUTTON_CAPS buttonCaps,
                                               PUSHORT buttonCapsLength,
                                               PHIDP_PREPARSED_DATA preparsedData) {
  if (preparsedData != nullptr && preparsedData == g_fakePreparsedData) {
    if (reportType != HidP_Input || buttonCapsLength == nullptr) {
      if (buttonCapsLength != nullptr)
        *buttonCapsLength = 0;
      return HIDP_STATUS_SUCCESS;
    }
    const USHORT need = 10;
    if (*buttonCapsLength < need) {
      *buttonCapsLength = need;
      return HIDP_STATUS_BUFFER_TOO_SMALL;
    }
    for (ULONG i = 0; i < need; ++i) {
      HIDP_BUTTON_CAPS c = {};
      c.UsagePage = 0x09; // Button
      c.LinkUsagePage = 0x01;
      c.LinkUsage = 0x05;
      c.IsRange = FALSE;
      c.IsAbsolute = TRUE;
      c.NotRange.Usage = (USHORT)(i + 1); // A..RThumb
      c.NotRange.DataIndex = (USHORT)i;
      buttonCaps[i] = c;
    }
    *buttonCapsLength = need;
    HidLogFake("hid: HidP_GetButtonCaps fake -> 10 buttons");
    return HIDP_STATUS_SUCCESS;
  }
  return g_realHidPGetButtonCaps(reportType, buttonCaps, buttonCapsLength, preparsedData);
}

static BOOLEAN WINAPI HidD_GetNumInputBuffers_Hook(HANDLE hidDeviceObject, PULONG numBuffers) {
  if (hidDeviceObject == kFakeHidHandle) {
    HidLogFake("hid: HidD_GetNumInputBuffers fake -> 32");
    if (numBuffers != nullptr)
      *numBuffers = 32;
    return TRUE;
  }
  return g_realHidDGetNumInputBuffers(hidDeviceObject, numBuffers);
}

static BOOLEAN WINAPI HidD_SetNumInputBuffers_Hook(HANDLE hidDeviceObject, ULONG numBuffers) {
  if (hidDeviceObject == kFakeHidHandle)
    return TRUE;
  return g_realHidDSetNumInputBuffers(hidDeviceObject, numBuffers);
}

static BOOLEAN WINAPI HidD_GetFeature_Hook(HANDLE hidDeviceObject, PVOID reportBuffer,
                                           ULONG reportBufferLength) {
  if (hidDeviceObject == kFakeHidHandle) {
    if (reportBuffer != nullptr && reportBufferLength != 0)
      memset(reportBuffer, 0, reportBufferLength);
    return TRUE;
  }
  return g_realHidDGetFeature(hidDeviceObject, reportBuffer, reportBufferLength);
}

static BOOLEAN WINAPI HidD_SetFeature_Hook(HANDLE hidDeviceObject, PVOID reportBuffer,
                                           ULONG reportBufferLength) {
  if (hidDeviceObject == kFakeHidHandle)
    return TRUE;
  return g_realHidDSetFeature(hidDeviceObject, reportBuffer, reportBufferLength);
}

static BOOLEAN WINAPI HidD_GetInputReport_Hook(HANDLE hidDeviceObject, PVOID reportBuffer,
                                               ULONG reportBufferLength) {
  if (hidDeviceObject == kFakeHidHandle) {
    if (reportBuffer != nullptr && reportBufferLength != 0)
      memset(reportBuffer, 0, reportBufferLength);
    return TRUE;
  }
  return g_realHidDGetInputReport(hidDeviceObject, reportBuffer, reportBufferLength);
}

static BOOLEAN WINAPI HidD_GetOutputReport_Hook(HANDLE hidDeviceObject, PVOID reportBuffer,
                                                ULONG reportBufferLength) {
  if (hidDeviceObject == kFakeHidHandle) {
    if (reportBuffer != nullptr && reportBufferLength != 0)
      memset(reportBuffer, 0, reportBufferLength);
    return TRUE;
  }
  return g_realHidDGetOutputReport(hidDeviceObject, reportBuffer, reportBufferLength);
}

static BOOLEAN WINAPI HidD_GetProductString_Hook(HANDLE hidDeviceObject, PVOID buffer,
                                                 ULONG bufferLength) {
  if (hidDeviceObject == kFakeHidHandle) {
    static const wchar_t kProduct[] = L"Xbox 360 Controller";
    const ULONG bytes = (ULONG)((wcslen(kProduct) + 1) * sizeof(wchar_t));
    if (buffer == nullptr || bufferLength < bytes)
      return FALSE;
    memcpy(buffer, kProduct, bytes);
    HidLogFake("hid: HidD_GetProductString fake -> Xbox 360 Controller");
    return TRUE;
  }
  return g_realHidDGetProductString(hidDeviceObject, buffer, bufferLength);
}

static BOOLEAN WINAPI HidD_GetSerialNumberString_Hook(HANDLE hidDeviceObject, PVOID buffer,
                                                      ULONG bufferLength) {
  if (hidDeviceObject == kFakeHidHandle)
    return FALSE;
  return g_realHidDGetSerialNumberString(hidDeviceObject, buffer, bufferLength);
}

static BOOLEAN WINAPI HidD_GetIndexedString_Hook(HANDLE hidDeviceObject, ULONG stringIndex,
                                                 PVOID buffer, ULONG bufferLength) {
  if (hidDeviceObject == kFakeHidHandle)
    return FALSE;
  return g_realHidDGetIndexedString(hidDeviceObject, stringIndex, buffer, bufferLength);
}

static BOOL WINAPI DeviceIoControl_Hook(HANDLE device, DWORD ioControlCode, LPVOID inBuf,
                                        DWORD inBufSize, LPVOID outBuf, DWORD outBufSize,
                                        LPDWORD bytesReturned, LPOVERLAPPED overlapped) {
  if (device == kFakeHidHandle) {
    HidLogFake("hid: DeviceIoControl fake (zero bytes)");
    if (bytesReturned != nullptr)
      *bytesReturned = 0;
    return TRUE;
  }
  return g_realDeviceIoControl(device, ioControlCode, inBuf, inBufSize, outBuf, outBufSize,
                               bytesReturned, overlapped);
}

/**
 * @brief Complete a caller-supplied OVERLAPPED the way the kernel does when an
 * operation finishes synchronously.
 * @param overlapped The OVERLAPPED (may be null).
 * @param bytes Bytes transferred.
 */
static void CompleteFakeOverlapped(LPOVERLAPPED overlapped, DWORD bytes) {
  if (overlapped == nullptr)
    return;
  overlapped->Internal = 0; // STATUS_SUCCESS
  overlapped->InternalHigh = bytes;
  if (overlapped->hEvent != nullptr)
    SetEvent(overlapped->hEvent);
}

static HANDLE WINAPI CreateFileW_Hook(LPCWSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES security, DWORD disposition,
                                      DWORD flags, HANDLE templateFile) {
  if (name != nullptr && wcscmp(name, kFakeHidDevicePath) == 0) {
    HidLogFake("hid: CreateFileW fake path -> virtual handle");
    return kFakeHidHandle;
  }
  return g_realCreateFileW(name, access, share, security, disposition, flags, templateFile);
}

static BOOL WINAPI CloseHandle_Hook(HANDLE object) {
  if (object == kFakeHidHandle)
    return TRUE;
  return g_realCloseHandle(object);
}

static BOOL WINAPI ReadFile_Hook(HANDLE file, LPVOID buffer, DWORD bytesToRead,
                                 LPDWORD bytesRead, LPOVERLAPPED overlapped) {
  if (file == kFakeHidHandle) {
    // A zeroed report reads as "no button pressed, every axis centred".
    if (buffer != nullptr && bytesToRead != 0)
      memset(buffer, 0, bytesToRead);
    if (overlapped != nullptr) {
      CompleteFakeOverlapped(overlapped, bytesToRead);
    } else if (bytesRead != nullptr) {
      *bytesRead = bytesToRead;
    }
    return TRUE;
  }
  return g_realReadFile(file, buffer, bytesToRead, bytesRead, overlapped);
}

static BOOL WINAPI WriteFile_Hook(HANDLE file, LPCVOID buffer, DWORD bytesToWrite,
                                  LPDWORD bytesWritten, LPOVERLAPPED overlapped) {
  if (file == kFakeHidHandle) {
    HidLogFake("hid: WriteFile fake (LED/rumble accepted)");
    if (overlapped != nullptr) {
      CompleteFakeOverlapped(overlapped, bytesToWrite);
    } else if (bytesWritten != nullptr) {
      *bytesWritten = bytesToWrite;
    }
    return TRUE;
  }
  return g_realWriteFile(file, buffer, bytesToWrite, bytesWritten, overlapped);
}

static BOOL WINAPI GetOverlappedResult_Hook(HANDLE file, LPOVERLAPPED overlapped,
                                            LPDWORD bytesTransferred, BOOL wait) {
  if (file == kFakeHidHandle) {
    if (bytesTransferred != nullptr)
      *bytesTransferred = (DWORD)overlapped->InternalHigh;
    return TRUE;
  }
  return g_realGetOverlappedResult(file, overlapped, bytesTransferred, wait);
}

/**
 * @brief Install the SetupDi/HidD virtual HID device (only with XInputFakePad).
 *
 * Detours setupapi!SetupDi* and hid!HidD and HidP plus the kernel32 file
 * APIs that must answer for the virtual handle. The fake device reports Xbox
 * 360 identifiers so a game that gates its XInput path on VID 0x045E switches.
 */
static void HidSetupDiHookInstall(void) {
  if (g_xinputFakeOn != 1) {
    Dlog("hid: virtual HID device skipped (XInputFakePad=0)");
    return;
  }
  InterlockedExchange(&g_hidFakeLogBudget, 24);
  HidDevInfoLockInit();
  InterlockedExchange(&g_fakeDevInfoSetCount, 0);
  FakePreparsedDataInit();

  HMODULE setupapi = GetModuleHandleW(L"setupapi.dll");
  HMODULE hid = GetModuleHandleW(L"hid.dll");
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (setupapi == nullptr)
    setupapi = LoadLibraryW(L"setupapi.dll");
  if (hid == nullptr)
    hid = LoadLibraryW(L"hid.dll");
  if (kernel32 == nullptr)
    kernel32 = LoadLibraryW(L"kernel32.dll");

  if (setupapi != nullptr) {
    HidAttachExport(setupapi, "SetupDiEnumDeviceInterfaces", (void*)&SetupDiEnumDeviceInterfaces_Hook,
                    (void**)&g_realSetupDiEnumDeviceInterfaces,
                    &g_exportSetupDiEnumDeviceInterfaces, &g_hidDetEnumInterfaces);
    HidAttachExport(setupapi, "SetupDiGetDeviceInterfaceDetailW",
                    (void*)&SetupDiGetDeviceInterfaceDetailW_Hook,
                    (void**)&g_realSetupDiGetDeviceInterfaceDetailW,
                    &g_exportSetupDiGetDeviceInterfaceDetailW, &g_hidDetDetailW);
    HidAttachExport(setupapi, "SetupDiGetDeviceInstanceIdW",
                    (void*)&SetupDiGetDeviceInstanceIdW_Hook,
                    (void**)&g_realSetupDiGetDeviceInstanceIdW, &g_exportSetupDiGetDeviceInstanceIdW,
                    &g_hidDetInstanceId);
    HidAttachExport(setupapi, "SetupDiGetDeviceRegistryPropertyW",
                    (void*)&SetupDiGetDeviceRegistryPropertyW_Hook,
                    (void**)&g_realSetupDiGetDeviceRegistryPropertyW,
                    &g_exportSetupDiGetDeviceRegistryPropertyW, &g_hidDetRegPropW);
    HidAttachExport(setupapi, "SetupDiGetDeviceRegistryPropertyA",
                    (void*)&SetupDiGetDeviceRegistryPropertyA_Hook,
                    (void**)&g_realSetupDiGetDeviceRegistryPropertyA,
                    &g_exportSetupDiGetDeviceRegistryPropertyA, &g_hidDetRegPropA);
    HidAttachExport(setupapi, "SetupDiGetClassDevsW", (void*)&SetupDiGetClassDevsW_Hook,
                    (void**)&g_realSetupDiGetClassDevsW, &g_exportSetupDiGetClassDevsW,
                    &g_hidDetClassDevsW);
    HidAttachExport(setupapi, "SetupDiGetClassDevsA", (void*)&SetupDiGetClassDevsA_Hook,
                    (void**)&g_realSetupDiGetClassDevsA, &g_exportSetupDiGetClassDevsA,
                    &g_hidDetClassDevsA);
    HidAttachExport(setupapi, "SetupDiDestroyDeviceInfoList",
                    (void*)&SetupDiDestroyDeviceInfoList_Hook,
                    (void**)&g_realSetupDiDestroyDeviceInfoList,
                    &g_exportSetupDiDestroyDeviceInfoList, &g_hidDetDestroyList);
  } else {
    Dlog("hid: setupapi.dll unavailable");
  }

  if (hid != nullptr) {
    HidAttachExport(hid, "HidD_GetAttributes", (void*)&HidD_GetAttributes_Hook,
                    (void**)&g_realHidDGetAttributes, &g_exportHidDGetAttributes, &g_hidDetAttrib);
    HidAttachExport(hid, "HidD_GetPreparsedData", (void*)&HidD_GetPreparsedData_Hook,
                    (void**)&g_realHidDGetPreparsedData, &g_exportHidDGetPreparsedData,
                    &g_hidDetPrep);
    HidAttachExport(hid, "HidD_FreePreparsedData", (void*)&HidD_FreePreparsedData_Hook,
                    (void**)&g_realHidDFreePreparsedData, &g_exportHidDFreePreparsedData,
                    &g_hidDetFreePrep);
    HidAttachExport(hid, "HidP_GetCaps", (void*)&HidP_GetCaps_Hook, (void**)&g_realHidPGetCaps,
                    &g_exportHidPGetCaps, &g_hidDetCaps);
    HidAttachExport(hid, "HidP_GetValueCaps", (void*)&HidP_GetValueCaps_Hook,
                    (void**)&g_realHidPGetValueCaps, &g_exportHidPGetValueCaps, &g_hidDetValCaps);
    HidAttachExport(hid, "HidP_GetButtonCaps", (void*)&HidP_GetButtonCaps_Hook,
                    (void**)&g_realHidPGetButtonCaps, &g_exportHidPGetButtonCaps, &g_hidDetBtnCaps);
    HidAttachExport(hid, "HidD_GetNumInputBuffers", (void*)&HidD_GetNumInputBuffers_Hook,
                    (void**)&g_realHidDGetNumInputBuffers, &g_exportHidDGetNumInputBuffers,
                    &g_hidDetNumBuf);
    HidAttachExport(hid, "HidD_SetNumInputBuffers", (void*)&HidD_SetNumInputBuffers_Hook,
                    (void**)&g_realHidDSetNumInputBuffers, &g_exportHidDSetNumInputBuffers,
                    &g_hidDetSetNumBuf);
    HidAttachExport(hid, "HidD_GetFeature", (void*)&HidD_GetFeature_Hook,
                    (void**)&g_realHidDGetFeature, &g_exportHidDGetFeature, &g_hidDetFeature);
    HidAttachExport(hid, "HidD_SetFeature", (void*)&HidD_SetFeature_Hook,
                    (void**)&g_realHidDSetFeature, &g_exportHidDSetFeature, &g_hidDetSetFeature);
    HidAttachExport(hid, "HidD_GetInputReport", (void*)&HidD_GetInputReport_Hook,
                    (void**)&g_realHidDGetInputReport, &g_exportHidDGetInputReport,
                    &g_hidDetInReport);
    HidAttachExport(hid, "HidD_GetOutputReport", (void*)&HidD_GetOutputReport_Hook,
                    (void**)&g_realHidDGetOutputReport, &g_exportHidDGetOutputReport,
                    &g_hidDetOutReport);
    HidAttachExport(hid, "HidD_GetProductString", (void*)&HidD_GetProductString_Hook,
                    (void**)&g_realHidDGetProductString, &g_exportHidDGetProductString,
                    &g_hidDetProduct);
    HidAttachExport(hid, "HidD_GetSerialNumberString", (void*)&HidD_GetSerialNumberString_Hook,
                    (void**)&g_realHidDGetSerialNumberString, &g_exportHidDGetSerialNumberString,
                    &g_hidDetSerial);
    HidAttachExport(hid, "HidD_GetIndexedString", (void*)&HidD_GetIndexedString_Hook,
                    (void**)&g_realHidDGetIndexedString, &g_exportHidDGetIndexedString,
                    &g_hidDetIndexed);
  } else {
    Dlog("hid: hid.dll unavailable");
  }

  if (kernel32 != nullptr) {
    HidAttachExport(kernel32, "CreateFileW", (void*)&CreateFileW_Hook, (void**)&g_realCreateFileW,
                    &g_exportCreateFileW, &g_hidDetCreateFileW);
    HidAttachExport(kernel32, "CloseHandle", (void*)&CloseHandle_Hook, (void**)&g_realCloseHandle,
                    &g_exportCloseHandle, &g_hidDetCloseHandle);
    HidAttachExport(kernel32, "ReadFile", (void*)&ReadFile_Hook, (void**)&g_realReadFile,
                    &g_exportReadFile, &g_hidDetReadFile);
    HidAttachExport(kernel32, "WriteFile", (void*)&WriteFile_Hook, (void**)&g_realWriteFile,
                    &g_exportWriteFile, &g_hidDetWriteFile);
    HidAttachExport(kernel32, "GetOverlappedResult", (void*)&GetOverlappedResult_Hook,
                    (void**)&g_realGetOverlappedResult, &g_exportGetOverlappedResult,
                    &g_hidDetGetOverlapped);
    HidAttachExport(kernel32, "DeviceIoControl", (void*)&DeviceIoControl_Hook,
                    (void**)&g_realDeviceIoControl, &g_exportDeviceIoControl, &g_hidDetIoControl);
  } else {
    Dlog("hid: kernel32.dll unavailable");
  }
  Dlog("hid: SetupDi + HID hooks installed");
}

/**
 * @brief Undo the SetupDi/HidD/kernel32 detours.
 */
static void HidSetupDiHookRestore(void) {
  DetourRestore(&g_hidDetEnumInterfaces, (void**)&g_realSetupDiEnumDeviceInterfaces,
                g_exportSetupDiEnumDeviceInterfaces);
  DetourRestore(&g_hidDetDetailW, (void**)&g_realSetupDiGetDeviceInterfaceDetailW,
                g_exportSetupDiGetDeviceInterfaceDetailW);
  DetourRestore(&g_hidDetInstanceId, (void**)&g_realSetupDiGetDeviceInstanceIdW,
                g_exportSetupDiGetDeviceInstanceIdW);
  DetourRestore(&g_hidDetRegPropW, (void**)&g_realSetupDiGetDeviceRegistryPropertyW,
                g_exportSetupDiGetDeviceRegistryPropertyW);
  DetourRestore(&g_hidDetRegPropA, (void**)&g_realSetupDiGetDeviceRegistryPropertyA,
                g_exportSetupDiGetDeviceRegistryPropertyA);
  DetourRestore(&g_hidDetClassDevsW, (void**)&g_realSetupDiGetClassDevsW,
                g_exportSetupDiGetClassDevsW);
  DetourRestore(&g_hidDetClassDevsA, (void**)&g_realSetupDiGetClassDevsA,
                g_exportSetupDiGetClassDevsA);
  DetourRestore(&g_hidDetDestroyList, (void**)&g_realSetupDiDestroyDeviceInfoList,
                g_exportSetupDiDestroyDeviceInfoList);
  DetourRestore(&g_hidDetAttrib, (void**)&g_realHidDGetAttributes, g_exportHidDGetAttributes);
  DetourRestore(&g_hidDetPrep, (void**)&g_realHidDGetPreparsedData, g_exportHidDGetPreparsedData);
  DetourRestore(&g_hidDetFreePrep, (void**)&g_realHidDFreePreparsedData,
                g_exportHidDFreePreparsedData);
  DetourRestore(&g_hidDetCaps, (void**)&g_realHidPGetCaps, g_exportHidPGetCaps);
  DetourRestore(&g_hidDetValCaps, (void**)&g_realHidPGetValueCaps, g_exportHidPGetValueCaps);
  DetourRestore(&g_hidDetBtnCaps, (void**)&g_realHidPGetButtonCaps, g_exportHidPGetButtonCaps);
  DetourRestore(&g_hidDetNumBuf, (void**)&g_realHidDGetNumInputBuffers,
                g_exportHidDGetNumInputBuffers);
  DetourRestore(&g_hidDetSetNumBuf, (void**)&g_realHidDSetNumInputBuffers,
                g_exportHidDSetNumInputBuffers);
  DetourRestore(&g_hidDetFeature, (void**)&g_realHidDGetFeature, g_exportHidDGetFeature);
  DetourRestore(&g_hidDetSetFeature, (void**)&g_realHidDSetFeature, g_exportHidDSetFeature);
  DetourRestore(&g_hidDetInReport, (void**)&g_realHidDGetInputReport, g_exportHidDGetInputReport);
  DetourRestore(&g_hidDetOutReport, (void**)&g_realHidDGetOutputReport,
                g_exportHidDGetOutputReport);
  DetourRestore(&g_hidDetProduct, (void**)&g_realHidDGetProductString,
                g_exportHidDGetProductString);
  DetourRestore(&g_hidDetSerial, (void**)&g_realHidDGetSerialNumberString,
                g_exportHidDGetSerialNumberString);
  DetourRestore(&g_hidDetIndexed, (void**)&g_realHidDGetIndexedString,
                g_exportHidDGetIndexedString);
  DetourRestore(&g_hidDetCreateFileW, (void**)&g_realCreateFileW, g_exportCreateFileW);
  DetourRestore(&g_hidDetCloseHandle, (void**)&g_realCloseHandle, g_exportCloseHandle);
  DetourRestore(&g_hidDetReadFile, (void**)&g_realReadFile, g_exportReadFile);
  DetourRestore(&g_hidDetWriteFile, (void**)&g_realWriteFile, g_exportWriteFile);
  DetourRestore(&g_hidDetGetOverlapped, (void**)&g_realGetOverlappedResult,
                g_exportGetOverlappedResult);
  DetourRestore(&g_hidDetIoControl, (void**)&g_realDeviceIoControl, g_exportDeviceIoControl);
  Dlog("hid: SetupDi + HID hooks restored");
}

/* ------------------------------------------------------------------ */
/* Windows.Gaming.Input fake gamepad (XInputFakeWgi=1)                */
/* ------------------------------------------------------------------ */
/* UE5 titles (Split Fiction among them) read the controller through
   Windows.Gaming.Input instead of XInput: the WinRT Gamepad statics
   enumerate and serve state from system code behind combase, so the
   game never calls an XInput export and the whole XInput-side illusion
   is invisible to it. This layer detours combase!RoGetActivationFactory
   for L"Windows.Gaming.Input.Gamepad" and hands the game a fake statics
   object. get_Gamepads passes the REAL system view through whenever the
   OS actually has a controller (a real pad always wins) and otherwise
   presents ONE connected fake pad; the pad's GetCurrentReading is fed
   from the same loopback UDP snapshots as the XInput rewrite, and its
   rumble writes are consumed rather than forwarded. Every other
   Windows.Gaming.Input class (RawGameController and friends) passes
   through untouched, but each class name is logged once so a live
   session shows which other WinRT input surfaces the title touches. */

/** HSTRING stand-in: WinRT string handles stay opaque to this layer. */
typedef void* WgiHstring;

/** WinRT EventRegistrationToken: one int64 field, passed by value. */
struct WgiEventToken {
  int64_t value; ///< Opaque per-subscription token.
};

/* Interface IIDs (SDK 10.0.26100 winrt/windows.gaming.input.h, including
   the parameterized collection instantiations). */
static const GUID kWgiIidUnknown = {0x00000000, 0x0000, 0x0000,
                                    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kWgiIidInspectable = {0xaf86e235, 0x12b5, 0x4c6a,
                                        {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
static const GUID kWgiIidActivationFactory = {0x00000035, 0x0000, 0x0000,
                                              {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kWgiIidIGamepadStatics = {0x8bbce529, 0xd49c, 0x39e9,
                                            {0x95, 0x60, 0xe4, 0x7d, 0xde, 0x96, 0xb7, 0xc8}};
static const GUID kWgiIidIGamepadStatics2 = {0x42676dc5, 0x0856, 0x47c4,
                                             {0x92, 0x13, 0xb3, 0x95, 0x50, 0x4c, 0x3a, 0x3c}};
static const GUID kWgiIidIGamepad = {0xbc7bb43c, 0x0a69, 0x3903,
                                     {0x9e, 0x9d, 0xa5, 0x0f, 0x86, 0xa4, 0x5d, 0xe5}};
static const GUID kWgiIidIGamepad2 = {0x3c1689bd, 0x5915, 0x4245,
                                      {0xb0, 0xc0, 0xc8, 0x9f, 0xae, 0x03, 0x08, 0xff}};
static const GUID kWgiIidIGameController = {0x1baf6522, 0x5f64, 0x42c5,
                                            {0x82, 0x67, 0xb9, 0xfe, 0x22, 0x15, 0xbf, 0xbd}};
static const GUID kWgiIidIGameControllerBatteryInfo = {0xdcecc681, 0x3963, 0x4da6,
                                                        {0x95, 0x5d, 0x55, 0x3f, 0x3b, 0x6f, 0x61, 0x61}};
static const GUID kWgiIidVectorViewGamepad = {0xeb97bb69, 0x09c9, 0x5a99,
                                              {0x86, 0xb2, 0x3e, 0x36, 0x08, 0x52, 0x84, 0xd4}};
static const GUID kWgiIidIterableGamepad = {0x47132ba0, 0x6b17, 0x5cd2,
                                            {0xa8, 0xbd, 0xb5, 0xd3, 0x44, 0x3c, 0xcb, 0x13}};
static const GUID kWgiIidIteratorGamepad = {0x246737e8, 0x12bc, 0x5c64,
                                             {0xaf, 0x52, 0x06, 0xdb, 0x4b, 0x13, 0xfa, 0x2f}};

/* RawGameController interface IIDs (SDK 10.0.26100 winrt/windows.gaming.input.h).
   UE5 discovers and reads controllers through IRawGameController, not IGamepad:
   the engine subscribes to RawGameControllerAdded and calls GetCurrentReading on
   the raw controller, so the Gamepad-only fake is invisible to it. These IIDs
   drive the parallel RawGameController statics/pad fake below. */
static const GUID kWgiIidIRawGameController = {0x7cad6d91, 0xa7e1, 0x4f71,
    {0x9a, 0x78, 0x33, 0xe9, 0xc5, 0xdf, 0xea, 0x62}};
static const GUID kWgiIidIRawGameController2 = {0x43c0c035, 0xbb73, 0x4756,
    {0xa7, 0x87, 0x3e, 0xd6, 0xbe, 0xa6, 0x17, 0xbd}};
static const GUID kWgiIidIRawGameControllerStatics = {0xeb8d0792, 0xe95a, 0x4b19,
    {0xaf, 0xc7, 0x0a, 0x59, 0xf8, 0xbf, 0x75, 0x9e}};

/* XInput button masks (XINPUT_GAMEPAD_*), declared locally so the layer is
   independent of the SDK header's include state. */
static const WORD kWgiXiDpadUp = 0x0001;         ///< XINPUT_GAMEPAD_DPAD_UP.
static const WORD kWgiXiDpadDown = 0x0002;       ///< XINPUT_GAMEPAD_DPAD_DOWN.
static const WORD kWgiXiDpadLeft = 0x0004;       ///< XINPUT_GAMEPAD_DPAD_LEFT.
static const WORD kWgiXiDpadRight = 0x0008;      ///< XINPUT_GAMEPAD_DPAD_RIGHT.
static const WORD kWgiXiStart = 0x0010;          ///< XINPUT_GAMEPAD_START.
static const WORD kWgiXiBack = 0x0020;           ///< XINPUT_GAMEPAD_BACK.
static const WORD kWgiXiLeftThumb = 0x0040;      ///< XINPUT_GAMEPAD_LEFT_THUMB.
static const WORD kWgiXiRightThumb = 0x0080;     ///< XINPUT_GAMEPAD_RIGHT_THUMB.
static const WORD kWgiXiLeftShoulder = 0x0100;   ///< XINPUT_GAMEPAD_LEFT_SHOULDER.
static const WORD kWgiXiRightShoulder = 0x0200;  ///< XINPUT_GAMEPAD_RIGHT_SHOULDER.
static const WORD kWgiXiA = 0x1000;              ///< XINPUT_GAMEPAD_A.
static const WORD kWgiXiB = 0x2000;              ///< XINPUT_GAMEPAD_B.
static const WORD kWgiXiX = 0x4000;              ///< XINPUT_GAMEPAD_X.
static const WORD kWgiXiY = 0x8000;              ///< XINPUT_GAMEPAD_Y.

/* GamepadButtons flags (Windows.Gaming.Input ABI). */
static const std::uint32_t kWgiBtnMenu = 0x0001;          ///< Menu (Start).
static const std::uint32_t kWgiBtnView = 0x0002;           ///< View (Back).
static const std::uint32_t kWgiBtnA = 0x0004;              ///< A button.
static const std::uint32_t kWgiBtnB = 0x0008;              ///< B button.
static const std::uint32_t kWgiBtnX = 0x0010;              ///< X button.
static const std::uint32_t kWgiBtnY = 0x0020;              ///< Y button.
static const std::uint32_t kWgiBtnDpadUp = 0x0040;         ///< D-pad up.
static const std::uint32_t kWgiBtnDpadDown = 0x0080;       ///< D-pad down.
static const std::uint32_t kWgiBtnDpadLeft = 0x0100;       ///< D-pad left.
static const std::uint32_t kWgiBtnDpadRight = 0x0200;      ///< D-pad right.
static const std::uint32_t kWgiBtnLeftShoulder = 0x0400;   ///< Left shoulder.
static const std::uint32_t kWgiBtnRightShoulder = 0x0800;  ///< Right shoulder.
static const std::uint32_t kWgiBtnLeftThumb = 0x1000;      ///< Left stick press.
static const std::uint32_t kWgiBtnRightThumb = 0x2000;     ///< Right stick press.

/**
 * @brief Windows.Gaming.Input.GamepadReading: one controller snapshot.
 */
struct WgiGamepadReading {
  std::uint64_t Timestamp;     ///< Reading time in microseconds (QPC clock).
  std::uint32_t Buttons;       ///< GamepadButtons flags.
  double LeftTrigger;          ///< Left trigger, 0..1.
  double RightTrigger;         ///< Right trigger, 0..1.
  double LeftThumbstickX;      ///< Left stick X, -1..1.
  double LeftThumbstickY;      ///< Left stick Y, -1..1.
  double RightThumbstickX;     ///< Right stick X, -1..1.
  double RightThumbstickY;     ///< Right stick Y, -1..1.
};

/**
 * @brief Windows.Gaming.Input.GamepadVibration: rumble levels.
 */
struct WgiGamepadVibration {
  double LeftMotor;    ///< Low-frequency motor, 0..1.
  double RightMotor;   ///< High-frequency motor, 0..1.
  double LeftTrigger;  ///< Left trigger actuator, 0..1.
  double RightTrigger; ///< Right trigger actuator, 0..1.
};

/**
 * @brief Minimal IInspectable: the six vtable slots every WinRT interface
 * starts with.
 */
struct WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) = 0;
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount, IID** iids) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(WgiHstring* className) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int* trustLevel) = 0;
};

/**
 * @brief IGamepad ABI (gamepad state and rumble).
 */
struct WgiIGamepad : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Vibration(WgiGamepadVibration* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Vibration(WgiGamepadVibration value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentReading(WgiGamepadReading* value) = 0;
};

/**
 * @brief IGamepad2 ABI (per-button label probing).
 */
struct WgiIGamepad2 : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetButtonLabel(std::int32_t button,
                                                   std::int32_t* value) = 0;
};

/**
 * @brief IGameController ABI (headset / user plumbing shared by all pads).
 */
struct WgiIGameController : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE add_HeadsetConnected(void* value, WgiEventToken* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_HeadsetConnected(WgiEventToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_HeadsetDisconnected(void* value, WgiEventToken* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_HeadsetDisconnected(WgiEventToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_UserChanged(void* value, WgiEventToken* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_UserChanged(WgiEventToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Headset(void** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsWireless(unsigned char* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_User(void** value) = 0;
};

/**
 * @brief IGameControllerBatteryInfo ABI.
 */
struct WgiIGameControllerBatteryInfo : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE TryGetBatteryReport(void** value) = 0;
};

/**
 * @brief IEventHandler<Gamepad> ABI: the game's GamepadAdded/Removed callback.
 */
struct WgiIEventHandlerGamepad : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE Invoke(WgiIInspectable* sender,
                                           WgiIGamepad* args) = 0;
};

/**
 * @brief IIterator<Gamepad> ABI (one-pass enumeration cursor).
 */
struct WgiIIteratorGamepad : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Current(WgiIGamepad** result) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_HasCurrent(unsigned char* result) = 0;
  virtual HRESULT STDMETHODCALLTYPE MoveNext(unsigned char* result) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetMany(std::uint32_t itemsLength,
                                            WgiIGamepad** items,
                                            std::uint32_t* result) = 0;
};

/**
 * @brief IIterable<Gamepad> ABI (range-entry point; C++/WinRT's begin()).
 */
struct WgiIIterableGamepad : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE First(WgiIIteratorGamepad** result) = 0;
};

/**
 * @brief IVectorView<Gamepad> ABI (the Gamepads list snapshot).
 */
struct WgiIVectorViewGamepad : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetAt(std::uint32_t index, WgiIGamepad** result) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Size(std::uint32_t* result) = 0;
  virtual HRESULT STDMETHODCALLTYPE IndexOf(WgiIGamepad* value, std::uint32_t* index,
                                            unsigned char* result) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetMany(std::uint32_t startIndex,
                                            std::uint32_t itemsLength, WgiIGamepad** items,
                                            std::uint32_t* result) = 0;
};

/**
 * @brief IRawGameController ABI (generic button/axis/switch controller state).
 *
 * UE5 reads input through this interface rather than IGamepad: GetCurrentReading
 * returns raw boolean button / double axis / switch arrays. The vtable order
 * matches the SDK (winrt/windows.gaming.input.h) exactly.
 */
struct WgiIRawGameController : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_AxisCount(std::int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ButtonCount(std::int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ForceFeedbackMotors(void** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_HardwareProductId(std::uint16_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_HardwareVendorId(std::uint16_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_SwitchCount(std::int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetButtonLabel(std::int32_t buttonIndex,
                                                    std::int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentReading(std::uint32_t buttonArrayLength,
                                                       unsigned char* buttonArray,
                                                       std::uint32_t switchArrayLength,
                                                       std::int32_t* switchArray,
                                                       std::uint32_t axisArrayLength,
                                                       double* axisArray,
                                                       std::uint64_t* timestamp) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetSwitchKind(std::int32_t switchIndex,
                                                   std::int32_t* value) = 0;
};

/**
 * @brief IRawGameController2 ABI (display name / haptics, UE5 5.0+).
 */
struct WgiIRawGameController2 : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_SimpleHapticsControllers(void** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_NonRoamableId(WgiHstring* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DisplayName(WgiHstring* value) = 0;
};

/**
 * @brief IEventHandler<RawGameController> ABI: the game's Added/Removed callback.
 */
struct WgiIEventHandlerRawGameController : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE Invoke(WgiIInspectable* sender,
                                            WgiIRawGameController* args) = 0;
};

/**
 * @brief IVectorView<RawGameController> ABI (the RawGameControllers list).
 */
struct WgiIVectorViewRawGameController : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetAt(std::uint32_t index,
                                          WgiIRawGameController** result) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Size(std::uint32_t* result) = 0;
  virtual HRESULT STDMETHODCALLTYPE IndexOf(WgiIRawGameController* value,
                                            std::uint32_t* index,
                                            unsigned char* result) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetMany(std::uint32_t startIndex,
                                            std::uint32_t itemsLength,
                                            WgiIRawGameController** items,
                                            std::uint32_t* result) = 0;
};

/**
 * @brief IIterable<RawGameController> ABI (range-for entry; First()).
 */
struct WgiIIteratorRawGameController; /* Forward: First() returns this cursor. */
struct WgiIIterableRawGameController : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE First(WgiIIteratorRawGameController** result) = 0;
};

/**
 * @brief IIterator<RawGameController> ABI (one-pass enumeration cursor).
 */
struct WgiIIteratorRawGameController : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Current(WgiIRawGameController** result) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_HasCurrent(unsigned char* result) = 0;
  virtual HRESULT STDMETHODCALLTYPE MoveNext(unsigned char* result) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetMany(std::uint32_t itemsLength,
                                            WgiIRawGameController** items,
                                            std::uint32_t* result) = 0;
};

/**
 * @brief IRawGameControllerStatics ABI (the RawGameController activation factory).
 */
struct WgiIRawGameControllerStatics : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE add_RawGameControllerAdded(
      WgiIEventHandlerRawGameController* value, WgiEventToken* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_RawGameControllerAdded(WgiEventToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_RawGameControllerRemoved(
      WgiIEventHandlerRawGameController* value, WgiEventToken* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_RawGameControllerRemoved(WgiEventToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_RawGameControllers(
      WgiIVectorViewRawGameController** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE FromGameController(void* gameController,
                                                       WgiIRawGameController** value) = 0;
};

/**
 * @brief Generic empty IVectorView<T> ABI for out-params that must be a valid
 *        collection but hold no elements (ForceFeedbackMotors,
 *        SimpleHapticsControllers). The element type is opaque at the ABI level
 *        (GetAt returns T** as void**), so one implementation serves all T.
 */
struct WgiIVectorViewEmpty : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetAt(std::uint32_t index, void** result) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Size(std::uint32_t* result) = 0;
  virtual HRESULT STDMETHODCALLTYPE IndexOf(void* value, std::uint32_t* index,
                                            unsigned char* result) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetMany(std::uint32_t startIndex,
                                            std::uint32_t itemsLength, void** items,
                                            std::uint32_t* result) = 0;
};

/**
 * @brief IActivationFactory ABI (default activation entry of every statics).
 */
struct WgiIActivationFactory : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE ActivateInstance(WgiIInspectable** instance) = 0;
};

/**
 * @brief IGamepadStatics ABI (the Gamepad runtime-class activation factory).
 */
struct WgiIGamepadStatics : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE add_GamepadAdded(WgiIEventHandlerGamepad* value,
                                                    WgiEventToken* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_GamepadAdded(WgiEventToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_GamepadRemoved(WgiIEventHandlerGamepad* value,
                                                       WgiEventToken* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_GamepadRemoved(WgiEventToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Gamepads(WgiIVectorViewGamepad** value) = 0;
};

/**
 * @brief IGamepadStatics2 ABI (FromGameController).
 */
struct WgiIGamepadStatics2 : public WgiIInspectable {
  virtual HRESULT STDMETHODCALLTYPE FromGameController(void* controller,
                                                       WgiIGamepad** value) = 0;
};

/** Maximum stored GamepadAdded / GamepadRemoved handlers. */
#define WGI_MAX_HANDLERS 8

static CRITICAL_SECTION g_wgiLock;               ///< Guards all WGI mutable state.
static volatile LONG g_wgiLockInit = 0;          ///< 1 after InitializeCriticalSection.
static volatile LONG g_wgiInstalled = 0;         ///< 1 while the WGI fake layer is live.
static volatile LONG64 g_wgiRoGetCalls = 0;      ///< Entries into the hooked factory resolver.
static volatile LONG64 g_wgiGamepadActivations = 0; ///< Gamepad-class activations intercepted.
static volatile LONG64 g_wgiListCalls = 0;        ///< get_Gamepads calls answered by the fake.
static volatile LONG64 g_wgiReadCalls = 0;       ///< GetCurrentReading calls answered by the fake.
static volatile LONG64 g_wgiServedFresh = 0;     ///< Readings served from a fresh UDP snapshot.
static volatile LONG64 g_wgiAddedSubs = 0;        ///< GamepadAdded subscriptions taken.
static volatile LONG64 g_wgiVibCalls = 0;        ///< Rumble writes consumed by the fake pad.
static volatile LONG64 g_wgiListLogged = 0;      ///< One-shot: first get_Gamepads served.
static volatile LONG64 g_wgiAddLogged = 0;       ///< One-shot: first add_GamepadAdded intercepted.
static volatile LONG64 g_wgiReadLogged = 0;      ///< One-shot: first reading served.
static volatile LONG64 g_wgiAddedLogged = 0;     ///< One-shot: first subscription taken.
static volatile LONG64 g_wgiVibLogged = 0;       ///< One-shot: first rumble consumed.
static volatile LONG64 g_wgiActLogged = 0;       ///< One-shot: first activation intercepted.
static std::uint64_t g_wgiQpcFreqHz = 0;         ///< Cached QPC frequency for timestamps.
static WgiGamepadVibration g_wgiVibration;       ///< Last rumble write (consumed, stored).
static WgiIGamepadStatics* g_wgiRealStatics = nullptr;   ///< OS statics, for pass-through.
static WgiIGamepadStatics2* g_wgiRealStatics2 = nullptr;  ///< OS FromGameController passthrough.
static WgiIEventHandlerGamepad* g_wgiAddedHandlers[WGI_MAX_HANDLERS];   ///< Added callbacks.
static WgiIEventHandlerGamepad* g_wgiRemovedHandlers[WGI_MAX_HANDLERS]; ///< Removed callbacks.
static wchar_t g_wgiSeenNames[16][48];           ///< Already-logged WGI class names.
static int g_wgiSeenCount = 0;                   ///< Number of names logged so far.
static void** g_wgiOrigStaticsVtable = nullptr;  ///< Original IGamepadStatics vtable (for restore).
static void** g_wgiFakeStaticsVtable = nullptr;  ///< Copied IGamepadStatics vtable with fake slots.
static void** g_wgiOrigStatics2Vtable = nullptr; ///< Original IGamepadStatics2 vtable (for restore).
static void** g_wgiFakeStatics2Vtable = nullptr; ///< Copied IGamepadStatics2 vtable with fake slots.

/* RawGameController fake-layer state (parallel to the Gamepad fakes above).
   UE5 discovers controllers through the RawGameControllerAdded event, so the
   Added handler is fired once with the fake pad the moment the game subscribes
   (real WGI never raises Added for pads already enumerable, but the fake pad is
   materialized on demand, so the subscription is the only discovery signal). */
#define WGI_MAX_RAW_HANDLERS 8
static WgiIRawGameController* g_wgiFakeRawPadI = nullptr; ///< Fake raw pad subobject pointer.
static WgiIRawGameControllerStatics* g_wgiRealRawStatics = nullptr; ///< OS statics, passthrough.
static WgiIEventHandlerRawGameController* g_wgiRawAddedHandlers[WGI_MAX_RAW_HANDLERS];
static WgiIEventHandlerRawGameController* g_wgiRawRemovedHandlers[WGI_MAX_RAW_HANDLERS];
static volatile LONG g_wgiRawAddedFired = 0;  ///< 1 after the Added event was raised.
static GUID g_wgiRawViewIids[4];              ///< Parameterized IIDs gleaned from the real view.
static int g_wgiRawViewIidCount = 0;          ///< Valid entries in g_wgiRawViewIids.
static volatile LONG64 g_wgiRawAddedSubs = 0; ///< RawGameControllerAdded subscriptions.
static volatile LONG64 g_wgiRawListCalls = 0; ///< get_RawGameControllers calls answered.
static volatile LONG64 g_wgiRawReadCalls = 0; ///< RawGameController GetCurrentReading calls.
static volatile LONG64 g_wgiRawServedFresh = 0; ///< Raw readings from a fresh UDP snapshot.
static volatile LONG64 g_wgiRawActLogged = 0; ///< One-shot: first RawGameController activation.
static volatile LONG64 g_wgiRawListLogged = 0; ///< One-shot: first RawGameControllers served.
static volatile LONG64 g_wgiRawReadLogged = 0; ///< One-shot: first raw reading served.
static volatile LONG64 g_wgiRawAddedLogged = 0; ///< One-shot: first Added subscription fired.

typedef HRESULT(WINAPI* FnWgiRoGetActivationFactory)(WgiHstring, REFIID, void**);
typedef PCWSTR(WINAPI* FnWgiGetStringRawBuffer)(WgiHstring, UINT32*);
typedef HRESULT(WINAPI* FnWgiCreateString)(PCWSTR, UINT32, WgiHstring*);
typedef HRESULT(WINAPI* FnWgiDeleteString)(WgiHstring);

static FnWgiRoGetActivationFactory g_realRoGetActivationFactory = nullptr; ///< Trampoline.
static void* g_exportRoGetActivationFactory = nullptr; ///< combase export entry.
static Detour g_detourRoGet;                     ///< Detour record for the resolver.
static FnWgiGetStringRawBuffer g_wgiGetStringRawBuffer = nullptr; ///< HSTRING reader.
static FnWgiCreateString g_wgiCreateString = nullptr;             ///< HSTRING builder.
static FnWgiDeleteString g_wgiDeleteString = nullptr;             ///< HSTRING releaser.

/**
 * @brief Monotonic timestamp in microseconds, the WGI reading clock.
 * @return Microseconds since an arbitrary fixed point (QPC-derived).
 */
static std::uint64_t WgiNowUs(void) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (g_wgiQpcFreqHz == 0)
    return (std::uint64_t)now.QuadPart;
  const std::uint64_t qpc = (std::uint64_t)now.QuadPart;
  return (qpc / g_wgiQpcFreqHz) * 1000000ull +
         ((qpc % g_wgiQpcFreqHz) * 1000000ull) / g_wgiQpcFreqHz;
}

/**
 * @brief Snapshot of XInput slot 0 with the same precedence the XInput
 * rewrite uses: a fresh UDP snapshot wins, otherwise the neutral pad.
 * @param[out] freshOut Set true when a snapshot inside the freshness window
 * was served (false = neutral fake state).
 * @return The gamepad state to present through WGI.
 */
static XINPUT_GAMEPAD WgiSourceState(bool* freshOut) {
  if (freshOut != nullptr)
    *freshOut = false;
  XINPUT_GAMEPAD g = g_xinputFakeNeutralPad;
  if (g_xinputLockInit == 1) {
    EnterCriticalSection(&g_xinputLock);
    const LONG64 ts = g_xinputSlots[0].timestamp;
    if (ts != 0 && (GetTickCount64() - ts) < XINPUT_UDP_FRESH_MS) {
      g = g_xinputSlots[0].gamepad;
      if (freshOut != nullptr)
        *freshOut = true;
    }
    LeaveCriticalSection(&g_xinputLock);
  }
  return g;
}

/**
 * @brief Map an XINPUT_GAMEPAD snapshot to the WGI GamepadReading layout.
 * @param g The XInput-side state.
 * @param[out] out The WGI reading to fill (timestamp from the QPC clock).
 */
static void WgiFillReading(const XINPUT_GAMEPAD& g, WgiGamepadReading* out) {
  std::uint32_t b = 0;
  if (g.wButtons & kWgiXiDpadUp) b |= kWgiBtnDpadUp;
  if (g.wButtons & kWgiXiDpadDown) b |= kWgiBtnDpadDown;
  if (g.wButtons & kWgiXiDpadLeft) b |= kWgiBtnDpadLeft;
  if (g.wButtons & kWgiXiDpadRight) b |= kWgiBtnDpadRight;
  if (g.wButtons & kWgiXiStart) b |= kWgiBtnMenu;
  if (g.wButtons & kWgiXiBack) b |= kWgiBtnView;
  if (g.wButtons & kWgiXiLeftThumb) b |= kWgiBtnLeftThumb;
  if (g.wButtons & kWgiXiRightThumb) b |= kWgiBtnRightThumb;
  if (g.wButtons & kWgiXiLeftShoulder) b |= kWgiBtnLeftShoulder;
  if (g.wButtons & kWgiXiRightShoulder) b |= kWgiBtnRightShoulder;
  if (g.wButtons & kWgiXiA) b |= kWgiBtnA;
  if (g.wButtons & kWgiXiB) b |= kWgiBtnB;
  if (g.wButtons & kWgiXiX) b |= kWgiBtnX;
  if (g.wButtons & kWgiXiY) b |= kWgiBtnY;
  out->Timestamp = WgiNowUs();
  out->Buttons = b;
  out->LeftTrigger = g.bLeftTrigger / 255.0;
  out->RightTrigger = g.bRightTrigger / 255.0;
  out->LeftThumbstickX = g.sThumbLX / 32767.0;
  out->LeftThumbstickY = g.sThumbLY / 32767.0;
  out->RightThumbstickX = g.sThumbRX / 32767.0;
  out->RightThumbstickY = g.sThumbRY / 32767.0;
}

/**
 * @brief Serve GetIids from a fixed GUID list through CoTaskMemAlloc'd memory.
 * @param list The implemented IIDs (caller keeps ownership).
 * @param count Number of entries in list.
 * @param[out] iidCount Receives the count.
 * @param[out] iids Receives the allocated array (caller frees CoTaskMemFree).
 * @return S_OK, or E_POINTER / E_OUTOFMEMORY on failure.
 */
static HRESULT WgiServeIids(const GUID* list, ULONG count, ULONG* iidCount, IID** iids) {
  if (iidCount == nullptr || iids == nullptr)
    return E_POINTER;
  IID* out = (IID*)CoTaskMemAlloc(sizeof(IID) * count);
  if (out == nullptr && count > 0) {
    *iidCount = 0;
    *iids = nullptr;
    return E_OUTOFMEMORY;
  }
  for (ULONG i = 0; i < count; ++i)
    out[i] = list[i];
  *iidCount = count;
  *iids = out;
  return S_OK;
}

/**
 * @brief Serve GetRuntimeClassName from a literal class name.
 * @param name The runtime class name.
 * @param[out] className Receives the new HSTRING (caller deletes it).
 * @return S_OK, E_POINTER, or E_NOTIMPL when combase string helpers are
 * unavailable.
 */
static HRESULT WgiServeClassName(const wchar_t* name, WgiHstring* className) {
  if (className == nullptr)
    return E_POINTER;
  if (g_wgiCreateString == nullptr) {
    *className = nullptr;
    return E_NOTIMPL;
  }
  return g_wgiCreateString(name, (UINT32)wcslen(name), className);
}

/* The fake pad and the fake statics are process-lifetime singletons: games
   store WinRT object identity (QI to IUnknown) and compare it, so the pad
   handed out at every enumeration must be the very same object for the whole
   session. Their Release therefore keeps the object alive (refcount floor of
   one); the per-call list views and iterators are ordinary heap objects. */

/**
 * @brief The fake connected gamepad served through Windows.Gaming.Input.
 *
 * Implements IGamepad, IGamepad2, IGameController and
 * IGameControllerBatteryInfo on one object so every QI path a title takes
   lands on the same pad identity. State comes from WgiSourceState (the UDP
 * rewrite snapshots), rumble writes are consumed and stored, and the
   headset/user/battery plumbing answers the empty-but-valid values a wired
   360 pad would.
 */
class WgiFakeGamepad final : public WgiIGamepad,
                             public WgiIGamepad2,
                             public WgiIGameController,
                             public WgiIGameControllerBatteryInfo {
 public:
  WgiFakeGamepad() : refs_(1) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;
    void* obj = nullptr;
    if (IsEqualGUID(riid, kWgiIidUnknown) || IsEqualGUID(riid, kWgiIidInspectable) ||
        IsEqualGUID(riid, kWgiIidIGamepad)) {
      obj = static_cast<WgiIGamepad*>(this);
    } else if (IsEqualGUID(riid, kWgiIidIGamepad2)) {
      obj = static_cast<WgiIGamepad2*>(this);
    } else if (IsEqualGUID(riid, kWgiIidIGameController)) {
      obj = static_cast<WgiIGameController*>(this);
    } else if (IsEqualGUID(riid, kWgiIidIGameControllerBatteryInfo)) {
      obj = static_cast<WgiIGameControllerBatteryInfo*>(this);
    } else {
      *ppvObject = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    *ppvObject = obj;
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return (ULONG)InterlockedIncrement(&refs_);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    /* Immortal singleton: identity must stay stable for the whole session. */
    if (refs_ > 0)
      InterlockedDecrement(&refs_);
    return (ULONG)refs_;
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount, IID** iids) override {
    static const GUID kIids[] = {kWgiIidIGamepad, kWgiIidIGamepad2, kWgiIidIGameController,
                                 kWgiIidIGameControllerBatteryInfo};
    return WgiServeIids(kIids, 4, iidCount, iids);
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(WgiHstring* className) override {
    return WgiServeClassName(L"Windows.Gaming.Input.Gamepad", className);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(int* trustLevel) override {
    if (trustLevel == nullptr)
      return E_POINTER;
    *trustLevel = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Vibration(WgiGamepadVibration* value) override {
    if (value == nullptr)
      return E_POINTER;
    *value = g_wgiVibration;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_Vibration(WgiGamepadVibration value) override {
    InterlockedIncrement64(&g_wgiVibCalls);
    g_wgiVibration = value;
    if (InterlockedCompareExchange64(&g_wgiVibLogged, 1, 0) == 0)
      Dlog("wgi: put_Vibration consumed (lm=%.3f rm=%.3f lt=%.3f rt=%.3f)",
           value.LeftMotor, value.RightMotor, value.LeftTrigger, value.RightTrigger);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCurrentReading(WgiGamepadReading* value) override {
    if (value == nullptr)
      return E_POINTER;
    InterlockedIncrement64(&g_wgiReadCalls);
    bool fresh = false;
    const XINPUT_GAMEPAD g = WgiSourceState(&fresh);
    WgiFillReading(g, value);
    if (fresh)
      InterlockedIncrement64(&g_wgiServedFresh);
    if (InterlockedCompareExchange64(&g_wgiReadLogged, 1, 0) == 0)
      Dlog("wgi: GetCurrentReading served (fresh=%d btn=%08lx)",
           fresh ? 1 : 0, (unsigned long)value->Buttons);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetButtonLabel(std::int32_t button, std::int32_t* value) override {
    (void)button;
    if (value == nullptr)
      return E_POINTER;
    *value = 0; /* GameControllerButtonLabel::None */
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE add_HeadsetConnected(void* value, WgiEventToken* token) override {
    (void)value;
    if (token == nullptr)
      return E_POINTER;
    token->value = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE remove_HeadsetConnected(WgiEventToken token) override {
    (void)token;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE add_HeadsetDisconnected(void* value, WgiEventToken* token) override {
    (void)value;
    if (token == nullptr)
      return E_POINTER;
    token->value = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE remove_HeadsetDisconnected(WgiEventToken token) override {
    (void)token;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE add_UserChanged(void* value, WgiEventToken* token) override {
    (void)value;
    if (token == nullptr)
      return E_POINTER;
    token->value = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE remove_UserChanged(WgiEventToken token) override {
    (void)token;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Headset(void** value) override {
    if (value == nullptr)
      return E_POINTER;
    *value = nullptr; /* wired pad, no headset attached */
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsWireless(unsigned char* value) override {
    if (value == nullptr)
      return E_POINTER;
    *value = 0; /* wired */
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_User(void** value) override {
    if (value == nullptr)
      return E_POINTER;
    *value = nullptr; /* no user pairing on a wired 360 pad */
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE TryGetBatteryReport(void** value) override {
    if (value == nullptr)
      return E_POINTER;
    *value = nullptr; /* wired: no battery to report */
    return S_OK;
  }

 private:
  volatile LONG refs_; ///< Object reference count (floor of one: immortal).
};

/** The single fake pad object (immortal; created by WgiFakeInstall). */
static WgiFakeGamepad* g_wgiFakePadObj = nullptr;
/** The pad's IGamepad subobject pointer, for identity compares and handouts. */
static WgiIGamepad* g_wgiFakePadI = nullptr;

/**
 * @brief The fake pad's IGamepad interface pointer.
 * @return The subobject pointer handed to the game, or nullptr before
 * install.
 */
static WgiIGamepad* WgiFakePadInterface(void) {
  return g_wgiFakePadI;
}

/**
 * @brief Hand the fake pad out through an out parameter (AddRef + assign).
 * @param[out] out Receives the pad's IGamepad pointer.
 */
static void WgiAcquireFakePad(WgiIGamepad** out) {
  if (out == nullptr || g_wgiFakePadObj == nullptr)
    return;
  g_wgiFakePadObj->AddRef();
  *out = g_wgiFakePadI;
}

/**
 * @brief AddRef the captured OS statics for a pass-through call.
 * @return The OS statics (caller releases), or nullptr when none was
 * captured.
 */

/**
 * @brief Whether an IGameController interface pointer is the fake pad.
 * @param controller The controller to test (any of its WinRT interfaces).
 * @return True when the object is the fake pad.
 */
static bool WgiIsFakePadController(void* controller) {
  if (controller == nullptr || g_wgiFakePadObj == nullptr)
    return false;
  WgiIGamepad* probe = nullptr;
  WgiIInspectable* insp = (WgiIInspectable*)controller;
  if (FAILED(insp->QueryInterface(kWgiIidIGamepad, (void**)&probe)) || probe == nullptr)
    return false;
  const bool ours = (probe == g_wgiFakePadI);
  probe->Release();
  return ours;
}

/**
 * @brief The fake IIterator over the one-element pad list.
 *
 * Also answers IIterable<Gamepad> (WinRT iterators conceptually re-enter
 * iteration) so a QI on the cursor keeps working.
 */
class WgiFakeVectorIterator final : public WgiIIteratorGamepad,
                                    public WgiIIterableGamepad {
 public:
  WgiFakeVectorIterator() : refs_(1), pos_(0) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;
    void* obj = nullptr;
    if (IsEqualGUID(riid, kWgiIidUnknown) || IsEqualGUID(riid, kWgiIidInspectable) ||
        IsEqualGUID(riid, kWgiIidIteratorGamepad)) {
      obj = static_cast<WgiIIteratorGamepad*>(this);
    } else if (IsEqualGUID(riid, kWgiIidIterableGamepad)) {
      obj = static_cast<WgiIIterableGamepad*>(this);
    } else {
      *ppvObject = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    *ppvObject = obj;
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return (ULONG)InterlockedIncrement(&refs_);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG r = InterlockedDecrement(&refs_);
    if (r == 0)
      delete this;
    return (ULONG)(r > 0 ? r : 0);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount, IID** iids) override {
    static const GUID kIids[] = {kWgiIidIteratorGamepad, kWgiIidIterableGamepad};
    return WgiServeIids(kIids, 2, iidCount, iids);
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(WgiHstring* className) override {
    return WgiServeClassName(
        L"Windows.Foundation.Collections.IIterator`1<Windows.Gaming.Input.Gamepad>",
        className);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(int* trustLevel) override {
    if (trustLevel == nullptr)
      return E_POINTER;
    *trustLevel = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Current(WgiIGamepad** result) override {
    if (result == nullptr)
      return E_POINTER;
    if (pos_ >= 1)
      return E_BOUNDS;
    WgiAcquireFakePad(result);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_HasCurrent(unsigned char* result) override {
    if (result == nullptr)
      return E_POINTER;
    *result = (pos_ < 1) ? 1 : 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE MoveNext(unsigned char* result) override {
    if (result == nullptr)
      return E_POINTER;
    pos_ += (pos_ < 1) ? 1 : 0;
    *result = (pos_ < 1) ? 1 : 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMany(std::uint32_t itemsLength, WgiIGamepad** items,
                                    std::uint32_t* result) override {
    if (result == nullptr || (itemsLength > 0 && items == nullptr))
      return E_POINTER;
    std::uint32_t copied = 0;
    if (items != nullptr) {
      for (; copied < itemsLength && pos_ < 1; ++copied, ++pos_)
        WgiAcquireFakePad(&items[copied]);
    }
    *result = copied;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE First(WgiIIteratorGamepad** result) override {
    if (result == nullptr)
      return E_POINTER;
    AddRef();
    *result = static_cast<WgiIIteratorGamepad*>(this);
    return S_OK;
  }

 private:
  volatile LONG refs_;      ///< Object reference count.
  std::uint32_t pos_;       ///< Next element index (0 = the one fake pad).
};

/**
 * @brief The fake IVectorView over the one fake pad.
 *
 * Also answers IIterable<Gamepad> because C++/WinRT's range-for enters the
 * view through QI for IIterable before calling First().
 */
class WgiFakeVectorView final : public WgiIVectorViewGamepad, public WgiIIterableGamepad {
 public:
  WgiFakeVectorView() : refs_(1) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;
    void* obj = nullptr;
    if (IsEqualGUID(riid, kWgiIidUnknown) || IsEqualGUID(riid, kWgiIidInspectable) ||
        IsEqualGUID(riid, kWgiIidVectorViewGamepad)) {
      obj = static_cast<WgiIVectorViewGamepad*>(this);
    } else if (IsEqualGUID(riid, kWgiIidIterableGamepad)) {
      obj = static_cast<WgiIIterableGamepad*>(this);
    } else {
      *ppvObject = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    *ppvObject = obj;
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return (ULONG)InterlockedIncrement(&refs_);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG r = InterlockedDecrement(&refs_);
    if (r == 0)
      delete this;
    return (ULONG)(r > 0 ? r : 0);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount, IID** iids) override {
    static const GUID kIids[] = {kWgiIidVectorViewGamepad, kWgiIidIterableGamepad};
    return WgiServeIids(kIids, 2, iidCount, iids);
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(WgiHstring* className) override {
    return WgiServeClassName(
        L"Windows.Foundation.Collections.IVectorView`1<Windows.Gaming.Input.Gamepad>",
        className);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(int* trustLevel) override {
    if (trustLevel == nullptr)
      return E_POINTER;
    *trustLevel = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetAt(std::uint32_t index, WgiIGamepad** result) override {
    if (result == nullptr)
      return E_POINTER;
    if (index != 0)
      return E_BOUNDS;
    WgiAcquireFakePad(result);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Size(std::uint32_t* result) override {
    if (result == nullptr)
      return E_POINTER;
    *result = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE IndexOf(WgiIGamepad* value, std::uint32_t* index,
                                   unsigned char* result) override {
    if (index == nullptr || result == nullptr)
      return E_POINTER;
    *index = 0;
    *result = (value == WgiFakePadInterface()) ? 1 : 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMany(std::uint32_t startIndex, std::uint32_t itemsLength,
                                    WgiIGamepad** items, std::uint32_t* result) override {
    if (result == nullptr || (itemsLength > 0 && items == nullptr))
      return E_POINTER;
    std::uint32_t copied = 0;
    if (startIndex == 0 && itemsLength > 0 && items != nullptr) {
      WgiAcquireFakePad(&items[0]);
      copied = 1;
    }
    *result = copied;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE First(WgiIIteratorGamepad** result) override {
    if (result == nullptr)
      return E_POINTER;
    *result = new WgiFakeVectorIterator();
    return S_OK;
  }

 private:
  volatile LONG refs_; ///< Object reference count.
};

/* ------------------------------------------------------------------ */
/* RawGameController fake (UE5 discovery + reading path)               */
/* ------------------------------------------------------------------ */

/**
 * @brief A generic empty IVectorView<T> for out-params that must be a valid
 *        collection but hold no elements (ForceFeedbackMotors,
 *        SimpleHapticsControllers). Per-call object (ordinary refcount).
 */
class WgiFakeEmptyVectorView final : public WgiIVectorViewEmpty {
 public:
  WgiFakeEmptyVectorView() : refs_(1) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;
    if (IsEqualGUID(riid, kWgiIidUnknown) || IsEqualGUID(riid, kWgiIidInspectable)) {
      AddRef();
      *ppvObject = static_cast<WgiIVectorViewEmpty*>(this);
      return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return (ULONG)(r > 0 ? r : 0);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount, IID** iids) override {
    return WgiServeIids(nullptr, 0, iidCount, iids);
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(WgiHstring* className) override {
    return WgiServeClassName(L"Windows.Foundation.Collections.IVectorView`1", className);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(int* trustLevel) override {
    if (trustLevel == nullptr) return E_POINTER;
    *trustLevel = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetAt(std::uint32_t /*index*/, void** result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return E_BOUNDS;
  }
  HRESULT STDMETHODCALLTYPE get_Size(std::uint32_t* result) override {
    if (result == nullptr) return E_POINTER;
    *result = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE IndexOf(void* /*value*/, std::uint32_t* index,
                                    unsigned char* result) override {
    if (index == nullptr || result == nullptr) return E_POINTER;
    *index = 0;
    *result = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMany(std::uint32_t /*startIndex*/, std::uint32_t /*itemsLength*/,
                                    void** /*items*/, std::uint32_t* result) override {
    if (result == nullptr) return E_POINTER;
    *result = 0;
    return S_OK;
  }

 private:
  volatile LONG refs_;
};

/** Xbox 360 button index -> GameControllerButtonLabel (SDK enum order). */
static const std::int32_t kWgiRawButtonLabels[14] = {
    1,  /* A */
    2,  /* B */
    3,  /* X */
    4,  /* Y */
    9,  /* LeftShoulder */
    10, /* RightShoulder */
    16, /* View (Back) */
    15, /* Menu (Start) */
    13, /* LeftThumbstick */
    14, /* RightThumbstick */
    5,  /* DPadUp */
    6,  /* DPadDown */
    7,  /* DPadLeft */
    8   /* DPadRight */
};

/**
 * @brief The fake connected RawGameController served through Windows.Gaming.Input.
 *
 * Presents a wired Xbox 360 pad (VID 045E / PID 028E): 14 buttons, 6 axes,
 * 0 switches. GetCurrentReading is fed from the same WgiSourceState (UDP
 * snapshot) as the Gamepad fake, so both surfaces stay in sync. Also
 * implements IGameController / IGameControllerBatteryInfo for the
 * headset/user/battery plumbing UE5 probes.
 */
class WgiFakeRawGamepad final : public WgiIRawGameController,
                               public WgiIRawGameController2,
                               public WgiIGameController,
                               public WgiIGameControllerBatteryInfo {
 public:
  WgiFakeRawGamepad() : refs_(1) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
    if (ppvObject == nullptr) return E_POINTER;
    void* obj = nullptr;
    if (IsEqualGUID(riid, kWgiIidUnknown) || IsEqualGUID(riid, kWgiIidInspectable) ||
        IsEqualGUID(riid, kWgiIidIRawGameController)) {
      obj = static_cast<WgiIRawGameController*>(this);
    } else if (IsEqualGUID(riid, kWgiIidIRawGameController2)) {
      obj = static_cast<WgiIRawGameController2*>(this);
    } else if (IsEqualGUID(riid, kWgiIidIGameController)) {
      obj = static_cast<WgiIGameController*>(this);
    } else if (IsEqualGUID(riid, kWgiIidIGameControllerBatteryInfo)) {
      obj = static_cast<WgiIGameControllerBatteryInfo*>(this);
    } else {
      *ppvObject = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    *ppvObject = obj;
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    if (refs_ > 0) InterlockedDecrement(&refs_);
    return (ULONG)refs_;
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount, IID** iids) override {
    static const GUID kIids[] = {kWgiIidIRawGameController, kWgiIidIRawGameController2,
                                 kWgiIidIGameController, kWgiIidIGameControllerBatteryInfo};
    return WgiServeIids(kIids, 4, iidCount, iids);
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(WgiHstring* className) override {
    return WgiServeClassName(L"Windows.Gaming.Input.RawGameController", className);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(int* trustLevel) override {
    if (trustLevel == nullptr) return E_POINTER;
    *trustLevel = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_AxisCount(std::int32_t* value) override {
    if (value == nullptr) return E_POINTER;
    *value = 6;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_ButtonCount(std::int32_t* value) override {
    if (value == nullptr) return E_POINTER;
    *value = 14;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_ForceFeedbackMotors(void** value) override {
    if (value == nullptr) return E_POINTER;
    *value = new WgiFakeEmptyVectorView();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_HardwareProductId(std::uint16_t* value) override {
    if (value == nullptr) return E_POINTER;
    *value = 0x028E;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_HardwareVendorId(std::uint16_t* value) override {
    if (value == nullptr) return E_POINTER;
    *value = 0x045E;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_SwitchCount(std::int32_t* value) override {
    if (value == nullptr) return E_POINTER;
    *value = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetButtonLabel(std::int32_t buttonIndex,
                                           std::int32_t* value) override {
    if (value == nullptr) return E_POINTER;
    if (buttonIndex < 0 || buttonIndex >= 14) {
      *value = 0;
      return S_OK;
    }
    *value = kWgiRawButtonLabels[buttonIndex];
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCurrentReading(std::uint32_t buttonArrayLength,
                                              unsigned char* buttonArray,
                                              std::uint32_t /*switchArrayLength*/,
                                              std::int32_t* /*switchArray*/,
                                              std::uint32_t axisArrayLength,
                                              double* axisArray,
                                              std::uint64_t* timestamp) override {
    InterlockedIncrement64(&g_wgiRawReadCalls);
    bool fresh = false;
    const XINPUT_GAMEPAD g = WgiSourceState(&fresh);
    if (fresh) InterlockedIncrement64(&g_wgiRawServedFresh);
    if (buttonArray != nullptr) {
      const std::uint32_t n = buttonArrayLength < 14 ? buttonArrayLength : 14;
      for (std::uint32_t i = 0; i < n; ++i) buttonArray[i] = 0;
      if (n > 0) {
        buttonArray[0] = (g.wButtons & kWgiXiA) ? 1 : 0;
        buttonArray[1] = (g.wButtons & kWgiXiB) ? 1 : 0;
        buttonArray[2] = (g.wButtons & kWgiXiX) ? 1 : 0;
        buttonArray[3] = (g.wButtons & kWgiXiY) ? 1 : 0;
        buttonArray[4] = (g.wButtons & kWgiXiLeftShoulder) ? 1 : 0;
        buttonArray[5] = (g.wButtons & kWgiXiRightShoulder) ? 1 : 0;
        buttonArray[6] = (g.wButtons & kWgiXiBack) ? 1 : 0;
        buttonArray[7] = (g.wButtons & kWgiXiStart) ? 1 : 0;
        buttonArray[8] = (g.wButtons & kWgiXiLeftThumb) ? 1 : 0;
        buttonArray[9] = (g.wButtons & kWgiXiRightThumb) ? 1 : 0;
        buttonArray[10] = (g.wButtons & kWgiXiDpadUp) ? 1 : 0;
        buttonArray[11] = (g.wButtons & kWgiXiDpadDown) ? 1 : 0;
        buttonArray[12] = (g.wButtons & kWgiXiDpadLeft) ? 1 : 0;
        buttonArray[13] = (g.wButtons & kWgiXiDpadRight) ? 1 : 0;
      }
    }
    if (axisArray != nullptr) {
      const std::uint32_t n = axisArrayLength < 6 ? axisArrayLength : 6;
      for (std::uint32_t i = 0; i < n; ++i) axisArray[i] = 0.0;
      if (n > 0) axisArray[0] = g.sThumbLX / 32767.0;
      if (n > 1) axisArray[1] = g.sThumbLY / 32767.0;
      if (n > 2) axisArray[2] = g.sThumbRX / 32767.0;
      if (n > 3) axisArray[3] = g.sThumbRY / 32767.0;
      if (n > 4) axisArray[4] = g.bLeftTrigger / 255.0;
      if (n > 5) axisArray[5] = g.bRightTrigger / 255.0;
    }
    if (timestamp != nullptr) *timestamp = WgiNowUs();
    if (InterlockedCompareExchange64(&g_wgiRawReadLogged, 1, 0) == 0)
      Dlog("wgi: RawGameController GetCurrentReading served (fresh=%d btn=%04x)",
           fresh ? 1 : 0, (unsigned)g.wButtons);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSwitchKind(std::int32_t /*switchIndex*/,
                                          std::int32_t* value) override {
    if (value == nullptr) return E_POINTER;
    *value = 0;
    return E_BOUNDS;
  }
  HRESULT STDMETHODCALLTYPE get_SimpleHapticsControllers(void** value) override {
    if (value == nullptr) return E_POINTER;
    *value = new WgiFakeEmptyVectorView();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_NonRoamableId(WgiHstring* value) override {
    return WgiServeClassName(L"0", value);
  }
  HRESULT STDMETHODCALLTYPE get_DisplayName(WgiHstring* value) override {
    return WgiServeClassName(L"Controller", value);
  }
  HRESULT STDMETHODCALLTYPE add_HeadsetConnected(void* /*value*/, WgiEventToken* token) override {
    if (token == nullptr) return E_POINTER;
    token->value = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE remove_HeadsetConnected(WgiEventToken /*token*/) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE add_HeadsetDisconnected(void* /*value*/, WgiEventToken* token) override {
    if (token == nullptr) return E_POINTER;
    token->value = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE remove_HeadsetDisconnected(WgiEventToken /*token*/) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE add_UserChanged(void* /*value*/, WgiEventToken* token) override {
    if (token == nullptr) return E_POINTER;
    token->value = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE remove_UserChanged(WgiEventToken /*token*/) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE get_Headset(void** value) override {
    if (value == nullptr) return E_POINTER;
    *value = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsWireless(unsigned char* value) override {
    if (value == nullptr) return E_POINTER;
    *value = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_User(void** value) override {
    if (value == nullptr) return E_POINTER;
    *value = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE TryGetBatteryReport(void** value) override {
    if (value == nullptr) return E_POINTER;
    *value = nullptr;
    return S_OK;
  }

 private:
  volatile LONG refs_;
};

/** The single fake RawGameController object (immortal; created by WgiFakeInstall). */
static WgiFakeRawGamepad* g_wgiFakeRawPadObj = nullptr;

/**
 * @brief Hand the fake raw controller out through an out parameter (AddRef + assign).
 * @param[out] out Receives the controller's IRawGameController pointer.
 */
static void WgiAcquireFakeRawPad(WgiIRawGameController** out) {
  if (out == nullptr || g_wgiFakeRawPadObj == nullptr) return;
  g_wgiFakeRawPadObj->AddRef();
  *out = g_wgiFakeRawPadI;
}

/**
 * @brief The fake raw controller's IRawGameController interface pointer.
 * @return The subobject pointer handed to the game, or nullptr before install.
 */
static WgiIRawGameController* WgiFakeRawPadInterface(void) { return g_wgiFakeRawPadI; }

/**
 * @brief Whether an IGameController interface pointer is the fake raw controller.
 * @param controller The controller to test (any of its WinRT interfaces).
 * @return True when the object is the fake raw controller.
 */
static bool WgiIsFakeRawController(void* controller) {
  if (controller == nullptr || g_wgiFakeRawPadObj == nullptr) return false;
  WgiIRawGameController* probe = nullptr;
  WgiIInspectable* insp = (WgiIInspectable*)controller;
  if (FAILED(insp->QueryInterface(kWgiIidIRawGameController, (void**)&probe)) || probe == nullptr)
    return false;
  const bool ours = (probe == g_wgiFakeRawPadI);
  probe->Release();
  return ours;
}

/**
 * @brief AddRef the captured OS RawGameController statics for a pass-through call.
 * @return The OS statics (caller releases), or nullptr when none was captured.
 */
static WgiIRawGameControllerStatics* WgiRawRealStaticsAcquire(void) {
  if (g_wgiLockInit != 1) return nullptr;
  EnterCriticalSection(&g_wgiLock);
  WgiIRawGameControllerStatics* s = g_wgiRealRawStatics;
  if (s != nullptr) s->AddRef();
  LeaveCriticalSection(&g_wgiLock);
  return s;
}

/**
 * @brief AddRef the captured OS GamepadStatics2 for a FromGameController pass-through.
 *
 * The real IGamepadStatics2 (WinRT extension of IGamepadStatics) owns the
 * FromGameController mapping. When a real controller is detected in the
 * pass-through path we delegate to it so the game sees the real device.
 * @return The OS statics2 (caller releases), or nullptr when none was captured.
 */
static WgiIGamepadStatics2* WgiRealStatics2Acquire(void) {
  if (g_wgiLockInit != 1) return nullptr;
  EnterCriticalSection(&g_wgiLock);
  WgiIGamepadStatics2* s = g_wgiRealStatics2;
  if (s != nullptr) s->AddRef();
  LeaveCriticalSection(&g_wgiLock);
  return s;
}

/**
 * @brief Whether an IID is one of the parameterized collection IIDs gleaned
 *        from the real OS RawGameControllers view (IVectorView/IIterable).
 *
 * The fake view answers IIterable through these IIDs so C++/WinRT's range-for
 * (which QIs the view to IIterable before calling First()) succeeds.
 * @param riid The requested IID.
 * @return True when the IID is a known RawGameController collection IID.
 */
static bool WgiIsRawCollectionIid(REFIID riid) {
  if (g_wgiRawViewIidCount <= 0) return false;
  for (int i = 0; i < g_wgiRawViewIidCount; ++i)
    if (IsEqualGUID(riid, g_wgiRawViewIids[i])) return true;
  return false;
}

/**
 * @brief The fake IIterator over the one-element raw controller list.
 */
class WgiFakeRawVectorIterator final : public WgiIIteratorRawGameController,
                                      public WgiIIterableRawGameController {
 public:
  WgiFakeRawVectorIterator() : refs_(1), pos_(0) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
    if (ppvObject == nullptr) return E_POINTER;
    void* obj = nullptr;
    if (IsEqualGUID(riid, kWgiIidUnknown) || IsEqualGUID(riid, kWgiIidInspectable)) {
      obj = static_cast<WgiIIteratorRawGameController*>(this);
    } else if (WgiIsRawCollectionIid(riid)) {
      obj = static_cast<WgiIIterableRawGameController*>(this);
    } else {
      *ppvObject = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    *ppvObject = obj;
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return (ULONG)(r > 0 ? r : 0);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount, IID** iids) override {
    return WgiServeIids(nullptr, 0, iidCount, iids);
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(WgiHstring* className) override {
    return WgiServeClassName(
        L"Windows.Foundation.Collections.IIterator`1<Windows.Gaming.Input.RawGameController>",
        className);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(int* trustLevel) override {
    if (trustLevel == nullptr) return E_POINTER;
    *trustLevel = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Current(WgiIRawGameController** result) override {
    if (result == nullptr) return E_POINTER;
    if (pos_ >= 1) return E_BOUNDS;
    WgiAcquireFakeRawPad(result);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_HasCurrent(unsigned char* result) override {
    if (result == nullptr) return E_POINTER;
    *result = (pos_ < 1) ? 1 : 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE MoveNext(unsigned char* result) override {
    if (result == nullptr) return E_POINTER;
    pos_ += (pos_ < 1) ? 1 : 0;
    *result = (pos_ < 1) ? 1 : 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMany(std::uint32_t itemsLength, WgiIRawGameController** items,
                                    std::uint32_t* result) override {
    if (result == nullptr || (itemsLength > 0 && items == nullptr)) return E_POINTER;
    std::uint32_t copied = 0;
    if (items != nullptr) {
      for (; copied < itemsLength && pos_ < 1; ++copied, ++pos_)
        WgiAcquireFakeRawPad(&items[copied]);
    }
    *result = copied;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE First(WgiIIteratorRawGameController** result) override {
    if (result == nullptr) return E_POINTER;
    AddRef();
    *result = static_cast<WgiIIteratorRawGameController*>(this);
    return S_OK;
  }

 private:
  volatile LONG refs_;
  std::uint32_t pos_;
};

/**
 * @brief The fake IVectorView over the one fake raw controller.
 *
 * Also answers IIterable<RawGameController> (through the IIDs gleaned from the
 * real OS view) so C++/WinRT's range-for can QI to IIterable and call First().
 */
class WgiFakeRawVectorView final : public WgiIVectorViewRawGameController,
                                  public WgiIIterableRawGameController {
 public:
  WgiFakeRawVectorView() : refs_(1) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
    if (ppvObject == nullptr) return E_POINTER;
    void* obj = nullptr;
    if (IsEqualGUID(riid, kWgiIidUnknown) || IsEqualGUID(riid, kWgiIidInspectable)) {
      obj = static_cast<WgiIVectorViewRawGameController*>(this);
    } else if (WgiIsRawCollectionIid(riid)) {
      obj = static_cast<WgiIIterableRawGameController*>(this);
    } else {
      *ppvObject = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    *ppvObject = obj;
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return (ULONG)(r > 0 ? r : 0);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount, IID** iids) override {
    return WgiServeIids(nullptr, 0, iidCount, iids);
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(WgiHstring* className) override {
    return WgiServeClassName(
        L"Windows.Foundation.Collections.IVectorView`1<Windows.Gaming.Input.RawGameController>",
        className);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(int* trustLevel) override {
    if (trustLevel == nullptr) return E_POINTER;
    *trustLevel = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetAt(std::uint32_t index, WgiIRawGameController** result) override {
    if (result == nullptr) return E_POINTER;
    if (index != 0) return E_BOUNDS;
    WgiAcquireFakeRawPad(result);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Size(std::uint32_t* result) override {
    if (result == nullptr) return E_POINTER;
    *result = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE IndexOf(WgiIRawGameController* value, std::uint32_t* index,
                                    unsigned char* result) override {
    if (index == nullptr || result == nullptr) return E_POINTER;
    *index = 0;
    *result = (value == WgiFakeRawPadInterface()) ? 1 : 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMany(std::uint32_t startIndex, std::uint32_t itemsLength,
                                    WgiIRawGameController** items,
                                    std::uint32_t* result) override {
    if (result == nullptr || (itemsLength > 0 && items == nullptr)) return E_POINTER;
    std::uint32_t copied = 0;
    if (startIndex == 0 && itemsLength > 0 && items != nullptr) {
      WgiAcquireFakeRawPad(&items[0]);
      copied = 1;
    }
    *result = copied;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE First(WgiIIteratorRawGameController** result) override {
    if (result == nullptr) return E_POINTER;
    *result = new WgiFakeRawVectorIterator();
    return S_OK;
  }

 private:
  volatile LONG refs_;
};

/**
 * @brief The fake RawGameController activation factory / statics.
 *
 * Serves IActivationFactory and IRawGameControllerStatics. The Added handler is
 * fired once with the fake pad on subscription (UE5 discovers controllers
 * through this event). get_RawGameControllers passes the real system view
 * through when the OS has a controller, otherwise presents the fake pad.
 */
class WgiFakeRawStatics final : public WgiIActivationFactory,
                               public WgiIRawGameControllerStatics {
 public:
  WgiFakeRawStatics() : refs_(1) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
    if (ppvObject == nullptr) return E_POINTER;
    void* obj = nullptr;
    if (IsEqualGUID(riid, kWgiIidUnknown) || IsEqualGUID(riid, kWgiIidInspectable) ||
        IsEqualGUID(riid, kWgiIidActivationFactory)) {
      obj = static_cast<WgiIActivationFactory*>(this);
    } else if (IsEqualGUID(riid, kWgiIidIRawGameControllerStatics)) {
      obj = static_cast<WgiIRawGameControllerStatics*>(this);
    } else {
      *ppvObject = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    *ppvObject = obj;
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    if (refs_ > 0) InterlockedDecrement(&refs_);
    return (ULONG)refs_;
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount, IID** iids) override {
    static const GUID kIids[] = {kWgiIidActivationFactory, kWgiIidIRawGameControllerStatics};
    return WgiServeIids(kIids, 2, iidCount, iids);
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(WgiHstring* className) override {
    return WgiServeClassName(L"Windows.Gaming.Input.RawGameController", className);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(int* trustLevel) override {
    if (trustLevel == nullptr) return E_POINTER;
    *trustLevel = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ActivateInstance(WgiIInspectable** instance) override {
    if (instance == nullptr) return E_POINTER;
    *instance = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE add_RawGameControllerAdded(
      WgiIEventHandlerRawGameController* value, WgiEventToken* token) override {
    if (value == nullptr || token == nullptr) return E_POINTER;
    InterlockedIncrement64(&g_wgiRawAddedSubs);
    int slot = -1;
    if (g_wgiLockInit == 1) {
      EnterCriticalSection(&g_wgiLock);
      for (int i = 0; i < WGI_MAX_RAW_HANDLERS; ++i) {
        if (g_wgiRawAddedHandlers[i] == nullptr) {
          slot = i;
          break;
        }
      }
      if (slot >= 0) {
        value->AddRef();
        g_wgiRawAddedHandlers[slot] = value;
      }
      LeaveCriticalSection(&g_wgiLock);
    }
    token->value = slot + 1;
    if (InterlockedCompareExchange64(&g_wgiRawAddedLogged, 1, 0) == 0)
      Dlog("wgi: RawGameControllerAdded subscribed (slot=%d)", slot);
    /* Fire the Added event once with the fake pad: UE5 creates its input device
       in this callback. Real WGI never raises Added for already-connected pads,
       but the fake pad is materialized on demand, so the subscription is the
       only discovery signal. Done outside the lock to avoid re-entry deadlock. */
    if (slot >= 0 && g_wgiFakeRawPadI != nullptr &&
        InterlockedCompareExchange(&g_wgiRawAddedFired, 1, 0) == 0) {
      g_wgiFakeRawPadObj->AddRef();
      value->Invoke(nullptr, g_wgiFakeRawPadI);
      Dlog("wgi: RawGameControllerAdded fired with the fake pad");
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE remove_RawGameControllerAdded(WgiEventToken token) override {
    if (g_wgiLockInit != 1 || token.value <= 0 || token.value > WGI_MAX_RAW_HANDLERS)
      return S_OK;
    EnterCriticalSection(&g_wgiLock);
    WgiIEventHandlerRawGameController* h = g_wgiRawAddedHandlers[token.value - 1];
    g_wgiRawAddedHandlers[token.value - 1] = nullptr;
    LeaveCriticalSection(&g_wgiLock);
    if (h != nullptr) h->Release();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE add_RawGameControllerRemoved(
      WgiIEventHandlerRawGameController* value, WgiEventToken* token) override {
    if (value == nullptr || token == nullptr) return E_POINTER;
    int slot = -1;
    if (g_wgiLockInit == 1) {
      EnterCriticalSection(&g_wgiLock);
      for (int i = 0; i < WGI_MAX_RAW_HANDLERS; ++i) {
        if (g_wgiRawRemovedHandlers[i] == nullptr) {
          slot = i;
          break;
        }
      }
      if (slot >= 0) {
        value->AddRef();
        g_wgiRawRemovedHandlers[slot] = value;
      }
      LeaveCriticalSection(&g_wgiLock);
    }
    token->value = slot + 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE remove_RawGameControllerRemoved(WgiEventToken token) override {
    if (g_wgiLockInit != 1 || token.value <= 0 || token.value > WGI_MAX_RAW_HANDLERS)
      return S_OK;
    EnterCriticalSection(&g_wgiLock);
    WgiIEventHandlerRawGameController* h = g_wgiRawRemovedHandlers[token.value - 1];
    g_wgiRawRemovedHandlers[token.value - 1] = nullptr;
    LeaveCriticalSection(&g_wgiLock);
    if (h != nullptr) h->Release();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_RawGameControllers(
      WgiIVectorViewRawGameController** value) override {
    if (value == nullptr) return E_POINTER;
    InterlockedIncrement64(&g_wgiRawListCalls);
    /* Real controllers always win: pass the system view through untouched. */
    WgiIRawGameControllerStatics* real = WgiRawRealStaticsAcquire();
    if (real != nullptr) {
      WgiIVectorViewRawGameController* view = nullptr;
      const HRESULT hr = real->get_RawGameControllers(&view);
      real->Release();
      if (SUCCEEDED(hr) && view != nullptr) {
        std::uint32_t size = 0;
        const bool sized = SUCCEEDED(view->get_Size(&size));
        if (sized && size > 0) {
          if (InterlockedCompareExchange64(&g_wgiRawListLogged, 1, 0) == 0)
            Dlog("wgi: get_RawGameControllers passed the REAL system view (size=%lu)",
                 (unsigned long)size);
          *value = view;
          return S_OK;
        }
        view->Release();
      }
    }
    if (InterlockedCompareExchange64(&g_wgiRawListLogged, 1, 0) == 0)
      Dlog("wgi: get_RawGameControllers serving the fake raw controller");
    *value = new WgiFakeRawVectorView();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE FromGameController(void* gameController,
                                               WgiIRawGameController** value) override {
    if (value == nullptr) return E_POINTER;
    *value = nullptr;
    if (gameController == nullptr) return E_POINTER;
    if (WgiIsFakeRawController(gameController)) {
      WgiAcquireFakeRawPad(value);
      return S_OK;
    }
    WgiIRawGameControllerStatics* real = WgiRawRealStaticsAcquire();
    if (real != nullptr) {
      const HRESULT hr = real->FromGameController(gameController, value);
      real->Release();
      return hr;
    }
    return E_INVALIDARG;
  }

 private:
  volatile LONG refs_;
};

/** The single fake RawGameController statics object (immortal). */
static WgiFakeRawStatics* g_wgiFakeRawStaticsObj = nullptr;

/**
 * @brief The fake Gamepad activation factory / statics.
 *
 * Serves IActivationFactory, IGamepadStatics and IGamepadStatics2 on one
 * object. get_Gamepads passes the real system view through whenever the OS
 * has a controller, and otherwise presents the fake pad; FromGameController
 * resolves the fake pad by identity and forwards real controllers to the OS
 * statics. GamepadAdded / GamepadRemoved subscriptions are stored but never
 * fired: real WGI does not raise Added for pads that are already
 * enumerated, and the fake pad is enumerable from the first call.
 */
class WgiFakeStatics final : public WgiIActivationFactory,
                             public WgiIGamepadStatics,
                             public WgiIGamepadStatics2 {
 public:
  WgiFakeStatics() : refs_(1) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;
    void* obj = nullptr;
    if (IsEqualGUID(riid, kWgiIidUnknown) || IsEqualGUID(riid, kWgiIidInspectable) ||
        IsEqualGUID(riid, kWgiIidActivationFactory)) {
      obj = static_cast<WgiIActivationFactory*>(this);
    } else if (IsEqualGUID(riid, kWgiIidIGamepadStatics)) {
      obj = static_cast<WgiIGamepadStatics*>(this);
    } else if (IsEqualGUID(riid, kWgiIidIGamepadStatics2)) {
      obj = static_cast<WgiIGamepadStatics2*>(this);
    } else {
      *ppvObject = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    *ppvObject = obj;
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return (ULONG)InterlockedIncrement(&refs_);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    /* Immortal singleton: games may hold the factory for the whole session. */
    if (refs_ > 0)
      InterlockedDecrement(&refs_);
    return (ULONG)refs_;
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount, IID** iids) override {
    static const GUID kIids[] = {kWgiIidActivationFactory, kWgiIidIGamepadStatics,
                                 kWgiIidIGamepadStatics2};
    return WgiServeIids(kIids, 3, iidCount, iids);
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(WgiHstring* className) override {
    return WgiServeClassName(L"Windows.Gaming.Input.Gamepad", className);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(int* trustLevel) override {
    if (trustLevel == nullptr)
      return E_POINTER;
    *trustLevel = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ActivateInstance(WgiIInspectable** instance) override {
    if (instance == nullptr)
      return E_POINTER;
    *instance = nullptr;
    Dlog("wgi: ActivateInstance on the statics-only Gamepad class (E_NOTIMPL)");
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE add_GamepadAdded(WgiIEventHandlerGamepad* value,
                                            WgiEventToken* token) override {
    if (value == nullptr || token == nullptr)
      return E_POINTER;
    InterlockedIncrement64(&g_wgiAddedSubs);
    int slot = -1;
    if (g_wgiLockInit == 1) {
      EnterCriticalSection(&g_wgiLock);
      for (int i = 0; i < WGI_MAX_HANDLERS; ++i) {
        if (g_wgiAddedHandlers[i] == nullptr) {
          slot = i;
          break;
        }
      }
      if (slot >= 0) {
        value->AddRef();
        g_wgiAddedHandlers[slot] = value;
      }
      LeaveCriticalSection(&g_wgiLock);
    }
    token->value = slot + 1; /* 0 = handler table full */
    if (InterlockedCompareExchange64(&g_wgiAddedLogged, 1, 0) == 0)
      Dlog("wgi: GamepadAdded subscribed (slot=%d; the fake pad is enumerable "
           "from the first get_Gamepads, so no Added event is raised)",
           slot);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE remove_GamepadAdded(WgiEventToken token) override {
    if (g_wgiLockInit != 1 || token.value <= 0 || token.value > WGI_MAX_HANDLERS)
      return S_OK;
    EnterCriticalSection(&g_wgiLock);
    WgiIEventHandlerGamepad* h = g_wgiAddedHandlers[token.value - 1];
    g_wgiAddedHandlers[token.value - 1] = nullptr;
    LeaveCriticalSection(&g_wgiLock);
    if (h != nullptr)
      h->Release();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE add_GamepadRemoved(WgiIEventHandlerGamepad* value,
                                               WgiEventToken* token) override {
    if (value == nullptr || token == nullptr)
      return E_POINTER;
    int slot = -1;
    if (g_wgiLockInit == 1) {
      EnterCriticalSection(&g_wgiLock);
      for (int i = 0; i < WGI_MAX_HANDLERS; ++i) {
        if (g_wgiRemovedHandlers[i] == nullptr) {
          slot = i;
          break;
        }
      }
      if (slot >= 0) {
        value->AddRef();
        g_wgiRemovedHandlers[slot] = value;
      }
      LeaveCriticalSection(&g_wgiLock);
    }
    token->value = slot + 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE remove_GamepadRemoved(WgiEventToken token) override {
    if (g_wgiLockInit != 1 || token.value <= 0 || token.value > WGI_MAX_HANDLERS)
      return S_OK;
    EnterCriticalSection(&g_wgiLock);
    WgiIEventHandlerGamepad* h = g_wgiRemovedHandlers[token.value - 1];
    g_wgiRemovedHandlers[token.value - 1] = nullptr;
    LeaveCriticalSection(&g_wgiLock);
    if (h != nullptr)
      h->Release();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Gamepads(WgiIVectorViewGamepad** value) override {
    if (value == nullptr)
      return E_POINTER;
    InterlockedIncrement64(&g_wgiListCalls);
    // Always serve the fake pad so the streaming client controls the game even
    // when a physical controller is attached. The real system view is skipped.
    if (InterlockedCompareExchange64(&g_wgiListLogged, 1, 0) == 0)
      Dlog("wgi: get_Gamepads serving the fake pad (one connected, neutral)");
    *value = new WgiFakeVectorView();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE FromGameController(void* controller,
                                               WgiIGamepad** value) override {
    if (value == nullptr)
      return E_POINTER;
    *value = nullptr;
    if (controller == nullptr)
      return E_POINTER;
    if (WgiIsFakePadController(controller)) {
      WgiAcquireFakePad(value);
      return S_OK;
    }
    /* UE5 calls Gamepad::FromGameController(rawController) to map the raw
       controller it discovered through RawGameControllerAdded onto the
       standard Gamepad surface; the fake raw controller resolves to the
       fake Gamepad so both surfaces share one identity. */
    if (WgiIsFakeRawController(controller)) {
      WgiAcquireFakePad(value);
      return S_OK;
    }
    /* The fake pad is the only device the streaming client ever drives.
       Never fall through to the real OS GamepadStatics2::FromGameController,
       otherwise a physically-attached controller would resolve to the real
       Gamepad surface and bypass the fake. */
    return E_INVALIDARG;
  }

 private:
  volatile LONG refs_; ///< Object reference count (floor of one: immortal).
};

/** The single fake statics object (immortal; created by WgiFakeInstall). */
static WgiFakeStatics* g_wgiFakeStaticsObj = nullptr;

/**
 * @brief Log the first activation of each Windows.Gaming.Input class.
 *
 * Budgeted to one log line per distinct class name, so a live session shows
 * which other WinRT input surfaces (RawGameController and friends) the game
 * touches without spamming the log.
 * @param name The activatable class name.
 */
static void WgiObserveClass(const wchar_t* name) {
  bool fresh = false;
  if (g_wgiLockInit == 1) {
    EnterCriticalSection(&g_wgiLock);
    bool seen = false;
    for (int i = 0; i < g_wgiSeenCount; ++i) {
      if (wcscmp(g_wgiSeenNames[i], name) == 0) {
        seen = true;
        break;
      }
    }
    if (!seen && g_wgiSeenCount < 16) {
      wcsncpy(g_wgiSeenNames[g_wgiSeenCount], name, 47);
      g_wgiSeenNames[g_wgiSeenCount][47] = 0;
      ++g_wgiSeenCount;
      fresh = true;
    }
    LeaveCriticalSection(&g_wgiLock);
  }
  if (fresh)
    Dlog("wgi: observed activation of %ls (passthrough)", name);
}

/**
 * @brief Hooked combase!RoGetActivationFactory.
 *
 * Intercepts the activation of Windows.Gaming.Input.Gamepad and serves the
 * fake statics for the factory interfaces it implements (IGamepadStatics,
 * IGamepadStatics2, IActivationFactory, IInspectable, IUnknown); every other
 * activation - other Gamepad interfaces and every other class - passes
 * through the real resolver.
 * @param activatableClassId The activatable class name.
 * @param iid The requested factory interface.
 * @param[out] factory Receives the factory.
 * @return S_OK when the fake statics was served, otherwise the real
 * resolver's HRESULT.
 */
static HRESULT WINAPI MyRoGetActivationFactory(WgiHstring activatableClassId,
                                               REFIID iid, void** factory) {
  InterlockedIncrement64(&g_wgiRoGetCalls);
  FnWgiRoGetActivationFactory real = g_realRoGetActivationFactory;
  if (real == nullptr)
    return REGDB_E_CLASSNOTREG;
  const wchar_t* name = nullptr;
  if (activatableClassId != nullptr && g_wgiGetStringRawBuffer != nullptr) {
    UINT32 len = 0;
    name = g_wgiGetStringRawBuffer(activatableClassId, &len);
  }
  if (name != nullptr) {
    static volatile LONG s_rogetLogged = 0;
    if (InterlockedIncrement(&s_rogetLogged) <= 10)
      Dlog("wgi: RoGetActivationFactory class=%ls", name);
  }
  if (name != nullptr && wcscmp(name, L"Windows.Gaming.Input.Gamepad") == 0) {
    InterlockedIncrement64(&g_wgiGamepadActivations);
    if (InterlockedCompareExchange64(&g_wgiActLogged, 1, 0) == 0)
      Dlog("wgi: Gamepad activation intercepted");
    if (IsEqualGUID(iid, kWgiIidIGamepadStatics) ||
        IsEqualGUID(iid, kWgiIidIGamepadStatics2) ||
        IsEqualGUID(iid, kWgiIidActivationFactory) ||
        IsEqualGUID(iid, kWgiIidInspectable) || IsEqualGUID(iid, kWgiIidUnknown)) {
      if (g_wgiFakeStaticsObj != nullptr)
        return g_wgiFakeStaticsObj->QueryInterface(iid, factory);
    }
  } else if (name != nullptr && wcscmp(name, L"Windows.Gaming.Input.RawGameController") == 0) {
    if (InterlockedCompareExchange64(&g_wgiRawActLogged, 1, 0) == 0)
      Dlog("wgi: RawGameController activation intercepted");
    if (IsEqualGUID(iid, kWgiIidIRawGameControllerStatics) ||
        IsEqualGUID(iid, kWgiIidActivationFactory) ||
        IsEqualGUID(iid, kWgiIidInspectable) || IsEqualGUID(iid, kWgiIidUnknown)) {
      if (g_wgiFakeRawStaticsObj != nullptr)
        return g_wgiFakeRawStaticsObj->QueryInterface(iid, factory);
    }
  } else if (name != nullptr && wcsncmp(name, L"Windows.Gaming.Input.", 21) == 0) {
    WgiObserveClass(name);
  }
  return real(activatableClassId, iid, factory);
}

/**
 * @brief Whether the Windows.Gaming.Input fake gamepad is enabled via the
 * settings file ("XInputFakeWgi", default 0).
 *
 * UE5 titles read the controller through Windows.Gaming.Input instead of
 * XInput; this layer presents the same one-pad illusion on that API surface:
 * the Gamepad statics are replaced by a fake that serves one connected pad
 * (fed from the XInput UDP snapshots) whenever the OS itself reports none.
 * Like the XInput serve family it is intentionally not armed-gated: games
 * enumerate the pad once at startup and never look again.
 * @return True when the WGI fake gamepad should be presented.
 */
static bool XInputFakeWgiEnabled(void) {
  return FileDword("XInputFakeWgi", 1) != 0;
}

/**
 * @brief Install the Windows.Gaming.Input fake gamepad (XInputFakeWgi=1).
 *
 * Captures the real OS Gamepad statics first so the fake can pass real
 * controllers through, then detours combase!RoGetActivationFactory. The
 * XInput slot lock is initialized here too, so the layer also works in
 * configurations where the XInput hook family stayed off.
 */
/**
 * @brief Vtable thunk: get_Gamepads returns the fake pad view directly.
 *
 * Avoids recursion by not calling the real statics.
 */
static HRESULT STDMETHODCALLTYPE WgiVtableGetGamepads_Thunk(
    void* /*self*/, WgiIVectorViewGamepad** value) {
  if (value == nullptr)
    return E_POINTER;
  InterlockedIncrement64(&g_wgiListCalls);
  *value = new WgiFakeVectorView();
  if (InterlockedCompareExchange64(&g_wgiListLogged, 1, 0) == 0)
    Dlog("wgi: vtable get_Gamepads serving fake pad");
  return S_OK;
}

/**
 * @brief Vtable thunk: add_GamepadAdded delegates to the fake statics.
 */
static HRESULT STDMETHODCALLTYPE WgiVtableAddGamepadAdded_Thunk(
    void* /*self*/, WgiIEventHandlerGamepad* value, WgiEventToken* token) {
  InterlockedIncrement64(&g_wgiAddedSubs);
  if (InterlockedCompareExchange64(&g_wgiAddLogged, 1, 0) == 0)
    Dlog("wgi: vtable add_GamepadAdded intercepted");
  return g_wgiFakeStaticsObj->add_GamepadAdded(value, token);
}

/**
 * @brief Vtable thunk: add_GamepadRemoved delegates to the fake statics.
 */
static HRESULT STDMETHODCALLTYPE WgiVtableAddGamepadRemoved_Thunk(
    void* /*self*/, WgiIEventHandlerGamepad* value, WgiEventToken* token) {
  return g_wgiFakeStaticsObj->add_GamepadRemoved(value, token);
}

/**
 * @brief Vtable thunk: FromGameController maps fake controllers to the fake
 * Gamepad, and delegates real controllers to the original vtable slot.
 */
static HRESULT STDMETHODCALLTYPE WgiVtableFromGameController_Thunk(
    void* self, void* controller, WgiIGamepad** value) {
  if (value == nullptr)
    return E_POINTER;
  *value = nullptr;
  if (controller == nullptr)
    return E_POINTER;
  {
    static volatile LONG s_fgcLogged = 0;
    if (InterlockedCompareExchange(&s_fgcLogged, 1, 0) == 0)
      Dlog("wgi: vtable FromGameController intercepted");
  }
  if (WgiIsFakePadController(controller) || WgiIsFakeRawController(controller)) {
    WgiAcquireFakePad(value);
    return S_OK;
  }
  if (g_wgiOrigStatics2Vtable != nullptr) {
    auto origFn = reinterpret_cast<HRESULT (STDMETHODCALLTYPE*)(void*, void*, WgiIGamepad**)>(
        g_wgiOrigStatics2Vtable[6]);
    return origFn(self, controller, value);
  }
  return E_INVALIDARG;
}

static void WgiFakeInstall(void) {
  if (!XInputFakeWgiEnabled()) {
    Dlog("wgi: fake gamepad skipped (XInputFakeWgi=0)");
    return;
  }
  if (g_wgiLockInit != 1) {
    InitializeCriticalSection(&g_wgiLock);
    InterlockedExchange(&g_wgiLockInit, 1);
  }
  if (g_xinputLockInit != 1) {
    InitializeCriticalSection(&g_xinputLock);
    InterlockedExchange(&g_xinputLockInit, 1);
    memset(g_xinputSlots, 0, sizeof(g_xinputSlots));
  }
  HMODULE comb = GetModuleHandleW(L"combase.dll");
  if (comb == nullptr)
    comb = LoadLibraryW(L"combase.dll");
  if (comb == nullptr) {
    Dlog("wgi: combase.dll unavailable, fake gamepad skipped");
    return;
  }
  g_wgiGetStringRawBuffer =
      (FnWgiGetStringRawBuffer)GetProcAddress(comb, "WindowsGetStringRawBuffer");
  g_wgiCreateString = (FnWgiCreateString)GetProcAddress(comb, "WindowsCreateString");
  g_wgiDeleteString = (FnWgiDeleteString)GetProcAddress(comb, "WindowsDeleteString");
  if (g_wgiGetStringRawBuffer == nullptr) {
    Dlog("wgi: WindowsGetStringRawBuffer missing, fake gamepad skipped");
    return;
  }
  LARGE_INTEGER freq;
  if (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0)
    g_wgiQpcFreqHz = (std::uint64_t)freq.QuadPart;
  memset(&g_wgiVibration, 0, sizeof(g_wgiVibration));
  memset(g_wgiAddedHandlers, 0, sizeof(g_wgiAddedHandlers));
  memset(g_wgiRemovedHandlers, 0, sizeof(g_wgiRemovedHandlers));

  const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool coInit = (coHr == S_OK);
  Dlog("wgi: install begin (CoInitializeEx hr=0x%08lX)", (unsigned long)coHr);

  /* Capture the OS statics BEFORE the detour goes in: the fake passes real
     controllers through, so the object it forwards to must be the genuine
     one obtained through the unhooked resolver. */
  FARPROC roGet = GetProcAddress(comb, "RoGetActivationFactory");
  if (roGet != nullptr && g_wgiCreateString != nullptr) {
    WgiHstring className = nullptr;
    if (SUCCEEDED(g_wgiCreateString(L"Windows.Gaming.Input.Gamepad", 28, &className)) &&
        className != nullptr) {
      WgiIGamepadStatics* real = nullptr;
      const HRESULT hr = ((FnWgiRoGetActivationFactory)roGet)(
          className, kWgiIidIGamepadStatics, (void**)&real);
      if (SUCCEEDED(hr) && real != nullptr) {
        WgiIGamepadStatics2* real2 = nullptr;
        real->QueryInterface(kWgiIidIGamepadStatics2, (void**)&real2);
        EnterCriticalSection(&g_wgiLock);
        g_wgiRealStatics = real;
        g_wgiRealStatics2 = real2;
        LeaveCriticalSection(&g_wgiLock);
        Dlog("wgi: real Gamepad statics captured (statics=%p statics2=%p)",
             (void*)real, (void*)real2);
      } else {
        Dlog("wgi: real Gamepad statics unavailable hr=0x%08lX (fake serves alone)",
             (unsigned long)hr);
      }
      if (g_wgiDeleteString != nullptr)
        g_wgiDeleteString(className);
    }
  }

  g_wgiFakePadObj = new WgiFakeGamepad();
  g_wgiFakePadI = static_cast<WgiIGamepad*>(g_wgiFakePadObj);
  g_wgiFakeStaticsObj = new WgiFakeStatics();

  /* Capture the OS RawGameController statics BEFORE the detour goes in, and
     glean the parameterized collection IIDs (IVectorView/IIterable<
     RawGameController>) from the real view's GetIids. The fake view answers
     the IIterable QI that C++/WinRT's range-for issues before First() through
     these IIDs; without them the enumeration walk throws hresult_no_interface. */
  if (roGet != nullptr && g_wgiCreateString != nullptr) {
    WgiHstring rawName = nullptr;
    if (SUCCEEDED(g_wgiCreateString(L"Windows.Gaming.Input.RawGameController", 37,
                                    &rawName)) &&
        rawName != nullptr) {
      WgiIRawGameControllerStatics* rawReal = nullptr;
      const HRESULT hr = ((FnWgiRoGetActivationFactory)roGet)(
          rawName, kWgiIidIRawGameControllerStatics, (void**)&rawReal);
      if (SUCCEEDED(hr) && rawReal != nullptr) {
        EnterCriticalSection(&g_wgiLock);
        g_wgiRealRawStatics = rawReal;
        LeaveCriticalSection(&g_wgiLock);
        Dlog("wgi: real RawGameController statics captured (statics=%p)", (void*)rawReal);
        WgiIVectorViewRawGameController* realView = nullptr;
        if (SUCCEEDED(rawReal->get_RawGameControllers(&realView)) && realView != nullptr) {
          ULONG iidCount = 0;
          IID* iidList = nullptr;
          if (SUCCEEDED(realView->GetIids(&iidCount, &iidList)) && iidList != nullptr) {
            int collected = 0;
            for (ULONG i = 0; i < iidCount && collected < 4; ++i) {
              if (!IsEqualGUID(iidList[i], kWgiIidInspectable) &&
                  !IsEqualGUID(iidList[i], kWgiIidUnknown))
                g_wgiRawViewIids[collected++] = iidList[i];
            }
            g_wgiRawViewIidCount = collected;
            CoTaskMemFree(iidList);
            Dlog("wgi: gleaned %d RawGameController collection IIDs from the real view",
                 collected);
          }
          realView->Release();
        }
      } else {
        Dlog("wgi: real RawGameController statics unavailable hr=0x%08lX (fake serves alone)",
             (unsigned long)hr);
      }
      if (g_wgiDeleteString != nullptr)
        g_wgiDeleteString(rawName);
    }
  }

  g_wgiFakeRawPadObj = new WgiFakeRawGamepad();
  g_wgiFakeRawPadI = static_cast<WgiIRawGameController*>(g_wgiFakeRawPadObj);
  g_wgiFakeRawStaticsObj = new WgiFakeRawStatics();

  if (roGet != nullptr) {
    void* real = (void*)roGet;
    if (DetourInstall((void*)roGet, (void*)&MyRoGetActivationFactory, &real,
                      &g_detourRoGet)) {
      g_realRoGetActivationFactory = (FnWgiRoGetActivationFactory)real;
      g_exportRoGetActivationFactory = (void*)roGet;
      InterlockedExchange(&g_wgiInstalled, 1);
      Dlog("wgi: RoGetActivationFactory hooked (target=%p)", (void*)roGet);
    } else {
      Dlog("wgi: RoGetActivationFactory hook failed, fake gamepad not served");
    }
  } else {
    Dlog("wgi: RoGetActivationFactory missing, fake gamepad not served");
  }
  /* Vtable-hook the real Gamepad statics so that games which already hold
     the OS statics pointer (obtained before injection) also see the fake
     pad. This is the critical path for hot-plug: the game subscribed to
     GamepadAdded via the real statics before we could intercept
     RoGetActivationFactory, so we must rewrite the vtable in-place. */
  if (g_wgiRealStatics != nullptr) {
    void** origVt = *reinterpret_cast<void***>(g_wgiRealStatics);
    if (origVt != nullptr) {
      void** copy = (void**)malloc(sizeof(void*) * 11);
      if (copy != nullptr) {
        for (int i = 0; i < 11; ++i)
          copy[i] = origVt[i];
        copy[6] = (void*)&WgiVtableAddGamepadAdded_Thunk;
        copy[8] = (void*)&WgiVtableAddGamepadRemoved_Thunk;
        copy[10] = (void*)&WgiVtableGetGamepads_Thunk;
        g_wgiOrigStaticsVtable = origVt;
        g_wgiFakeStaticsVtable = copy;
        *reinterpret_cast<void***>(g_wgiRealStatics) = copy;
        Dlog("wgi: real Gamepad statics vtable hooked (Added+Removed+get_Gamepads)");
      }
    }
  }
  if (g_wgiRealStatics2 != nullptr) {
    void** origVt = *reinterpret_cast<void***>(g_wgiRealStatics2);
    if (origVt != nullptr) {
      void** copy = (void**)malloc(sizeof(void*) * 7);
      if (copy != nullptr) {
        for (int i = 0; i < 7; ++i)
          copy[i] = origVt[i];
        copy[6] = (void*)&WgiVtableFromGameController_Thunk;
        g_wgiOrigStatics2Vtable = origVt;
        g_wgiFakeStatics2Vtable = copy;
        *reinterpret_cast<void***>(g_wgiRealStatics2) = copy;
        Dlog("wgi: real Gamepad statics2 vtable hooked (FromGameController)");
      }
    }
  }
  Dlog("wgi: fake gamepad armed (XInputFakeWgi=1)");
  if (coInit)
    CoUninitialize();
}

/**
 * @brief Undo the WGI fake layer.
 *
 * Removes the resolver detour and releases everything the layer holds. The
 * fake objects themselves stay alive: the game may still hold interface
 * pointers into them, and their methods are harmless once the hook is gone
 * (they only read slot state that stops updating).
 */
static void WgiFakeRestore(void) {
  if (g_wgiInstalled != 1)
    return;
  if (g_exportRoGetActivationFactory != nullptr)
    DetourRestore(&g_detourRoGet, (void**)&g_realRoGetActivationFactory,
                  g_exportRoGetActivationFactory);
  InterlockedExchange(&g_wgiInstalled, 0);
  if (g_wgiLockInit == 1) {
    EnterCriticalSection(&g_wgiLock);
    if (g_wgiFakeStaticsVtable != nullptr) {
      free(g_wgiFakeStaticsVtable);
      g_wgiFakeStaticsVtable = nullptr;
    }
    g_wgiOrigStaticsVtable = nullptr;
    if (g_wgiFakeStatics2Vtable != nullptr) {
      free(g_wgiFakeStatics2Vtable);
      g_wgiFakeStatics2Vtable = nullptr;
    }
    g_wgiOrigStatics2Vtable = nullptr;
    g_wgiRealStatics = nullptr;
    g_wgiRealStatics2 = nullptr;
    WgiIRawGameControllerStatics* rawReal = g_wgiRealRawStatics;
    g_wgiRealRawStatics = nullptr;
    for (int i = 0; i < WGI_MAX_HANDLERS; ++i) {
      if (g_wgiAddedHandlers[i] != nullptr) {
        g_wgiAddedHandlers[i]->Release();
        g_wgiAddedHandlers[i] = nullptr;
      }
      if (g_wgiRemovedHandlers[i] != nullptr) {
        g_wgiRemovedHandlers[i]->Release();
        g_wgiRemovedHandlers[i] = nullptr;
      }
      if (g_wgiRawAddedHandlers[i] != nullptr) {
        g_wgiRawAddedHandlers[i]->Release();
        g_wgiRawAddedHandlers[i] = nullptr;
      }
      if (g_wgiRawRemovedHandlers[i] != nullptr) {
        g_wgiRawRemovedHandlers[i]->Release();
        g_wgiRawRemovedHandlers[i] = nullptr;
      }
    }
    LeaveCriticalSection(&g_wgiLock);
    if (rawReal != nullptr)
      rawReal->Release();
  }
  Dlog("wgi: fake gamepad restored");
}

/* ------------------------------------------------------------------ */
/* Auto-rescan activation driver (XInputFakeRescan=1)                  */
/* ------------------------------------------------------------------ */
// Some engines initialise their input subsystem only once, at boot. A pad
// that exists during that window is bound as DirectInput and locked there for
// the session, so the verified-working flow (inject when the game is settled
// at the title) is: game window idle -> WM_DEVICECHANGE -> HID interface
// injected -> DI_EnumDevicesW sees the fake -> virtual pad created ->
// FakeDiGetCaps -> WMI filter -> XInputGetCapabilities(type=1).
//
// This thread waits for the game to be clearly idle (visibly sized window that
// answers WM_NULL probes quickly, XInputFakeIdleProbes in a row within
// XInputFakeIdleMaxMs), arms the fakes, posts the WM_DEVICECHANGE rescan
// sequence {40, 700, 1600, 5000 ms}, then watches how the game adopts the pad:
// WMI Win32_PNPEntity query -> adopted (stay armed); DirectInput-only bind ->
// retract, rotate the identity and retry (XInputFakeRetries); no reaction ->
// left armed. Only WM_DEVICECHANGE DBT_DEVNODES_CHANGED is posted; a
// back-to-back WM_INPUT_DEVICE_CHANGE (0x00FE) crashed DMC5 in testing.

/**
 * @brief Post the DBT_DEVNODES_CHANGED rescan message to the game window.
 * @param window Target window.
 * @param phase Log phase tag.
 */
static void AutoRescanPost(HWND window, const char* phase) {
  Dlog("auto rescan: posting WM_DEVICECHANGE DBT_DEVNODES_CHANGED to %p (%s; wmiHook=%d "
       "wmiCalls=%d)",
       (void*)window, phase, WmiFakeInstalled() ? 1 : 0, WmiCallCount());
  PostMessageW(window, WM_DEVICECHANGE, 0x0007 /* DBT_DEVNODES_CHANGED */, 0);
}

/**
 * @brief Auto-rescan driver: idle wait, arm, rescan sequence, adopt/retry.
 * @param param Unused.
 * @return 0.
 */
static DWORD WINAPI AutoRescanThreadProc(LPVOID param) {
  (void)param;
  if (g_xinputFakeOn != 1)
    return 0;
  HWND window = g_target;
  for (int attempt = 0; attempt < 200 && (window == nullptr || !IsWindow(window)); ++attempt) {
    if (g_xinputFakeOn != 1)
      return 0;
    window = g_target;
    Sleep(50);
  }
  if (window == nullptr || !IsWindow(window)) {
    Dlog("auto rescan: game window unavailable; skipped");
    return 0;
  }
  if (InterlockedCompareExchange(&g_autoRescanFired, 1, 0) == 1)
    return 0;
  // Generation 0 = the original, verified-working device identity.
  InterlockedExchange(&g_fakeIdentityGen, 0);

  // Stage 1: wait until the game is idle at the title. A sustained idle
  // streak is the behavioural signal that the engine's input init finished.
  const DWORD start = GetTickCount();
  const DWORD kMaxArmWaitMs = XInputFakeIdleMaxMs();
  const DWORD kProbeTimeoutMs = 150;
  const DWORD kRequiredProbes = XInputFakeIdleProbes();
  DWORD fastProbes = 0;
  bool idle = false;
  while (GetTickCount() - start < kMaxArmWaitMs) {
    if (g_xinputFakeOn != 1 || !IsWindow(window))
      return 0;
    RECT rect = {};
    const bool visibleSized =
        IsWindowVisible(window) && GetWindowRect(window, &rect) && rect.right > rect.left &&
        rect.bottom > rect.top;
    DWORD_PTR unused = 0;
    const bool responsive =
        visibleSized &&
        SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, kProbeTimeoutMs, &unused) != 0;
    if (responsive) {
      if (++fastProbes >= kRequiredProbes) {
        idle = true;
        break;
      }
    } else {
      fastProbes = 0;
    }
    Sleep(100);
  }

  // Stage 2: reveal the fakes and drive hot-plug, then watch adoption.
  const DWORD kMaxAttempts = XInputFakeRetries();
  bool adopted = false;
  for (DWORD attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    if (g_xinputFakeOn != 1 || !IsWindow(window))
      return 0;
    InterlockedExchange64(&g_wmiPnpQueried, 0);
    InterlockedExchange64(&g_fakeDiBound, 0);
    // Retry: re-offer the pad under a NEW device identity (fresh DI instance
    // GUID, same X360 product) so the game sees a brand-new pad arrival.
    if (attempt > 1) {
      const unsigned gen = (unsigned)InterlockedIncrement(&g_fakeIdentityGen);
      Dlog("auto rescan: re-armed pad under new device identity (generation=%u)", gen);
    }
    InterlockedExchange(&g_fakeArmed, 1);
    Dlog("auto rescan: fake pad armed (attempt=%lu/%lu idle=%d after=%lums identity=%u; "
         "wmiHook=%d wmiCalls=%d)",
         (unsigned long)attempt, (unsigned long)kMaxAttempts, idle ? 1 : 0,
         (unsigned long)(GetTickCount() - start), FakeIdentityGen(), WmiFakeInstalled() ? 1 : 0,
         WmiCallCount());
    const DWORD armedAt = GetTickCount();
    const DWORD offsets[] = {40, 700, 1600, 5000};
    for (int i = 0; i < 4; ++i) {
      const DWORD target = armedAt + offsets[i];
      const DWORD now = GetTickCount();
      if (now < target)
        Sleep(target - now);
      if (g_xinputFakeOn != 1 || !IsWindow(window))
        return 0;
      char phase[40];
      snprintf(phase, sizeof(phase), "attempt%lu arm+%lums", (unsigned long)attempt,
               (unsigned long)offsets[i]);
      AutoRescanPost(window, phase);
      if (InterlockedExchangeAdd64(&g_wmiPnpQueried, 0) != 0) {
        // The game already ran its WMI XInput filter during the rescan burst:
        // remember it, otherwise this early-exit would consume the flag and the
        // observation loop below would read zero and give up.
        adopted = true;
        break;
      }
    }
    // Observe the game's reaction over the next ~8 s.
    const DWORD kObserveMs = 8000;
    const DWORD observeStart = GetTickCount();
    bool boundWithoutWmi = false;
    while (GetTickCount() - observeStart < kObserveMs) {
      if (g_xinputFakeOn != 1 || !IsWindow(window))
        return 0;
      if (InterlockedExchangeAdd64(&g_wmiPnpQueried, 0) != 0) {
        adopted = true;
        break;
      }
      if (InterlockedExchangeAdd64(&g_fakeDiBound, 0) != 0) {
        boundWithoutWmi = true;
        break;
      }
      Sleep(100);
    }
    if (adopted) {
      Dlog("auto rescan: WMI XInput filter ran (attempt=%lu) - pad adopted as XInput; "
           "staying armed (wmiCalls=%d)",
           (unsigned long)attempt, WmiCallCount());
      return 0;
    }
    if (!boundWithoutWmi) {
      Dlog("auto rescan: no game reaction this cycle; pad left armed");
      return 0;
    }
    Dlog("auto rescan: game bound pad as DirectInput without the WMI filter (attempt=%lu); "
         "retracting and retrying",
         (unsigned long)attempt);
    InterlockedExchange(&g_fakeArmed, 0);
    AutoRescanPost(window, "retract (device removal)");
    Sleep(4000); // pacing between observable events
  }
  // Out of attempts: leave the pad armed as a plain DirectInput pad and say so.
  InterlockedExchange(&g_fakeArmed, 1);
  Dlog("auto rescan: giving up after %lu attempts; pad left armed (DirectInput fallback)",
       (unsigned long)kMaxAttempts);
  return 0;
}

/* ------------- Synthetic hot-plug nudges ------------------------------- */

/// Wired Xbox 360 pad USB device path (GUID_DEVINTERFACE_USB_DEVICE suffix).
static const WCHAR kFakeUsbDeviceNameW[] =
    L"\\\\?\\USB#VID_045E&PID_028E&IG_00#6&1b8d23d3&0&0000#{a5dcbf10-6530-11d2-901f-00c04fb951ed}";

/// GUID_DEVINTERFACE_USB_DEVICE (usbiodef.h).
static const GUID kGuidUsbDevice = {0xa5dcbf10, 0x6530, 0x11d2,
                                    {0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}};

/**
 * @brief Process-local payload for a synthetic DBT_DEVICEARRIVAL nudge.
 *
 * A real device arrival carries a per-process DEV_BROADCAST_HDR pointer in
 * lParam; that pointer is meaningless in any other process, which is why a
 * cross-process message cannot carry one. The worker thread runs INSIDE the
 * target process, so a payload posted to the target window IS dereferenceable
 * there - the one place where a synthetic arrival can be convincing. The
 * buffers live for the whole DLL lifetime (never freed while armed), so a
 * message still queued behind a later nudge keeps pointing at valid memory.
 *
 * The header mirrors DEV_BROADCAST_DEVICEINTERFACEW from dbt.h (same field
 * order and packing) so a receiving engine that parses the arrival sees the
 * exact layout it expects; MinGW's dbt.h does not ship the ..._W variant, so
 * the layout is spelled out here instead of relying on a typedef.
 */
struct FakeArrivalPayload {
  DWORD dbcc_size;          ///< Total size of header plus the name bytes.
  DWORD dbcc_devicetype;    ///< DBT_DEVTYP_DEVICEINTERFACE (5).
  DWORD dbcc_reserved;      ///< Reserved, zero.
  GUID dbcc_classguid;      ///< Interface class GUID being announced.
  WCHAR name[160];          ///< Device path carried past the flexible tail.
};

/**
 * @brief Fill one process-local arrival payload for the given interface.
 *
 * @param p Payload to fill.
 * @param guid Interface class GUID to announce (HID or USB device).
 * @param name Device path to carry, kept short enough for the payload.
 */
static void HotPlugPrepareArrival(FakeArrivalPayload *p, const GUID &guid,
                                  const WCHAR *name) {
  size_t n = wcslen(name);
  const size_t cap = sizeof(p->name) / sizeof(p->name[0]);
  if (n >= cap)
    n = cap - 1;
  memset(p, 0, sizeof(*p));
  p->dbcc_size = (DWORD)(sizeof(FakeArrivalPayload) - sizeof(p->name) +
                         (n + 1) * sizeof(WCHAR));
  p->dbcc_devicetype = 5 /* DBT_DEVTYP_DEVICEINTERFACE */;
  p->dbcc_classguid = guid;
  memcpy(p->name, name, n * sizeof(WCHAR));
  p->name[n] = L'\0';
}

/// Reused DBT_DEVICEARRIVAL payload announcing the fake HID interface.
static FakeArrivalPayload g_arrivalHid;
/// Reused DBT_DEVICEARRIVAL payload announcing the fake USB interface.
static FakeArrivalPayload g_arrivalUsb;

/**
 * @brief Prepare the two arrival payloads once, before the first nudge.
 *
 * The HID announcement carries the fake HID device path, the USB announcement
 * the matching USB path; both expose VID 045E / PID 028E so an engine that
 * parses the name or checks the interface class can recognise the wired Xbox
 * 360 identity.
 */
static void HotPlugPrepareArrivals(void) {
  HotPlugPrepareArrival(&g_arrivalHid, kGuidHidInterface, kRawFakeDeviceNameW);
  HotPlugPrepareArrival(&g_arrivalUsb, kGuidUsbDevice, kFakeUsbDeviceNameW);
}

/**
 * @brief Whether the synthetic WM_DEVICECHANGE nudge sequence is enabled
 * ("HotPlugNudges", default 1).
 *
 * A real controller announces itself at runtime by broadcasting
 * WM_DEVICECHANGE/DBT_DEVNODES_CHANGED; with no driver here that event never
 * comes, so after the hooks are installed the worker nudges the running game
 * to re-run its full device scan against the fake pad. Next to the bare
 * devnodes code the nudges also post DBT_DEVICEARRIVAL messages carrying a
 * process-local DEV_BROADCAST_DEVICEINTERFACE (HID and USB) describing the
 * fake Xbox 360 pad, so an engine that reacts to interface arrivals instead
 * of device-tree changes sees a controller plug in it can bind XInput against.
 * @return True when nudges should be posted.
 */
static bool HotPlugNudges(void) {
  return FileDword("HotPlugNudges", 1) != 0;
}

/**
 * @brief Number of nudge rounds ("HotPlugRounds", default 5, clamped to 64).
 * @return The configured round count.
 */
static int HotPlugRounds(void) {
  DWORD r = FileDword("HotPlugRounds", 5);
  if (r > 64)
    r = 64;
  return (int)r;
}

/**
 * @brief Synthesise a sequence of WM_DEVICECHANGE/DBT_DEVNODES_CHANGED
 * nudges, spaced two and a half seconds apart.
 *
 * Runs on its own thread after the worker finished installing every hook
 * layer, so each nudge lands against armed XInput (presence family included)
 * and DirectInput fakes. Each round posts DBT_DEVNODES_CHANGED (bare devnodes
 * change, which every game treats as "rescan the tree") plus two
 * DBT_DEVICEARRIVAL messages carrying the process-local DEV_BROADCAST_
 * DEVICEINTERFACE payloads - HID and USB - for the fake wired Xbox 360 pad,
 * inside the game process where their lParam pointers are valid. Messages go
 * to the subclassed target window when one exists - that is the window the
 * game's input manager listens on - and only fall back to HWND_BROADCAST with
 * the bare devnodes code when there is no target (pointer payloads cannot
 * cross processes, so arrivals are skipped on that path), to avoid disturbing
 * unrelated top-level windows system-wide. The loop stops early on g_stop.
 * @param param Unused.
 * @return 0.
 */
static DWORD WINAPI HotPlugNudgeThread(LPVOID param) {
  (void)param;
  int rounds = HotPlugRounds();
  Dlog("hotplug: start %d nudge round(s)", rounds);
  HotPlugPrepareArrivals();
  for (int i = 0; i < rounds; ++i) {
    if (InterlockedExchangeAdd(&g_stop, 0) == 1)
      break;
    if (g_target && IsWindow(g_target)) {
      PostMessageW(g_target, WM_DEVICECHANGE, 0x0007 /* DBT_DEVNODES_CHANGED */,
                   0);
      PostMessageW(g_target, WM_DEVICECHANGE, 0x8000 /* DBT_DEVICEARRIVAL */,
                   (LPARAM)&g_arrivalHid);
      Dlog("hotplug: arrival (hid interface) sent to target");
      PostMessageW(g_target, WM_DEVICECHANGE, 0x8000 /* DBT_DEVICEARRIVAL */,
                   (LPARAM)&g_arrivalUsb);
      Dlog("hotplug: arrival (usb interface) sent to target");
    } else {
      PostMessageW(HWND_BROADCAST, WM_DEVICECHANGE,
                   0x0007 /* DBT_DEVNODES_CHANGED */, 0);
    }
    Sleep(2500);
  }
  Dlog("hotplug: nudge sequence finished");
  return 0;
}

/**
 * @brief Undo the DirectInput8 fake-pad layer.
 *
 * Restores the CreateDevice/EnumDevices/GetDeviceStatus detours and drops
 * the arm flag. The temporarily created interfaces are already released.
 */
static void Di8HookRestore(void) {
  InterlockedExchange(&g_diFakeOn, 0);
  if (g_diHookedW == 1) {
    DetourRestore(&g_diDetCreateW, (void**)&g_realDiCreateDeviceW, nullptr);
    DetourRestore(&g_diDetEnumW, (void**)&g_realDiEnumDevicesW, nullptr);
    DetourRestore(&g_diDetStatusW, (void**)&g_realDiGetStatusW, nullptr);
    InterlockedExchange(&g_diHookedW, 0);
  }
  if (g_diHookedA == 1) {
    DetourRestore(&g_diDetCreateA, (void**)&g_realDiCreateDeviceA, nullptr);
    DetourRestore(&g_diDetEnumA, (void**)&g_realDiEnumDevicesA, nullptr);
    DetourRestore(&g_diDetStatusA, (void**)&g_realDiGetStatusA, nullptr);
    InterlockedExchange(&g_diHookedA, 0);
  }
  Dlog("di8: restore done");
}

/* ------------------------------------------------------------------ */
/* Window subclass: spoof activation messages                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Replacement wndproc for the target window. A line-for-line mirror
 * of the battle-tested capture-hook reference (Capture.Hook.WindowSubClass
 * WindowSubClass.cs), using the SAME message set and NOTHING else: WM_INPUT
 * (real, wParam != SRI_WPARAM, is dropped while blocking), WM_ACTIVATE /
 * WM_ACTIVATEAPP / WM_NCACTIVATE forced active, WM_MOUSEACTIVATE forwarded
 * with the target handle, WM_MOUSELEAVE / WM_KILLFOCUS acknowledged,
 * WM_SIZE / WM_MOUSEHOVER / WM_NCHITTEST pass through. The reference
 * handles NO legacy mouse/keyboard button messages, no WM_SETCURSOR, no
 * non-client button messages - and neither do we; they are all forwarded
 * untouched by the single CallWindowProcW at the end, exactly like the
 * reference. The window therefore stays a completely normal host-desktop
 * window: it can be dragged, resized and closed. The "disable keyboard and
 * mouse" guarantee for the game comes entirely from the API layer (raw and
 * polled reads return nothing while blocking, and real WM_INPUT is dropped)
 * - never from the window procedure, mirroring the reference.
 *
 * @param h The target window.
 * @param m Message id.
 * @param w Message wParam.
 * @param l Message lParam.
 * @return Result of the original wndproc (or 0 for swallowed messages).
 */
static LRESULT CALLBACK SwallowProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  /* Always forward to the original wndproc via CallWindowProcW, matching
     the battle-tested subclass pattern (see Capture.Hook WindowSubClass).
     Only the messages below are rewritten/swallowed before forwarding. If
     the window is gone/no longer subclassed we fall back to DefWindowProcW
     (a pass-through), which is always safe. */
  WNDPROC prev = g_prev;
  if (!prev || !IsWindow(h))
    return DefWindowProcW(h, m, w, l);

  bool block = g_blockWmInput == 1;

  if (IsDiagEvent(m))
    DiagEvent(h, m, w, l, block);

  switch (m) {
  case WM_SIZE:
    /* Forward: the engine owns its resize/backbuffer handling. */
    break;
  case WM_INPUT:
    /* Drop REAL raw input (wParam != SRI_WPARAM, the tag the capture-hook
       reference gives its own simulated packets) while blocking, mirroring
       the reference exactly: the engine's raw path must never run on real
       host events (measured: it wedges the UI thread and hard-freezes the
       input - ghost window + force-close dialog). The reference forwards
       simulated packets carrying wParam == SRI_WPARAM from its capture-hook
       client; a direct-play client never posts those, so every WM_INPUT
       this window sees is real and is swallowed. Acknowledging the real
       packet here also keeps the engine from entering a handler that waits
       for data that will never arrive. */
    InterlockedIncrement64(&g_wmInputSeen);
    if (block && w != (WPARAM)SRI_WPARAM)
      return 0;
    break;
  case WM_ACTIVATE:
    /* Keep the window looking active: force WA_ACTIVE and clear the
       window being deactivated, exactly like the reference. */
    if (g_state == 1) {
      w = WA_ACTIVE;
      l = 0;
    }
    break;
  case WM_ACTIVATEAPP:
    if (g_state == 1) {
      w = 1; /* fActive = true */
      l = 0;
    }
    break;
  case WM_MOUSELEAVE:
    /* Never report the mouse as having left the client area (matches the
       reference exactly: return 0, nothing else). */
    return 0;
  case WM_KILLFOCUS:
    /* Swallow: the window never "loses" focus (matches the reference
       exactly: return 0, nothing else). */
    if (g_state == 1)
      return 0;
    break;
  case WM_NCHITTEST:
    /* Pass through untouched - the reference explicitly notes that an
       RE-engine title ("鬼泣5/DMC5") must NOT have this message
       intercepted. */
    break;
  case WM_NCACTIVATE:
    if (g_state == 1) {
      w = 1; /* non-client looks active */
      l = 0;
    }
    break;
  case WM_MOUSEACTIVATE:
    /* Forward with the target window handle, exactly like the reference:
       the engine never decides "don't activate, eat the mouse click". */
    if (g_state == 1)
      w = (WPARAM)g_target;
    break;
  default:
    /* Everything else - including every legacy key/mouse message - is
       forwarded untouched, exactly like the reference's single
       CallWindowProcW at the end. */
    break;
  }
  return CallWindowProcW(prev, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Window subclass install/restore (single window, mirror of the        */
/* battle-tested Capture.Hook.WindowSubClass reference)                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Replace the target window's wndproc with SwallowProc, exactly
 * like the capture-hook reference (SetWindowLongPtrW + forward everything
 * via CallWindowProcW). Only the SINGLE main window is subclassed - the
 * reference never touches child or message-only windows, and neither do we
 * (subclassing the render window or a per-thread input window introduced
 * an immediate "window not responding" on RE-engine titles, because the
 * engine's own wndproc was bypassed mid-render).
 *
 * Runs on the injected DLL's worker thread - SetWindowLongPtrW is legal
 * from any thread, and because SwallowProc always forwards to the original
 * proc, a late or stale install can never wedge the engine. The stored
 * original proc is verified after the write and dropped if the engine
 * replaced the proc behind our back.
 *
 * @return True when SwallowProc is now the active wndproc.
 */
static bool SubClassInstall(void) {
  if (!IsWindow(g_target))
    return false;
  g_prev = (WNDPROC)GetWindowLongPtrW(g_target, GWLP_WNDPROC);
  SetWindowLongPtrW(g_target, GWLP_WNDPROC, (LONG_PTR)SwallowProc);
  WNDPROC back = (WNDPROC)GetWindowLongPtrW(g_target, GWLP_WNDPROC);
  Trace("subclass: prev=%p now=%p", (void*)g_prev, (void*)back);
  if (back != SwallowProc) {
    g_prev = 0; /* engine replaced the proc again behind our back */
    return false;
  }
  /* Post the activation burst ASYNCHRONOUSLY (PostMessageW, not
     SendMessageW). This is the original focusspoof timing that never made
     an injecting game window busy: the messages queue to the engine's own
     thread and are processed at its pace, so no foreign thread force-drops
     WM_ACTIVATE/WM_SETFOCUS into a window that is mid-boot (mid-boot
     synchronous SendMessage into a WGC-captured D3D12 title is exactly
     what produced the immediate "not responding"). The reference's
     SendMessage variant is safe there only because that library is loaded
     at process start, a hundred-plus milliseconds before the window even
     exists. */
  PostMessageW(g_target, WM_ACTIVATE, WA_ACTIVE, 0);
  PostMessageW(g_target, WM_ACTIVATEAPP, WA_ACTIVE, 0);
  PostMessageW(g_target, WM_SETFOCUS, 0, 0);
  return true;
}

/**
 * @brief Restore the original wndproc of the target window, if any.
 *
 * Only restored when the window is still directed at our SwallowProc. Many
 * engines (RE-engine, UE5/EA titles) legitimately re-install their own
 * wndproc during the session (fullscreen switches, swapchain rebuilds,
 * message-loop re-arounds). Writing our stored g_prev over such a current
 * proc would clobber the engine's live proc with a stale one. If the
 * engine already replaced the proc, leave the window entirely untouched.
 */
static void SubClassRestore(void) {
  if (g_prev && IsWindow(g_target)) {
    LONG_PTR cur = GetWindowLongPtrW(g_target, GWLP_WNDPROC);
    if (cur == (LONG_PTR)SwallowProc) {
      SetWindowLongPtrW(g_target, GWLP_WNDPROC, (LONG_PTR)g_prev);
      Dlog("subclass: restored prev=%p", (void*)g_prev);
    } else {
      Dlog("subclass: engine wndproc %p != SwallowProc; keep engine proc",
           (void*)cur);
      Trace("subclass restore: engine replaced the wndproc (%p); leaving it",
            (void*)cur);
    }
  } else if (g_prev) {
    Trace("subclass restore: target window gone; nothing to restore");
  }
  g_prev = 0;
}

/* ------------------------------------------------------------------ */
/* Target discovery: largest visible top-level window of our process   */
/* ------------------------------------------------------------------ */

/** @brief Enumeration context for FindTargetWindow. */
struct EnumCtx {
  DWORD pid;     ///< Process id to filter by.
  HWND best;     ///< Best candidate found so far.
  LONGLONG area; ///< Area of the best candidate.
};

/**
 * @brief EnumWindows callback: pick the largest visible top-level
 * window owned by our process that is activatable.
 * @param h Candidate window.
 * @param lp Pointer to EnumCtx.
 * @return Always TRUE to keep enumerating.
 */
static BOOL CALLBACK EnumCb(HWND h, LPARAM lp) {
  EnumCtx* c = (EnumCtx*)lp;
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid != c->pid)
    return TRUE;
  if (!IsWindowVisible(h))
    return TRUE;
  LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
  if ((ex & WS_EX_TOOLWINDOW) || (ex & WS_EX_NOACTIVATE) ||
      (ex & WS_EX_LAYERED))
    return TRUE;
  RECT r;
  GetWindowRect(h, &r);
  LONGLONG area = (LONGLONG)(r.right - r.left) * (LONGLONG)(r.bottom - r.top);
  if (area > c->area) {
    c->area = area;
    c->best = h;
  }
  return TRUE;
}

/**
 * @brief Find the game's main window.
 * @return HWND of the largest visible top-level window, or 0.
 */
static HWND FindTargetWindow(void) {
  EnumCtx c;
  c.pid = GetCurrentProcessId();
  c.best = 0;
  c.area = 0;
  EnumWindows(EnumCb, (LPARAM)&c);
  return c.best;
}

/* ------------------------------------------------------------------ */
/* Worker thread: real work happens HERE, never under the loader lock  */
/* ------------------------------------------------------------------ */

/**
 * @brief Whether the whole focus spoof is disabled via the settings file
 * ("DisableFocusSpoof=1"). With this set the DLL loads and exits silently.
 * @return True when disabled.
 */
static bool FocusSpoofDisabled(void) {
  return FileDword("DisableFocusSpoof", 0) != 0;
}

/**
 * @brief Heartbeat thread: proves the worker lives independently of the
 * game's UI thread.
 *
 * When the game wedges, its message pump stops but THIS thread keeps
 * logging; the last wndproc line before the gap names the exact message
 * whose handler froze it. Also reports the live WM_INPUT counter so we can
 * see whether real raw input is flowing to the subclassed main window at
 * all (0 while freezing = input is routed elsewhere).
 * @param param Unused.
 * @return 0.
 */
static DWORD WINAPI HeartbeatThread(LPVOID param) {
  (void)param;
  long n = 0;
  while (InterlockedExchangeAdd(&g_stop, 0) == 0) {
    Sleep(5000);
    if (InterlockedExchangeAdd(&g_stop, 0) != 0)
      break;
    n++;
    unsigned btn0 = 0;
    if (g_xinputLockInit == 1) {
      EnterCriticalSection(&g_xinputLock);
      btn0 = (unsigned)g_xinputSlots[0].gamepad.wButtons;
      LeaveCriticalSection(&g_xinputLock);
    }
    char rtbuf[192];
    rtbuf[0] = '\0';
    int off = 0;
    for (int i = 0; i < g_xinputRuntimeCount && off < 176; ++i) {
      off += snprintf(rtbuf + off, sizeof(rtbuf) - (size_t)off, " r%d=%s/%lld", i,
                      g_xinputRuntimes[i].name ? g_xinputRuntimes[i].name : "?",
                      (long long)InterlockedExchangeAdd64(
                          &g_xinputRuntimes[i].callsGetState, 0));
    }
if (g_state != 1 || n % 60 == 0) {
      Dlog("heartbeat %ld wmInputSeen=%lld fgw=%lld fgws=%lld aw=%lld aws=%lld foc=%lld focs=%lld udp=%lld slot0btn=%04x getstate=%lld served=%lld caps=%lld capsex=%lld set=%lld bat=%lld key=%lld enable=%lld dsound=%lld audiodev=%lld di_enum=%lld di_create=%lld di_status=%lld di_state=%lld di_fmt=%lld di_obj=%lld di_data=%lld di_caps=%lld di_info=%lld di_objinfo=%lld di_setprop=%lld di_event=%lld di_poll=%lld di_acq=%lld di_unacq=%lld di_coop=%lld rawlist=%lld rawinfo=%lld rawreg=%lld armed=%d wmiquery=%lld dibound=%lld wmicalls=%d gen=%u roget=%lld wgiact=%lld wgilist=%lld wgiread=%lld wgifresh=%lld wgiadd=%lld wgivib=%lld%s",
           n, (long long)InterlockedExchangeAdd64(&g_wmInputSeen, 0),
           (long long)InterlockedExchangeAdd64(&g_fgwCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_fgwSpoofed, 0),
           (long long)InterlockedExchangeAdd64(&g_awCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_awSpoofed, 0),
           (long long)InterlockedExchangeAdd64(&g_focusCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_focusSpoofed, 0),
           (long long)InterlockedExchangeAdd64(&g_udpDatagrams, 0), btn0,
           (long long)InterlockedExchangeAdd64(&g_xinputGetStateCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_xinputServedFresh, 0),
           (long long)InterlockedExchangeAdd64(&g_xinputCapsCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_xinputCapsExCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_xinputSetCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_xinputBatteryCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_xinputKeystrokeCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_xinputEnableCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_xinputDsoundCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_xinputAudioIdsCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diEnumCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diCreateCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diStatusCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diStateCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diFormatCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diObjCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diDataCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diCapsCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diInfoCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diObjInfoCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diSetPropCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diEventCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diPollCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diAcquireCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diUnacquireCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_diCoopCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_rawListCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_rawInfoCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_rawRegCalls, 0), FakeArmed() ? 1 : 0,
           // wmiquery / dibound are STICKY state the auto-rescan driver polls, not
           // per-interval counters: read them without clearing, or a heartbeat that
           // lands between the driver's arm and its observation eats the adoption
           // signal and the driver wrongly concludes the game ignored the pad.
           (long long)InterlockedCompareExchange64(&g_wmiPnpQueried, 0, 0),
           (long long)InterlockedCompareExchange64(&g_fakeDiBound, 0, 0), WmiCallCount(),
           FakeIdentityGen(),
           (long long)InterlockedExchangeAdd64(&g_wgiRoGetCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_wgiGamepadActivations, 0),
           (long long)InterlockedExchangeAdd64(&g_wgiListCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_wgiReadCalls, 0),
           (long long)InterlockedExchangeAdd64(&g_wgiServedFresh, 0),
           (long long)InterlockedExchangeAdd64(&g_wgiAddedSubs, 0),
           (long long)InterlockedExchangeAdd64(&g_wgiVibCalls, 0), rtbuf);
}
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Setup worker                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Setup worker: wait for the loader lock to be released, then
 * find the window, install detours and subclass it.
 * @param param Unused.
 * @return 0.
 */
static DWORD WINAPI WorkerThread(LPVOID param) {
  (void)param;
  Sleep(500);
  if (InterlockedExchangeAdd(&g_stop, 0) == 1)
    return 0;

  if (FocusSpoofDisabled()) {
    return 0;
  }

  ResetWorkerLog();
  g_workerLog = 1;
  g_diagEvents = FileDword("DiagFocusEvents", 0);
  Dlog("worker started, pid=%lu", GetCurrentProcessId());
  /* Identity of the process we were injected into. The controller supplies the
     target window from the PID it chose, so a wrong-PID injection (a launcher,
     a helper, or a second instance) would otherwise look identical to a
     working injection whose hooks simply never fire. The image path names the
     real host once and for all, and matches the host-side
     "Capturing audio from process [pid]" line. */
  {
    char selfPath[MAX_PATH];
    selfPath[0] = '\0';
    if (!GetModuleFileNameA(NULL, selfPath, MAX_PATH))
      snprintf(selfPath, sizeof(selfPath), "?");
    Dlog("worker: injected image=\"%s\"", selfPath);
  }

  /* Prefer the window handle passed by the controller ("TargetWindow" in
     the settings file). Only fall back to self-discovery when the controller
     did not provide one - the handle is authoritative and locked. A stale
     handle from a previous session (window destroyed, or the controller
     enumerated a window of another process) must be dropped and replaced by
     self-discovery, otherwise the subclass and detours would target a dead
     or foreign window and spoofing would silently do nothing. */
  ULONG_PTR ctlTarget = FileUintPtr("TargetWindow", 0);
  bool ctlValid = ctlTarget != 0 && IsWindow((HWND)ctlTarget);
  if (ctlValid) {
    DWORD owner = 0;
    GetWindowThreadProcessId((HWND)ctlTarget, &owner);
    ctlValid = owner == GetCurrentProcessId();
  }
  if (ctlValid) {
    g_target = (HWND)ctlTarget;
    Dlog("target window=%p (from controller)", (void*)g_target);
  } else {
    if (ctlTarget) {
      Dlog("controller target %p stale or foreign; self-discovering",
           (void*)ctlTarget);
    }
    g_target = FindTargetWindow();
    if (!g_target) {
      Dlog("no target window found");
      InterlockedExchange(&g_state, 0);
      return 0;
    }
    Dlog("target window=%p (self-discovered)",
         (void*)g_target);
  }
  /* Which window the spoof is anchored to. The class name distinguishes the
     real game window (e.g. "UnrealWindow") from a launcher/overlay/splash
     window that happens to be the largest one, and the title confirms it is
     THIS title rather than a foreign process window the controller handed us.
     Logged for both the controller-supplied and the self-discovered paths. */
  {
    char cls[256];
    char title[256];
    cls[0] = '\0';
    title[0] = '\0';
    GetClassNameA(g_target, cls, sizeof(cls));
    GetWindowTextA(g_target, title, sizeof(title));
    Dlog("target: class=\"%s\" title=\"%s\"", cls, title);
  }

  /* Enable spoofing once we have a valid target; both the probe hooks and
     the subclass read g_state as the on/off switch. */
  InterlockedExchange(&g_state, 1);

  /* Detours first: they intercept every call path and redirect g_real* to
     the trampolines. */
  /* One consolidated progress line instead of the old installing/done pair
     for every subsystem. */
  Dlog("worker: installing detours + subclass + input/cursor blocks + fake pads");
  DetourHookInstall();
  if (SubclassEnabled() && !SubClassInstall())
    Dlog("subclass install failed (non-fatal)");
  InputBlockHookInstall();
  CursorBlockHookInstall();
  XInputHookInstall();
  Di8HookInstall();
  RawFakeInstall();
  HidSetupDiHookInstall();
  WmiPnpInstall();
  WgiFakeInstall();

  /* Two-stage activation:
     - XInputFakeRescan=1 (default): the auto-rescan driver waits for the game
       to idle at the title, arms the fakes, posts the WM_DEVICECHANGE rescan
       sequence and watches for WMI adoption (retracting, rotating the device
       identity and retrying on a DirectInput-only bind).
     - XInputFakeRescan=0: legacy immediate arm + fixed hot-plug nudges. */
  if (XInputFakeRescanEnabled()) {
    CreateThread(NULL, 0, AutoRescanThreadProc, 0, 0, NULL);
    Dlog("worker: auto-rescan driver started (XInputFakeRescan=1)");
  } else {
    InterlockedExchange(&g_fakeArmed, 1);
    Dlog("worker: fakes armed immediately (XInputFakeRescan=0)");
    if (HotPlugNudges())
      CreateThread(NULL, 0, HotPlugNudgeThread, 0, 0, NULL);
  }

  Dlog("focus spoof active");
  CreateThread(NULL, 0, HeartbeatThread, 0, 0, NULL);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Start / stop                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Install the full focus spoof (inline detours + window subclass).
 * Safe to call from any thread; runs async on a worker.
 * @return True when accepted (not necessarily completed yet).
 */
static BOOL SpoofStartImpl(void) {
  /* Every start rewrites the worker log: repeated injection or re-enable
     into a still-running game must never accumulate older runs. */
  ResetWorkerLog();
  InterlockedExchange(&g_stop, 0);
  if (InterlockedCompareExchange(&g_state, 1, 0) == 1)
    return TRUE; /* already on */

  if (g_worker && WaitForSingleObject(g_worker, 0) != WAIT_OBJECT_0)
    return TRUE; /* an install is already in flight */

  DWORD tid = 0;
  g_worker = CreateThread(NULL, 0, WorkerThread, 0, 0, &tid);
  return g_worker != 0;
}

/**
 * @brief Undo the focus spoof: make the wndproc a pass-through, then
 * restore the original wndproc on the owning thread.
 *
 * Each undo step is logged with a millisecond timestamp so a live game
 * capture shows the exact step that wedged (the last timestamped line).
 * The detours are undone first (stop-the-world guarded) so g_real* can
 * safely go back to the exports; the window subclass is undone last and
 * only if it is still installed.
 * @return True.
 */
static BOOL SpoofStopImpl(void) {
  InterlockedExchange(&g_stop, 1);
  InterlockedExchange(&g_state, 0);
  Dlog("stop: g_state=0");
  /* Restore the detoured entry bytes first so g_real* can safely go back
     to the exports. */
  DetourHookRestore();
  Dlog("stop: focus detours restored");
  InputBlockHookRestore();
  Dlog("stop: input-block restored");
  CursorBlockHookRestore();
  Dlog("stop: cursor-block restored");
  XInputHookRestore();
  Dlog("stop: xinput restore done");
  Di8HookRestore();
  Dlog("stop: directinput8 restore done");
  /* Disarm the presence layers first so no late enumeration feeds on fakes
     that are already half-restored, then undo the SetupDi/HidD and WMI
     detours. */
  InterlockedExchange(&g_fakeArmed, 0);
  HidSetupDiHookRestore();
  WmiPnpRestore();
  WgiFakeRestore();
  Dlog("stop: wgi fake gamepad restored");
  if (g_target) {
    /* Stale install is harmless (SwallowProc is pass-through when off),
       but try to restore cleanly anyway. */
    SubClassRestore();
  }
  Dlog("stop: done");
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* DllMain + exports                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief DLL entry point. Only spawns the worker thread here; no real
 * work happens under the loader lock.
 * @param h Module handle.
 * @param reason Load/unload reason.
 * @param reserved Reserved.
 * @return Always TRUE.
 */
BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID reserved) {
  (void)reserved;
  switch (reason) {
  case DLL_PROCESS_ATTACH:
    DisableThreadLibraryCalls(h);
    g_host = h;
    /* Capture the per-game OptionsFile pointer before the worker starts,
       so every config read in this process goes to the game's own file. */
    ReadOptionsFileOverride();
    g_exportGetForegroundWindow =
        (FnGetWindow)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                    "GetForegroundWindow");
    g_exportGetActiveWindow =
        (FnGetWindow)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                    "GetActiveWindow");
    g_exportGetFocus =
        (FnGetWindow)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                    "GetFocus");
    g_exportGetAsyncKeyState =
        (FnKeyState)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                   "GetAsyncKeyState");
    g_exportGetKeyState =
        (FnKeyState)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                   "GetKeyState");
    g_exportGetKeyboardState =
        (FnKeybState)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                    "GetKeyboardState");
    g_exportGetRawInputData =
        (FnRawInput)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                   "GetRawInputData");
    g_exportGetRawInputBuffer =
        (FnRawBuf)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                 "GetRawInputBuffer");
    g_exportGetRawInputDeviceList =
        (FnRawList)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                  "GetRawInputDeviceList");
    g_exportGetRawInputDeviceInfoW =
        (FnRawInfo)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                  "GetRawInputDeviceInfoW");
    g_exportGetRawInputDeviceInfoA =
        (FnRawInfo)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                  "GetRawInputDeviceInfoA");
    g_exportRegisterRawInputDevices =
        (FnRawReg)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                 "RegisterRawInputDevices");
    g_exportShowCursor =
        (FnShowCursor)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                     "ShowCursor");
    g_exportClipCursor =
        (FnClipCursor)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                     "ClipCursor");
    g_exportGetClipCursor =
        (FnGetClipCursor)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                        "GetClipCursor");
    g_realGetAsyncKeyState = g_exportGetAsyncKeyState;
    g_realGetKeyState = g_exportGetKeyState;
    g_realGetKeyboardState = g_exportGetKeyboardState;
    g_realGetRawInputData = g_exportGetRawInputData;
    g_realGetRawInputBuffer = g_exportGetRawInputBuffer;
    g_realShowCursor = g_exportShowCursor;
    g_realClipCursor = g_exportClipCursor;
    g_realGetClipCursor = g_exportGetClipCursor;
    g_realGetForegroundWindow = g_exportGetForegroundWindow;
    g_realGetActiveWindow = g_exportGetActiveWindow;
    g_realGetFocus = g_exportGetFocus;
    Trace("attached");
    g_worker = CreateThread(NULL, 0, WorkerThread, 0, 0, NULL);
    break;
  case DLL_PROCESS_DETACH:
    InterlockedExchange(&g_stop, 1);
    Trace("detaching");
    if (reserved == NULL) {
      /* Explicit FreeLibrary: the process keeps running, so undo the
         hooks. On process exit (reserved != NULL) we deliberately do NOT
         restore: patching shared user32 entries while other threads are
         tearing down is a crash vector with zero benefit. */
      SpoofStopImpl();
    }
    if (g_worker) {
      WaitForSingleObject(g_worker, 2000);
      CloseHandle(g_worker);
      g_worker = 0;
    }
    break;
  default:
    break;
  }
  return TRUE;
}

/**
 * @brief Export: activate the focus spoof.
 * @return True when accepted.
 */
extern "C" __declspec(dllexport) BOOL WINAPI SpoofStart(void) {
  return SpoofStartImpl();
}

/**
 * @brief Export: deactivate the focus spoof.
 * @return True.
 */
extern "C" __declspec(dllexport) BOOL WINAPI SpoofStop(void) {
  return SpoofStopImpl();
}
