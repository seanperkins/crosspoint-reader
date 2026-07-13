#include <gtest/gtest.h>

#include <cstdint>

#include "PowerButtonGesture.h"

namespace {

void sample(PowerButtonGesture& gesture, bool enabled, uint32_t now, bool powerPressed = false,
            bool downPressed = false, bool powerReleased = false, bool downReleased = false) {
  gesture.update(enabled, now, powerPressed, downPressed, powerReleased, downReleased);
}

}  // namespace

TEST(PowerButtonGesture, SingleClickIsImmediateWhenDoubleClickIsDisabled) {
  PowerButtonGesture gesture;

  sample(gesture, false, 100, false, false, true);

  EXPECT_TRUE(gesture.wasSingleClicked());
  EXPECT_FALSE(gesture.wasDoubleClicked());
  sample(gesture, false, 101);
  EXPECT_FALSE(gesture.wasSingleClicked());
}

TEST(PowerButtonGesture, SingleClickWaitsUntilWindowHasElapsed) {
  PowerButtonGesture gesture;

  sample(gesture, true, 100, false, false, true);
  EXPECT_FALSE(gesture.wasSingleClicked());
  sample(gesture, true, 100 + PowerButtonGesture::DOUBLE_CLICK_MS);
  EXPECT_FALSE(gesture.wasSingleClicked());
  sample(gesture, true, 101 + PowerButtonGesture::DOUBLE_CLICK_MS);
  EXPECT_TRUE(gesture.wasSingleClicked());
  EXPECT_FALSE(gesture.wasDoubleClicked());
}

TEST(PowerButtonGesture, SecondReleaseAtWindowBoundaryIsDoubleClick) {
  PowerButtonGesture gesture;

  sample(gesture, true, 100, false, false, true);
  sample(gesture, true, 100 + PowerButtonGesture::DOUBLE_CLICK_MS, false, false, true);

  EXPECT_FALSE(gesture.wasSingleClicked());
  EXPECT_TRUE(gesture.wasDoubleClicked());
}

TEST(PowerButtonGesture, ReleasesOutsideWindowBecomeTwoSingleClicks) {
  PowerButtonGesture gesture;

  sample(gesture, true, 100, false, false, true);
  sample(gesture, true, 401, false, false, true);
  EXPECT_TRUE(gesture.wasSingleClicked());
  EXPECT_FALSE(gesture.wasDoubleClicked());

  sample(gesture, true, 702);
  EXPECT_TRUE(gesture.wasSingleClicked());
  EXPECT_FALSE(gesture.wasDoubleClicked());
}

TEST(PowerButtonGesture, DisablingDetectionFlushesPendingClickWithoutEmittingIt) {
  PowerButtonGesture gesture;

  sample(gesture, true, 100, false, false, true);
  sample(gesture, false, 200);
  sample(gesture, true, 500);

  EXPECT_FALSE(gesture.wasSingleClicked());
  EXPECT_FALSE(gesture.wasDoubleClicked());
}

TEST(PowerButtonGesture, ScreenshotChordIsSuppressedWhenPowerIsReleasedFirst) {
  PowerButtonGesture gesture;

  sample(gesture, true, 100, true, true);
  sample(gesture, true, 110, false, true, true);
  EXPECT_FALSE(gesture.wasSingleClicked());
  EXPECT_FALSE(gesture.wasDoubleClicked());
  sample(gesture, true, 120, false, false, false, true);
  sample(gesture, true, 500);
  EXPECT_FALSE(gesture.wasSingleClicked());
}

TEST(PowerButtonGesture, ScreenshotChordIsSuppressedWhenDownIsReleasedFirst) {
  PowerButtonGesture gesture;

  sample(gesture, true, 100, true, true);
  sample(gesture, true, 110, true, false, false, true);
  sample(gesture, true, 120, false, false, true);
  EXPECT_FALSE(gesture.wasSingleClicked());
  EXPECT_FALSE(gesture.wasDoubleClicked());
  sample(gesture, true, 500);
  EXPECT_FALSE(gesture.wasSingleClicked());
}

TEST(PowerButtonGesture, ScreenshotChordIsSuppressedWhenDoubleClickIsDisabled) {
  PowerButtonGesture gesture;

  sample(gesture, false, 100, true, true);
  sample(gesture, false, 110, true, false, false, true);
  sample(gesture, false, 120, false, false, true);

  EXPECT_FALSE(gesture.wasSingleClicked());
  EXPECT_FALSE(gesture.wasDoubleClicked());
}

TEST(PowerButtonGesture, ScreenshotChordCancelsAPendingSingleClick) {
  PowerButtonGesture gesture;

  sample(gesture, true, 100, false, false, true);
  sample(gesture, true, 200, true, true);
  sample(gesture, true, 210, false, true, true);
  sample(gesture, true, 500);

  EXPECT_FALSE(gesture.wasSingleClicked());
  EXPECT_FALSE(gesture.wasDoubleClicked());
}

TEST(PowerButtonGesture, TimingWorksAcrossMillisRollover) {
  PowerButtonGesture gesture;
  constexpr uint32_t firstRelease = UINT32_MAX - 100;

  sample(gesture, true, firstRelease, false, false, true);
  sample(gesture, true, 50, false, false, true);

  EXPECT_TRUE(gesture.wasDoubleClicked());
  EXPECT_FALSE(gesture.wasSingleClicked());
}
