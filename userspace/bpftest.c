// bpf(2) without execution: maps (hash, array, per-CPU, LRU, LPM trie),
// their errnos, program load, cgroup attach (recorded), pinning in a bpffs,
// object IDs and info. What Android's bpfloader and netd rely on.
// Prints PASS/FAIL per check; exit status = number of failures.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        fails++;
}

static long sys_bpf(int cmd, union bpf_attr* a) { return syscall(SYS_bpf, cmd, a, sizeof(*a)); }

static int map_create(unsigned type, unsigned ks, unsigned vs, unsigned max, const char* name) {
    union bpf_attr a;
    memset(&a, 0, sizeof(a));
    a.map_type = type;
    a.key_size = ks;
    a.value_size = vs;
    a.max_entries = max;
    if (name)
        strncpy(a.map_name, name, sizeof(a.map_name) - 1);
    return (int)sys_bpf(BPF_MAP_CREATE, &a);
}
static long elem(int cmd, int fd, const void* key, void* val, unsigned long long flags) {
    union bpf_attr a;
    memset(&a, 0, sizeof(a));
    a.map_fd = fd;
    a.key = (uintptr_t)key;
    a.value = (uintptr_t)val;
    a.flags = flags;
    return sys_bpf(cmd, &a);
}
static long next_key(int fd, const void* key, void* next) {
    union bpf_attr a;
    memset(&a, 0, sizeof(a));
    a.map_fd = fd;
    a.key = (uintptr_t)key;
    a.next_key = (uintptr_t)next;
    return sys_bpf(BPF_MAP_GET_NEXT_KEY, &a);
}

