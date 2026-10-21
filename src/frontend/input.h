#pragma once

#include <SDL3/SDL.h>

#include "core/keyboard.h"
#include "core/types.h"

namespace mynext::frontend {

// Maps the host keyboard onto the Spectrum's 8x5 matrix and reads a gamepad as
// a Kempston joystick.
class Input {
public:
  void openGamepads();
  void closeGamepads();

  // Returns true if the key belongs to the emulated machine.
  bool handleKey(SDL_Keycode key, bool down, Keyboard& keyboard);
  void pollGamepads(u8& kempston);

private:
  SDL_Gamepad* pad_ = nullptr;
  u8 arrowMask_ = 0;
};

} // namespace mynext::frontend
