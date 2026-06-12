/*
 * Microsemi Switchtec NTB device over shared memory
 *
 * The BAR0 layout follows the Switchtec Global Address Space (GAS).  This
 * first implementation models the NTB information and DBMSG register blocks.
 * The shared state deliberately keeps per-partition control storage separate
 * so the NTB memory-window control blocks can be added without changing the
 * DBMSG transport.
 *
 * Based on hw/misc/ivshmem-pci.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "system/kvm.h"
#include "migration/blocker.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "qemu/event_notifier.h"
#include "qemu/module.h"
#include "qom/object_interfaces.h"
#include "chardev/char-fe.h"
#include "system/hostmem.h"
#include "qapi/visitor.h"

#include "hw/misc/ivshmem.h"
#include "qom/object.h"

#define PCI_VENDOR_ID_SWITCHTEC 0x11f8
#define PCI_DEVICE_ID_SWITCHTEC 0x8531

#define IVSHMEM_MAX_PEERS UINT16_MAX
#define IVSHMEM_IOEVENTFD   0
#define IVSHMEM_MSI     1

#define SWITCHTEC_GAS_SIZE              (2 * MiB)
#define SWITCHTEC_GAS_SYS_INFO_OFFSET   0x2000
#define SWITCHTEC_SYS_INFO_GEN3_PART_ID 0x18
#define SWITCHTEC_GAS_PART_CFG_OFFSET   0x4000
#define SWITCHTEC_GAS_NTB_OFFSET        0x10000
#define SWITCHTEC_NTB_INFO_OFFSET       0x0000
#define SWITCHTEC_NTB_CTRL_OFFSET       0x4000
#define SWITCHTEC_NTB_DBMSG_OFFSET      0x64000
#define SWITCHTEC_DBMSG_SIZE            0x4000
#define SWITCHTEC_DBMSG_ODB             0x1000
#define SWITCHTEC_DBMSG_ODB_MASK        0x1008
#define SWITCHTEC_DBMSG_IDB             0x1010
#define SWITCHTEC_DBMSG_IDB_MASK        0x1018
#define SWITCHTEC_DBMSG_IDB_VEC_MAP     0x1020
#define SWITCHTEC_DBMSG_MSG_MAP         0x1060
#define SWITCHTEC_DBMSG_OMSG            0x1068
#define SWITCHTEC_DBMSG_IMSG            0x1088
#define SWITCHTEC_DBMSG_MSIX_TABLE      0x2000
#define SWITCHTEC_DBMSG_PBA             0x3000
#define SWITCHTEC_PARTITIONS            2
#define SWITCHTEC_MESSAGES              4
#define SWITCHTEC_DBMSG_IMSG_STATUS     (1ULL << 32)
#define SWITCHTEC_DBMSG_IMSG_MASK       (1ULL << 40)
#define SWITCHTEC_SHM_MAGIC             0x53574e54

#define IVSHMEM_DEBUG 0
#define IVSHMEM_DPRINTF(fmt, ...)                       \
    do {                                                \
        if (IVSHMEM_DEBUG) {                            \
            printf("IVSHMEM: " fmt, ## __VA_ARGS__);    \
        }                                               \
    } while (0)

#define TYPE_SWITCHTEC_NTB_IVSHMEM "switchtec-ntb-ivshmem"
typedef struct SwitchtecNtbState SwitchtecNtbState;
DECLARE_INSTANCE_CHECKER(SwitchtecNtbState, SWITCHTEC_NTB_IVSHMEM,
                         TYPE_SWITCHTEC_NTB_IVSHMEM)

typedef struct Peer {
    int nb_eventfds;
    EventNotifier *eventfds;
} Peer;

typedef struct MSIVector {
    PCIDevice *pdev;
    int virq;
    bool unmasked;
} MSIVector;

typedef struct SwitchtecDbmsgShared {
    uint64_t odb;
    uint64_t odb_mask;
    uint64_t idb;
    uint64_t idb_mask;
    uint8_t idb_vec_map[64];
    uint32_t msg_map;
    struct {
        uint32_t msg;
        uint32_t status;
    } omsg[SWITCHTEC_MESSAGES];
    struct {
        uint32_t msg;
        uint8_t status;
        uint8_t mask;
        uint8_t src;
        uint8_t reserved;
    } imsg[SWITCHTEC_MESSAGES];
} SwitchtecDbmsgShared;

typedef struct SwitchtecPartitionShared {
    /*
     * Reserved for the per-partition NTB control/MW state implemented by a
     * later phase.  DBMSG is independent of this storage.
     */
    uint8_t ctrl[0x4000];
    SwitchtecDbmsgShared dbmsg;
} SwitchtecPartitionShared;

typedef struct SwitchtecShared {
    uint32_t magic;
    uint32_t version;
    int32_t vm_id[SWITCHTEC_PARTITIONS];
    SwitchtecPartitionShared partition[SWITCHTEC_PARTITIONS];
} SwitchtecShared;

