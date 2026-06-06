/*
 * QTest suite for the IDT NTB ivshmem PCI device.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include <sys/mman.h>

#include "contrib/ivshmem-server/ivshmem-server.h"
#include "libqos/libqos-pc.h"
#include "libqtest.h"

#define PCI_VENDOR_ID_IDT_NTB  0x111d
#define PCI_DEVICE_ID_IDT_NTB  0x8091

#define IDT_TEST_SHM_SIZE      (1U << 20)
#define IDT_TEST_PEER_MEM_SIZE (64 * 1024)

#define IDT_NT_PCICMDSTS       0x004U
#define IDT_NT_PCIELCAP        0x04CU
#define IDT_NT_PCIELCTLSTS     0x050U
#define IDT_NT_NTCTL           0x400U
#define IDT_NT_NTINTSTS        0x404U
#define IDT_NT_NTINTMSK        0x00408U
#define IDT_NT_NTGSIGNAL       0x410U
#define IDT_NT_OUTDBELLSET     0x420U
#define IDT_NT_INDBELLSTS      0x428U
#define IDT_NT_INDBELLMSK      0x42CU
#define IDT_NT_OUTMSG0         0x430U
#define IDT_NT_INMSG0          0x440U
#define IDT_NT_MSGSTS          0x460U
#define IDT_NT_MSGSTSMSK       0x464U
#define IDT_NT_NTMTBLADDR      0x4D0U
#define IDT_NT_NTMTBLDATA      0x4D8U
#define IDT_NT_GASAADDR        0xFF8U
#define IDT_NT_GASADATA        0xFFCU
#define IDT_NT_BARSETUP0       0x470U
#define IDT_NT_BARLIMIT2       0x494U
#define IDT_NT_BARLTBASE2      0x498U
#define IDT_NT_BARUTBASE2      0x49CU
#define IDT_OOF_TPART          0x1000U
#define IDT_OOF_LUTOFF         0x1004U
#define IDT_OOF_LDATA          0x1008U
#define IDT_OOF_HDATA          0x100CU
#define IDT_OOF_LLIMIT         0x1010U
#define IDT_SW_SWPORT0STS      0x3E204U
#define IDT_SW_SWPORT2STS      0x3E244U
#define IDT_SW_SWPART0STS      0x3E104U
#define IDT_SW_NTP0_PCIECMDSTS 0x1004U
#define IDT_SW_NTP0_NTCTL      0x1400U
#define IDT_SW_NTP2_PCIECMDSTS 0x5004U
#define IDT_SW_NTP2_NTCTL      0x5400U
#define IDT_SW_NTP0_BARSETUP0  0x1470U
#define IDT_SW_NTP2_BARSETUP0  0x5470U
#define IDT_SW_SWP0MSGCTL0     0x3EE00U
#define IDT_SW_SWP0MSGCTL1     0x3EE20U
#define IDT_SW_SWP0MSGCTL2     0x3EE40U
#define IDT_SW_SWP0MSGCTL3     0x3EE60U
#define IDT_SW_SESTS           0x3EC00U
#define IDT_SW_SELINKUPSTS     0x3EC0CU
#define IDT_SW_SELINKDNSTS     0x3EC14U
#define IDT_SW_SEGSIGSTS       0x3EC30U
#define IDT_NT_OUTMSG1         0x434U
#define IDT_NT_OUTMSG2         0x438U
#define IDT_NT_OUTMSG3         0x43CU
#define IDT_NT_INMSG1          0x444U
#define IDT_NT_BARSETUP1       0x480U
#define IDT_NT_BARLIMIT1       0x484U
#define IDT_NT_REQIDCAP        0x4DCU

static char *tmpdir;
static char *tmpserver;
static char *tmpshm;
static void *tmpshmem;
static char *peer_mem0;
static char *peer_mem1;
static void *peer0_ptr;
static void *peer1_ptr;

typedef struct IdtServerThread {
    GThread *thread;
    IvshmemServer *server;
    int pipe[2];
} IdtServerThread;

typedef struct IdtNtbState {
    QOSState *qs;
    QPCIDevice *dev;
    QPCIBar mmio_bar;
    QPCIBar mem_bar;
} IdtNtbState;

static IvshmemServer global_server;
static IdtServerThread global_thread = { 0 };
static bool server_running;

static void save_device(QPCIDevice *dev, int devfn, void *data)
{
    QPCIDevice **pdev = data;

    *pdev = dev;
}

static QPCIDevice *get_idt_device(QPCIBus *pcibus)
{
    QPCIDevice *dev = NULL;

    qpci_device_foreach(pcibus, PCI_VENDOR_ID_IDT_NTB, PCI_DEVICE_ID_IDT_NTB,
                        save_device, &dev);
    g_assert_nonnull(dev);
    return dev;
}

static uint32_t idt_readl(IdtNtbState *s, uint32_t reg)
{
    return qpci_io_readl(s->dev, s->mmio_bar, reg);
}

static void idt_writel(IdtNtbState *s, uint32_t reg, uint32_t val)
{
    qpci_io_writel(s->dev, s->mmio_bar, reg, val);
}

static void setup_peer_shm_files(void)
{
    int fd;
    void *ptr;

    peer_mem0 = g_strdup("/dev/shm/qemu1");
    peer_mem1 = g_strdup("/dev/shm/qemu2");

    fd = open(peer_mem0, O_RDWR | O_CREAT | O_TRUNC, 0666);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, IDT_TEST_PEER_MEM_SIZE), ==, 0);
    ptr = mmap(NULL, IDT_TEST_PEER_MEM_SIZE, PROT_READ | PROT_WRITE,
               MAP_SHARED, fd, 0);
    g_assert(ptr != MAP_FAILED);
    peer0_ptr = ptr;
    close(fd);

    fd = open(peer_mem1, O_RDWR | O_CREAT | O_TRUNC, 0666);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, IDT_TEST_PEER_MEM_SIZE), ==, 0);
    ptr = mmap(NULL, IDT_TEST_PEER_MEM_SIZE, PROT_READ | PROT_WRITE,
               MAP_SHARED, fd, 0);
    g_assert(ptr != MAP_FAILED);
    peer1_ptr = ptr;
    close(fd);
}

static void *server_thread(void *data)
{
    IdtServerThread *t = data;
    IvshmemServer *server = t->server;

    while (true) {
        fd_set fds;
        int maxfd, ret;

        FD_ZERO(&fds);
        FD_SET(t->pipe[0], &fds);
        maxfd = t->pipe[0] + 1;

        ivshmem_server_get_fds(server, &fds, &maxfd);
        ret = select(maxfd, &fds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ret == 0) {
            continue;
        }
        if (FD_ISSET(t->pipe[0], &fds)) {
            break;
        }
        if (ivshmem_server_handle_fds(server, &fds, maxfd) < 0) {
            break;
        }
    }
    return NULL;
}

static void start_ivshmem_server(IvshmemServer *server, IdtServerThread *thread,
                                 int nvectors)
{
    GError *err = NULL;
    int ret;

    ret = ivshmem_server_init(server, tmpserver, tmpshm, true,
                              IDT_TEST_SHM_SIZE, nvectors, false);
    g_assert_cmpint(ret, ==, 0);
    ret = ivshmem_server_start(server);
    g_assert_cmpint(ret, ==, 0);

    g_unix_open_pipe(thread->pipe, FD_CLOEXEC, &err);
    g_assert_no_error(err);
    thread->server = server;
    thread->thread = g_thread_new("idt-ivshmem-server", server_thread, thread);
}

static void stop_ivshmem_server(IdtServerThread *thread, IvshmemServer *server)
{
    if (thread->thread) {
        g_assert_cmpint(qemu_write_full(thread->pipe[1], "q", 1), ==, 1);
        g_thread_join(thread->thread);
        thread->thread = NULL;
    }
    ivshmem_server_close(server);
    close(thread->pipe[0]);
    close(thread->pipe[1]);
}

static void stop_global_server(void)
{
    if (server_running) {
        stop_ivshmem_server(&global_thread, &global_server);
        server_running = false;
    }
}

static void ensure_global_server(void)
{
    if (!server_running) {
        start_ivshmem_server(&global_server, &global_thread, 2);
        server_running = true;
    }
}

static void setup_idt_vm(IdtNtbState *s, const char *chr_id, int number,
                         bool master, bool new_server)
{
    uint64_t barsize;
    gchar *cmd;

    if (new_server) {
        stop_global_server();
    }
    ensure_global_server();

    cmd = g_strdup_printf(
        "-chardev socket,id=%s,path=%s "
        "-device idt-ntb-ivshmem,chardev=%s,vectors=2,number=%d,master=%s",
        chr_id, tmpserver, chr_id, number, master ? "on" : "off");

    s->qs = qtest_pc_boot("%s", cmd);
    s->dev = get_idt_device(s->qs->pcibus);
    s->mmio_bar = qpci_iomap(s->dev, 0, &barsize);
    g_assert_cmpuint(barsize, >=, 0x2000);
    s->mem_bar = qpci_iomap(s->dev, 2, &barsize);
    qpci_device_enable(s->dev);
    g_free(cmd);
}

static void cleanup_idt_vm(IdtNtbState *s)
{
    if (s->dev) {
        g_free(s->dev);
        s->dev = NULL;
    }
    if (s->qs) {
        qtest_shutdown(s->qs);
        s->qs = NULL;
    }
}

static void test_idt_ntb_basic_registers(void)
{
    IdtNtbState state = { 0 };
    uint32_t val;

    setup_idt_vm(&state, "chr0", 0, true, true);

    val = idt_readl(&state, IDT_NT_PCICMDSTS);
    g_assert_cmpuint(val & (1U << 2), !=, 0);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_PCIELCAP), ==, 0);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_PCIELCTLSTS), !=, 0);

    idt_writel(&state, IDT_NT_NTCTL, 0x42);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_NTCTL), ==, 0x42);
    g_assert_cmpuint(idt_readl(&state, 0xdead), ==, 0);

    cleanup_idt_vm(&state);
}

static void test_idt_ntb_doorbell(void)
{
    IdtNtbState s0 = { 0 }, s1 = { 0 };

    setup_idt_vm(&s0, "chr0", 0, true, true);
    setup_idt_vm(&s1, "chr1", 1, false, false);

    idt_writel(&s0, IDT_NT_INDBELLMSK, 0xffffffff);
    idt_writel(&s0, IDT_NT_OUTDBELLSET, 0x1234);
    g_usleep(50000);

    g_assert_cmpuint(idt_readl(&s1, IDT_NT_INDBELLSTS), ==, 0x1234);
    idt_writel(&s1, IDT_NT_INDBELLSTS, 0x1234);
    g_assert_cmpuint(idt_readl(&s1, IDT_NT_INDBELLSTS), ==, 0);

    cleanup_idt_vm(&s0);
    cleanup_idt_vm(&s1);
}

static void test_idt_ntb_messages(void)
{
    IdtNtbState s0 = { 0 }, s1 = { 0 };

    setup_idt_vm(&s0, "chr0", 0, true, true);
    setup_idt_vm(&s1, "chr1", 1, false, false);

    idt_writel(&s0, IDT_NT_MSGSTSMSK, 0);
    idt_writel(&s1, IDT_NT_MSGSTSMSK, 0);
    idt_writel(&s0, IDT_NT_NTINTMSK, 0);
    idt_writel(&s1, IDT_NT_NTINTMSK, 0);

    idt_writel(&s0, IDT_NT_OUTMSG0, 0xaaaabbbb);
    g_usleep(50000);
    g_assert_cmpuint(idt_readl(&s1, IDT_NT_INMSG0), ==, 0xaaaabbbb);

    idt_writel(&s0, IDT_NT_OUTMSG0, 0x2);
    g_usleep(50000);
    g_assert_cmpuint(idt_readl(&s0, IDT_NT_MSGSTS) & 1, !=, 0);

    idt_writel(&s1, IDT_NT_MSGSTS, 0xffffffff);
    g_assert_cmpuint(idt_readl(&s1, IDT_NT_MSGSTS), ==, 0);

    cleanup_idt_vm(&s0);
    cleanup_idt_vm(&s1);
}

static void test_idt_ntb_mtb_gas(void)
{
    IdtNtbState state = { 0 };

    setup_idt_vm(&state, "chr0", 0, true, true);

    idt_writel(&state, IDT_NT_NTMTBLADDR, 0);
    idt_writel(&state, IDT_NT_NTMTBLDATA, 0xfeedface);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_NTMTBLDATA), ==, 0xfeedface);

    idt_writel(&state, IDT_NT_NTMTBLADDR, 0x1);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_NTMTBLDATA), ==, 0);

    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_SWPORT0STS);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_GASADATA), !=, 0);

    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_NTP0_NTCTL);
    idt_writel(&state, IDT_NT_NTCTL, 0x99);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_GASADATA), ==, 0x99);

    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_SWP0MSGCTL0);
    idt_writel(&state, IDT_NT_GASADATA, 0x10);
    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_SEGSIGSTS);
    idt_writel(&state, IDT_NT_GASADATA, 0xffffffff);

    idt_writel(&state, IDT_NT_NTINTSTS, 0xffffffff);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_NTINTSTS), ==, 0);

    cleanup_idt_vm(&state);
}

static void test_idt_ntb_bar_config_and_memory(void)
{
    IdtNtbState s0 = { 0 }, s1 = { 0 };
    uint32_t val;

    setup_idt_vm(&s0, "chr0", 0, true, true);
    setup_idt_vm(&s1, "chr1", 1, false, false);

    idt_writel(&s0, IDT_NT_BARSETUP0, 0x12345678);
    g_assert_cmpuint(idt_readl(&s0, IDT_NT_BARSETUP0), ==, 0x12345678);

    idt_writel(&s0, IDT_OOF_TPART, 2);
    idt_writel(&s0, IDT_OOF_LUTOFF, 2);
    idt_writel(&s0, IDT_OOF_LDATA, 0x4000);
    idt_writel(&s0, IDT_OOF_HDATA, 0);
    idt_writel(&s0, IDT_OOF_LLIMIT, 0x5000);

    memset(peer1_ptr, 0, IDT_TEST_PEER_MEM_SIZE);
    idt_writel(&s0, IDT_NT_BARLTBASE2, 0x1000);
    idt_writel(&s0, IDT_NT_BARUTBASE2, 0);
    /* limit is an absolute address; must exceed the mapped BAR base */
    idt_writel(&s0, IDT_NT_BARLIMIT2, 0xf0000000);

    qpci_io_writel(s0.dev, s0.mem_bar, 0, 0xdeadbeef);
    val = qpci_io_readl(s0.dev, s0.mem_bar, 0);
    g_assert_cmpuint(val, ==, 0xdeadbeef);
    g_assert_cmpuint(*(uint32_t *)((char *)peer1_ptr + 0x1000), ==, 0xdeadbeef);

    idt_writel(&s0, IDT_NT_NTGSIGNAL, 1);
    g_usleep(50000);

    cleanup_idt_vm(&s0);
    cleanup_idt_vm(&s1);
}

