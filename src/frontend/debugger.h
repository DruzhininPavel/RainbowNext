#pragma once

#include "core/machine.h"

namespace rainbow::frontend {

// Text fields and view state the debugger keeps between frames, so the input
// boxes do not reset on every draw.
struct DebuggerUi {
  char breakpointInput[8] = "";
  char memoryAddress[8] = "4000";
};

struct DebuggerRequest {
  bool visible = true;  // the window is still open
  bool resume = false;  // the user asked to continue running
};

// Runs one instruction; when that instruction is a call, runs until the call
// returns so that stepping does not descend into ROM routines.
void stepOverInstruction(Machine& machine);

// Draws the Dear ImGui debugger window. Stepping, breakpoints and memory edits
// are applied to `machine` directly; resuming has to go back to the frame loop,
// so it is reported through the returned request.
DebuggerRequest drawDebugger(Machine& machine, DebuggerUi& ui, bool* open);

} // namespace rainbow::frontend
