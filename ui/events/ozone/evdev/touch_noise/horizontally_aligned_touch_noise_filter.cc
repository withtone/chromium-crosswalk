// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/events/ozone/evdev/touch_noise/horizontally_aligned_touch_noise_filter.h"

#include <inttypes.h>
#include <cmath>

#include "base/logging.h"
#include "base/strings/stringprintf.h"

namespace ui {

namespace {

// The maximum horizontal distance between touches considered aligned.
int kMaxDistance = 3;

}  // namespace

void HorizontallyAlignedTouchNoiseFilter::Filter(
    const std::vector<InProgressTouchEvdev>& touches,
    base::TimeDelta time,
    std::bitset<kNumTouchEvdevSlots>* slots_with_noise) {
  for (const InProgressTouchEvdev& touch : touches) {
    // Only consider new touches.
    if (!touch.touching || touch.was_touching)
      continue;

    // Check if within kMaxDistance of an existing touch.
    for (const InProgressTouchEvdev& other_touch : touches) {
      if (touch.slot == other_touch.slot || !other_touch.touching)
        continue;
      if (std::abs(other_touch.x - touch.x) <= kMaxDistance) {
        VLOG(2) << base::StringPrintf("Cancel tracking id %d, down at %" PRId64
                                      " at %f,%f near touch %d at "
                                      "%f,%f",
                                      touch.tracking_id, time.ToInternalValue(),
                                      touch.x, touch.y, other_touch.tracking_id,
                                      other_touch.x, other_touch.y);
        slots_with_noise->set(touch.slot);
      }
    }
  }
}

}  // namespace ui
