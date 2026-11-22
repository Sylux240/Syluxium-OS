# Hardware drivers

Syluxium runs Linux's own drivers, unmodified or nearly so, through the LinuxKPI
compatibility layer (`kernel/linuxkpi/`), next to a few native drivers written for
Syluxium. This page lists them, which upstream version each comes from, and how far
each is tested.

## Upstream versions

| Source | Used for |
|---|---|
| **Linux 7.4-rc3** (pinned for new imports) | Ethernet (igb, alx, atl1c, bnx2, MDIO library), SCSI (megaraid_sas, scsi_common), SCSI headers |
| **Linux 6.12** | the vendored wireless stack (cfg80211, mac80211) and Bluetooth core, and every driver that plugs into them (iwlwifi, ath9k/ath10k, mt76, rtw88, rtw89, brcmfmac, b43, rtl8xxxu, mt7601u, btusb...), the DRM core and GPU drivers, earlier Ethernet imports (e1000e, igc, r8169, tg3, r8152) |

New imports come from 7.4-rc3. Wi-Fi and Bluetooth drivers come from 6.12 instead: they
call straight into mac80211/cfg80211 or the Bluetooth core, whose interfaces changed
between the two (rtw89 from 7.4 needs a 7.4 mac80211), and those stacks are vendored
from 6.12. Moving the wireless stack to 7.4 is a separate job.

Imported files are copied as they are, with their SPDX/GPL-2.0 headers. The
differences live in the LinuxKPI headers, not in the drivers:
- `include/linux/lkpi_v7.h`: Linux 7.x core APIs (typed allocators `kzalloc_obj()`...,
  `timer_container_of`, lock guards, compat types, driver attributes, PCI helpers),
  force-included for 7.4 imports;
- `include/net/lkpi_netdrv.h`: the 7.x network driver API (NAPI config, queue maps,
  AF_XDP stubs, RSS fields ...) for 7.4 Ethernet drivers;
- `include/linux/cpumask.h`, `blkdev.h`, `irq_poll.h`, `include/scsi/*`: CPU masks and IRQ
  affinity, the block/SCSI view for storage drivers;
- driver-specific headers (`net/lkpi_tg3.h`, `net/lkpi_iwlwifi.h`, ...).

Driver patches so far: none for the 7.4 imports. rtw89 leaves out `wow.c` (Wake on
WLAN; it's only built with `CONFIG_PM`, i.e. system sleep).

### Getting the upstream source

The Linux trees are build caches, never committed: `deps/linux-upstream` (a symlink to an
unpacked 7.4-rc3 tree) and `deps/linux-6.12` (from
`https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.tar.xz`, sha256
`b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb`). Only the
imported driver files are in the repository.

### What does a driver still need? `tools/lkpi-check.py`

```sh
tools/lkpi-check.py drivers/net/ethernet/intel/igb --include net/lkpi_netdrv.h
tools/lkpi-check.py --upstream deps/linux-6.12 drivers/net/wireless/realtek/rtw89 --include net/lkpi_netcompat.h
tools/lkpi-check.py --ours kernel/linuxkpi/drivers/scsi/megaraid --include linux/lkpi_v7.h --include linux/cpumask.h
```

It compiles every file the driver's Makefile names, using the kernel's own flags
(`make print-lkpi-cflags`), and writes nothing into the source tree. Missing headers get
empty stand-ins so a single run shows everything. It lists the missing headers,
undeclared functions and identifiers, missing struct members, incomplete types, and the
link-time symbols that neither the driver nor `build/syluxium.elf` define. Output goes
to `build/lkpi-check/<dir>/report.txt`. A driver is ready to import when the lists are
empty.

### Firmware

Firmware comes from Debian's non-free-firmware packages at build time (`FIRMWARE_PKGS`,
`NET_FIRMWARE` in the Makefile: firmware-realtek, -iwlwifi, -atheros, -brcm80211,
-mediatek, -misc-nonfree, -bnx2, ...); only the files the drivers load go into the image.
b43's firmware is cut from Broadcom's driver (`tools/b43-firmware.sh`).

## Diagnostics on real hardware

Every boot from the stick writes `hardware.txt` to the stick's log folder
(`syluxium-diag hardware`). It starts with:
- **Drivers**: one line per PCI/USB device: `bound to <driver>`, `no driver` or
  `probe failed: <driver> failed to start (error N)`;
- **Firmware requested by drivers but missing**: every firmware file a Linux driver asked
  for and didn't find (from `/sys/kernel/syluxium/firmware`, which also lists the loaded
  ones).

So after each test round the stick shows which drivers to fix next.

## Support table

Status: **hw** verified on real hardware · **qemu** probes and works in QEMU · **builds**
compiles and links, probe path checked on a stand-in device (`pci_fakeid=`), not yet on
real hardware · **native**: Syluxium's own driver.

### Ethernet

