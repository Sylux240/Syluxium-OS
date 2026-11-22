#include <arch/x86_64/cpu.hpp>
#include <boot/limine_requests.hpp>
#include <core/log.hpp>
#include <lib/random.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
#include <proc/compat.hpp>
#include <proc/elf.hpp>
#include <proc/process.hpp>

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

// 32-bit (ia32) variants. They're converted to the 64-bit structures when
// read, so everything after read_headers() handles both classes alike.
struct Elf32Header {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u32 entry;
    u32 phoff;
    u32 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
};
static_assert(sizeof(Elf32Header) == 52);

struct Elf32Phdr {
    u32 type;
    u32 offset;
    u32 vaddr;
    u32 paddr;
    u32 filesz;
    u32 memsz;
    u32 flags;
    u32 align;
};
static_assert(sizeof(Elf32Phdr) == 32);

constexpr u16 ET_EXEC = 2, ET_DYN = 3;
constexpr u16 EM_386 = 3, EM_X86_64 = 62;
constexpr u8 ELFCLASS32 = 1, ELFCLASS64 = 2;
constexpr u32 PT_LOAD = 1, PT_INTERP = 3, PT_PHDR = 6;
constexpr u32 PF_X = 1, PF_W = 2, PF_R = 4;

// Auxiliary vector keys.
constexpr u64 AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6, AT_BASE = 7,
              AT_FLAGS = 8, AT_ENTRY = 9, AT_UID = 11, AT_EUID = 12, AT_GID = 13, AT_EGID = 14,
              AT_PLATFORM = 15, AT_HWCAP = 16, AT_CLKTCK = 17, AT_SECURE = 23, AT_RANDOM = 25,
              AT_HWCAP2 = 26, AT_EXECFN = 31, AT_SYSINFO = 32;

constexpr u64 PIE_BASE = 0x555555554000; // where Linux puts position-independent executables
constexpr u64 PIE_BASE32 = 0x56555000;   // ... and 32-bit ones
constexpr usize STACK_IMAGE_MAX = 128 * KiB;

// What differs between loading a 64-bit and a 32-bit program.
struct Layout {
    bool is32;
    u64 stack_top;
    u64 pie_base;
    usize word; // pointer size on the initial stack (argv, envp, auxv)
    const char* platform;
};
constexpr Layout LAYOUT64{false, proc::STACK_TOP, PIE_BASE, 8, "x86_64"};
constexpr Layout LAYOUT32{true, proc::COMPAT_STACK_TOP, PIE_BASE32, 4, "i686"};