struct SwitchtecNtbState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    uint32_t features;

    /* exactly one of these two may be set */
    HostMemoryBackend *hostmem; /* with interrupts */
    CharFrontend server_chr; /* without interrupts */

    int vm_id;
    uint32_t partition;
    SwitchtecShared *shared;
    SwitchtecDbmsgShared *dbmsg;

    /* BARs */
    MemoryRegion switchtec_gas;  /* BAR 0 (registers) */
    MemoryRegion *ivshmem_bar2; /* BAR 2 (shared memory) */
    MemoryRegion server_bar2;   /* used with server_chr */

    /* interrupt support */
    Peer *peers;
    int nb_peers;               /* space in @peers[] */
    uint32_t vectors;
    MSIVector *msi_vectors;
    uint64_t msg_buf;           /* buffer for receiving server messages */
    int msg_buffered_bytes;     /* #bytes in @msg_buf */

    /* migration stuff */
    OnOffAuto master;
    Error *migration_blocker;
};

static inline uint32_t ivshmem_has_feature(SwitchtecNtbState *ivs,
                                           unsigned int feature)
{
    return ivs->features & (1 << feature);
}

static inline bool ivshmem_is_master(SwitchtecNtbState *s)
{
    assert(s->master != ON_OFF_AUTO_AUTO);
    return s->master == ON_OFF_AUTO_ON;
}

static uint64_t switchtec_load(const void *ptr, unsigned size)
{
    switch (size) {
    case 1:
        return *(const uint8_t *)ptr;
    case 2:
        return lduw_le_p(ptr);
    case 4:
        return ldl_le_p(ptr);
    case 8:
        return ldq_le_p(ptr);
    default:
        g_assert_not_reached();
    }
}

static void switchtec_store(void *ptr, uint64_t val, unsigned size)
{
    switch (size) {
    case 1:
        *(uint8_t *)ptr = val;
        break;
    case 2:
        stw_le_p(ptr, val);
        break;
    case 4:
        stl_le_p(ptr, val);
        break;
    case 8:
        stq_le_p(ptr, val);
        break;
    default:
        g_assert_not_reached();
    }
}

static int switchtec_peer_vm_id(SwitchtecNtbState *s)
{
    int peer_partition = s->partition ^ 1;

    return qatomic_read(&s->shared->vm_id[peer_partition]);
}

static void switchtec_notify_peer(SwitchtecNtbState *s, unsigned vector)
{
    int peer_id = switchtec_peer_vm_id(s);

    if (peer_id < 0 || peer_id >= s->nb_peers ||
        vector >= s->peers[peer_id].nb_eventfds) {
        return;
    }
    event_notifier_set(&s->peers[peer_id].eventfds[vector]);
}

static unsigned switchtec_db_vector(SwitchtecDbmsgShared *dbmsg,
                                    uint64_t bits)
{
    unsigned bit = ctz64(bits);

    return dbmsg->idb_vec_map[bit];
}

static void switchtec_write_odb(SwitchtecNtbState *s, hwaddr byte_offset,
                                uint64_t val, unsigned size)
{
    SwitchtecDbmsgShared *peer = &s->shared->partition[s->partition ^ 1].dbmsg;
    uint64_t asserted;

    switchtec_store((uint8_t *)&s->dbmsg->odb + byte_offset, val, size);
    asserted = (val << (byte_offset * 8)) & s->dbmsg->odb_mask;
    peer->idb |= asserted;
    asserted &= peer->idb_mask;
    if (asserted) {
        switchtec_notify_peer(s, switchtec_db_vector(peer, asserted));
    }
}

static void switchtec_write_omsg(SwitchtecNtbState *s, unsigned index,
                                 uint32_t val)
{
    SwitchtecDbmsgShared *peer = &s->shared->partition[s->partition ^ 1].dbmsg;
    unsigned target = (s->dbmsg->msg_map >> (index * 8)) & 0x3;

    s->dbmsg->omsg[index].msg = val;
    peer->imsg[target].msg = val;
    peer->imsg[target].src = s->partition;
    peer->imsg[target].status = 1;

    if (!peer->imsg[target].mask) {
        switchtec_notify_peer(s, peer->idb_vec_map[60 + target]);
    }
}

static bool switchtec_dbmsg_decode(SwitchtecNtbState *s, hwaddr addr,
                                   SwitchtecDbmsgShared **dbmsg,
                                   hwaddr *offset)
{
    hwaddr base = SWITCHTEC_GAS_NTB_OFFSET + SWITCHTEC_NTB_DBMSG_OFFSET;
    unsigned partition;

    if (addr < base ||
        addr >= base + SWITCHTEC_PARTITIONS * SWITCHTEC_DBMSG_SIZE) {
        return false;
    }

    partition = (addr - base) / SWITCHTEC_DBMSG_SIZE;
    *offset = (addr - base) % SWITCHTEC_DBMSG_SIZE;
    *dbmsg = &s->shared->partition[partition].dbmsg;
    return true;
}

