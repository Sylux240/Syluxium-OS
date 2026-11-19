#pragma once

// ACPI through the AML interpreter (uACPI): power button, battery, AC
// adapter, lid. Started after PCI is up (AML may touch PCI config space).
namespace acpi {

void rearm_interrupts(); // uacpi_glue: unmask an SCI masked by the storm guard

void start_interpreter();
bool interpreter_ready();
void enter_s5(); // power off through uACPI (_PTS, wake GPEs off, S5); returns only on failure

} // namespace acpi
