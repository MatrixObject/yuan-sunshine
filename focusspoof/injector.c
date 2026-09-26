/**
 * @file focusspoof/injector.c
 * @brief Injects focusspoof.dll into a running process and invokes its
 *        exported control functions remotely.
 *
 * The injector must have the SAME bitness as the target process: the
 * absolute addresses of kernel32 exports are only interchangeable between
 * processes of the same architecture (a WOW64 32-bit process uses its own
 * copy of kernel32 at different addresses). Compile this file once with
 * the 64-bit toolchain ( -> injectedll.exe) and once with the 32-bit
 * toolchain ( -> injectedll32.exe); the --call stub below is selected by
 * _WIN64 at compile time.
 *
 * Usage:
 *   injectedll.exe <pid> <path-to-focusspoof.dll>
 *       Inject the DLL (its DllMain auto-starts the spoof worker).
 *   injectedll.exe --call <pid> <path-to-focusspoof.dll> <SpoofStart|SpoofStop>
 *       Call the named export in the already-loaded DLL (used by the
 *       controller to unhook / re-enable a window without re-injecting).
 *
 * The classic CreateRemoteThread + LoadLibraryW technique for the inject
 * path:
 *   1. Open the target process (needs admin on a protected game).
 *   2. Allocate remote memory and copy the wide DLL path into it.
 *   3. Create a remote thread running kernel32!LoadLibraryW(path).
 *      The target's DllMain auto-runs the activation spoof.
 *   4. Wait, read the thread exit code (the LoadLibrary return value),
 *      and clean up.
 *
 * The --call path installs a small position-independent stub in the target
 * that performs LoadLibraryW(path) (returns the already-loaded handle),
 * GetProcAddress(handle, name) and finally calls the export. The freshly
 * acquired reference is released with FreeLibrary so a repeated remote
 * call never leaks a module reference. The addresses of kernel32's
 * LoadLibraryW / GetProcAddress / FreeLibrary are identical in every
 * process of the same architecture (systems DLLs load at the same image
 * base), so resolving them locally is valid remotely.
 *
 * Build (MSYS2):
 *   64-bit (UCRT64 / MINGW64):
 *     gcc -O2 -s injector.c -o injectedll.exe -lshlwapi
 *   32-bit (MINGW32):
 *     gcc -m32 -O2 -s injector.c -o injectedll32.exe -lshlwapi
 *
 * Run as administrator for a protected / other-user game.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Poke one of the DLL's exports through a remote thread. */
