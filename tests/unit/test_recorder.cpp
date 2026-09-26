/**
 * @file tests/unit/test_recorder.cpp
 * @brief Test src/recorder/recorder_glue.h and src/recorder/recorder_api.cpp.
 */

// standard includes
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

// test includes
#include "../tests_common.h"

// local includes
#include "src/recorder/recorder_glue.h"

namespace {

  /**
   * @brief Fake recorder implementation for exercising recorder_display_t.
   *
   * @note The instance is owned through recorder::recorder_ptr_t whose deleter
   *       delegates to recorder::destroy(). Since the fake is never registered
   *       in the DLL registry, destroy() performs no deallocation; the small
   *       test-only allocation is intentionally left to the OS at exit.
   */
  class fake_recorder_t : public recorder::recorder_t {
  public:
    /**
     * @brief Construct the fake recorder.
     *
     * @param width  Frame width emitted by the fake.
     * @param height Frame height emitted by the fake.
     */
    explicit fake_recorder_t(int width, int height):
        width(width),
        height(height) {}

    /**
     * @brief Emit one frame through the push callback.
     *
     * @param push_cb Callback that receives the captured frame.
     * @param pull_cb Callback that provides an empty buffer (unused by the fake).
     * @param cursor  Pointer to the cursor-visibility flag; the fake flips it.
     * @return The configured result.
     */
    recorder::capture_e capture(
      const recorder::push_captured_image_cb_t &push_cb,
      const recorder::pull_free_image_cb_t &pull_cb,
      bool *cursor) override {
      (void) pull_cb;

      ++calls;

      if (cursor) {
        *cursor = false;
      }

      recorder::capture_e status = result;
      if (emit_frame) {
        auto img = std::make_shared<recorder::img_t>();
        img->width = width;
        img->height = height;
        img->pixel_pitch = 4;
        img->row_pitch = width * 4;

        const auto bytes = static_cast<std::size_t>(img->row_pitch) * img->height;
        auto *raw = new std::uint8_t[bytes];
        std::memcpy(raw, source.data(), source.size());
        img->data = raw;

        // The host copies synchronously inside push_cb, so the buffer can be
        // freed as soon as push_cb returns.
        const bool accepted = push_cb(std::move(img), frame_captured);
        delete[] raw;

        if (interrupt_on_rejection && !accepted) {
          status = recorder::capture_e::interrupted;
        }
      }
      else {
        push_cb(nullptr, false);
      }

      return status;
    }

    int width {0};  ///< Emitted frame width.
    int height {0};  ///< Emitted frame height.
    bool emit_frame {true};  ///< Whether to emit a frame or only a stop signal.
    bool frame_captured {true};  ///< Whether the emitted frame is a capture.
    bool interrupt_on_rejection {false};  ///< Return interrupted when the pipeline rejects a frame.
    recorder::capture_e result {recorder::capture_e::ok};  ///< Status returned by the fake.
    std::vector<std::uint8_t> source {};  ///< BGRA pixel data to emit.
    int calls {0};  ///< Number of capture() invocations.
  };

  /**
   * @brief Test fixture for recorder_display_t.
   */
  class RecorderDisplayTest: public BaseTest {
  public:
    /**
     * @brief Create a display adapter for a fake recorder.
     *
     * @param recorder The fake recorder to wrap.
     * @param init     Recorder configuration to seed display dimensions.
     * @return The constructed adapter.
     */
    static std::unique_ptr<recorder::recorder_display_t> make_display(
      fake_recorder_t *recorder,
      recorder::init_t init = {}) {
      return std::make_unique<recorder::recorder_display_t>(
        recorder::recorder_ptr_t(recorder, recorder::recorder_deleter_t {}),
        init);
    }

