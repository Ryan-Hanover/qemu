/*
 * QEMU educational PCIe device, version 2 ("edu2")
 *
 * A more realistic educational device than "edu": a multi-queue,
 * descriptor-ring based DMA/compute offload engine, structured the way
 * real PCIe endpoints (NICs, dmaengine offload hardware, NVMe) are:
 *
 *  - PCI Express endpoint capability (when plugged into a PCIe bus) + FLR
 *  - Power Management capability
 *  - MSI-X (one vector per queue plus an error vector), MSI and INTx
 *  - Two independent DMA queues driven by in-memory descriptor rings
 *    with head/tail (doorbell) semantics and status write-back
 *  - COPY / FILL / CRC32 operations with per-descriptor completion status
 *  - Interrupt cause/enable registers with write-1-to-clear semantics
 *  - Per-queue interrupt throttling (moderation)
 *  - Error reporting: failing descriptors halt the queue with an error
 *    code that the driver must recover from
 *
 * See docs/specs/edu2.rst for the register interface specification.
 *
 * Copyright (c) 2026
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include <zlib.h> /* for crc32 */
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_PCI_EDU2_DEVICE "edu2"
typedef struct Edu2State Edu2State;
DECLARE_INSTANCE_CHECKER(Edu2State, EDU2, TYPE_PCI_EDU2_DEVICE)

#define EDU2_DEVICE_ID          0x11e9
#define EDU2_ID_VALUE           0xed200100  /* magic 0xed20, version 1.0 */

#define EDU2_NUM_QUEUES         2
#define EDU2_MSIX_VECTORS       (EDU2_NUM_QUEUES + 1)
#define EDU2_MSIX_VEC_ERROR     EDU2_NUM_QUEUES

#define EDU2_BAR0_SIZE          (16 * KiB)
#define EDU2_MSIX_TABLE_OFFSET  0x1000
#define EDU2_MSIX_PBA_OFFSET    0x2000
#define EDU2_REGS_SIZE          0x1000

#define EDU2_MAX_XFER           (1 * MiB)
#define EDU2_RING_MIN_SIZE      2
#define EDU2_RING_MAX_SIZE      32768
#define EDU2_RING_ALIGN         32

/* Global registers */
#define EDU2_REG_ID             0x000
#define EDU2_REG_CTRL           0x004
#define  EDU2_CTRL_RESET        BIT(0)
#define EDU2_REG_STATUS         0x008
#define  EDU2_STATUS_READY      BIT(0)
#define EDU2_REG_SCRATCH        0x00c
#define EDU2_REG_NUM_QUEUES     0x010
#define EDU2_REG_MAX_XFER       0x014

/* Interrupt registers */
#define EDU2_REG_ISR            0x020
#define EDU2_REG_IACK           0x024
#define EDU2_REG_IEN            0x028
#define EDU2_REG_IEN_SET        0x02c
#define EDU2_REG_IEN_CLR        0x030

#define EDU2_ISR_QDONE(n)       BIT(n)
#define EDU2_ISR_ERROR          BIT(16)
#define EDU2_ISR_ALL            (EDU2_ISR_QDONE(0) | EDU2_ISR_QDONE(1) | \
                                 EDU2_ISR_ERROR)

/* Per-queue register block: 0x100 + n * 0x40 */
#define EDU2_QUEUE_BASE         0x100
#define EDU2_QUEUE_STRIDE       0x40
#define EDU2_REG_QCTRL          0x00
#define  EDU2_QCTRL_ENABLE      BIT(0)
#define  EDU2_QCTRL_RESET       BIT(1)
#define EDU2_REG_QSTATUS        0x04
#define  EDU2_QSTATUS_ACTIVE    BIT(0)
#define  EDU2_QSTATUS_HALTED    BIT(1)
#define  EDU2_QSTATUS_ERR_SHIFT 8
#define EDU2_REG_QBASE_LO       0x08
#define EDU2_REG_QBASE_HI       0x0c
#define EDU2_REG_QSIZE          0x10
#define EDU2_REG_QHEAD          0x14
#define EDU2_REG_QTAIL          0x18
#define EDU2_REG_QITR           0x1c
#define EDU2_REG_QDONE_COUNT    0x20
#define EDU2_REG_QERR_COUNT     0x24

