/*
 * Copyright (C) 2018 Jolla Ltd.
 * Copyright (C) 2018 Slava Monich <slava.monich@jolla.com>
 *
 * You may use this file under the terms of BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the name of Jolla Ltd nor the names of its contributors may
 *      be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "gbinder_driver.h"
#include "gbinder_buffer_p.h"
#include "gbinder_handler.h"
#include "gbinder_io.h"
#include "gbinder_local_object_p.h"
#include "gbinder_local_reply_p.h"
#include "gbinder_local_request_p.h"
#include "gbinder_object_registry.h"
#include "gbinder_output_data.h"
#include "gbinder_remote_object_p.h"
#include "gbinder_remote_reply_p.h"
#include "gbinder_remote_request_p.h"
#include "gbinder_rpc_protocol.h"
#include "gbinder_system.h"
#include "gbinder_writer.h"
#include "gbinder_log.h"

#include <gutil_intarray.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

/* Log module */
GLOG_MODULE_DEFINE("gbinder");

/* BINDER_VM_SIZE copied from native/libs/binder/ProcessState.cpp */
#define BINDER_VM_SIZE ((1024*1024) - sysconf(_SC_PAGE_SIZE)*2)

#define BINDER_MAX_REPLY_SIZE (256)

/* ioctl code (the only one we really need here) */
#define BINDER_VERSION _IOWR('b', 9, gint32)

/* OK, one more */
#define BINDER_SET_MAX_THREADS _IOW('b', 5, guint32)

#define DEFAULT_MAX_BINDER_THREADS (0)

struct gbinder_driver {
    gint refcount;
    int fd;
    void* vm;
    gsize vmsize;
    char* dev;
    const GBinderIo* io;
    const GBinderRpcProtocol* protocol;
};

typedef struct gbinder_io_read_buf {
    GBinderIoBuf buf;
    guint8 data[GBINDER_IO_READ_BUFFER_SIZE];
} GBinderIoReadBuf;

/*==========================================================================*
 * Implementation
 *==========================================================================*/

#if GUTIL_LOG_VERBOSE
static
void
gbinder_driver_verbose_dump(
    const char mark,
    uintptr_t ptr,
    gsize len)
{
    /* The caller should make sure that verbose log is enabled */
    if (len > 0 && GLOG_ENABLED(GLOG_LEVEL_VERBOSE)) {
        char prefix[3];
        char line[GUTIL_HEXDUMP_BUFSIZE];

        prefix[0] = mark;
        prefix[1] = ' ';
        prefix[2] = 0;
        while (len > 0) {
            const guint dumped = gutil_hexdump(line, (void*)ptr, len);

            GVERBOSE("%s%s", prefix, line);
            len -= dumped;
            ptr += dumped;
            prefix[0] = ' ';
        }
    }
}

GBINDER_INLINE_FUNC
void
gbinder_driver_verbose_dump_bytes(
    const char mark,
    const GByteArray* bytes)
{
    gbinder_driver_verbose_dump(mark, (uintptr_t)bytes->data, bytes->len);
}

static
void
gbinder_driver_verbose_transaction_data(
    const char* name,
    const GBinderIoTxData* tx)
{
    if (GLOG_ENABLED(GLOG_LEVEL_VERBOSE)) {
        if (tx->objects) {
            guint n = 0;
            while (tx->objects[n]) n++;
            if (tx->status) {
                GVERBOSE("> %s %d (%u bytes, %u objects)", name, tx->status,
                    (guint)tx->size, n);
            } else {
                GVERBOSE("> %s (%u bytes, %u objects)", name,
                    (guint)tx->size, n);
            }
        } else {
            if (tx->status) {
                GVERBOSE("> %s %d (%u bytes)", name, tx->status,
                    (guint)tx->size);
            } else {
                GVERBOSE("> %s (%u bytes)", name, (guint)tx->size);
            }
        }
    }
}

#else
#  define gbinder_driver_verbose_dump(x,y,z) GLOG_NOTHING
#  define gbinder_driver_verbose_dump_bytes(x,y) GLOG_NOTHING
#  define gbinder_driver_verbose_transaction_data(x,y) GLOG_NOTHING
#endif /* GUTIL_LOG_VERBOSE */

