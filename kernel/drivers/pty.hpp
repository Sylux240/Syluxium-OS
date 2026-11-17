#pragma once

// Pseudo-terminals: /dev/ptmx (each open creates a master) and /dev/pts/N.
namespace pty {

void init();

} // namespace pty
