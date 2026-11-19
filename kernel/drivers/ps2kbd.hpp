#pragma once

// PS/2 keyboard (i8042 controller, IRQ 1). Also what USB keyboards look
// like on most PCs until there's a USB driver, thanks to firmware legacy
// emulation. Keys are translated (US layout) and fed to the console tty.
namespace ps2kbd {

bool init();
bool mouse_present();
void late_init(); // after the USB BIOS handoff: set the controller up again; polling fallback

// Feed a key (Linux key code) to the console terminal; used by every keyboard driver.
void console_key(u16 keycode, bool pressed);

} // namespace ps2kbd
