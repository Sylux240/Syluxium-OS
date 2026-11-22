// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for QEMU's "edu" educational PCI device, written exactly as a
 * Linux driver would be (C, Linux kernel API). It exercises the LinuxKPI:
 * PCI probing, MMIO, MSI interrupts, completions, DMA, workqueues, timers
 * and firmware loading, and reports each check in the kernel log.
 *
 * Boot QEMU with:  -device edu,dma_mask=0xffffffffffffffff
 */
#define pr_fmt(fmt) "edu: " fmt

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#define EDU_VENDOR 0x1234
#define EDU_DEVICE 0x11e8

#define EDU_ID 0x00
#define EDU_LIVENESS 0x04
#define EDU_FACTORIAL 0x08
#define EDU_STATUS 0x20
#define EDU_IRQ_STATUS 0x24
#define EDU_IRQ_RAISE 0x60
#define EDU_IRQ_ACK 0x64
#define EDU_DMA_SRC 0x80
#define EDU_DMA_DST 0x88
#define EDU_DMA_COUNT 0x90
#define EDU_DMA_CMD 0x98

#define STATUS_COMPUTING 0x01
#define STATUS_IRQ_FACT 0x80
#define DMA_START 0x1
#define DMA_TO_RAM 0x2
#define DMA_IRQ 0x4
#define IRQ_FACT 0x1
#define IRQ_DMA 0x100
#define EDU_DMA_BUF 0x40000
#define DMA_LEN 4096

struct edu {
    struct pci_dev* pdev;
    void __iomem* regs;
    struct completion fact_done, dma_done, work_done, timer_done;
    u32 last_irq;
    atomic_t irqs;
    struct work_struct work;
    struct timer_list timer;
    int work_ran_in_irq;
};

static int passed, failed;

static void check(struct edu* e, bool ok, const char* what)
{
    if (ok)
        passed++;
    else
        failed++;
    dev_info(&e->pdev->dev, "[%s] %s\n", ok ? " ok " : "FAIL", what);
}

static irqreturn_t edu_irq(int irq, void* data)
{
    struct edu* e = data;
    u32 status = readl(e->regs + EDU_IRQ_STATUS);

    if (!status)
        return IRQ_NONE;
    writel(status, e->regs + EDU_IRQ_ACK);
    e->last_irq = status;
    atomic_inc(&e->irqs);
    if (status & IRQ_FACT)
        complete(&e->fact_done);
    if (status & IRQ_DMA)
        complete(&e->dma_done);
    return IRQ_HANDLED;
}

static void edu_work(struct work_struct* w)
{
    struct edu* e = container_of(w, struct edu, work);

    e->work_ran_in_irq = in_interrupt();
    msleep(5); /* work items may sleep */
    complete(&e->work_done);
}

static void edu_timer(struct timer_list* t)
{
    struct edu* e = from_timer(e, t, timer);

    complete(&e->timer_done);
}

static int edu_dma(struct edu* e, dma_addr_t addr, bool to_ram)
{
    reinit_completion(&e->dma_done);
    writeq(to_ram ? EDU_DMA_BUF : addr, e->regs + EDU_DMA_SRC);
    writeq(to_ram ? addr : EDU_DMA_BUF, e->regs + EDU_DMA_DST);
    writeq(DMA_LEN, e->regs + EDU_DMA_COUNT);
    writeq(DMA_START | DMA_IRQ | (to_ram ? DMA_TO_RAM : 0), e->regs + EDU_DMA_CMD);
    return wait_for_completion_timeout(&e->dma_done, msecs_to_jiffies(1000)) ? 0 : -ETIMEDOUT;
}