static int CallRemoteExport(DWORD pid, const char* dllPathA,
                            const char* exportName) {
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  FARPROC loadlib = GetProcAddress(kernel32, "LoadLibraryW");
  FARPROC getproc = GetProcAddress(kernel32, "GetProcAddress");
  FARPROC freelib = GetProcAddress(kernel32, "FreeLibrary");
  if (!loadlib || !getproc || !freelib) {
    fprintf(stderr, "resolving kernel32 exports failed: %lu\n",
            GetLastError());
    return 1;
  }

  int wlen = MultiByteToWideChar(CP_ACP, 0, dllPathA, -1, NULL, 0);
  size_t nameLen = strlen(exportName) + 1;
  if (wlen <= 0) {
    fprintf(stderr, "Cannot convert path to wide string.\n");
    return 1;
  }
  size_t pathBytes = (size_t)wlen * sizeof(WCHAR);

  /*
   * Stub for the --call path, selected at compile time by _WIN64.
   *
   * x64:
   *   A thread entry has rsp % 16 == 8, and the Windows x64 ABI needs
   *   32 bytes of shadow space plus rsp % 16 == 0 before each call, hence
   *   the sub/add rsp,0x28 frame. Uses absolute movs only (position
   *   independent).
   *   sub rsp,0x28
   *   mov rcx, imm64 pathW            ; LoadLibraryW arg
   *   mov rax, imm64 LoadLibraryW
   *   call rax                        ; hMod = LoadLibraryW(pathW)
   *   mov rbx, rax                    ; keep hMod across the calls
   *   mov rcx, rax                    ; GetProcAddress arg 1
   *   mov rdx, imm64 exportName       ; GetProcAddress arg 2
   *   mov rax, imm64 GetProcAddress
   *   call rax                        ; fn = GetProcAddress(hMod, name)
   *   test rax,rax
   *   jz   skip                       ; fn == NULL -> skip
   *   call rax                        ; fn()
   * skip:
   *   mov rcx, rbx                    ; FreeLibrary arg
   *   mov rax, imm64 FreeLibrary
   *   call rax                        ; FreeLibrary(hMod)
   *   add rsp,0x28
   *   xor eax,eax                     ; return 0
   *   ret
   *
   * x86:
   *   Thread entry esp is 4-byte aligned; the callees (LoadLibraryW,
   *   GetProcAddress, FreeLibrary) are all __stdcall and clean their own
   *   stack arguments, so only a small local frame is needed.
   *   sub esp,0x10
   *   mov eax, imm32 LoadLibraryW
   *   push imm32 pathW                ; LoadLibraryW arg
   *   call eax                        ; hMod = LoadLibraryW(pathW)
   *   mov ebx, eax                    ; keep hMod across the calls
   *   mov eax, imm32 GetProcAddress
   *   push imm32 exportName           ; GetProcAddress arg 2
   *   push ebx                        ; GetProcAddress arg 1
   *   call eax                        ; fn = GetProcAddress(hMod, name)
   *   test eax,eax
   *   jz   skip                       ; fn == NULL -> skip
   *   call eax                        ; fn()
   * skip:
   *   mov eax, imm32 FreeLibrary
   *   push ebx                        ; FreeLibrary arg
   *   call eax                        ; FreeLibrary(hMod)
   *   add esp,0x10
   *   xor eax,eax                     ; return 0
   *   ret
   */
  BYTE stub[192] = { 0 };
  int n = 0;
#if defined(_WIN64)
  stub[n++] = 0x48; stub[n++] = 0x83; stub[n++] = 0xEC; stub[n++] = 0x28; /* sub rsp,0x28 */
  /* mov rcx, imm64 pathW (patched below) */
  stub[n++] = 0x48; stub[n++] = 0xB9;
  int pathOff = n;
  n += 8;
  /* mov rax, imm64 loadlib */
  stub[n++] = 0x48; stub[n++] = 0xB8;
  int loadlibOff = n;
  n += 8;
  stub[n++] = 0xFF; stub[n++] = 0xD0; /* call rax */
  stub[n++] = 0x48; stub[n++] = 0x8B; stub[n++] = 0xD8; /* mov rbx,rax */
  stub[n++] = 0x48; stub[n++] = 0x89; stub[n++] = 0xC1; /* mov rcx,rax */
  /* mov rdx, imm64 exportName */
  stub[n++] = 0x48; stub[n++] = 0xBA;
  int nameOff = n;
  n += 8;
  /* mov rax, imm64 getproc */
  stub[n++] = 0x48; stub[n++] = 0xB8;
  int getprocOff = n;
  n += 8;
  stub[n++] = 0xFF; stub[n++] = 0xD0; /* call rax */
  stub[n++] = 0x48; stub[n++] = 0x85; stub[n++] = 0xC0; /* test rax,rax */
  stub[n++] = 0x74; stub[n++] = 0x02; /* jz +2 (skip call rax) */
  stub[n++] = 0xFF; stub[n++] = 0xD0; /* call rax */
  stub[n++] = 0x48; stub[n++] = 0x8B; stub[n++] = 0xCB; /* mov rcx,rbx */
  /* mov rax, imm64 freelib */
  stub[n++] = 0x48; stub[n++] = 0xB8;
  int freelibOff = n;
  n += 8;
  stub[n++] = 0xFF; stub[n++] = 0xD0; /* call rax */
  stub[n++] = 0x48; stub[n++] = 0x83; stub[n++] = 0xC4; stub[n++] = 0x28; /* add rsp,0x28 */
  stub[n++] = 0x31; stub[n++] = 0xC0; /* xor eax,eax */
  stub[n++] = 0xC3; /* ret */
#else
  stub[n++] = 0x83; stub[n++] = 0xEC; stub[n++] = 0x10; /* sub esp,0x10 */
  /* mov eax, imm32 loadlib */
  stub[n++] = 0xB8;
  int loadlibOff = n;
  n += 4;
  /* push imm32 pathW */
  stub[n++] = 0x68;
  int pathOff = n;
  n += 4;
  stub[n++] = 0xFF; stub[n++] = 0xD0; /* call eax */
  stub[n++] = 0x89; stub[n++] = 0xC3; /* mov ebx,eax */
  /* mov eax, imm32 getproc */
  stub[n++] = 0xB8;
  int getprocOff = n;
  n += 4;
  /* push imm32 exportName */
  stub[n++] = 0x68;
  int nameOff = n;
  n += 4;
  stub[n++] = 0x53; /* push ebx */
  stub[n++] = 0xFF; stub[n++] = 0xD0; /* call eax */
  stub[n++] = 0x85; stub[n++] = 0xC0; /* test eax,eax */
  stub[n++] = 0x74; stub[n++] = 0x02; /* jz +2 (skip call eax) */
  stub[n++] = 0xFF; stub[n++] = 0xD0; /* call eax */
  /* mov eax, imm32 freelib */
  stub[n++] = 0xB8;
  int freelibOff = n;
  n += 4;
  stub[n++] = 0x53; /* push ebx */
  stub[n++] = 0xFF; stub[n++] = 0xD0; /* call eax */
  stub[n++] = 0x83; stub[n++] = 0xC4; stub[n++] = 0x10; /* add esp,0x10 */
  stub[n++] = 0x31; stub[n++] = 0xC0; /* xor eax,eax */
  stub[n++] = 0xC3; /* ret */
#endif

  HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
  if (!hProc) {
    fprintf(stderr, "OpenProcess(%lu) failed: %lu (run as administrator?)\n",
            pid, GetLastError());
    return 1;
  }

  /* One remote allocation with the stub followed by the two strings. */
  size_t nameBytes = nameLen;
  size_t total = n + pathBytes + nameBytes;
  LPVOID remoteMem =
      VirtualAllocEx(hProc, NULL, total, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE);
  if (!remoteMem) {
    fprintf(stderr, "VirtualAllocEx failed: %lu\n", GetLastError());
    CloseHandle(hProc);
    return 1;
  }

  /* Patch in the absolute addresses (they are system-wide). */
  WCHAR* pathW = (WCHAR*)malloc(pathBytes);
  if (!pathW) {
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return 1;
  }
  MultiByteToWideChar(CP_ACP, 0, dllPathA, -1, pathW, wlen);

  BYTE* code = (BYTE*)malloc((size_t)n);
  if (!code) {
    free(pathW);
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return 1;
  }
  memcpy(code, stub, (size_t)n);
  (void)code; /* silence -Wunused-variable while assembling below */

  /* Rebuild the stub with correct remote offsets (path sits right after
     the code, name right after the path). */
  BYTE* pathRemote = (BYTE*)remoteMem + n;
  BYTE* nameRemote = pathRemote + pathBytes;
  memcpy(code + pathOff, &pathRemote, sizeof(pathRemote));
  memcpy(code + nameOff, &nameRemote, sizeof(nameRemote));
  memcpy(code + loadlibOff, &loadlib, sizeof(loadlib));
  memcpy(code + getprocOff, &getproc, sizeof(getproc));
  memcpy(code + freelibOff, &freelib, sizeof(freelib));

  if (!WriteProcessMemory(hProc, remoteMem, code, (SIZE_T)n, NULL) ||
      !WriteProcessMemory(hProc, pathRemote, pathW,
                          (SIZE_T)pathBytes, NULL) ||
      !WriteProcessMemory(hProc, nameRemote, exportName,
                          (SIZE_T)nameBytes, NULL)) {
    fprintf(stderr, "WriteProcessMemory failed: %lu\n", GetLastError());
    free(pathW);
    free(code);
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return 1;
  }
  free(pathW);
  free(code);

  HANDLE hThread = CreateRemoteThread(hProc, NULL, 0,
                                       (LPTHREAD_START_ROUTINE)remoteMem,
                                       NULL, 0, NULL);
  if (!hThread) {
    fprintf(stderr, "CreateRemoteThread failed: %lu\n", GetLastError());
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return 1;
  }
  WaitForSingleObject(hThread, 8000);
  DWORD exitCode = 0;
  GetExitCodeThread(hThread, &exitCode);
  CloseHandle(hThread);
  VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
  CloseHandle(hProc);

  if (exitCode == STILL_ACTIVE) {
    fprintf(stderr, "remote call thread still running; giving up.\n");
    return 1;
  }
  if (exitCode != 0) {
    fprintf(stderr, "remote call returned %lu (export not found?)\n",
            exitCode);
    return 1;
  }
  printf("Called %s in PID %lu\n", exportName, pid);
  return 0;
}