int main(void) {
    // ---- hash map ----
    int h = map_create(BPF_MAP_TYPE_HASH, 4, 8, 3, "test_hash");
    check(h >= 0, "hash map created");
    check((fcntl(h, F_GETFD) & FD_CLOEXEC) != 0, "map fd is close-on-exec");
    uint32_t k = 7;
    uint64_t v = 70, out = 0;
    check(elem(BPF_MAP_LOOKUP_ELEM, h, &k, &out, 0) < 0 && errno == ENOENT, "lookup of a missing key: ENOENT");
    check(elem(BPF_MAP_UPDATE_ELEM, h, &k, &v, BPF_EXIST) < 0 && errno == ENOENT, "BPF_EXIST on a missing key: ENOENT");
    check(elem(BPF_MAP_UPDATE_ELEM, h, &k, &v, BPF_NOEXIST) == 0, "BPF_NOEXIST insert");
    check(elem(BPF_MAP_UPDATE_ELEM, h, &k, &v, BPF_NOEXIST) < 0 && errno == EEXIST, "BPF_NOEXIST again: EEXIST");
    check(elem(BPF_MAP_LOOKUP_ELEM, h, &k, &out, 0) == 0 && out == 70, "lookup finds the value");
    for (uint32_t i = 1; i <= 2; i++) {
        uint64_t x = i * 10;
        elem(BPF_MAP_UPDATE_ELEM, h, &i, &x, BPF_ANY);
    }
    uint32_t extra = 99;
    check(elem(BPF_MAP_UPDATE_ELEM, h, &extra, &v, BPF_ANY) < 0 && errno == E2BIG, "full hash map: E2BIG");
    uint32_t cur, nk, seen = 0;
    long r = next_key(h, NULL, &cur);
    while (r == 0) {
        seen++;
        r = next_key(h, &cur, &nk);
        cur = nk;
    }
    check(seen == 3 && errno == ENOENT, "get_next_key walks all 3 keys, then ENOENT");
    check(elem(BPF_MAP_UPDATE_ELEM, h, &k, &v, 42) < 0 && errno == EINVAL, "bad update flags: EINVAL");
    check(elem(BPF_MAP_DELETE_ELEM, h, &k, NULL, 0) == 0, "delete");
    check(elem(BPF_MAP_DELETE_ELEM, h, &k, NULL, 0) < 0 && errno == ENOENT, "delete again: ENOENT");

    // ---- array ----
    int arr = map_create(BPF_MAP_TYPE_ARRAY, 4, 4, 4, "test_array");
    check(arr >= 0, "array map created");
    check(map_create(BPF_MAP_TYPE_ARRAY, 8, 4, 4, NULL) < 0 && errno == EINVAL, "array with 8-byte keys: EINVAL");
    uint32_t idx = 2, val = 0;
    check(elem(BPF_MAP_LOOKUP_ELEM, arr, &idx, &val, 0) == 0 && val == 0, "array elements exist, zeroed");
    val = 5;
    check(elem(BPF_MAP_UPDATE_ELEM, arr, &idx, &val, BPF_NOEXIST) < 0 && errno == EEXIST, "array BPF_NOEXIST: EEXIST");
    check(elem(BPF_MAP_UPDATE_ELEM, arr, &idx, &val, BPF_ANY) == 0, "array update");
    idx = 4;
    check(elem(BPF_MAP_UPDATE_ELEM, arr, &idx, &val, BPF_ANY) < 0 && errno == E2BIG, "array index past the end: E2BIG");
    check(elem(BPF_MAP_DELETE_ELEM, arr, &idx, NULL, 0) < 0 && errno == EINVAL, "array delete: EINVAL");
    idx = 3;
    check(next_key(arr, &idx, &nk) < 0 && errno == ENOENT, "array next after the last: ENOENT");

    // ---- per-CPU: one 8-byte slot per possible CPU ----
    long ncpu = sysconf(_SC_NPROCESSORS_CONF);
    int pc = map_create(BPF_MAP_TYPE_PERCPU_HASH, 4, 4, 8, "test_percpu");
    uint64_t slots[256] = {0}, back[256] = {0};
    for (long i = 0; i < ncpu && i < 256; i++)
        slots[i] = 100 + i;
    k = 1;
    check(pc >= 0 && elem(BPF_MAP_UPDATE_ELEM, pc, &k, slots, BPF_ANY) == 0 &&
              elem(BPF_MAP_LOOKUP_ELEM, pc, &k, back, 0) == 0 && back[ncpu - 1] == (uint64_t)(100 + ncpu - 1),
          "per-CPU hash keeps one value per CPU");

    // ---- LRU: evicts instead of E2BIG ----
    int lru = map_create(BPF_MAP_TYPE_LRU_HASH, 4, 4, 2, "test_lru");
    uint32_t a1 = 1, a2 = 2, a3 = 3, x = 1;
    elem(BPF_MAP_UPDATE_ELEM, lru, &a1, &x, BPF_ANY);
    elem(BPF_MAP_UPDATE_ELEM, lru, &a2, &x, BPF_ANY);
    check(lru >= 0 && elem(BPF_MAP_UPDATE_ELEM, lru, &a3, &x, BPF_ANY) == 0 &&
              elem(BPF_MAP_LOOKUP_ELEM, lru, &a1, &x, 0) < 0, "LRU hash evicts the oldest key");

    // ---- LPM trie ----
    struct { uint32_t prefix; uint8_t ip[4]; } lk;
    int lpm = map_create(BPF_MAP_TYPE_LPM_TRIE, sizeof(lk), 4, 16, "test_lpm");
    uint32_t lv = 8;
    lk.prefix = 8, lk.ip[0] = 10, lk.ip[1] = lk.ip[2] = lk.ip[3] = 0;
    elem(BPF_MAP_UPDATE_ELEM, lpm, &lk, &lv, BPF_ANY);
    lv = 24;
    lk.prefix = 24, lk.ip[1] = 1, lk.ip[2] = 2;
    elem(BPF_MAP_UPDATE_ELEM, lpm, &lk, &lv, BPF_ANY);
    lk.prefix = 32, lk.ip[3] = 9;
    uint32_t got = 0;
    check(lpm >= 0 && elem(BPF_MAP_LOOKUP_ELEM, lpm, &lk, &got, 0) == 0 && got == 24, "LPM trie: longest prefix wins");
    lk.ip[1] = 5;
    check(elem(BPF_MAP_LOOKUP_ELEM, lpm, &lk, &got, 0) == 0 && got == 8, "LPM trie: shorter prefix matches");

    // ---- programs ----
    struct bpf_insn prog[2] = {{.code = BPF_ALU64 | BPF_MOV | BPF_K, .dst_reg = 0, .imm = 1},
                               {.code = BPF_JMP | BPF_EXIT}};
    union bpf_attr pa;
    memset(&pa, 0, sizeof(pa));
    pa.prog_type = BPF_PROG_TYPE_CGROUP_SKB;
    pa.insn_cnt = 2;
    pa.insns = (uintptr_t)prog;
    pa.license = (uintptr_t)"Apache 2.0";
    strcpy(pa.prog_name, "test_prog");
    int p = (int)sys_bpf(BPF_PROG_LOAD, &pa);
    check(p >= 0, "program loaded");
    pa.insn_cnt = 0;
    check(sys_bpf(BPF_PROG_LOAD, &pa) < 0 && errno == EINVAL, "program without instructions: EINVAL");

    // cgroup attach / query / detach
    mkdir("/sys/fs/cgroup", 0755);
    int cg = open("/sys/fs/cgroup", O_RDONLY | O_DIRECTORY);
    union bpf_attr at;
    memset(&at, 0, sizeof(at));
    at.target_fd = cg;
    at.attach_bpf_fd = p;
    at.attach_type = BPF_CGROUP_INET_EGRESS;
    check(cg >= 0 && sys_bpf(BPF_PROG_ATTACH, &at) == 0, "program attached to a cgroup");
    uint32_t ids[4] = {0};
    union bpf_attr q;
    memset(&q, 0, sizeof(q));
    q.query.target_fd = cg;
    q.query.attach_type = BPF_CGROUP_INET_EGRESS;
    q.query.prog_ids = (uintptr_t)ids;
    q.query.prog_cnt = 4;
    check(sys_bpf(BPF_PROG_QUERY, &q) == 0 && q.query.prog_cnt == 1 && ids[0] != 0, "PROG_QUERY lists it");
    check(sys_bpf(BPF_PROG_DETACH, &at) == 0, "program detached");

    // ---- info and IDs ----
    struct bpf_map_info mi;
    memset(&mi, 0, sizeof(mi));
    union bpf_attr ia;
    memset(&ia, 0, sizeof(ia));
    ia.info.bpf_fd = h;
    ia.info.info_len = sizeof(mi);
    ia.info.info = (uintptr_t)&mi;
    check(sys_bpf(BPF_OBJ_GET_INFO_BY_FD, &ia) == 0 && mi.type == BPF_MAP_TYPE_HASH && mi.key_size == 4 &&
              mi.max_entries == 3 && strcmp(mi.name, "test_hash") == 0, "map info");
    union bpf_attr ga;
    memset(&ga, 0, sizeof(ga));
    ga.map_id = mi.id;
    int again = (int)sys_bpf(BPF_MAP_GET_FD_BY_ID, &ga);
    check(again >= 0 && elem(BPF_MAP_LOOKUP_ELEM, again, &a1, &out, 0) == 0, "map by ID opens the same map");

    // ---- pinning in a bpffs ----
    mkdir("/tmp/bpffs", 0755);
    check(mount("bpf", "/tmp/bpffs", "bpf", 0, NULL) == 0, "bpffs mounted");
    struct statfs sf;
    check(statfs("/tmp/bpffs", &sf) == 0 && (unsigned long)sf.f_type == 0xcafe4a11UL, "bpffs reports BPF_FS_MAGIC");
    mkdir("/tmp/bpffs/netd_shared", 0755);
    union bpf_attr pin;
    memset(&pin, 0, sizeof(pin));
    pin.pathname = (uintptr_t)"/tmp/bpffs/netd_shared/map_test_hash";
    pin.bpf_fd = h;
    check(sys_bpf(BPF_OBJ_PIN, &pin) == 0, "map pinned");
    check(sys_bpf(BPF_OBJ_PIN, &pin) < 0 && errno == EEXIST, "pinning twice: EEXIST");
    struct stat st;
    check(stat("/tmp/bpffs/netd_shared/map_test_hash", &st) == 0 && S_ISREG(st.st_mode), "the pin is a file");
    close(h);
    close(again);
    union bpf_attr og;
    memset(&og, 0, sizeof(og));
    og.pathname = (uintptr_t)"/tmp/bpffs/netd_shared/map_test_hash";
    int h2 = (int)sys_bpf(BPF_OBJ_GET, &og);
    check(h2 >= 0 && elem(BPF_MAP_LOOKUP_ELEM, h2, &a1, &out, 0) == 0 && out == 10,
          "OBJ_GET reopens the pinned map (it outlived its fds)");
    og.file_flags = BPF_F_RDONLY;
    int ro = (int)sys_bpf(BPF_OBJ_GET, &og);
    check(ro >= 0, "OBJ_GET read-only");
    og.pathname = (uintptr_t)"/tmp/bpffs/netd_shared";
    og.file_flags = 0;
    check(sys_bpf(BPF_OBJ_GET, &og) < 0 && errno == EINVAL, "OBJ_GET of a directory: EINVAL");
    check(rename("/tmp/bpffs/netd_shared/map_test_hash", "/tmp/bpffs/netd_shared/map_renamed") == 0,
          "a pin can be renamed");
    check(unlink("/tmp/bpffs/netd_shared/map_renamed") == 0, "a pin can be removed");
    close(h2);
    close(ro);
    umount("/tmp/bpffs");

    printf("bpftest: %s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails;
}
