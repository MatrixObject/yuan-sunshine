/**
 * @file src/platform/windows/audio.cpp
 * @brief Definitions for Windows audio capture.
 */
#define INITGUID

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <ios>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// platform includes
#include <Audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <newdev.h>
#include <roapi.h>
#include <synchapi.h>
#include <windows.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "utf_utils.h"

// Must be the last included file
// clang-format off
#include "PolicyConfig.h"
// clang-format on

// WASAPI Process Loopback (Windows 10 build 20348+) activation types are
// missing from the MinGW audioclient.h shipped with the toolchain, so the
// small set used by per-process capture is declared here. The runtime API
// (ActivateAudioInterfaceAsync) and the completion-handler interface are
// present in the SDK import library and headers.
#ifndef __AUDIOCLIENT_PROCESS_LOOPBACK_DEFINED
#define __AUDIOCLIENT_PROCESS_LOOPBACK_DEFINED

typedef enum AUDIOCLIENT_ACTIVATION_TYPE {
  AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
  AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1,
} AUDIOCLIENT_ACTIVATION_TYPE;

typedef enum PROCESS_LOOPBACK_MODE {
  PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
  PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1,
} PROCESS_LOOPBACK_MODE;

typedef struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
  DWORD TargetProcessId;
  PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

typedef struct AUDIOCLIENT_ACTIVATION_PARAMS {
  AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
  union {
    AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
  } loopback_union;
} AUDIOCLIENT_ACTIVATION_PARAMS;

/// Virtual device identifier selecting WASAPI process-isolated loopback capture.
/// Passed verbatim as the device interface path to ActivateAudioInterfaceAsync;
/// it must not be extended with a GUID or path suffix.
static const PCWSTR VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK = L"VAD\\Process_Loopback";

#endif  // __AUDIOCLIENT_PROCESS_LOOPBACK_DEFINED

/// IID_IAgileObject is not exposed by all MinGW SDK versions.
/// {94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90}
static const GUID IID_IAgileObject_local = {
    0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 }
};

#ifdef DOXYGEN
/**
 * @brief Property key for a device description.
 */
extern const PROPERTYKEY PKEY_Device_DeviceDesc;
/**
 * @brief Property key for a device friendly name.
 */
extern const PROPERTYKEY PKEY_Device_FriendlyName;
/**
 * @brief Property key for a device interface friendly name.
 */
extern const PROPERTYKEY PKEY_DeviceInterface_FriendlyName;
#else
DEFINE_PROPERTYKEY(PKEY_Device_DeviceDesc, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 2);  // DEVPROP_TYPE_STRING
DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);  // DEVPROP_TYPE_STRING
DEFINE_PROPERTYKEY(PKEY_DeviceInterface_FriendlyName, 0x026e516e, 0xb814, 0x414b, 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22, 2);
#endif

#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(__amd64__) || defined(_M_AMD64)
  #define STEAM_DRIVER_SUBDIR L"x64"
#endif

namespace {

  constexpr auto SAMPLE_RATE = 48000;
#ifdef STEAM_DRIVER_SUBDIR
  constexpr auto STEAM_AUDIO_DRIVER_PATH = L"%CommonProgramFiles(x86)%\\Steam\\drivers\\Windows10\\" STEAM_DRIVER_SUBDIR L"\\SteamStreamingSpeakers.inf";
#endif

  constexpr auto waveformat_mask_stereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

  constexpr auto waveformat_mask_surround51_with_backspeakers = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                                                SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                                                SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;

  constexpr auto waveformat_mask_surround51_with_sidespeakers = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                                                SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                                                SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

  constexpr auto waveformat_mask_surround71 = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                              SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                              SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
                                              SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

  enum class sample_format_e {
    f32,
    s32,
    s24in32,
    s24,
    s16,
    _size,
  };

  constexpr WAVEFORMATEXTENSIBLE create_waveformat(sample_format_e sample_format, WORD channel_count, DWORD channel_mask) {
    WAVEFORMATEXTENSIBLE waveformat = {};

    switch (sample_format) {
      default:
      case sample_format_e::f32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 32;
        break;

      case sample_format_e::s32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 32;
        break;

      case sample_format_e::s24in32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 24;
        break;

      case sample_format_e::s24:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 24;
        waveformat.Samples.wValidBitsPerSample = 24;
        break;

      case sample_format_e::s16:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 16;
        waveformat.Samples.wValidBitsPerSample = 16;
        break;
    }

    static_assert((int) sample_format_e::_size == 5, "Unrecognized sample_format_e");

    waveformat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    waveformat.Format.nChannels = channel_count;
    waveformat.Format.nSamplesPerSec = SAMPLE_RATE;

    waveformat.Format.nBlockAlign = waveformat.Format.nChannels * waveformat.Format.wBitsPerSample / 8;
    waveformat.Format.nAvgBytesPerSec = waveformat.Format.nSamplesPerSec * waveformat.Format.nBlockAlign;
    waveformat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    waveformat.dwChannelMask = channel_mask;

    return waveformat;
  }

  using virtual_sink_waveformats_t = std::vector<WAVEFORMATEXTENSIBLE>;

  /**
   * @brief List of supported waveformats for an N-channel virtual audio device
   * @tparam channel_count Number of virtual audio channels
   * @returns std::vector<WAVEFORMATEXTENSIBLE>
   * @note The list of virtual formats returned are sorted in preference order and the first valid
   *       format will be used. All bits-per-sample options are listed because we try to match
   *       this to the default audio device. See also: set_format() below.
   */
  template<WORD channel_count>
  virtual_sink_waveformats_t create_virtual_sink_waveformats() {
    if constexpr (channel_count == 2) {
      auto channel_mask = waveformat_mask_stereo;
      // The 32-bit formats are a lower priority for stereo because using one will disable Dolby/DTS
      // spatial audio mode if the user enabled it on the Steam speaker.
      return {
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask),
        create_waveformat(sample_format_e::f32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask),
      };
    } else if (channel_count == 6) {
      auto channel_mask1 = waveformat_mask_surround51_with_backspeakers;
      auto channel_mask2 = waveformat_mask_surround51_with_sidespeakers;
      return {
        create_waveformat(sample_format_e::f32, channel_count, channel_mask1),
        create_waveformat(sample_format_e::f32, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask2),
      };
    } else if (channel_count == 8) {
      auto channel_mask = waveformat_mask_surround71;
      return {
        create_waveformat(sample_format_e::f32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask),
      };
    }
  }

  std::string waveformat_to_pretty_string(const WAVEFORMATEXTENSIBLE &waveformat) {
    std::string result = waveformat.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ? "F" :
                         waveformat.SubFormat == KSDATAFORMAT_SUBTYPE_PCM        ? "S" :
                                                                                   "UNKNOWN";

    result += std::format("{} {} ", static_cast<int>(waveformat.Samples.wValidBitsPerSample), static_cast<int>(waveformat.Format.nSamplesPerSec));

    switch (waveformat.dwChannelMask) {
      case waveformat_mask_stereo:
        result += "2.0";
        break;

      case waveformat_mask_surround51_with_backspeakers:
        result += "5.1";
        break;

      case waveformat_mask_surround51_with_sidespeakers:
        result += "5.1 (sidespeakers)";
        break;

      case waveformat_mask_surround71:
        result += "7.1";
        break;

      default:
        result += std::format("{} channels (unrecognized)", static_cast<int>(waveformat.Format.nChannels));
        break;
    }

    return result;
  }

}  // namespace

using namespace std::literals;

namespace platf::audio {

  /**
   * @brief Release the COM or platform reference owned by the pointer.
   *
   * @param p Pointer passed to the deleter or conversion helper.
   */
  template<class T>
  void Release(T *p) {
    p->Release();
  }

  /**
   * @brief Free memory allocated by COM task APIs.
   *
   * @param p Pointer passed to the deleter or conversion helper.
   */
  template<class T>
  void co_task_free(T *p) {
    CoTaskMemFree((LPVOID) p);
  }

  /**
   * @brief COM device enumerator pointer for WASAPI endpoint discovery.
   */
  using device_enum_t = util::safe_ptr<IMMDeviceEnumerator, Release<IMMDeviceEnumerator>>;
  /**
   * @brief COM pointer to a Windows audio endpoint device.
   */
  using device_t = util::safe_ptr<IMMDevice, Release<IMMDevice>>;
  /**
   * @brief COM pointer to a collection of Windows audio endpoint devices.
   */
  using collection_t = util::safe_ptr<IMMDeviceCollection, Release<IMMDeviceCollection>>;
  /**
   * @brief COM pointer to the WASAPI audio client interface.
   */
  using audio_client_t = util::safe_ptr<IAudioClient, Release<IAudioClient>>;
  /**
   * @brief COM pointer to the WASAPI capture client interface.
   */
  using audio_capture_t = util::safe_ptr<IAudioCaptureClient, Release<IAudioCaptureClient>>;
  /**
   * @brief CoTaskMem-allocated WAVEFORMATEX pointer.
   */
  using wave_format_t = util::safe_ptr<WAVEFORMATEX, co_task_free<WAVEFORMATEX>>;
  /**
   * @brief CoTaskMem-allocated wide string pointer.
   */
  using wstring_t = util::safe_ptr<WCHAR, co_task_free<WCHAR>>;
  /**
   * @brief Windows HANDLE wrapper closed with `CloseHandle`.
   */
  using handle_t = util::safe_ptr_v2<void, BOOL, CloseHandle>;
  /**
   * @brief COM pointer to the Windows policy configuration interface.
   */
  using policy_t = util::safe_ptr<IPolicyConfig, Release<IPolicyConfig>>;
  /**
   * @brief COM pointer to a Windows property store.
   */
  using prop_t = util::safe_ptr<IPropertyStore, Release<IPropertyStore>>;

  /**
   * @brief Initializes COM for the current thread and uninitializes it on exit.
   */
  class co_init_t: public deinit_t {
  public:
    co_init_t() {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
    }

    ~co_init_t() override {
      CoUninitialize();
    }
  };

