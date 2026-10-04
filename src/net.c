#include "net.h"
#include <stdint.h>

/* Polling virtio-net and a small outbound TCP client for QEMU user networking.
 * No offloads: all Ethernet/IPv4/TCP headers and checksums are generated here. */
#define QSIZE 8
#define FRAME_MAX 1514
#define RX_SIZE 8192
#define SOCKETS 4
#define LOCAL_IP 0x0a00020fu
#define GATEWAY_IP 0x0a000202u
#define SYN 2
#define ACK 16
#define FIN 1
#define RST 4
#define PSH 8

typedef struct { uint64_t addr; uint32_t len; uint16_t flags, next; } Desc;
typedef struct { uint16_t flags, idx, ring[QSIZE], event; } Avail;
typedef struct { uint32_t id, len; } UsedEntry;
typedef struct { uint16_t flags, idx; UsedEntry ring[QSIZE]; uint16_t event; } Used;
typedef struct {
    uint8_t memory[8192] __attribute__((aligned(4096)));
    volatile Desc *desc; volatile Avail *avail; volatile Used *used;
    uint16_t consumed;
} Queue;
typedef struct {
    int state; /* 0 unused, 1 SYN sent, 2 established, 3 remote FIN, 4 failed */
    uint32_t remote_ip, seq, ack, acknowledged;
    uint16_t local_port, remote_port;
    uint8_t mac[6], received[RX_SIZE];
    size_t head, size;
    uint16_t window;
} Socket;
static volatile uint32_t *device;
static Queue rxq, txq;
static uint8_t rx_frames[QSIZE][FRAME_MAX + 12] __attribute__((aligned(16)));
static uint8_t tx_frame[FRAME_MAX + 12] __attribute__((aligned(16)));
static uint8_t our_mac[6];
static int ready, header_size, tx_pending;
static Socket sockets[SOCKETS];
static uint16_t next_port = 49152, ip_id;
static uint32_t arp_ip;
static uint8_t arp_mac[6];
static int arp_found;