static
int
gbinder_driver_write(
    GBinderDriver* self,
    GBinderIoBuf* buf)
{
    int err = (-EAGAIN);

    while (err == (-EAGAIN)) {
        gbinder_driver_verbose_dump('<',
            buf->ptr +  buf->consumed,
            buf->size - buf->consumed);
        GVERBOSE_("%u/%u", (guint)buf->consumed, (guint)buf->size);
        err = self->io->write_read(self->fd, buf, NULL);
        GVERBOSE_("%u/%u err %d", (guint)buf->consumed, (guint)buf->size, err);
    }
    return err;
}

static
int
gbinder_driver_write_read(
    GBinderDriver* self,
    GBinderIoBuf* write,
    GBinderIoBuf* read)
{
    int err = (-EAGAIN);

    while (err == (-EAGAIN)) {
#if GUTIL_LOG_VERBOSE
        const gsize were_consumed = read ? read->consumed : 0;
        if (GLOG_ENABLED(GLOG_LEVEL_VERBOSE)) {
            if (write) {
                gbinder_driver_verbose_dump('<',
                    write->ptr +  write->consumed,
                    write->size - write->consumed);
            }
            GVERBOSE_("write %u/%u read %u/%u",
              (guint)(write ? write->consumed : 0),
              (guint)(write ? write->size : 0),
              (guint)(read ? read->consumed : 0),
              (guint)(read ? read->size : 0));
        }
#endif /* GUTIL_LOG_VERBOSE */
        err = self->io->write_read(self->fd, write, read);
#if GUTIL_LOG_VERBOSE
        if (GLOG_ENABLED(GLOG_LEVEL_VERBOSE)) {
            GVERBOSE_("write %u/%u read %u/%u err %d",
              (guint)(write ? write->consumed : 0),
              (guint)(write ? write->size : 0),
              (guint)(read ? read->consumed : 0),
              (guint)(read ? read->size : 0), err);
            if (read) {
                gbinder_driver_verbose_dump('>',
                    read->ptr + were_consumed,
                    read->consumed - were_consumed);
            }
        }
#endif /* GUTIL_LOG_VERBOSE */
    }
    return err;
}

static
gboolean
gbinder_driver_cmd(
    GBinderDriver* self,
    guint32 cmd)
{
    GBinderIoBuf write;

    memset(&write, 0, sizeof(write));
    write.ptr = (uintptr_t)&cmd;
    write.size = sizeof(cmd);
    return gbinder_driver_write(self, &write) >= 0;
}

static
gboolean
gbinder_driver_cmd_int32(
    GBinderDriver* self,
    guint32 cmd,
    guint32 param)
{
    GBinderIoBuf write;
    guint32 data[2];

    data[0] = cmd;
    data[1] = param;
    memset(&write, 0, sizeof(write));
    write.ptr = (uintptr_t)data;
    write.size = sizeof(data);
    return gbinder_driver_write(self, &write) >= 0;
}

static
gboolean
gbinder_driver_cmd_data(
    GBinderDriver* self,
    guint32 cmd,
    const void* payload,
    void* buf)
{
    GBinderIoBuf write;
    guint32* data = buf;

    data[0] = cmd;
    memcpy(data + 1, payload, _IOC_SIZE(cmd));
    memset(&write, 0, sizeof(write));
    write.ptr = (uintptr_t)buf;
    write.size = 4 + _IOC_SIZE(cmd);

    return gbinder_driver_write(self, &write) >= 0;
}

static
gboolean
gbinder_driver_death_notification(
    GBinderDriver* self,
    guint32 cmd,
    GBinderRemoteObject* obj)
{
    if (G_LIKELY(obj)) {
        GBinderIoBuf write;
        guint8 buf[4 + GBINDER_MAX_DEATH_NOTIFICATION_SIZE];
        guint32* data = (guint32*)buf;

        data[0] = cmd;
        memset(&write, 0, sizeof(write));
        write.ptr = (uintptr_t)buf;
        write.size = 4 + self->io->encode_death_notification(data + 1, obj);

        return gbinder_driver_write(self, &write) >= 0;
    }
    return FALSE;
}

