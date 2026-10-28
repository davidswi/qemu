/*
 * QEMU IO Bridge
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Creates an IO bridge between two QEMU instances where messages can be passed
 * between the parent and child instances via messages queues and shared memory.
 *
 * The parent is usually the QEMU instance that runs the operating system (like
 * Linux) on the application processor whilst the child is typically a smaller
 * processor running an embedded firmware. The parent and child do not need to
 * be the same architecture but are expected to communicate over a local bus.
 */

#include "qemu/osdep.h"
#include "mqueue.h"
#include "qemu/io-bridge.h"
#include <glib.h>

/* set to 1 to enable debug */
static int io_bridge_debug = 0;

/* we can either be parent or child */
#define ROLE_NONE    0
#define ROLE_PARENT  1
#define ROLE_CHILD   2

static int role = ROLE_NONE;

#define QEMU_IO_MAX_MSGS    8
#define QEMU_IO_MAX_MSG_SIZE    128
#define QEMU_IO_MAX_SHM_REGIONS    32

#define NAME_SIZE       64

struct io_shm {
    int fd;
    void *addr;
    char name[NAME_SIZE];
    size_t size;
};

struct io_mq {
    char mq_name[NAME_SIZE];
    char thread_name[NAME_SIZE];
    struct mq_attr mqattr;
    mqd_t mqdes;
};

struct io_bridge {
    struct io_mq parent;
    struct io_mq child;
    GThread *io_thread;
    int (*cb)(void *data, struct qemu_io_msg *msg);
    struct io_shm shm[QEMU_IO_MAX_SHM_REGIONS];
    void *data;
};

static struct io_bridge _iob;
static int _id = 0;

/* parent reader Q */
static gpointer parent_reader_thread(gpointer data)
{
    struct io_bridge *io = data;
    char buf[QEMU_IO_MAX_MSG_SIZE];
    ssize_t ret;
    int i;

    ret = mq_getattr(io->parent.mqdes, &io->parent.mqattr);
    if (ret < 0) {
        perror("bridge-io: mq_getattr failed");
        return NULL;
    }
    if (io_bridge_debug) {
        fprintf(stdout, "bridge-io: %zd messages are currently on parent queue.\n",
                (ssize_t)io->parent.mqattr.mq_curmsgs);
    }

    /* flush old messages here */
    for (i = 0; i < io->parent.mqattr.mq_curmsgs; i++) {
        ret = mq_receive(io->parent.mqdes, buf, QEMU_IO_MAX_MSG_SIZE, NULL);
        if (ret > 0) {
            struct qemu_io_msg *hdr = (struct qemu_io_msg *)buf;

            if (io_bridge_debug) {
                fprintf(stdout, "bridge-io: flushed %d type %zd msg %d\n",
                        hdr->id, (ssize_t)hdr->size, hdr->msg);
            }
        }
    }

    while ((ret = mq_receive(io->parent.mqdes, buf, QEMU_IO_MAX_MSG_SIZE, NULL)) != -1) {
        struct qemu_io_msg *hdr = (struct qemu_io_msg *)buf;

        if (io_bridge_debug) {
            fprintf(stdout, "bridge-io: msg recv %d type %zd msg %d\n",
                    hdr->id, (ssize_t)hdr->size, hdr->msg);
        }

        if (io->cb) {
            io->cb(io->data, hdr);
        }
    }

    return NULL;
}

/* child reader Q */
static gpointer child_reader_thread(gpointer data)
{
    struct io_bridge *io = data;
    char buf[QEMU_IO_MAX_MSG_SIZE];
    ssize_t ret;
    int i;

    ret = mq_getattr(io->child.mqdes, &io->child.mqattr);
    if (ret < 0) {
        perror("bridge-io: mq_getattr failed");
        return NULL;
    }
    if (io_bridge_debug) {
        fprintf(stdout, "bridge-io: %zd messages are currently on child queue.\n",
                (ssize_t)io->child.mqattr.mq_curmsgs);
    }

    /* flush old messages here */
    for (i = 0; i < io->child.mqattr.mq_curmsgs; i++) {
        ret = mq_receive(io->child.mqdes, buf, QEMU_IO_MAX_MSG_SIZE, NULL);
        if (ret > 0) {
            struct qemu_io_msg *hdr = (struct qemu_io_msg *)buf;

            if (io_bridge_debug) {
                fprintf(stdout, "bridge-io: flushed %d type %zd msg %d\n",
                        hdr->id, (ssize_t)hdr->size, hdr->msg);
            }
        }
    }

    while ((ret = mq_receive(io->child.mqdes, buf, QEMU_IO_MAX_MSG_SIZE, NULL)) != -1) {
        struct qemu_io_msg *hdr = (struct qemu_io_msg *)buf;

        if (io_bridge_debug) {
            fprintf(stdout, "bridge-io: msg recv %d type %zd msg %d\n",
                    hdr->id, (ssize_t)hdr->size, hdr->msg);
        }

        if (io->cb) {
            io->cb(io->data, hdr);
        }
    }

    return NULL;
}

