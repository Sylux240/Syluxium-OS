#pragma once

#include <lib/types.hpp>

// ACPI power control: power off (S5), restart, sleep (S3) state values.
namespace acpi {

void power_init(); // after acpi::init(): reads the FADT, finds \_S5_ / \_S3_

[[noreturn]] void power_off();
[[noreturn]] void restart();
void enable_acpi_mode(); // SMI_CMD <- ACPI_ENABLE if the firmware is still in legacy mode

bool has_8042(); // FADT IA-PC boot flags say there's a PS/2 controller (a hint only)

struct SleepState {
    u8 a, b; // SLP_TYP values for PM1a / PM1b
};
bool can_sleep();
SleepState s3_state();
u64 pm1a_control();
u64 pm1b_control();

} // namespace acpi