static void barrier(void) { __asm__ volatile("dmb sy" ::: "memory"); }
static uint64_t now(void) { uint64_t v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }
static uint64_t frequency(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
static int expired(uint64_t start, unsigned seconds) { return now() - start >= frequency() * seconds; }
static uint32_t reg(unsigned offset) { return device[offset / 4]; }
static void putreg(unsigned offset, uint32_t value) { device[offset / 4] = value; }
static void copy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src; while (n--) *d++ = *s++;
}
static void zero(void *data, size_t n) { uint8_t *p = data; while (n--) *p++ = 0; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)be16(p) << 16) | be16(p + 2); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }
static uint32_t sum(const uint8_t *p, size_t n, uint32_t s) {
    while (n > 1) { s += be16(p); p += 2; n -= 2; }
    if (n) s += (uint32_t)*p << 8;
    return s;
}
static uint16_t checksum(uint32_t s) {
    while (s >> 16) s = (s & 65535u) + (s >> 16);
    return (uint16_t)~s;
}
static uint32_t pseudo(uint32_t src, uint32_t dst, size_t n) {
    return (src >> 16) + (src & 65535u) + (dst >> 16) + (dst & 65535u) + 6u + (uint32_t)n;
}
static void queue_address(unsigned offset, const volatile void *p) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    putreg(offset, (uint32_t)a); putreg(offset + 4, (uint32_t)(a >> 32));
}
static int setup_queue(Queue *q, int index, int modern) {
    zero(q->memory, sizeof(q->memory));
    q->desc = (volatile Desc *)q->memory;
    q->avail = (volatile Avail *)(q->memory + sizeof(Desc) * QSIZE);
    q->used = (volatile Used *)(q->memory + 4096); q->consumed = 0;
    putreg(0x030, (uint32_t)index);
    if (reg(0x034) < QSIZE) return -1;
    putreg(0x038, QSIZE);
    if (modern) {
        queue_address(0x080, q->desc); queue_address(0x090, q->avail); queue_address(0x0a0, q->used);
        putreg(0x044, 1);
    } else { putreg(0x03c, 4096); putreg(0x040, (uint32_t)((uintptr_t)q->memory >> 12)); }
    return 0;
}
static void publish(Queue *q, unsigned id, int index) {
    uint16_t n = q->avail->idx;
    q->avail->ring[n % QSIZE] = (uint16_t)id; barrier(); q->avail->idx = n + 1; barrier();
    putreg(0x050, (uint32_t)index);
}
int net_init(void) {
    if (ready) return 0;
    unsigned slot;
    for (slot = 0; slot < 32; slot++) {
        device = (volatile uint32_t *)(uintptr_t)(0x0a000000u + slot * 0x200u);
        if (reg(0x000) == 0x74726976u && reg(0x008) == 1) break;
    }
    if (slot == 32) return -1;
    unsigned version = reg(0x004);
    if (version != 1 && version != 2) return -1;
    int modern = version == 2;
    header_size = modern ? 12 : 10;
    putreg(0x070, 0); putreg(0x070, 1); putreg(0x070, 3);
    putreg(0x014, 0); uint32_t features = reg(0x010);
    /* Require the device-supplied MAC; accept only that feature and VERSION_1. */
    if (!(features & (1u << 5))) return -1;
    putreg(0x024, 0); putreg(0x020, 1u << 5);
    if (modern) {
        putreg(0x014, 1); if (!(reg(0x010) & 1)) return -1;
        putreg(0x024, 1); putreg(0x020, 1); putreg(0x070, 11);
        if (!(reg(0x070) & 8)) return -1;
    } else putreg(0x028, 4096);
    volatile uint8_t *config = (volatile uint8_t *)device + 0x100;
    for (int i = 0; i < 6; i++) our_mac[i] = config[i];
    if (setup_queue(&rxq, 0, modern) || setup_queue(&txq, 1, modern)) return -1;
    for (int i = 0; i < QSIZE; i++) {
        rxq.desc[i].addr = (uint64_t)(uintptr_t)rx_frames[i];
        rxq.desc[i].len = FRAME_MAX + (uint32_t)header_size;
        rxq.desc[i].flags = 2; rxq.desc[i].next = 0;
        rxq.avail->ring[i] = (uint16_t)i;
    }
    barrier(); rxq.avail->idx = QSIZE; barrier();
    putreg(0x070, modern ? 15 : 7); putreg(0x050, 0);
    ready = 1; return 0;
}
static void poll(void);
static int transmit(const uint8_t *frame, size_t n) {
    if (!ready || n > FRAME_MAX) return -1;
    uint64_t start = now();
    /* Never overwrite a DMA buffer after a timeout until its used entry arrives. */
    while (tx_pending && txq.used->idx == txq.consumed) if (expired(start, 1)) return -1;
    if (tx_pending) { barrier(); txq.consumed++; tx_pending = 0; }
    zero(tx_frame, (size_t)header_size); copy(tx_frame + header_size, frame, n);
    txq.desc[0].addr = (uint64_t)(uintptr_t)tx_frame;
    txq.desc[0].len = (uint32_t)(n + header_size); txq.desc[0].flags = 0; txq.desc[0].next = 0;
    publish(&txq, 0, 1); tx_pending = 1;
    start = now();
    while (txq.used->idx == txq.consumed) if (expired(start, 1)) return -1;
    barrier(); txq.consumed++; tx_pending = 0; return 0;
}
static void ethernet(uint8_t *frame, const uint8_t *dest, uint16_t type) {
    copy(frame, dest, 6); copy(frame + 6, our_mac, 6); put16(frame + 12, type);
}
static int arp_packet(const uint8_t *mac, uint32_t ip, int reply) {
    uint8_t frame[60]; zero(frame, sizeof(frame)); ethernet(frame, mac, 0x0806);
    uint8_t *a = frame + 14;
    put16(a, 1); put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; put16(a + 6, reply ? 2 : 1);
    copy(a + 8, our_mac, 6); put32(a + 14, LOCAL_IP);
    if (reply) copy(a + 18, mac, 6);
    put32(a + 24, ip); return transmit(frame, sizeof(frame));
}
static int route(uint32_t ip, uint8_t *mac) {
    arp_ip = (ip & 0xffffff00u) == (LOCAL_IP & 0xffffff00u) ? ip : GATEWAY_IP;
    arp_found = 0;
    const uint8_t broadcast[6] = {255,255,255,255,255,255};
    for (int attempt = 0; attempt < 3; attempt++) {
        if (arp_packet(broadcast, arp_ip, 0)) return -1;
        uint64_t start = now();
        while (!arp_found && !expired(start, 1)) poll();
        if (arp_found) { copy(mac, arp_mac, 6); return 0; }
    }
    return -1;
}
static int segment(Socket *s, unsigned flags, uint32_t seq, const void *data, size_t n) {
    uint8_t frame[14 + 20 + 24 + 1024];
    size_t tcp_header = flags & SYN ? 24 : 20;
    size_t total = 20 + tcp_header + n;
    zero(frame, 14 + total); ethernet(frame, s->mac, 0x0800);
    uint8_t *ip = frame + 14, *tcp = ip + 20;
    ip[0] = 0x45; put16(ip + 2, (uint16_t)total); put16(ip + 4, ip_id++);
    put16(ip + 6, 0x4000); ip[8] = 64; ip[9] = 6;
    put32(ip + 12, LOCAL_IP); put32(ip + 16, s->remote_ip);
    put16(ip + 10, checksum(sum(ip, 20, 0)));
    put16(tcp, s->local_port); put16(tcp + 2, s->remote_port);
    put32(tcp + 4, seq); put32(tcp + 8, s->ack);
    tcp[12] = (uint8_t)((tcp_header / 4) << 4); tcp[13] = (uint8_t)flags;
    put16(tcp + 14, (uint16_t)(RX_SIZE - s->size));
    if (flags & SYN) { tcp[20] = 2; tcp[21] = 4; put16(tcp + 22, 1024); }
    if (n) copy(tcp + tcp_header, data, n);
    put16(tcp + 16, checksum(sum(tcp, tcp_header + n, pseudo(LOCAL_IP, s->remote_ip, tcp_header + n))));
    return transmit(frame, 14 + total);
}
static void receive(const uint8_t *frame, size_t n) {
    if (n < 14) return;
    uint16_t type = be16(frame + 12);
    if (type == 0x0806) {
        if (n < 42) return;
        const uint8_t *a = frame + 14;
        if (be16(a) != 1 || be16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4 || be32(a + 24) != LOCAL_IP) return;
        if (be16(a + 6) == 2 && be32(a + 14) == arp_ip) { copy(arp_mac, a + 8, 6); arp_found = 1; }
        if (be16(a + 6) == 1) arp_packet(a + 8, be32(a + 14), 1);
        return;
    }
    if (type != 0x0800 || n < 54) return;
    const uint8_t *ip = frame + 14;
    size_t ih = (ip[0] & 15u) * 4, total = be16(ip + 2);
    if ((ip[0] >> 4) != 4 || ih < 20 || total < ih + 20 || total > n - 14 || ip[9] != 6 ||
        be32(ip + 16) != LOCAL_IP || (be16(ip + 6) & 0x3fffu) || checksum(sum(ip, ih, 0))) return;
    const uint8_t *tcp = ip + ih;
    size_t th = (tcp[12] >> 4) * 4, tcp_size = total - ih;
    if (th < 20 || th > tcp_size || checksum(sum(tcp, tcp_size, pseudo(be32(ip + 12), LOCAL_IP, tcp_size)))) return;
    for (int i = 0; i < SOCKETS; i++) {
        Socket *s = &sockets[i];
        if (!s->state || s->remote_ip != be32(ip + 12) || s->remote_port != be16(tcp) || s->local_port != be16(tcp + 2)) continue;
        uint32_t seq = be32(tcp + 4), ack = be32(tcp + 8); unsigned flags = tcp[13];
        if (flags & RST) { s->state = 4; return; }
        if (s->state == 1) {
            if ((flags & (SYN | ACK)) != (SYN | ACK) || ack != s->seq) return;
            s->ack = seq + 1; s->acknowledged = ack; s->window = be16(tcp + 14);
            s->state = 2; segment(s, ACK, s->seq, 0, 0); return;
        }
        if (s->state == 4) return;
        if ((flags & ACK) && (int32_t)(ack - s->acknowledged) >= 0 && (int32_t)(s->seq - ack) >= 0) {
            s->acknowledged = ack; s->window = be16(tcp + 14);
        }
        size_t payload = tcp_size - th;
        if (payload || (flags & (FIN | SYN))) {
            /* Only accept in-order segments that fit; duplicate/out-of-order
             * segments receive the current cumulative ACK for retransmission. */
            if (seq == s->ack && s->state == 2 && payload <= RX_SIZE - s->size) {
                for (size_t j = 0; j < payload; j++) s->received[(s->head + s->size + j) % RX_SIZE] = tcp[th + j];
                s->size += payload; s->ack += (uint32_t)payload;
                if (flags & FIN) { s->ack++; s->state = 3; }
            }
            segment(s, ACK, s->seq, 0, 0);
        }
        return;
    }
}
static void poll(void) {
    if (!ready) return;
    /* Bound each poll even if packets arrive continuously. */
    for (int budget = 0; budget < QSIZE && rxq.consumed != rxq.used->idx; budget++) {
        barrier(); UsedEntry entry;
        entry.id = rxq.used->ring[rxq.consumed % QSIZE].id;
        entry.len = rxq.used->ring[rxq.consumed % QSIZE].len;
        rxq.consumed++;
        if (entry.id >= QSIZE) continue;
        if (entry.len >= (unsigned)header_size && entry.len <= FRAME_MAX + (unsigned)header_size)
            receive(rx_frames[entry.id] + header_size, entry.len - (unsigned)header_size);
        publish(&rxq, entry.id, 0);
    }
    putreg(0x064, reg(0x060));
}
static int parse_ip(const char *str, uint32_t *out) {
    uint32_t ip = 0;
    for (int i = 0; i < 4; i++) {
        unsigned value = 0, digits = 0;
        while (*str >= '0' && *str <= '9') {
            value = value * 10 + (unsigned)(*str++ - '0');
            if (++digits > 3 || value > 255) return -1;
        }
        if (!digits || (i < 3 ? *str++ != '.' : *str != 0)) return -1;
        ip = (ip << 8) | value;
    }
    if (!ip || ip == 0xffffffffu || (ip >> 24) >= 224) return -1;
    *out = ip; return 0;
}
int tcp_connect(const char *ipv4, unsigned short port) {
    uint32_t ip;
    if (!ipv4 || !port || parse_ip(ipv4, &ip) || net_init()) return -1;
    int fd;
    for (fd = 0; fd < SOCKETS; fd++) if (!sockets[fd].state) break;
    if (fd == SOCKETS) return -1;
    Socket *s = &sockets[fd]; zero(s, sizeof(*s));
    s->remote_ip = ip; s->remote_port = port; s->local_port = next_port++;
    if (next_port < 49152) next_port = 49152;
    if (route(ip, s->mac)) return -1;
    uint32_t initial = (uint32_t)now(); s->seq = initial + 1; s->acknowledged = initial; s->state = 1;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (segment(s, SYN, initial, 0, 0)) break;
        uint64_t start = now(); while (s->state == 1 && !expired(start, 1)) poll();
        if (s->state == 2) return fd;
        if (s->state == 4) break;
    }
    s->state = 0; return -1;
}
long tcp_send(int fd, const void *data, size_t count) {
    if (fd < 0 || fd >= SOCKETS || sockets[fd].state != 2 || (!data && count)) return -1;
    Socket *s = &sockets[fd]; size_t sent = 0;
    while (sent < count && s->state == 2) {
        uint64_t start = now();
        while (!s->window && s->state == 2 && !expired(start, 3)) poll();
        if (!s->window || s->state != 2) break;
        size_t n = count - sent; if (n > 1024) n = 1024; if (n > s->window) n = s->window;
        uint32_t seq = s->seq; s->seq += (uint32_t)n;
        int done = 0;
        for (int attempt = 0; attempt < 3; attempt++) {
            if (segment(s, ACK | PSH, seq, (const uint8_t *)data + sent, n)) break;
            start = now();
            while ((int32_t)(s->acknowledged - s->seq) < 0 && s->state != 4 && !expired(start, 1)) poll();
            if (s->acknowledged == s->seq) { done = 1; break; }
            if (s->state == 4) break;
        }
        if (!done) { s->state = 4; break; }
        sent += n;
    }
    return sent ? (long)sent : count == 0 ? 0 : -1;
}
long tcp_recv(int fd, void *data, size_t count) {
    if (fd < 0 || fd >= SOCKETS || !sockets[fd].state || (!data && count)) return -1;
    Socket *s = &sockets[fd]; if (!count) return 0;
    uint64_t start = now();
    while (!s->size && s->state == 2 && !expired(start, 3)) poll();
    if (!s->size) return s->state == 3 ? 0 : -1;
    size_t n = count < s->size ? count : s->size;
    for (size_t i = 0; i < n; i++) ((uint8_t *)data)[i] = s->received[(s->head + i) % RX_SIZE];
    s->head = (s->head + n) % RX_SIZE; s->size -= n;
    if (s->state != 4) segment(s, ACK, s->seq, 0, 0); /* advertise reopened receive window */
    return (long)n;
}
int tcp_close(int fd) {
    if (fd < 0 || fd >= SOCKETS || !sockets[fd].state) return -1;
    Socket *s = &sockets[fd];
    if (s->state == 2 || s->state == 3) {
        uint32_t seq = s->seq++;
        for (int attempt = 0; attempt < 3; attempt++) {
            if (segment(s, FIN | ACK, seq, 0, 0)) break;
            uint64_t start = now();
            while (s->acknowledged != s->seq && s->state != 4 && !expired(start, 1)) poll();
            if (s->acknowledged == s->seq || s->state == 4) break;
        }
    }
    s->state = 0; return 0;
}