static
void
gbinder_driver_read_init(
    GBinderIoReadBuf* rb)
{
    rb->buf.ptr = (uintptr_t)(rb->data);
    rb->buf.size = sizeof(rb->data);
    rb->buf.consumed = 0;

    /*
     * It shouldn't be necessary to zero-initialize the buffer but
     * valgrind complains about access to uninitialised data if we
     * don't do so. Oh well...
     */
    memset(rb->data, 0, sizeof(rb->data));
}

static
guint32
gbinder_driver_next_command(
    GBinderDriver* self,
    const GBinderIoBuf* buf)
{
    const size_t remaining = buf->size - buf->consumed;
    guint32 cmd = 0;

    if (remaining >= sizeof(cmd)) {
        int datalen;
        /* The size of the data to follow is encoded in the command code */
        cmd = *(guint32*)(buf->ptr + buf->consumed);
        datalen = _IOC_SIZE(cmd);
        if (remaining >= sizeof(cmd) + datalen) {
            return cmd;
        }
    }
    return 0;
}

static
gboolean
gbinder_driver_reply_status(
    GBinderDriver* self,
    gint32 status)
{
    const GBinderIo* io = self->io;
    GBinderIoBuf write;
    guint8 buf[sizeof(guint32) + GBINDER_MAX_BC_TRANSACTION_SIZE];
    guint8* ptr = buf;
    const guint32* code = &io->bc.reply;

    /* Command (this has to be slightly convoluted to avoid breaking
     * strict-aliasing rules.. oh well) */
    memcpy(buf, code, sizeof(*code));
    ptr += sizeof(*code);

    /* Data */
    ptr += io->encode_status_reply(ptr, &status);

    GVERBOSE("< BC_REPLY (%d)", status);
    memset(&write, 0, sizeof(write));
    write.ptr = (uintptr_t)buf;
    write.size = ptr - buf;
    return gbinder_driver_write(self, &write) >= 0;
}

static
gboolean
gbinder_driver_reply_data(
    GBinderDriver* self,
    GBinderOutputData* data)
{
    GBinderIoBuf write;
    const GBinderIo* io = self->io;
    const gsize extra_buffers = gbinder_output_data_buffers_size(data);
    guint8 buf[GBINDER_MAX_BC_TRANSACTION_SG_SIZE + sizeof(guint32)];
    guint32* cmd = (guint32*)buf;
    guint len = sizeof(*cmd);
    int status;
    GUtilIntArray* offsets = gbinder_output_data_offsets(data);
    void* offsets_buf = NULL;

    /* Build BC_TRANSACTION */
    if (extra_buffers) {
        GVERBOSE("< BC_REPLY_SG %u bytes", (guint)extra_buffers);
        gbinder_driver_verbose_dump_bytes(' ', data->bytes);
        *cmd = io->bc.reply_sg;
        len += io->encode_transaction_sg(buf + len, 0, 0, data->bytes, 0,
            offsets, &offsets_buf, extra_buffers);
    } else {
        GVERBOSE("< BC_REPLY");
        gbinder_driver_verbose_dump_bytes(' ', data->bytes);
        *cmd = io->bc.reply;
        len += io->encode_transaction(buf + len, 0, 0, data->bytes, 0,
            offsets, &offsets_buf);
    }

#if 0 /* GUTIL_LOG_VERBOSE */
    if (offsets && offsets->count) {
        gbinder_driver_verbose_dump('<', (uintptr_t)offsets_buf,
            offsets->count * io->pointer_size);
    }
#endif /* GUTIL_LOG_VERBOSE */

    /* Write it */
    write.ptr = (uintptr_t)buf;
    write.size = len;
    write.consumed = 0;
    status = gbinder_driver_write(self, &write) >= 0;

    g_free(offsets_buf);
    return status >= 0;
}