  /**
   * @brief RAII wrapper that initializes and clears a Windows PROPVARIANT.
   */
  class prop_var_t {
  public:
    prop_var_t() {
      PropVariantInit(&prop);
    }

    ~prop_var_t() {
      PropVariantClear(&prop);
    }

    PROPVARIANT prop;  ///< Variant value returned by Windows property-store queries.
  };

  /**
   * @brief Windows audio format details selected for capture.
   */
  struct format_t {
    WORD channel_count;  ///< Channel count.
    std::string name;  ///< Human-readable name for this item.
    int capture_waveformat_channel_mask;  ///< Capture waveformat channel mask.
    virtual_sink_waveformats_t virtual_sink_waveformats;  ///< Virtual sink waveformats.
  };

  /**
   * @brief Formats.
   */
  const std::array<const format_t, 3> formats = {
    format_t {
      2,
      "Stereo",
      waveformat_mask_stereo,
      create_virtual_sink_waveformats<2>(),
    },
    format_t {
      6,
      "Surround 5.1",
      waveformat_mask_surround51_with_backspeakers,
      create_virtual_sink_waveformats<6>(),
    },
    format_t {
      8,
      "Surround 7.1",
      waveformat_mask_surround71,
      create_virtual_sink_waveformats<8>(),
    },
  };

  /**
   * @brief Create audio client.
   *
   * @param device D3D, audio, or platform device used by the operation.
   * @param format Pixel, audio, or protocol format being converted.
   * @return Constructed audio client object.
   */
  audio_client_t make_audio_client(device_t &device, const format_t &format) {
    audio_client_t audio_client;
    auto status = device->Activate(
      IID_IAudioClient,
      CLSCTX_ALL,
      nullptr,
      (void **) &audio_client
    );

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't activate Device: [0x"sv << util::hex(status).to_string_view() << ']';