/* Descriptor layout (32 bytes, little-endian, in guest memory) */
#define EDU2_DESC_SIZE          32
#define EDU2_DESC_SRC           0   /* u64 */
#define EDU2_DESC_DST           8   /* u64 */
#define EDU2_DESC_LEN           16  /* u32 */
#define EDU2_DESC_OPCODE        20  /* u8 */
#define EDU2_DESC_FLAGS         21  /* u8 */
#define EDU2_DESC_RESULT        24  /* u32, written back */
#define EDU2_DESC_STATUS        28  /* u32, written back */

#define EDU2_OP_NOP             0x00
#define EDU2_OP_COPY            0x01
#define EDU2_OP_FILL            0x02
#define EDU2_OP_CRC32           0x03

#define EDU2_DESC_F_NO_IRQ      BIT(0)

#define EDU2_DESC_STATUS_DD     BIT(0)
#define EDU2_DESC_STATUS_ERR    BIT(1)
#define EDU2_DESC_STATUS_CODE(e) ((e) << 8)

/* Error codes (descriptor status bits 15:8 and QSTATUS bits 15:8) */
#define EDU2_ERR_NONE           0x00
#define EDU2_ERR_BAD_OPCODE     0x01
#define EDU2_ERR_BAD_LEN        0x02
#define EDU2_ERR_DMA_READ       0x03
#define EDU2_ERR_DMA_WRITE      0x04
#define EDU2_ERR_BAD_CONFIG     0x05

#define EDU2_CHUNK_SIZE         4096

typedef struct Edu2Queue {
    Edu2State *parent;
    uint32_t index;

    bool enabled;
    bool halted;
    uint32_t error;

    uint64_t base;
    uint32_t size;          /* number of descriptors, power of two */
    uint32_t head;          /* device consumer index */
    uint32_t tail;          /* driver producer index (doorbell) */

    uint32_t itr;           /* interrupt throttle interval, microseconds */
    uint32_t done_count;
    uint32_t err_count;

    bool irq_pending;       /* completion IRQ deferred by throttling */
    int64_t next_irq;       /* QEMU_CLOCK_VIRTUAL ns: next IRQ allowed */

    QEMUTimer proc_timer;
    QEMUTimer itr_timer;
} Edu2Queue;

struct Edu2State {
    PCIDevice pdev;
    MemoryRegion bar0;      /* container: registers + MSI-X table/PBA */
    MemoryRegion regs;

    uint32_t scratch;
    uint32_t isr;
    uint32_t ien;

    Edu2Queue queue[EDU2_NUM_QUEUES];

    /* Simulated processing latency between doorbell and execution. */
    uint64_t latency_ns;
};

/* Interrupt cause bit -> MSI-X vector */
static unsigned edu2_cause_to_vector(uint32_t cause)
{
    if (cause == EDU2_ISR_ERROR) {
        return EDU2_MSIX_VEC_ERROR;
    }
    return ctz32(cause);
}

static void edu2_intx_update(Edu2State *s)
{
    if (msix_enabled(&s->pdev) || msi_enabled(&s->pdev)) {
        pci_set_irq(&s->pdev, 0);
        return;
    }
    pci_set_irq(&s->pdev, !!(s->isr & s->ien));
}

/*
 * Fire message interrupts for the given causes.  Message (MSI/MSI-X)
 * interrupts are edge triggered: one message per cause bit that becomes
 * both pending and enabled.  INTx is level triggered on (ISR & IEN).
 */
static void edu2_notify(Edu2State *s, uint32_t causes)
{
    if (msix_enabled(&s->pdev)) {
        while (causes) {
            uint32_t cause = causes & -causes;

            msix_notify(&s->pdev, edu2_cause_to_vector(cause));
            causes &= ~cause;
        }
    } else if (msi_enabled(&s->pdev)) {
        if (causes) {
            msi_notify(&s->pdev, 0);
        }
    } else {
        edu2_intx_update(s);
    }
}