static
void
gbinder_driver_handle_transaction(
    GBinderDriver* self,
    GBinderObjectRegistry* reg,
    GBinderHandler* h,
    const void* data)
{
    GBinderLocalReply* reply = NULL;
    GBinderRemoteRequest* req;
    GBinderIoTxData tx;
    GBinderLocalObject* obj;
    const char* iface;
    int status = -EBADMSG;

    self->io->decode_transaction_data(data, &tx);
    gbinder_driver_verbose_transaction_data("BR_TRANSACTION", &tx);
    req = gbinder_remote_request_new(reg, self->protocol, tx.pid, tx.euid);
    obj = gbinder_object_registry_get_local(reg, tx.target);

    /* Transfer data ownership to the request */
    if (tx.data && tx.size) {
        gbinder_driver_verbose_dump(' ', (uintptr_t)tx.data, tx.size);
        gbinder_remote_request_set_data(req,
            gbinder_buffer_new(self, tx.data, tx.size),
            tx.objects);
    } else {
        g_free(tx.objects);
        gbinder_driver_free_buffer(self, tx.data);
    }

    /* Process the transaction (NULL is properly handled) */
    iface = gbinder_remote_request_interface(req);
    switch (gbinder_local_object_can_handle_transaction(obj, iface, tx.code)) {
    case GBINDER_LOCAL_TRANSACTION_LOOPER:
        reply = gbinder_local_object_handle_looper_transaction(obj, req,
            tx.code, tx.flags, &status);
        break;
    case GBINDER_LOCAL_TRANSACTION_SUPPORTED:
        reply = gbinder_handler_transact(h, obj, req, tx.code, tx.flags,
            &status);
        break;
    default:
        GWARN("Unhandled transaction 0x%08x", tx.code);
        break;
    }

    /* No reply for one-way transactions */
    if (!(tx.flags & GBINDER_TX_FLAG_ONEWAY)) {
        if (reply) {
            gbinder_driver_reply_data(self, gbinder_local_reply_data(reply));
        } else {
            gbinder_driver_reply_status(self, status);
        }
    }

    /* Free the data allocated for the transaction */
    gbinder_remote_request_unref(req);
    gbinder_local_reply_unref(reply);
    gbinder_local_object_unref(obj);
}

static
void
gbinder_driver_handle_command(
    GBinderDriver* self,
    GBinderObjectRegistry* reg,
    GBinderHandler* handler,
    guint32 cmd,
    const void* data)
{
    const GBinderIo* io = self->io;

    if (cmd == io->br.noop) {
        GVERBOSE("> BR_NOOP");
    } else if (cmd == io->br.ok) {
        GVERBOSE("> BR_OK");
    } else if (cmd == io->br.transaction_complete) {
        GVERBOSE("> BR_TRANSACTION_COMPLETE");
    } else if (cmd == io->br.spawn_looper) {
        GVERBOSE("> BR_SPAWN_LOOPER");
    } else if (cmd == io->br.finished) {
        GVERBOSE("> BR_FINISHED");
    } else if (cmd == io->br.increfs) {
        guint8 buf[4 + GBINDER_MAX_PTR_COOKIE_SIZE];
        GBinderLocalObject* obj = gbinder_object_registry_get_local
            (reg, io->decode_binder_ptr_cookie(data));

        GVERBOSE("> BR_INCREFS %p", obj);
        gbinder_local_object_handle_increfs(obj);
        gbinder_local_object_unref(obj);
        GVERBOSE("< BC_INCREFS_DONE %p", obj);
        gbinder_driver_cmd_data(self, io->bc.increfs_done, data, buf);
    } else if (cmd == io->br.decrefs) {
        GBinderLocalObject* obj = gbinder_object_registry_get_local
            (reg, io->decode_binder_ptr_cookie(data));

        GVERBOSE("> BR_DECREFS %p", obj);
        gbinder_local_object_handle_decrefs(obj);
        gbinder_local_object_unref(obj);
    } else if (cmd == io->br.acquire) {
        guint8 buf[4 + GBINDER_MAX_PTR_COOKIE_SIZE];
        GBinderLocalObject* obj = gbinder_object_registry_get_local
            (reg, io->decode_binder_ptr_cookie(data));

        GVERBOSE("> BR_ACQUIRE %p", obj);
        gbinder_local_object_handle_acquire(obj);
        gbinder_local_object_unref(obj);
        GVERBOSE("< BC_ACQUIRE_DONE %p", obj);
        gbinder_driver_cmd_data(self, io->bc.acquire_done, data, buf);
    } else if (cmd == io->br.release) {
        GBinderLocalObject* obj = gbinder_object_registry_get_local
            (reg, io->decode_binder_ptr_cookie(data));

        GVERBOSE("> BR_RELEASE %p", obj);
        gbinder_local_object_handle_release(obj);
        gbinder_local_object_unref(obj);
    } else if (cmd == io->br.transaction) {
        gbinder_driver_handle_transaction(self, reg, handler, data);
    } else if (cmd == io->br.dead_binder) {
        guint64 handle = 0;
        GBinderRemoteObject* obj;

        io->decode_cookie(data, &handle);
        GVERBOSE("> BR_DEAD_BINDER %llu", (long long unsigned int)handle);
        obj = gbinder_object_registry_get_remote(reg, (guint32)handle);
        if (obj) {
            gbinder_remote_object_handle_death_notification(obj);
            gbinder_remote_object_unref(obj);
        }
    } else if (cmd == io->br.clear_death_notification_done) {
        GVERBOSE("> BR_CLEAR_DEATH_NOTIFICATION_DONE");
    } else {
#pragma message("TODO: handle more commands from the driver")
        GWARN("Unexpected command 0x%08x", cmd);
    }
}

