#pragma once

#include <fs/vfs.hpp>

// The system console terminal (/dev/console, /dev/tty).
//
// Output goes to the framebuffer console and the serial port. Input comes
// from the serial port for now (the PS/2 and USB keyboard drivers will feed
// the same line discipline in phase 5). Canonical mode gives line editing
// with echo and backspace, like a Linux tty.
namespace tty {

// False while an X server owns the keyboard (KDSKBMODE K_OFF/K_RAW).
bool keyboard_to_tty();
void set_keyboard_to_tty(bool on);

void init();                       // enable serial receive interrupts
void input_char(char c);           // called by input drivers (interrupt-safe)
vfs::Inode* console_inode();
void set_foreground_group(int pgid); // the job that receives Ctrl+C

} // namespace tty