static void edu2_raise_causes(Edu2State *s, uint32_t causes)
{
    uint32_t newbits = causes & ~s->isr;

    s->isr |= causes;
    edu2_notify(s, newbits & s->ien);
}

static void edu2_queue_stop_timers(Edu2Queue *q)
{
    timer_del(&q->proc_timer);
    timer_del(&q->itr_timer);
    q->irq_pending = false;
}

static void edu2_queue_halt(Edu2Queue *q, uint32_t error)
{
    q->halted = true;
    q->error = error;
    q->err_count++;
    edu2_queue_stop_timers(q);
    edu2_raise_causes(q->parent, EDU2_ISR_ERROR);
}

/*
 * Raise (or throttle) the completion interrupt for a queue.  QITR
 * enforces a minimum gap, in microseconds, between two completion
 * interrupts of the same queue; completions arriving inside the window
 * are coalesced into a single deferred interrupt.
 */
static void edu2_queue_completion_irq(Edu2Queue *q)
{
    Edu2State *s = q->parent;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (q->itr == 0 || now >= q->next_irq) {
        q->next_irq = now + (int64_t)q->itr * 1000;
        edu2_raise_causes(s, EDU2_ISR_QDONE(q->index));
    } else if (!q->irq_pending) {
        q->irq_pending = true;
        timer_mod(&q->itr_timer, q->next_irq);
    }
}

static void edu2_itr_timer_cb(void *opaque)
{
    Edu2Queue *q = opaque;

    if (q->irq_pending) {
        q->irq_pending = false;
        q->next_irq = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      (int64_t)q->itr * 1000;
        edu2_raise_causes(q->parent, EDU2_ISR_QDONE(q->index));
    }
}

static uint32_t edu2_desc_copy(Edu2State *s, uint64_t src, uint64_t dst,
                               uint32_t len)
{
    uint8_t buf[EDU2_CHUNK_SIZE];

    while (len) {
        uint32_t chunk = MIN(len, sizeof(buf));

        if (pci_dma_read(&s->pdev, src, buf, chunk) != MEMTX_OK) {
            return EDU2_ERR_DMA_READ;
        }
        if (pci_dma_write(&s->pdev, dst, buf, chunk) != MEMTX_OK) {
            return EDU2_ERR_DMA_WRITE;
        }
        src += chunk;
        dst += chunk;
        len -= chunk;
    }

    return EDU2_ERR_NONE;
}

static uint32_t edu2_desc_fill(Edu2State *s, uint64_t pattern, uint64_t dst,
                               uint32_t len)
{
    uint8_t buf[EDU2_CHUNK_SIZE];

    for (unsigned i = 0; i < sizeof(buf); i += 4) {
        stl_le_p(buf + i, (uint32_t)pattern);
    }

    while (len) {
        uint32_t chunk = MIN(len, sizeof(buf));

        if (pci_dma_write(&s->pdev, dst, buf, chunk) != MEMTX_OK) {
            return EDU2_ERR_DMA_WRITE;
        }
        dst += chunk;
        len -= chunk;
    }

    return EDU2_ERR_NONE;
}

static uint32_t edu2_desc_crc32(Edu2State *s, uint64_t src, uint32_t seed,
                                uint32_t len, uint32_t *result)
{
    uint8_t buf[EDU2_CHUNK_SIZE];
    uint32_t crc = seed;

    while (len) {
        uint32_t chunk = MIN(len, sizeof(buf));

        if (pci_dma_read(&s->pdev, src, buf, chunk) != MEMTX_OK) {
            return EDU2_ERR_DMA_READ;
        }
        crc = crc32(crc, buf, chunk);
        src += chunk;
        len -= chunk;
    }

    *result = crc;
    return EDU2_ERR_NONE;
}

/*
 * Execute one descriptor.  Returns an EDU2_ERR_* code and, on success,
 * the value to write back to the descriptor's result field.
 */