static int edu_probe(struct pci_dev* pdev, const struct pci_device_id* id)
{
    struct edu* e;
    u32 ident, fact;
    u8 *a, *b;
    dma_addr_t a_dma, b_dma;
    const struct firmware* fw;
    unsigned long sum;
    size_t i;
    int ret, ok;

    e = kzalloc(sizeof(*e), GFP_KERNEL);
    if (!e)
        return -ENOMEM;
    e->pdev = pdev;
    pci_set_drvdata(pdev, e);
    init_completion(&e->fact_done);
    init_completion(&e->dma_done);
    init_completion(&e->work_done);
    init_completion(&e->timer_done);
    atomic_set(&e->irqs, 0);

    ret = pci_enable_device(pdev);
    if (ret)
        goto err_free;
    pci_set_master(pdev);
    ret = pci_request_regions(pdev, "edu");
    if (ret)
        goto err_free;
    e->regs = pci_iomap(pdev, 0, 0);
    if (!e->regs) {
        ret = -ENOMEM;
        goto err_free;
    }

    ident = readl(e->regs + EDU_ID);
    dev_info(&pdev->dev, "QEMU edu device, version %u.%u, BAR0 at %#llx (%llu KiB)\n", ident >> 24,
             (ident >> 16) & 0xff, (unsigned long long)pci_resource_start(pdev, 0),
             (unsigned long long)pci_resource_len(pdev, 0) / 1024);
    check(e, (ident & 0xff) == 0xed, "identification register (MMIO read)");

    writel(0x12345678, e->regs + EDU_LIVENESS);
    check(e, readl(e->regs + EDU_LIVENESS) == ~0x12345678u, "liveness register (MMIO write/read)");

    /* Interrupt: MSI vector + handler; the device signals a finished factorial. */
    ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_MSIX);
    check(e, ret == 1, "MSI interrupt vector allocated");
    if (ret == 1) {
        ret = request_irq(pci_irq_vector(pdev, 0), edu_irq, 0, "edu", e);
        check(e, ret == 0, "request_irq");
        writel(STATUS_IRQ_FACT, e->regs + EDU_STATUS);
        writel(10, e->regs + EDU_FACTORIAL);
        ok = wait_for_completion_timeout(&e->fact_done, msecs_to_jiffies(1000)) != 0;
        fact = readl(e->regs + EDU_FACTORIAL);
        check(e, ok && fact == 3628800, "factorial 10! = 3628800 computed, interrupt delivered");
    }

    /* DMA: RAM -> device buffer -> other RAM buffer, compare. */
    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
    a = dma_alloc_coherent(&pdev->dev, DMA_LEN, &a_dma, GFP_KERNEL);
    b = dma_alloc_coherent(&pdev->dev, DMA_LEN, &b_dma, GFP_KERNEL);
    if (a && b) {
        for (i = 0; i < DMA_LEN; i++)
            a[i] = (u8)(i * 13 + 5);
        memset(b, 0, DMA_LEN);
        ok = edu_dma(e, a_dma, false) == 0 && edu_dma(e, b_dma, true) == 0 && memcmp(a, b, DMA_LEN) == 0;
        check(e, ok, "DMA round trip of 4 KiB (coherent buffers, completion interrupts)");
    } else {
        check(e, false, "dma_alloc_coherent");
    }
    if (a)
        dma_free_coherent(&pdev->dev, DMA_LEN, a, a_dma);
    if (b)
        dma_free_coherent(&pdev->dev, DMA_LEN, b, b_dma);

    /* Deferred work and timers. */
    INIT_WORK(&e->work, edu_work);
    schedule_work(&e->work);
    ok = wait_for_completion_timeout(&e->work_done, msecs_to_jiffies(1000)) != 0;
    check(e, ok && !e->work_ran_in_irq, "workqueue item ran (in process context, could sleep)");

    timer_setup(&e->timer, edu_timer, 0);
    {
        unsigned long start = jiffies;

        mod_timer(&e->timer, jiffies + msecs_to_jiffies(20));
        ok = wait_for_completion_timeout(&e->timer_done, msecs_to_jiffies(1000)) != 0;
        check(e, ok && time_after_eq(jiffies, start + 20), "kernel timer fired after 20 ms");
    }

    /* Firmware loading from /lib/firmware. */
    ret = request_firmware(&fw, "syluxium-test.bin", &pdev->dev);
    if (!ret) {
        sum = 0;
        for (i = 0; i < fw->size; i++)
            sum += fw->data[i];
        check(e, fw->size == 5000 && sum == 637404, "request_firmware(syluxium-test.bin): 5000 bytes, checksum ok");
        release_firmware(fw);
    } else {
        check(e, false, "request_firmware");
    }

    /* Write-combined kernel memory (x86 PAT): what TTM's write-combined
     * pools and amdgpu's GTT do with their pages. */
    {
        extern int lkpi_kernel_cache_type(const void *virt);
        struct page *pg = alloc_pages(GFP_KERNEL, 2);
        if (pg) {
            u32 *p = page_address(pg);
            struct page *arr[4];
            int r1, t1, t2, r2, t3, bad = -1;
            for (i = 0; i < 4; i++)
                arr[i] = virt_to_page((u8 *)p + i * PAGE_SIZE);
            for (i = 0; i < 4 * PAGE_SIZE / 4; i++)
                p[i] = (u32)(i * 2654435761u);
            r1 = set_pages_array_wc(arr, 4);
            t1 = lkpi_kernel_cache_type(p);
            t2 = lkpi_kernel_cache_type((u8 *)p + 3 * PAGE_SIZE);
            for (i = 0; bad < 0 && i < 4 * PAGE_SIZE / 4; i++)
                if (p[i] != (u32)(i * 2654435761u))
                    bad = i;
            p[5] = 0xabcdef01;
            r2 = set_pages_array_wb(arr, 4);
            t3 = lkpi_kernel_cache_type(p);
            if (r1 || t1 != 1 || t2 != 1 || bad >= 0 || r2 || t3 != 0 || p[5] != 0xabcdef01)
                dev_info(&pdev->dev, "PAT test: wc %d types %d/%d data %d wb %d type %d\n", r1, t1, t2, bad, r2, t3);
            check(e, !r1 && t1 == 1 && t2 == 1 && bad < 0 && !r2 && t3 == 0 && p[5] == 0xabcdef01,
                  "set_pages_array_wc/wb: direct map write-combined and back, data kept");
            __free_pages(pg, 2);
        } else {
            check(e, false, "alloc_pages for the PAT test");
        }
    }

    /* Scatter-gather DMA under a small DMA mask: entries above it go
     * through bounce buffers (dma_map_sg honours the mask). */
    {
        u64 old_mask = pdev->dev.dma_mask ? *pdev->dev.dma_mask : 0;
        struct page *pg = alloc_pages(GFP_KERNEL, 0);
        u64 mask = 0x7ffffff; /* 128 MiB: below most of a VM's free memory */
        if (pg && page_to_phys(pg) > mask) {
            struct scatterlist sg;
            u8 *buf = page_address(pg);
            bool ok;
            memset(buf, 0x5a, PAGE_SIZE);
            dma_set_mask(&pdev->dev, mask);
            sg_init_table(&sg, 1);
            sg_set_page(&sg, pg, PAGE_SIZE, 0);
            ok = dma_map_sg(&pdev->dev, &sg, 1, DMA_BIDIRECTIONAL) == 1 && sg_dma_address(&sg) &&
                 sg_dma_address(&sg) + PAGE_SIZE - 1 <= mask;
            if (ok) {
                u8 *dev_view = phys_to_virt(sg_dma_address(&sg));
                ok = dev_view[100] == 0x5a; /* copied to the device's buffer */
                memset(dev_view, 0xa5, PAGE_SIZE); /* the "device" writes */
                dma_unmap_sg(&pdev->dev, &sg, 1, DMA_BIDIRECTIONAL);
                ok = ok && buf[0] == 0xa5 && buf[PAGE_SIZE - 1] == 0xa5;
            }
            check(e, ok, "dma_map_sg under a 128 MiB DMA mask: bounce buffer below it, data both ways");
            dma_set_mask(&pdev->dev, old_mask ? old_mask : DMA_BIT_MASK(64));
        }
        if (pg)
            __free_pages(pg, 0);
    }

    dev_info(&pdev->dev, "LinuxKPI self-test: %d passed, %d failed (%d interrupts)\n", passed, failed,
             atomic_read(&e->irqs));
    return 0;

err_free:
    kfree(e);
    return ret;
}

static void edu_remove(struct pci_dev* pdev)
{
    struct edu* e = pci_get_drvdata(pdev);

    del_timer_sync(&e->timer);
    cancel_work_sync(&e->work);
    free_irq(pci_irq_vector(pdev, 0), e);
    kfree(e);
}

static const struct pci_device_id edu_ids[] = {
    { PCI_DEVICE(EDU_VENDOR, EDU_DEVICE) },
    { 0 }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static struct pci_driver edu_driver = {
    .name = "edu",
    .id_table = edu_ids,
    .probe = edu_probe,
    .remove = edu_remove,
};
module_pci_driver(edu_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QEMU edu device (LinuxKPI test driver)");
