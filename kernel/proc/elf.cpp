#include <arch/x86_64/cpu.hpp>
#include <core/log.hpp>
#include <lib/random.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
#include <proc/elf.hpp>

namespace elf {

namespace {

struct Elf64Header {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
};
static_assert(sizeof(Elf64Header) == 64);

struct Elf64Phdr {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
};
static_assert(sizeof(Elf64Phdr) == 56);

constexpr u16 ET_EXEC = 2, ET_DYN = 3;
constexpr u16 EM_X86_64 = 62;
constexpr u32 PT_LOAD = 1, PT_INTERP = 3, PT_PHDR = 6;
constexpr u32 PF_X = 1, PF_W = 2, PF_R = 4;

// Auxiliary vector keys.
constexpr u64 AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6, AT_BASE = 7,
              AT_FLAGS = 8, AT_ENTRY = 9, AT_UID = 11, AT_EUID = 12, AT_GID = 13, AT_EGID = 14,
              AT_PLATFORM = 15, AT_HWCAP = 16, AT_CLKTCK = 17, AT_SECURE = 23, AT_RANDOM = 25,
              AT_HWCAP2 = 26, AT_EXECFN = 31;

constexpr u64 PIE_BASE = 0x555555554000; // where Linux puts position-independent executables
constexpr usize STACK_IMAGE_MAX = 128 * KiB;

u32 segment_prot(u32 pflags) {
    u32 prot = 0;
    if (pflags & PF_R) prot |= proc::PROT_READ;
    if (pflags & PF_W) prot |= proc::PROT_WRITE;
    if (pflags & PF_X) prot |= proc::PROT_EXEC;
    return prot;
}

// Builds the top of the initial stack in a kernel buffer, growing downward,
// then copies it into the new address space in one go.
class StackBuilder {
public:
    StackBuilder() : m_buf(static_cast<u8*>(kmalloc(STACK_IMAGE_MAX))) {}
    ~StackBuilder() { kfree(m_buf); }

    bool ok() const { return m_buf && !m_overflow; }
    u64 sp() const { return proc::STACK_TOP - m_used; }