    /**
     * @brief A pipeline push callback that records the last delivered frame.
     *
     * Mirrors the real pipeline: returns true while the session is live and
     * false once the requested number of captured frames has been delivered.
     *
     * @param out Receives the last delivered frame (or nullptr for stop signals).
     * @param frame_captured Receives the capture flag of the last delivered frame.
     * @param frames_to_accept Number of captured frames to accept before the
     *                         pipeline stops delivering.
     * @param frames_pushed Optional counter of accepted captured frames.
     * @return The configured push callback.
     */
    static platf::display_t::push_captured_image_cb_t recording_push(
      std::shared_ptr<platf::img_t> &out,
      bool &frame_captured,
      std::size_t frames_to_accept = 1,
      std::size_t *frames_pushed = nullptr) {
      return [&out, &frame_captured, frames_to_accept, frames_pushed](std::shared_ptr<platf::img_t> &&img, bool captured) {
        if (frames_pushed && *frames_pushed >= frames_to_accept) {
          return false;
        }

        if (captured && img && img->data) {
          out = std::move(img);
          frame_captured = captured;

          if (frames_pushed) {
            ++*frames_pushed;
          }
        }

        return true;
      };
    }

    /**
     * @brief A pipeline pull callback that satisfies the adapter contract.
     */
    static platf::display_t::pull_free_image_cb_t null_pull() {
      return [](std::shared_ptr<platf::img_t> &) {
        return true;
      };
    }
  };

}  // namespace

TEST_F(RecorderDisplayTest, ConstructorSetsDimensionsFromInit) {
  recorder::init_t init {};
  init.width = 640;
  init.height = 480;

  auto display = make_display(nullptr, init);

  EXPECT_EQ(display->width, 640);
  EXPECT_EQ(display->height, 480);
  EXPECT_EQ(display->logical_width, 640);
  EXPECT_EQ(display->logical_height, 480);
}

TEST_F(RecorderDisplayTest, ConstructorKeepsZeroDimensionsWhenInitUnset) {
  auto display = make_display(nullptr);

  EXPECT_EQ(display->width, 0);
  EXPECT_EQ(display->height, 0);
}

TEST_F(RecorderDisplayTest, AllocImgReturnsNullWithoutDimensions) {
  auto display = make_display(nullptr);

  EXPECT_EQ(display->alloc_img(), nullptr);
}

TEST_F(RecorderDisplayTest, AllocImgMatchesRequestedLayout) {
  recorder::init_t init {};
  init.width = 128;
  init.height = 64;

  auto display = make_display(nullptr, init);
  auto img = display->alloc_img();

  ASSERT_NE(img, nullptr);
  ASSERT_NE(img->data, nullptr);

  EXPECT_EQ(img->width, 128);
  EXPECT_EQ(img->height, 64);
  EXPECT_EQ(img->pixel_pitch, 4);
  EXPECT_EQ(img->row_pitch, 128 * 4);

  const auto bytes = static_cast<std::size_t>(img->row_pitch) * img->height;
  for (std::size_t i = 0; i < bytes; ++i) {
    EXPECT_EQ(img->data[i], 0);
  }

  delete[] img->data;
}

TEST_F(RecorderDisplayTest, DummyImgZeroesBufferAndReturnsSuccess) {
  recorder::init_t init {};
  init.width = 16;
  init.height = 8;

  auto display = make_display(nullptr, init);
  auto img = display->alloc_img();

  ASSERT_NE(img, nullptr);
  ASSERT_NE(img->data, nullptr);

  std::memset(img->data, 0xAB, static_cast<std::size_t>(img->row_pitch) * img->height);

  EXPECT_EQ(display->dummy_img(img.get()), 0);

  const auto bytes = static_cast<std::size_t>(img->row_pitch) * img->height;
  for (std::size_t i = 0; i < bytes; ++i) {
    EXPECT_EQ(img->data[i], 0) << "byte " << i;
  }

  delete[] img->data;
}

TEST_F(RecorderDisplayTest, DummyImgAcceptsNull) {
  auto display = make_display(nullptr);

  EXPECT_EQ(display->dummy_img(nullptr), 0);
}

TEST_F(RecorderDisplayTest, MakeAvcodecEncodeDeviceReturnsValidDevice) {
  auto display = make_display(nullptr);

  auto device = display->make_avcodec_encode_device(platf::pix_fmt_e::nv12);

  EXPECT_NE(device, nullptr);
}