static
void
gbinder_driver_handle_commands(
    GBinderDriver* self,
    GBinderObjectRegistry* reg,
    GBinderHandler* handler,
    GBinderIoReadBuf* rb)
{
    guint32 cmd;
    gsize unprocessed;
    GBinderIoBuf buf;

    buf.ptr = rb->buf.ptr;
    buf.size = rb->buf.consumed;
    buf.consumed = 0;

    while ((cmd = gbinder_driver_next_command(self, &buf)) != 0) {
        const size_t datalen = _IOC_SIZE(cmd);
        const size_t total = datalen + sizeof(cmd);

        /* Handle this command */
        gbinder_driver_handle_command(self, reg, handler, cmd,
            (void*)(buf.ptr + buf.consumed + sizeof(cmd)));

        /* Switch to the next packet in the buffer */
        buf.consumed += total;
    }

    /* Move the data to the beginning of the buffer to make room for the
     * next portion of data (in case if we need one) */
    unprocessed = buf.size - buf.consumed;
    memmove(rb->data, rb->data + buf.consumed, unprocessed);
    rb->buf.consumed = unprocessed;
}

static
int
gbinder_driver_txstatus(
    GBinderDriver* self,
    GBinderObjectRegistry* reg,
    GBinderHandler* handler,
    GBinderIoReadBuf* rb,
    GBinderRemoteReply* reply)
{
    guint32 cmd;
    gsize unprocessed;
    int txstatus = (-EAGAIN);
    GBinderIoBuf buf;
    const GBinderIo* io = self->io;

    buf.ptr = rb->buf.ptr;
    buf.size = rb->buf.consumed;
    buf.consumed = 0;

    while (txstatus == (-EAGAIN) && (cmd =
        gbinder_driver_next_command(self, &buf)) != 0) {
        /* The size of the data is encoded in the command code */
        const size_t datalen = _IOC_SIZE(cmd);
        const size_t total = datalen + sizeof(cmd);
        const void* data = (void*)(buf.ptr + buf.consumed + sizeof(cmd));

        /* Handle the packet */
        if (cmd == io->br.transaction_complete) {
            GVERBOSE("> BR_TRANSACTION_COMPLETE");
            if (!reply) {
                txstatus = GBINDER_STATUS_OK;
            }
        } else if (cmd == io->br.dead_reply) {
            GVERBOSE("> BR_DEAD_REPLY");
            txstatus = GBINDER_STATUS_DEAD_OBJECT;
        } else if (cmd == io->br.failed_reply) {
            GVERBOSE("> BR_FAILED_REPLY");
            txstatus = GBINDER_STATUS_FAILED;
        } else if (cmd == io->br.reply) {
            GBinderIoTxData tx;

            io->decode_transaction_data(data, &tx);
            gbinder_driver_verbose_transaction_data("BR_REPLY", &tx);

            /* Transfer data ownership to the request */
            if (tx.data && tx.size) {
                gbinder_driver_verbose_dump(' ', (uintptr_t)tx.data, tx.size);
                gbinder_remote_reply_set_data(reply,
                    gbinder_buffer_new(self, tx.data, tx.size),
                    tx.objects);
            } else {
                g_free(tx.objects);
                gbinder_driver_free_buffer(self, tx.data);
            }

            txstatus = tx.status;
            GASSERT(txstatus != (-EAGAIN));
            if (txstatus == (-EAGAIN)) txstatus = (-EFAULT);
        } else {
            gbinder_driver_handle_command(self, reg, handler, cmd, data);
        }

        /* Switch to the next packet in the buffer */
        buf.consumed += total;
    }

    /* Move the data to the beginning of the buffer to make room for the
     * next portion of data (in case if we need one) */
    unprocessed = buf.size - buf.consumed;
    memmove(rb->data, rb->data + buf.consumed, unprocessed);
    rb->buf.consumed = unprocessed;
    return txstatus;
}