static int mq_init(const char *name, struct io_bridge *io)
{
    int ret = 0;

    memset(&io->parent.mqattr, 0, sizeof(io->parent.mqattr));
    memset(&io->child.mqattr, 0, sizeof(io->child.mqattr));

    io->parent.mqattr.mq_maxmsg = QEMU_IO_MAX_MSGS;
    io->parent.mqattr.mq_msgsize = QEMU_IO_MAX_MSG_SIZE;
    io->parent.mqattr.mq_flags = 0;
    io->parent.mqattr.mq_curmsgs = 0;

    io->child.mqattr.mq_maxmsg = QEMU_IO_MAX_MSGS;
    io->child.mqattr.mq_msgsize = QEMU_IO_MAX_MSG_SIZE;
    io->child.mqattr.mq_flags = 0;
    io->child.mqattr.mq_curmsgs = 0;

    if (role == ROLE_PARENT) {

        /* Host */

        g_snprintf(io->parent.thread_name, NAME_SIZE, "io-bridge-%s", name);
        io->io_thread = g_thread_new(io->parent.thread_name,
                                     parent_reader_thread, io);

        /* parent Rx Q */
        g_snprintf(io->parent.mq_name, NAME_SIZE, "/qemu-io-parent-%s", name);
        io->parent.mqdes = mq_open(io->parent.mq_name, O_RDONLY,
                                   0664, &io->parent.mqattr);
        if (io->parent.mqdes < 0) {
            fprintf(stderr, "failed to open parent Rx queue %s: %d, has DSP been started?\n",
                    io->parent.mq_name, errno);
            ret = -errno;
        }

        /* parent Tx Q */
        g_snprintf(io->child.mq_name, NAME_SIZE, "/qemu-io-child-%s", name);
        io->child.mqdes = mq_open(io->child.mq_name, O_WRONLY,
                                  0664, &io->child.mqattr);
        if (io->child.mqdes < 0) {
            fprintf(stderr, "failed to open parent Tx queue %s: %d, has DSP been started?\n",
                    io->child.mq_name, errno);
            ret = -errno;
        }

    } else {

        /* Child (e.g., DSP firmware) */

        g_snprintf(io->child.thread_name, NAME_SIZE, "io-bridge-%s", name);
        io->io_thread = g_thread_new(io->child.thread_name,
                                     child_reader_thread, io);

        /* child Rx Q */
        g_snprintf(io->child.mq_name, NAME_SIZE, "/qemu-io-child-%s", name);
        mq_unlink(io->child.mq_name);
        io->child.mqdes = mq_open(io->child.mq_name, O_RDONLY | O_CREAT,
                                  0664, &io->child.mqattr);
        if (io->child.mqdes < 0) {
            fprintf(stderr, "failed to open child Rx queue: %d\n", errno);
            ret = -errno;
        }

        /* child Tx Q */
        g_snprintf(io->parent.mq_name, NAME_SIZE, "/qemu-io-parent-%s", name);
        mq_unlink(io->parent.mq_name);
        io->parent.mqdes = mq_open(io->parent.mq_name, O_WRONLY | O_CREAT,
                                   0664, &io->parent.mqattr);
        if (io->parent.mqdes < 0) {
            fprintf(stderr, "failed to open child Tx queue: %d\n", errno);
            ret = -errno;
        }

    }

    if (ret == 0 && io_bridge_debug) {
        fprintf(stdout, "bridge-io-mq: added %s\n", io->parent.mq_name);
        fprintf(stdout, "bridge-io-mq: added %s\n", io->child.mq_name);
    }
    return ret;
}

int qemu_io_register_parent(const char *name,
                            int (*cb)(void *data, struct qemu_io_msg *msg),
                            void *data)
{
    if (role != ROLE_NONE) {
        return -EINVAL;
    }

    role = ROLE_PARENT;
    _iob.cb = cb;
    _iob.data = data;

    memset(&_iob, 0, sizeof(_iob));

    return mq_init(name, &_iob);
}

int qemu_io_register_child(const char *name,
                           int (*cb)(void *data, struct qemu_io_msg *msg),
                           void *data)
{
    int ret = 0;

    if (role != ROLE_NONE) {
        return -EINVAL;
    }

    role = ROLE_CHILD;
    _iob.cb = cb;
    _iob.data = data;

    memset(&_iob, 0, sizeof(_iob));

    ret = mq_init(name, &_iob);

    return ret;
}

