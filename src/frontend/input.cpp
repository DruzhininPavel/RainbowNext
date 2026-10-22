#include "frontend/input.h"

#include <cstdio>

namespace rainbow::frontend {
namespace {

struct KeyMapping {
  SDL_Keycode key;
  int row;
  int column;
};

// Row/column positions in the Spectrum matrix. Rows are selected by A8..A15,
// columns are data bits 0..4 of port 0xFE.
constexpr KeyMapping Mapping[] = {
    {SDLK_LSHIFT, 0, 0}, {SDLK_Z, 0, 1}, {SDLK_X, 0, 2}, {SDLK_C, 0, 3}, {SDLK_V, 0, 4},
    {SDLK_A, 1, 0},      {SDLK_S, 1, 1}, {SDLK_D, 1, 2}, {SDLK_F, 1, 3}, {SDLK_G, 1, 4},
    {SDLK_Q, 2, 0},      {SDLK_W, 2, 1}, {SDLK_E, 2, 2}, {SDLK_R, 2, 3}, {SDLK_T, 2, 4},
    {SDLK_1, 3, 0},      {SDLK_2, 3, 1}, {SDLK_3, 3, 2}, {SDLK_4, 3, 3}, {SDLK_5, 3, 4},
    {SDLK_0, 4, 0},      {SDLK_9, 4, 1}, {SDLK_8, 4, 2}, {SDLK_7, 4, 3}, {SDLK_6, 4, 4},
    {SDLK_P, 5, 0},      {SDLK_O, 5, 1}, {SDLK_I, 5, 2}, {SDLK_U, 5, 3}, {SDLK_Y, 5, 4},
    {SDLK_RETURN, 6, 0}, {SDLK_L, 6, 1}, {SDLK_K, 6, 2}, {SDLK_J, 6, 3}, {SDLK_H, 6, 4},
    {SDLK_SPACE, 7, 0},  {SDLK_RSHIFT, 7, 1}, {SDLK_M, 7, 2}, {SDLK_N, 7, 3}, {SDLK_B, 7, 4},
};

constexpr u8 KempstonRight = 0x01;
constexpr u8 KempstonLeft = 0x02;
constexpr u8 KempstonDown = 0x04;
constexpr u8 KempstonUp = 0x08;
constexpr u8 KempstonFire = 0x10;

struct PadKeyMapping {
  SDL_GamepadButton button;
  int row;
  int column;
};

// Plenty of Spectrum software only ever reads the matrix, so a pad that offers
// nothing but Kempston cannot play it. These four turn the face and shoulder
// buttons into the keys those games wait for.
constexpr PadKeyMapping PadKeys[] = {
    {SDL_GAMEPAD_BUTTON_WEST, 7, 0},           // X -> SPACE
    {SDL_GAMEPAD_BUTTON_NORTH, 6, 0},          // Y -> ENTER
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0, 0},  // LB -> CAPS SHIFT
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 7, 1}, // RB -> SYMBOL SHIFT
};

// The left stick has to travel well past a thumb resting on it before it counts
// as a direction, which is why a pad sits idle instead of walking left.
constexpr float StickThreshold = 16000.0f;

u8 arrowBit(SDL_Keycode key) {
  switch (key) {
    case SDLK_RIGHT: return KempstonRight;
    case SDLK_LEFT: return KempstonLeft;
    case SDLK_DOWN: return KempstonDown;
    case SDLK_UP: return KempstonUp;
    default: return 0;
  }
}

} // namespace

void Input::openGamepads() {
  closeGamepads();
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  if (ids == nullptr) return;
  for (int i = 0; i < count; ++i) {
    pad_ = SDL_OpenGamepad(ids[i]);
    if (pad_ != nullptr) break;
  }
  SDL_free(ids);
  if (pad_ != nullptr) std::printf("input: gamepad '%s' connected\n", SDL_GetGamepadName(pad_));
}

void Input::closeGamepads() {
  if (pad_ != nullptr) {
    SDL_CloseGamepad(pad_);
    pad_ = nullptr;
  }
}

bool Input::handleKey(SDL_Keycode key, bool down, Keyboard& keyboard) {
  for (const KeyMapping& m : Mapping) {
    if (m.key == key) {
      keyboard.setKey(m.row, m.column, down);
      return true;
    }
  }

  const u8 bit = arrowBit(key);
  if (bit != 0) {
    if (down) {
      arrowMask_ |= bit;
    } else {
      arrowMask_ = u8(arrowMask_ & ~bit);
    }
    return true;
  }

  if (key == SDLK_LCTRL || key == SDLK_RCTRL) {
    if (down) {
      arrowMask_ |= KempstonFire;
    } else {
      arrowMask_ = u8(arrowMask_ & ~KempstonFire);
    }
    return true;
  }
  return false;
}

void Input::releasePadKeys(Keyboard& keyboard) {
  for (const PadKeyMapping& m : PadKeys) keyboard.setPadKey(m.row, m.column, false);
}

PadAction Input::pollGamepads(u8& kempston, Keyboard& keyboard) {
  u8 bits = arrowMask_;
  PadAction action = PadAction::None;

  if (pad_ == nullptr) {
    // The pad went away while a button was down, so its keys have to come up.
    releasePadKeys(keyboard);
    menuDown_ = false;
    pauseDown_ = false;
    kempston = bits;
    return action;
  }

  const float x = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTX);
  const float y = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTY);
  if (x < -StickThreshold) bits |= KempstonLeft;
  if (x > StickThreshold) bits |= KempstonRight;
  if (y < -StickThreshold) bits |= KempstonUp;
  if (y > StickThreshold) bits |= KempstonDown;

  if (SDL_GetGamepadButton(pad_, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) bits |= KempstonLeft;
  if (SDL_GetGamepadButton(pad_, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) bits |= KempstonRight;
  if (SDL_GetGamepadButton(pad_, SDL_GAMEPAD_BUTTON_DPAD_UP)) bits |= KempstonUp;
  if (SDL_GetGamepadButton(pad_, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) bits |= KempstonDown;

  if (SDL_GetGamepadButton(pad_, SDL_GAMEPAD_BUTTON_SOUTH)) bits |= KempstonFire;
  if (SDL_GetGamepadButton(pad_, SDL_GAMEPAD_BUTTON_EAST)) bits |= KempstonFire;

  for (const PadKeyMapping& m : PadKeys) {
    keyboard.setPadKey(m.row, m.column, SDL_GetGamepadButton(pad_, m.button));
  }

  const bool menu = SDL_GetGamepadButton(pad_, SDL_GAMEPAD_BUTTON_START);
  const bool pause = SDL_GetGamepadButton(pad_, SDL_GAMEPAD_BUTTON_BACK);
  if (menu && !menuDown_) {
    action = PadAction::Menu;
  } else if (pause && !pauseDown_) {
    action = PadAction::Pause;
  }
  menuDown_ = menu;
  pauseDown_ = pause;

  kempston = bits;
  return action;
}

} // namespace rainbow::frontend