// An ELF header + program headers of either class, normalized to ELF64.
struct Image {
    Elf64Header eh;
    Elf64Phdr phdrs[64];
    bool is32;
    u16 phent_size; // the file's own program-header size (for AT_PHENT)
};

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
    explicit StackBuilder(u64 top) : m_buf(static_cast<u8*>(kmalloc(STACK_IMAGE_MAX))), m_top(top) {}
    ~StackBuilder() { kfree(m_buf); }

    bool ok() const { return m_buf && !m_overflow; }
    u64 sp() const { return m_top - m_used; }

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
    // One pointer-sized word of the program's ABI (4 bytes for 32-bit programs).
    void push_word(u64 v, usize size) {
        if (size == 4) {
            u32 w = static_cast<u32>(v);
            push_bytes(&w, 4);
        } else {
            push_bytes(&v, 8);
        }
    }
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
    u64 m_top;
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
long map_segments(proc::Memory* mem, vfs::Inode* file, const Image& img, u64 base, const Layout& layout,
                  u64* highest, u64* phdr_addr) {
    const Elf64Header& eh = img.eh;
    *highest = 0;
    *phdr_addr = 0;
    for (u32 i = 0; i < eh.phnum; i++) {
        const Elf64Phdr& ph = img.phdrs[i];
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
        if (end > layout.stack_top - proc::STACK_SIZE || start < proc::USER_MIN || end < start)
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

long read_headers(vfs::Inode* file, Image* img) {
    u8 raw[sizeof(Elf64Header)];
    if (file->read(0, raw, sizeof(raw)) != sizeof(raw))
        return -err::NOEXEC;
    if (memcmp(raw, "\x7f" "ELF", 4) != 0 || raw[5] != 1 /* little endian */)
        return -err::NOEXEC;
    Elf64Header* eh = &img->eh;
    if (raw[4] == ELFCLASS64) {
        memcpy(eh, raw, sizeof(*eh));
        if (eh->machine != EM_X86_64 || eh->phentsize != sizeof(Elf64Phdr))
            return -err::NOEXEC;
        img->is32 = false;
        img->phent_size = sizeof(Elf64Phdr);
    } else if (raw[4] == ELFCLASS32) {
        Elf32Header h;
        memcpy(&h, raw, sizeof(h));
        if (h.machine != EM_386 || h.phentsize != sizeof(Elf32Phdr))
            return -err::NOEXEC;
        memcpy(eh->ident, h.ident, sizeof(eh->ident));
        eh->type = h.type;
        eh->machine = h.machine;
        eh->version = h.version;
        eh->entry = h.entry;
        eh->phoff = h.phoff;
        eh->shoff = h.shoff;
        eh->flags = h.flags;
        eh->ehsize = h.ehsize;
        eh->phentsize = sizeof(Elf64Phdr);
        eh->phnum = h.phnum;
        eh->shentsize = h.shentsize;
        eh->shnum = h.shnum;
        eh->shstrndx = h.shstrndx;
        img->is32 = true;
        img->phent_size = sizeof(Elf32Phdr);
    } else {
        return -err::NOEXEC;
    }
    if ((eh->type != ET_EXEC && eh->type != ET_DYN) || eh->phnum == 0 || eh->phnum > 64)
        return -err::NOEXEC;

    usize ph_bytes = eh->phnum * img->phent_size;
    if (!img->is32)
        return file->read(eh->phoff, img->phdrs, ph_bytes) == static_cast<isize>(ph_bytes) ? 0 : -err::NOEXEC;

    Elf32Phdr ph32[64];
    if (file->read(eh->phoff, ph32, ph_bytes) != static_cast<isize>(ph_bytes))
        return -err::NOEXEC;
    for (u32 i = 0; i < eh->phnum; i++)
        img->phdrs[i] = Elf64Phdr{ph32[i].type,  ph32[i].flags,  ph32[i].offset, ph32[i].vaddr,
                                  ph32[i].paddr, ph32[i].filesz, ph32[i].memsz,  ph32[i].align};
    return 0;
}

// Load the program interpreter (ld-linux.so) at an address mmap picks. It
// must be of the same class as the program (ld-linux.so.2 for 32-bit).
long load_interpreter(proc::Memory* mem, const char* path, const Layout& layout, Image* img, u64* base_out,
                      u64* entry_out) {
    vfs::Inode* file;
    if (long r = vfs::resolve(path, nullptr, true, &file); r < 0)
        return r;
    if (long r = read_headers(file, img); r < 0)
        return r;
    if (img->eh.type != ET_DYN || img->is32 != layout.is32)
        return -err::NOEXEC;

    // Reserve one region big enough for the whole image, then map the
    // segments into it (MAP_FIXED replaces the reservation piece by piece).
    u64 lo = ~0ull, hi = 0;
    for (u32 i = 0; i < img->eh.phnum; i++) {
        if (img->phdrs[i].type != PT_LOAD)
            continue;
        lo = min(lo, align_down<u64>(img->phdrs[i].vaddr, mm::PAGE_SIZE));
        hi = max(hi, align_up<u64>(img->phdrs[i].vaddr + img->phdrs[i].memsz, mm::PAGE_SIZE));
    }
    if (hi <= lo)
        return -err::NOEXEC;
    uptr region;
    if (long r = mem->map(0, hi - lo, proc::PROT_NONE, proc::MAP_PRIVATE | proc::MAP_ANONYMOUS, nullptr, 0, 0, &region);
        r < 0)
        return r;
    u64 base = region - lo;
    u64 highest, phdr_addr;
    if (long r = map_segments(mem, file, *img, base, layout, &highest, &phdr_addr); r < 0)
        return r;
    *base_out = base;
    *entry_out = base + img->eh.entry;
    return 0;
}

// ---- ASLR: where things go differs on every exec (like Linux: 28 bits of
// page entropy for mmap and PIE programs, 22 for the stack, 32 MiB for brk;
// 8 bits for 32-bit programs). Off with "norandmaps" on the kernel command
// line, or per process with personality(ADDR_NO_RANDOMIZE) (setarch -R).
constexpr u32 ADDR_NO_RANDOMIZE = 0x0040000;

bool randomize(const proc::Process* p) {
    static int boot_off = -1;
    if (boot_off < 0)
        boot_off = strstr(boot::cmdline(), "norandmaps") ? 1 : 0;
    return !boot_off && !(p && (p->personality & ADDR_NO_RANDOMIZE));
}

// A random multiple of the page size below 2^bits pages.
u64 random_pages(u32 bits) { return (random::next_u64() & ((1ull << bits) - 1)) << mm::PAGE_SHIFT; }

// `img` (already read) is reused as scratch space for the interpreter's headers.
long load_image(proc::Memory* mem, vfs::Inode* file, const char* exec_path, const char* const* argv,
                const char* const* envp, Image* img, LoadResult* out, const ExecIds& ids) {
    const Layout& layout = img->is32 ? LAYOUT32 : LAYOUT64;
    if (layout.is32)
        mem->set_compat(); // before anything is mapped: mmap() must stay below 4 GiB
    bool aslr = randomize(proc::current());
    u64 pie_shift = aslr ? random_pages(layout.is32 ? 8 : 28) : 0;
    u64 stack_shift = aslr ? random_pages(layout.is32 ? 11 : 22) : 0; // 8 MiB / 16 GiB
    u64 brk_shift = aslr ? random_pages(13) : 0;                        // 32 MiB
    if (aslr)
        mem->set_mmap_shift(random_pages(layout.is32 ? 8 : 28));
    u64 stack_top = layout.stack_top - stack_shift;

    char interp[256] = {};
    for (u32 i = 0; i < img->eh.phnum; i++) {
        if (img->phdrs[i].type == PT_INTERP) {
            if (img->phdrs[i].filesz >= sizeof(interp))
                return -err::NOEXEC;
            file->read(img->phdrs[i].offset, interp, img->phdrs[i].filesz);
            interp[img->phdrs[i].filesz] = 0;
        }
    }

    // Everything the auxiliary vector needs from the program's own headers.
    u64 base = img->eh.type == ET_DYN ? layout.pie_base + pie_shift : 0;
    u64 highest, phdr_addr;
    if (long r = map_segments(mem, file, *img, base, layout, &highest, &phdr_addr); r < 0)
        return r;
    u64 prog_entry = base + img->eh.entry;
    u64 phnum = img->eh.phnum;
    u64 phent = img->phent_size;

    // Dynamically linked: the interpreter starts first and loads the libraries.
    u64 interp_base = 0;
    u64 start_entry = prog_entry;
    if (interp[0]) {
        if (long r = load_interpreter(mem, interp, layout, img, &interp_base, &start_entry); r < 0) {
            klog::warn("exec %s: cannot load interpreter %s (error %ld)", exec_path, interp, -r);
            return r;
        }
    }
    mem->set_brk_base(highest + brk_shift);

    if (layout.is32) {
        if (long r = compat::map_vsyscall_page(mem); r < 0)
            return r;
    }

    // Stack: 8 MiB, faulted in on demand.
    uptr stack_base;
    long r = mem->map(stack_top - proc::STACK_SIZE, proc::STACK_SIZE, proc::PROT_READ | proc::PROT_WRITE,
                      proc::MAP_PRIVATE | proc::MAP_ANONYMOUS | proc::MAP_FIXED | proc::MAP_GROWSDOWN, nullptr, 0, 0,
                      &stack_base);
    if (r < 0)
        return r;

    // ---- Initial stack, from the top down ----
    StackBuilder sb(stack_top);
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
    u64 platform = sb.push_string(layout.platform);
    u8 random_bytes[16];
    random::fill(random_bytes, sizeof(random_bytes));
    sb.align(16);
    u64 random_addr = sb.push_bytes(random_bytes, sizeof(random_bytes));

    u64 auxv[][2] = {
        // 32-bit only (dropped below for 64-bit): glibc calls __kernel_vsyscall
        // for system calls. No AT_SYSINFO_EHDR: there's no 32-bit vDSO image,
        // so glibc makes real syscalls for clock_gettime & co.
        {AT_SYSINFO, compat::VSYSCALL_ENTRY},
        {AT_PHDR, phdr_addr},
        {AT_PHENT, phent},
        {AT_PHNUM, phnum},
        {AT_PAGESZ, mm::PAGE_SIZE},
        {AT_BASE, interp_base},
        {AT_FLAGS, 0},
        {AT_ENTRY, prog_entry},
        {AT_UID, ids.uid}, {AT_EUID, ids.euid}, {AT_GID, ids.gid}, {AT_EGID, ids.egid},
        {AT_SECURE, ids.secure ? 1u : 0u},
        {AT_HWCAP, arch::cpuid(1).edx},
        {AT_HWCAP2, 0},
        {AT_CLKTCK, 100},
        {AT_PLATFORM, platform},
        {AT_RANDOM, random_addr},
        {AT_EXECFN, execfn},
        {AT_NULL, 0},
    };
    usize first_aux = layout.is32 ? 0 : 1;
    usize aux_count = sizeof(auxv) / sizeof(auxv[0]) - first_aux;

    // Final stack pointer (at argc) must be 16-byte aligned.
    usize words = 1 + (argc + 1) + (envc + 1) + aux_count * 2;
    sb.align(16);
    usize misalign = (words * layout.word) & 15;
    if (misalign) {
        u8 zeros[16] = {};
        sb.push_bytes(zeros, 16 - misalign);
    }

    for (usize i = sizeof(auxv) / sizeof(auxv[0]); i-- > first_aux;) {
        sb.push_word(auxv[i][1], layout.word);
        sb.push_word(auxv[i][0], layout.word);
    }
    sb.push_word(0, layout.word); // envp terminator
    for (usize i = envc; i-- > 0;)
        sb.push_word(env_ptrs[i], layout.word);
    sb.push_word(0, layout.word); // argv terminator
    for (usize i = argc; i-- > 0;)
        sb.push_word(arg_ptrs[i], layout.word);
    sb.push_word(argc, layout.word);

    if (!sb.ok()) {
        kfree(env_ptrs);
        return -err::TOOBIG;
    }
    if ((r = sb.commit(mem)) < 0) {
        kfree(env_ptrs);
        return r;
    }

    // Strings were pushed top-down: argv, then envp, then the exec path above them.
    mem->arg_start = argc ? arg_ptrs[0] : execfn;
    mem->arg_end = envc ? env_ptrs[0] : execfn;
    mem->env_start = mem->arg_end;
    mem->env_end = execfn;
    out->entry = start_entry;
    out->stack_ptr = sb.sp();
    mem->start_stack = out->stack_ptr;
    out->compat = layout.is32;
    kfree(env_ptrs);
    return 0;
}

} // namespace

long load(proc::Memory* mem, vfs::Inode* file, const char* exec_path, const char* const* argv,
          const char* const* envp, LoadResult* out, const ExecIds& ids) {
    // Header buffers are large (64 program headers): keep them off the kernel stack.
    auto* img = static_cast<Image*>(kmalloc(sizeof(Image)));
    if (!img)
        return -err::NOMEM;
    long r = read_headers(file, img);
    if (r == 0)
        r = load_image(mem, file, exec_path, argv, envp, img, out, ids);
    kfree(img);
    return r;
}

} // namespace elf
