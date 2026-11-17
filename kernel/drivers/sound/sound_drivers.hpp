#pragma once

// Sound card drivers (they publish cards through the sound core, kernel/sound/).
namespace hda {
void register_driver(); // Intel High Definition Audio controllers (PCI class 0x0403)
}