int qemu_io_register_shm(const char *rname, int region, size_t size, void **addr)
{
    char *name;
    int fd, ret;
    void *a;

    /* check that region is not already in use */
    if (region < 0 || region >= QEMU_IO_MAX_SHM_REGIONS) {
        return -EINVAL;
    }
    if (_iob.shm[region].fd != 0) {
        return -EBUSY;
    }

    name = _iob.shm[region].name;
    g_snprintf(name, NAME_SIZE, "qemu-bridge-%s", rname);

    fd = shm_open(name, O_RDWR | O_CREAT, 0664);
    if (fd < 0) {
        fprintf(stderr, "bridge-io: cant open SHM %d\n", errno);
        return -errno;
    }

    ret = ftruncate(fd, size);
    if (ret < 0) {
        fprintf(stderr, "bridge-io: cant truncate %d\n", errno);
        shm_unlink(name);
        close(fd);
        return -errno;
    }

    a = mmap(*addr ? *addr : MAP_FAILED, size, PROT_READ | PROT_WRITE,
             MAP_SHARED, fd, 0);
    if (a == MAP_FAILED) {
        fprintf(stderr, "bridge-io: cant mmap %d\n", errno);
        shm_unlink(name);
        close(fd);
        return -errno;
    }

    if (io_bridge_debug) {
        fprintf(stdout, "bridge-io: %s fd %d region %d at %p allocated %zu bytes\n",
                name, fd, region, a, size);
    }
    _iob.shm[region].fd = fd;
    _iob.shm[region].addr = a;
    _iob.shm[region].size = size;
    *addr = a;

    return 0;
}

#define PAGE_SIZE 4096

int qemu_io_sync(int region, unsigned int offset, size_t length)
{
    if (region < 0 || region >= QEMU_IO_MAX_SHM_REGIONS) {
        return -EINVAL;
    }

    /* check that region is in use */
    if (_iob.shm[region].fd == 0) {
        return -EINVAL;
    }

    /* align offset to page size */
    offset -= (offset % PAGE_SIZE);

    return msync(_iob.shm[region].addr + offset, length, MS_SYNC | MS_INVALIDATE);
}

int qemu_io_send_msg(struct qemu_io_msg *msg)
{
    int ret;

    msg->id = _id++;

    if (role == ROLE_PARENT) {
        ret = mq_send(_iob.child.mqdes, (const char *)msg, msg->size, 0);
    } else {
        ret = mq_send(_iob.parent.mqdes, (const char *)msg, msg->size, 0);
    }

    if (io_bridge_debug) {
        fprintf(stdout, "bridge-io: msg send: %d type %d msg %d size %zd ret %d\n",
                msg->id, msg->type, msg->msg, (ssize_t)msg->size, ret);
    }
    if (ret < 0) {
        fprintf(stderr, "bridge-io: msg send failed %d\n", -errno);
    }

    return ret;
}

int qemu_io_send_msg_reply(struct qemu_io_msg *msg)
{
    int ret;

    if (role == ROLE_PARENT) {
        ret = mq_send(_iob.child.mqdes, (const char *)msg, msg->size, 0);
    } else {
        ret = mq_send(_iob.parent.mqdes, (const char *)msg, msg->size, 0);
    }

    if (io_bridge_debug) {
        fprintf(stdout, "bridge-io: repmsg send: %d type %d msg %d size %zd ret %d\n",
                msg->id, msg->type, msg->msg, (ssize_t)msg->size, ret);
    }
    if (ret < 0) {
        fprintf(stderr, "bridge-io: rmsg send failed %d\n", -errno);
    }

    return ret;
}

void qemu_io_free(void)
{
    int i;

    for (i = 0; i < QEMU_IO_MAX_SHM_REGIONS; i++) {
        if (_iob.shm[i].fd != 0) {
            munmap(_iob.shm[i].addr, _iob.shm[i].size);
            shm_unlink(_iob.shm[i].name);
            close(_iob.shm[i].fd);
            _iob.shm[i].fd = 0;
        }
    }
    mq_unlink(_iob.parent.mq_name);
    mq_unlink(_iob.child.mq_name);
}

void qemu_io_free_shm(int region)
{
    int err;

    if ((region >= 0) && (region < QEMU_IO_MAX_SHM_REGIONS) && _iob.shm[region].fd != 0) {
        err = munmap(_iob.shm[region].addr, _iob.shm[region].size);
        if (err < 0) {
            fprintf(stderr, "bridge-io: munmap failed %d\n", errno);
        }

        /* client or host can unlink this, so it gets done twice */
        shm_unlink(_iob.shm[region].name);
        close(_iob.shm[region].fd);
        _iob.shm[region].fd = 0;
    }
}
