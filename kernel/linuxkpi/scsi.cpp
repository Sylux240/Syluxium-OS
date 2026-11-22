// LinuxKPI SCSI disks: what a Linux host adapter driver (megaraid_sas,
// mpt3sas, ...) reports as a direct-access device becomes a Syluxium block
// device (/dev/sdX, partitions scanned), backed by SCSI READ/WRITE commands
// through the midlayer in glue/scsi.c.

#include <block/block.hpp>
#include <core/log.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>

extern "C" {
int lkpi_scsi_rw(void* sdev, u64 lba, u32 count, void* buf, int write);
int lkpi_scsi_flush(void* sdev);
u32 lkpi_scsi_max_sectors(void* sdev);
}

namespace {

class ScsiDisk : public block::Device {
public:
    explicit ScsiDisk(void* sdev) : m_sdev(sdev), m_max(lkpi_scsi_max_sectors(sdev)) {}

    long read_sectors(u64 lba, u32 count, void* buf) override { return rw(lba, count, buf, false); }
    long write_sectors(u64 lba, u32 count, const void* buf) override {
        return rw(lba, count, const_cast<void*>(buf), true);
    }
    long flush() override { return m_gone ? -err::IO : lkpi_scsi_flush(m_sdev); }

    bool m_gone = false;

private:
    long rw(u64 lba, u32 count, void* buf, bool write) {
        if (m_gone)
            return -err::IO;
        auto* p = static_cast<u8*>(buf);
        while (count) {
            u32 n = count < m_max ? count : m_max;
            if (int r = lkpi_scsi_rw(m_sdev, lba, n, p, write); r < 0)
                return r;
            lba += n;
            count -= n;
            p += static_cast<usize>(n) * sector_size;
        }
        return 0;
    }

    void* m_sdev;
    u32 m_max;
};

} // namespace

extern "C" void* lkpi_scsi_disk_add(void* sdev, u64 sectors, u32 sector_size, const char* model, void* hw,
                                    int removable) {
    auto* disk = new ScsiDisk(sdev);
    disk->sector_count = sectors;
    disk->sector_size = sector_size;
    disk->removable = removable != 0;
    disk->hw = static_cast<dev::Device*>(hw);
    snprintf(disk->model, sizeof(disk->model), "%s", model);
    block::next_sd_name(disk->name, sizeof(disk->name));
    block::register_disk(disk);
    klog::ok("SCSI: /dev/%s: %s, %lu MiB", disk->name, disk->model, (sectors * sector_size) >> 20);
    return disk;
}

extern "C" void lkpi_scsi_disk_remove(void* d) {
    auto* disk = static_cast<ScsiDisk*>(d);
    disk->m_gone = true;
    block::unregister_disk(disk); // stays allocated: mounts may still refer to it
}
