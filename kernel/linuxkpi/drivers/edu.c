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

#include <drm/gpu_scheduler.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/dma-fence.h>
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
    struct dma_fence *sched_hw_fence; /* drm_sched test: signalled by the next factorial interrupt */
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
    if (status & IRQ_FACT) {
        struct dma_fence *f = xchg(&e->sched_hw_fence, NULL);
        if (f) { /* a drm_sched test job finished on the "GPU" */
            dma_fence_signal(f);
            dma_fence_put(f);
        } else {
            complete(&e->fact_done);
        }
    }
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


/* The GPU job scheduler (drm_sched) the way amdgpu uses it, with this
 * device as the "GPU": each job starts a factorial and its hardware fence
 * is signalled from the device's interrupt, as amdgpu's ring fences are.
 * One job in flight at a time; jobs depend on the previous job's finished
 * fence (a chain, like a context's submissions), and the last finished
 * fence is waited for. Before amdgpu's first command submission nothing in
 * Syluxium ran drm_sched (ring and IB tests bypass it). */
struct edu_job {
    struct drm_sched_job base;
    struct edu *e;
    int n;
};

static struct edu *sched_edu;
static DEFINE_SPINLOCK(sched_fence_lock);
static u64 sched_fence_ctx;
static atomic_t sched_ran, sched_freed;
static int sched_order_bad, sched_last = -1;

static const char *edu_fence_name(struct dma_fence *f) { return "edu"; }
static const struct dma_fence_ops edu_fence_ops = {
    .get_driver_name = edu_fence_name,
    .get_timeline_name = edu_fence_name,
};

static struct dma_fence *edu_run_job(struct drm_sched_job *sj)
{
    struct edu_job *j = container_of(sj, struct edu_job, base);
    struct edu *e = j->e;
    struct dma_fence *f = kzalloc(sizeof(*f), GFP_KERNEL);

    if (!f)
        return ERR_PTR(-ENOMEM);
    dma_fence_init(f, &edu_fence_ops, &sched_fence_lock, sched_fence_ctx, j->n + 1);
    if (j->n != sched_last + 1)
        sched_order_bad++;
    sched_last = j->n;
    atomic_inc(&sched_ran);
    dma_fence_get(f); /* the interrupt's reference */
    WRITE_ONCE(e->sched_hw_fence, f);
    writel(STATUS_IRQ_FACT, e->regs + EDU_STATUS);
    writel(5, e->regs + EDU_FACTORIAL);
    return f; /* the scheduler's reference */
}

static enum drm_gpu_sched_stat edu_timedout_job(struct drm_sched_job *sj)
{
    dev_info(&sched_edu->pdev->dev, "drm_sched test: job %d timed out\n",
             container_of(sj, struct edu_job, base)->n);
    return DRM_GPU_SCHED_STAT_NOMINAL;
}

static void edu_free_job(struct drm_sched_job *sj)
{
    drm_sched_job_cleanup(sj);
    kfree(container_of(sj, struct edu_job, base));
    atomic_inc(&sched_freed);
}

static const struct drm_sched_backend_ops edu_sched_ops = {
    .run_job = edu_run_job,
    .timedout_job = edu_timedout_job,
    .free_job = edu_free_job,
};

#define SCHED_JOBS 64

struct edu_flush_ctx {
    struct drm_sched_entity *entity;
    struct completion done;
};

static int edu_flush_thread(void *data)
{
    struct edu_flush_ctx *fc = data;

    drm_sched_entity_destroy(fc->entity); /* flush (no time limit: not exiting), then fini */
    complete(&fc->done);
    return 0;
}

