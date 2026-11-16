#pragma once

#include <fs/vfs.hpp>
#include <proc/memory.hpp>

// ELF64 executable loader (System V ABI, x86_64).
namespace elf {

struct LoadResult {
    u64 entry;      // where execution starts
    u64 stack_ptr;  // initial RSP (points at argc)
};

// Map `file` into `mem` and build the initial stack (argc, argv, envp,
// auxiliary vector) exactly as Linux does.
long load(proc::Memory* mem, vfs::Inode* file, const char* exec_path, const char* const* argv,
          const char* const* envp, LoadResult* out);

} // namespace elf
