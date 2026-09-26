/**
 * @file tests/unit/test_config.cpp
 * @brief Test src/config.* recorder (DLL-based) capture options.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <limits>
#include <string>

// local includes
#include <src/config.h>
#include <src/file_handler.h>

using namespace std::literals;

/**
 * @brief Fixture that restores the global configuration singletons.
 */
struct RecorderConfigTest: BaseTest {
  void SetUp() override {
    BaseTest::SetUp();
    config::video.capture = "ddx"s;
    config::video.capture_process = 0;
    config::video.capture_window = 0;
    config::stream.file_apps = SUNSHINE_SOURCE_DIR "/tests/unit/test_config.cpp";
  }

  void TearDown() override {
    config::video = original_video;
    config::audio = original_audio;
    config::stream = original_stream;
    config::nvhttp = original_nvhttp;
    config::input = original_input;
    config::sunshine = original_sunshine;
    config::modified_config_settings = original_modified_config_settings;
    BaseTest::TearDown();
  }

  config::video_t original_video {config::video};  ///< Video configuration restored after each test.
  config::audio_t original_audio {config::audio};  ///< Audio configuration restored after each test.
  config::stream_t original_stream {config::stream};  ///< Stream configuration restored after each test.
  config::nvhttp_t original_nvhttp {config::nvhttp};  ///< HTTP configuration restored after each test.
  config::input_t original_input {config::input};  ///< Input configuration restored after each test.
  config::sunshine_t original_sunshine {config::sunshine};  ///< Core configuration restored after each test.
  decltype(config::modified_config_settings) original_modified_config_settings {config::modified_config_settings};  ///< Modified settings restored after each test.
};

TEST_F(RecorderConfigTest, ParsesCaptureProcess) {
  config::apply_config_for_test("capture_process = 4242\n"sv);

  EXPECT_EQ(4242u, config::video.capture_process);
}

TEST_F(RecorderConfigTest, ParsesCaptureWindow) {
  config::apply_config_for_test("capture_window = 123456789\n"sv);

  EXPECT_EQ(123456789u, config::video.capture_window);
}

TEST_F(RecorderConfigTest, ParsesCaptureProcessAndWindowTogether) {
  config::apply_config_for_test("capture_process = 1234\ncapture_window = 5678\n"sv);

  EXPECT_EQ(1234u, config::video.capture_process);
  EXPECT_EQ(5678u, config::video.capture_window);
}

TEST_F(RecorderConfigTest, KeepsDefaultsForEmptySettings) {
  config::apply_config_for_test("capture = dll:window_capture\n"sv);

  EXPECT_EQ(0u, config::video.capture_process);
  EXPECT_EQ(0u, config::video.capture_window);
}

TEST_F(RecorderConfigTest, IgnoresInvalidCaptureProcess) {
  config::apply_config_for_test("capture_process = not-a-number\n"sv);

  EXPECT_EQ(0u, config::video.capture_process);
}

TEST_F(RecorderConfigTest, IgnoresInvalidCaptureWindow) {
  config::apply_config_for_test("capture_window = not-a-number\n"sv);

  EXPECT_EQ(0u, config::video.capture_window);
}

TEST_F(RecorderConfigTest, HandlesMaximumCaptureWindow) {
  config::apply_config_for_test("capture_window = 18446744073709551615\n"sv);

  EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), config::video.capture_window);
}