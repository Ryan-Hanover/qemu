/*
 * QTest testcase for the edu2 educational PCIe DMA engine
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqos/libqos-pc.h"
#include "libqtest.h"

/* Register interface, see docs/specs/edu2.rst */
#define EDU2_REG_ID             0x000
#define EDU2_REG_CTRL           0x004
#define  EDU2_CTRL_RESET        1
#define EDU2_REG_STATUS         0x008
#define EDU2_REG_SCRATCH        0x00c
#define EDU2_REG_NUM_QUEUES     0x010
#define EDU2_REG_MAX_XFER       0x014

#define EDU2_REG_ISR            0x020
#define EDU2_REG_IACK           0x024
#define EDU2_REG_IEN            0x028
#define EDU2_REG_IEN_SET        0x02c
#define EDU2_REG_IEN_CLR        0x030

#define EDU2_ISR_QDONE0         (1u << 0)
#define EDU2_ISR_QDONE1         (1u << 1)
#define EDU2_ISR_ERROR          (1u << 16)

#define EDU2_Q0                 0x100
#define EDU2_REG_QCTRL          0x00
#define  EDU2_QCTRL_ENABLE      1
#define  EDU2_QCTRL_RESET       2
#define EDU2_REG_QSTATUS        0x04
#define  EDU2_QSTATUS_ACTIVE    1
#define  EDU2_QSTATUS_HALTED    2
#define EDU2_REG_QBASE_LO       0x08
#define EDU2_REG_QBASE_HI       0x0c
#define EDU2_REG_QSIZE          0x10
#define EDU2_REG_QHEAD          0x14
#define EDU2_REG_QTAIL          0x18
#define EDU2_REG_QITR           0x1c
#define EDU2_REG_QDONE_COUNT    0x20
#define EDU2_REG_QERR_COUNT     0x24

#define EDU2_DESC_SIZE          32

#define EDU2_OP_NOP             0x00
#define EDU2_OP_COPY            0x01
#define EDU2_OP_FILL            0x02
#define EDU2_OP_CRC32           0x03

#define EDU2_DESC_F_NO_IRQ      1

#define EDU2_DESC_STATUS_DD     1
#define EDU2_DESC_STATUS_ERR    2

#define EDU2_ERR_BAD_OPCODE     0x01
#define EDU2_ERR_BAD_LEN        0x02
#define EDU2_ERR_BAD_CONFIG     0x05

/* Default latency-ns is 50000; step a bit more than that. */
#define PROC_STEP_NS            (60 * 1000)

typedef struct {
    QOSState *qs;
    QPCIDevice *dev;
    QPCIBar bar;
} Edu2Test;

static void save_fn(QPCIDevice *dev, int devfn, void *data)
{
    QPCIDevice **pdev = (QPCIDevice **)data;

    *pdev = dev;
}

static void setup_vm(Edu2Test *s, const char *extra)
{
    uint64_t barsize;
    char *cmd = g_strdup_printf("-device edu2%s", extra ?: "");

    s->qs = qtest_pc_boot("%s", cmd);
    g_free(cmd);

    s->dev = NULL;
    qpci_device_foreach(s->qs->pcibus, 0x1234, 0x11e9, save_fn, &s->dev);
    g_assert_nonnull(s->dev);

    s->bar = qpci_iomap(s->dev, 0, &barsize);
    g_assert_cmpuint(barsize, ==, 16 * 1024);
    qpci_device_enable(s->dev);
}

static void cleanup_vm(Edu2Test *s)
{
    g_free(s->dev);
    qtest_shutdown(s->qs);
}

static uint32_t in_reg(Edu2Test *s, uint32_t reg)
{
    return qpci_io_readl(s->dev, s->bar, reg);
}

static void out_reg(Edu2Test *s, uint32_t reg, uint32_t val)
{
    qpci_io_writel(s->dev, s->bar, reg, val);
}

/* Bitwise CRC32 (IEEE 802.3, reflected, same as zlib crc32()) */
static uint32_t sw_crc32(uint32_t seed, const uint8_t *buf, size_t len)
{
    uint32_t crc = ~seed;

    while (len--) {
        crc ^= *buf++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
        }
    }

    return ~crc;
}

