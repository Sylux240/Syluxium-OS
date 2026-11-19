#pragma once

// ACPI Embedded Controller (laptops: battery, AC, lid, hotkeys, fans).
namespace acpi::ec {

void early_init(); // after the namespace is loaded, before it's initialized (ECDT)
void late_init();  // after initialization: the EC device, its events (GPE -> _Qxx)
bool present();

} // namespace acpi::ec
