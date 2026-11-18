// menu_logic.h - the settings menu as a state machine. Pure logic, no Arduino, no screen.
// Turn the knob to move, press to edit a value, turn to change it, press again to confirm,
// hold to leave (or cancel an edit). tools/test_tft_native.py exercises every path.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum MenuItem : uint8_t { MI_VOLUME, MI_BRIGHTNESS, MI_MODE, MI_BLE, MI_UTC, MI_EXIT, MI_COUNT };

struct MenuValues {
  int volume = 100;        // 0..100
  int brightness = 100;    // 10..100 (never fully dark: you could not get back to the menu)
  int mode = 0;            // 0 research, 1 car
  int ble = 1;             // 0/1
  int utcHours = 0;        // -12..14
};

// What a menu action did, for the owner to apply. Volume and brightness are applied while editing
// (you hear/see the change); the rest only when `commit` is set.
struct MenuEvent {
  int8_t item = -1;        // item whose value changed, or -1
  bool commit = false;     // the edit was confirmed
  bool cancel = false;     // the edit was abandoned and the old value restored
  bool opened = false;
  bool closed = false;
};

class MenuLogic {
 public:
  static constexpr uint32_t TIMEOUT_MS = 20000;

  MenuValues values;       // the owner keeps these in sync with the real settings

  bool isOpen() const { return open_; }
  bool editing() const { return editing_; }
  int selected() const { return sel_; }

  MenuEvent open(uint32_t now) {
    MenuEvent e;
    if (!open_) { open_ = true; editing_ = false; sel_ = MI_VOLUME; e.opened = true; }
    last_ = now;
    return e;
  }
  MenuEvent close() {
    MenuEvent e;
    if (editing_) e = restore();
    if (open_) e.closed = true;
    open_ = false;
    editing_ = false;
    return e;
  }

  MenuEvent rotate(int steps, uint32_t now) {
    MenuEvent e;
    if (!open_ || steps == 0) return e;
    last_ = now;
    if (!editing_) {
      int s = sel_ + (steps > 0 ? 1 : -1) * (steps > 0 ? steps : -steps);
      s = ((s % MI_COUNT) + MI_COUNT) % MI_COUNT;   // wraps around
      sel_ = static_cast<uint8_t>(s);
      return e;
    }
    int* v = nullptr;
    int lo = 0, hi = 1, step = 1;
    switch (sel_) {
      case MI_VOLUME:     v = &values.volume;     lo = 0;   hi = 100; step = 5; break;
      case MI_BRIGHTNESS: v = &values.brightness; lo = 10;  hi = 100; step = 10; break;
      case MI_MODE:       v = &values.mode;       break;
      case MI_BLE:        v = &values.ble;        break;
      case MI_UTC:        v = &values.utcHours;   lo = -12; hi = 14;  step = 1; break;
      default: return e;
    }
    const int before = *v;
    if (hi == 1 && lo == 0) *v = steps > 0 ? 1 : 0;            // on/off style: any turn picks a side
    else { *v += steps * step; if (*v < lo) *v = lo; if (*v > hi) *v = hi; }
    if (*v != before) e.item = static_cast<int8_t>(sel_);
    return e;
  }

  MenuEvent press(uint32_t now) {
    MenuEvent e;
    if (!open_) return open(now);
    last_ = now;
    if (sel_ == MI_EXIT) return close();
    if (!editing_) {
      editing_ = true;
      backup_ = values;
    } else {
      editing_ = false;
      e.item = static_cast<int8_t>(sel_);
      e.commit = true;
    }
    return e;
  }

  // Held press: abandons an edit in progress, otherwise leaves the menu.
  MenuEvent longPress(uint32_t now) {
    (void)now;
    if (!open_) return MenuEvent();
    if (editing_) return restore();
    return close();
  }

  MenuEvent tick(uint32_t now) {
    if (open_ && static_cast<int32_t>(now - last_) > static_cast<int32_t>(TIMEOUT_MS)) return close();
    return MenuEvent();
  }

  static const char* label(int item) {
    static const char* const t[MI_COUNT] = {"Volume", "Brightness", "Logging", "Bluetooth", "Time zone", "Done"};
    return item >= 0 && item < MI_COUNT ? t[item] : "";
  }
  void valueText(int item, char* out, size_t n) const {
    switch (item) {
      case MI_VOLUME:     snprintf(out, n, "%d%%", values.volume); break;
      case MI_BRIGHTNESS: snprintf(out, n, "%d%%", values.brightness); break;
      case MI_MODE:       snprintf(out, n, "%s", values.mode ? "Car" : "Research"); break;
      case MI_BLE:        snprintf(out, n, "%s", values.ble ? "On" : "Off"); break;
      case MI_UTC:        snprintf(out, n, "UTC%+d", values.utcHours); break;
      default:            if (n) out[0] = 0; break;
    }
  }

 private:
  MenuEvent restore() {
    MenuEvent e;
    editing_ = false;
    int changed = -1;
    if (values.volume != backup_.volume || values.brightness != backup_.brightness) {
      changed = values.volume != backup_.volume ? MI_VOLUME : MI_BRIGHTNESS;
    }
    values = backup_;
    e.cancel = true;
    e.item = static_cast<int8_t>(changed >= 0 ? changed : sel_);
    return e;
  }

  bool open_ = false, editing_ = false;
  uint8_t sel_ = 0;
  uint32_t last_ = 0;
  MenuValues backup_;
};