static void test_idt_ntb_extended_registers(void)
{
    IdtNtbState state = { 0 };
    uint32_t val;

    setup_idt_vm(&state, "chr0", 0, true, true);

    g_assert_cmpuint(idt_readl(&state, IDT_NT_REQIDCAP), !=, 0);

    idt_writel(&state, IDT_NT_BARSETUP1, 0xaabbccdd);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_BARSETUP1), ==, 0xaabbccdd);
    idt_writel(&state, IDT_NT_BARLIMIT1, 0x100);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_BARLIMIT1), ==, 0x100);

    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_SWPORT2STS);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_GASADATA), !=, 0);
    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_SWPART0STS);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_GASADATA), !=, 0);
    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_NTP0_PCIECMDSTS);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_GASADATA), !=, 0);
    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_NTP2_PCIECMDSTS);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_GASADATA), !=, 0);
    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_NTP2_NTCTL);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_GASADATA), ==, 0);
    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_NTP0_BARSETUP0);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_GASADATA), !=, 0);
    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_NTP2_BARSETUP0);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_GASADATA), !=, 0);
    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_SESTS);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_GASADATA), ==, 0);
    idt_writel(&state, IDT_NT_GASAADDR, 0xbadcafe);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_GASADATA), ==, 0);

    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_SWP0MSGCTL1);
    idt_writel(&state, IDT_NT_GASADATA, 0x11);
    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_SWP0MSGCTL2);
    idt_writel(&state, IDT_NT_GASADATA, 0x12);
    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_SWP0MSGCTL3);
    idt_writel(&state, IDT_NT_GASADATA, 0x13);
    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_SELINKDNSTS);
    idt_writel(&state, IDT_NT_GASADATA, 1);
    idt_writel(&state, IDT_NT_GASAADDR, IDT_SW_SELINKUPSTS);
    idt_writel(&state, IDT_NT_GASADATA, 1);

    idt_writel(&state, IDT_NT_NTMTBLADDR, 0x2);
    idt_writel(&state, IDT_NT_NTMTBLDATA, 0x55);

    idt_writel(&state, IDT_NT_MSGSTSMSK, 0xffffffff);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_MSGSTSMSK), ==, 0xffffffff);

    val = idt_readl(&state, IDT_NT_INDBELLMSK);
    idt_writel(&state, IDT_NT_INDBELLMSK, val);

    cleanup_idt_vm(&state);
}

