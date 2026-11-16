#pragma once

// Legacy 8259 PIC. We use the APIC instead (phase 3), but the PIC must be
// remapped away from the CPU exception vectors and masked so it can't fire
// stray interrupts.
namespace pic {

void disable();

} // namespace pic
