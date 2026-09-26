/**
 * @file focusspoof/focusspoof.cpp
 * @brief Injected into the game process: keeps a designated window
 *        "focused" without ever taking the real foreground, so the game
 *        keeps rendering and consuming XInput gamepad input even when
 *        another window has the keyboard focus.
 *
 * Build:
 *   g++ -shared -O2 -s focusspoof.cpp -o focusspoof.dll
 * (or the equivalent MinGW-w64/MSYS2 UCRT64 toolchain)
 *   32-bit (WOW64 games such as Doom 3 BFG; MINGW32 toolchain):
 *     g++ -m32 -shared -O2 -s focusspoof.cpp -o focusspoof32.dll \
 *         -static-libgcc -static-libstdc++ -Wl,--kill-at
 *   -static-libgcc/-static-libstdc++ keep the DLL free of libgcc_s_dw2-1.dll
 *   (the game process would not find it), --kill-at drops the i686
 *   '@0' stdcall decoration so GetProcAddress("SpoofStart") resolves.
 *
 * Settings all come from the controller's config file, never the registry:
 *   %APPDATA%\SunshineWindowController\focus_options.txt
 * each line "Name=Number". Missing keys fall back to the DLL defaults
 * listed here, so a bare injection follows the same behavior as the UI.
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
 *   3. Optionally blocks every keyboard/mouse input path so the game only
 *      renders and keeps accepting XInput gamepad input. Blocking is a
 *      single install, armed by a flag switch (g_block), never by
 *      re-patching. Three complementary cut points (ensure a working
 *      baseline; hook every path Raw Input games use):
 *        a. Window-procedure layer: SwallowProc (installed by the subclass
 *           when EnableSubclass=1, DEFAULT ON) swallows WM_INPUT and the
 *           legacy key/mouse messages right at the window procedure - per
 *           the Raw Input SDK contract (WM_INPUT arrives on the WindowProc,
 *           then GetRawInputData reads it out), this is the earliest cut.
 *           SwallowProc is written after the battle-tested capture-hook
 *           reference: every message is forwarded to the original wndproc
 *           via CallWindowProcW (activation messages are rewritten, e.g.
 *           WM_ACTIVATE forced to WA_ACTIVE) so the engine's activation
 *           state machine is never broken; no message is dropped mid-
 *           dispatch without a good reason. Never uses GetMessageW /
 *           PeekMessageW detours.
 *        b. Raw-API layer: GetRawInputData and GetRawInputBuffer return no
 *           data while blocking (size probes honored per the SDK, so no
 *           mis-sized heap allocations / "Heap allocation failed" dialog).
 *        c. Polled state APIs: GetAsyncKeyState / GetKeyState /
 *           GetKeyboardState return "nothing" for titles that poll.
 *      The hooks are only installed when "BlockKeyboardMouse"=1 (default 0
 *      so a fresh install stays on the previously-known-good baseline).
 *      XInput gamepad polling is never touched.
 *   4. Keeps the mouse cursor visible and free so the remote viewer can
 *      always see and move it, independent of what the game wants. Two
 *      individually controllable cut points ("BlockCursorHide", default 1,
 *      and "BlockCursorLock", default 1):
 *        a. ShowCursor: a game request to hide the cursor (ShowCursor(FALSE))
 *           is neutralized, so the display count never drops below zero.
 *        b. ClipCursor / GetClipCursor: a game request to confine the cursor
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

#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static HWND g_target;            ///< The window we keep looking "focused".
static WNDPROC g_prev;           ///< Original wndproc of g_target.
static volatile LONG g_state = 0;/* 0 = off, 1 = on */
static volatile LONG g_stop = 0; /* 1 = worker must bail out */
static HANDLE g_worker = 0;      ///< The setup worker thread.
static HMODULE g_host = 0;       ///< This DLL's module handle.
static volatile LONG g_block = 0; ///< 1 while keyboard/mouse blocking is live.
static volatile LONG g_disableRawInput = 0; ///< 1 if RawInput hook is disabled

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