      return nullptr;
    }

    WAVEFORMATEXTENSIBLE capture_waveformat =
      create_waveformat(sample_format_e::f32, format.channel_count, format.capture_waveformat_channel_mask);

    {
      wave_format_t mixer_waveformat;
      status = audio_client->GetMixFormat(&mixer_waveformat);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't get mix format for audio device: [0x"sv << util::hex(status).to_string_view() << ']';
        return nullptr;
      }

      // Prefer the native channel layout of captured audio device when channel counts match
      if (mixer_waveformat->nChannels == format.channel_count && mixer_waveformat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mixer_waveformat->cbSize >= 22) {
        auto waveformatext_pointer = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mixer_waveformat.get());
        capture_waveformat.dwChannelMask = waveformatext_pointer->dwChannelMask;
      }

      BOOST_LOG(info) << "Audio mixer format is "sv << mixer_waveformat->wBitsPerSample << "-bit, "sv
                      << mixer_waveformat->nSamplesPerSec << " Hz, "sv
                      << ((mixer_waveformat->nSamplesPerSec != 48000) ? "will be resampled to 48000 by Windows"sv : "no resampling needed"sv);
    }

    status = audio_client->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,  // Enable automatic resampling to 48 KHz
      0,
      0,
      (LPWAVEFORMATEX) &capture_waveformat,
      nullptr
    );

    if (status) {
      BOOST_LOG(error) << "Couldn't initialize audio client for ["sv << format.name << "]: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    BOOST_LOG(info) << "Audio capture format is "sv << logging::bracket(waveformat_to_pretty_string(capture_waveformat));

    return audio_client;
  }

  /**
   * @brief Query the default Windows render endpoint.
   *
   * @param device_enum Windows multimedia device enumerator.
   * @return Default render endpoint, or an empty handle if lookup fails.
   */
  device_t default_device(device_enum_t &device_enum) {
    device_t device;
    HRESULT status;
    status = device_enum->GetDefaultAudioEndpoint(
      eRender,
      eConsole,
      &device
    );

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't get default audio endpoint [0x"sv << util::hex(status).to_string_view() << ']';

      return nullptr;
    }

    return device;
  }

  /**
   * @brief Windows audio endpoint notification callback registered with MMDevice.
   */
  class audio_notification_t: public ::IMMNotificationClient {
  public:
    audio_notification_t() {
    }

    // IUnknown implementation (unused by IMMDeviceEnumerator)
    /**
     * @brief Satisfy IUnknown reference counting for the notification callback.
     *
     * @return Static reference count because the callback lifetime is externally owned.
     */
    ULONG STDMETHODCALLTYPE AddRef() {
      return 1;
    }

    /**
     * @brief Release the COM or platform reference owned by the pointer.
     *
     * @return Reference count or status returned after releasing the object.
     */
    ULONG STDMETHODCALLTYPE Release() {
      return 1;
    }

    /**
     * @brief Return the supported COM interface for the notification callback.
     *
     * @param riid COM interface identifier requested by QueryInterface.
     * @param ppvInterface Output pointer receiving the requested interface.
     * @return S_OK when the interface is supported; E_NOINTERFACE otherwise.
     */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID **ppvInterface) {
      if (IID_IUnknown == riid) {
        AddRef();
        *ppvInterface = (IUnknown *) this;
        return S_OK;
      } else if (__uuidof(IMMNotificationClient) == riid) {
        AddRef();
        *ppvInterface = (IMMNotificationClient *) this;
        return S_OK;
      } else {
        *ppvInterface = nullptr;
        return E_NOINTERFACE;
      }
    }

    // IMMNotificationClient
    /**
     * @brief Handle a Windows default-audio-device change notification.
     *
     * @param flow Audio endpoint data-flow direction.
     * @param role Audio endpoint role used for default-device lookup.
     * @param pwstrDeviceId Windows endpoint ID for the new default device.
     * @return S_OK after recording the render-device change notification.
     */
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId) {
      if (flow == eRender) {
        default_render_device_changed_flag.store(true);
      }
      return S_OK;
    }

    /**
     * @brief Ignore endpoint-add notifications.
     *
     * @param pwstrDeviceId Windows endpoint ID for the added device.
     * @return S_OK because Sunshine does not act on this notification.
     */
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) {
      return S_OK;
    }

    /**
     * @brief Ignore endpoint-removal notifications.
     *
     * @param pwstrDeviceId Windows endpoint ID for the removed device.
     * @return S_OK because Sunshine does not act on this notification.
     */
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) {
      return S_OK;
    }

    /**
     * @brief Handle Windows audio endpoint state changes.
     *
     * @param pwstrDeviceId Audio device ID.
     * @param dwNewState New device state.
     * @return COM status code.
     */
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(
      LPCWSTR pwstrDeviceId,
      DWORD dwNewState
    ) {
      return S_OK;
    }

    /**
     * @brief Handle Windows audio endpoint property changes.
     *
     * @param pwstrDeviceId Audio device ID.
     * @param key Changed property key.
     * @return COM status code.
     */
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(
      LPCWSTR pwstrDeviceId,
      const PROPERTYKEY key
    ) {
      return S_OK;
    }

    /**
     * @brief Checks if the default rendering device changed and resets the change flag
     * @return `true` if the device changed since last call
     */
    bool check_default_render_device_changed() {
      return default_render_device_changed_flag.exchange(false);
    }

  private:
    std::atomic_bool default_render_device_changed_flag;
  };

  /// Backoff window after a failed Process Loopback activation: the audio
  /// thread retries the process microphone at most this often instead of
  /// re-activating in a tight loop whenever process capture is requested but
  /// unavailable (e.g. the OS is older than Windows 10 build 20348).
  constexpr auto PROC_CAPTURE_BACKOFF = std::chrono::seconds(5);

  /// PID whose Process Loopback activation last failed, or 0 when no backoff is armed.
  static std::uint32_t proc_capture_backoff_pid = 0;

  /// Point in time until which process-capture retries are suppressed.
  static std::chrono::steady_clock::time_point proc_capture_backoff_until;

  /**
   * @brief Whether a desktop (WASAPI) microphone should rebuild itself as a
   *        process (Process Loopback) microphone.
   *
   * The /api/capture-window handler updates config::video.capture_process
   * mid-session and rebuilds video capture via the switch_capture event, but
   * audio has no equivalent event: the WASAPI microphone polls this gate from
   * its sample loop and returns capture_e::reinit so the pipeline recreates
   * the microphone through the factory. A failed activation arms a short
   * backoff to avoid a reactivation storm; selecting a different target PID
   * clears the backoff immediately.
   *
   * @return True when a process is targeted and no backoff suppresses the retry.
   */
  static bool process_capture_switch_pending() {
    const auto pid = config::video.capture_process;
    if (pid == 0) {
      return false;
    }
    if (pid != proc_capture_backoff_pid) {
      return true;
    }
    return std::chrono::steady_clock::now() >= proc_capture_backoff_until;
  }

  /**
   * @brief Arm the process-capture backoff after a failed Process Loopback activation.
   * @param pid Target PID whose activation failed.
   */
  static void arm_proc_capture_backoff(std::uint32_t pid) {
    proc_capture_backoff_pid = pid;
    proc_capture_backoff_until = std::chrono::steady_clock::now() + PROC_CAPTURE_BACKOFF;
  }

  /**
   * @brief Clear any armed process-capture backoff.
   *
   * Called when process capture starts successfully or when capture returns
   * to desktop mode, so a later target switch is acted on immediately.
   */
  static void clear_proc_capture_backoff() {
    proc_capture_backoff_pid = 0;
    proc_capture_backoff_until = {};
  }

  /**
   * @brief WASAPI microphone capture stream and endpoint notification state.
   */
  class mic_wasapi_t: public mic_t {
  public:
    /**
     * @brief Deliver a captured audio sample to Sunshine's audio pipeline.
     *
     * @param sample_out Sample out.
     * @return Capture status reported to the streaming pipeline.
     */
    capture_e sample(std::vector<float> &sample_out) override {
      // Desktop -> process switch mid-session: /api/capture-window changed
      // capture_process while this WASAPI microphone keeps looping. Ask the
      // pipeline to rebuild the microphone so the factory activates Process
      // Loopback for the target process; failed activations are throttled by
      // the gate.
      if (process_capture_switch_pending()) {
        return capture_e::reinit;
      }

      auto sample_size = sample_out.size();

      // Refill the sample buffer if needed
      while (sample_buf_pos - std::begin(sample_buf) < sample_size) {
        auto capture_result = _fill_buffer();
        if (capture_result == capture_e::timeout && continuous_audio) {
          // Write silence to sample_buf
          std::fill_n(sample_buf_pos, sample_size, 0.0f);
          sample_buf_pos += sample_size;
        } else if (capture_result != capture_e::ok) {
          return capture_result;
        }
      }

      // Fill the output buffer with samples
      std::copy_n(std::begin(sample_buf), sample_size, std::begin(sample_out));

      // Move any excess samples to the front of the buffer
      std::move(&sample_buf[sample_size], sample_buf_pos, std::begin(sample_buf));
      sample_buf_pos -= sample_size;

      return capture_e::ok;
    }

    /**
     * @brief Initialize WASAPI capture for the selected audio endpoint.
     *
     * @param sample_rate Audio sample rate in hertz.
     * @param frame_size Number of samples captured per audio frame.
     * @param channels_out Channels out.
     * @param continuous Whether silent audio should continue to be emitted.
     * @param capture_device Endpoint device to capture from; the default render device is used when empty.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(std::uint32_t sample_rate, std::uint32_t frame_size, std::uint32_t channels_out, bool continuous, device_t capture_device) {
      audio_event.reset(CreateEventA(nullptr, FALSE, FALSE, nullptr));
      if (!audio_event) {
        BOOST_LOG(error) << "Couldn't create Event handle"sv;

        return -1;
      }

      HRESULT status;

      status = CoCreateInstance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        (void **) &device_enum
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't create Device Enumerator [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      status = device_enum->RegisterEndpointNotificationCallback(&endpt_notification);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't register endpoint notification [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      select_capture_device(std::move(capture_device));

      if (!device) {
        return -1;
      }

      for (const auto &format : formats) {
        if (format.channel_count != channels_out) {
          BOOST_LOG(debug) << "Skipping audio format ["sv << format.name << "] with channel count ["sv
                           << format.channel_count << " != "sv << channels_out << ']';
          continue;
        }

        BOOST_LOG(debug) << "Trying audio format ["sv << format.name << ']';
        audio_client = make_audio_client(device, format);

        if (audio_client) {
          BOOST_LOG(debug) << "Found audio format ["sv << format.name << ']';
          channels = channels_out;
          break;
        }
      }

      if (!audio_client) {
        BOOST_LOG(error) << "Couldn't find supported format for audio"sv;
        return -1;
      }

      REFERENCE_TIME default_latency;
      audio_client->GetDevicePeriod(&default_latency, nullptr);
      default_latency_ms = default_latency / 1000;
      continuous_audio = continuous;

      std::uint32_t frames;
      status = audio_client->GetBufferSize(&frames);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't acquire the number of audio frames [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      // *2 --> needs to fit double
      sample_buf = util::buffer_t<float> {std::max(frames, frame_size) * 2 * channels_out};
      sample_buf_pos = std::begin(sample_buf);

      status = audio_client->GetService(IID_IAudioCaptureClient, (void **) &audio_capture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't initialize audio capture client [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      status = audio_client->SetEventHandle(audio_event.get());
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't set event handle [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      {
        DWORD task_index = 0;
        mmcss_task_handle = AvSetMmThreadCharacteristics("Pro Audio", &task_index);
        if (!mmcss_task_handle) {
          BOOST_LOG(error) << "Couldn't associate audio capture thread with Pro Audio MMCSS task [0x" << util::hex(GetLastError()).to_string_view() << ']';
        }
      }

      status = audio_client->Start();
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't start recording [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      return 0;
    }

    /**
     * @brief Select the endpoint used by this capture stream.
     *
     * @param capture_device Explicit endpoint to capture, or an empty pointer to follow the default endpoint.
     */
    void select_capture_device(device_t capture_device) {
      follows_default_device = !capture_device;
      if (follows_default_device) {
        device = default_device(device_enum);
      } else {
        device = std::move(capture_device);
      }
    }

    ~mic_wasapi_t() override {
      if (device_enum) {
        device_enum->UnregisterEndpointNotificationCallback(&endpt_notification);
      }

      if (audio_client) {
        audio_client->Stop();
      }

      if (mmcss_task_handle) {
        AvRevertMmThreadCharacteristics(mmcss_task_handle);
      }
    }

  private:
    capture_e _fill_buffer() {
      HRESULT status;

      // Total number of samples
      struct sample_aligned_t {
        std::uint32_t uninitialized;
        float *samples;
      } sample_aligned;

      // number of samples / number of channels
      struct block_aligned_t {
        std::uint32_t audio_sample_size;
      } block_aligned;

      // Check if the default audio device has changed
      if (endpt_notification.check_default_render_device_changed()) {
        // Invoke the audio_control_t's callback if it wants one
        if (default_endpt_changed_cb) {
          (*default_endpt_changed_cb)();
        }

        // Reinitialize to pick up the new default device, unless capture is
        // pinned to an explicitly requested sink
        if (follows_default_device) {
          return capture_e::reinit;
        }
      }

      status = WaitForSingleObjectEx(audio_event.get(), default_latency_ms, FALSE);
      switch (status) {
        case WAIT_OBJECT_0:
          break;
        case WAIT_TIMEOUT:
          return capture_e::timeout;
        default:
          BOOST_LOG(error) << "Couldn't wait for audio event: [0x"sv << util::hex(status).to_string_view() << ']';
          return capture_e::error;
      }

      std::uint32_t packet_size {};
      for (
        status = audio_capture->GetNextPacketSize(&packet_size);
        SUCCEEDED(status) && packet_size > 0;
        status = audio_capture->GetNextPacketSize(&packet_size)) {
        DWORD buffer_flags;
        status = audio_capture->GetBuffer(
          (BYTE **) &sample_aligned.samples,
          &block_aligned.audio_sample_size,
          &buffer_flags,
          nullptr,
          nullptr
        );

        switch (status) {
          case S_OK:
            break;
          case AUDCLNT_E_DEVICE_INVALIDATED:
            return capture_e::reinit;
          default:
            BOOST_LOG(error) << "Couldn't capture audio [0x"sv << util::hex(status).to_string_view() << ']';
            return capture_e::error;
        }

        if (buffer_flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
          BOOST_LOG(debug) << "Audio capture signaled buffer discontinuity";
        }

        sample_aligned.uninitialized = std::end(sample_buf) - sample_buf_pos;
        auto n = std::min(sample_aligned.uninitialized, block_aligned.audio_sample_size * channels);

        if (n < block_aligned.audio_sample_size * channels) {
          BOOST_LOG(warning) << "Audio capture buffer overflow";
        }

        if (buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          std::fill_n(sample_buf_pos, n, 0);
        } else {
          std::copy_n(sample_aligned.samples, n, sample_buf_pos);
        }

        sample_buf_pos += n;

        audio_capture->ReleaseBuffer(block_aligned.audio_sample_size);
      }

      if (status == AUDCLNT_E_DEVICE_INVALIDATED) {
        return capture_e::reinit;
      }

      if (FAILED(status)) {
        return capture_e::error;
      }

      return capture_e::ok;
    }

  public:
    handle_t audio_event;  ///< Event signaled by WASAPI when captured audio is available.

    device_enum_t device_enum;  ///< Device enum.
    device_t device;  ///< WASAPI endpoint device selected for capture.
    audio_client_t audio_client;  ///< WASAPI audio client configured for shared-mode capture.
    audio_capture_t audio_capture;  ///< WASAPI capture client used to read sample packets.

    audio_notification_t endpt_notification;  ///< Endpoint notification callback registered with Windows.
    std::optional<std::function<void()>> default_endpt_changed_cb;  ///< Callback invoked when the default endpoint changes.

    REFERENCE_TIME default_latency_ms;  ///< WASAPI default device period used as capture latency.

    util::buffer_t<float> sample_buf;  ///< Floating-point sample buffer filled from WASAPI packets.
    float *sample_buf_pos;  ///< Current write position in `sample_buf`.
    int channels;  ///< Number of channels in the capture format.
    bool continuous_audio;  ///< Whether audio packets continue during silence.
    bool follows_default_device;  ///< Whether capture follows the default render device rather than an explicit sink.

    HANDLE mmcss_task_handle = nullptr;  ///< MMCSS task handle for the audio capture thread.
  };

  /**
   * @brief Completion handler for ActivateAudioInterfaceAsync().
   *
   * The async activation API invokes ActivateCompleted() on an MTA thread once
   * the virtual audio interface (IAudioClient) is ready; the handler stores the
   * operation and signals an event so the capture thread can collect the result.
   */
  class activate_completion_handler_t: public IActivateAudioInterfaceCompletionHandler {
  public:
    /**
     * @brief Construct the handler.
     * @param activated_event Event signalled from ActivateCompleted().
     */
    explicit activate_completion_handler_t(HANDLE activated_event):
        ref_count(1), activated_event(activated_event), operation(nullptr) {
    }

    /// Virtual destructor (object is deleted through Release()).
    virtual ~activate_completion_handler_t() {
      if (operation) {
        operation->Release();
      }
    }

    /// @cond DOXYGEN_SHOULD_SKIP_THIS
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
      if (!object) {
        return E_POINTER;
      }
      // ActivateAudioInterfaceAsync marshals the completion handler back to an
      // MTA thread and requires IAgileObject (the Free-Threaded Marshaler is
      // not queried for this path); without it activation fails with
      // E_ILLEGAL_METHOD_CALL.
      if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
          riid == IID_IAgileObject_local) {
        *object = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
        AddRef();
        return S_OK;
      }
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return InterlockedIncrement(&ref_count);
    }

    ULONG STDMETHODCALLTYPE Release() override {
      auto count = InterlockedDecrement(&ref_count);
      if (count == 0) {
        delete this;
      }
      return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *async_operation) override {
      operation = async_operation;
      operation->AddRef();
      SetEvent(activated_event);
      return S_OK;
    }
    /// @endcond

    /**
     * @brief Retrieve the activated interface once completion has fired.
     * @param[out] activated Receives the resulting IUnknown (caller releases it).
     * @return The activation HRESULT reported by the operation, or E_UNEXPECTED
     *         when ActivateCompleted() has not run yet.
     */
    HRESULT result(IUnknown **activated) {
      if (!operation) {
        return E_UNEXPECTED;
      }
      HRESULT activation_hr = E_FAIL;
      auto hr = operation->GetActivateResult(&activation_hr, activated);
      if (FAILED(hr)) {
        return hr;
      }
      return activation_hr;
    }

  private:
    LONG ref_count;  ///< COM reference count.
    HANDLE activated_event;  ///< Signalled when the async operation completes.
    IActivateAudioInterfaceAsyncOperation *operation;  ///< Completed operation, if any.
  };

  /**
   * @brief Per-process audio capture backed by in-process WASAPI Process Loopback.
   *
   * Activates an IAudioClient against VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK for
   * the target PID (Windows 10 build 20348+), so only that process' render
   * output is captured - no helper executable or pipes.  Captured frames are
   * normalised to interleaved stereo float32 and upmixed into the channel
   * layout requested by the client (2, 6, or 8 channels).
   *
   * @note The stream is always normalised to 48 kHz; the @p sample_rate
   *       parameter in init() is accepted for interface compatibility but a
   *       mismatch is logged and resampled.
   */
  class mic_proc_t: public mic_t {
  public:
    std::jthread worker;  ///< Thread running WASAPI activation and the capture loop.
    std::mutex mutex;   ///< Guards sample_buf and stream_ended.
    std::condition_variable cv;  ///< Signalled when sample_buf gains or loses data.
    std::vector<float> sample_buf;  ///< Accumulated interleaved stereo float32 frames.
    std::size_t max_buf {SAMPLE_RATE * 2 * 2};  ///< Maximum sample_buf capacity in floats (2 s stereo).
    std::uint32_t frame_size {0};  ///< Number of stereo frames consumed per sample() call.
    std::uint32_t channels {0};  ///< Output channel count (2, 6, or 8).
    std::uint32_t process_id {0};  ///< Target process ID captured by WASAPI.
    bool continuous_audio {false};  ///< When true, emit silence on timeout or stream end instead of an error.
    bool stream_ended {false};  ///< Set when the capture thread stops or fails to start.
    std::chrono::milliseconds sample_timeout {50};  ///< Maximum wait in sample() before returning timeout or silence.
    HANDLE cancel_event {nullptr};  ///< Manual-reset event waking the capture thread for shutdown.
    HANDLE activated_event {nullptr};  ///< Signalled by the worker once activate_status is final.
    HANDLE async_done_event {nullptr};  ///< Signalled by the activation completion handler; worker-only.
    HRESULT activate_status {E_FAIL};  ///< Activation result published by the capture thread.

    /**
     * @brief Default-construct an uninitialised process capture microphone.
     */
    mic_proc_t() = default;

    /**
     * @brief Activate WASAPI Process Loopback for the target process.
     *
     * Starts the capture thread, which asynchronously activates the process
     * loopback audio client; this call blocks until activation finishes (or
     * fails) so the caller can fall back to desktop loopback on error.
     *
     * @param target_process_id PID of the process whose audio stream should be captured.
     * @param sample_rate Audio sample rate in Hz (the stream is normalised to 48 000).
     * @param target_frame_size Number of stereo frames per sample() call.
     * @param output_channels Output channel count (2, 6, or 8).
     * @param continuous Flag indicating whether silence should fill gaps instead of errors.
     * @return 0 on success; -1 on failure.
     */
    int init(std::uint32_t target_process_id, std::uint32_t sample_rate, std::uint32_t target_frame_size, std::uint32_t output_channels, bool continuous) {
      if (output_channels != 2 && output_channels != 6 && output_channels != 8) {
        BOOST_LOG(error) << "Unsupported channel count for process audio capture: "sv << output_channels;
        return -1;
      }

      if (sample_rate != SAMPLE_RATE) {
        BOOST_LOG(warning) << "Process audio capture is normalised to "sv << SAMPLE_RATE
                           << " Hz; requested "sv << sample_rate << " Hz will be resampled"sv;
      }

      process_id = target_process_id;
      frame_size = target_frame_size;
      channels = output_channels;
      continuous_audio = continuous;
      stream_ended = false;
      activate_status = E_FAIL;

      cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      activated_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      async_done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!cancel_event || !activated_event || !async_done_event) {
        BOOST_LOG(error) << "Couldn't create process audio capture events"sv;
        return -1;
      }

      worker = std::jthread([this](std::stop_token token) { capture_loop(token); });

      // activated_event is only raised by the worker after activate_status has
      // been published; the completion handler must never signal it directly,
      // otherwise this wait would race ahead and read the initial E_FAIL.
      HANDLE wait_handles[] {activated_event, cancel_event};
      auto wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, 10000);
      if (wait_result != WAIT_OBJECT_0 || FAILED(activate_status)) {
        BOOST_LOG(error) << "Couldn't activate WASAPI process audio capture for pid ["sv << process_id
                         << "]: [0x"sv << util::hex(static_cast<std::uint32_t>(activate_status)).to_string_view()
                         << ']';
        if (worker.joinable()) {
          SetEvent(cancel_event);
          worker.request_stop();
          worker.join();
        }
        return -1;
      }

      return 0;
    }

    /**
     * @brief Upmix interleaved stereo frames to the requested channel layout.
     *
     * The stereo pair is written to the front-left and front-right speakers.
     * For 5.1 and 7.1 layouts the centre channel receives the mono sum and
     * all remaining channels are silent.
     *
     * @param stereo Pointer to interleaved L/R sample pairs.
     * @param frames Number of stereo frames to convert.
     * @param ch Target channel count (2, 6, or 8).
     * @param out Destination buffer large enough for @p frames * @p ch floats.
     */
    static void convert(const float *stereo, std::size_t frames, std::uint32_t ch, float *out) {
      for (std::size_t i = 0; i < frames; ++i) {
        const auto left = stereo[i * 2];
        const auto right = stereo[i * 2 + 1];
        float *o = out + i * ch;
        switch (ch) {
          case 2:
            o[0] = left;
            o[1] = right;
            break;
          case 6:
            o[0] = left;
            o[1] = right;
            o[2] = (left + right) * 0.5f;
            o[3] = 0.0f;
            o[4] = 0.0f;
            o[5] = 0.0f;
            break;
          case 8:
            o[0] = left;
            o[1] = right;
            o[2] = (left + right) * 0.5f;
            o[3] = 0.0f;
            o[4] = 0.0f;
            o[5] = 0.0f;
            o[6] = 0.0f;
            o[7] = 0.0f;
            break;
          default:
            break;
        }
      }
    }

    /**
     * @brief Deliver a captured audio sample to Sunshine's audio pipeline.
     *
     * Blocks until frame_size stereo frames are available, the capture
     * stream ends, or sample_timeout elapses.  The stereo frames are upmixed
     * to the output channel count before being written to @p sample_out.
     *
     * @param sample_out Destination buffer; must be sized to frame_size * channels.
     * @return capture_e::ok on success, timeout when data is unavailable,
     *         or error when the capture stream has ended.
     */
    capture_e sample(std::vector<float> &sample_out) override {
      // If the user targets a different process mid-session, ask the pipeline to rebuild
      // the microphone so a new Process Loopback client is activated.
      if (config::video.capture_process != process_id) {
        return capture_e::reinit;
      }

      const auto needed = static_cast<std::size_t>(frame_size) * 2;
      std::unique_lock lock(mutex);

      auto ready = [&] { return sample_buf.size() >= needed || stream_ended; };
      if (!cv.wait_for(lock, sample_timeout, ready)) {
        if (continuous_audio) {
          std::fill(sample_out.begin(), sample_out.end(), 0.0f);
          return capture_e::ok;
        }
        return capture_e::timeout;
      }

      if (sample_buf.size() < needed) {
        if (continuous_audio) {
          std::fill(sample_out.begin(), sample_out.end(), 0.0f);
          return capture_e::ok;
        }
        return capture_e::error;
      }

      convert(sample_buf.data(), frame_size, channels, sample_out.data());
      sample_buf.erase(sample_buf.begin(), sample_buf.begin() + needed);
      return capture_e::ok;
    }

    /**
     * @brief Stop capture and join the WASAPI capture thread, closing events.
     */
    ~mic_proc_t() override {
      if (cancel_event) {
        SetEvent(cancel_event);
      }
      cv.notify_all();
      if (worker.joinable()) {
        worker.request_stop();
        worker.join();
      }
      if (cancel_event) {
        CloseHandle(cancel_event);
      }
      if (activated_event) {
        CloseHandle(activated_event);
      }
      if (async_done_event) {
        CloseHandle(async_done_event);
      }
    }

  private:
    /**
     * @brief Activate the Process Loopback audio client and pump packets.
     *
     * Runs on the worker thread with its own MTA COM initialisation (required
     * by ActivateAudioInterfaceAsync).  Publishes activate_status and signals
     * activated_event once activation succeeds or fails, then loops over
     * captured packets until shutdown.  Any failure marks stream_ended so
     * sample() reports silence or an error depending on continuous_audio.
     *
     * @param token Stop token used to request an early exit.
     */
    void capture_loop(std::stop_token token) {
      co_init_t com;

      // Schedule the capture thread with the same Pro Audio MMCSS task as the
      // desktop loopback path to keep activation and packet pumping responsive.
      DWORD mmcss_task_index = 0;
      auto *mmcss_task = AvSetMmThreadCharacteristics("Pro Audio", &mmcss_task_index);
      if (!mmcss_task) {
        BOOST_LOG(warning) << "Couldn't associate process audio capture thread with Pro Audio MMCSS task [0x"
                           << util::hex(GetLastError()).to_string_view() << ']';
      }
      auto mmcss_cleanup = util::fail_guard([&mmcss_task]() {
        if (mmcss_task) {
          AvRevertMmThreadCharacteristics(mmcss_task);
        }
      });

      audio_client_t client;
      audio_capture_t capture;
      handle_t data_event;

      HRESULT status = E_FAIL;
      do {
        auto *handler = new activate_completion_handler_t(async_done_event);

        AUDIOCLIENT_ACTIVATION_PARAMS activation_params {};
        activation_params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        activation_params.loopback_union.ProcessLoopbackParams.TargetProcessId = process_id;
        activation_params.loopback_union.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        // The async activation entry point takes a PROPVARIANT; process loopback
        // parameters are passed as a VT_BLOB carrying the activation struct.
        PROPVARIANT activation_property {};
        PropVariantInit(&activation_property);
        activation_property.vt = VT_BLOB;
        activation_property.blob.cbSize = sizeof(activation_params);
        activation_property.blob.pBlobData = reinterpret_cast<BYTE *>(&activation_params);

        IActivateAudioInterfaceAsyncOperation *operation = nullptr;
        status = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                             __uuidof(IAudioClient), &activation_property, handler, &operation);
        // No PropVariantClear: the blob points at stack memory and must not be freed.
        if (operation) {
          operation->Release();
        }
        if (FAILED(status)) {
          handler->Release();
          break;
        }

        HANDLE activation_handles[] {async_done_event, cancel_event};
        auto activation_wait = WaitForMultipleObjects(2, activation_handles, FALSE, 10000);
        if (activation_wait != WAIT_OBJECT_0) {
          handler->Release();
          status = E_ABORT;
          break;
        }

        IUnknown *activated = nullptr;
        status = handler->result(&activated);
        handler->Release();
        if (FAILED(status) || !activated) {
          break;
        }
        status = activated->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&client));
        activated->Release();
        if (FAILED(status)) {
          break;
        }

        // Prefer 48 kHz stereo float32 (the pipeline's contract); fall back to
        // the endpoint mix format and normalise in append_frames() if rejected.
        auto wanted = create_waveformat(sample_format_e::f32, 2, waveformat_mask_stereo);
        const WAVEFORMATEX *format = &wanted.Format;
        wave_format_t mix_format;

        status = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    1'000'000, 0, format, nullptr);
        if (status == AUDCLNT_E_UNSUPPORTED_FORMAT || status == E_INVALIDARG) {
          status = client->GetMixFormat(&mix_format);
          if (SUCCEEDED(status)) {
            format = mix_format.get();
            status = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        1'000'000, 0, format, nullptr);
          }
        }
        if (FAILED(status)) {
          break;
        }

        data_event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!data_event) {
          status = HRESULT_FROM_WIN32(GetLastError());
          break;
        }

        status = client->SetEventHandle(data_event.get());
        if (FAILED(status)) {
          break;
        }
        status = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&capture));
        if (FAILED(status)) {
          break;
        }
        status = client->Start();
        if (FAILED(status)) {
          break;
        }

        activate_status = S_OK;
        SetEvent(activated_event);

        pump_capture(capture.get(), format, data_event.get(), token);

        client->Stop();
        status = S_OK;
      } while (false);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Process audio capture failed for pid ["sv << process_id << "]: [0x"sv
                         << util::hex(static_cast<std::uint32_t>(status)).to_string_view() << ']';
        activate_status = status;
        SetEvent(activated_event);
      }

      {
        std::lock_guard lock(mutex);
        stream_ended = true;
      }
      cv.notify_all();
    }

    /**
     * @brief Drain captured WASAPI packets into sample_buf until shutdown.
     *
     * @param capture Active IAudioCaptureClient.
     * @param format Format of the captured packets (must be float32).
     * @param data_event Event signalled by WASAPI when packets are available.
     * @param token Stop token used to request an early exit.
     */
    void pump_capture(IAudioCaptureClient *capture, const WAVEFORMATEX *format, HANDLE data_event,
                      std::stop_token token) {
      const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
      const auto is_float =
          format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
          (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
      if (!is_float || format->wBitsPerSample != 32) {
        BOOST_LOG(error) << "Process audio capture needs float32 samples; capture stream unsupported"sv;
        return;
      }

      const auto source_channels = format->nChannels;
      const auto source_rate = format->nSamplesPerSec;

      HANDLE wait_handles[] {data_event, cancel_event};
      while (!token.stop_requested()) {
        auto wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, 200);
        if (wait_result == WAIT_OBJECT_0 + 1) {
          break;
        }

        UINT32 packet_length = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet_length)) && packet_length > 0) {
          BYTE *data = nullptr;
          UINT32 frames = 0;
          DWORD flags = 0;
          if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
            return;
          }

          if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            append_silence(frames, source_rate);
          } else {
            append_frames(reinterpret_cast<const float *>(data), frames, source_channels, source_rate);
          }

          capture->ReleaseBuffer(frames);
        }
      }
    }

    /**
     * @brief Downmix captured float frames to stereo and append them.
     *
     * Windows channel ordering places front-left/front-right first, so for
     * multi-channel streams only the first two channels are used; mono is
     * duplicated.  A non-48 kHz source is linearly resampled.
     *
     * @param input Interleaved float samples from IAudioCaptureClient.
     * @param frames Number of source frames in @p input.
     * @param source_channels Channel count of the captured stream.
     * @param source_rate Sample rate of the captured stream.
     */
    void append_frames(const float *input, UINT32 frames, WORD source_channels, DWORD source_rate) {
      std::vector<float> stereo;
      stereo.reserve(static_cast<std::size_t>(frames) * 2);
      for (UINT32 frame = 0; frame < frames; ++frame) {
        auto left = input[static_cast<std::size_t>(frame) * source_channels];
        auto right = source_channels > 1 ? input[static_cast<std::size_t>(frame) * source_channels + 1] : left;
        stereo.push_back(left);
        stereo.push_back(right);
      }

      if (source_rate != SAMPLE_RATE) {
        stereo = resample_stereo(stereo, source_rate);
      }
      push_samples(stereo);
    }

    /**
     * @brief Append silence for a silent WASAPI packet.
     * @param frames Number of source frames of silence.
     * @param source_rate Source rate used to size the resampled output.
     */
    void append_silence(UINT32 frames, DWORD source_rate) {
      auto out_frames = static_cast<std::size_t>(frames);
      if (source_rate != SAMPLE_RATE) {
        out_frames = static_cast<std::size_t>(static_cast<double>(frames) * SAMPLE_RATE / source_rate);
      }
      std::vector<float> silence(out_frames * 2, 0.0f);
      push_samples(silence);
    }

    /**
     * @brief Linearly resample interleaved stereo frames to 48 kHz.
     *
     * Only used when the endpoint mix format rejects 48 kHz directly; the
     * default mix format is 48 kHz on virtually all systems.
     *
     * @param input Interleaved stereo frames at @p source_rate.
     * @param source_rate Source sample rate in Hz.
     * @return Resampled interleaved stereo frames.
     */
    static std::vector<float> resample_stereo(const std::vector<float> &input, DWORD source_rate) {
      auto input_frames = input.size() / 2;
      auto output_frames =
          static_cast<std::size_t>(static_cast<double>(input_frames) * SAMPLE_RATE / source_rate);
      std::vector<float> output(output_frames * 2);
      for (std::size_t out = 0; out < output_frames; ++out) {
        auto position = static_cast<double>(out) * source_rate / SAMPLE_RATE;
        auto index = static_cast<std::size_t>(position);
        auto fraction = static_cast<float>(position - static_cast<double>(index));
        auto next = index + 1 < input_frames ? index + 1 : index;
        output[out * 2] = input[index * 2] + (input[next * 2] - input[index * 2]) * fraction;
        output[out * 2 + 1] =
            input[index * 2 + 1] + (input[next * 2 + 1] - input[index * 2 + 1]) * fraction;
      }
      return output;
    }

    /**
     * @brief Append stereo frames to the shared buffer, capping its length.
     * @param samples Interleaved stereo floats to append.
     */
    void push_samples(const std::vector<float> &samples) {
      std::lock_guard lock(mutex);
      sample_buf.insert(sample_buf.end(), samples.begin(), samples.end());
      if (sample_buf.size() > max_buf) {
        sample_buf.erase(sample_buf.begin(),
                         sample_buf.begin() + static_cast<std::ptrdiff_t>(sample_buf.size() - max_buf));
      }
      cv.notify_all();
    }
  };

  /**
   * @brief Platform audio controller that manages sinks and microphone capture.
   */
  class audio_control_t: public ::platf::audio_control_t {
  public:
    /**
     * @brief Query host and virtual sink names available to Sunshine.
     *
     * @return Host and virtual sink names when the backend can report them.
     */
    std::optional<sink_t> sink_info() override {
      sink_t sink;

      // Fill host sink name with the device_id of the current default audio device.
      {
        auto device = default_device(device_enum);
        if (!device) {
          return std::nullopt;
        }

        audio::wstring_t id;
        device->GetId(&id);

        sink.host = utf_utils::to_utf8(id.get());
      }

      // Prepare to search for the device_id of the virtual audio sink device,
      // this device can be either user-configured or
      // the Steam Streaming Speakers we use by default.
      match_fields_list_t match_list;
      if (config::audio.virtual_sink.empty()) {
        match_list = match_steam_speakers();
      } else {
        match_list = match_all_fields(utf_utils::from_utf8(config::audio.virtual_sink));
      }

      // Search for the virtual audio sink device currently present in the system.
      auto matched = find_device_id(match_list);
      if (matched) {
        // Prepare to fill virtual audio sink names with device_id.
        auto device_id = utf_utils::to_utf8(matched->second);
        // Also prepend format name (basically channel layout at the moment)
        // because we don't want to extend the platform interface.
        sink.null = std::make_optional(sink_t::null_t {
          "virtual-"s + formats[0].name + device_id,
          "virtual-"s + formats[1].name + device_id,
          "virtual-"s + formats[2].name + device_id,
        });
      } else if (!config::audio.virtual_sink.empty()) {
        BOOST_LOG(warning) << "Couldn't find the specified virtual audio sink " << config::audio.virtual_sink;
      }

      return sink;
    }

    bool is_sink_available(const std::string &sink) override {
      const auto match_list = match_all_fields(utf_utils::from_utf8(sink));
      const auto matched = find_device_id(match_list);
      return static_cast<bool>(matched);
    }

    /**
     * @brief Extract virtual audio sink information possibly encoded in the sink name.
     * @param sink The sink name
     * @return A pair of device_id and format reference if the sink name matches
     *         our naming scheme for virtual audio sinks, `std::nullopt` otherwise.
     */
    std::optional<std::pair<std::wstring, std::reference_wrapper<const format_t>>> extract_virtual_sink_info(const std::string &sink) {
      // Encoding format:
      // [virtual-(format name)]device_id
      std::string current = sink;
      auto prefix = "virtual-"sv;
      if (current.find(prefix) == 0) {
        current = current.substr(prefix.size(), current.size() - prefix.size());

        for (const auto &format : formats) {
          auto &name = format.name;
          if (current.find(name) == 0) {
            auto device_id = utf_utils::from_utf8(current.substr(name.size(), current.size() - name.size()));
            return std::make_pair(device_id, std::reference_wrapper(format));
          }
        }
      }

      return std::nullopt;
    }

    /**
     * @brief Resolve a sink name to the audio endpoint device it refers to.
     *
     * @param sink Sink name, virtual sink descriptor, or device identifier.
     * @return Endpoint device to capture from, or an empty pointer if the sink couldn't be resolved.
     */
    device_t get_sink_device(const std::string &sink) {
      std::wstring device_id;
      if (auto virtual_sink_info = extract_virtual_sink_info(sink)) {
        device_id = virtual_sink_info->first;
      } else if (auto matched = find_device_id(match_all_fields(utf_utils::from_utf8(sink)))) {
        device_id = matched->second;
      } else {
        return nullptr;
      }

      device_t device;
      if (FAILED(device_enum->GetDevice(device_id.c_str(), &device))) {
        return nullptr;
      }

      if (DWORD device_state {}; FAILED(device->GetState(&device_state)) || device_state != DEVICE_STATE_ACTIVE) {
        return nullptr;
      }

      return device;
    }

    /**
     * @brief Create a microphone capture stream for the requested layout.
     *
     * @param mapping Opus channel mapping table for the requested layout.
     * @param channels Number of audio channels in the stream.
     * @param sample_rate Audio sample rate in hertz.
     * @param frame_size Number of samples captured per audio frame.
     * @param continuous_audio Continuous audio.
     * @param host_audio_enabled Whether host playback should remain enabled during capture.
     * @return Microphone capture object for the requested audio layout.
     */
    std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio, [[maybe_unused]] bool host_audio_enabled) override {
      // When a specific process is targeted, prefer in-process WASAPI Process
      // Loopback so only that process' render output is streamed.
      if (config::video.capture_process != 0) {
        auto target_pid = config::video.capture_process;
        auto proc_mic = std::make_unique<mic_proc_t>();
        if (proc_mic->init(target_pid, sample_rate, frame_size, channels, continuous_audio) == 0) {
          clear_proc_capture_backoff();
          BOOST_LOG(info) << "Capturing audio from process ["sv << target_pid << ']';
          return proc_mic;
        }
        // Stay on desktop loopback for now; the WASAPI microphone re-checks
        // the gate after the backoff instead of re-activating in a loop.
        arm_proc_capture_backoff(target_pid);
        BOOST_LOG(warning) << "Couldn't start Process Loopback capture; falling back to desktop WASAPI"sv;
      } else {
        clear_proc_capture_backoff();
      }

      auto mic = std::make_unique<mic_wasapi_t>();

      // Prefer the sink that was assigned to this capture session since it accounts
      // for the priority between virtual and configured sinks.
      const auto &requested_sink = assigned_sink.empty() ? config::audio.sink : assigned_sink;

      // Capture the requested sink directly instead of relying on it being the default
      // render device, so that capture keeps working when the default device differs
      // from the sink or changes during the session.
      device_t capture_device;
      if (!requested_sink.empty()) {
        capture_device = get_sink_device(requested_sink);
        if (!capture_device) {
          BOOST_LOG(error) << "Couldn't resolve audio sink ["sv << requested_sink << "] to a capture device"sv;
          return nullptr;
        }

        BOOST_LOG(info) << "Capturing audio from sink ["sv << requested_sink << ']';
      }

      if (mic->init(sample_rate, frame_size, channels, continuous_audio, std::move(capture_device))) {
        return nullptr;
      }

      // If this is a virtual sink, set a callback that will change the sink back if it's changed
      auto virtual_sink_info = extract_virtual_sink_info(assigned_sink);
      if (virtual_sink_info) {
        mic->default_endpt_changed_cb = [this] {
          BOOST_LOG(info) << "Resetting sink to ["sv << assigned_sink << "] after default changed";
          set_sink(assigned_sink);
        };
      }

      return mic;
    }

    /**
     * If the requested sink is a virtual sink, meaning no speakers attached to
     * the host, then we can seamlessly set the format to stereo and surround sound.
     *
     * Any virtual sink detected will be prefixed by:
     *    virtual-(format name)
     * If it doesn't contain that prefix, then the format will not be changed
     * @param sink Audio sink name to route or capture.
     * @return Status from updating format.
     */
    std::optional<std::wstring> set_format(const std::string &sink) {
      if (sink.empty()) {
        return std::nullopt;
      }

      auto virtual_sink_info = extract_virtual_sink_info(sink);

      if (!virtual_sink_info) {
        // Sink name does not begin with virtual-(format name), hence it's not a virtual sink
        // and we don't want to change playback format of the corresponding device.
        // Also need to perform matching, sink name is not necessarily device_id in this case.
        auto matched = find_device_id(match_all_fields(utf_utils::from_utf8(sink)));
        if (matched) {
          return matched->second;
        } else {
          BOOST_LOG(error) << "Couldn't find audio sink " << sink;
          return std::nullopt;
        }
      }

      // When switching to a Steam virtual speaker device, try to retain the bit depth of the
      // default audio device. Switching from a 16-bit device to a 24-bit one has been known to
      // cause glitches for some users.
      int wanted_bits_per_sample = 32;
      auto current_default_dev = default_device(device_enum);
      if (current_default_dev) {
        audio::prop_t prop;
        prop_var_t current_device_format;

        if (SUCCEEDED(current_default_dev->OpenPropertyStore(STGM_READ, &prop)) && SUCCEEDED(prop->GetValue(PKEY_AudioEngine_DeviceFormat, &current_device_format.prop))) {
          auto *format = (WAVEFORMATEXTENSIBLE *) current_device_format.prop.blob.pBlobData;
          wanted_bits_per_sample = format->Samples.wValidBitsPerSample;
          BOOST_LOG(info) << "Virtual audio device will use "sv << wanted_bits_per_sample << "-bit to match default device"sv;
        }
      }

      auto &device_id = virtual_sink_info->first;
      auto &waveformats = virtual_sink_info->second.get().virtual_sink_waveformats;
      for (const auto &waveformat : waveformats) {
        // We're using completely undocumented and unlisted API,
        // better not pass objects without copying them first.
        auto device_id_copy = device_id;
        auto waveformat_copy = waveformat;
        auto waveformat_copy_pointer = reinterpret_cast<WAVEFORMATEX *>(&waveformat_copy);

        if (wanted_bits_per_sample != waveformat.Samples.wValidBitsPerSample) {
          continue;
        }

        WAVEFORMATEXTENSIBLE p {};
        if (SUCCEEDED(policy->SetDeviceFormat(device_id_copy.c_str(), waveformat_copy_pointer, (WAVEFORMATEX *) &p))) {
          BOOST_LOG(info) << "Changed virtual audio sink format to " << logging::bracket(waveformat_to_pretty_string(waveformat));
          return device_id;
        }
      }

      BOOST_LOG(error) << "Couldn't set virtual audio sink waveformat";
      return std::nullopt;
    }

    /**
     * @brief Update the sink value on the backend.
     *
     * @param sink Audio sink name to route or capture.
     * @return Status from updating sink.
     */
    int set_sink(const std::string &sink) override {
      auto device_id = set_format(sink);
      if (!device_id) {
        return -1;
      }

      int failure {};
      for (int x = 0; x < (int) ERole_enum_count; ++x) {
        auto status = policy->SetDefaultEndpoint(device_id->c_str(), (ERole) x);
        if (status) {
          // Depending on the format of the string, we could get either of these errors
          if (status == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || status == E_INVALIDARG) {
            BOOST_LOG(warning) << "Audio sink not found: "sv << sink;
          } else {
            BOOST_LOG(warning) << "Couldn't set ["sv << sink << "] to role ["sv << x << "]: 0x"sv << util::hex(status).to_string_view();
          }

          ++failure;
        }
      }

      // Remember the assigned sink name, so we have it for later if we need to set it
      // back after another application changes it
      if (!failure) {
        assigned_sink = sink;
      }

      return failure;
    }

    /**
     * @brief Enumerates supported match field options.
     */
    enum class match_field_e {
      device_id,  ///< Match device_id
      device_friendly_name,  ///< Match endpoint friendly name
      adapter_friendly_name,  ///< Match adapter friendly name
      device_description,  ///< Match endpoint description
    };

    /**
     * @brief List of format fields used to compare audio formats.
     */
    using match_fields_list_t = std::vector<std::pair<match_field_e, std::wstring>>;
    /**
     * @brief One matched audio-format field and its expected value.
     */
    using matched_field_t = std::pair<match_field_e, std::wstring>;

    /**
     * @brief Build matching fields for Steam Streaming Speakers.
     *
     * @return Field list used to identify Steam's virtual speaker endpoint.
     */
    audio_control_t::match_fields_list_t match_steam_speakers() {
      return {
        {match_field_e::adapter_friendly_name, L"Steam Streaming Speakers"}
      };
    }

    /**
     * @brief Build matching fields that all contain the same endpoint name.
     *
     * @param name Endpoint name or identifier to match across all fields.
     * @return Field list requiring every supported endpoint field to match the name.
     */
    audio_control_t::match_fields_list_t match_all_fields(const std::wstring &name) {
      return {
        {match_field_e::device_id, name},  // {0.0.0.00000000}.{29dd7668-45b2-4846-882d-950f55bf7eb8}
        {match_field_e::device_friendly_name, name},  // Digital Audio (S/PDIF) (High Definition Audio Device)
        {match_field_e::device_description, name},  // Digital Audio (S/PDIF)
        {match_field_e::adapter_friendly_name, name},  // High Definition Audio Device
      };
    }

    /**
     * @brief Search for currently present audio device_id using multiple match fields.
     * @param match_list Pairs of match fields and values
     * @return Optional pair of matched field and device_id
     */
    std::optional<matched_field_t> find_device_id(const match_fields_list_t &match_list) {
      if (match_list.empty()) {
        return std::nullopt;
      }

      collection_t collection;
      auto status = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't enumerate: [0x"sv << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      UINT count = 0;
      collection->GetCount(&count);

      std::vector<std::wstring> matched(match_list.size());
      for (auto x = 0; x < count; ++x) {
        audio::device_t device;
        collection->Item(x, &device);

        audio::wstring_t wstring_id;
        device->GetId(&wstring_id);
        std::wstring device_id = wstring_id.get();

        audio::prop_t prop;
        device->OpenPropertyStore(STGM_READ, &prop);

        prop_var_t adapter_friendly_name;
        prop_var_t device_friendly_name;
        prop_var_t device_desc;

        prop->GetValue(PKEY_Device_FriendlyName, &device_friendly_name.prop);
        prop->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter_friendly_name.prop);
        prop->GetValue(PKEY_Device_DeviceDesc, &device_desc.prop);

        for (size_t i = 0; i < match_list.size(); i++) {
          if (matched[i].empty()) {
            const wchar_t *match_value = nullptr;
            switch (match_list[i].first) {
              case match_field_e::device_id:
                match_value = device_id.c_str();
                break;

              case match_field_e::device_friendly_name:
                match_value = device_friendly_name.prop.pwszVal;
                break;

              case match_field_e::adapter_friendly_name:
                match_value = adapter_friendly_name.prop.pwszVal;
                break;

              case match_field_e::device_description:
                match_value = device_desc.prop.pwszVal;
                break;
            }
            if (match_value && std::wcscmp(match_value, match_list[i].second.c_str()) == 0) {
              matched[i] = device_id;
            }
          }
        }
      }

      for (size_t i = 0; i < match_list.size(); i++) {
        if (!matched[i].empty()) {
          return matched_field_t(match_list[i].first, matched[i]);
        }
      }

      return std::nullopt;
    }

    /**
     * @brief Resets the default audio device from Steam Streaming Speakers.
     */
    void reset_default_device() {
      auto matched_steam = find_device_id(match_steam_speakers());
      if (!matched_steam) {
        return;
      }
      auto steam_device_id = matched_steam->second;

      {
        // Get the current default audio device (if present)
        auto current_default_dev = default_device(device_enum);
        if (!current_default_dev) {
          return;
        }

        audio::wstring_t current_default_id;
        current_default_dev->GetId(&current_default_id);

        // If Steam Streaming Speakers are already not default, we're done.
        if (steam_device_id != current_default_id.get()) {
          return;
        }
      }

      // Disable the Steam Streaming Speakers temporarily to allow the OS to pick a new default.
      auto hr = policy->SetEndpointVisibility(steam_device_id.c_str(), FALSE);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "Failed to disable Steam audio device: "sv << util::hex(hr).to_string_view();
        return;
      }

      // Get the newly selected default audio device
      auto new_default_dev = default_device(device_enum);

      // Enable the Steam Streaming Speakers again
      hr = policy->SetEndpointVisibility(steam_device_id.c_str(), TRUE);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "Failed to enable Steam audio device: "sv << util::hex(hr).to_string_view();
        return;
      }

      // If there's now no audio device, the Steam Streaming Speakers were the only device available.
      // There's no other device to set as the default, so just return.
      if (!new_default_dev) {
        return;
      }

      audio::wstring_t new_default_id;
      new_default_dev->GetId(&new_default_id);

      // Set the new default audio device
      for (int x = 0; x < (int) ERole_enum_count; ++x) {
        policy->SetDefaultEndpoint(new_default_id.get(), (ERole) x);
      }

      BOOST_LOG(info) << "Successfully reset default audio device"sv;
    }

    /**
     * @brief Installs the Steam Streaming Speakers driver, if present.
     * @return `true` if installation was successful.
     */
    bool install_steam_audio_drivers() {
#ifdef STEAM_DRIVER_SUBDIR
      // MinGW's libnewdev.a is missing DiInstallDriverW() even though the headers have it,
      // so we have to load it at runtime. It's Vista or later, so it will always be available.
      auto newdev = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!newdev) {
        BOOST_LOG(error) << "newdev.dll failed to load"sv;
        return false;
      }
      auto fg = util::fail_guard([newdev]() {
        FreeLibrary(newdev);
      });

      auto fn_DiInstallDriverW = (decltype(DiInstallDriverW) *) GetProcAddress(newdev, "DiInstallDriverW");
      if (!fn_DiInstallDriverW) {
        BOOST_LOG(error) << "DiInstallDriverW() is missing"sv;
        return false;
      }

      // Get the current default audio device (if present)
      auto old_default_dev = default_device(device_enum);

      // Install the Steam Streaming Speakers driver
      WCHAR driver_path[MAX_PATH] = {};
      ExpandEnvironmentStringsW(STEAM_AUDIO_DRIVER_PATH, driver_path, ARRAYSIZE(driver_path));
      if (fn_DiInstallDriverW(nullptr, driver_path, 0, nullptr)) {
        BOOST_LOG(info) << "Successfully installed Steam Streaming Speakers"sv;

        // Wait for 5 seconds to allow the audio subsystem to reconfigure things before
        // modifying the default audio device or enumerating devices again.
        Sleep(5000);

        // If there was a previous default device, restore that original device as the
        // default output device just in case installing the new one changed it.
        if (old_default_dev) {
          audio::wstring_t old_default_id;
          old_default_dev->GetId(&old_default_id);

          for (int x = 0; x < (int) ERole_enum_count; ++x) {
            policy->SetDefaultEndpoint(old_default_id.get(), (ERole) x);
          }
        }

        return true;
      } else {
        auto err = GetLastError();
        switch (err) {
          case ERROR_ACCESS_DENIED:
            BOOST_LOG(warning) << "Administrator privileges are required to install Steam Streaming Speakers"sv;
            break;
          case ERROR_FILE_NOT_FOUND:
          case ERROR_PATH_NOT_FOUND:
            BOOST_LOG(info) << "Steam audio drivers not found. This is expected if you don't have Steam installed."sv;
            break;
          default:
            BOOST_LOG(warning) << "Failed to install Steam audio drivers: "sv << err;
            break;
        }

        return false;
      }