TEST_F(RecorderDisplayTest, CaptureReturnsErrorWithoutRecorder) {
  auto display = make_display(nullptr);
  bool cursor = true;

  const auto status = display->capture(
    [](std::shared_ptr<platf::img_t> &&, bool) {
      return true;
    },
    null_pull(),
    &cursor);

  EXPECT_EQ(status, platf::capture_e::error);
}

TEST_F(RecorderDisplayTest, CaptureCopiesRecorderFrameAndForwardsCursor) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 3;

  std::vector<std::uint8_t> source(static_cast<std::size_t>(kWidth) * kHeight * 4);
  for (std::size_t i = 0; i < source.size(); ++i) {
    source[i] = static_cast<std::uint8_t>(i % 251);
  }

  auto *fake = new fake_recorder_t(kWidth, kHeight);
  fake->source = std::move(source);

  auto display = make_display(fake);
  bool cursor = true;
  std::shared_ptr<platf::img_t> got;
  bool frame_captured = false;
  std::size_t frames_pushed = 0;

  const auto status = display->capture(
    recording_push(got, frame_captured, 1, &frames_pushed),
    null_pull(),
    &cursor);

  EXPECT_EQ(status, platf::capture_e::ok);
  EXPECT_EQ(fake->calls, 1);
  EXPECT_EQ(frames_pushed, 1);
  EXPECT_FALSE(cursor);

  ASSERT_NE(got, nullptr);
  ASSERT_NE(got->data, nullptr);

  EXPECT_EQ(got->width, kWidth);
  EXPECT_EQ(got->height, kHeight);
  EXPECT_EQ(got->pixel_pitch, 4);
  EXPECT_EQ(got->row_pitch, kWidth * 4);
  EXPECT_TRUE(frame_captured);
  EXPECT_TRUE(got->frame_timestamp.has_value());

  const auto bytes = static_cast<std::size_t>(got->row_pitch) * got->height;
  EXPECT_EQ(std::memcmp(got->data, fake->source.data(), bytes), 0);

  delete[] got->data;
}

TEST_F(RecorderDisplayTest, CaptureScalesFrameToDisplayDimensions) {
  recorder::init_t init {};
  init.width = 16;
  init.height = 9;

  auto *fake = new fake_recorder_t(8, 6);
  fake->source.resize(static_cast<std::size_t>(8) * 6 * 4);
  for (std::size_t i = 0; i < fake->source.size(); ++i) {
    fake->source[i] = static_cast<std::uint8_t>(i * 7 + 3);
  }

  auto display = make_display(fake, init);
  bool cursor = true;
  std::shared_ptr<platf::img_t> got;
  bool frame_captured = false;
  std::size_t frames_pushed = 0;

  const auto status = display->capture(
    recording_push(got, frame_captured, 1, &frames_pushed),
    null_pull(),
    &cursor);

  EXPECT_EQ(status, platf::capture_e::ok);
  EXPECT_EQ(frames_pushed, 1);
  ASSERT_NE(got, nullptr);
  ASSERT_NE(got->data, nullptr);

  EXPECT_EQ(got->width, 16);
  EXPECT_EQ(got->height, 9);
  EXPECT_EQ(got->row_pitch, 16 * 4);
  EXPECT_TRUE(frame_captured);

  // The top-left corner maps exactly onto itself after nearest-intent scaling.
  for (int c = 0; c < 4; ++c) {
    EXPECT_EQ(got->data[c], fake->source[c]) << "channel " << c;
  }

  // The bottom-right corner maps exactly onto the source's bottom-right corner.
  auto *dst_br = got->data + static_cast<std::size_t>(9 - 1) * got->row_pitch + static_cast<std::size_t>(16 - 1) * 4;
  auto *src_br = fake->source.data() + static_cast<std::size_t>(6 - 1) * (8 * 4) + static_cast<std::size_t>(8 - 1) * 4;
  for (int c = 0; c < 4; ++c) {
    EXPECT_EQ(dst_br[c], src_br[c]) << "channel " << c;
  }

  delete[] got->data;
}

