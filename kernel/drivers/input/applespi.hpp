#pragma once

#include <lib/types.hpp>

// Apple SPI keyboard and trackpad: the built-in keyboard/trackpad of the
// MacBook (12-inch, 2015-2017: MacBook8,1 .. 10,1) and the MacBook Pro
// 2016-2017 (MacBookPro13,x / 14,x). One controller behind the Intel LPSS
// SPI port, described in ACPI as APP000D; it signals data through an ACPI
// GPE. Protocol as documented by Linux's applespi (256-byte packets with
// CRC-16, keyboard and multi-touch messages); our own code.
// 2018+ Macs put keyboard and trackpad behind the T2 chip (apple-bce, not
// handled here); MacBook Airs and the 2015 MacBook Pro use USB (bcm5974).
namespace applespi {

void init();          // after the AML interpreter and PCI
bool run_selftest();  // CRC and message decoding checked with sample packets (boot)

} // namespace applespi