| Driver | Hardware | Upstream | Status | Notes |
|---|---|---|---|---|
| e1000e | Intel 82571..82574, I217/I218/I219 | 6.12 | qemu (QEMU's default NIC, 82574L) | H97 desktop, NUCs, ThinkPads; native e1000 as a fallback |
| igb | Intel 82575/82576/82580, I350, I354, I210/I211 | 7.4-rc3 | **qemu** (`-device igb`: DHCP, 50 MB download, checksum ok) | PowerEdge (I350), desktop boards (I210/I211) |
| igc | Intel I225/I226 2.5 GbE | 6.12 | builds | i5-11400 boards often have I225-V |
| r8169 | Realtek RTL8111/8168/8125... | 6.12 | builds | most desktops and mini PCs (Beelink); native r8169 as a fallback |
| 8139cp / 8139too | Realtek RTL8139 | 6.12 | qemu (`-device rtl8139`) | |
| tg3 | Broadcom Tigon3 (BCM57766, 5720...) | 6.12 | builds | 2013 iMac, PowerEdge R720 (5720) |
| bnx2 | Broadcom NetXtreme II BCM5706/5708/5709/5716 | 7.4-rc3 | builds | PowerEdge R610/R710; firmware-bnx2 |
| alx | Qualcomm Atheros AR8161/8171, Killer E2200/E2400/E2500 | 7.4-rc3 | builds | gaming boards and laptops |
| atl1c | Atheros AR8131/8132/8151/8152 (L1c/L2c) | 7.4-rc3 | builds | older laptops/netbooks |
| r8152 | Realtek RTL8152/8153/8156 USB | 6.12 | builds | USB dongles, docks |
| ax88179_178a | ASIX AX88179/178A USB | 6.12 | builds | USB 3 dongles |
| cdc_ether | USB CDC-ECM | native | hw | |
| virtio-net, e1000 | QEMU/VMs | native | qemu | |

### Wi-Fi

| Driver | Hardware | Upstream | Status | Notes |
|---|---|---|---|---|
| iwlwifi + iwlmvm | Intel 7260 .. AX211/BE200 | 6.12 | builds | NUCs, ThinkPads |
| ath9k | Atheros AR5008 .. AR9580, QCA9565 | 6.12 | builds | |
| ath10k (PCI) | QCA6174, QCA9377, QCA988x | 6.12 | builds | |
| mt7921e | MediaTek MT7921/MT7922 PCIe | 6.12 | builds | AMD laptops, mini PCs |
| mt7921u | MediaTek MT7921AU USB | 6.12 | builds | USB Wi-Fi 6 dongles |
| rtw88 (PCI) | RTL8822BE/CE, 8821CE, 8723DE | 6.12 | builds | |
| rtw88 (USB) | RTL8822BU/CU, 8821CU, 8723DU | 6.12 | builds | common USB dongles |
| rtw89 | RTL8852AE/BE/BTE/CE, 8851BE, 8922AE | 6.12 | builds | Beelink mini PCs, many 2021+ laptops |
| brcmfmac | Broadcom FullMAC PCIe/USB (BCM43602, 4350...) | 6.12 | builds | 2015-17 MacBook Pros |
| b43 + bcma | Broadcom SoftMAC BCM4331... | 6.12 | builds (probe seen on an iMac13,1) | 2013 iMac (BCM4331) |
| rtl8xxxu | Realtek RTL8188/8192/8723 USB | 6.12 | **hw** (TL-WN725N, WPA2) | |
| mt7601u | MediaTek MT7601U USB | 6.12 | builds | |
| mac80211_hwsim | virtual radios | 6.12 | qemu | |

### Bluetooth

| Driver | Hardware | Upstream | Status | Notes |
|---|---|---|---|---|
| btusb + btintel/btrtl/btbcm/btmtk | USB Bluetooth (Intel, Realtek, Broadcom, MediaTek) | 6.12 | builds; hci_vhci in QEMU | with BlueZ |

### Storage

| Driver | Hardware | Upstream | Status | Notes |
|---|---|---|---|---|
| megaraid_sas | PERC H310/H710/H730/H330, LSI SAS 1078..39xx | 7.4-rc3 | **qemu** (`-device megasas-gen2`: /dev/sdb, partitions, read/write) | PowerEdge RAID; through the LinuxKPI SCSI midlayer |
| AHCI, NVMe | SATA, NVMe | native | hw + qemu | |
| usb-storage | USB mass storage (BOT) | native | hw + qemu | UAS devices fall back to BOT |
| mpt3sas, rtsx, mmc/sdhci | | | not yet | see "Next" |

### Input, sound, platform

| Driver | Upstream | Status | Notes |
|---|---|---|---|
| HID (keyboards, mice, gamepads, tablets, multitouch), xpad | native | hw | |
| bcm5974 (MacBook trackpads), applespi | native | hw | 2013 MacBook Air |
| i2c-hid + DesignWare I2C, PS/2 Synaptics/Elantech/TrackPoint | native | hw | ThinkPads |
| Intel HDA (Realtek, Conexant, Cirrus CS4208 quirks, HDMI pending), USB audio, virtio-sound | native | hw + qemu | |
| ACPI battery/AC/lid/power button/EC, backlight | native (uACPI) | hw | |

## QEMU coverage

QEMU can exercise e1000e (default NIC), igb, rtl8139 (8139cp), megasas-gen2
(megaraid_sas), mptsas1068 (Fusion MPT, not imported), nvme, ahci, usb-storage/uas,
intel-hda/ich9-intel-hda, usb-audio, xhci, virtio, sdhci-pci (no MMC driver yet), and
mac80211_hwsim. The other drivers above only get a probe smoke test: `pci_fakeid=` shows
QEMU's edu device as the target chip, the driver probes, fails cleanly (error 5) and the
boot goes on.

## Next

1. mpt3sas (PowerEdge HBA330/H200 in IT mode): needs the SAS transport class
   (`scsi_transport_sas`) on top of the SCSI midlayer.
2. MMC core + sdhci-pci + rtsx_pci (laptop card readers; QEMU has sdhci-pci).
3. ath11k (QCA6390/WCN6855, newer ThinkPads): MHI bus and QMI.
4. Multi-vector MSI-X (one vector per device for now): multi-queue NICs and storage
   would scale.
5. uvcvideo + V4L2 core (webcams).
6. thinkpad_acpi, applesmc (hotkeys, fans, keyboard backlight).
7. The wireless stack on 7.4, so Wi-Fi drivers can come from the pinned tree too.
