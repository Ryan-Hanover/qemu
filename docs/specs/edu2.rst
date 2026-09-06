
EDU2 device
===========

..
   This document is licensed under the GPLv2 (or later).

EDU2 is the second-generation educational PCI device. Where the original
:doc:`edu` device is intentionally minimal, EDU2 is structured like real
modern PCIe hardware (network controllers, DMA offload engines, NVMe) so
that a non-trivial driver can be written against it. It exercises:

* PCI Express endpoint discovery, config space capabilities, and FLR
* MSI-X with multiple vectors (plus MSI and INTx fallback)
* Descriptor rings in guest memory with head/tail (doorbell) semantics
* Bus-master DMA with per-descriptor status write-back
* Interrupt cause/enable registers with write-1-to-clear acknowledgment
* Interrupt moderation (throttling)
* Hardware error reporting and queue recovery

The device is a *DMA/compute offload engine*: the driver builds
descriptors in memory describing COPY, FILL, or CRC32 operations, rings a
doorbell, and the device executes them asynchronously, writes completion
status back into each descriptor, and raises an interrupt.

Command line switches
---------------------

``-device edu2[,latency-ns=n]``
    ``latency-ns`` is the simulated processing delay in nanoseconds of
    virtual time between a doorbell write and descriptor execution
    (default 50000, i.e. 50 microseconds). Execution is always
    asynchronous relative to the doorbell write, so drivers must not
    assume completion on doorbell return even with ``latency-ns=0``.

PCI specs
---------

PCI ID:
    ``1234:11e9``, revision 1

PCI Region 0 (BAR0):
    64-bit non-prefetchable memory BAR, 16 KiB:

    ============== =========================================
    0x0000--0x0fff device registers (see below)
    0x1000         MSI-X vector table
    0x2000         MSI-X PBA (pending bit array)
    ============== =========================================

Capabilities:
    Power Management, MSI (1 vector), MSI-X (3 vectors), and -- when the
    device is plugged into a PCI Express bus (e.g. the q35 machine) --
    a PCI Express endpoint capability with Function Level Reset (FLR).

The device is a bus master: the driver must set the Bus Master Enable
bit in the PCI command register before ringing any doorbell, or DMA
(including descriptor fetch) will fail.

Register map (BAR0)
-------------------

All registers are 32 bits wide and must be accessed with aligned 32-bit
reads/writes. 64-bit values are split into LO/HI register pairs.
Reserved/unknown registers read as 0; writes to them are ignored.

Global registers
^^^^^^^^^^^^^^^^

0x000 ID (RO)
    ``0xED2MMmm.`` -- magic ``0xED2`` in bits 31:12 is constant
    (the register reads ``0xED200100``): bits 15:8 major version (0x01),
    bits 7:0 minor version (0x00). Drivers should check bits 31:16 ==
    ``0xED20``.

0x004 CTRL (WO)
    bit 0
        RESET: writing 1 performs a full soft reset of the device
        function: all queues are disabled and cleared, ISR/IEN are
        cleared, SCRATCH is cleared. PCI config space (including MSI-X
        state) is not affected. Reads as 0.

0x008 STATUS (RO)
    bit 0
        READY: always 1 once the function is out of reset.

0x00C SCRATCH (RW)
    Read/write scratch register with no side effects (liveness check).

0x010 NUM_QUEUES (RO)
    Number of DMA queues implemented (2).

0x014 MAX_XFER (RO)
    Maximum transfer length in bytes per descriptor (1 MiB).

Interrupt registers
^^^^^^^^^^^^^^^^^^^

The device maintains an interrupt cause register (ISR) and an interrupt
enable register (IEN). A cause bit is *pending* when set in ISR. The
mapping of cause bits:

    ======= ==================================================
    bit 0   queue 0 completion
    bit 1   queue 1 completion
    bit 16  error (any queue halt, see `Error handling`_)
    ======= ==================================================

0x020 ISR (RO)
    Pending interrupt causes. Reading does **not** clear.