/** @brief Inject the DLL into the target process. */
static int InjectDll(DWORD pid, const char* dllPathA) {
  int wlen = MultiByteToWideChar(CP_ACP, 0, dllPathA, -1, NULL, 0);
  if (wlen <= 0) {
    fprintf(stderr, "Cannot convert path to wide string.\n");
    return 2;
  }
  WCHAR* pathW = (WCHAR*)malloc((size_t)wlen * sizeof(WCHAR));
  if (!pathW) {
    fprintf(stderr, "out of memory\n");
    return 2;
  }
  MultiByteToWideChar(CP_ACP, 0, dllPathA, -1, pathW, wlen);

  HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
  if (!hProc) {
    fprintf(stderr, "OpenProcess(%lu) failed: %lu (run as administrator?)\n",
            pid, GetLastError());
    free(pathW);
    return 1;
  }

  SIZE_T memSize = (SIZE_T)wlen * sizeof(WCHAR);
  LPVOID remoteMem = VirtualAllocEx(hProc, NULL, memSize,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remoteMem) {
    fprintf(stderr, "VirtualAllocEx failed: %lu\n", GetLastError());
    CloseHandle(hProc);
    free(pathW);
    return 1;
  }

  if (!WriteProcessMemory(hProc, remoteMem, pathW, memSize, NULL)) {
    fprintf(stderr, "WriteProcessMemory failed: %lu\n", GetLastError());
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);
    free(pathW);
    return 1;
  }

  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  FARPROC loadlib = GetProcAddress(kernel32, "LoadLibraryW");
  if (!loadlib) {
    fprintf(stderr, "GetProcAddress(LoadLibraryW) failed: %lu\n",
            GetLastError());
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);
    free(pathW);
    return 1;
  }

  HANDLE hThread = CreateRemoteThread(hProc, NULL, 0,
                                       (LPTHREAD_START_ROUTINE)loadlib,
                                       remoteMem, 0, NULL);
  if (!hThread) {
    fprintf(stderr, "CreateRemoteThread failed: %lu\n", GetLastError());
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);
    free(pathW);
    return 1;
  }

  /* Give the target a little time to run DllMain and install the hooks. */
  WaitForSingleObject(hThread, 5000);

  DWORD exitCode = 0;
  GetExitCodeThread(hThread, &exitCode);
  BOOL stillRunning = (exitCode == STILL_ACTIVE);

  CloseHandle(hThread);
  VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
  CloseHandle(hProc);
  free(pathW);

  if (stillRunning) {
    fprintf(stderr, "remote LoadLibrary thread still running; giving up.\n");
    return 1;
  }
  if (exitCode == 0) {
    fprintf(stderr, "LoadLibrary returned 0 (injection likely failed).\n");
    return 1;
  }

  printf("Injected. LoadLibrary handle = 0x%llX (PID %lu)\n",
         (unsigned long long)(uintptr_t)exitCode, pid);
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 5 && strcmp(argv[1], "--call") == 0) {
    DWORD pid = (DWORD)strtoul(argv[2], NULL, 0);
    if (strcmp(argv[4], "SpoofStart") == 0 ||
        strcmp(argv[4], "SpoofStop") == 0)
      return CallRemoteExport(pid, argv[3], argv[4]);
    fprintf(stderr, "Unknown export '%s' (expected SpoofStart/SpoofStop).\n",
            argv[4]);
    return 2;
  }
  if (argc < 3) {
    fprintf(stderr,
            "Usage:\n"
            "  %s <pid> <path-to-focusspoof.dll>\n"
            "  %s --call <pid> <path-to-focusspoof.dll> "
            "<SpoofStart|SpoofStop>\n",
            argv[0], argv[0]);
    return 2;
  }

  DWORD pid = (DWORD)strtoul(argv[1], NULL, 0);
  return InjectDll(pid, argv[2]);
}