static void write_desc(Edu2Test *s, uint64_t ring, uint32_t idx,
                       uint64_t src, uint64_t dst, uint32_t len,
                       uint8_t opcode, uint8_t flags)
{
    uint8_t desc[EDU2_DESC_SIZE] = { 0 };

    memcpy(desc + 0, &src, 8);
    memcpy(desc + 8, &dst, 8);
    memcpy(desc + 16, &len, 4);
    desc[20] = opcode;
    desc[21] = flags;

    qtest_memwrite(s->qs->qts, ring + idx * EDU2_DESC_SIZE, desc,
                   sizeof(desc));
}

static uint32_t read_desc_status(Edu2Test *s, uint64_t ring, uint32_t idx)
{
    uint8_t buf[4];

    qtest_memread(s->qs->qts, ring + idx * EDU2_DESC_SIZE + 28, buf, 4);
    return buf[0] | buf[1] << 8 | buf[2] << 16 | ((uint32_t)buf[3]) << 24;
}

static uint32_t read_desc_result(Edu2Test *s, uint64_t ring, uint32_t idx)
{
    uint8_t buf[4];

    qtest_memread(s->qs->qts, ring + idx * EDU2_DESC_SIZE + 24, buf, 4);
    return buf[0] | buf[1] << 8 | buf[2] << 16 | ((uint32_t)buf[3]) << 24;
}

static void setup_queue(Edu2Test *s, uint32_t qbase, uint64_t ring,
                        uint32_t size)
{
    out_reg(s, qbase + EDU2_REG_QBASE_LO, ring & 0xffffffff);
    out_reg(s, qbase + EDU2_REG_QBASE_HI, ring >> 32);
    out_reg(s, qbase + EDU2_REG_QSIZE, size);
    out_reg(s, qbase + EDU2_REG_QCTRL, EDU2_QCTRL_ENABLE);
    g_assert_cmpuint(in_reg(s, qbase + EDU2_REG_QSTATUS) & 0xff, ==,
                     EDU2_QSTATUS_ACTIVE);
}

static void test_regs(void)
{
    Edu2Test s;

    setup_vm(&s, NULL);

    g_assert_cmphex(in_reg(&s, EDU2_REG_ID), ==, 0xed200100);
    g_assert_cmpuint(in_reg(&s, EDU2_REG_STATUS), ==, 1);
    g_assert_cmpuint(in_reg(&s, EDU2_REG_NUM_QUEUES), ==, 2);
    g_assert_cmphex(in_reg(&s, EDU2_REG_MAX_XFER), ==, 1024 * 1024);

    out_reg(&s, EDU2_REG_SCRATCH, 0xdeadbeef);
    g_assert_cmphex(in_reg(&s, EDU2_REG_SCRATCH), ==, 0xdeadbeef);

    /* Interrupt enable set/clear */
    out_reg(&s, EDU2_REG_IEN_SET, EDU2_ISR_QDONE0 | EDU2_ISR_ERROR);
    g_assert_cmphex(in_reg(&s, EDU2_REG_IEN), ==,
                    EDU2_ISR_QDONE0 | EDU2_ISR_ERROR);
    out_reg(&s, EDU2_REG_IEN_CLR, EDU2_ISR_QDONE0);
    g_assert_cmphex(in_reg(&s, EDU2_REG_IEN), ==, EDU2_ISR_ERROR);

    /* Soft reset clears everything */
    out_reg(&s, EDU2_REG_CTRL, EDU2_CTRL_RESET);
    g_assert_cmphex(in_reg(&s, EDU2_REG_SCRATCH), ==, 0);
    g_assert_cmphex(in_reg(&s, EDU2_REG_IEN), ==, 0);

    cleanup_vm(&s);
}