static void test_idt_ntb_peer_vm1(void)
{
    IdtNtbState state = { 0 };

    setup_idt_vm(&state, "chr0", 1, false, true);
    g_assert_cmpuint(idt_readl(&state, IDT_NT_PCIELCAP), ==, 0x2U << 24);
    cleanup_idt_vm(&state);
}

static void test_idt_ntb_all_outmsgs(void)
{
    IdtNtbState s0 = { 0 }, s1 = { 0 };

    setup_idt_vm(&s0, "chr0", 0, true, true);
    setup_idt_vm(&s1, "chr1", 1, false, false);

    idt_writel(&s0, IDT_NT_MSGSTSMSK, 0);
    idt_writel(&s1, IDT_NT_MSGSTSMSK, 0);

    idt_writel(&s0, IDT_NT_OUTMSG1, 0x1111);
    idt_writel(&s0, IDT_NT_OUTMSG2, 0x2222);
    idt_writel(&s0, IDT_NT_OUTMSG3, 0x3333);
    g_usleep(50000);

    g_assert_cmpuint(idt_readl(&s1, IDT_NT_INMSG1), ==, 0x1111);
    g_assert_cmpuint(idt_readl(&s1, IDT_NT_INMSG0), ==, 0);

    cleanup_idt_vm(&s0);
    cleanup_idt_vm(&s1);
}