static uint32_t edu2_desc_execute(Edu2State *s, const uint8_t *desc,
                                  uint32_t *result)
{
    uint64_t src = ldq_le_p(desc + EDU2_DESC_SRC);
    uint64_t dst = ldq_le_p(desc + EDU2_DESC_DST);
    uint32_t len = ldl_le_p(desc + EDU2_DESC_LEN);
    uint8_t opcode = desc[EDU2_DESC_OPCODE];

    *result = 0;

    if (opcode == EDU2_OP_NOP) {
        return EDU2_ERR_NONE;
    }

    if (len == 0 || len > EDU2_MAX_XFER) {
        return EDU2_ERR_BAD_LEN;
    }

    switch (opcode) {
    case EDU2_OP_COPY:
        return edu2_desc_copy(s, src, dst, len);
    case EDU2_OP_FILL:
        return edu2_desc_fill(s, src, dst, len);
    case EDU2_OP_CRC32:
        return edu2_desc_crc32(s, src, (uint32_t)dst, len, result);
    default:
        return EDU2_ERR_BAD_OPCODE;
    }
}

static void edu2_queue_process(void *opaque)
{
    Edu2Queue *q = opaque;
    Edu2State *s = q->parent;
    bool completion = false;
    uint32_t tail = q->tail;

    if (!q->enabled || q->halted) {
        return;
    }

    while (q->head != tail) {
        uint64_t desc_addr = q->base + (uint64_t)q->head * EDU2_DESC_SIZE;
        uint8_t desc[EDU2_DESC_SIZE];
        uint8_t wb[8];
        uint32_t result = 0;
        uint32_t err;
        uint32_t status;

        if (pci_dma_read(&s->pdev, desc_addr, desc, sizeof(desc)) !=
            MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "edu2: q%u: failed to fetch descriptor at "
                          "0x%" PRIx64 "\n", q->index, desc_addr);
            edu2_queue_halt(q, EDU2_ERR_DMA_READ);
            break;
        }

        err = edu2_desc_execute(s, desc, &result);

        status = EDU2_DESC_STATUS_DD;
        if (err != EDU2_ERR_NONE) {
            status |= EDU2_DESC_STATUS_ERR | EDU2_DESC_STATUS_CODE(err);
        }
        stl_le_p(wb, result);
        stl_le_p(wb + 4, status);
        if (pci_dma_write(&s->pdev, desc_addr + EDU2_DESC_RESULT, wb,
                          sizeof(wb)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "edu2: q%u: descriptor write-back failed at "
                          "0x%" PRIx64 "\n", q->index, desc_addr);
            err = EDU2_ERR_DMA_WRITE;
        }

        if (err != EDU2_ERR_NONE) {
            /* Head is left pointing at the failing descriptor. */
            edu2_queue_halt(q, err);
            break;
        }

        q->head = (q->head + 1) & (q->size - 1);
        q->done_count++;
        if (!(desc[EDU2_DESC_FLAGS] & EDU2_DESC_F_NO_IRQ)) {
            completion = true;
        }
    }

    if (completion) {
        edu2_queue_completion_irq(q);
    }
}

static void edu2_queue_reset(Edu2Queue *q, bool full)
{
    edu2_queue_stop_timers(q);
    q->enabled = false;
    q->halted = false;
    q->error = EDU2_ERR_NONE;
    q->head = 0;
    q->tail = 0;
    q->done_count = 0;
    q->err_count = 0;
    q->next_irq = 0;
    if (full) {
        q->base = 0;
        q->size = 0;
        q->itr = 0;
    }
}

static void edu2_soft_reset(Edu2State *s)
{
    for (unsigned i = 0; i < EDU2_NUM_QUEUES; i++) {
        edu2_queue_reset(&s->queue[i], true);
    }
    s->scratch = 0;
    s->isr = 0;
    s->ien = 0;
    edu2_intx_update(s);
}

