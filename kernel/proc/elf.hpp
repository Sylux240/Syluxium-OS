#pragma once

#include <fs/vfs.hpp>
#include <proc/memory.hpp>

// ELF executable loader: ELF64 (x86_64) and ELF32 (i386, run in
// compatibility mode), static or with a PT_INTERP dynamic linker.
namespace elf {

struct LoadResult {
    u64 entry;      // where execution starts
    u64 stack_ptr;  // initial RSP (points at argc)
    bool compat;    // a 32-bit program: start it in compatibility mode
};

// Map `file` into `mem` and build the initial stack (argc, argv, envp,
// auxiliary vector) exactly as Linux does.
long load(proc::Memory* mem, vfs::Inode* file, const char* exec_path, const char* const* argv,
          const char* const* envp, LoadResult* out);

} // namespace elf
