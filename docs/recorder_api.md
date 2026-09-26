# Recorder Plugin API

## Overview

Sunshine uses a plugin architecture for screen capture. The **recorder API**
(`src/recorder/recorder_api.h`) is a dependency-free header that defines the
capture interface shared between sunshine and concrete capture implementations
(e.g. `window_capture.dll`). Sunshine loads an implementation DLL at runtime
based on the `capture` configuration value.

The two halves of the system:

- `recorder_api.{h,cpp}` — interface header **and** loader/adapter, compiled
  directly into `sunshine.exe`.
- `window_capture.dll` — a self-contained implementation DLL. It only includes
  the interface header and depends on no Sunshine header besides it.

```
┌──────────────────────┐                ┌──────────────────────┐
│  sunshine.exe        │                │  window_capture.dll  │
│                      │                │  (implementation)    │
│  recorder::load()    │──LoadLibrary──▶│                      │
│  recorder_display_t  │                │  recorder_create()   │
│  (platf::display_t)  │◀──capture()────│  recorder_destroy()  │
└──────────────────────┘                └──────────────────────┘
        │                                    │
        └── both include recorder_api.h ◀────┘
```

The host calls `recorder::load()` (linked into sunshine) to load the DLL, which
returns a `recorder::recorder_ptr_t` (a `std::unique_ptr` with a custom deleter
that releases the DLL). The adapter `recorder::recorder_display_t` then wraps
this into a `platf::display_t` so it plugs into the existing streaming pipeline.

> **Important:** `recorder_display_t` and its glue types live in
> `src/recorder/recorder_glue.h`, which includes Sunshine platform headers
> (`platf::display_t`). Implementation DLLs must **never** include the glue
> header or `src/platform/common.h`; they should only include
> `recorder_api.h`.

## Interface Contract

### recorder_t

The abstract base class that every implementation must derive from.

```cpp
namespace recorder {

  /**
   * @brief Abstract frame capture interface for recorder implementations.
   */
  class recorder_t {
  public:
    virtual ~recorder_t() = default;

    /**
     * @brief Capture one frame and deliver it to the pipeline.
     *
     * @param push_cb  Deliver a captured frame to the pipeline.
     *                 Return false to stop.
     * @param pull_cb  Request an empty image to be filled; returns false
     *                 when capture is interrupted.
     * @param cursor   Pointer to cursor-visibility flag.
     * @return Capture status.
     */
    virtual capture_e capture(
      const push_captured_image_cb_t &push_cb,
      const pull_free_image_cb_t &pull_cb,
      bool *cursor) = 0;
  };

}
```

### img_t

```cpp
namespace recorder {
  /**
   * @brief Captured frame data container.
   *
   * Plain container, safe for cross-DLL boundaries. Frame format:
   * BGRA, 4 bytes per pixel, row_pitch = width * 4.
   */
  struct img_t {
    std::uint8_t *data {};         ///< Pointer to the captured pixel buffer.
    std::int32_t width {};         ///< Image width in pixels.
    std::int32_t height {};        ///< Image height in pixels.
    std::int32_t pixel_pitch {};   ///< Bytes per pixel (4 for BGRA).
    std::int32_t row_pitch {};     ///< Bytes between consecutive image rows.
  };
}
```

The buffer handed to an implementation via `pull_cb` may be resized (reallocated)
by the implementation; ownership of the buffer follows the shared pointer's
deleter, so the allocation and deletion happen on the same side of the boundary
(see "Lifetime and Thread Safety").

### Required Exports

Every implementation DLL must export these C functions:

| Function | Signature | Description |
|---|---|---|
| `recorder_create` | `recorder::recorder_t* recorder_create(const recorder::init_t&)` | Create a recorder instance. |
| `recorder_destroy` | `void recorder_destroy(recorder::recorder_t*)` | Destroy a recorder instance. |

Use `extern "C"` linkage for both exports.

### init_t

Configuration passed to `recorder_create`:

```cpp
namespace recorder {
  struct init_t {
    std::uint32_t capture_process {0};  ///< PID; 0 = not specified.
    std::uint64_t capture_window  {0};  ///< HWND (decimal); 0 = not specified.
    bool          capture_cursor  {true};
    int           width           {0};  ///< 0 = auto-detect from target.
    int           height          {0};  ///< 0 = auto-detect from target.
    int           fps             {0};  ///< 0 = auto-detect from stream config.
  };
}
```

## Loader Functions

The loader and adapter are built into `sunshine.exe` (`src/recorder/recorder_api.cpp`):

```cpp
namespace recorder {
  /**
   * @brief Load an implementation DLL and create a recorder instance.
   *
   * @param dll_path  Path to the implementation DLL.
   * @param init      Recorder configuration.
   * @return Owning pointer to the recorder; nullptr on failure.
   */
  recorder_ptr_t load(const std::string &dll_path, const init_t &init);

  /**
   * @brief Release a recorder instance (calls the DLL's destroy function).
   */
  void destroy(recorder_t *ptr);
}
```

`recorder_ptr_t` is a `std::unique_ptr<recorder_t, recorder_deleter_t>` whose
deleter calls `destroy()`, which invokes the DLL's `recorder_destroy` and unloads
the module (`FreeLibrary`) once the recorder is gone.

## Configuration

### Config File

```ini
# Select the capture backend:
#   ddx       — DXGI Desktop Duplication (default)
#   wgc       — Windows Graphics Capture
#   dll:<name> — Load <name>.dll from the recorder directory
capture = dll:window_capture

# Target window (used by window capture recorder):
capture_process = 1234       # PID of target process
capture_window  = 1234567    # HWND (decimal) of target window
```

### CLI Parameters

```
--captureProcess <pid>    PID of the process whose window to capture
--captureWindow  <hwnd>   HWND (decimal) of the window to capture
```

These override the config file values and implicitly set `capture = dll:window_capture`
if `capture` is empty, `ddx`, or `wgc`.

## Building an Implementation DLL

1. Include `recorder_api.h` (via a relative path or an appropriate include dir).
2. Derive from `recorder::recorder_t`.
3. Implement `capture()` — produce BGRA frames via the callback interface.
4. Export `recorder_create` and `recorder_destroy` with `extern "C"` linkage.
5. **Do not** link against or include any Sunshine source beyond the interface
   header; the DLL is standalone.

### CMake Example

```cmake
add_library(my_recorder SHARED my_recorder.cpp)
target_compile_definitions(my_recorder PRIVATE SUNSHINE_PLATFORM="windows")
set_target_properties(my_recorder PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/recorder"
  PREFIX ""
  SUFFIX ".dll")
install(TARGETS my_recorder RUNTIME DESTINATION "recorder")
```

> **Note:** In this repository, do **not** add `${CMAKE_SOURCE_DIR}/src` to the
> implementation's include directories. It shadows MSYS2's `<process.h>` (included
> by `pthread.h`), which drags `boost::process` into libstdc++'s gthread
> initialization and breaks the build.

## Lifetime and Thread Safety

- `recorder_create` / `recorder_destroy` are synchronized internally by the
  loader; `destroy()` must not be called concurrently with `load()` for the same
  instance.
- `capture()` is called from a single streaming thread; implementations may use
  internal threads for frame acquisition, but must synchronize frame delivery.
- The `img_t` images obtained through `pull_cb` must only be used within the
  `capture()` call; the pipeline copies frame data into its own pool on
  delivery, so implementations must not hold references to those buffers after
  `capture()` returns.
- Buffer allocations (e.g. `new[]`/`delete[]`) made by an implementation on the
  `img_t.data` pointer must be balanced within that implementation, so that
  allocation and deallocation occur on the same side of the DLL boundary.

## Current Implementations

| Name | DLL | Description |
|---|---|---|
| window_capture | `window_capture.dll` | GDI-based window capture by PID or HWND |

## Adding New Implementations

1. Create `src/recorder/impl/<name>/`.
2. Implement the `recorder_t` interface.
3. Add a CMake `add_library(<name> SHARED ...)` target (see the example above).
4. Add the DLL to the install rules in `cmake/compile_definitions/windows.cmake`.
5. Set `capture = dll:<name>` in config, or start sunshine with
   `--captureProcess` / `--captureWindow`.