/* Keyboard/mouse blocking runs at the window-procedure layer, per the Raw
   Input SDK contract: raw input arrives as WM_INPUT on the window's
   WindowProc and is only then read out via GetRawInputData, so SwallowProc
   swallows WM_INPUT and the legacy key/mouse messages right there - earlier
   than any GetRawInputData hook, without touching user32 entry bytes
   (GetMessageW/PeekMessageW detours crashed re9) and without touching the
   message queue itself. */
// ===== 前向声明结束 =====

/* ------------------------------------------------------------------ */
/* Debug tracing (cold paths only; never inside the wndproc)           */
/* ------------------------------------------------------------------ */

static volatile LONG g_workerLog = 0; ///< 1 after worker logging is open.
static volatile LONG g_diagEvents = 0; ///< 1 when wndproc message tracing is on.
static volatile LONG g_blockSetCursor = 0; ///< 1 to swallow WM_SETCURSOR while blocking.

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
 * The file always lives under %TEMP% with a FIXED file name so it is easy
 * to locate while debugging a live game; the process id is only recorded
 * inside the file content. A backslash is inserted between the temp dir and
 * the file name explicitly, because GetTempPathA is not guaranteed to
 * terminate the path with a separator.
 *
 * @param[out] path Buffer receiving the full path.
 * @param size Size of the buffer.
 * @param stem File name stem (e.g. "focusspoof_worker").
 */
static void LogPath(char* path, size_t size, const char* stem) {
  DWORD n = GetTempPathA((DWORD)size, path);
  if (n == 0 || n >= size) {
    snprintf(path, size, ".\\%s.log", stem);
    return;
  }
  size_t base = strlen(path);
  if (base > 0 && base + 1 < size && path[base - 1] != '\\') {
    path[base++] = '\\';
    path[base] = '\0';
  }
  snprintf(path + base, size - base, "%s.log", stem);
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
  char buf[1024];
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
  case WM_ACTIVATE:
  case WM_ACTIVATEAPP:
  case WM_NCACTIVATE:
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
  case WM_SETCURSOR:
  case WM_CAPTURECHANGED:
  case WM_LBUTTONDOWN:
  case WM_LBUTTONUP:
  case WM_MBUTTONDOWN:
  case WM_MBUTTONUP:
  case WM_RBUTTONDOWN:
  case WM_RBUTTONUP:
  case WM_NCLBUTTONDOWN:
  case WM_NCLBUTTONUP:
  case WM_NCRBUTTONDOWN:
  case WM_NCRBUTTONUP:
  case WM_NCMBUTTONDOWN:
  case WM_NCMBUTTONUP:
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
  if (g_state == 1 && g_target && IsWindow(g_target))
    return g_target;
  return g_realGetForegroundWindow();
}

/**
 * @brief Hooked GetActiveWindow: pretend the target is the active window.
 * @return HWND of the target window while active, otherwise the real one.
 */
static HWND WINAPI MyGetActiveWindow(void) {
  if (g_state == 1 && g_target && IsWindow(g_target))
    return g_target;
  return g_realGetActiveWindow();
}

/**
 * @brief Hooked GetFocus: pretend the target holds keyboard focus.
 * @return HWND of the target window while active, otherwise the real one.
 */
static HWND WINAPI MyGetFocus(void) {
  if (g_state == 1 && g_target && IsWindow(g_target))
    return g_target;
  return g_realGetFocus();
}

/**
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

  /* 0x70-0x7F rel8 jcc and 0xE3 jcxz: unusual in a prologue; reject. */
  if ((op >= 0x70 && op <= 0x7F) || op == 0xE3)
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
 * independent of the trampoline's position. A `jmp/call [rip+disp32]`
 * indirection is rewritten as `mov rax, <slot>; <op> [rax]`, preserving
 * the memory indirection (the slot address, not the pointer value, is
 * loaded). Everything else is copied verbatim (register/base-pointer
 * moves carry no relative addressing and stay position-independent).
 *
 * @param src Original instruction bytes.
 * @param insn Decoded instruction.
 * @param[out] out Result buffer.
 * @return True on success.
 */
