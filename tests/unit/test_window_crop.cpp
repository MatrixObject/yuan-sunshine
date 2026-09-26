/**
 * @file tests/unit/test_window_crop.cpp
 * @brief Tests for the window chrome crop region computation.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <cstdint>

// local includes
#include "src/platform/windows/display.h"

namespace {

  platf::dxgi::window_crop_t compute_crop(int client_left, int client_top, int client_width, int client_height, int source_width, int source_height) {
    return platf::dxgi::compute_window_crop(client_left, client_top, client_width, client_height, source_width, source_height);
  }

}  // namespace

namespace test_window_crop {

  TEST(WindowCropTest, CenteredClientNoClamping) {
    auto crop = compute_crop(8, 32, 1000, 500, 1016, 564);
    EXPECT_EQ(crop.x, 8);
    EXPECT_EQ(crop.y, 32);
    EXPECT_EQ(crop.width, 1000);
    EXPECT_EQ(crop.height, 500);
  }

  TEST(WindowCropTest, ZeroChromeFrameWithoutBorders) {
    auto crop = compute_crop(0, 0, 1920, 1080, 1920, 1080);
    EXPECT_EQ(crop.x, 0);
    EXPECT_EQ(crop.y, 0);
    EXPECT_EQ(crop.width, 1920);
    EXPECT_EQ(crop.height, 1080);
  }

  TEST(WindowCropTest, ClampsNegativeClientOrigin) {
    auto crop = compute_crop(-32, -16, 1000, 500, 984, 516);
    EXPECT_EQ(crop.x, 0);
    EXPECT_EQ(crop.y, 0);
    EXPECT_EQ(crop.width, 984);
    EXPECT_EQ(crop.height, 500);
  }

  TEST(WindowCropTest, ClampsClientAreaLargerThanFrame) {
    auto crop = compute_crop(12, 24, 2000, 1200, 1016, 564);
    EXPECT_EQ(crop.x, 12);
    EXPECT_EQ(crop.y, 24);
    EXPECT_EQ(crop.width, 1004);
    EXPECT_EQ(crop.height, 540);
  }

  TEST(WindowCropTest, ClampsRightAndBottomEdges) {
    auto crop = compute_crop(1000, 600, 500, 300, 1016, 564);
    EXPECT_EQ(crop.x, 1000);
    EXPECT_EQ(crop.y, 564);
    EXPECT_EQ(crop.width, 16);
    EXPECT_EQ(crop.height, 0);
  }

  TEST(WindowCropTest, DegenerateSourceSize) {
    auto crop = compute_crop(8, 32, 1000, 500, 0, 0);
    EXPECT_EQ(crop.x, 0);
    EXPECT_EQ(crop.y, 0);
    EXPECT_EQ(crop.width, 0);
    EXPECT_EQ(crop.height, 0);
  }

}  // namespace test_window_crop
