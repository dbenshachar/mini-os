#include "mmio.h"

#include <stdint.h>
#include <stddef.h>

#define UART_BASE 0x09000000
#define UART_DR (*((volatile uint32_t *)(UART_BASE + 0x000)))
#define UART_FR (*((volatile uint32_t *)(UART_BASE + 0x018)))

#define UART_FR_TXFF (1 << 5)
#define UART_FR_RXFE (1 << 4)

#define VIRTIO_BASE 0x0a000000
#define VIRTIO_MMIO_STRIDE 0x200
#define VIRTIO_MMIO_SLOTS 32
#define VIRTIO_MAGIC_VALUE 0x000
#define VIRTIO_VERSION 0x004
#define VIRTIO_DEVICE_ID 0x008
#define VIRTIO_DEVICE_FEATURES 0x010
#define VIRTIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_DRIVER_FEATURES 0x020
#define VIRTIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_QUEUE_SEL 0x030
#define VIRTIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_QUEUE_NUM 0x038
#define VIRTIO_QUEUE_ALIGN 0x03c
#define VIRTIO_QUEUE_PFN 0x040
#define VIRTIO_QUEUE_READY 0x044
#define VIRTIO_QUEUE_NOTIFY 0x050
#define VIRTIO_INTERRUPT_STATUS 0x060
#define VIRTIO_INTERRUPT_ACK 0x064
#define VIRTIO_STATUS 0x070
#define VIRTIO_QUEUE_DESC_LOW 0x080
#define VIRTIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_QUEUE_AVAIL_LOW 0x090
#define VIRTIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_QUEUE_USED_LOW 0x0a0
#define VIRTIO_QUEUE_USED_HIGH 0x0a4

#define VIRTIO_MAGIC_EXPECTED 0x74726976
#define VIRTIO_VERSION_LEGACY 1
#define VIRTIO_VERSION_MODERN 2
#define VIRTIO_DEVICE_BLOCK 2

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED 128

#define VIRTIO_F_VERSION_1_BIT 32

#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1

#define VIRTQ_SIZE 8
#define SECTOR_SIZE 512
#define PAGE_SIZE 4096
#define DISK_WAIT_LIMIT 1000000u

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} VirtqDesc;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_SIZE];
    uint16_t used_event;
} VirtqAvail;

typedef struct {
    uint32_t id;
    uint32_t len;
} VirtqUsedElem;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    VirtqUsedElem ring[VIRTQ_SIZE];
    uint16_t avail_event;
} VirtqUsed;

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} VirtioBlkReq;

static uint8_t vq_mem[PAGE_SIZE * 2] __attribute__((aligned(PAGE_SIZE)));
static volatile VirtqDesc *vq_desc;
static volatile VirtqAvail *vq_avail;
static volatile VirtqUsed *vq_used;
static volatile VirtioBlkReq blk_req __attribute__((aligned(16)));
static volatile uint8_t blk_status __attribute__((aligned(4)));
static uint16_t driver_avail_idx;
static uint16_t driver_used_idx;
static int disk_ready;
static int virtio_version;

static volatile uint32_t *virtio = (volatile uint32_t *)VIRTIO_BASE;

/* Reads a 32-bit register from the currently selected virtio-MMIO device. */
static uint32_t mmio_read(uint32_t offset) {
    return virtio[offset / 4];
}

/* Writes a 32-bit register on the currently selected virtio-MMIO device. */
static void mmio_write(uint32_t offset, uint32_t value) {
    virtio[offset / 4] = value;
}

/* Forces memory operations around virtqueue updates to become visible in order. */
static void memory_barrier(void) {
    __asm__ volatile("dmb sy" ::: "memory");
}

/* Returns the low 32 bits of a pointer address for modern virtio queue registers. */
static uint32_t addr_low(const volatile void *ptr) {
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    return (uint32_t)(addr & 0xffffffffu);
}

/* Returns the high 32 bits of a pointer address for modern virtio queue registers. */
static uint32_t addr_high(const volatile void *ptr) {
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    return (uint32_t)(addr >> 32);
}

/* Rounds a value up to the next boundary required by a virtqueue layout. */
static uint32_t align_up(uint32_t value, uint32_t align) {
    return (value + align - 1) & ~(align - 1);
}

