#pragma once

// PS/2 keyboard (i8042 controller, IRQ 1). Also what USB keyboards look
// like on most PCs until there's a USB driver, thanks to firmware legacy
// emulation. Keys are translated (US layout) and fed to the console tty.
namespace ps2kbd {

bool init();

} // namespace ps2kbd
