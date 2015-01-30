// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_EVENTS_OZONE_EVDEV_DEVICE_EVENT_DISPATCHER_H_
#define UI_EVENTS_OZONE_EVDEV_DEVICE_EVENT_DISPATCHER_H_

#include <vector>

#include "base/time/time.h"
#include "ui/events/devices/input_device.h"
#include "ui/events/devices/keyboard_device.h"
#include "ui/events/devices/touchscreen_device.h"
#include "ui/events/event_constants.h"
#include "ui/events/ozone/evdev/events_ozone_evdev_export.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/gfx/geometry/vector2d_f.h"

namespace ui {

struct EVENTS_OZONE_EVDEV_EXPORT KeyEventParams {
  KeyEventParams(int device_id, unsigned int code, bool down);
  KeyEventParams(const KeyEventParams& other);
  ~KeyEventParams();

  int device_id;
  unsigned int code;
  bool down;
};

struct EVENTS_OZONE_EVDEV_EXPORT MouseMoveEventParams {
  MouseMoveEventParams(int device_id, const gfx::PointF& location);
  MouseMoveEventParams(const MouseMoveEventParams& other);
  ~MouseMoveEventParams();

  int device_id;
  gfx::PointF location;
};

struct EVENTS_OZONE_EVDEV_EXPORT MouseButtonEventParams {
  MouseButtonEventParams(int device_id,
                         const gfx::PointF& location,
                         unsigned int button,
                         bool down,
                         bool allow_remap);
  MouseButtonEventParams(const MouseButtonEventParams& other);
  ~MouseButtonEventParams();

  int device_id;
  gfx::PointF location;
  unsigned int button;
  bool down;
  bool allow_remap;
};

struct EVENTS_OZONE_EVDEV_EXPORT MouseWheelEventParams {
  MouseWheelEventParams(int device_id,
                        const gfx::PointF& location,
                        const gfx::Vector2d& delta);
  MouseWheelEventParams(const MouseWheelEventParams& other);
  ~MouseWheelEventParams();

  int device_id;
  gfx::PointF location;
  gfx::Vector2d delta;
};

struct EVENTS_OZONE_EVDEV_EXPORT ScrollEventParams {
  ScrollEventParams(int device_id,
                    EventType type,
                    const gfx::PointF location,
                    const gfx::Vector2dF delta,
                    const gfx::Vector2dF ordinal_delta,
                    int finger_count,
                    const base::TimeDelta timestamp);
  ScrollEventParams(const ScrollEventParams& other);
  ~ScrollEventParams();

  int device_id;
  EventType type;
  const gfx::PointF location;
  const gfx::Vector2dF delta;
  const gfx::Vector2dF ordinal_delta;
  int finger_count;
  const base::TimeDelta timestamp;
};

struct EVENTS_OZONE_EVDEV_EXPORT TouchEventParams {
  TouchEventParams(int device_id,
                   int touch_id,
                   EventType type,
                   const gfx::PointF& location,
                   const gfx::Vector2dF& radii,
                   float pressure,
                   const base::TimeDelta& timestamp);
  TouchEventParams(const TouchEventParams& other);
  ~TouchEventParams();

  int device_id;
  int touch_id;
  EventType type;
  gfx::PointF location;
  gfx::Vector2dF radii;
  float pressure;
  base::TimeDelta timestamp;
};

// Interface used by device objects for event dispatch.
class EVENTS_OZONE_EVDEV_EXPORT DeviceEventDispatcherEvdev {
 public:
  DeviceEventDispatcherEvdev() {}
  virtual ~DeviceEventDispatcherEvdev() {}

  // User input events.
  virtual void DispatchKeyEvent(const KeyEventParams& params) = 0;
  virtual void DispatchMouseMoveEvent(const MouseMoveEventParams& params) = 0;
  virtual void DispatchMouseButtonEvent(
      const MouseButtonEventParams& params) = 0;
  virtual void DispatchMouseWheelEvent(const MouseWheelEventParams& params) = 0;
  virtual void DispatchScrollEvent(const ScrollEventParams& params) = 0;
  virtual void DispatchTouchEvent(const TouchEventParams& params) = 0;

  // Device lifecycle events.
  virtual void DispatchKeyboardDevicesUpdated(
      const std::vector<KeyboardDevice>& devices) = 0;
  virtual void DispatchTouchscreenDevicesUpdated(
      const std::vector<TouchscreenDevice>& devices) = 0;
  virtual void DispatchMouseDevicesUpdated(
      const std::vector<InputDevice>& devices) = 0;
  virtual void DispatchTouchpadDevicesUpdated(
      const std::vector<InputDevice>& devices) = 0;
};

}  // namespace ui

#endif  // UI_EVENTS_OZONE_EVDEV_DEVICE_EVENT_DISPATCHER_H_