/* Maps the shared virtqueue memory into descriptor, available, and used ring pointers. */
static void setup_queue_pointers(void) {
    uint32_t desc_bytes = sizeof(VirtqDesc) * VIRTQ_SIZE;
    uint32_t avail_bytes = 6 + (2 * VIRTQ_SIZE);
    uint32_t used_offset = align_up(desc_bytes + avail_bytes, PAGE_SIZE);

    vq_desc = (volatile VirtqDesc *)vq_mem;
    vq_avail = (volatile VirtqAvail *)(vq_mem + desc_bytes);
    vq_used = (volatile VirtqUsed *)(vq_mem + used_offset);
}

/* Writes one character to the UART, waiting until the transmit FIFO has room. */
void uart_put(char c) {
    while (UART_FR & UART_FR_TXFF);
    UART_DR = c;
}

/* Reads one character from the UART, waiting until the receive FIFO has data. */
char uart_get() {
    while (UART_FR & UART_FR_RXFE);
    return UART_DR & 0xFF;
}

/*
 * Initializes the virtio block MMIO device and prepares queue zero for requests.
 * QEMU exposes several virtio-MMIO slots, so this scans for the block device before
 * negotiating either the modern or legacy register layout. It then publishes the
 * queue addresses, clears ring indexes, acknowledges interrupts, and marks the
 * driver ready once the device can accept sector transfers.
 */