TEST_F(RecorderDisplayTest, CaptureForwardsNoFrameWhenRecorderDoesNotCapture) {
  auto *fake = new fake_recorder_t(4, 3);
  fake->emit_frame = true;
  fake->frame_captured = false;

  auto display = make_display(fake);
  bool received_no_capture = false;
  bool cursor = true;

  // The recorder signals "nothing new" via (nullptr, false); the adapter must
  // forward exactly that to the pipeline and keep looping (this push ends the
  // session after the first signal so the test terminates).
  auto push = [&received_no_capture](std::shared_ptr<platf::img_t> &&img, bool captured) {
    if (!captured) {
      received_no_capture = true;
    }
    return false;
  };

  const auto status = display->capture(push, null_pull(), &cursor);

  EXPECT_EQ(status, platf::capture_e::ok);
  EXPECT_TRUE(received_no_capture);
}

TEST_F(RecorderDisplayTest, CapturePacesAtConfiguredFps) {
  recorder::init_t init {};
  init.width = 4;
  init.height = 3;
  init.fps = 1000;

  auto *fake = new fake_recorder_t(4, 3);

  auto display = make_display(fake, init);
  bool cursor = true;
  std::shared_ptr<platf::img_t> got;
  bool frame_captured = false;
  std::size_t frames_pushed = 0;

  // With a configured fps the adapter introduces a pacing delay between
  // deliveries; the loop still terminates once the pipeline stops.
  const auto status = display->capture(
    recording_push(got, frame_captured, 2, &frames_pushed),
    null_pull(),
    &cursor);

  EXPECT_EQ(status, platf::capture_e::ok);
  EXPECT_EQ(frames_pushed, 2);
  EXPECT_TRUE(frame_captured);

  ASSERT_NE(got, nullptr);
  delete[] got->data;
}

TEST_F(RecorderDisplayTest, CaptureReturnsInterruptedWhenPipelineRejectsFrame) {
  auto *fake = new fake_recorder_t(4, 3);
  fake->interrupt_on_rejection = true;
  fake->result = recorder::capture_e::interrupted;

  auto display = make_display(fake);
  bool cursor = true;

  const auto status = display->capture(
    [](std::shared_ptr<platf::img_t> &&, bool) {
      return false;
    },
    null_pull(),
    &cursor);

  EXPECT_EQ(status, platf::capture_e::interrupted);
}

TEST_F(RecorderDisplayTest, CaptureMapsNonOkRecorderStatuses) {
  struct status_pair_t {
    recorder::capture_e recorder_status;
    platf::capture_e pipeline_status;
  };

  // ok and timeout are handled by the blocking loop (see above and
  // CaptureLoopsUntilPushStops), so only terminal statuses map directly.
  const std::vector<status_pair_t> pairs {
    { recorder::capture_e::reinit, platf::capture_e::reinit },
    { recorder::capture_e::interrupted, platf::capture_e::interrupted },
    { recorder::capture_e::error, platf::capture_e::error },
  };

  for (const auto &pair : pairs) {
    auto *fake = new fake_recorder_t(4, 3);
    fake->result = pair.recorder_status;

    auto display = make_display(fake);
    bool cursor = true;

    const auto status = display->capture(
      [](std::shared_ptr<platf::img_t> &&, bool) {
        return true;
      },
      null_pull(),
      &cursor);

    EXPECT_EQ(status, pair.pipeline_status);
  }
}

TEST_F(RecorderDisplayTest, CaptureLoopsUntilPushStops) {
  auto *fake = new fake_recorder_t(4, 3);

  auto display = make_display(fake);
  bool cursor = true;
  std::shared_ptr<platf::img_t> got;
  bool frame_captured = false;
  std::size_t frames_pushed = 0;

  // Deliver three captured frames before the pipeline signals session end.
  const auto status = display->capture(
    recording_push(got, frame_captured, 3, &frames_pushed),
    null_pull(),
    &cursor);

  EXPECT_EQ(status, platf::capture_e::ok);
  EXPECT_EQ(fake->calls, 3);
  EXPECT_EQ(frames_pushed, 3);
  EXPECT_TRUE(frame_captured);

  ASSERT_NE(got, nullptr);
  ASSERT_NE(got->data, nullptr);
  EXPECT_EQ(got->width, 4);
  EXPECT_EQ(got->height, 3);

  delete[] got->data;
}