static void switchtec_gas_write(void *opaque, hwaddr addr,
                                uint64_t val, unsigned size)
{
    SwitchtecNtbState *s = opaque;
    SwitchtecDbmsgShared *dbmsg;
    hwaddr offset;

    if (!switchtec_dbmsg_decode(s, addr, &dbmsg, &offset)) {
        return;
    }

    if (dbmsg == s->dbmsg && offset >= SWITCHTEC_DBMSG_ODB &&
        offset + size <= SWITCHTEC_DBMSG_ODB + 8) {
        switchtec_write_odb(s, offset - SWITCHTEC_DBMSG_ODB, val, size);
    } else if (offset >= SWITCHTEC_DBMSG_ODB_MASK &&
               offset + size <= SWITCHTEC_DBMSG_ODB_MASK + 8) {
        switchtec_store((uint8_t *)&dbmsg->odb_mask +
                        offset - SWITCHTEC_DBMSG_ODB_MASK, val, size);
    } else if (offset >= SWITCHTEC_DBMSG_IDB &&
               offset + size <= SWITCHTEC_DBMSG_IDB + 8) {
        dbmsg->idb &= ~(val << ((offset - SWITCHTEC_DBMSG_IDB) * 8));
    } else if (offset >= SWITCHTEC_DBMSG_IDB_MASK &&
               offset + size <= SWITCHTEC_DBMSG_IDB_MASK + 8) {
        switchtec_store((uint8_t *)&dbmsg->idb_mask +
                        offset - SWITCHTEC_DBMSG_IDB_MASK, val, size);
    } else if (offset >= SWITCHTEC_DBMSG_IDB_VEC_MAP &&
               offset + size <= SWITCHTEC_DBMSG_IDB_VEC_MAP + 64) {
        switchtec_store(dbmsg->idb_vec_map +
                        offset - SWITCHTEC_DBMSG_IDB_VEC_MAP, val, size);
    } else if (offset == SWITCHTEC_DBMSG_MSG_MAP && size == 4) {
        dbmsg->msg_map = val;
    } else if (dbmsg == s->dbmsg && offset >= SWITCHTEC_DBMSG_OMSG &&
               offset < SWITCHTEC_DBMSG_OMSG + 8 * SWITCHTEC_MESSAGES &&
               (offset - SWITCHTEC_DBMSG_OMSG) % 8 == 0 && size == 4) {
        switchtec_write_omsg(s, (offset - SWITCHTEC_DBMSG_OMSG) / 8, val);
    } else if (offset >= SWITCHTEC_DBMSG_IMSG &&
               offset < SWITCHTEC_DBMSG_IMSG + 8 * SWITCHTEC_MESSAGES) {
        unsigned index = (offset - SWITCHTEC_DBMSG_IMSG) / 8;
        unsigned field = (offset - SWITCHTEC_DBMSG_IMSG) % 8;
        uint8_t *raw = (uint8_t *)&dbmsg->imsg[index];

        if (field == 4 && size == 1) {
            dbmsg->imsg[index].status &= ~(uint8_t)val;
        } else {
            switchtec_store(raw + field, val, size);
        }
    }
}

static uint64_t switchtec_gas_read(void *opaque, hwaddr addr, unsigned size)
{
    SwitchtecNtbState *s = opaque;
    SwitchtecDbmsgShared *dbmsg;
    hwaddr offset;
    hwaddr ntb_info = SWITCHTEC_GAS_NTB_OFFSET + SWITCHTEC_NTB_INFO_OFFSET;

    if (addr == SWITCHTEC_GAS_SYS_INFO_OFFSET +
                SWITCHTEC_SYS_INFO_GEN3_PART_ID && size == 1) {
        return s->partition;
    }
    if (addr == ntb_info && size == 1) {
        return SWITCHTEC_PARTITIONS;
    }
    if (addr == ntb_info + 1 && size == 1) {
        return s->partition;
    }
    if (addr == ntb_info + 4 && size == 4) {
        return (1U << SWITCHTEC_PARTITIONS) - 1;
    }
    if (addr == ntb_info + 0x20 + s->partition * 16 + 4 && size == 4) {
        return 1U << (s->partition ^ 1);
    }

    if (!switchtec_dbmsg_decode(s, addr, &dbmsg, &offset)) {
        return 0;
    }

    if (offset >= SWITCHTEC_DBMSG_ODB &&
        offset + size <= SWITCHTEC_DBMSG_ODB + 8) {
        return switchtec_load((uint8_t *)&dbmsg->odb +
                              offset - SWITCHTEC_DBMSG_ODB, size);
    }
    if (offset >= SWITCHTEC_DBMSG_ODB_MASK &&
        offset + size <= SWITCHTEC_DBMSG_ODB_MASK + 8) {
        return switchtec_load((uint8_t *)&dbmsg->odb_mask +
                              offset - SWITCHTEC_DBMSG_ODB_MASK, size);
    }
    if (offset >= SWITCHTEC_DBMSG_IDB &&
        offset + size <= SWITCHTEC_DBMSG_IDB + 8) {
        return switchtec_load((uint8_t *)&dbmsg->idb +
                              offset - SWITCHTEC_DBMSG_IDB, size);
    }
    if (offset >= SWITCHTEC_DBMSG_IDB_MASK &&
        offset + size <= SWITCHTEC_DBMSG_IDB_MASK + 8) {
        return switchtec_load((uint8_t *)&dbmsg->idb_mask +
                              offset - SWITCHTEC_DBMSG_IDB_MASK, size);
    }
    if (offset >= SWITCHTEC_DBMSG_IDB_VEC_MAP &&
        offset + size <= SWITCHTEC_DBMSG_IDB_VEC_MAP + 64) {
        return switchtec_load(dbmsg->idb_vec_map +
                              offset - SWITCHTEC_DBMSG_IDB_VEC_MAP, size);
    }
    if (offset == SWITCHTEC_DBMSG_MSG_MAP && size == 4) {
        return dbmsg->msg_map;
    }
    if (offset >= SWITCHTEC_DBMSG_OMSG &&
        offset + size <= SWITCHTEC_DBMSG_OMSG + 8 * SWITCHTEC_MESSAGES) {
        return switchtec_load((uint8_t *)dbmsg->omsg +
                              offset - SWITCHTEC_DBMSG_OMSG, size);
    }
    if (offset >= SWITCHTEC_DBMSG_IMSG &&
        offset + size <= SWITCHTEC_DBMSG_IMSG + 8 * SWITCHTEC_MESSAGES) {
        return switchtec_load((uint8_t *)dbmsg->imsg +
                              offset - SWITCHTEC_DBMSG_IMSG, size);
    }
    return 0;
}

