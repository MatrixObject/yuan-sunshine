/**
 * @file tests/unit/platform/windows/test_audio.cpp
 * @brief Tests for Windows audio sink selection and endpoint-change handling.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <cstring>
  #include <vector>

  // platform includes
  #include <mmdeviceapi.h>
  #include <propsys.h>

  // local includes
  #include "src/platform/common.h"

namespace platf::audio::tests {
  bool sink_device_available(const std::string &sink, IMMDeviceEnumerator *device_enum);
  bool microphone_available(const std::string &assigned_sink, const std::string &configured_sink, IMMDeviceEnumerator *device_enum);
  bool capture_follows_default_device(IMMDeviceEnumerator *device_enum, IMMDevice *capture_device);
  capture_e simulate_default_device_change(bool follows_default_device, bool install_callback, bool render_device_changed, int &callback_count);
  capture_e proc_mic_convert(const std::vector<float> &stereo_input, std::uint32_t output_channels, std::uint32_t target_frame_size, std::vector<float> &output);
  capture_e proc_mic_continuous_silence(std::uint32_t target_frame_size, std::uint32_t output_channels, std::vector<float> &output);
  capture_e proc_mic_stream_ended_error(std::uint32_t target_frame_size, std::uint32_t output_channels, std::vector<float> &output);
  capture_e proc_mic_stream_ended_continuous(std::uint32_t target_frame_size, std::uint32_t output_channels, std::vector<float> &output);
  capture_e proc_mic_timeout(std::uint32_t frame_size, std::vector<float> &output);
  capture_e proc_mic_process_changed(std::vector<float> &output);
  const wchar_t *process_loopback_device_path();
  std::size_t process_loopback_params_size();
  bool process_switch_gate_closed_for_desktop();
  bool process_switch_backoff_blocks_same_pid();
  bool process_switch_backoff_allows_different_pid();
  capture_e wasapi_mic_process_switch();
}  // namespace platf::audio::tests

namespace {
  class fake_property_store_t final: public IPropertyStore {
  public:
    explicit fake_property_store_t(std::wstring friendly_name):
        friendly_name {std::move(friendly_name)} {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE GetCount(DWORD *property_count) override {
      *property_count = friendly_name.empty() ? 0 : 1;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAt(DWORD, PROPERTYKEY *) override {
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY, PROPVARIANT *value) override {
      if (friendly_name.empty()) {
        return E_NOTIMPL;
      }

      const auto byte_count = (friendly_name.size() + 1) * sizeof(wchar_t);
      value->pwszVal = static_cast<wchar_t *>(CoTaskMemAlloc(byte_count));
      if (!value->pwszVal) {
        return E_OUTOFMEMORY;
      }

      std::memcpy(value->pwszVal, friendly_name.c_str(), byte_count);
      value->vt = VT_LPWSTR;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override {
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Commit() override {
      return E_NOTIMPL;
    }

    std::wstring friendly_name;
  };

  class fake_device_t final: public IMMDevice {
  public:
    fake_device_t(std::wstring id, std::wstring friendly_name):
        id {std::move(id)},
        properties {std::move(friendly_name)} {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE Activate(REFIID, DWORD, PROPVARIANT *, void **) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore **property_store) override {
      properties.AddRef();
      *property_store = &properties;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetId(LPWSTR *device_id) override {
      const auto byte_count = (id.size() + 1) * sizeof(wchar_t);
      *device_id = static_cast<wchar_t *>(CoTaskMemAlloc(byte_count));
      if (!*device_id) {
        return E_OUTOFMEMORY;
      }

      std::memcpy(*device_id, id.c_str(), byte_count);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetState(DWORD *device_state) override {
      if (FAILED(state_status)) {
        return state_status;
      }

      *device_state = state;
      return S_OK;
    }

    std::wstring id;
    fake_property_store_t properties;
    HRESULT state_status = S_OK;
    DWORD state = DEVICE_STATE_ACTIVE;
  };

  class fake_device_collection_t final: public IMMDeviceCollection {
  public:
    explicit fake_device_collection_t(fake_device_t &device):
        device {device} {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE GetCount(UINT *device_count) override {
      *device_count = 1;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Item(UINT index, IMMDevice **item) override {
      if (index != 0) {
        return E_INVALIDARG;
      }

      device.AddRef();
      *item = &device;
      return S_OK;
    }

    fake_device_t &device;
  };

  class fake_device_enumerator_t final: public IMMDeviceEnumerator {
  public:
    explicit fake_device_enumerator_t(std::wstring id, std::wstring friendly_name = {}):
        device {std::move(id), std::move(friendly_name)},
        collection {device} {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow, DWORD, IMMDeviceCollection **devices) override {
      if (FAILED(enumeration_status)) {
        return enumeration_status;
      }

      collection.AddRef();
      *devices = &collection;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow, ERole, IMMDevice **resolved_device) override {
      ++get_default_device_calls;
      device.AddRef();
      *resolved_device = &device;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR device_id, IMMDevice **resolved_device) override {
      ++get_device_calls;
      last_requested_id = device_id;
      if (FAILED(get_device_status)) {
        return get_device_status;
      }
      if (last_requested_id != device.id) {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
      }

      device.AddRef();
      *resolved_device = &device;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient *) override {
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient *) override {
      return S_OK;
    }

    fake_device_t device;
    fake_device_collection_t collection;
    HRESULT enumeration_status = S_OK;
    HRESULT get_device_status = S_OK;
    int get_device_calls = 0;
    int get_default_device_calls = 0;
    std::wstring last_requested_id;
  };
}  // namespace

TEST(WindowsAudioTest, AssignedSinkTakesPriorityOverConfiguredSink) {
  fake_device_enumerator_t enumerator {L"assigned-id"};

  EXPECT_FALSE(platf::audio::tests::microphone_available("assigned-id", "configured-id", &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"assigned-id");
}

TEST(WindowsAudioTest, ConfiguredSinkIsUsedWhenNoSinkWasAssigned) {
  fake_device_enumerator_t enumerator {L"configured-id"};

  EXPECT_FALSE(platf::audio::tests::microphone_available({}, "configured-id", &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"configured-id");
}

TEST(WindowsAudioTest, DefaultDeviceIsUsedWhenNoSinkWasRequested) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  platf::audio::tests::microphone_available({}, {}, &enumerator);
  EXPECT_EQ(enumerator.get_device_calls, 0);
}

TEST(WindowsAudioTest, DefaultCaptureSelectsDefaultEndpoint) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  EXPECT_TRUE(platf::audio::tests::capture_follows_default_device(&enumerator, nullptr));
  EXPECT_EQ(enumerator.get_default_device_calls, 1);
}

TEST(WindowsAudioTest, PinnedCaptureKeepsExplicitEndpoint) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  EXPECT_FALSE(platf::audio::tests::capture_follows_default_device(&enumerator, &enumerator.device));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST(WindowsAudioTest, MicrophoneRejectsUnresolvedSink) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  EXPECT_FALSE(platf::audio::tests::microphone_available("unknown", {}, &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 0);
}

TEST(WindowsAudioTest, ResolvesVirtualSinkDescriptorToActiveEndpoint) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  EXPECT_TRUE(platf::audio::tests::sink_device_available("virtual-Stereoendpoint-id", &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"endpoint-id");
}

TEST(WindowsAudioTest, ResolvesDeviceIdentifiersAndFriendlyNames) {
  fake_device_enumerator_t id_enumerator {L"endpoint-id"};
  EXPECT_TRUE(platf::audio::tests::sink_device_available("endpoint-id", &id_enumerator));

  fake_device_enumerator_t name_enumerator {L"endpoint-id", L"Friendly Endpoint"};
  EXPECT_TRUE(platf::audio::tests::sink_device_available("Friendly Endpoint", &name_enumerator));
}

TEST(WindowsAudioTest, RejectsUnknownOrUnenumerableSinks) {
  fake_device_enumerator_t unknown_enumerator {L"endpoint-id"};
  EXPECT_FALSE(platf::audio::tests::sink_device_available("unknown", &unknown_enumerator));
  EXPECT_EQ(unknown_enumerator.get_device_calls, 0);

  fake_device_enumerator_t failed_enumerator {L"endpoint-id"};
  failed_enumerator.enumeration_status = E_FAIL;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("endpoint-id", &failed_enumerator));
  EXPECT_EQ(failed_enumerator.get_device_calls, 0);
}

TEST(WindowsAudioTest, RejectsUnavailableResolvedEndpoints) {
  fake_device_enumerator_t missing_enumerator {L"endpoint-id"};
  missing_enumerator.get_device_status = E_FAIL;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("virtual-Stereoendpoint-id", &missing_enumerator));

  fake_device_enumerator_t state_failure_enumerator {L"endpoint-id"};
  state_failure_enumerator.device.state_status = E_FAIL;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("virtual-Stereoendpoint-id", &state_failure_enumerator));

  fake_device_enumerator_t inactive_enumerator {L"endpoint-id"};
  inactive_enumerator.device.state = DEVICE_STATE_DISABLED;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("virtual-Stereoendpoint-id", &inactive_enumerator));
}

TEST(WindowsAudioTest, DefaultFollowingCaptureReinitializesAfterRenderEndpointChange) {
  int callback_count = 0;

  EXPECT_EQ(
    platf::audio::tests::simulate_default_device_change(true, true, true, callback_count),
    platf::capture_e::reinit
  );
  EXPECT_EQ(callback_count, 1);
}

TEST(WindowsAudioTest, PinnedCaptureContinuesAfterRenderEndpointChange) {
  int callback_count = 0;

  EXPECT_EQ(
    platf::audio::tests::simulate_default_device_change(false, true, true, callback_count),
    platf::capture_e::timeout
  );
  EXPECT_EQ(callback_count, 1);
}

TEST(WindowsAudioTest, CaptureEndpointChangeDoesNotTriggerRenderCallback) {
  int callback_count = 0;

  EXPECT_EQ(
    platf::audio::tests::simulate_default_device_change(false, true, false, callback_count),
    platf::capture_e::timeout
  );
  EXPECT_EQ(callback_count, 0);
}

TEST(WindowsAudioTest, DefaultChangeWithoutCallbackStillReinitializes) {
  int callback_count = 0;

  EXPECT_EQ(
    platf::audio::tests::simulate_default_device_change(true, false, true, callback_count),
    platf::capture_e::reinit
  );
  EXPECT_EQ(callback_count, 0);
}

TEST(WindowsAudioTest, ProcMicPassthroughStereo) {
  // Two stereo frames: [L0 R0 L1 R1]
  std::vector<float> stereo {1.0f, 2.0f, 3.0f, 4.0f};
  std::vector<float> out;
  EXPECT_EQ(
    platf::audio::tests::proc_mic_convert(stereo, 2, 2, out),
    platf::capture_e::ok
  );
  ASSERT_EQ(out.size(), 4u);
  EXPECT_FLOAT_EQ(out[0], 1.0f);
  EXPECT_FLOAT_EQ(out[1], 2.0f);
  EXPECT_FLOAT_EQ(out[2], 3.0f);
  EXPECT_FLOAT_EQ(out[3], 4.0f);
}

TEST(WindowsAudioTest, ProcMicUpmixToSixChannels) {
  // One stereo frame: [L=0.8 R=-0.4]
  std::vector<float> stereo {0.8f, -0.4f};
  std::vector<float> out;
  EXPECT_EQ(
    platf::audio::tests::proc_mic_convert(stereo, 6, 1, out),
    platf::capture_e::ok
  );
  ASSERT_EQ(out.size(), 6u);
  EXPECT_FLOAT_EQ(out[0], 0.8f);   // FL
  EXPECT_FLOAT_EQ(out[1], -0.4f);  // FR
  EXPECT_FLOAT_EQ(out[2], 0.2f);   // FC = (0.8 + -0.4) / 2
  EXPECT_FLOAT_EQ(out[3], 0.0f);   // LFE
  EXPECT_FLOAT_EQ(out[4], 0.0f);   // BL
  EXPECT_FLOAT_EQ(out[5], 0.0f);   // BR
}

TEST(WindowsAudioTest, ProcMicUpmixToEightChannels) {
  // One stereo frame: [L=0.5 R=0.5]
  std::vector<float> stereo {0.5f, 0.5f};
  std::vector<float> out;
  EXPECT_EQ(
    platf::audio::tests::proc_mic_convert(stereo, 8, 1, out),
    platf::capture_e::ok
  );
  ASSERT_EQ(out.size(), 8u);
  EXPECT_FLOAT_EQ(out[0], 0.5f);  // FL
  EXPECT_FLOAT_EQ(out[1], 0.5f);  // FR
  EXPECT_FLOAT_EQ(out[2], 0.5f);  // FC = (0.5+0.5)/2
  EXPECT_FLOAT_EQ(out[3], 0.0f);  // LFE
  EXPECT_FLOAT_EQ(out[4], 0.0f);  // BL
  EXPECT_FLOAT_EQ(out[5], 0.0f);  // BR
  EXPECT_FLOAT_EQ(out[6], 0.0f);  // SL
  EXPECT_FLOAT_EQ(out[7], 0.0f);  // SR
}

TEST(WindowsAudioTest, ProcMicContinuousModeReturnsSilenceOnTimeout) {
  std::vector<float> out;
  EXPECT_EQ(
    platf::audio::tests::proc_mic_continuous_silence(480, 2, out),
    platf::capture_e::ok
  );
  ASSERT_EQ(out.size(), 960u);
  for (auto v : out) {
    EXPECT_FLOAT_EQ(v, 0.0f);
  }
}

TEST(WindowsAudioTest, ProcMicStreamEndedReturnsError) {
  std::vector<float> out;
  EXPECT_EQ(
    platf::audio::tests::proc_mic_stream_ended_error(480, 2, out),
    platf::capture_e::error
  );
}

TEST(WindowsAudioTest, ProcMicStreamEndedContinuousReturnsSilence) {
  std::vector<float> out;
  EXPECT_EQ(
    platf::audio::tests::proc_mic_stream_ended_continuous(480, 2, out),
    platf::capture_e::ok
  );
  ASSERT_EQ(out.size(), 960u);
  for (auto v : out) {
    EXPECT_FLOAT_EQ(v, 0.0f);
  }
}

TEST(WindowsAudioTest, ProcMicTimeoutReturnsTimeoutWhenNotContinuous) {
  std::vector<float> out;
  EXPECT_EQ(
    platf::audio::tests::proc_mic_timeout(480, out),
    platf::capture_e::timeout
  );
}

TEST(WindowsAudioTest, ProcMicProcessChangeRequestsReinit) {
  std::vector<float> out;
  EXPECT_EQ(
    platf::audio::tests::proc_mic_process_changed(out),
    platf::capture_e::reinit
  );
}

TEST(WindowsAudioTest, ProcessSwitchGateClosedForDesktop) {
  EXPECT_FALSE(platf::audio::tests::process_switch_gate_closed_for_desktop());
}

TEST(WindowsAudioTest, ProcessSwitchBackoffBlocksSamePid) {
  EXPECT_FALSE(platf::audio::tests::process_switch_backoff_blocks_same_pid());
}

TEST(WindowsAudioTest, ProcessSwitchBackoffAllowsDifferentPid) {
  EXPECT_TRUE(platf::audio::tests::process_switch_backoff_allows_different_pid());
}

TEST(WindowsAudioTest, WasapiMicProcessSwitchRequestsReinit) {
  EXPECT_EQ(
    platf::audio::tests::wasapi_mic_process_switch(),
    platf::capture_e::reinit
  );
}

TEST(WindowsAudioTest, ProcessLoopbackDevicePathIsExactVirtualDeviceIdentifier) {
  // Any extra path segment (e.g. a trailing GUID) makes the async activation
  // complete with HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND).
  const wchar_t *path = platf::audio::tests::process_loopback_device_path();
  ASSERT_NE(path, nullptr);
  EXPECT_STREQ(path, L"VAD\\Process_Loopback");
}

TEST(WindowsAudioTest, ProcessLoopbackActivationParamsBlobLayout) {
  // DWORD TargetProcessId followed by the 4-byte loopback mode enum, wrapped by
  // a 4-byte activation type: 12 bytes on the Windows ABI.
  EXPECT_EQ(platf::audio::tests::process_loopback_params_size(), 12U);
}
#endif