0x024 IACK (WO)
    Write 1 to clear (W1C): each set bit written clears the
    corresponding ISR bit. Interrupt handlers must acknowledge causes
    here.

0x028 IEN (RO)
    Currently enabled interrupt causes.

0x02C IEN_SET (WO)
    Each set bit written enables that cause. If a newly enabled cause is
    already pending in ISR, a message interrupt fires immediately.

0x030 IEN_CLR (WO)
    Each set bit written disables that cause.

Interrupt delivery:

* **MSI-X enabled**: each cause fires its own vector -- queue *n*
  completion fires vector *n*, error fires vector 2. Message interrupts
  are edge-like: one message is sent when a cause becomes pending while
  enabled (or becomes enabled while pending). Acknowledge via IACK
  before re-enabling to avoid missing subsequent events.
* **MSI enabled** (MSI-X off): all causes fire the single MSI vector.
* **Neither**: INTx is asserted level-triggered while
  ``(ISR & IEN) != 0`` and deasserted when all enabled causes are
  acknowledged.

Per-queue registers
^^^^^^^^^^^^^^^^^^^

Queue *n* (*n* = 0, 1) occupies a 0x40-byte block at
``0x100 + n * 0x40``. Offsets below are relative to the block start.

+0x00 QCTRL (RW)
    bit 0
        ENABLE: setting 0->1 validates the ring configuration
        (QBASE/QSIZE) and starts the queue with HEAD = TAIL = 0. If the
        configuration is invalid (base not 32-byte aligned, size not a
        power of two in [2, 32768]) the queue instead halts with error
        code 0x05 (BAD_CONFIG). Clearing the bit stops the queue;
        in-flight work already accepted may still complete before the
        stop takes effect, but no new doorbells are accepted.
    bit 1
        QRESET: writing 1 resets the queue: disables it, clears
        HALTED/error state, HEAD, TAIL and the statistics counters.
        QBASE, QSIZE and QITR are preserved. Reads as 0.

+0x04 QSTATUS (RO)
    bit 0
        ACTIVE: queue is enabled and not halted.
    bit 1
        HALTED: queue stopped due to an error.
    bits 15:8
        Error code of the halt cause (see `Error handling`_).

+0x08 QBASE_LO (RW), +0x0C QBASE_HI (RW)
    Physical (DMA) address of the descriptor ring. Must be 32-byte
    aligned. Writes are ignored while the queue is enabled.

+0x10 QSIZE (RW)
    Ring size in descriptors. Must be a power of two, 2 to 32768.
    Writes are ignored while the queue is enabled.

+0x14 QHEAD (RO)
    Consumer index: the next descriptor the device will process. The
    device increments HEAD (mod QSIZE) after each completed descriptor.

+0x18 QTAIL (RW, doorbell)
    Producer index: writing places the first index the device must
    *not* process (one past the last valid descriptor) and rings the
    doorbell. Must be < QSIZE; out-of-range writes are ignored. Writes
    while the queue is disabled or halted are ignored.

+0x1C QITR (RW)
    Interrupt throttle interval in microseconds (0 to 10000; larger
    values are clamped). Enforces a minimum gap between two completion
    interrupts of this queue. Completions inside the throttle window
    are coalesced into a single deferred interrupt at the end of the
    window. 0 disables throttling.

+0x20 QDONE_COUNT (RO)
    Cumulative successfully completed descriptors (wraps at 2^32).

+0x24 QERR_COUNT (RO)
    Cumulative queue halts due to errors.

Descriptor ring protocol
------------------------

A ring is an array of QSIZE 32-byte descriptors in physically
contiguous, DMA-able memory. The driver is the producer (advances
TAIL), the device is the consumer (advances HEAD).

* Ring **empty**: ``HEAD == TAIL``.
* Ring **full**: ``(TAIL + 1) % QSIZE == HEAD`` -- the driver must
  always leave one slot unused.

