/**
 * @file src/recorder/recorder_glue.h
 * @brief Recorder adapter that wraps recorder implementations for Sunshine.
 *
 * This header provides the adapter that connects the recorder plugin
 * interface to Sunshine's platform display interface.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

// local includes
#include "recorder_api.h"
#include "src/platform/common.h"

namespace recorder {

  /**
   * @brief Recorder display adapter that wraps a recorder implementation.
   *
   * Implements platf::display_t by delegating capture() to the underlying
   * recorder::recorder_t implementation. Frame scaling, cursor forwarding,
   * and frame pacing are handled here so the DLL implementation stays
   * dependency-free.
   */
  class recorder_display_t : public platf::display_t {
  public:
    /**
     * @brief Construct a recorder display adapter.
     *
     * @param recorder Recorder implementation to wrap.
     * @param init     Recorder configuration (dimensions, fps, etc.).
     */
    explicit recorder_display_t(
      recorder::recorder_ptr_t&& recorder,
      const recorder::init_t& init = {}) :
        m_recorder(std::move(recorder)),
        m_init(init) {
      // Initialize display dimensions from init if provided
      if (init.width > 0 && init.height > 0) {
        width = init.width;
        height = init.height;
        logical_width = init.width;
        logical_height = init.height;
      }
    }

    /**
     * @brief Allocate an image buffer for capture.
     *
     * @return Allocated image or nullptr if dimensions not set.
     */
    std::shared_ptr<platf::img_t> alloc_img() override {
      if (width == 0 || height == 0) {
        return nullptr;
      }

      auto img = std::make_shared<platf::img_t>();
      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;  // BGRA
      img->row_pitch = width * 4;
      img->data = new std::uint8_t[static_cast<std::size_t>(img->row_pitch) * height];
      std::memset(img->data, 0, static_cast<std::size_t>(img->row_pitch) * height);
      return img;
    }

    /**
     * @brief Zero out an image buffer.
     *
     * @param img Image to zero; may be nullptr.
     * @return 0 on success.
     */
    int dummy_img(platf::img_t* img) override {
      if (img && img->data) {
        std::memset(img->data, 0, static_cast<std::size_t>(img->row_pitch) * img->height);
      }
      return 0;
    }

    /**
     * @brief Create an avcodec encoding device.
     *
     * @param pix_fmt Pixel format.
     * @return Encoding device.
     */
    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
      (void) pix_fmt;
      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    /**
     * @brief Capture frames and deliver them to the pipeline.
     *
     * Drives the underlying recorder in a loop: each iteration pulls a frame
     * from the recorder, copies/scales its pixels into a host-owned buffer,
     * and forwards it to the pipeline via push_cb. The loop terminates when
     * the pipeline stops accepting frames, when the recorder signals a
     * terminal status, or on pacing. Pixel data is copied (never aliased) so
     * the recorder's buffer can be freed as soon as the push callback returns.
     *
     * @param push_cb Callback to deliver a captured frame to the pipeline.
     * @param pull_cb Callback to obtain an empty image buffer.
     * @param cursor  Pointer to cursor visibility flag.
     * @return Capture status.
     */
    platf::capture_e capture(
      const platf::display_t::push_captured_image_cb_t& push_cb,
      const platf::display_t::pull_free_image_cb_t& pull_cb,
      bool* cursor) override {

      if (!m_recorder) {
        return platf::capture_e::error;
      }

      bool pipeline_stopped = false;
      bool first_iteration = true;

      while (!pipeline_stopped) {
        auto rec_push = [&](std::shared_ptr<recorder::img_t> img, bool frame_captured) -> bool {
          // Forward an explicit "no frame" stop signal straight through.
          if (!img || !img->data) {
            const bool accepted = push_cb(nullptr, false);
            pipeline_stopped = !accepted;
            return accepted;
          }

          // Build a host-owned platform frame and copy/scale the recorder's
          // pixels into it so the recorder may free its buffer immediately.
          auto plat_img = std::make_shared<platf::img_t>();
          plat_img->width = width > 0 ? width : img->width;
          plat_img->height = height > 0 ? height : img->height;
          plat_img->pixel_pitch = 4;
          plat_img->row_pitch = plat_img->width * 4;
          const auto bytes = static_cast<std::size_t>(plat_img->row_pitch) * plat_img->height;
          plat_img->data = new std::uint8_t[bytes];
          scale_to_platform(img.get(), plat_img.get());
          plat_img->frame_timestamp = std::chrono::steady_clock::now();

          const bool accepted = push_cb(std::move(plat_img), frame_captured);
          pipeline_stopped = !accepted;
          return accepted;
        };

        auto rec_pull = [&](std::shared_ptr<recorder::img_t>& img_out) -> bool {
          img_out = std::make_shared<recorder::img_t>();
          img_out->width = width;
          img_out->height = height;
          img_out->pixel_pitch = 4;
          img_out->row_pitch = width * 4;
          img_out->data = new std::uint8_t[static_cast<std::size_t>(img_out->row_pitch) * height];
          std::memset(img_out->data, 0, static_cast<std::size_t>(img_out->row_pitch) * height);
          return true;
        };

        const auto status = m_recorder->capture(rec_push, rec_pull, cursor);
        if (status != recorder::capture_e::ok && status != recorder::capture_e::timeout) {
          return map_status(status);
        }

        if (pipeline_stopped) {
          return platf::capture_e::ok;
        }

        if (!first_iteration && m_init.fps > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1000 / m_init.fps));
        }
        first_iteration = false;
      }

      return platf::capture_e::ok;
    }

  private:
    /**
     * @brief Scale and copy recorder pixels into a platform image buffer.
     *
     * Uses nearest-neighbor sampling when the recorder frame and the
     * display dimensions differ; a straight row copy when they match.
     *
     * @param src Source recorder frame (non-null, with data).
     * @param dst Destination platform frame (non-null, with data).
     */
    static void scale_to_platform(const recorder::img_t* src, platf::img_t* dst) {
      const int dst_w = dst->width;
      const int dst_h = dst->height;
      const int src_w = src->width;
      const int src_h = src->height;

      if (dst_w == src_w && dst_h == src_h) {
        const auto bytes = static_cast<std::size_t>(dst->row_pitch) * dst_h;
        std::memcpy(dst->data, src->data, bytes);
        return;
      }

      const auto src_pitch = static_cast<std::size_t>(std::max(src->row_pitch, 1));
      for (int y = 0; y < dst_h; ++y) {
        const int sy = dst_h * src_h > 0 ? y * src_h / dst_h : 0;
        auto* dst_row = dst->data + static_cast<std::size_t>(y) * dst->row_pitch;
        const auto* src_row = src->data + static_cast<std::size_t>(sy) * src_pitch;
        for (int x = 0; x < dst_w; ++x) {
          const int sx = dst_w * src_w > 0 ? x * src_w / dst_w : 0;
          const auto* px = src_row + static_cast<std::size_t>(sx) * 4;
          std::memcpy(dst_row + static_cast<std::size_t>(x) * 4, px, 4);
        }
      }
    }

    /**
     * @brief Map a terminal recorder status to the platform status.
     *
     * @param status Recorder status to map.
     * @return Mapped platform status.
     */
    static platf::capture_e map_status(recorder::capture_e status) {
      switch (status) {
        case recorder::capture_e::reinit:
          return platf::capture_e::reinit;
        case recorder::capture_e::timeout:
          return platf::capture_e::timeout;
        case recorder::capture_e::interrupted:
          return platf::capture_e::interrupted;
        case recorder::capture_e::error:
        default:
          return platf::capture_e::error;
      }
    }

  private:
    recorder::recorder_ptr_t m_recorder;
    recorder::init_t m_init;
  };

}  // namespace recorder