/*==========================================================================*
 * Interface
 *
 * This is an internal module, we can assume that GBinderDriver pointer
 * is never NULL, GBinderIpc makes sure of that.
 *==========================================================================*/

GBinderDriver*
gbinder_driver_new(
    const char* dev)
{
    const int fd = gbinder_system_open(dev, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        gint32 version = 0;

        if (gbinder_system_ioctl(fd, BINDER_VERSION, &version) >= 0) {
            const GBinderIo* io = NULL;

            /* Decide which kernel we are dealing with */
            GDEBUG("Opened %s version %d", dev, version);
            if (version == gbinder_io_32.version) {
                io = &gbinder_io_32;
            } else if (version == gbinder_io_64.version) {
                io = &gbinder_io_64;
            } else {
                GERR("%s unexpected version %d", dev, version);
            }
            if (io) {
                /* mmap the binder, providing a chunk of virtual address
                 * space to receive transactions. */
                const gsize vmsize = BINDER_VM_SIZE;
                void* vm = gbinder_system_mmap(vmsize, PROT_READ,
                    MAP_PRIVATE | MAP_NORESERVE, fd);
                if (vm != MAP_FAILED) {
                    guint32 max_threads = DEFAULT_MAX_BINDER_THREADS;
                    GBinderDriver* self = g_slice_new0(GBinderDriver);

                    g_atomic_int_set(&self->refcount, 1);
                    self->fd = fd;
                    self->io = io;
                    self->vm = vm;
                    self->vmsize = vmsize;
                    self->dev = g_strdup(dev);
                    if (gbinder_system_ioctl(fd, BINDER_SET_MAX_THREADS,
                        &max_threads) < 0) {
                        GERR("%s failed to set max threads (%u): %s", dev,
                            max_threads, strerror(errno));
                    }
                    /* Choose the protocol based on the device name */
                    self->protocol = gbinder_rpc_protocol_for_device(dev);
                    return self;
                } else {
                    GERR("%s failed to mmap: %s", dev, strerror(errno));
                }
            }
        } else {
            GERR("Can't get binder version from %s: %s", dev, strerror(errno));
        }
        gbinder_system_close(fd);
    } else {
        GERR("Can't open %s: %s", dev, strerror(errno));
    }
    return NULL;
}

GBinderDriver*
gbinder_driver_ref(
    GBinderDriver* self)
{
    GASSERT(self->refcount > 0);
    g_atomic_int_inc(&self->refcount);
    return self;
}

void
gbinder_driver_unref(
    GBinderDriver* self)
{
    GASSERT(self->refcount > 0);
    if (g_atomic_int_dec_and_test(&self->refcount)) {
        GDEBUG("Closing %s", self->dev);
        gbinder_system_munmap(self->vm, self->vmsize);
        gbinder_system_close(self->fd);
        g_free(self->dev);
        g_slice_free(GBinderDriver, self);
    }
}