static void test_copy_fill_crc(void)
{
    Edu2Test s;
    uint64_t ring, src, dst;
    uint8_t pattern[1024], readback[1024];
    uint32_t status;

    setup_vm(&s, NULL);

    ring = guest_alloc(&s.qs->alloc, 16 * EDU2_DESC_SIZE);
    src = guest_alloc(&s.qs->alloc, sizeof(pattern));
    dst = guest_alloc(&s.qs->alloc, sizeof(pattern));

    for (unsigned i = 0; i < sizeof(pattern); i++) {
        pattern[i] = i * 7 + 3;
    }
    qtest_memwrite(s.qs->qts, src, pattern, sizeof(pattern));

    setup_queue(&s, EDU2_Q0, ring, 16);

    /* Three ops in one batch: COPY, FILL over the copy tail, CRC32 */
    write_desc(&s, ring, 0, src, dst, sizeof(pattern), EDU2_OP_COPY, 0);
    write_desc(&s, ring, 1, 0xa5a5a5a5, dst + 512, 512, EDU2_OP_FILL, 0);
    write_desc(&s, ring, 2, src, 0 /* seed */, sizeof(pattern),
               EDU2_OP_CRC32, 0);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QTAIL, 3);

    /* Nothing happens until virtual time advances past the latency */
    g_assert_cmpuint(in_reg(&s, EDU2_Q0 + EDU2_REG_QHEAD), ==, 0);
    qtest_clock_step(s.qs->qts, PROC_STEP_NS);

    g_assert_cmpuint(in_reg(&s, EDU2_Q0 + EDU2_REG_QHEAD), ==, 3);
    g_assert_cmpuint(in_reg(&s, EDU2_Q0 + EDU2_REG_QDONE_COUNT), ==, 3);

    for (int i = 0; i < 3; i++) {
        status = read_desc_status(&s, ring, i);
        g_assert_cmphex(status, ==, EDU2_DESC_STATUS_DD);
    }

    qtest_memread(s.qs->qts, dst, readback, sizeof(readback));
    g_assert_cmpmem(readback, 512, pattern, 512);
    for (unsigned i = 512; i < 1024; i++) {
        g_assert_cmphex(readback[i], ==, 0xa5);
    }

    g_assert_cmphex(read_desc_result(&s, ring, 2), ==,
                    sw_crc32(0, pattern, sizeof(pattern)));

    /* Chained CRC: feed the result back as the seed of a second half */
    write_desc(&s, ring, 3, src, 0, 512, EDU2_OP_CRC32, 0);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QTAIL, 4);
    qtest_clock_step(s.qs->qts, PROC_STEP_NS);
    write_desc(&s, ring, 4, src + 512, read_desc_result(&s, ring, 3), 512,
               EDU2_OP_CRC32, 0);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QTAIL, 5);
    qtest_clock_step(s.qs->qts, PROC_STEP_NS);

    g_assert_cmphex(read_desc_result(&s, ring, 4), ==,
                    sw_crc32(0, pattern, sizeof(pattern)));

    /* Ring wrap-around: 11 more NOPs pushes head past the end */
    for (int i = 0; i < 11; i++) {
        write_desc(&s, ring, (5 + i) % 16, 0, 0, 0, EDU2_OP_NOP, 0);
    }
    out_reg(&s, EDU2_Q0 + EDU2_REG_QTAIL, 0);
    qtest_clock_step(s.qs->qts, PROC_STEP_NS);
    g_assert_cmpuint(in_reg(&s, EDU2_Q0 + EDU2_REG_QHEAD), ==, 0);
    g_assert_cmpuint(in_reg(&s, EDU2_Q0 + EDU2_REG_QDONE_COUNT), ==, 16);

    guest_free(&s.qs->alloc, ring);
    guest_free(&s.qs->alloc, src);
    guest_free(&s.qs->alloc, dst);
    cleanup_vm(&s);
}

