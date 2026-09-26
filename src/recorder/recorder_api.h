/**
 * @file src/recorder/recorder_api.h
 * @brief Recorder plugin interface for Sunshine screen capture.
 *
 * This header is intentionally dependency-free (no Sunshine platform headers)
 * so that implementation DLLs can be built standalone. It defines the abstract
 * recorder interface and the C factory functions every implementation DLL must
 * export.
 *
 * @section recorder_impl Creating an Implementation DLL
 * 1. Include this header.
 * 2. Derive from recorder::recorder_t and implement capture().
 * 3. Export recorder_create() and recorder_destroy() as extern "C".
 *
 * @section recorder_config Configuration
 * Set config::video.capture to "dll:name" where name is the DLL filename
 * (without extension) located next to the sunshine executable.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

/**
 * @brief Export macro for recorder DLL functions.
 */
#ifdef _WIN32
  #define RECORDER_EXPORT extern "C" __declspec(dllexport)
#else
  #define RECORDER_EXPORT extern "C"
#endif

/**
 * @brief Recorder plugin namespace.
 */
namespace recorder {

  /**
   * @brief Enumerates supported recorder capture status values.
   */
  enum class capture_e : int {
    ok,  ///< Success
    reinit,  ///< Need to reinitialize
    timeout,  ///< Timeout
    interrupted,  ///< Capture was interrupted
    error  ///< Error
  };

  /**
   * @brief Captured frame data container.
   *
   * A plain container, safe for cross-DLL boundaries. The host owns the
   * underlying buffer and mapping; implementations fill data/width/height/
   * pixel_pitch/row_pitch and may resize the buffer if dims change.
   *
   * Frame format: BGRA, 4 bytes per pixel, row_pitch = width * 4.
   */
  struct img_t {
    std::uint8_t *data {};  ///< Pointer to the captured pixel buffer.
    std::int32_t width {};  ///< Image width in pixels.
    std::int32_t height {};  ///< Image height in pixels.
    std::int32_t pixel_pitch {};  ///< Bytes per pixel (4 for BGRA).
    std::int32_t row_pitch {};  ///< Bytes between consecutive image rows.
  };

  /**
   * @brief Configuration passed to a recorder implementation at creation time.
   *
   * Uses only simple value types safe for cross-DLL boundaries.
   */
  struct init_t {
    std::uint32_t capture_process {0};  ///< PID of the target process; 0 = not specified.
    std::uint64_t capture_window {0};  ///< Window handle (HWND, decimal); 0 = not specified.
    bool capture_cursor {true};  ///< Whether to render the cursor in captured frames.
    int width {0};  ///< Requested frame width; 0 = auto-detect from target.
    int height {0};  ///< Requested frame height; 0 = auto-detect from target.
    int fps {0};  ///< Requested frame rate; 0 = auto-detect from stream config.
  };

  /**
   * @brief Callback to deliver a captured frame to the pipeline.
   *
   * Return false to signal stop.
   */
  using push_captured_image_cb_t = std::function<bool(std::shared_ptr<img_t> &&img, bool frame_captured)>;

  /**
   * @brief Callback to obtain an empty image buffer.
   *
   * Blocks until an image is available or capture is interrupted.
   * Returns false when capture has been interrupted (img contains nullptr).
   */
  using pull_free_image_cb_t = std::function<bool(std::shared_ptr<img_t> &img_out)>;

  /**
   * @brief Abstract frame capture interface for recorder implementations.
   *
   * Each implementation DLL derives from this class and implements capture().
   * The host calls capture() in a loop; each invocation must deliver exactly
   * one frame via the push callback, or return a non-ok status.
   *
   * Implementations may acquire frames on an internal thread, but must
   * synchronize delivery through the provided callbacks.
   */
  class recorder_t {
  public:
    virtual ~recorder_t() = default;

    /**
     * @brief Capture one frame and deliver it to the pipeline.
     *
     * The implementation pulls an empty image via pull_cb, fills it with
     * frame data, and delivers it via push_cb.
     *
     * @param push_cb Callback to deliver a captured frame to the pipeline.
     *                Return false to signal stop.
     * @param pull_cb Callback to obtain an empty image buffer.
     *                Returns false when capture has been interrupted.
     * @param cursor  Pointer to the cursor-visibility flag.
     * @return Capture status: ok on success, error/reinit/interrupted on failure.
     */
    virtual capture_e capture(
      const push_captured_image_cb_t &push_cb,
      const pull_free_image_cb_t &pull_cb,
      bool *cursor) = 0;
  };

  /**
   * @brief Release a recorder instance created by an implementation DLL.
   *
   * Calls the DLL's recorder_destroy() and unloads the DLL module once
   * the recorder is destroyed.
   *
   * @param ptr Recorder instance to destroy; may be nullptr.
   */
  void destroy(recorder_t *ptr);

  /**
   * @brief Stateless deleter used to release recorder instances.
   */
  struct recorder_deleter_t {
    /**
     * @brief Destroy a recorder instance.
     *
     * @param ptr Recorder instance to destroy; may be nullptr.
     */
    void operator()(recorder_t *ptr) const {
      destroy(ptr);
    }
  };

  /**
   * @brief Owning pointer type for a recorder instance.
   *
   * The custom deleter ensures the implementing DLL is released when the
   * recorder is destroyed.
   */
  using recorder_ptr_t = std::unique_ptr<recorder_t, recorder_deleter_t>;

  /**
   * @brief Load an implementation DLL and create a recorder instance.
   *
   * Loads the DLL at dll_path, resolves the recorder_create symbol,
   * and creates a recorder with the given configuration.
   *
   * @param dll_path Full path to the implementation DLL.
   * @param init     Recorder configuration.
   * @return Owning pointer to the created recorder; nullptr on failure.
   */
  recorder_ptr_t load(const std::string &dll_path, const init_t &init);

}  // namespace recorder
