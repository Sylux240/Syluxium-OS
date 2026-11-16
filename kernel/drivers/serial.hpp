#pragma once

#include <lib/types.hpp>

// 16550 UART on COM1. Used as the early debug log; in QEMU it shows up in
// the terminal thanks to `-serial stdio`.
namespace serial {

bool init();
bool present();
void putc(char c);
void write(const char* s, usize len);

} // namespace serial