static void edu_sched_test(struct edu *e)
{
    static struct drm_gpu_scheduler sched; /* static: see the flush test */
    static struct drm_gpu_scheduler *list[1];
    static struct drm_sched_entity entity;
    struct dma_fence *prev = NULL, *last = NULL;
    long left;
    int i, r;

    memset(&sched, 0, sizeof(sched));
    memset(&entity, 0, sizeof(entity));
    list[0] = &sched;
    sched_edu = e;
    sched_fence_ctx = dma_fence_context_alloc(1);
    r = drm_sched_init(&sched, &edu_sched_ops, NULL, DRM_SCHED_PRIORITY_COUNT, 1, 0, msecs_to_jiffies(2000), NULL,
                       NULL, "edu", &e->pdev->dev);
    check(e, r == 0, "drm_sched_init (GPU job scheduler)");
    if (r)
        return;
    r = drm_sched_entity_init(&entity, DRM_SCHED_PRIORITY_NORMAL, list, 1, NULL);
    check(e, r == 0, "drm_sched_entity_init");
    if (r) {
        drm_sched_fini(&sched);
        return;
    }
    for (i = 0; i < SCHED_JOBS; i++) {
        struct edu_job *j = kzalloc(sizeof(*j), GFP_KERNEL);

        if (!j)
            break;
        j->e = e;
        j->n = i;
        r = drm_sched_job_init(&j->base, &entity, 1, NULL);
        if (r) {
            kfree(j);
            break;
        }
        if (prev)
            drm_sched_job_add_dependency(&j->base, prev); /* takes the reference */
        drm_sched_job_arm(&j->base);
        prev = dma_fence_get(&j->base.s_fence->finished);
        if (i == SCHED_JOBS - 1)
            last = dma_fence_get(prev);
        drm_sched_entity_push_job(&j->base);
    }
    if (prev)
        dma_fence_put(prev); /* the last job's: no later job took it */
    left = last ? dma_fence_wait_timeout(last, false, msecs_to_jiffies(5000)) : 0;
    check(e, i == SCHED_JOBS && left > 0 && last->error == 0,
          "drm_sched: 64 chained jobs ran, hardware fences signalled from the device interrupt");
    check(e, atomic_read(&sched_ran) == SCHED_JOBS && !sched_order_bad, "drm_sched: every job ran once, in order");
    if (last)
        dma_fence_put(last);
    for (i = 0; i < 100 && atomic_read(&sched_freed) < SCHED_JOBS; i++)
        msleep(10); /* free_job runs from the scheduler's work item */
    check(e, atomic_read(&sched_freed) == SCHED_JOBS, "drm_sched: every finished job freed (free_job)");

    /* amdgpu closing a device file: a KERNEL-priority entity (the GPU
     * address space's page-table updates) on the same scheduler gets jobs
     * and is flushed and destroyed at once, while they are still queued,
     * from a live process (no PF_EXITING: the flush waits without a time
     * limit). On an RX 6700 XT that close never returned. Run in a thread:
     * a hang here fails the check instead of stopping the boot. */
    {
        static struct edu_flush_ctx fc; /* static: a hung flush thread keeps using them */
        static struct drm_sched_entity kentity;
        struct task_struct *t;

        memset(&kentity, 0, sizeof(kentity));
        r = drm_sched_entity_init(&kentity, DRM_SCHED_PRIORITY_KERNEL, list, 1, NULL);
        check(e, r == 0, "drm_sched_entity_init (kernel priority, beside a normal one)");
        if (!r) {
            int pushed = 0;

            for (i = 0; i < 8; i++) {
                struct edu_job *j = kzalloc(sizeof(*j), GFP_KERNEL);

                if (!j)
                    break;
                j->e = e;
                j->n = SCHED_JOBS + i;
                if (drm_sched_job_init(&j->base, &kentity, 1, NULL)) {
                    kfree(j);
                    break;
                }
                drm_sched_job_arm(&j->base);
                drm_sched_entity_push_job(&j->base);
                pushed++;
            }
            fc.entity = &kentity;
            init_completion(&fc.done);
            t = kthread_run(edu_flush_thread, &fc, "edu-flush");
            if (IS_ERR_OR_NULL(t)) {
                check(e, false, "drm_sched: flush thread");
            } else {
                unsigned long ok = wait_for_completion_timeout(&fc.done, msecs_to_jiffies(5000));
                check(e, ok != 0 && pushed == 8,
                      "drm_sched: entity flushed and destroyed with jobs still queued (amdgpu's file close)");
                if (!ok) {
                    dev_info(&e->pdev->dev, "drm_sched test: flush still waiting after 5 s (%d of %d jobs ran)\n",
                             atomic_read(&sched_ran) - SCHED_JOBS, pushed);
                    return; /* the scheduler stays: the hung thread still uses it */
                }
            }
        }
    }
    drm_sched_entity_destroy(&entity);
    drm_sched_fini(&sched);
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

    edu_sched_test(e);

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