static const MemoryRegionOps switchtec_gas_ops = {
    .read = switchtec_gas_read,
    .write = switchtec_gas_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void ivshmem_vector_notify(void *opaque)
{
    MSIVector *entry = opaque;
    PCIDevice *pdev = entry->pdev;
    SwitchtecNtbState *s = SWITCHTEC_NTB_IVSHMEM(pdev);
    int vector = entry - s->msi_vectors;
    EventNotifier *n = &s->peers[s->vm_id].eventfds[vector];

    if (!event_notifier_test_and_clear(n)) {
        return;
    }

    IVSHMEM_DPRINTF("interrupt on vector %p %d\n", pdev, vector);
    if (msix_enabled(pdev)) {
        msix_notify(pdev, vector);
    }
}

static int ivshmem_vector_unmask(PCIDevice *dev, unsigned vector,
                                 MSIMessage msg)
{
    SwitchtecNtbState *s = SWITCHTEC_NTB_IVSHMEM(dev);
    EventNotifier *n = &s->peers[s->vm_id].eventfds[vector];
    MSIVector *v = &s->msi_vectors[vector];
    int ret;

    IVSHMEM_DPRINTF("vector unmask %p %d\n", dev, vector);
    if (!v->pdev) {
        error_report("ivshmem: vector %d route does not exist", vector);
        return -EINVAL;
    }
    assert(!v->unmasked);

    ret = kvm_irqchip_update_msi_route(kvm_state, v->virq, msg, dev);
    if (ret < 0) {
        return ret;
    }
    kvm_irqchip_commit_routes(kvm_state);

    ret = kvm_irqchip_add_irqfd_notifier_gsi(kvm_state, n, NULL, v->virq);
    if (ret < 0) {
        return ret;
    }
    v->unmasked = true;

    return 0;
}

static void ivshmem_vector_mask(PCIDevice *dev, unsigned vector)
{
    SwitchtecNtbState *s = SWITCHTEC_NTB_IVSHMEM(dev);
    EventNotifier *n = &s->peers[s->vm_id].eventfds[vector];
    MSIVector *v = &s->msi_vectors[vector];
    int ret;

    IVSHMEM_DPRINTF("vector mask %p %d\n", dev, vector);
    if (!v->pdev) {
        error_report("ivshmem: vector %d route does not exist", vector);
        return;
    }
    assert(v->unmasked);

    ret = kvm_irqchip_remove_irqfd_notifier_gsi(kvm_state, n, v->virq);
    if (ret < 0) {
        error_report("remove_irqfd_notifier_gsi failed");
        return;
    }
    v->unmasked = false;
}

static void ivshmem_vector_poll(PCIDevice *dev,
                                unsigned int vector_start,
                                unsigned int vector_end)
{
    SwitchtecNtbState *s = SWITCHTEC_NTB_IVSHMEM(dev);
    unsigned int vector;

    IVSHMEM_DPRINTF("vector poll %p %d-%d\n", dev, vector_start, vector_end);

    vector_end = MIN(vector_end, s->vectors);

    for (vector = vector_start; vector < vector_end; vector++) {
        EventNotifier *notifier = &s->peers[s->vm_id].eventfds[vector];

        if (!msix_is_masked(dev, vector)) {
            continue;
        }

        if (event_notifier_test_and_clear(notifier)) {
            msix_set_pending(dev, vector);
        }
    }
}

static void watch_vector_notifier(SwitchtecNtbState *s, EventNotifier *n,
                                 int vector)
{
    int eventfd = event_notifier_get_fd(n);

    assert(!s->msi_vectors[vector].pdev);
    s->msi_vectors[vector].pdev = PCI_DEVICE(s);

    qemu_set_fd_handler(eventfd, ivshmem_vector_notify,
                        NULL, &s->msi_vectors[vector]);
}

static void ivshmem_add_eventfd(SwitchtecNtbState *s, int posn, int i)
{
}

static void ivshmem_del_eventfd(SwitchtecNtbState *s, int posn, int i)
{
}

static void close_peer_eventfds(SwitchtecNtbState *s, int posn)
{
    int i, n;

    assert(posn >= 0 && posn < s->nb_peers);
    n = s->peers[posn].nb_eventfds;

    if (ivshmem_has_feature(s, IVSHMEM_IOEVENTFD)) {
        memory_region_transaction_begin();
        for (i = 0; i < n; i++) {
            ivshmem_del_eventfd(s, posn, i);
        }
        memory_region_transaction_commit();
    }

    for (i = 0; i < n; i++) {
        event_notifier_cleanup(&s->peers[posn].eventfds[i]);
    }

    g_free(s->peers[posn].eventfds);
    s->peers[posn].nb_eventfds = 0;
}

static void resize_peers(SwitchtecNtbState *s, int nb_peers)
{
    int old_nb_peers = s->nb_peers;
    int i;

    assert(nb_peers > old_nb_peers);
    IVSHMEM_DPRINTF("bumping storage to %d peers\n", nb_peers);

    s->peers = g_renew(Peer, s->peers, nb_peers);
    s->nb_peers = nb_peers;

    for (i = old_nb_peers; i < nb_peers; i++) {
        s->peers[i].eventfds = g_new0(EventNotifier, s->vectors);
        s->peers[i].nb_eventfds = 0;
    }
}

static void ivshmem_add_kvm_msi_virq(SwitchtecNtbState *s, int vector,
                                     Error **errp)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    KVMRouteChange c;
    int ret;

    IVSHMEM_DPRINTF("ivshmem_add_kvm_msi_virq vector:%d\n", vector);
    assert(!s->msi_vectors[vector].pdev);

    c = kvm_irqchip_begin_route_changes(kvm_state);
    ret = kvm_irqchip_add_msi_route(&c, vector, pdev);
    if (ret < 0) {
        error_setg(errp, "kvm_irqchip_add_msi_route failed");
        return;
    }
    kvm_irqchip_commit_route_changes(&c);

    s->msi_vectors[vector].virq = ret;
    s->msi_vectors[vector].pdev = pdev;
}