static void test_idt_ntb_invalid_writes(void)
{
    IdtNtbState state = { 0 };

    setup_idt_vm(&state, "chr0", 0, true, true);
    idt_writel(&state, 0xdeadbeef, 0x1);
    idt_writel(&state, IDT_NT_GASAADDR, 0xbad);
    idt_writel(&state, IDT_NT_GASADATA, 1);
    cleanup_idt_vm(&state);
}

static void cleanup(void)
{
    stop_global_server();
    if (peer0_ptr) {
        munmap(peer0_ptr, IDT_TEST_PEER_MEM_SIZE);
        peer0_ptr = NULL;
    }
    if (peer1_ptr) {
        munmap(peer1_ptr, IDT_TEST_PEER_MEM_SIZE);
        peer1_ptr = NULL;
    }
    if (peer_mem0) {
        unlink(peer_mem0);
        g_free(peer_mem0);
        peer_mem0 = NULL;
    }
    if (peer_mem1) {
        unlink(peer_mem1);
        g_free(peer_mem1);
        peer_mem1 = NULL;
    }
    if (tmpshmem) {
        munmap(tmpshmem, IDT_TEST_SHM_SIZE);
        tmpshmem = NULL;
    }
    if (tmpshm) {
        shm_unlink(tmpshm);
        g_free(tmpshm);
        tmpshm = NULL;
    }
    if (tmpserver) {
        g_unlink(tmpserver);
        g_free(tmpserver);
        tmpserver = NULL;
    }
    if (tmpdir) {
        g_rmdir(tmpdir);
        tmpdir = NULL;
    }
}

