#pragma once

#include <string>
#include <vector>

#include "formats/snapshot.h"

namespace mynext::formats {

bool loadZ80(const std::vector<u8>& data, Snapshot& out, std::string& error);

} // namespace mynext::formats