static bool setup_interrupt(SwitchtecNtbState *s, int vector, Error **errp)
{
    EventNotifier *n = &s->peers[s->vm_id].eventfds[vector];
    bool with_irqfd = kvm_msi_via_irqfd_enabled() &&
        ivshmem_has_feature(s, IVSHMEM_MSI);
    PCIDevice *pdev = PCI_DEVICE(s);
    Error *err = NULL;
    int ret;

    IVSHMEM_DPRINTF("setting up interrupt for vector: %d\n", vector);

    if (!with_irqfd) {
        IVSHMEM_DPRINTF("with eventfd\n");
        watch_vector_notifier(s, n, vector);
    } else if (msix_enabled(pdev)) {
        IVSHMEM_DPRINTF("with irqfd\n");
        ivshmem_add_kvm_msi_virq(s, vector, &err);
        if (err) {
            error_propagate(errp, err);
            return false;
        }

        if (!msix_is_masked(pdev, vector)) {
            ret = kvm_irqchip_add_irqfd_notifier_gsi(kvm_state, n, NULL,
                                               s->msi_vectors[vector].virq);
            if (ret < 0) {
                error_setg(errp, "Failed to configure irqfd notifier");
                return false;
            }
        }
    } else {
        /* it will be delayed until msix is enabled, in write_config */
        IVSHMEM_DPRINTF("with irqfd, delayed until msix enabled\n");
    }
    return true;
}

static void process_msg_shmem(SwitchtecNtbState *s, int fd, Error **errp)
{
    struct stat buf;
    size_t size;

    if (fd < 0) {
        error_setg(errp, "server didn't provide fd with shared memory message");
        return;
    }

    if (s->ivshmem_bar2) {
        error_setg(errp, "server sent unexpected shared memory message");
        close(fd);
        return;
    }

    if (fstat(fd, &buf) < 0) {
        error_setg_errno(errp, errno,
            "can't determine size of shared memory sent by server");
        close(fd);
        return;
    }

    size = buf.st_size;

    /* mmap the region and map into the BAR2 */
    if (!memory_region_init_ram_from_fd(&s->server_bar2, OBJECT(s),
                                        "ivshmem.bar2", size, RAM_SHARED,
                                        fd, 0, errp)) {
        return;
    }

    s->ivshmem_bar2 = &s->server_bar2;
}

static void process_msg_disconnect(SwitchtecNtbState *s, uint16_t posn,
                                   Error **errp)
{
    IVSHMEM_DPRINTF("posn %d has gone away\n", posn);
    if (posn >= s->nb_peers || posn == s->vm_id) {
        error_setg(errp, "invalid peer %d", posn);
        return;
    }
    close_peer_eventfds(s, posn);
}

static void process_msg_connect(SwitchtecNtbState *s, uint16_t posn, int fd,
                                Error **errp)
{
    Peer *peer = &s->peers[posn];
    int vector;

    /*
     * The N-th connect message for this peer comes with the file
     * descriptor for vector N-1.  Count messages to find the vector.
     */
    if (peer->nb_eventfds >= s->vectors) {
        error_setg(errp, "Too many eventfd received, device has %d vectors",
                   s->vectors);
        close(fd);
        return;
    }
    vector = peer->nb_eventfds++;

    IVSHMEM_DPRINTF("eventfds[%d][%d] = %d\n", posn, vector, fd);
    event_notifier_init_fd(&peer->eventfds[vector], fd);

    /* msix/irqfd poll non block */
    if (!qemu_set_blocking(fd, false, errp)) {
        close(fd);
        return;
    }

    if (posn == s->vm_id) {
        setup_interrupt(s, vector, errp);
        /* TODO do we need to handle the error? */
    }

    if (ivshmem_has_feature(s, IVSHMEM_IOEVENTFD)) {
        ivshmem_add_eventfd(s, posn, vector);
    }
}

static void process_msg(SwitchtecNtbState *s, int64_t msg, int fd, Error **errp)
{
    IVSHMEM_DPRINTF("posn is %" PRId64 ", fd is %d\n", msg, fd);

    if (msg < -1 || msg > IVSHMEM_MAX_PEERS) {
        error_setg(errp, "server sent invalid message %" PRId64, msg);
        if (fd >= 0) {
            close(fd);
        }
        return;
    }

    if (msg == -1) {
        process_msg_shmem(s, fd, errp);
        return;
    }

    if (msg >= s->nb_peers) {
        resize_peers(s, msg + 1);
    }

    if (fd >= 0) {
        process_msg_connect(s, msg, fd, errp);
    } else {
        process_msg_disconnect(s, msg, errp);
    }
}