static void abrt_handler(void *data)
{
    cleanup();
}

static gchar *mktempshm(int size, int *fd)
{
    while (true) {
        gchar *name = g_strdup_printf("/qtest-idt-%u-%u",
                                      getpid(), g_test_rand_int());
        *fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL,
                       S_IRWXU | S_IRWXG | S_IRWXO);
        if (*fd > 0) {
            g_assert_cmpint(ftruncate(*fd, size), ==, 0);
            return name;
        }
        g_free(name);
        if (errno != EEXIST) {
            return NULL;
        }
    }
}

int main(int argc, char **argv)
{
    int ret, fd;
    gchar dir[] = "/tmp/idt-ntb-test.XXXXXX";

    g_test_init(&argc, &argv, NULL);
    qtest_add_abrt_handler(abrt_handler, NULL);

    tmpshm = mktempshm(IDT_TEST_SHM_SIZE, &fd);
    g_assert_nonnull(tmpshm);
    tmpshmem = mmap(0, IDT_TEST_SHM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, 0);
    g_assert(tmpshmem != MAP_FAILED);
    close(fd);

    g_assert_nonnull(g_mkdtemp(dir));
    tmpdir = dir;
    tmpserver = g_strconcat(tmpdir, "/server", NULL);
    setup_peer_shm_files();

    qtest_add_func("/idt-ntb-ivshmem/basic-registers",
                   test_idt_ntb_basic_registers);
    qtest_add_func("/idt-ntb-ivshmem/invalid-writes",
                   test_idt_ntb_invalid_writes);
    qtest_add_func("/idt-ntb-ivshmem/mtb-gas",
                   test_idt_ntb_mtb_gas);
    qtest_add_func("/idt-ntb-ivshmem/doorbell", test_idt_ntb_doorbell);
    qtest_add_func("/idt-ntb-ivshmem/messages", test_idt_ntb_messages);
    qtest_add_func("/idt-ntb-ivshmem/bar-config-memory",
                   test_idt_ntb_bar_config_and_memory);
    qtest_add_func("/idt-ntb-ivshmem/extended-registers",
                   test_idt_ntb_extended_registers);
    qtest_add_func("/idt-ntb-ivshmem/peer-vm1", test_idt_ntb_peer_vm1);
    qtest_add_func("/idt-ntb-ivshmem/all-outmsgs", test_idt_ntb_all_outmsgs);

    ret = g_test_run();
    cleanup();
    return ret;
}
