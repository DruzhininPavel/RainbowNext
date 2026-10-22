#pragma once

#include "core/machine.h"

namespace rainbow::frontend {

// Draws the Dear ImGui debugger window. Returns false when the user closed it.
bool drawDebugger(Machine& machine, bool* open);

} // namespace rainbow::frontend