static int ivshmem_can_receive(void *opaque)
{
    SwitchtecNtbState *s = opaque;

    assert(s->msg_buffered_bytes < sizeof(s->msg_buf));
    return sizeof(s->msg_buf) - s->msg_buffered_bytes;
}

static void ivshmem_read(void *opaque, const uint8_t *buf, int size)
{
    SwitchtecNtbState *s = opaque;
    Error *err = NULL;
    int fd;
    int64_t msg;

    assert(size >= 0 && s->msg_buffered_bytes + size <= sizeof(s->msg_buf));
    memcpy((unsigned char *)&s->msg_buf + s->msg_buffered_bytes, buf, size);
    s->msg_buffered_bytes += size;
    if (s->msg_buffered_bytes < sizeof(s->msg_buf)) {
        return;
    }
    msg = le64_to_cpu(s->msg_buf);
    s->msg_buffered_bytes = 0;

    fd = qemu_chr_fe_get_msgfd(&s->server_chr);

    process_msg(s, msg, fd, &err);
    if (err) {
        error_report_err(err);
    }
}

static int64_t ivshmem_recv_msg(SwitchtecNtbState *s, int *pfd, Error **errp)
{
    int64_t msg;
    int n, ret;

    n = 0;
    do {
        ret = qemu_chr_fe_read_all(&s->server_chr, (uint8_t *)&msg + n,
                                   sizeof(msg) - n);
        if (ret < 0) {
            if (ret == -EINTR) {
                continue;
            }
            error_setg_errno(errp, -ret, "read from server failed");
            return INT64_MIN;
        }
        n += ret;
    } while (n < sizeof(msg));

    *pfd = qemu_chr_fe_get_msgfd(&s->server_chr);
    return le64_to_cpu(msg);
}

static void ivshmem_recv_setup(SwitchtecNtbState *s, Error **errp)
{
    Error *err = NULL;
    int64_t msg;
    int fd;

    msg = ivshmem_recv_msg(s, &fd, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    if (msg != IVSHMEM_PROTOCOL_VERSION) {
        error_setg(errp, "server sent version %" PRId64 ", expecting %d",
                   msg, IVSHMEM_PROTOCOL_VERSION);
        return;
    }
    if (fd != -1) {
        error_setg(errp, "server sent invalid version message");
        return;
    }

    /*
     * ivshmem-server sends the remaining initial messages in a fixed
     * order, but the device has always accepted them in any order.
     * Stay as compatible as practical, just in case people use
     * servers that behave differently.
     */

    /*
     * ivshmem_device_spec.txt has always required the ID message
     * right here, and ivshmem-server has always complied.  However,
     * older versions of the device accepted it out of order, but
     * broke when an interrupt setup message arrived before it.
     */
    msg = ivshmem_recv_msg(s, &fd, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    if (fd != -1 || msg < 0 || msg > IVSHMEM_MAX_PEERS) {
        error_setg(errp, "server sent invalid ID message");
        return;
    }
    s->vm_id = msg;

    /*
     * Receive more messages until we got shared memory.
     */
    do {
        msg = ivshmem_recv_msg(s, &fd, &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }
        process_msg(s, msg, fd, &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }
    } while (msg != -1);

    /*
     * This function must either map the shared memory or fail.  The
     * loop above ensures that: it terminates normally only after it
     * successfully processed the server's shared memory message.
     * Assert that actually mapped the shared memory:
     */
    assert(s->ivshmem_bar2);
}

/*
 * Select the MSI-X vectors used by the device.
 * ivshmem maps events to vectors statically, so
 * we just enable all vectors on init and after reset.
 */
static void ivshmem_msix_vector_use(SwitchtecNtbState *s)
{
    PCIDevice *d = PCI_DEVICE(s);
    int i;

    for (i = 0; i < s->vectors; i++) {
        msix_vector_use(d, i);
    }
}

static void ivshmem_disable_irqfd(SwitchtecNtbState *s);

static void ivshmem_reset(DeviceState *d)
{
    SwitchtecNtbState *s = SWITCHTEC_NTB_IVSHMEM(d);

    ivshmem_disable_irqfd(s);

    if (ivshmem_has_feature(s, IVSHMEM_MSI)) {
        ivshmem_msix_vector_use(s);
    }
}

static int ivshmem_setup_interrupts(SwitchtecNtbState *s, Error **errp)
{
    /* allocate QEMU callback data for receiving interrupts */
    s->msi_vectors = g_new0(MSIVector, s->vectors);

    if (ivshmem_has_feature(s, IVSHMEM_MSI)) {
        hwaddr dbmsg = SWITCHTEC_GAS_NTB_OFFSET +
                       SWITCHTEC_NTB_DBMSG_OFFSET +
                       s->partition * SWITCHTEC_DBMSG_SIZE;

        if (msix_init(PCI_DEVICE(s), s->vectors,
                      &s->switchtec_gas, 0,
                      dbmsg + SWITCHTEC_DBMSG_MSIX_TABLE,
                      &s->switchtec_gas, 0,
                      dbmsg + SWITCHTEC_DBMSG_PBA, 0, errp)) {
            return -1;
        }

        IVSHMEM_DPRINTF("msix initialized (%d vectors)\n", s->vectors);
        ivshmem_msix_vector_use(s);
    }

    return 0;
}

static void ivshmem_remove_kvm_msi_virq(SwitchtecNtbState *s, int vector)
{
    IVSHMEM_DPRINTF("ivshmem_remove_kvm_msi_virq vector:%d\n", vector);

    if (s->msi_vectors[vector].pdev == NULL) {
        return;
    }

    /* it was cleaned when masked in the frontend. */
    kvm_irqchip_release_virq(kvm_state, s->msi_vectors[vector].virq);

    s->msi_vectors[vector].pdev = NULL;
}

static void ivshmem_enable_irqfd(SwitchtecNtbState *s)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    int i;

    for (i = 0; i < s->peers[s->vm_id].nb_eventfds; i++) {
        Error *err = NULL;

        ivshmem_add_kvm_msi_virq(s, i, &err);
        if (err) {
            error_report_err(err);
            goto undo;
        }
    }

    if (msix_set_vector_notifiers(pdev,
                                  ivshmem_vector_unmask,
                                  ivshmem_vector_mask,
                                  ivshmem_vector_poll)) {
        error_report("ivshmem: msix_set_vector_notifiers failed");
        goto undo;
    }
    return;

undo:
    while (--i >= 0) {
        ivshmem_remove_kvm_msi_virq(s, i);
    }
}

static void ivshmem_disable_irqfd(SwitchtecNtbState *s)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    int i;

    if (!pdev->msix_vector_use_notifier) {
        return;
    }

    msix_unset_vector_notifiers(pdev);

    for (i = 0; i < s->peers[s->vm_id].nb_eventfds; i++) {
        /*
         * MSI-X is already disabled here so msix_unset_vector_notifiers()
         * didn't call our release notifier.  Do it now to keep our masks and
         * unmasks balanced.
         */
        if (s->msi_vectors[i].unmasked) {
            ivshmem_vector_mask(pdev, i);
        }
        ivshmem_remove_kvm_msi_virq(s, i);
    }

}

