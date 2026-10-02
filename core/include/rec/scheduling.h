// CPU scheduling hygiene for the host (recorder plan §13.5): a recorder must get its CPU time even when a
// game is using all of it, or it loses frames, and Windows 11 may put a background process in "efficiency
// mode" (EcoQoS: lower priority and clock) unless it opts out.
#pragma once

#include <windows.h>

namespace rec {

// Opts this process out of power throttling (EcoQoS) and timer-resolution throttling. Harmless where
// the OS doesn't have them. Call once at start.
void disable_power_throttling();

// Applies the encoder priority setting ("normal" | "above-normal") to the calling thread, and opts the
// thread out of power throttling.
void set_encoder_thread_priority(bool above_normal);

}  // namespace rec