static void test_error_recovery(void)
{
    Edu2Test s;
    uint64_t ring;
    uint32_t status;

    setup_vm(&s, NULL);

    ring = guest_alloc(&s.qs->alloc, 8 * EDU2_DESC_SIZE);

    /* Invalid ring size refuses to enable */
    out_reg(&s, EDU2_Q0 + EDU2_REG_QBASE_LO, ring & 0xffffffff);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QBASE_HI, ring >> 32);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QSIZE, 7);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QCTRL, EDU2_QCTRL_ENABLE);
    status = in_reg(&s, EDU2_Q0 + EDU2_REG_QSTATUS);
    g_assert_cmphex(status, ==,
                    EDU2_QSTATUS_HALTED | (EDU2_ERR_BAD_CONFIG << 8));
    g_assert_cmphex(in_reg(&s, EDU2_REG_ISR), ==, EDU2_ISR_ERROR);
    out_reg(&s, EDU2_REG_IACK, EDU2_ISR_ERROR);

    /* Recover with QRESET and a valid config */
    out_reg(&s, EDU2_Q0 + EDU2_REG_QCTRL, EDU2_QCTRL_RESET);
    g_assert_cmphex(in_reg(&s, EDU2_Q0 + EDU2_REG_QSTATUS), ==, 0);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QSIZE, 8);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QCTRL, EDU2_QCTRL_ENABLE);

    /* NOP, then a bad opcode: queue halts on the second descriptor */
    write_desc(&s, ring, 0, 0, 0, 0, EDU2_OP_NOP, 0);
    write_desc(&s, ring, 1, 0, 0, 16, 0x7f, 0);
    write_desc(&s, ring, 2, 0, 0, 0, EDU2_OP_NOP, 0);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QTAIL, 3);
    qtest_clock_step(s.qs->qts, PROC_STEP_NS);

    /* Head points at the failing descriptor, desc 2 untouched */
    g_assert_cmpuint(in_reg(&s, EDU2_Q0 + EDU2_REG_QHEAD), ==, 1);
    status = in_reg(&s, EDU2_Q0 + EDU2_REG_QSTATUS);
    g_assert_cmphex(status, ==,
                    EDU2_QSTATUS_HALTED | (EDU2_ERR_BAD_OPCODE << 8));
    g_assert_cmphex(read_desc_status(&s, ring, 1), ==,
                    EDU2_DESC_STATUS_DD | EDU2_DESC_STATUS_ERR |
                    (EDU2_ERR_BAD_OPCODE << 8));
    g_assert_cmphex(read_desc_status(&s, ring, 2), ==, 0);
    /* The NOP before the bad descriptor completed, so QDONE0 is also set */
    g_assert_cmphex(in_reg(&s, EDU2_REG_ISR), ==,
                    EDU2_ISR_ERROR | EDU2_ISR_QDONE0);
    out_reg(&s, EDU2_REG_IACK, EDU2_ISR_QDONE0);
    g_assert_cmpuint(in_reg(&s, EDU2_Q0 + EDU2_REG_QERR_COUNT), ==, 1);

    /* Doorbells are ignored while halted */
    out_reg(&s, EDU2_Q0 + EDU2_REG_QTAIL, 3);
    qtest_clock_step(s.qs->qts, PROC_STEP_NS);
    g_assert_cmpuint(in_reg(&s, EDU2_Q0 + EDU2_REG_QHEAD), ==, 1);

    /* Recover and resubmit */
    out_reg(&s, EDU2_REG_IACK, EDU2_ISR_ERROR);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QCTRL, EDU2_QCTRL_RESET);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QCTRL, EDU2_QCTRL_ENABLE);
    write_desc(&s, ring, 0, 0, 0, 0, EDU2_OP_NOP, 0);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QTAIL, 1);
    qtest_clock_step(s.qs->qts, PROC_STEP_NS);
    g_assert_cmpuint(in_reg(&s, EDU2_Q0 + EDU2_REG_QHEAD), ==, 1);
    g_assert_cmphex(in_reg(&s, EDU2_REG_ISR), ==, EDU2_ISR_QDONE0);

    guest_free(&s.qs->alloc, ring);
    cleanup_vm(&s);
}

static void test_msix(void)
{
    Edu2Test s;
    uint64_t ring;

    setup_vm(&s, NULL);
    qpci_msix_enable(s.dev);
    g_assert_cmpuint(qpci_msix_table_size(s.dev), ==, 3);
    /*
     * The MSI-X table shares BAR0 with the registers and
     * qpci_msix_enable() re-mapped the BAR: reuse its mapping.
     */
    s.bar = s.dev->msix_table_bar;

    ring = guest_alloc(&s.qs->alloc, 8 * EDU2_DESC_SIZE);
    setup_queue(&s, EDU2_Q0, ring, 8);
    out_reg(&s, EDU2_REG_IEN_SET, EDU2_ISR_QDONE0 | EDU2_ISR_ERROR);

    /*
     * Vectors are left masked (the reset default), so delivery is
     * observable in the PBA.
     */
    write_desc(&s, ring, 0, 0, 0, 0, EDU2_OP_NOP, 0);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QTAIL, 1);
    qtest_clock_step(s.qs->qts, PROC_STEP_NS);

    g_assert_cmphex(in_reg(&s, EDU2_REG_ISR), ==, EDU2_ISR_QDONE0);
    g_assert_true(qpci_msix_pending(s.dev, 0));
    g_assert_false(qpci_msix_pending(s.dev, 2));

    out_reg(&s, EDU2_REG_IACK, EDU2_ISR_QDONE0);

    /* A halting error fires the error vector */
    write_desc(&s, ring, 1, 0, 0, 16, 0x7f, 0);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QTAIL, 2);
    qtest_clock_step(s.qs->qts, PROC_STEP_NS);
    g_assert_true(qpci_msix_pending(s.dev, 2));

    guest_free(&s.qs->alloc, ring);
    cleanup_vm(&s);
}