static void ivshmem_write_config(PCIDevice *pdev, uint32_t address,
                                 uint32_t val, int len)
{
    SwitchtecNtbState *s = SWITCHTEC_NTB_IVSHMEM(pdev);
    int is_enabled, was_enabled = msix_enabled(pdev);

    pci_default_write_config(pdev, address, val, len);
    is_enabled = msix_enabled(pdev);

    if (kvm_msi_via_irqfd_enabled()) {
        if (!was_enabled && is_enabled) {
            ivshmem_enable_irqfd(s);
        } else if (was_enabled && !is_enabled) {
            ivshmem_disable_irqfd(s);
        }
    }
}

static void switchtec_ntb_realize(PCIDevice *dev, Error **errp)
{
    ERRP_GUARD();
    SwitchtecNtbState *s = SWITCHTEC_NTB_IVSHMEM(dev);
    Error *err = NULL;
    uint8_t *pci_conf;

    /* IRQFD requires MSI */
    if (ivshmem_has_feature(s, IVSHMEM_IOEVENTFD) &&
        !ivshmem_has_feature(s, IVSHMEM_MSI)) {
        error_setg(errp, "ioeventfd/irqfd requires MSI");
        return;
    }

    pci_conf = dev->config;
    pci_conf[PCI_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;

    memory_region_init_io(&s->switchtec_gas, OBJECT(s), &switchtec_gas_ops, s,
                          "switchtec-gas", SWITCHTEC_GAS_SIZE);

    /* region for registers*/
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->switchtec_gas);

    if (s->hostmem != NULL) {
        IVSHMEM_DPRINTF("using hostmem\n");

        s->ivshmem_bar2 = host_memory_backend_get_memory(s->hostmem);
        host_memory_backend_set_mapped(s->hostmem, true);
    } else {
        Chardev *chr = qemu_chr_fe_get_driver(&s->server_chr);
        g_autofree char *filename = NULL;

        assert(chr);
        filename = qemu_chr_get_filename(chr);
        IVSHMEM_DPRINTF("using shared memory server (socket = %s)\n", filename);

        /* we allocate enough space for 16 peers and grow as needed */
        resize_peers(s, 16);

        /*
         * Receive setup messages from server synchronously.
         * Older versions did it asynchronously, but that creates a
         * number of entertaining race conditions.
         */
        ivshmem_recv_setup(s, &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }

        if (s->master == ON_OFF_AUTO_ON && s->vm_id != 0) {
            error_setg(errp,
                       "master must connect to the server before any peers");
            return;
        }

        qemu_chr_fe_set_handlers(&s->server_chr, ivshmem_can_receive,
                                 ivshmem_read, NULL, NULL, s, NULL, true);

        if (ivshmem_setup_interrupts(s, errp) < 0) {
            error_prepend(errp, "Failed to initialize interrupts: ");
            return;
        }
    }

    if (s->master == ON_OFF_AUTO_AUTO) {
        s->master = s->vm_id == 0 ? ON_OFF_AUTO_ON : ON_OFF_AUTO_OFF;
    }

    if (memory_region_size(s->ivshmem_bar2) < sizeof(*s->shared)) {
        error_setg(errp, "shared memory must be at least %zu bytes",
                   sizeof(*s->shared));
        return;
    }

    s->shared = memory_region_get_ram_ptr(s->ivshmem_bar2);
    if (s->shared->magic != SWITCHTEC_SHM_MAGIC) {
        memset(s->shared, 0, sizeof(*s->shared));
        s->shared->magic = SWITCHTEC_SHM_MAGIC;
        s->shared->version = 1;
        for (int i = 0; i < SWITCHTEC_PARTITIONS; i++) {
            s->shared->vm_id[i] = -1;
        }
    }
    s->shared->vm_id[s->partition] = s->vm_id;
    s->dbmsg = &s->shared->partition[s->partition].dbmsg;

    if (!ivshmem_is_master(s)) {
        error_setg(&s->migration_blocker,
                   "Migration is disabled for switchtec-ntb-ivshmem peers");
        if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
            return;
        }
    }

    vmstate_register_ram(s->ivshmem_bar2, DEVICE(s));
}

