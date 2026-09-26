/**
 * @file src/recorder/recorder_api.cpp
 * @brief Recorder plugin loader and adapter implementation.
 */

// standard includes
#include <functional>
#include <memory>
#include <string>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

// local includes
#include "recorder_api.h"

namespace recorder {

  /**
   * @brief Function pointer type for recorder_create.
   */
  using create_fn_t = recorder_t*(*)(const init_t&);

  /**
   * @brief Function pointer type for recorder_destroy.
   */
  using destroy_fn_t = void(*)(recorder_t*);

  /**
   * @brief Internal recorder wrapper that manages DLL lifecycle.
   */
  class recorder_impl_t : public recorder_t {
  public:
    /**
     * @brief Construct a recorder implementation wrapper.
     *
     * @param impl_impl  Raw recorder implementation pointer.
     * @param dl_handle  DLL handle for cleanup.
     * @param destroy_fn Function pointer to destroy implementation.
     */
    recorder_impl_t(
      recorder_t* impl_impl,
      void* dl_handle,
      destroy_fn_t destroy_fn) :
        m_impl(impl_impl),
        m_dl_handle(dl_handle),
        m_destroy_fn(destroy_fn) {
    }

    /**
     * @brief Destroy the recorder implementation wrapper.
     */
    ~recorder_impl_t() override {
      if (m_impl && m_destroy_fn) {
        m_destroy_fn(m_impl);
      }
#ifdef _WIN32
      if (m_dl_handle) {
        FreeLibrary(static_cast<HMODULE>(m_dl_handle));
      }
#else
      if (m_dl_handle) {
        dlclose(m_dl_handle);
      }
#endif
    }

    /**
     * @brief Capture one frame and deliver it to the pipeline.
     *
     * @param push_cb Callback to deliver a captured frame to the pipeline.
     * @param pull_cb Callback to obtain an empty image buffer.
     * @param cursor  Pointer to cursor visibility flag.
     * @return Capture status.
     */
    capture_e capture(
      const push_captured_image_cb_t& push_cb,
      const pull_free_image_cb_t& pull_cb,
      bool* cursor) override {
      if (m_impl) {
        return m_impl->capture(push_cb, pull_cb, cursor);
      }
      return capture_e::error;
    }

  private:
    recorder_t* m_impl;
    void* m_dl_handle;
    destroy_fn_t m_destroy_fn;
  };

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
  recorder_ptr_t load(const std::string& dll_path, const init_t& init) {
#ifdef _WIN32
    // Load DLL
    HMODULE dl_handle = LoadLibraryA(dll_path.c_str());
    if (!dl_handle) {
      return nullptr;
    }

    // Resolve recorder_create
    auto create_fn = reinterpret_cast<create_fn_t>(GetProcAddress(dl_handle, "recorder_create"));
    if (!create_fn) {
      FreeLibrary(dl_handle);
      return nullptr;
    }

    // Resolve recorder_destroy
    auto destroy_fn = reinterpret_cast<destroy_fn_t>(GetProcAddress(dl_handle, "recorder_destroy"));
    if (!destroy_fn) {
      FreeLibrary(dl_handle);
      return nullptr;
    }

    // Create the recorder instance
    recorder_t* impl = create_fn(init);
    if (!impl) {
      FreeLibrary(dl_handle);
      return nullptr;
    }

    // Return wrapped recorder
    return recorder_ptr_t(new recorder_impl_t(impl, dl_handle, destroy_fn));
#else
    // Open the DLL
    void* dl_handle = dlopen(dll_path.c_str(), RTLD_NOW);
    if (!dl_handle) {
      return nullptr;
    }

    // Resolve recorder_create
    auto create_fn = reinterpret_cast<create_fn_t>(dlsym(dl_handle, "recorder_create"));
    if (!create_fn) {
      dlclose(dl_handle);
      return nullptr;
    }

    // Resolve recorder_destroy
    auto destroy_fn = reinterpret_cast<destroy_fn_t>(dlsym(dl_handle, "recorder_destroy"));
    if (!destroy_fn) {
      dlclose(dl_handle);
      return nullptr;
    }

    // Create the recorder instance
    recorder_t* impl = create_fn(init);
    if (!impl) {
      dlclose(dl_handle);
      return nullptr;
    }

    // Return wrapped recorder
    return recorder_ptr_t(new recorder_impl_t(impl, dl_handle, destroy_fn));
#endif
  }

  /**
   * @brief Release a recorder instance created by an implementation DLL.
   *
   * Calls the DLL's recorder_destroy() and unloads the DLL module once
   * the recorder is destroyed.
   *
   * @param ptr Recorder instance to destroy; may be nullptr.
   */
  void destroy(recorder_t* ptr) {
    // This is handled by the recorder_impl_t destructor
    delete ptr;
  }

}  // namespace recorder
