#pragma once

// HID over I2C (Microsoft's i2c-hid protocol): the touchpads (precision
// touchpads), touchscreens and some keyboards of laptops since ~2015.
// Devices are found in ACPI (PNP0C50 / ACPI0C50) with their bus address and
// controller (DesignWare I2C: Intel LPSS or AMD FCH); reports go through the
// same HID code as USB devices. No GPIO interrupt driver yet: the input
// register is polled (~120 times a second).
namespace i2c_hid {

void init(); // after the AML interpreter (needs _CRS/_DSM) and PCI

} // namespace i2c_hid
