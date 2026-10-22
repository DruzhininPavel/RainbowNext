#pragma once

#include <string>
#include <vector>

#include "formats/snapshot.h"

namespace rainbow::formats {

bool loadSna(const std::vector<u8>& data, Snapshot& out, std::string& error);

} // namespace rainbow::formats
