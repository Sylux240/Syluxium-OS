#pragma once

// Storage controller drivers; each registers itself with the device registry.
namespace ahci {
void register_driver();
}

namespace nvme {
void register_driver();
}