int
gbinder_driver_fd(
    GBinderDriver* self)
{
    /* Only used by unit tests */
    return self->fd;
}

int
gbinder_driver_poll(
    GBinderDriver* self,
    struct pollfd* pipefd)
{
    struct pollfd fds[2];
    nfds_t n = 1;
    int err;

    memset(fds, 0, sizeof(fds));
    fds[0].fd = self->fd;
    fds[0].events = POLLIN | POLLERR | POLLHUP | POLLNVAL;

    if (pipefd) {
        fds[n].fd = pipefd->fd;
        fds[n].events = pipefd->events;
        n++;
    }

    err = poll(fds, n, -1);
    if (err >= 0) {
        if (pipefd) {
            pipefd->revents = fds[1].revents;
        }
        return fds[0].revents;
    }

    if (pipefd) {
        pipefd->revents = 0;
    }
    return err;
}

const char*
gbinder_driver_dev(
    GBinderDriver* self)
{
    return self->dev;
}

const GBinderIo*
gbinder_driver_io(
    GBinderDriver* self)
{
    return self->io;
}

gboolean
gbinder_driver_request_death_notification(
    GBinderDriver* self,
    GBinderRemoteObject* obj)
{
    return gbinder_driver_death_notification
        (self, self->io->bc.request_death_notification, obj);
}

gboolean
gbinder_driver_clear_death_notification(
    GBinderDriver* self,
    GBinderRemoteObject* obj)
{
    return gbinder_driver_death_notification
        (self, self->io->bc.clear_death_notification, obj);
}

gboolean
gbinder_driver_increfs(
    GBinderDriver* self,
    guint32 handle)
{
    GVERBOSE("< BC_INCREFS 0x%08x", handle);
    return gbinder_driver_cmd_int32(self, self->io->bc.increfs, handle);
}

gboolean
gbinder_driver_decrefs(
    GBinderDriver* self,
    guint32 handle)
{
    GVERBOSE("< BC_DECREFS 0x%08x", handle);
    return gbinder_driver_cmd_int32(self, self->io->bc.decrefs, handle);
}

gboolean
gbinder_driver_acquire(
    GBinderDriver* self,
    guint32 handle)
{
    GVERBOSE("< BC_ACQUIRE 0x%08x", handle);
    return gbinder_driver_cmd_int32(self, self->io->bc.acquire, handle);
}

gboolean
gbinder_driver_release(
    GBinderDriver* self,
    guint32 handle)
{
    GVERBOSE("< BC_RELEASE 0x%08x", handle);
    return gbinder_driver_cmd_int32(self, self->io->bc.release, handle);
}

void
gbinder_driver_free_buffer(
    GBinderDriver* self,
    void* buffer)
{
    if (buffer) {
        GBinderIoBuf write;
        const GBinderIo* io = self->io;
        guint8 wbuf[GBINDER_MAX_POINTER_SIZE + sizeof(guint32)];
        guint32* cmd = (guint32*)wbuf;
        guint len = sizeof(*cmd);

        GVERBOSE("< BC_FREE_BUFFER %p", buffer);
        *cmd = io->bc.free_buffer;
        len += io->encode_pointer(wbuf + len, buffer);

        /* Write it */
        write.ptr = (uintptr_t)wbuf;
        write.size = len;
        write.consumed = 0;
        gbinder_driver_write(self, &write);
    }
}

gboolean
gbinder_driver_enter_looper(
    GBinderDriver* self)
{
    GVERBOSE("< BC_ENTER_LOOPER");
    return gbinder_driver_cmd(self, self->io->bc.enter_looper);
}

gboolean
gbinder_driver_exit_looper(
    GBinderDriver* self)
{
    GVERBOSE("< BC_EXIT_LOOPER");
    return gbinder_driver_cmd(self, self->io->bc.exit_looper);
}