static void edu2_queue_enable(Edu2Queue *q)
{
    if (q->enabled) {
        return;
    }

    if ((q->base & (EDU2_RING_ALIGN - 1)) ||
        !is_power_of_2(q->size) ||
        q->size < EDU2_RING_MIN_SIZE || q->size > EDU2_RING_MAX_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "edu2: q%u: invalid ring config: base 0x%" PRIx64
                      " size %u\n", q->index, q->base, q->size);
        edu2_queue_halt(q, EDU2_ERR_BAD_CONFIG);
        return;
    }

    q->enabled = true;
    q->halted = false;
    q->error = EDU2_ERR_NONE;
    q->head = 0;
    q->tail = 0;
}

static void edu2_queue_write(Edu2State *s, Edu2Queue *q, hwaddr addr,
                             uint32_t val)
{
    switch (addr) {
    case EDU2_REG_QCTRL:
        if (val & EDU2_QCTRL_RESET) {
            edu2_queue_reset(q, false);
            break;
        }
        if (val & EDU2_QCTRL_ENABLE) {
            edu2_queue_enable(q);
        } else if (q->enabled) {
            q->enabled = false;
            edu2_queue_stop_timers(q);
        }
        break;
    case EDU2_REG_QBASE_LO:
        if (!q->enabled) {
            q->base = deposit64(q->base, 0, 32, val);
        }
        break;
    case EDU2_REG_QBASE_HI:
        if (!q->enabled) {
            q->base = deposit64(q->base, 32, 32, val);
        }
        break;
    case EDU2_REG_QSIZE:
        if (!q->enabled) {
            q->size = val;
        }
        break;
    case EDU2_REG_QTAIL:
        if (!q->enabled || q->halted) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "edu2: q%u: doorbell while queue not running\n",
                          q->index);
            break;
        }
        if (val >= q->size) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "edu2: q%u: doorbell value %u out of range\n",
                          q->index, val);
            break;
        }
        q->tail = val;
        timer_mod(&q->proc_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->latency_ns);
        break;
    case EDU2_REG_QITR:
        q->itr = MIN(val, 10000);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "edu2: q%u: write to invalid register 0x%"
                      HWADDR_PRIx "\n", q->index, addr);
        break;
    }
}

static uint32_t edu2_queue_read(Edu2Queue *q, hwaddr addr)
{
    switch (addr) {
    case EDU2_REG_QCTRL:
        return q->enabled ? EDU2_QCTRL_ENABLE : 0;
    case EDU2_REG_QSTATUS:
        return ((q->enabled && !q->halted) ? EDU2_QSTATUS_ACTIVE : 0) |
               (q->halted ? EDU2_QSTATUS_HALTED : 0) |
               (q->error << EDU2_QSTATUS_ERR_SHIFT);
    case EDU2_REG_QBASE_LO:
        return extract64(q->base, 0, 32);
    case EDU2_REG_QBASE_HI:
        return extract64(q->base, 32, 32);
    case EDU2_REG_QSIZE:
        return q->size;
    case EDU2_REG_QHEAD:
        return q->head;
    case EDU2_REG_QTAIL:
        return q->tail;
    case EDU2_REG_QITR:
        return q->itr;
    case EDU2_REG_QDONE_COUNT:
        return q->done_count;
    case EDU2_REG_QERR_COUNT:
        return q->err_count;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "edu2: q%u: read from invalid register 0x%"
                      HWADDR_PRIx "\n", q->index, addr);
        return 0;
    }
}

static uint64_t edu2_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Edu2State *s = opaque;

    if (addr >= EDU2_QUEUE_BASE &&
        addr < EDU2_QUEUE_BASE + EDU2_NUM_QUEUES * EDU2_QUEUE_STRIDE) {
        unsigned n = (addr - EDU2_QUEUE_BASE) / EDU2_QUEUE_STRIDE;

        return edu2_queue_read(&s->queue[n],
                               (addr - EDU2_QUEUE_BASE) % EDU2_QUEUE_STRIDE);
    }

    switch (addr) {
    case EDU2_REG_ID:
        return EDU2_ID_VALUE;
    case EDU2_REG_CTRL:
        return 0;
    case EDU2_REG_STATUS:
        return EDU2_STATUS_READY;
    case EDU2_REG_SCRATCH:
        return s->scratch;
    case EDU2_REG_NUM_QUEUES:
        return EDU2_NUM_QUEUES;
    case EDU2_REG_MAX_XFER:
        return EDU2_MAX_XFER;
    case EDU2_REG_ISR:
        return s->isr;
    case EDU2_REG_IEN:
        return s->ien;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "edu2: read from invalid register 0x%" HWADDR_PRIx "\n",
                      addr);
        return 0;
    }
}