static void ivshmem_exit(PCIDevice *dev)
{
    SwitchtecNtbState *s = SWITCHTEC_NTB_IVSHMEM(dev);
    int i;

    migrate_del_blocker(&s->migration_blocker);

    if (memory_region_is_mapped(s->ivshmem_bar2)) {
        if (!s->hostmem) {
            void *addr = memory_region_get_ram_ptr(s->ivshmem_bar2);
            int fd;

            if (munmap(addr, memory_region_size(s->ivshmem_bar2) == -1)) {
                error_report("Failed to munmap shared memory %s",
                             strerror(errno));
            }

            fd = memory_region_get_fd(s->ivshmem_bar2);
            close(fd);
        }

        vmstate_unregister_ram(s->ivshmem_bar2, DEVICE(dev));
    }

    if (s->hostmem) {
        host_memory_backend_set_mapped(s->hostmem, false);
    }

    if (s->peers) {
        for (i = 0; i < s->nb_peers; i++) {
            close_peer_eventfds(s, i);
        }
        g_free(s->peers);
    }

    if (ivshmem_has_feature(s, IVSHMEM_MSI)) {
        msix_uninit(dev, &s->switchtec_gas, &s->switchtec_gas);
    }

    g_free(s->msi_vectors);
}

static int ivshmem_pre_load(void *opaque)
{
    SwitchtecNtbState *s = opaque;

    if (!ivshmem_is_master(s)) {
        error_report("'peer' devices are not migratable");
        return -EINVAL;
    }

    return 0;
}

static int ivshmem_post_load(void *opaque, int version_id)
{
    SwitchtecNtbState *s = opaque;

    if (ivshmem_has_feature(s, IVSHMEM_MSI)) {
        ivshmem_msix_vector_use(s);
    }
    return 0;
}

static const VMStateDescription switchtec_ntb_vmsd = {
    .name = TYPE_SWITCHTEC_NTB_IVSHMEM,
    .version_id = 0,
    .minimum_version_id = 0,
    .pre_load = ivshmem_pre_load,
    .post_load = ivshmem_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SwitchtecNtbState),
        VMSTATE_MSIX(parent_obj, SwitchtecNtbState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property switchtec_ntb_properties[] = {
    DEFINE_PROP_CHR("chardev", SwitchtecNtbState, server_chr),
    DEFINE_PROP_UINT32("vectors", SwitchtecNtbState, vectors, 4),
    DEFINE_PROP_UINT32("partition", SwitchtecNtbState, partition, 0),
    DEFINE_PROP_BIT("ioeventfd", SwitchtecNtbState, features, IVSHMEM_IOEVENTFD,
                    false),
    DEFINE_PROP_ON_OFF_AUTO("master", SwitchtecNtbState, master,
                            ON_OFF_AUTO_OFF),
};

static void switchtec_ntb_instance_init(Object *obj)
{
    SwitchtecNtbState *s = SWITCHTEC_NTB_IVSHMEM(obj);

    s->features |= 1 << IVSHMEM_MSI;
}

static void switchtec_ntb_device_realize(PCIDevice *dev, Error **errp)
{
    SwitchtecNtbState *s = SWITCHTEC_NTB_IVSHMEM(dev);

    if (!qemu_chr_fe_backend_connected(&s->server_chr)) {
        error_setg(errp, "You must specify a 'chardev'");
        return;
    }
    if (s->partition >= SWITCHTEC_PARTITIONS) {
        error_setg(errp, "partition must be 0 or 1");
        return;
    }

    switchtec_ntb_realize(dev, errp);
}

static void switchtec_ntb_device_class_init(ObjectClass *klass,
                                             const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = switchtec_ntb_device_realize;
    k->exit = ivshmem_exit;
    k->config_write = ivshmem_write_config;
    k->vendor_id = PCI_VENDOR_ID_SWITCHTEC;
    k->device_id = PCI_DEVICE_ID_SWITCHTEC;
    k->class_id = PCI_CLASS_BRIDGE_OTHER;
    k->revision = 0;
    device_class_set_legacy_reset(dc, ivshmem_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, switchtec_ntb_properties);
    dc->desc = "Microsemi Switchtec NTB over shared memory";
    dc->vmsd = &switchtec_ntb_vmsd;
}

static const TypeInfo switchtec_ntb_info = {
    .name          = TYPE_SWITCHTEC_NTB_IVSHMEM,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(SwitchtecNtbState),
    .instance_init = switchtec_ntb_instance_init,
    .class_init    = switchtec_ntb_device_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ivshmem_register_types(void)
{
    type_register_static(&switchtec_ntb_info);
}

type_init(ivshmem_register_types)