    u64 push_bytes(const void* data, usize len) {
        if (m_used + len > STACK_IMAGE_MAX) {
            m_overflow = true;
            return 0;
        }
        m_used += len;
        memcpy(m_buf + STACK_IMAGE_MAX - m_used, data, len);
        return sp();
    }
    u64 push_string(const char* s) { return push_bytes(s, strlen(s) + 1); }
    void push_u64(u64 v) { push_bytes(&v, 8); }
    void align(usize a) {
        usize pad = (sp() & (a - 1));
        if (pad) {
            u8 zeros[16] = {};
            push_bytes(zeros, pad);
        }
    }
    long commit(proc::Memory* mem) {
        return mem->write_remote(sp(), m_buf + STACK_IMAGE_MAX - m_used, m_used);
    }

private:
    u8* m_buf;
    usize m_used = 0;
    bool m_overflow = false;
};

usize count(const char* const* v) {
    usize n = 0;
    while (v && v[n])
        n++;
    return n;
}

// Map every PT_LOAD segment of an ELF image at `base`. Fills in the
// highest mapped address and the run-time address of the program headers.
long map_segments(proc::Memory* mem, vfs::Inode* file, const Elf64Header& eh, const Elf64Phdr* phdrs, u64 base,
                  u64* highest, u64* phdr_addr) {
    *highest = 0;
    *phdr_addr = 0;
    for (u32 i = 0; i < eh.phnum; i++) {
        const Elf64Phdr& ph = phdrs[i];
        if (ph.type == PT_PHDR)
            *phdr_addr = base + ph.vaddr;
        if (ph.type != PT_LOAD || ph.memsz == 0)
            continue;
        if (ph.filesz > ph.memsz || (ph.vaddr & (mm::PAGE_SIZE - 1)) != (ph.offset & (mm::PAGE_SIZE - 1)))
            return -err::NOEXEC;

        u64 page_vaddr = align_down<u64>(ph.vaddr, mm::PAGE_SIZE);
        u64 page_offset = align_down<u64>(ph.offset, mm::PAGE_SIZE);
        u64 lead = ph.vaddr - page_vaddr;
        u64 start = base + page_vaddr;
        u64 end = base + align_up<u64>(ph.vaddr + ph.memsz, mm::PAGE_SIZE);
        if (end > proc::STACK_TOP - proc::STACK_SIZE || start < proc::USER_MIN)
            return -err::NOEXEC;

        uptr mapped;
        long r = mem->map(start, end - start, segment_prot(ph.flags), proc::MAP_PRIVATE | proc::MAP_FIXED, file,
                          page_offset, lead + ph.filesz, &mapped);
        if (r < 0)
            return r;
        *highest = max(*highest, end);

        if (!*phdr_addr && eh.phoff >= ph.offset && eh.phoff < ph.offset + ph.filesz)
            *phdr_addr = base + ph.vaddr + (eh.phoff - ph.offset);
    }
    return *highest ? 0 : -err::NOEXEC;
}

long read_headers(vfs::Inode* file, Elf64Header* eh, Elf64Phdr* phdrs) {
    if (file->read(0, eh, sizeof(*eh)) != sizeof(*eh))
        return -err::NOEXEC;
    if (memcmp(eh->ident, "\x7f" "ELF", 4) != 0 || eh->ident[4] != 2 /* 64-bit */ || eh->ident[5] != 1 /* LE */)
        return -err::NOEXEC;
    if (eh->machine != EM_X86_64 || (eh->type != ET_EXEC && eh->type != ET_DYN))
        return -err::NOEXEC;
    if (eh->phentsize != sizeof(Elf64Phdr) || eh->phnum == 0 || eh->phnum > 64)
        return -err::NOEXEC;
    usize ph_bytes = eh->phnum * sizeof(Elf64Phdr);
    if (file->read(eh->phoff, phdrs, ph_bytes) != static_cast<isize>(ph_bytes))
        return -err::NOEXEC;
    return 0;
}

// Load the program interpreter (ld-linux.so) at an address mmap picks.
long load_interpreter(proc::Memory* mem, const char* path, u64* base_out, u64* entry_out) {
    vfs::Inode* file;
    if (long r = vfs::resolve(path, nullptr, true, &file); r < 0)
        return r;
    Elf64Header eh;
    Elf64Phdr phdrs[64];
    if (long r = read_headers(file, &eh, phdrs); r < 0)
        return r;
    if (eh.type != ET_DYN)
        return -err::NOEXEC;

    // Reserve one region big enough for the whole image, then map the
    // segments into it (MAP_FIXED replaces the reservation piece by piece).
    u64 lo = ~0ull, hi = 0;
    for (u32 i = 0; i < eh.phnum; i++) {
        if (phdrs[i].type != PT_LOAD)
            continue;
        lo = min(lo, align_down<u64>(phdrs[i].vaddr, mm::PAGE_SIZE));
        hi = max(hi, align_up<u64>(phdrs[i].vaddr + phdrs[i].memsz, mm::PAGE_SIZE));
    }
    if (hi <= lo)
        return -err::NOEXEC;
    uptr region;
    if (long r = mem->map(0, hi - lo, proc::PROT_NONE, proc::MAP_PRIVATE | proc::MAP_ANONYMOUS, nullptr, 0, 0, &region);
        r < 0)
        return r;
    u64 base = region - lo;
    u64 highest, phdr_addr;
    if (long r = map_segments(mem, file, eh, phdrs, base, &highest, &phdr_addr); r < 0)
        return r;
    *base_out = base;
    *entry_out = base + eh.entry;
    return 0;
}

} // namespace

long load(proc::Memory* mem, vfs::Inode* file, const char* exec_path, const char* const* argv,
          const char* const* envp, LoadResult* out) {
    Elf64Header eh;
    Elf64Phdr phdrs[64];
    if (long r = read_headers(file, &eh, phdrs); r < 0)
        return r;

    char interp[256] = {};
    for (u32 i = 0; i < eh.phnum; i++) {
        if (phdrs[i].type == PT_INTERP) {
            if (phdrs[i].filesz >= sizeof(interp))
                return -err::NOEXEC;
            file->read(phdrs[i].offset, interp, phdrs[i].filesz);
            interp[phdrs[i].filesz] = 0;
        }
    }

    u64 base = eh.type == ET_DYN ? PIE_BASE : 0;
    u64 highest, phdr_addr;
    if (long r = map_segments(mem, file, eh, phdrs, base, &highest, &phdr_addr); r < 0)
        return r;

    // Dynamically linked: the interpreter starts first and loads the libraries.
    u64 interp_base = 0;
    u64 start_entry = base + eh.entry;
    if (interp[0]) {
        if (long r = load_interpreter(mem, interp, &interp_base, &start_entry); r < 0) {
            klog::warn("exec %s: cannot load interpreter %s (error %ld)", exec_path, interp, -r);
            return r;
        }
    }
    mem->set_brk_base(highest);

    // Stack: 8 MiB, faulted in on demand.
    uptr stack_base;
    long r = mem->map(proc::STACK_TOP - proc::STACK_SIZE, proc::STACK_SIZE, proc::PROT_READ | proc::PROT_WRITE,
                      proc::MAP_PRIVATE | proc::MAP_ANONYMOUS | proc::MAP_FIXED | proc::MAP_GROWSDOWN, nullptr, 0, 0,
                      &stack_base);
    if (r < 0)
        return r;

    // ---- Initial stack, from the top down ----
    StackBuilder sb;
    usize argc = count(argv), envc = count(envp);

    u64 execfn = sb.push_string(exec_path);
    u64* env_ptrs = static_cast<u64*>(kmalloc((envc + argc + 2) * 8));
    u64* arg_ptrs = env_ptrs + envc + 1;
    if (!env_ptrs)
        return -err::NOMEM;
    for (usize i = envc; i-- > 0;)
        env_ptrs[i] = sb.push_string(envp[i]);
    for (usize i = argc; i-- > 0;)
        arg_ptrs[i] = sb.push_string(argv[i]);
    u64 platform = sb.push_string("x86_64");
    u8 random_bytes[16];
    random::fill(random_bytes, sizeof(random_bytes));
    sb.align(16);
    u64 random_addr = sb.push_bytes(random_bytes, sizeof(random_bytes));

    u64 auxv[][2] = {
        {AT_PHDR, phdr_addr},
        {AT_PHENT, sizeof(Elf64Phdr)},
        {AT_PHNUM, eh.phnum},
        {AT_PAGESZ, mm::PAGE_SIZE},
        {AT_BASE, interp_base},
        {AT_FLAGS, 0},
        {AT_ENTRY, base + eh.entry},
        {AT_UID, 0}, {AT_EUID, 0}, {AT_GID, 0}, {AT_EGID, 0},
        {AT_SECURE, 0},
        {AT_HWCAP, arch::cpuid(1).edx},
        {AT_HWCAP2, 0},
        {AT_CLKTCK, 100},
        {AT_PLATFORM, platform},
        {AT_RANDOM, random_addr},
        {AT_EXECFN, execfn},
        {AT_NULL, 0},
    };
    constexpr usize AUXV_WORDS = sizeof(auxv) / 8;

    // Final RSP (at argc) must be 16-byte aligned.
    usize words = 1 + (argc + 1) + (envc + 1) + AUXV_WORDS;
    sb.align(16);
    if (words & 1)
        sb.push_u64(0);

    for (usize i = sizeof(auxv) / sizeof(auxv[0]); i-- > 0;) {
        sb.push_u64(auxv[i][1]);
        sb.push_u64(auxv[i][0]);
    }
    sb.push_u64(0); // envp terminator
    for (usize i = envc; i-- > 0;)
        sb.push_u64(env_ptrs[i]);
    sb.push_u64(0); // argv terminator
    for (usize i = argc; i-- > 0;)
        sb.push_u64(arg_ptrs[i]);
    sb.push_u64(argc);
    kfree(env_ptrs);

    if (!sb.ok())
        return -err::TOOBIG;
    if ((r = sb.commit(mem)) < 0)
        return r;

    out->entry = start_entry;
    out->stack_ptr = sb.sp();
    return 0;
}

} // namespace elf
