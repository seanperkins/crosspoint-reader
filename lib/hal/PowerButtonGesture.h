#pragma once

#include <cstdint>

// Resolves raw Power/Down button samples into one-shot power click gestures.
// Kept independent of Arduino and InputManager so timing and chord behavior can
// be covered by host-side tests.
class PowerButtonGesture {
 public:
  static constexpr uint32_t DOUBLE_CLICK_MS = 300;

  void update(bool doubleClickEnabled, uint32_t now, bool powerPressed, bool downPressed, bool powerReleased,
              bool downReleased) {
    singleClick = false;
    doubleClick = false;

    if (powerPressed && downPressed) {
      screenshotChordActive = true;
      pendingClick = false;
    }

    if (!doubleClickEnabled) {
      pendingClick = false;
      if (powerReleased) {
        if (screenshotChordActive || downPressed || downReleased) {
          screenshotChordActive = false;
        } else {
          singleClick = true;
        }
      }
      return;
    }

    if (pendingClick && now - pendingReleaseTime > DOUBLE_CLICK_MS) {
      pendingClick = false;
      singleClick = true;
    }

    if (!powerReleased) return;

    // Power + Down is the screenshot chord. Remember that it was active so
    // either release order is suppressed rather than becoming a power click.
    if (screenshotChordActive || downPressed || downReleased) {
      pendingClick = false;
      singleClick = false;
      screenshotChordActive = false;
      return;
    }

    if (pendingClick && now - pendingReleaseTime <= DOUBLE_CLICK_MS) {
      pendingClick = false;
      doubleClick = true;
    } else {
      pendingClick = true;
      pendingReleaseTime = now;
    }
  }

  [[nodiscard]] bool wasSingleClicked() const { return singleClick; }
  [[nodiscard]] bool wasDoubleClicked() const { return doubleClick; }

 private:
  bool pendingClick = false;
  bool singleClick = false;
  bool doubleClick = false;
  bool screenshotChordActive = false;
  uint32_t pendingReleaseTime = 0;
};
