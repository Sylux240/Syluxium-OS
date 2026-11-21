#pragma once
// SMBIOS (DMI): what the firmware says this computer is.
#include <lib/types.hpp>

namespace smbios {

// "LENOVO ThinkPad P53s", "Apple Inc. MacBookAir6,2", or the motherboard
// for desktops whose system fields say "To Be Filled By O.E.M.". Empty if
// the firmware has no SMBIOS. Cached after the first call.
const char* machine_name();

} // namespace smbios