To submit work, the driver fills descriptors starting at its current
producer index, then writes the new producer index to QTAIL. The device
fetches and executes descriptors from HEAD to TAIL in order, writes the
``result`` and ``status`` fields back into each descriptor (the first
24 bytes are never written by the device), then advances HEAD. Software
can detect completion either by interrupt, by polling QHEAD, or by
polling the DD bit in descriptor status.

The driver must clear the ``status`` field (at least the DD bit) of a
descriptor before submitting it.

Descriptor format
^^^^^^^^^^^^^^^^^

All fields little-endian:

    ====== ==== ======== =================================================
    offset size name     description
    ====== ==== ======== =================================================
    0      8    src      source DMA address (COPY, CRC32); 32-bit fill
                         pattern (FILL); ignored (NOP)
    8      8    dst      destination DMA address (COPY, FILL); initial
                         CRC seed in low 32 bits (CRC32); ignored (NOP)
    16     4    len      transfer length in bytes, 1..MAX_XFER
                         (ignored for NOP)
    20     1    opcode   see below
    21     1    flags    bit 0: NO_IRQ -- completion of this descriptor
                         does not contribute to the completion interrupt
    22     2    reserved must be zero
    24     4    result   written back by device: CRC32 value for CRC32,
                         0 otherwise
    28     4    status   written back by device, see below
    ====== ==== ======== =================================================

Opcodes:

    ==== ===== ====================================================
    0x00 NOP   completes without any data transfer
    0x01 COPY  copy ``len`` bytes from ``src`` to ``dst``
    0x02 FILL  fill ``len`` bytes at ``dst`` with the 32-bit pattern
               in the low 4 bytes of ``src`` (repeated; a partial
               pattern is written if ``len`` is not a multiple of 4)
    0x03 CRC32 compute CRC32 (IEEE 802.3, same as zlib ``crc32()``)
               over ``len`` bytes at ``src``, seeded with the low 32
               bits of ``dst``; the CRC is written to ``result``.
               Chain multi-buffer checksums by passing the previous
               descriptor's result as the next seed (first seed 0).
    ==== ===== ====================================================

Status field:

    bit 0
        DD (descriptor done): set when the device finished the
        descriptor (successfully or not).
    bit 1
        ERR: descriptor failed; the queue is halted.
    bits 15:8
        error code

Error handling
--------------

Error codes:

    ==== ============ ===============================================
    0x01 BAD_OPCODE   unknown opcode
    0x02 BAD_LEN      len is 0 or exceeds MAX_XFER
    0x03 DMA_READ     source/descriptor read failed (bad address)
    0x04 DMA_WRITE    destination/write-back write failed
    0x05 BAD_CONFIG   queue enabled with invalid QBASE/QSIZE
    ==== ============ ===============================================

When a descriptor fails, the device writes back DD|ERR and the error
code, leaves QHEAD pointing at the failing descriptor, halts the queue
(ACTIVE=0, HALTED=1, error code in QSTATUS), and raises the ERROR
interrupt cause (bit 16). QERR_COUNT is incremented. Descriptors after
the failing one are not processed.

To recover, the driver must write QRESET, reprogram/repopulate the ring
as needed, and re-enable the queue.

Reset
-----

Three reset levels, from largest to smallest scope:

* **Bus reset / FLR**: resets PCI config space, MSI/MSI-X state and all
  device registers. FLR is available through the PCIe capability when
  on a PCIe bus.
* **CTRL.RESET**: resets all device registers and queues, but not PCI
  config space or MSI/MSI-X state.
* **QCTRL.QRESET**: resets one queue's state, preserving its ring
  configuration registers.

Running a development VM
------------------------

Building this QEMU
^^^^^^^^^^^^^^^^^^

From the top of the source tree (only needs to be done once; rebuilds
after source changes are just ``ninja -C build``)::

  ./configure --target-list=x86_64-softmmu
  ninja -C build

The system emulator is then ``build/qemu-system-x86_64``. A quick sanity
check that the device is compiled in::

  ./build/qemu-system-x86_64 -device edu2,help