static void test_irq_throttle(void)
{
    Edu2Test s;
    uint64_t ring;

    setup_vm(&s, NULL);

    ring = guest_alloc(&s.qs->alloc, 8 * EDU2_DESC_SIZE);
    setup_queue(&s, EDU2_Q0, ring, 8);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QITR, 100); /* 100 us */
    out_reg(&s, EDU2_REG_IEN_SET, EDU2_ISR_QDONE0);

    /* First completion raises the cause immediately */
    write_desc(&s, ring, 0, 0, 0, 0, EDU2_OP_NOP, 0);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QTAIL, 1);
    qtest_clock_step(s.qs->qts, PROC_STEP_NS);
    g_assert_cmphex(in_reg(&s, EDU2_REG_ISR), ==, EDU2_ISR_QDONE0);
    out_reg(&s, EDU2_REG_IACK, EDU2_ISR_QDONE0);

    /*
     * Second completion lands inside the 100us throttle window: the
     * descriptor completes but the interrupt is deferred.
     */
    write_desc(&s, ring, 1, 0, 0, 0, EDU2_OP_NOP, 0);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QTAIL, 2);
    qtest_clock_step(s.qs->qts, PROC_STEP_NS);
    g_assert_cmpuint(in_reg(&s, EDU2_Q0 + EDU2_REG_QHEAD), ==, 2);
    g_assert_cmphex(in_reg(&s, EDU2_REG_ISR), ==, 0);

    /* ...and fires once the window expires */
    qtest_clock_step(s.qs->qts, 100 * 1000);
    g_assert_cmphex(in_reg(&s, EDU2_REG_ISR), ==, EDU2_ISR_QDONE0);

    guest_free(&s.qs->alloc, ring);
    cleanup_vm(&s);
}

static void test_second_queue(void)
{
    Edu2Test s;
    uint64_t ring0, ring1;
    uint32_t q1 = EDU2_Q0 + 0x40;

    setup_vm(&s, NULL);

    ring0 = guest_alloc(&s.qs->alloc, 8 * EDU2_DESC_SIZE);
    ring1 = guest_alloc(&s.qs->alloc, 8 * EDU2_DESC_SIZE);
    setup_queue(&s, EDU2_Q0, ring0, 8);
    setup_queue(&s, q1, ring1, 8);

    write_desc(&s, ring0, 0, 0, 0, 0, EDU2_OP_NOP, 0);
    write_desc(&s, ring1, 0, 0, 0, 0, EDU2_OP_NOP, 0);
    write_desc(&s, ring1, 1, 0, 0, 0, EDU2_OP_NOP, 0);
    out_reg(&s, EDU2_Q0 + EDU2_REG_QTAIL, 1);
    out_reg(&s, q1 + EDU2_REG_QTAIL, 2);
    qtest_clock_step(s.qs->qts, PROC_STEP_NS);

    g_assert_cmpuint(in_reg(&s, EDU2_Q0 + EDU2_REG_QHEAD), ==, 1);
    g_assert_cmpuint(in_reg(&s, q1 + EDU2_REG_QHEAD), ==, 2);
    g_assert_cmphex(in_reg(&s, EDU2_REG_ISR), ==,
                    EDU2_ISR_QDONE0 | EDU2_ISR_QDONE1);

    guest_free(&s.qs->alloc, ring0);
    guest_free(&s.qs->alloc, ring1);
    cleanup_vm(&s);
}

int main(int argc, char **argv)
{
    const char *arch = qtest_get_arch();

    g_test_init(&argc, &argv, NULL);

    if (strcmp(arch, "i386") != 0 && strcmp(arch, "x86_64") != 0) {
        g_test_message("edu2-test only runs on x86");
        return 0;
    }

    qtest_add_func("/edu2/regs", test_regs);
    qtest_add_func("/edu2/copy-fill-crc", test_copy_fill_crc);
    qtest_add_func("/edu2/error-recovery", test_error_recovery);
    qtest_add_func("/edu2/msix", test_msix);
    qtest_add_func("/edu2/irq-throttle", test_irq_throttle);
    qtest_add_func("/edu2/second-queue", test_second_queue);

    return g_test_run();
}
