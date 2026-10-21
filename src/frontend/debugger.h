#pragma once

#include "core/machine.h"

namespace mynext::frontend {

// Draws the Dear ImGui debugger window. Returns false when the user closed it.
bool drawDebugger(Machine& machine, bool* open);

} // namespace mynext::frontend
