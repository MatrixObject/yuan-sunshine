// proctap: process-isolated audio capture verification utility.
//
// Usage:
//   proctap.exe --pid <pid>
//
// When invoked with --pid, proctap attempts to open the target process with
// PROCESS_QUERY_LIMITED_INFORMATION. If the handle is acquired successfully,
// the utility writes "OK pid=<N>" to stdout and exits with code 0. If the
// process cannot be opened (e.g. access denied or not found), it writes an
// error message to stderr and exits with a non-zero code.
//
// The sunshine audio backend spawns proctap as a child process and reads
// raw float32 PCM from its stdout pipe; this standalone tool is provided
// for offline verification that a given PID is accessible.
//
// Compile:
//   g++ -std=c++17 -o proctap proctap.cpp -lwinbase

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

// ── command-line parsing ────────────────────────────────────────────────────

static const char* get_switch_value(const int argc, char* const* argv, const char* flag) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], flag) == 0 && i + 1 < argc) {
      return argv[i + 1];
    }
  }
  return nullptr;
}

static bool has_switch(const int argc, char* const* argv, const char* flag) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], flag) == 0) return true;
  }
  return false;
}

// ── main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  const char* pid_str = get_switch_value(argc, argv, "--pid");
  if (pid_str == nullptr) {
    fprintf(stderr, "proctap: missing --pid <pid>\n");
    fprintf(stderr, "Usage: proctap.exe --pid <pid>\n");
    return 1;
  }

  const uint32_t pid = (uint32_t)strtoul(pid_str, nullptr, 10);
  if (pid == 0) {
    fprintf(stderr, "proctap: invalid pid '%s'\n", pid_str);
    return 1;
  }

#ifdef _WIN32
  // Open the target process with limited query access.
  HANDLE h_proc = OpenProcess(
      PROCESS_QUERY_LIMITED_INFORMATION,
      FALSE,
      pid);
  if (h_proc == nullptr) {
    const DWORD err = GetLastError();
    fprintf(stderr, "proctap: cannot open process %u (error %lu)\n", pid, err);
    return 1;
  }

  // Query the exit code to confirm the process is still alive.
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(h_proc, &exit_code)) {
    const DWORD err = GetLastError();
    CloseHandle(h_proc);
    fprintf(stderr, "proctap: cannot query process %u (error %lu)\n", pid, err);
    return 1;
  }

  CloseHandle(h_proc);

  if (exit_code == STILL_ACTIVE) {
    printf("OK pid=%u\n", pid);
    return 0;
  } else {
    fprintf(stderr, "proctap: process %u exited with code %lu\n", pid, exit_code);
    return 1;
  }
#else
  // Non-Windows: not supported.
  (void)pid;
  fprintf(stderr, "proctap: unsupported on this platform\n");
  return 1;
#endif
}
