/*
 * QTest suite for the Switchtec NTB ivshmem PCI device.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include <sys/mman.h>

#include "contrib/ivshmem-server/ivshmem-server.h"
#include "libqos/libqos-pc.h"
#include "libqtest.h"

#define SWITCHTEC_VENDOR_ID       0x11f8
#define SWITCHTEC_DEVICE_ID       0x8531
#define SWITCHTEC_TEST_SHM_SIZE   (1U << 20)

#define GAS_SYS_INFO              0x2000
#define SYS_INFO_GEN3_PART_ID     0x18
#define GAS_NTB                   0x10000
#define NTB_DBMSG                 0x64000
#define DBMSG_SIZE                0x4000
#define DBMSG_ODB                 0x1000
#define DBMSG_ODB_MASK            0x1008
#define DBMSG_IDB                 0x1010
#define DBMSG_IDB_MASK            0x1018
#define DBMSG_VEC_MAP             0x1020
#define DBMSG_MSG_MAP             0x1060
#define DBMSG_OMSG                0x1068
#define DBMSG_IMSG                0x1088

typedef struct ServerThread {
    GThread *thread;
    IvshmemServer *server;
    int pipe[2];
} ServerThread;

typedef struct SwitchtecTestState {
    QOSState *qs;
    QPCIDevice *dev;
    QPCIBar gas;
    unsigned partition;
} SwitchtecTestState;

static char *tmpdir;
static char *tmpserver;
static char *tmpshm;
static void *tmpshmem;
static IvshmemServer server;
static ServerThread server_thread_state;
static bool server_running;

static void *server_thread(void *data)
{
    ServerThread *thread = data;

    while (true) {
        fd_set fds;
        int maxfd;
        int ret;

        FD_ZERO(&fds);
        FD_SET(thread->pipe[0], &fds);
        maxfd = thread->pipe[0] + 1;
        ivshmem_server_get_fds(thread->server, &fds, &maxfd);

        ret = select(maxfd, &fds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (FD_ISSET(thread->pipe[0], &fds)) {
            break;
        }
        if (ivshmem_server_handle_fds(thread->server, &fds, maxfd) < 0) {
            break;
        }
    }
    return NULL;
}

static void start_server(void)
{
    GError *err = NULL;
    int ret;

    ret = ivshmem_server_init(&server, tmpserver, tmpshm, true,
                              SWITCHTEC_TEST_SHM_SIZE, 4, false);
    g_assert(ret == 0);
    ret = ivshmem_server_start(&server);
    g_assert(ret == 0);
    g_unix_open_pipe(server_thread_state.pipe, FD_CLOEXEC, &err);
    g_assert_no_error(err);
    server_thread_state.server = &server;
    server_thread_state.thread = g_thread_new("switchtec-ivshmem-server",
                                               server_thread,
                                               &server_thread_state);
    server_running = true;
}

static void stop_server(void)
{
    if (!server_running) {
        return;
    }

    g_assert_cmpint(qemu_write_full(server_thread_state.pipe[1], "q", 1),
                    ==, 1);
    g_thread_join(server_thread_state.thread);
    server_thread_state.thread = NULL;
    ivshmem_server_close(&server);
    close(server_thread_state.pipe[0]);
    close(server_thread_state.pipe[1]);
    server_running = false;
}

static void save_device(QPCIDevice *dev, int devfn, void *data)
{
    QPCIDevice **result = data;

    *result = dev;
}

static void setup_vm(SwitchtecTestState *s, unsigned partition, bool master)
{
    QPCIDevice *dev = NULL;
    g_autofree char *cmd = NULL;
    uint64_t bar_size;

    if (!server_running) {
        start_server();
    }

    cmd = g_strdup_printf(
        "-chardev socket,id=chr%u,path=%s "
        "-device switchtec-ntb-ivshmem,chardev=chr%u,vectors=4,"
        "partition=%u,master=%s",
        partition, tmpserver, partition, partition, master ? "on" : "off");
    s->qs = qtest_pc_boot("%s", cmd);
    qpci_device_foreach(s->qs->pcibus, SWITCHTEC_VENDOR_ID,
                        SWITCHTEC_DEVICE_ID, save_device, &dev);
    g_assert_nonnull(dev);
    s->dev = dev;
    s->gas = qpci_iomap(dev, 0, &bar_size);
    g_assert_cmpuint(bar_size, ==, 2U * 1024 * 1024);
    qpci_device_enable(dev);
    s->partition = partition;
}

static void cleanup_vm(SwitchtecTestState *s)
{
    g_clear_pointer(&s->dev, g_free);
    if (s->qs) {
        qtest_shutdown(s->qs);
        s->qs = NULL;
    }
}

static uint64_t dbmsg_base(const SwitchtecTestState *s)
{
    return GAS_NTB + NTB_DBMSG + s->partition * DBMSG_SIZE;
}

static void test_gas_layout(void)
{
    SwitchtecTestState s = { 0 };
    uint64_t base;

    setup_vm(&s, 0, true);
    base = dbmsg_base(&s);

    g_assert_cmpuint(qpci_io_readb(s.dev, s.gas,
                                  GAS_SYS_INFO + SYS_INFO_GEN3_PART_ID), ==, 0);
    g_assert_cmpuint(qpci_io_readb(s.dev, s.gas, GAS_NTB), ==, 2);
    g_assert_cmpuint(qpci_io_readb(s.dev, s.gas, GAS_NTB + 1), ==, 0);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.gas, GAS_NTB + 4), ==, 3);

    qpci_io_writeq(s.dev, s.gas, base + DBMSG_ODB_MASK,
                   0x1122334455667788ULL);
    g_assert_cmphex(qpci_io_readq(s.dev, s.gas, base + DBMSG_ODB_MASK),
                    ==, 0x1122334455667788ULL);
    qpci_io_writel(s.dev, s.gas, base + DBMSG_VEC_MAP, 0x03020100);
    g_assert_cmphex(qpci_io_readl(s.dev, s.gas, base + DBMSG_VEC_MAP),
                    ==, 0x03020100);

    cleanup_vm(&s);
    stop_server();
}

static void test_doorbells(void)
{
    SwitchtecTestState a = { 0 };
    SwitchtecTestState b = { 0 };
    uint64_t a_base;
    uint64_t b_base;

    setup_vm(&a, 0, true);
    setup_vm(&b, 1, false);
    a_base = dbmsg_base(&a);
    b_base = dbmsg_base(&b);

    qpci_io_writeq(a.dev, a.gas, a_base + DBMSG_ODB_MASK, 0x100000004ULL);
    qpci_io_writeq(b.dev, b.gas, b_base + DBMSG_IDB_MASK, 0x100000004ULL);
    qpci_io_writeq(a.dev, a.gas, a_base + DBMSG_ODB, 0x100000004ULL);
    g_assert_cmphex(qpci_io_readq(b.dev, b.gas, b_base + DBMSG_IDB),
                    ==, 0x100000004ULL);

    qpci_io_writeq(b.dev, b.gas, b_base + DBMSG_IDB, 4);
    g_assert_cmphex(qpci_io_readq(b.dev, b.gas, b_base + DBMSG_IDB),
                    ==, 0x100000000ULL);
    qpci_io_writeq(b.dev, b.gas, b_base + DBMSG_IDB, 0x100000000ULL);
    g_assert_cmphex(qpci_io_readq(b.dev, b.gas, b_base + DBMSG_IDB), ==, 0);

    cleanup_vm(&a);
    cleanup_vm(&b);
    stop_server();
}

static void test_messages(void)
{
    SwitchtecTestState a = { 0 };
    SwitchtecTestState b = { 0 };
    uint64_t a_base;
    uint64_t b_base;
    uint64_t imsg;

    setup_vm(&a, 0, true);
    setup_vm(&b, 1, false);
    a_base = dbmsg_base(&a);
    b_base = dbmsg_base(&b);

    qpci_io_writel(a.dev, a.gas, a_base + DBMSG_MSG_MAP, 0x03020100);
    qpci_io_writeb(b.dev, b.gas, b_base + DBMSG_VEC_MAP + 62, 2);
    qpci_io_writel(a.dev, a.gas, a_base + DBMSG_OMSG + 16, 0xfeedface);

    imsg = qpci_io_readq(b.dev, b.gas, b_base + DBMSG_IMSG + 16);
    g_assert_cmphex((uint32_t)imsg, ==, 0xfeedface);
    g_assert_cmpuint((imsg >> 32) & 1, ==, 1);
    g_assert_cmpuint((imsg >> 48) & 0xff, ==, 0);

    qpci_io_writeb(b.dev, b.gas, b_base + DBMSG_IMSG + 20, 1);
    imsg = qpci_io_readq(b.dev, b.gas, b_base + DBMSG_IMSG + 16);
    g_assert_cmpuint((imsg >> 32) & 1, ==, 0);

    cleanup_vm(&a);
    cleanup_vm(&b);
    stop_server();
}

static char *make_shm(int size, int *fd)
{
    while (true) {
        char *name = g_strdup_printf("/qtest-switchtec-%u-%u",
                                     getpid(), g_test_rand_int());

        *fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
        if (*fd >= 0) {
            g_assert_cmpint(ftruncate(*fd, size), ==, 0);
            return name;
        }
        g_free(name);
        g_assert_cmpint(errno, ==, EEXIST);
    }
}

static void cleanup(void)
{
    stop_server();
    if (tmpshmem) {
        munmap(tmpshmem, SWITCHTEC_TEST_SHM_SIZE);
        tmpshmem = NULL;
    }
    if (tmpshm) {
        shm_unlink(tmpshm);
        g_clear_pointer(&tmpshm, g_free);
    }
    if (tmpserver) {
        g_unlink(tmpserver);
        g_clear_pointer(&tmpserver, g_free);
    }
    if (tmpdir) {
        g_rmdir(tmpdir);
        tmpdir = NULL;
    }
}

static void abort_handler(void *data)
{
    cleanup();
}

int main(int argc, char **argv)
{
    char dir[] = "/tmp/switchtec-ntb-test.XXXXXX";
    int fd;
    int ret;

    g_test_init(&argc, &argv, NULL);
    qtest_add_abrt_handler(abort_handler, NULL);

    tmpshm = make_shm(SWITCHTEC_TEST_SHM_SIZE, &fd);
    tmpshmem = mmap(NULL, SWITCHTEC_TEST_SHM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, 0);
    g_assert(tmpshmem != MAP_FAILED);
    close(fd);
    g_assert_nonnull(g_mkdtemp(dir));
    tmpdir = dir;
    tmpserver = g_strconcat(tmpdir, "/server", NULL);

    qtest_add_func("/switchtec-ntb-ivshmem/gas-layout", test_gas_layout);
    qtest_add_func("/switchtec-ntb-ivshmem/doorbells", test_doorbells);
    qtest_add_func("/switchtec-ntb-ivshmem/messages", test_messages);

    ret = g_test_run();
    cleanup();
    return ret;
}