static void edu2_mmio_write(void *opaque, hwaddr addr, uint64_t val64,
                            unsigned size)
{
    Edu2State *s = opaque;
    uint32_t val = val64;

    if (addr >= EDU2_QUEUE_BASE &&
        addr < EDU2_QUEUE_BASE + EDU2_NUM_QUEUES * EDU2_QUEUE_STRIDE) {
        unsigned n = (addr - EDU2_QUEUE_BASE) / EDU2_QUEUE_STRIDE;

        edu2_queue_write(s, &s->queue[n],
                         (addr - EDU2_QUEUE_BASE) % EDU2_QUEUE_STRIDE, val);
        return;
    }

    switch (addr) {
    case EDU2_REG_CTRL:
        if (val & EDU2_CTRL_RESET) {
            edu2_soft_reset(s);
        }
        break;
    case EDU2_REG_SCRATCH:
        s->scratch = val;
        break;
    case EDU2_REG_IACK:
        s->isr &= ~val;
        edu2_intx_update(s);
        break;
    case EDU2_REG_IEN_SET: {
        uint32_t newly = val & EDU2_ISR_ALL & ~s->ien;

        s->ien |= val & EDU2_ISR_ALL;
        /* Causes already pending when enabled fire immediately. */
        edu2_notify(s, newly & s->isr);
        break;
    }
    case EDU2_REG_IEN_CLR:
        s->ien &= ~val;
        edu2_intx_update(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "edu2: write to invalid register 0x%" HWADDR_PRIx "\n",
                      addr);
        break;
    }
}

static const MemoryRegionOps edu2_mmio_ops = {
    .read = edu2_mmio_read,
    .write = edu2_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void edu2_write_config(PCIDevice *pdev, uint32_t addr, uint32_t val,
                              int len)
{
    Edu2State *s = EDU2(pdev);

    pci_default_write_config(pdev, addr, val, len);
    if (pci_is_express(pdev)) {
        pcie_cap_flr_write_config(pdev, addr, val, len);
    }
    /* Enabling/disabling MSI or MSI-X changes how INTx is driven. */
    edu2_intx_update(s);
}

static void edu2_reset(DeviceState *dev)
{
    Edu2State *s = EDU2(dev);

    edu2_soft_reset(s);
    msix_reset(&s->pdev);
}

static void pci_edu2_realize(PCIDevice *pdev, Error **errp)
{
    Edu2State *s = EDU2(pdev);
    uint8_t *pci_conf = pdev->config;
    int ret;

    pci_config_set_interrupt_pin(pci_conf, 1);

    if (pci_pm_init(pdev, 0x60, errp) < 0) {
        return;
    }

    if (pci_bus_is_express(pci_get_bus(pdev))) {
        ret = pcie_endpoint_cap_init(pdev, 0x80);
        if (ret < 0) {
            error_setg(errp, "failed to initialize PCIe capability");
            return;
        }
        pcie_cap_flr_init(pdev);
    }

    if (msi_init(pdev, 0, 1, true, false, errp) < 0) {
        return;
    }

    memory_region_init(&s->bar0, OBJECT(s), "edu2-bar0", EDU2_BAR0_SIZE);
    memory_region_init_io(&s->regs, OBJECT(s), &edu2_mmio_ops, s,
                          "edu2-regs", EDU2_REGS_SIZE);
    memory_region_add_subregion(&s->bar0, 0, &s->regs);

    ret = msix_init(pdev, EDU2_MSIX_VECTORS,
                    &s->bar0, 0, EDU2_MSIX_TABLE_OFFSET,
                    &s->bar0, 0, EDU2_MSIX_PBA_OFFSET, 0, errp);
    if (ret < 0) {
        msi_uninit(pdev);
        return;
    }

    for (unsigned i = 0; i < EDU2_MSIX_VECTORS; i++) {
        msix_vector_use(pdev, i);
    }

    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->bar0);

    for (unsigned i = 0; i < EDU2_NUM_QUEUES; i++) {
        Edu2Queue *q = &s->queue[i];

        q->parent = s;
        q->index = i;
        timer_init_ns(&q->proc_timer, QEMU_CLOCK_VIRTUAL,
                      edu2_queue_process, q);
        timer_init_ns(&q->itr_timer, QEMU_CLOCK_VIRTUAL,
                      edu2_itr_timer_cb, q);
    }
}

