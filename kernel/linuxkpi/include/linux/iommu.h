/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: no IOMMU (DMA addresses are physical). */
struct iommu_domain { unsigned type; };
#define IOMMU_DOMAIN_IDENTITY 1
#define IOMMU_DOMAIN_DMA 2
#define iommu_get_domain_for_dev(dev) ((struct iommu_domain *)NULL)
#define iommu_present(bus) false
#define device_iommu_capable(dev, cap) false