The device model's functional tests can be run with::

  cd build && QTEST_QEMU_BINARY=./qemu-system-x86_64 ./tests/qtest/edu2-test

Launching a Linux guest
^^^^^^^^^^^^^^^^^^^^^^^

Any x86-64 Linux guest works; use the q35 machine so the device is
enumerated as a PCI Express endpoint. The easiest disposable guest is a
"nocloud" cloud image (passwordless root login on the serial console,
no cloud-init setup needed)::

  wget https://cloud.debian.org/images/cloud/trixie/latest/debian-13-nocloud-amd64.qcow2

  # Overlay so the pristine image is never modified
  qemu-img create -f qcow2 -b debian-13-nocloud-amd64.qcow2 -F qcow2 dev.qcow2 20G

  ./build/qemu-system-x86_64 \
      -machine q35,accel=kvm -cpu host -smp 4 -m 4G \
      -drive file=dev.qcow2,if=virtio \
      -device edu2 \
      -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22 \
      -virtfs local,path=$HOME/edu2-driver,mount_tag=src,security_model=mapped-xattr \
      -nographic

Log in as ``root`` on the serial console (``Ctrl-a x`` exits QEMU,
``Ctrl-a c`` toggles the monitor). Then, in the guest::

  lspci -vvv -d 1234:11e9        # inspect BAR0, PM/MSI/MSI-X/Express caps
  apt install build-essential linux-headers-$(uname -r)
  mount -t 9p -o trans=virtio src /mnt   # driver source shared from host

The 9p share means you can edit driver source on the host and rebuild
in the guest without copying files. Inside the guest a plain
out-of-tree module build works::

  cd /mnt && make -C /lib/modules/$(uname -r)/build M=$PWD modules
  insmod edu2.ko && dmesg | tail

Faster iteration with a custom kernel
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

If you are building your own kernel anyway, skip the disk image boot
path and boot the kernel directly -- the edit/compile/boot loop drops
to a few seconds::

  ./build/qemu-system-x86_64 \
      -machine q35,accel=kvm -cpu host -m 2G \
      -kernel /path/to/linux/arch/x86/boot/bzImage \
      -drive file=dev.qcow2,if=virtio \
      -append "root=/dev/vda1 rw console=ttyS0" \
      -device edu2 -nographic

Debugging tips
^^^^^^^^^^^^^^

* ``-d guest_errors``: the device reports driver programming mistakes
  (invalid ring configuration, out-of-range doorbells, doorbell while
  halted, bad register accesses) through this log -- run with it always
  enabled while developing.
* ``-device edu2,latency-ns=100000000`` (100 ms) exaggerates the
  asynchrony between doorbell and completion and flushes out drivers
  that accidentally rely on quick completion.
* ``-snapshot`` makes all disk writes throwaway for risky experiments.
* To debug the guest kernel with gdb, add ``-s -S`` (gdbserver on port
  1234, start paused), then on the host::

    gdb /path/to/linux/vmlinux -ex 'target remote :1234'

* In the QEMU monitor (``Ctrl-a c`` with ``-nographic``), ``info pci``
  shows BAR mappings and IRQ routing for the device.

Suggested driver exercises
--------------------------

The device is designed so a driver can grow in stages:

1. Probe: match ``1234:11e9``, enable the device, map BAR0, check ID.
2. Polled operation: allocate a coherent ring buffer, submit a FILL or
   COPY, poll QHEAD/DD.
3. Interrupt-driven operation with INTx, then MSI, then MSI-X with the
   per-queue vector mapping; acknowledge via IACK.
4. Multi-queue submission with a proper ring producer/consumer
   implementation and ring-full backpressure.
5. Interrupt moderation: set QITR and batch completions (NAPI-style).
6. Error recovery: submit a bad descriptor, handle the ERROR vector,
   reset and resume the queue.
7. CRC32 offload with chained seeds, verified against software crc32.
8. Suspend/resume and FLR handling.