#else
      BOOST_LOG(warning) << "Unable to install Steam Streaming Speakers on unknown architecture"sv;
      return false;
#endif
    }

    /**
     * @brief Initialize Windows audio policy interfaces.
     *
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init() {
      auto status = CoCreateInstance(
        CLSID_CPolicyConfigClient,
        nullptr,
        CLSCTX_ALL,
        IID_IPolicyConfig,
        (void **) &policy
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't create audio policy config: [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      status = CoCreateInstance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        (void **) &device_enum
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't create Device Enumerator: [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      return 0;
    }

    /**
     * @brief Destroy the Windows audio control.
     */
    ~audio_control_t() override {
    }

    policy_t policy;  ///< Windows policy configuration interface used to switch default audio devices.
    audio::device_enum_t device_enum;  ///< Device enumerator used to query and watch audio endpoints.
    std::string assigned_sink;  ///< Sink assigned while Sunshine captures host audio, captured directly by the microphone.
  };

#ifdef SUNSHINE_TESTS
  namespace tests {
    /**
     * @brief Resolve a sink through the production Windows endpoint lookup.
     *
     * @param sink Sink name, virtual sink descriptor, or device identifier.
     * @param device_enum Device enumerator supplied by the test.
     * @return `true` when the sink resolves to an active endpoint.
     */
    bool sink_device_available(const std::string &sink, IMMDeviceEnumerator *device_enum) {
      audio_control_t control;
      device_enum->AddRef();
      control.device_enum.reset(device_enum);
      return static_cast<bool>(control.get_sink_device(sink));
    }

    /**
     * @brief Exercise microphone creation with controlled assigned and configured sinks.
     *
     * @param assigned_sink Sink selected by the shared audio context.
     * @param configured_sink Sink configured by the user.
     * @param device_enum Device enumerator supplied by the test.
     * @return `true` when microphone initialization succeeds.
     */
    bool microphone_available(const std::string &assigned_sink, const std::string &configured_sink, IMMDeviceEnumerator *device_enum) {
      audio_control_t control;
      device_enum->AddRef();
      control.device_enum.reset(device_enum);
      control.assigned_sink = assigned_sink;

      auto previous_configured_sink = std::exchange(config::audio.sink, configured_sink);
      auto microphone = control.microphone(nullptr, 2, 48000, 240, false, false);
      config::audio.sink = std::move(previous_configured_sink);
      return static_cast<bool>(microphone);
    }

    /**
     * @brief Select a default or explicit capture endpoint through the production selection path.
     *
     * @param device_enum Device enumerator supplied by the test.
     * @param capture_device Explicit endpoint, or `nullptr` to select the default endpoint.
     * @return `true` when capture follows the default endpoint.
     */
    bool capture_follows_default_device(IMMDeviceEnumerator *device_enum, IMMDevice *capture_device) {
      mic_wasapi_t microphone;
      device_enum->AddRef();
      microphone.device_enum.reset(device_enum);

      device_t selected_device;
      if (capture_device) {
        capture_device->AddRef();
        selected_device.reset(capture_device);
      }

      microphone.select_capture_device(std::move(selected_device));
      return microphone.follows_default_device;
    }

    /**
     * @brief Exercise the production default-device-change path without live audio hardware.
     *
     * @param follows_default_device Whether the capture follows the default render endpoint.
     * @param install_callback Whether to install a default-device-change callback.
     * @param render_device_changed Whether to signal a render rather than capture endpoint change.
     * @param callback_count Receives the number of callback invocations.
     * @return Capture result produced after processing the notification.
     */
    capture_e simulate_default_device_change(bool follows_default_device, bool install_callback, bool render_device_changed, int &callback_count) {
      mic_wasapi_t mic;
      mic.audio_event.reset(CreateEventA(nullptr, FALSE, FALSE, nullptr));
      mic.default_latency_ms = 0;
      mic.sample_buf = util::buffer_t<float> {1};
      mic.sample_buf_pos = std::begin(mic.sample_buf);
      mic.continuous_audio = false;
      mic.follows_default_device = follows_default_device;

      if (install_callback) {
        mic.default_endpt_changed_cb = [&callback_count] {
          ++callback_count;
        };
      }

      mic.endpt_notification.OnDefaultDeviceChanged(
        render_device_changed ? eRender : eCapture,
        eConsole,
        nullptr
      );

      std::vector<float> sample(1);
      return mic.sample(sample);
    }

    /**
     * @brief Return the virtual device identifier used for process loopback activation.
     *
     * The identifier must be passed verbatim to ActivateAudioInterfaceAsync();
     * appending a GUID or path suffix makes activation fail with
     * HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND).
     *
     * @return The null-terminated virtual device identifier.
     */
    const wchar_t *process_loopback_device_path() {
      return VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK;
    }

    /**
     * @brief Return the byte size of the process loopback activation parameter blob.
     *
     * @return `sizeof(AUDIOCLIENT_ACTIVATION_PARAMS)` for the current toolchain ABI.
     */
    std::size_t process_loopback_params_size() {
      return sizeof(AUDIOCLIENT_ACTIVATION_PARAMS);
    }

    /**
     * @brief Exercise the mic_proc_t upmix path without activating WASAPI.
     *
     * Constructs a mic_proc_t, injects stereo samples into its buffer, and
     * invokes sample() to verify the 2-to-N channel conversion.
     *
     * @param stereo_input Interleaved stereo frames to feed to the microphone.
     * @param output_channels Output channel count (2, 6, or 8).
     * @param target_frame_size Number of stereo frames per sample() call.
     * @param output Receives the converted multi-channel output.
     * @return Capture result from sample().
     */
    capture_e proc_mic_convert(const std::vector<float> &stereo_input, std::uint32_t output_channels, std::uint32_t target_frame_size, std::vector<float> &output) {
      mic_proc_t mic;
      mic.frame_size = target_frame_size;
      mic.channels = output_channels;
      mic.process_id = config::video.capture_process;
      mic.continuous_audio = false;
      mic.stream_ended = false;
      {
        std::lock_guard lock(mic.mutex);
        mic.sample_buf = stereo_input;
      }
      output.resize(static_cast<std::size_t>(target_frame_size) * output_channels);
      return mic.sample(output);
    }

    /**
     * @brief Verify that mic_proc_t returns silence when the buffer underflows in continuous mode.
     *
     * @param target_frame_size Number of stereo frames per sample() call.
     * @param output_channels Output channel count (2, 6, or 8).
     * @param output Receives the silence-filled output.
     * @return Capture result from sample().
     */
    capture_e proc_mic_continuous_silence(std::uint32_t target_frame_size, std::uint32_t output_channels, std::vector<float> &output) {
      mic_proc_t mic;
      mic.frame_size = target_frame_size;
      mic.channels = output_channels;
      mic.process_id = config::video.capture_process;
      mic.continuous_audio = true;
      mic.stream_ended = false;
      mic.sample_timeout = std::chrono::milliseconds(5);
      output.resize(static_cast<std::size_t>(target_frame_size) * output_channels);
      return mic.sample(output);
    }

    /**
     * @brief Verify that mic_proc_t returns error when the stream ends with insufficient data.
     *
     * @param target_frame_size Number of stereo frames per sample() call.
     * @param output_channels Output channel count (2, 6, or 8).
     * @param output Receives any partial output.
     * @return Capture result from sample().
     */
    capture_e proc_mic_stream_ended_error(std::uint32_t target_frame_size, std::uint32_t output_channels, std::vector<float> &output) {
      mic_proc_t mic;
      mic.frame_size = target_frame_size;
      mic.channels = output_channels;
      mic.process_id = config::video.capture_process;
      mic.continuous_audio = false;
      mic.stream_ended = true;
      output.resize(static_cast<std::size_t>(target_frame_size) * output_channels);
      return mic.sample(output);
    }

    /**
     * @brief Verify that mic_proc_t returns timeout when no data arrives in non-continuous mode.
     *
     * @param frame_size Number of stereo frames per sample() call.
     * @param output Receives any partial output.
     * @return Capture result from sample().
     */
    capture_e proc_mic_timeout(std::uint32_t frame_size, std::vector<float> &output) {
      mic_proc_t mic;
      mic.frame_size = frame_size;
      mic.channels = 2;
      mic.process_id = config::video.capture_process;
      mic.continuous_audio = false;
      mic.stream_ended = false;
      mic.sample_timeout = std::chrono::milliseconds(5);
      output.resize(static_cast<std::size_t>(frame_size) * 2);
      return mic.sample(output);
    }

    /**
     * @brief Verify that mic_proc_t returns silence when the stream ends in continuous mode.
     *
     * @param target_frame_size Number of stereo frames per sample() call.
     * @param output_channels Output channel count (2, 6, or 8).
     * @param output Receives the silence-filled output.
     * @return Capture result from sample().
     */
    capture_e proc_mic_stream_ended_continuous(std::uint32_t target_frame_size, std::uint32_t output_channels, std::vector<float> &output) {
      mic_proc_t mic;
      mic.frame_size = target_frame_size;
      mic.channels = output_channels;
      mic.process_id = config::video.capture_process;
      mic.continuous_audio = true;
      mic.stream_ended = true;
      output.resize(static_cast<std::size_t>(target_frame_size) * output_channels);
      return mic.sample(output);
    }

    /**
     * @brief Verify that mic_proc_t requests reinitialization when the capture target changes.
     *
     * Simulates a mid-session window change by temporarily pointing
     * config::video.capture_process at a different PID than the one this
     * microphone was spawned for.
     *
     * @param output Receives any output written before reinitialization.
     * @return Capture result from sample().
     */
    capture_e proc_mic_process_changed(std::vector<float> &output) {
      auto previous_process = std::exchange(config::video.capture_process, 1u);
      mic_proc_t mic;
      mic.frame_size = 1;
      mic.channels = 2;
      mic.process_id = config::video.capture_process + 1;
      mic.continuous_audio = false;
      mic.stream_ended = false;
      {
        std::lock_guard lock(mic.mutex);
        mic.sample_buf = std::vector<float> {1.0f, 2.0f};
      }
      output.resize(2);
      auto result = mic.sample(output);
      config::video.capture_process = previous_process;
      return result;
    }

    /**
     * @brief Verify the gate stays closed in desktop mode (capture_process == 0),
     *        even with a backoff armed.
     * @return The gate decision; must be false.
     */
    bool process_switch_gate_closed_for_desktop() {
      auto previous_process = std::exchange(config::video.capture_process, 0u);
      arm_proc_capture_backoff(900u);
      auto open = process_capture_switch_pending();
      config::video.capture_process = previous_process;
      clear_proc_capture_backoff();
      return open;
    }

    /**
     * @brief Verify a fresh backoff suppresses retries for the same target PID.
     * @return The gate decision; must be false.
     */
    bool process_switch_backoff_blocks_same_pid() {
      auto previous_process = std::exchange(config::video.capture_process, 901u);
      arm_proc_capture_backoff(901u);
      auto open = process_capture_switch_pending();
      config::video.capture_process = previous_process;
      clear_proc_capture_backoff();
      return open;
    }

    /**
     * @brief Verify selecting a different target PID bypasses an armed backoff.
     * @return The gate decision; must be true.
     */
    bool process_switch_backoff_allows_different_pid() {
      auto previous_process = std::exchange(config::video.capture_process, 902u);
      arm_proc_capture_backoff(901u);
      auto open = process_capture_switch_pending();
      config::video.capture_process = previous_process;
      clear_proc_capture_backoff();
      return open;
    }

    /**
     * @brief Verify that mic_wasapi_t requests reinitialization when a process
     *        target is selected mid-session.
     *
     * Temporarily points config::video.capture_process at a fake PID and invokes
     * the WASAPI microphone's sample() without any audio hardware; the switch
     * gate must make it return capture_e::reinit before touching WASAPI.
     *
     * @return Capture result from sample().
     */
    capture_e wasapi_mic_process_switch() {
      auto previous_process = std::exchange(config::video.capture_process, 321u);
      clear_proc_capture_backoff();
      mic_wasapi_t microphone;
      std::vector<float> sample(1);
      auto result = microphone.sample(sample);
      config::video.capture_process = previous_process;
      clear_proc_capture_backoff();
      return result;
    }
  }  // namespace tests
#endif
}  // namespace platf::audio

namespace platf {

  // It's not big enough to justify it's own source file :/
  namespace dxgi {
    /**
     * @brief Initialize the Windows audio-control backend.
     *
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init();
  }  // namespace dxgi

  std::unique_ptr<audio_control_t> audio_control() {
    auto control = std::make_unique<audio::audio_control_t>();

    if (control->init()) {
      return nullptr;
    }

    // Install Steam Streaming Speakers if needed. We do this during audio_control() to ensure
    // the sink information returned includes the new Steam Streaming Speakers device.
    if (config::audio.install_steam_drivers && !control->find_device_id(control->match_steam_speakers())) {
      // This is best effort. Don't fail if it doesn't work.
      control->install_steam_audio_drivers();
    }

    return control;
  }

  std::unique_ptr<deinit_t> init() {
    if (dxgi::init()) {
      return nullptr;
    }

    // Initialize COM
    auto co_init = std::make_unique<platf::audio::co_init_t>();

    // If Steam Streaming Speakers are currently the default audio device,
    // change the default to something else (if another device is available).
    audio::audio_control_t audio_ctrl;
    if (audio_ctrl.init() == 0) {
      audio_ctrl.reset_default_device();
    }

    return co_init;
  }
}  // namespace platf