static void pci_edu2_uninit(PCIDevice *pdev)
{
    Edu2State *s = EDU2(pdev);

    for (unsigned i = 0; i < EDU2_NUM_QUEUES; i++) {
        timer_del(&s->queue[i].proc_timer);
        timer_del(&s->queue[i].itr_timer);
    }
    msix_unuse_all_vectors(pdev);
    msix_uninit(pdev, &s->bar0, &s->bar0);
    msi_uninit(pdev);
}

static const VMStateDescription vmstate_edu2_queue = {
    .name = "edu2/queue",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(enabled, Edu2Queue),
        VMSTATE_BOOL(halted, Edu2Queue),
        VMSTATE_UINT32(error, Edu2Queue),
        VMSTATE_UINT64(base, Edu2Queue),
        VMSTATE_UINT32(size, Edu2Queue),
        VMSTATE_UINT32(head, Edu2Queue),
        VMSTATE_UINT32(tail, Edu2Queue),
        VMSTATE_UINT32(itr, Edu2Queue),
        VMSTATE_UINT32(done_count, Edu2Queue),
        VMSTATE_UINT32(err_count, Edu2Queue),
        VMSTATE_BOOL(irq_pending, Edu2Queue),
        VMSTATE_INT64(next_irq, Edu2Queue),
        VMSTATE_TIMER(proc_timer, Edu2Queue),
        VMSTATE_TIMER(itr_timer, Edu2Queue),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_edu2 = {
    .name = "edu2",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(pdev, Edu2State),
        VMSTATE_MSIX(pdev, Edu2State),
        VMSTATE_UINT32(scratch, Edu2State),
        VMSTATE_UINT32(isr, Edu2State),
        VMSTATE_UINT32(ien, Edu2State),
        VMSTATE_STRUCT_ARRAY(queue, Edu2State, EDU2_NUM_QUEUES, 1,
                             vmstate_edu2_queue, Edu2Queue),
        VMSTATE_END_OF_LIST()
    },
};

static void edu2_instance_init(Object *obj)
{
    /*
     * Hybrid PCI/PCIe device: express config space is enabled up front;
     * the express capability itself is only added when the device is
     * plugged into an express bus (see pci_edu2_realize()).
     */
    PCI_DEVICE(obj)->cap_present |= QEMU_PCI_CAP_EXPRESS;
}

static const Property edu2_properties[] = {
    DEFINE_PROP_UINT64("latency-ns", Edu2State, latency_ns, 50000),
};

static void edu2_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize = pci_edu2_realize;
    k->exit = pci_edu2_uninit;
    k->config_write = edu2_write_config;
    k->vendor_id = PCI_VENDOR_ID_QEMU;
    k->device_id = EDU2_DEVICE_ID;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_OTHERS;
    dc->desc = "EDU2 educational PCIe DMA engine";
    dc->vmsd = &vmstate_edu2;
    device_class_set_legacy_reset(dc, edu2_reset);
    device_class_set_props(dc, edu2_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo edu2_types[] = {
    {
        .name          = TYPE_PCI_EDU2_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(Edu2State),
        .instance_init = edu2_instance_init,
        .class_init    = edu2_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { INTERFACE_PCIE_DEVICE },
            { },
        },
    }
};

DEFINE_TYPES(edu2_types)
