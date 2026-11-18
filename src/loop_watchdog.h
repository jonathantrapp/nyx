// loop_watchdog.h - recovery if Arduino's loop task stops making progress.
#pragma once
#include <stdint.h>

void loopWatchdogBegin();  // call from setup(), after hardware/filesystem startup
void loopWatchdogFeed();   // call from loop(), or its progressing USB dump

// Unsubscribe only the calling loop task during an intentional blocking operation.
// Scope exit re-subscribes it, including after an early return. Idle-task checks
// and the global timeout are unchanged. Nested scopes are safe on the loop task.
class LoopWatchdogPause {
 public:
  LoopWatchdogPause();
  ~LoopWatchdogPause();
  LoopWatchdogPause(const LoopWatchdogPause&) = delete;
  LoopWatchdogPause& operator=(const LoopWatchdogPause&) = delete;

 private:
  bool resume_ = false;
};