int
gbinder_driver_read(
    GBinderDriver* self,
    GBinderObjectRegistry* reg,
    GBinderHandler* handler)
{
    GBinderIoReadBuf rb;
    int ret;

    gbinder_driver_read_init(&rb);
    ret = gbinder_driver_write_read(self, NULL, &rb.buf);
    if (ret >= 0) {
        /* Loop until we have handled all the incoming commands */
        gbinder_driver_handle_commands(self, reg, handler, &rb);
        while (rb.buf.consumed) {
            ret = gbinder_driver_write_read(self, NULL, &rb.buf);
            if (ret >= 0) {
                gbinder_driver_handle_commands(self, reg, handler, &rb);
            } else {
                break;
            }
        }
    }
    return ret;
}

int
gbinder_driver_transact(
    GBinderDriver* self,
    GBinderObjectRegistry* reg,
    guint32 handle,
    guint32 code,
    GBinderLocalRequest* req,
    GBinderRemoteReply* reply)
{
    GBinderIoBuf write;
    GBinderIoReadBuf rb;
    const GBinderIo* io = self->io;
    const guint flags = reply ? 0 : GBINDER_TX_FLAG_ONEWAY;
    GBinderOutputData* data = gbinder_local_request_data(req);
    const gsize extra_buffers = gbinder_output_data_buffers_size(data);
    GUtilIntArray* offsets = gbinder_output_data_offsets(data);
    void* offsets_buf = NULL;
    guint8 wbuf[GBINDER_MAX_BC_TRANSACTION_SG_SIZE + sizeof(guint32)];
    guint32* cmd = (guint32*)wbuf;
    guint len = sizeof(*cmd);
    int txstatus = (-EAGAIN);

    gbinder_driver_read_init(&rb);

    /* Build BC_TRANSACTION */
    if (extra_buffers) {
        GVERBOSE("< BC_TRANSACTION_SG 0x%08x 0x%08x %u bytes", handle, code,
            (guint)extra_buffers);
        gbinder_driver_verbose_dump_bytes(' ', data->bytes);
        *cmd = io->bc.transaction_sg;
        len += io->encode_transaction_sg(wbuf + len, handle, code,
            data->bytes, flags, offsets, &offsets_buf, extra_buffers);
    } else {
        GVERBOSE("< BC_TRANSACTION 0x%08x 0x%08x", handle, code);
        gbinder_driver_verbose_dump_bytes(' ', data->bytes);
        *cmd = io->bc.transaction;
        len += io->encode_transaction(wbuf + len, handle, code,
            data->bytes, flags, offsets, &offsets_buf);
    }

#if 0 /* GUTIL_LOG_VERBOSE */
    if (offsets && offsets->count) {
        gbinder_driver_verbose_dump('<', (uintptr_t)offsets_buf,
            offsets->count * io->pointer_size);
    }
#endif /* GUTIL_LOG_VERBOSE */

    /* Write it */
    write.ptr = (uintptr_t)wbuf;
    write.size = len;
    write.consumed = 0;

    /* And wait for reply. Positive txstatus is the transaction status,
     * negative is a driver error (except for -EAGAIN meaning that there's
     * no status yet) */
    while (txstatus == (-EAGAIN)) {
        int err = gbinder_driver_write_read(self, &write, &rb.buf);
        if (err < 0) {
            txstatus = err;
        } else {
            txstatus = gbinder_driver_txstatus(self, reg, NULL, &rb, reply);
        }
    }

    if (txstatus >= 0) {
        /* The whole thing should've been written in case of success */
        GASSERT(write.consumed == write.size || txstatus > 0);

        /* Loop until we have handled all the incoming commands */
        gbinder_driver_handle_commands(self, reg, NULL, &rb);
        while (rb.buf.consumed) {
            int err = gbinder_driver_write_read(self, NULL, &rb.buf);
            if (err < 0) {
                txstatus = err;
                break;
            } else {
                gbinder_driver_handle_commands(self, reg, NULL, &rb);
            }
        }
    }

    g_free(offsets_buf);
    return txstatus;
}

GBinderLocalRequest*
gbinder_driver_local_request_new(
    GBinderDriver* self,
    const char* iface)
{
    GBinderLocalRequest* req = gbinder_local_request_new(self->io, NULL);
    GBinderWriter writer;

    gbinder_local_request_init_writer(req, &writer);
    self->protocol->write_rpc_header(&writer, iface);
    return req;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
