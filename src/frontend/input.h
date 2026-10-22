#pragma once

#include <SDL3/SDL.h>

#include "core/keyboard.h"
#include "core/types.h"

namespace rainbow::frontend {

// A gamepad button that drives the emulator instead of the machine. Polled once
// a frame, so it fires on the press edge rather than for as long as it is held.
enum class PadAction { None, Menu, Pause };

// Maps the host keyboard onto the Spectrum's 8x5 matrix, and reads a gamepad as
// a Kempston joystick plus the handful of matrix keys it can stand in for.
class Input {
public:
  void openGamepads();
  void closeGamepads();

  // Returns true if the key belongs to the emulated machine.
  bool handleKey(SDL_Keycode key, bool down, Keyboard& keyboard);
  // Fills the Kempston byte and holds the matrix keys the pad is mapped to.
  PadAction pollGamepads(u8& kempston, Keyboard& keyboard);

private:
  void releasePadKeys(Keyboard& keyboard);

  SDL_Gamepad* pad_ = nullptr;
  u8 arrowMask_ = 0;
  bool menuDown_ = false;
  bool pauseDown_ = false;
};

} // namespace rainbow::frontend