static bool RelocateInsn(const BYTE* src, const DetourInsn* insn,
                         RelocatedInsn* out) {
  int n = 0;

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
  /* [rip + disp32]: mov rax, <slot address>; jmp/call [rax]. */
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

  BYTE trampBuf[DETOUR_MAX_PATCH * 4];
  int tOff = 0;

  while (off < DETOUR_MAX_PATCH) {
    if (!DecodeInsn((BYTE*)entry + off, &insn))
      break; /* unsupported: stop copying, bail below. */
    if (!RelocateInsn((BYTE*)entry + off, &insn, &rel))
      break;
    if (tOff + rel.len > (int)sizeof(trampBuf))
      break;
    memcpy(trampBuf + tOff, rel.bytes, (size_t)rel.len);
    tOff += rel.len;
    off += insn.len;
    if (off >= DETOUR_MIN_PATCH)
      break;
  }
  if (off < DETOUR_MIN_PATCH)
    return false; /* could not cover enough bytes */

  /* Append: jmp to the first unpatched original byte. */
  const uintptr_t cont = (uintptr_t)entry + off;
#if defined(_WIN64)
  memcpy(trampBuf + tOff, "\x48\xB8", 2); /* mov rax, imm64 */
  tOff += 2;
#else
  memcpy(trampBuf + tOff, "\xB8", 1); /* mov eax, imm32 */
  tOff += 1;
#endif
  memcpy(trampBuf + tOff, &cont, sizeof(cont));
  tOff += (int)sizeof(cont);
  memcpy(trampBuf + tOff, "\xFF\xE0", 2); /* jmp rax|eax */
  tOff += 2;

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

  det->active = true;
  *realOut = tramp;
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
 * @brief Read one "name=value" switch from the controller's settings file
 * (%APPDATA%\SunshineWindowController\focus_options.txt). The companion
 * WPF controller writes this file; the registry is deliberately never
 * touched so the feature needs no registry permissions anywhere.
 * @param name Switch name, e.g. "BlockKeyboardMouse".
 * @param def Value returned when the file or line is missing.
 * @return The stored value, or the default on any failure.
 */
static DWORD FileDword(const char* name, DWORD def) {
  const char* appdata = getenv("APPDATA");
  if (!appdata || !appdata[0])
    return def;
  char path[MAX_PATH];
  if (snprintf(path, sizeof(path),
               "%s\\SunshineWindowController\\focus_options.txt", appdata) <= 0)
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
  const char* appdata = getenv("APPDATA");
  if (!appdata || !appdata[0])
    return def;
  char path[MAX_PATH];
  if (snprintf(path, sizeof(path),
               "%s\\SunshineWindowController\\focus_options.txt", appdata) <= 0)
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
 * @brief Whether blocking keyboard/mouse input is enabled. Defaults to OFF
 * (a fresh install keeps the previously-known-good, non-blocking baseline;
 * the UI writes "BlockKeyboardMouse=1" into focus_options.txt to arm it).
 * XInput gamepad input is never affected.
 * @return True when the hooks should be installed.
 */
static bool InputBlockEnabled(void) {
  return FileDword("BlockKeyboardMouse", 0) != 0;
}

/**
 * @brief Hooked GetAsyncKeyState: reports nothing pressed while blocking.
 * @param vKey Virtual-key code.
 * @return 0 while active, otherwise the real state.
 */
static SHORT WINAPI MyGetAsyncKeyState(int vKey) {
  if (g_realGetAsyncKeyState == 0) return 0;
  if (g_block == 1)
    return 0;
  return g_realGetAsyncKeyState(vKey);
}

/**
 * @brief Hooked GetKeyState: reports nothing toggled/pressed while active.
 * @param nVirtKey Virtual-key code.
 * @return 0 while active, otherwise the real state.
 */
static SHORT WINAPI MyGetKeyState(int nVirtKey) {
  if (g_realGetKeyState == 0) return 0;
  if (g_block == 1)
    return 0;
  return g_realGetKeyState(nVirtKey);
}

/**
 * @brief Hooked GetKeyboardState: zeroes the whole state table while active.
 * @param lpKeyState 256-byte output buffer.
 * @return TRUE.
 */
static BOOL WINAPI MyGetKeyboardState(PBYTE lpKeyState) {
  if (g_realGetKeyboardState == 0) {
    memset(lpKeyState, 0, 256);
    return TRUE;
  }
  if (g_block == 1) {
    memset(lpKeyState, 0, 256);
    return TRUE;
  }
  return g_realGetKeyboardState(lpKeyState);
}

/**
 * @brief Whether a message carries direct keyboard/mouse input that
 * SwallowProc drops while blocking is active.
 * @param msg The message id.
 * @return True when the message carries direct user keyboard/mouse input.
 */
static bool IsBlockedMessage(UINT msg) {
  switch (msg) {
  case WM_KEYDOWN:
  case WM_KEYUP:
  case WM_SYSKEYDOWN:
  case WM_SYSKEYUP:
  case WM_MOUSEMOVE:
  case WM_LBUTTONDOWN:
  case WM_LBUTTONUP:
  case WM_MBUTTONDOWN:
  case WM_MBUTTONUP:
  case WM_RBUTTONDOWN:
  case WM_RBUTTONUP:
  case WM_XBUTTONDOWN:
  case WM_XBUTTONUP:
  case WM_MOUSEWHEEL:
  case WM_MOUSEHWHEEL:
    return true;
  default:
    return false;
  }
}

/**
 * @brief Hooked GetRawInputData: while blocking is active the data query
 * returns 0 bytes so raw keyboard/mouse input is invisible to any code
 * path that reads it outside the message dispatch (the primary path is cut
 * earlier, at WM_INPUT in SwallowProc). The size probe (pData == NULL) is
 * honored with the real required size so an engine that allocates its
 * RAWINPUT buffer from the probe never sees a bogus 0-length allocation
 * (the earlier "Heap allocation failed" dialog was the engine's heap
 * bookkeeping reacting to that inconsistency); the subsequent data query
 * is then satisfied with 0 bytes. Per the SDK, pData==NULL success returns
 * 0, pData!=NULL success returns bytes copied, error returns (UINT)-1.
 * @param hRawInput Raw input handle from WM_INPUT.
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
  if (g_block == 1) {
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
 * @brief Hooked GetRawInputBuffer: while blocking is active, no RAWINPUT
 * records are written so an engine that drains raw input in batches (the
 * pattern of GetRawInputBuffer-based loops) sees no keyboard/mouse events.
 * The buffer-size probe (pData == NULL) is honored with the real first-
 * record size so the engine's allocation never mis-sizes. Per the SDK,
 * pData==NULL success returns 0 (with the required size in *pcbSize),
 * pData!=NULL success returns the number of records written, error
 * returns (UINT)-1.
 * @param pData RAWINPUT output buffer, NULL when probing the size.
 * @param pcbSize In/out buffer size in bytes.
 * @param cbSizeHeader Size of RAWINPUTHEADER.
 * @return Records written (0 while blocking), like the original API.
 */
static UINT WINAPI MyGetRawInputBuffer(PRAWINPUT pData, PUINT pcbSize,
                                       UINT cbSizeHeader) {
  if (g_realGetRawInputBuffer == 0)
    return 0;
  if (g_block == 1) {
    if (pData == NULL && pcbSize)
      return g_realGetRawInputBuffer(NULL, pcbSize, cbSizeHeader);
    if (pcbSize)
      *pcbSize = 0;
    return 0;
  }
  return g_realGetRawInputBuffer(pData, pcbSize, cbSizeHeader);
}

/**
 * @brief Install the keyboard/mouse blocking hooks (inline detours) unless
 * disabled via the settings file. Failure of any single hook is non-fatal
 * and logged; the game keeps the remaining behavior.
 */
static void InputBlockHookInstall(void) {
  bool enabled = InputBlockEnabled();
  Trace("InputBlockHookInstall: BlockKeyboardMouse=%d", enabled ? 1 : 0);
  Dlog("InputBlockHookInstall: BlockKeyboardMouse=%d", enabled ? 1 : 0);
  
  if (!enabled) {
    Trace("keyboard/mouse blocking disabled by config");
    Dlog("keyboard/mouse blocking disabled by config");
    return;
  }

  bool disableRawInput = FileDword("DisableRawInput", 0) != 0;
  /* Window-procedure layer: SwallowProc also swallows WM_INPUT and legacy
     key/mouse messages at the window procedure when a subclass is present.
     The subclass is installed by the focus-spoof path (EnableSubclass,
     default ON) and forwards every message via CallWindowProcW per the
     battle-tested capture-hook reference pattern. The raw-API layer is a
     secondary cut for code paths that read outside WM_INPUT dispatch. */

  InterlockedExchange(&g_disableRawInput, disableRawInput ? 1 : 0);

  {
    Trace("Installing detours for keyboard/mouse blocking");
    Dlog("Installing detours for keyboard/mouse blocking");

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

    // RawInput 独立开关：默认开启两层 raw 钩子（GetRawInputData +
    // GetRawInputBuffer），两者都按 SDK 语义实现（保留 size probe）。
    if (!disableRawInput) {
      ok = DetourInstall((void*)g_exportGetRawInputData,
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
      Dlog("block: raw input hooks disabled by config");
    }
  }

  if (g_target && g_prev) {
    Dlog("block: window-procedure subclass present");
  } else {
    Dlog("block: window-procedure subclass absent (raw-API layer only)");
  }

  InterlockedExchange(&g_block, 1);
  Dlog("keyboard/mouse input blocked");
}

/**
 * @brief Restore all keyboard/mouse blocking hooks and their fall-through
 * pointers.
 */
static void InputBlockHookRestore(void) {
  InterlockedExchange(&g_block, 0);
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
 * @brief Whether the game is prevented from hiding the cursor via
 * "BlockCursorHide" (default ON). With this set, ShowCursor(FALSE) is
 * neutralized so the cursor never disappears (RE-engine games hide the
 * cursor during menus / cutscenes, which breaks the remote cursor).
 * @return True when the cursor should stay visible.
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
 * @brief Hooked ShowCursor: while blocking is active, a FALSE request
 * (hide) is turned into a no-op so the display count never drops below
 * zero and the cursor stays visible; TRUE is passed through.
 * @param bShow TRUE to show, FALSE to hide.
 * @return The new display count.
 */
static int WINAPI MyShowCursor(BOOL bShow) {
  if (g_realShowCursor == 0)
    return 0;
  if (g_enableCursorHide == 1 && bShow == FALSE) {
    /* Stay visible: keep the reference count pinned at >= 1. */
    return g_realShowCursor(TRUE);
  }
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
  Trace("CursorBlockHookInstall: hide=%d lock=%d", hide ? 1 : 0, lock ? 1 : 0);

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
/* Window subclass: spoof activation messages                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Replacement wndproc for the target window. Spoofs activation
 * messages while spoofing is active; otherwise it is a pure pass-through
 * (so a stale or late install/restore can never hurt the game). No file
 * I/O here by design.
 *
 * @param h The target window.
 * @param m Message id.
 * @param w Message wParam.
 * @param l Message lParam.
 * @return Result of the original wndproc (or 0 for swallowed messages).
 */
static LRESULT CALLBACK SwallowProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  WNDPROC prev = g_prev;
  /* Always forward to the original wndproc via CallWindowProcW, matching
     the battle-tested subclass pattern (see Capture.Hook WindowSubClass).
     Only the messages below are rewritten/swallowed before forwarding; a
     bare return would break the engine's activation state machine (the
     crashes seen when WM_ACTIVATE/mouse-activation messages were dropped
     mid-dispatch). If the window is gone/no longer subclassed we fall back
     to DefWindowProcW, which is always safe. */
  if (!prev || !IsWindow(h))
    return DefWindowProcW(h, m, w, l);

  bool block = g_block == 1;

  if (IsDiagEvent(m))
    DiagEvent(h, m, w, l, block);

  switch (m) {
  case WM_INPUT:
    /* Raw keyboard/mouse input: when blocking, drop the message so the
       game's own wndproc never sees a key/mouse event. DefWindowProcW
       performs the system's raw-input handle cleanup; the Raw Input SDK
       contract is exactly "WM_INPUT arrives, then GetRawInputData reads
       it out", so cutting it here stops every read path at once. */
    if (block)
      return DefWindowProcW(h, m, w, l);
    break;
  case WM_ACTIVATE:
    /* Keep the window looking active: force WA_ACTIVE and clear the
       lParam (the window being deactivated), then forward. */
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
  case WM_NCACTIVATE:
    if (g_state == 1)
      w = 1; /* non-client looks active */
    break;
  case WM_MOUSEACTIVATE:
    /* Forward with the target window handle so the game never decides
       "don't activate, eat the mouse click" (MA_NOACTIVATE). */
    if (g_state == 1)
      w = (WPARAM)g_target;
    break;
  case WM_KILLFOCUS:
    /* Swallow: the window never "loses" focus. Engines that track focus via
       the WM_SETFOCUS/WM_KILLFOCUS pair (UE5/EA titles, e.g. Split Fiction)
       drop gamepad and keyboard input when their pair went to "unfocused";
       a swallowed KILLFOCUS therefore re-issues a synthetic WM_SETFOCUS for
       the target so the pair stays balanced and the input layer keeps
       accepting gamepad. */
    if (g_state == 1) {
      PostMessageW(h, WM_SETFOCUS, (WPARAM)g_target, 0);
      return 0;
    }
    break;
  case WM_MOUSELEAVE:
    /* Never report the mouse as having left the client area. */
    return 0;
  case WM_NCHITTEST:
    /* Must not be intercepted: some RE-engine titles break if we answer
       HTCLIENT here (reference code note). */
    break;
  case WM_SIZE:
    break;
  case WM_SETCURSOR:
    /* Optional extra hardening for WGC-captured RE-engine titles: the
       engine performs synchronous D3D12 GPU work inside its WM_SETCURSOR
       handling, and under window capture that render path never completes -
       field evidence showed the wndproc hung inside WM_SETCURSOR right
       after the first client-area click (no further message ever
       dispatched), with the stack deep in D3D12Core/d3d12 fence waits.
       The classic mouse-button messages are already swallowed by the
       default branch, so WM_SETCURSOR is the only pointer message that
       still reaches the engine while blocking. When BlockSetCursor=1 we
       swallow it too, keeping the engine completely out of the pointer-
       driven GPU path; the cursor is irrelevant on the remote client
       (always hidden by the cursor-block hooks, pointer drawn client-side
       by the streaming client). */
    if (block && g_blockSetCursor == 1)
      return 0;
    break;
  default:
    /* Legacy key/mouse messages: swallow while blocking. Everything else
       (WM_PAINT, WM_TIMER, engine-private messages, ...) is forwarded
       untouched. */
    if (block && IsBlockedMessage(m))
      return 0;
    break;
  }
  return CallWindowProcW(prev, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Owning-thread install/restore via a WH_GETMESSAGE hook              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Window subclass install/restore (direct, race-free)                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Replace the target window's wndproc with SwallowProc.
 *
 * This is the battle-tested pattern from the capture-hook reference
 * (SetWindowLongPtrW + forward everything via CallWindowProcW). It runs
 * on the injected DLL's worker thread - SetWindowLongPtrW is legal from
 * any thread, and because SwallowProc is a pass-through while g_state is
 * off and always forwards to the original proc, a late or stale class can
 * never wedge the engine. The stored original proc is verified after the
 * write and dropped if the engine replaced the proc behind our back.
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
  /* Per the reference: after subclassing, send the window a burst of
     activation messages so the engine flushes its "inactive" state. The
     WM_SETFOCUS wParam is the window that gained the keyboard focus -
     engines that read it (UE5/EA titles) see the target itself, matching
     the GetFocus detour so all focus signals agree on the same HWND. */
  PostMessageW(g_target, WM_ACTIVATE, WA_ACTIVE, 0);
  PostMessageW(g_target, WM_ACTIVATEAPP, 1, 0);
  PostMessageW(g_target, WM_SETFOCUS, (WPARAM)g_target, 0);
  return true;
}

/**
 * @brief Restore the original wndproc of the target window, if any.
 *
 * Only restored when the window is still directed at our SwallowProc. Many
 * engines (RE-engine, UE5/EA titles) legitimately re-install their own
 * wndproc during the session (fullscreen switches, swapchain rebuilds,
 * message-loop re-arounds). Writing our stored g_prev over such a current
 * proc would clobber the engine's live proc with a stale one, permanently
 * breaking the window's message processing - exactly the "SpoofStop does
 * not recover" symptom. If the engine already replaced the proc, we clear
 * g_prev (SwallowProc then falls back to DefWindowProcW defensively) and
 * leave the window entirely untouched.
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
 * @brief Setup worker: wait for the loader lock to be released, then
 * find the window, install detours and subclass it.
 * @param param Unused.
 * @return 0.
 */
static DWORD WINAPI WorkerThread(LPVOID param) {
  (void)param;
  Trace("worker: sleeping 500ms");
  Sleep(500);
  if (InterlockedExchangeAdd(&g_stop, 0) == 1)
    return 0;

  if (FocusSpoofDisabled()) {
    Trace("focus spoof disabled by config; doing nothing");
    return 0;
  }

char path[MAX_PATH];
  LogPath(path, sizeof(path), "focusspoof_worker");
  FILE* f = fopen(path, "w");
  if (f)
    fclose(f);
  g_workerLog = 1;
  g_diagEvents = FileDword("DiagFocusEvents", 0);
  if (g_diagEvents == 1)
    Dlog("wndproc message tracing enabled (DiagFocusEvents=1)");
  g_blockSetCursor = FileDword("BlockSetCursor", 0);
  if (g_blockSetCursor == 1)
    Dlog("WM_SETCURSOR swallowed while blocking (BlockSetCursor=1)");
  Dlog("worker started, pid=%lu", GetCurrentProcessId());

  /* Prefer the window handle passed by the controller ("TargetWindow" in
     the settings file). Only fall back to self-discovery when the controller
     did not provide one - the handle is authoritative and locked. */
  ULONG_PTR ctlTarget = FileUintPtr("TargetWindow", 0);
  if (ctlTarget && IsWindow((HWND)ctlTarget)) {
    g_target = (HWND)ctlTarget;
    Dlog("target window=%p (from controller)", (void*)g_target);
  } else {
    g_target = FindTargetWindow();
    if (!g_target) {
      Trace("no target window found");
      Dlog("no target window found");
      InterlockedExchange(&g_state, 0);
      return 0;
    }
    Dlog("target window=%p (self-discovered)",
         (void*)g_target);
  }
  Trace("target window=%p", (void*)g_target);

  /* Enable spoofing once we have a valid target; both the probe hooks and
     the subclass read g_state as the on/off switch. */
  InterlockedExchange(&g_state, 1);

  /* Detours first: they intercept every call path and redirect g_real* to
     the trampolines. */
  Dlog("worker: installing detours");
  DetourHookInstall();
  Dlog("worker: detours done");

  if (SubclassEnabled()) {
    Dlog("worker: subclass enabled via config");
    if (!SubClassInstall()) {
      Trace("subclass install failed");
      Dlog("subclass install failed (non-fatal)");
    } else {
      Dlog("worker: subclass done");
    }
  } else {
    Dlog("worker: subclass skipped (EnableSubclass=0)");
  }

  Dlog("worker: blocking keyboard/mouse");
  InputBlockHookInstall();
  Dlog("worker: keyboard/mouse blocked (if enabled)");

  Dlog("worker: installing cursor-control hooks");
  CursorBlockHookInstall();
  Dlog("worker: cursor-control hooks done");

  Trace("focus spoof active");
  Dlog("focus spoof active");
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
    CreateThread(NULL, 0, WorkerThread, 0, 0, NULL);
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