int disk_mmio_init() {
    uint32_t queue_max;
    uint32_t version;
    uint32_t slot;

    for (slot = 0; slot < VIRTIO_MMIO_SLOTS; slot++) {
        virtio = (volatile uint32_t *)(uintptr_t)(VIRTIO_BASE + (slot * VIRTIO_MMIO_STRIDE));
        if (mmio_read(VIRTIO_MAGIC_VALUE) == VIRTIO_MAGIC_EXPECTED &&
            mmio_read(VIRTIO_DEVICE_ID) == VIRTIO_DEVICE_BLOCK) {
            break;
        }
    }

    if (slot == VIRTIO_MMIO_SLOTS) return -12;
    version = mmio_read(VIRTIO_VERSION);
    if (version != VIRTIO_VERSION_LEGACY && version != VIRTIO_VERSION_MODERN) return -11;
    virtio_version = (int)version;
    setup_queue_pointers();

    mmio_write(VIRTIO_STATUS, 0);
    mmio_write(VIRTIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write(VIRTIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    if (virtio_version == VIRTIO_VERSION_MODERN) {
        mmio_write(VIRTIO_DEVICE_FEATURES_SEL, 1);
        if ((mmio_read(VIRTIO_DEVICE_FEATURES) & (1u << (VIRTIO_F_VERSION_1_BIT - 32))) == 0) {
            mmio_write(VIRTIO_STATUS, mmio_read(VIRTIO_STATUS) | VIRTIO_STATUS_FAILED);
            return -13;
        }

        mmio_write(VIRTIO_DRIVER_FEATURES_SEL, 0);
        mmio_write(VIRTIO_DRIVER_FEATURES, 0);
        mmio_write(VIRTIO_DRIVER_FEATURES_SEL, 1);
        mmio_write(VIRTIO_DRIVER_FEATURES, 1u << (VIRTIO_F_VERSION_1_BIT - 32));
        mmio_write(VIRTIO_STATUS, mmio_read(VIRTIO_STATUS) | VIRTIO_STATUS_FEATURES_OK);
        if ((mmio_read(VIRTIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) {
            mmio_write(VIRTIO_STATUS, mmio_read(VIRTIO_STATUS) | VIRTIO_STATUS_FAILED);
            return -14;
        }
    } else {
        mmio_write(VIRTIO_GUEST_PAGE_SIZE, PAGE_SIZE);
        mmio_write(VIRTIO_DRIVER_FEATURES, 0);
    }

    mmio_write(VIRTIO_QUEUE_SEL, 0);
    queue_max = mmio_read(VIRTIO_QUEUE_NUM_MAX);
    if (queue_max < VIRTQ_SIZE || queue_max == 0) {
        mmio_write(VIRTIO_STATUS, mmio_read(VIRTIO_STATUS) | VIRTIO_STATUS_FAILED);
        return -15;
    }

    mmio_write(VIRTIO_QUEUE_NUM, VIRTQ_SIZE);
    if (virtio_version == VIRTIO_VERSION_MODERN) {
        mmio_write(VIRTIO_QUEUE_DESC_LOW, addr_low(vq_desc));
        mmio_write(VIRTIO_QUEUE_DESC_HIGH, addr_high(vq_desc));
        mmio_write(VIRTIO_QUEUE_AVAIL_LOW, addr_low(vq_avail));
        mmio_write(VIRTIO_QUEUE_AVAIL_HIGH, addr_high(vq_avail));
        mmio_write(VIRTIO_QUEUE_USED_LOW, addr_low(vq_used));
        mmio_write(VIRTIO_QUEUE_USED_HIGH, addr_high(vq_used));
        mmio_write(VIRTIO_QUEUE_READY, 1);
    } else {
        mmio_write(VIRTIO_QUEUE_ALIGN, PAGE_SIZE);
        mmio_write(VIRTIO_QUEUE_PFN, (uint32_t)((uintptr_t)vq_mem >> 12));
    }

    vq_avail->flags = 0;
    vq_avail->idx = 0;
    vq_used->flags = 0;
    vq_used->idx = 0;
    driver_avail_idx = 0;
    driver_used_idx = 0;

    mmio_write(VIRTIO_INTERRUPT_ACK, mmio_read(VIRTIO_INTERRUPT_STATUS));
    mmio_write(VIRTIO_STATUS, mmio_read(VIRTIO_STATUS) | VIRTIO_STATUS_DRIVER_OK);
    disk_ready = 1;
    return 0;
}

/*
 * Performs one blocking 512-byte virtio block request against the selected LBA.
 * The request is submitted as a three-descriptor chain: header, data buffer, and
 * status byte. After publishing the head descriptor to the available ring it waits
 * for the used index to advance, with a simple timeout to avoid hanging forever if
 * the device never completes the request.
 */
static int disk_transfer(uint32_t lba, void *buffer, int write) {
    uint16_t ring_slot;
    uint32_t wait;

    if (!disk_ready) return -1;
    if (buffer == NULL) return -1;

    blk_req.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    blk_req.reserved = 0;
    blk_req.sector = lba;
    blk_status = 0xff;

    vq_desc[0].addr = (uint64_t)(uintptr_t)&blk_req;
    vq_desc[0].len = sizeof(blk_req);
    vq_desc[0].flags = VIRTQ_DESC_F_NEXT;
    vq_desc[0].next = 1;

    vq_desc[1].addr = (uint64_t)(uintptr_t)buffer;
    vq_desc[1].len = SECTOR_SIZE;
    vq_desc[1].flags = VIRTQ_DESC_F_NEXT | (write ? 0 : VIRTQ_DESC_F_WRITE);
    vq_desc[1].next = 2;

    vq_desc[2].addr = (uint64_t)(uintptr_t)&blk_status;
    vq_desc[2].len = sizeof(blk_status);
    vq_desc[2].flags = VIRTQ_DESC_F_WRITE;
    vq_desc[2].next = 0;

    ring_slot = driver_avail_idx % VIRTQ_SIZE;
    vq_avail->ring[ring_slot] = 0;
    memory_barrier();
    driver_avail_idx++;
    vq_avail->idx = driver_avail_idx;
    memory_barrier();

    mmio_write(VIRTIO_QUEUE_NOTIFY, 0);

    wait = 0;
    while (vq_used->idx == driver_used_idx) {
        if (++wait == DISK_WAIT_LIMIT) return -2;
    }
    memory_barrier();
    driver_used_idx++;
    mmio_write(VIRTIO_INTERRUPT_ACK, mmio_read(VIRTIO_INTERRUPT_STATUS));

    return blk_status == 0 ? 0 : -3;
}

/* Reads one 512-byte sector from the block device into the caller's buffer. */
int disk_read_sector(uint32_t lba, void *buffer) {
    return disk_transfer(lba, buffer, 0);
}

/* Writes one 512-byte sector from the caller's buffer to the block device. */
int disk_write_sector(uint32_t lba, const void *buffer) {
    return disk_transfer(lba, (void *)buffer, 1);
}
