/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2009 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "tap_int.h"


#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <net/if.h>

#include "net/net.h"
#include "clients.h"
#include "monitor/monitor.h"
#include "sysemu/sysemu.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"

#include "net/tap.h"

#include "net/vhost_net.h"

typedef struct TAPState {
    NetClientState nc;
    int fd;
    char down_script[1024];
    char down_script_arg[128];
    uint8_t buf[NET_BUFSIZE];
    bool read_poll;
    bool write_poll;
    bool using_vnet_hdr;
    bool has_ufo;
    bool enabled;
	//与内核的vhost-net联系
    VHostNetState *vhost_net;
    unsigned host_vnet_hdr_len;
    Notifier exit;
} TAPState;

static void launch_script(const char *setup_script, const char *ifname,
                          int fd, Error **errp);

static void tap_send(void *opaque);
static void tap_writable(void *opaque);

/*
 * main() [vl.c]
 *  net_init_clients() 
 *   net_init_client() 处理-net参数
 *    net_client_init()
 *     net_client_init1()
 *      net_init_tap()
 *       net_init_tap()
 *        net_init_tap_one(name="tap")
 *         net_tap_fd_init()
 *          tap_read_poll(enable=true) 
 *           tap_update_fd_handler(s->enable==true)
 */
static void tap_update_fd_handler(TAPState *s)
{
    //加入到iohandler_ctx,qemu主循环可以监听tap设备的fd上的可读时间了
    qemu_set_fd_handler(s->fd,
                        s->read_poll && s->enabled ? tap_send : NULL,
                        s->write_poll && s->enabled ? tap_writable : NULL,
                        s);
}

/*
 * main() [vl.c]
 *  net_init_clients() 
 *   net_init_client() 处理-net参数
 *    net_client_init()
 *     net_client_init1()
 *      net_init_tap()
 *       net_init_tap()
 *        net_init_tap_one(name="tap")
 *         net_tap_fd_init()
 *          tap_read_poll(enable=true) 
 */
static void tap_read_poll(TAPState *s, bool enable)
{
    s->read_poll = enable;
    tap_update_fd_handler(s);
}

static void tap_write_poll(TAPState *s, bool enable)
{
    s->write_poll = enable;
    tap_update_fd_handler(s);
}

static void tap_writable(void *opaque)
{
    TAPState *s = opaque;

    tap_write_poll(s, false);

    qemu_flush_queued_packets(&s->nc);
}

/*
* set_tctl()
*  start_xmit()
*   process_tx_desc()
*	xmit_seg()
*	 e1000_send_packet()
*	  qemu_send_packet()
*	   qemu_send_packet_async()
*		qemu_send_packet_async_with_flags()
*		 qemu_net_queue_send()
*		  qemu_net_queue_deliver()
*          qemu_deliver_packet_iov()
*           tap_receive_iov()
*            tap_write_packet()
*/
static ssize_t tap_write_packet(TAPState *s, const struct iovec *iov, int iovcnt)
{
    ssize_t len;

    do {
		//走向glibc啦，发送内核态的tap设备，然后内核的br0,转给物理网卡，发送出去
        len = writev(s->fd, iov, iovcnt);
    } while (len == -1 && errno == EINTR);

    if (len == -1 && errno == EAGAIN) {
        tap_write_poll(s, true);
        return 0;
    }

    return len;
}

/*
* set_tctl()
*  start_xmit()
*   process_tx_desc()
*	xmit_seg()
*	 e1000_send_packet()
*	  qemu_send_packet()
*	   qemu_send_packet_async()
*		qemu_send_packet_async_with_flags()
*		 qemu_net_queue_send()
*		  qemu_net_queue_deliver()
*          qemu_deliver_packet_iov()
*           tap_receive_iov()
*/
static ssize_t tap_receive_iov(NetClientState *nc, const struct iovec *iov,
                               int iovcnt)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    const struct iovec *iovp = iov;
    struct iovec iov_copy[iovcnt + 1];
    struct virtio_net_hdr_mrg_rxbuf hdr = { };

    if (s->host_vnet_hdr_len && !s->using_vnet_hdr) {
        iov_copy[0].iov_base = &hdr;
        iov_copy[0].iov_len =  s->host_vnet_hdr_len;
        memcpy(&iov_copy[1], iov, iovcnt * sizeof(*iov));
        iovp = iov_copy;
        iovcnt++;
    }

    return tap_write_packet(s, iovp, iovcnt);
}

static ssize_t tap_receive_raw(NetClientState *nc, const uint8_t *buf, size_t size)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    struct iovec iov[2];
    int iovcnt = 0;
    struct virtio_net_hdr_mrg_rxbuf hdr = { };

    if (s->host_vnet_hdr_len) {
        iov[iovcnt].iov_base = &hdr;
        iov[iovcnt].iov_len  = s->host_vnet_hdr_len;
        iovcnt++;
    }

    iov[iovcnt].iov_base = (char *)buf;
    iov[iovcnt].iov_len  = size;
    iovcnt++;

    return tap_write_packet(s, iov, iovcnt);
}

static ssize_t tap_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    struct iovec iov[1];

    if (s->host_vnet_hdr_len && !s->using_vnet_hdr) {
        return tap_receive_raw(nc, buf, size);
    }

    iov[0].iov_base = (char *)buf;
    iov[0].iov_len  = size;

    return tap_write_packet(s, iov, 1);
}

#ifndef __sun__
/*
 * tap_send()
 *  tap_read_packet()
 */
ssize_t tap_read_packet(int tapfd, uint8_t *buf, int maxlen)
{
    return read(tapfd, buf, maxlen);
}
#endif

static void tap_send_completed(NetClientState *nc, ssize_t len)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    tap_read_poll(s, true);
}

/*
 * aio_dispatch()
 *  tap_send()
 */
static void tap_send(void *opaque)
{
    TAPState *s = opaque;
    int size;
    int packets = 0;

    while (true) {
        uint8_t *buf = s->buf;

        //从内核的tap设备中去读取数据过来
        size = tap_read_packet(s->fd, s->buf, sizeof(s->buf));
        if (size <= 0) {
            break;
        }

        if (s->host_vnet_hdr_len && !s->using_vnet_hdr) {
            buf  += s->host_vnet_hdr_len;
            size -= s->host_vnet_hdr_len;
        }

        //将读取过来的数据转给e1000->incoming_queue
        size = qemu_send_packet_async(&s->nc, buf, size, tap_send_completed);
        if (size == 0) {
            tap_read_poll(s, false);
            break;
        } else if (size < 0) {
            break;
        }

        /*
         * When the host keeps receiving more packets while tap_send() is
         * running we can hog the QEMU global mutex.  Limit the number of
         * packets that are processed per tap_send() callback to prevent
         * stalling the guest.
         */
        packets++;
        if (packets >= 50) {
            break;
        }
    }
}

static bool tap_has_ufo(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);

    return s->has_ufo;
}

static bool tap_has_vnet_hdr(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);

    return !!s->host_vnet_hdr_len;
}

static bool tap_has_vnet_hdr_len(NetClientState *nc, int len)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);

    return !!tap_probe_vnet_hdr_len(s->fd, len);
}

static void tap_set_vnet_hdr_len(NetClientState *nc, int len)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);
    assert(len == sizeof(struct virtio_net_hdr_mrg_rxbuf) ||
           len == sizeof(struct virtio_net_hdr));

    tap_fd_set_vnet_hdr_len(s->fd, len);
    s->host_vnet_hdr_len = len;
}

static void tap_using_vnet_hdr(NetClientState *nc, bool using_vnet_hdr)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);
    assert(!!s->host_vnet_hdr_len == using_vnet_hdr);

    s->using_vnet_hdr = using_vnet_hdr;
}

static int tap_set_vnet_le(NetClientState *nc, bool is_le)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    return tap_fd_set_vnet_le(s->fd, is_le);
}

static int tap_set_vnet_be(NetClientState *nc, bool is_be)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    return tap_fd_set_vnet_be(s->fd, is_be);
}

static void tap_set_offload(NetClientState *nc, int csum, int tso4,
                     int tso6, int ecn, int ufo)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    if (s->fd < 0) {
        return;
    }

    tap_fd_set_offload(s->fd, csum, tso4, tso6, ecn, ufo);
}

static void tap_exit_notify(Notifier *notifier, void *data)
{
    TAPState *s = container_of(notifier, TAPState, exit);
    Error *err = NULL;

    if (s->down_script[0]) {
        launch_script(s->down_script, s->down_script_arg, s->fd, &err);
        if (err) {
            error_report_err(err);
        }
    }
}

static void tap_cleanup(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    if (s->vhost_net) {
        vhost_net_cleanup(s->vhost_net);
        g_free(s->vhost_net);
        s->vhost_net = NULL;
    }

    qemu_purge_queued_packets(nc);

    tap_exit_notify(&s->exit, NULL);
    qemu_remove_exit_notifier(&s->exit);

    tap_read_poll(s, false);
    tap_write_poll(s, false);
    close(s->fd);
    s->fd = -1;
}

static void tap_poll(NetClientState *nc, bool enable)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    tap_read_poll(s, enable);
    tap_write_poll(s, enable);
}

int tap_get_fd(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);
    return s->fd;
}

/* fd support */

static NetClientInfo net_tap_info = {
    .type = NET_CLIENT_DRIVER_TAP,
    .size = sizeof(TAPState),
    .receive = tap_receive,
    .receive_raw = tap_receive_raw,
    .receive_iov = tap_receive_iov,
    .poll = tap_poll,
    .cleanup = tap_cleanup,
    .has_ufo = tap_has_ufo,
    .has_vnet_hdr = tap_has_vnet_hdr,
    .has_vnet_hdr_len = tap_has_vnet_hdr_len,
    .using_vnet_hdr = tap_using_vnet_hdr,
    .set_offload = tap_set_offload,
    .set_vnet_hdr_len = tap_set_vnet_hdr_len,
    .set_vnet_le = tap_set_vnet_le,
    .set_vnet_be = tap_set_vnet_be,
};

/*
 * main() [vl.c]
 *  net_init_clients() 
 *   net_init_client() 处理-net参数
 *    net_client_init()
 *     net_client_init1()
 *      net_init_tap()
 *       net_init_tap()
 *        net_init_tap_one(model="tap")
 *         net_tap_fd_init()
 */
static TAPState *net_tap_fd_init(NetClientState *peer,
                                 const char *model,
                                 const char *name,
                                 int fd,
                                 int vnet_hdr)
{
    NetClientState *nc;
    TAPState *s;

    //创建NetClientState
    nc = qemu_new_net_client(&net_tap_info, peer, model, name);

    s = DO_UPCAST(TAPState, nc, nc);

    s->fd = fd;
    s->host_vnet_hdr_len = vnet_hdr ? sizeof(struct virtio_net_hdr) : 0;
    s->using_vnet_hdr = false;
    s->has_ufo = tap_probe_has_ufo(s->fd);
    s->enabled = true;
    tap_set_offload(&s->nc, 0, 0, 0, 0, 0);
    /*
     * Make sure host header length is set correctly in tap:
     * it might have been modified by another instance of qemu.
     */
    if (tap_probe_vnet_hdr_len(s->fd, s->host_vnet_hdr_len)) {
        tap_fd_set_vnet_hdr_len(s->fd, s->host_vnet_hdr_len);
    }
	//对TAPState->fd 设置aio 监听
    tap_read_poll(s, true);
    s->vhost_net = NULL;

   
    s->exit.notify = tap_exit_notify;
    qemu_add_exit_notifier(&s->exit);

    return s;
}

static void launch_script(const char *setup_script, const char *ifname,
                          int fd, Error **errp)
{
    int pid, status;
    char *args[3];
    char **parg;

    /* try to launch network script */
    pid = fork();
    if (pid < 0) {
        error_setg_errno(errp, errno, "could not launch network script %s",
                         setup_script);
        return;
    }
    if (pid == 0) {
        int open_max = sysconf(_SC_OPEN_MAX), i;

        for (i = 3; i < open_max; i++) {
            if (i != fd) {
                close(i);
            }
        }
        parg = args;
        *parg++ = (char *)setup_script;
        *parg++ = (char *)ifname;
        *parg = NULL;
        execv(setup_script, args);
        _exit(1);
    } else {
        while (waitpid(pid, &status, 0) != pid) {
            /* loop */
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return;
        }
        error_setg(errp, "network script %s failed with status %d",
                   setup_script, status);
    }
}

static int recv_fd(int c)
{
    int fd;
    uint8_t msgbuf[CMSG_SPACE(sizeof(fd))];
    struct msghdr msg = {
        .msg_control = msgbuf,
        .msg_controllen = sizeof(msgbuf),
    };
    struct cmsghdr *cmsg;
    struct iovec iov;
    uint8_t req[1];
    ssize_t len;

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    msg.msg_controllen = cmsg->cmsg_len;

    iov.iov_base = req;
    iov.iov_len = sizeof(req);

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    len = recvmsg(c, &msg, 0);
    if (len > 0) {
        memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
        return fd;
    }

    return len;
}

static int net_bridge_run_helper(const char *helper, const char *bridge,
                                 Error **errp)
{
    sigset_t oldmask, mask;
    int pid, status;
    char *args[5];
    char **parg;
    int sv[2];

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    if (socketpair(PF_UNIX, SOCK_STREAM, 0, sv) == -1) {
        error_setg_errno(errp, errno, "socketpair() failed");
        return -1;
    }

    /* try to launch bridge helper */
    pid = fork();
    if (pid < 0) {
        error_setg_errno(errp, errno, "Can't fork bridge helper");
        return -1;
    }
    if (pid == 0) {
        int open_max = sysconf(_SC_OPEN_MAX), i;
        char fd_buf[6+10];
        char br_buf[6+IFNAMSIZ] = {0};
        char helper_cmd[PATH_MAX + sizeof(fd_buf) + sizeof(br_buf) + 15];

        for (i = 3; i < open_max; i++) {
            if (i != sv[1]) {
                close(i);
            }
        }

        snprintf(fd_buf, sizeof(fd_buf), "%s%d", "--fd=", sv[1]);

        if (strrchr(helper, ' ') || strrchr(helper, '\t')) {
            /* assume helper is a command */

            if (strstr(helper, "--br=") == NULL) {
                snprintf(br_buf, sizeof(br_buf), "%s%s", "--br=", bridge);
            }

            snprintf(helper_cmd, sizeof(helper_cmd), "%s %s %s %s",
                     helper, "--use-vnet", fd_buf, br_buf);

            parg = args;
            *parg++ = (char *)"sh";
            *parg++ = (char *)"-c";
            *parg++ = helper_cmd;
            *parg++ = NULL;

            execv("/bin/sh", args);
        } else {
            /* assume helper is just the executable path name */

            snprintf(br_buf, sizeof(br_buf), "%s%s", "--br=", bridge);

            parg = args;
            *parg++ = (char *)helper;
            *parg++ = (char *)"--use-vnet";
            *parg++ = fd_buf;
            *parg++ = br_buf;
            *parg++ = NULL;

            execv(helper, args);
        }
        _exit(1);

    } else {
        int fd;
        int saved_errno;

        close(sv[1]);

        do {
            fd = recv_fd(sv[0]);
        } while (fd == -1 && errno == EINTR);
        saved_errno = errno;

        close(sv[0]);

        while (waitpid(pid, &status, 0) != pid) {
            /* loop */
        }
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        if (fd < 0) {
            error_setg_errno(errp, saved_errno,
                             "failed to recv file descriptor");
            return -1;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            error_setg(errp, "bridge helper failed");
            return -1;
        }
        return fd;
    }
}

int net_init_bridge(const Netdev *netdev, const char *name,
                    NetClientState *peer, Error **errp)
{
    const NetdevBridgeOptions *bridge;
    const char *helper, *br;
    TAPState *s;
    int fd, vnet_hdr;

    assert(netdev->type == NET_CLIENT_DRIVER_BRIDGE);
    bridge = &netdev->u.bridge;

    helper = bridge->has_helper ? bridge->helper : DEFAULT_BRIDGE_HELPER;
    br     = bridge->has_br     ? bridge->br     : DEFAULT_BRIDGE_INTERFACE;

    fd = net_bridge_run_helper(helper, br, errp);
    if (fd == -1) {
        return -1;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);
    vnet_hdr = tap_probe_vnet_hdr(fd);
    s = net_tap_fd_init(peer, "bridge", name, fd, vnet_hdr);

    snprintf(s->nc.info_str, sizeof(s->nc.info_str), "helper=%s,br=%s", helper,
             br);

    return 0;
}

/*
 * main() [vl.c]
 *	net_init_clients() 
 *	 net_init_client() 处理-net参数
 *	  net_client_init()
 *	   net_client_init1()
 *		net_init_tap()
 *       net_tap_init()
 */
static int net_tap_init(const NetdevTapOptions *tap, int *vnet_hdr,
                        const char *setup_script, char *ifname,
                        size_t ifname_sz, int mq_required, Error **errp)
{
    Error *err = NULL;
    int fd, vnet_hdr_required;

    if (tap->has_vnet_hdr) {
        *vnet_hdr = tap->vnet_hdr;
        vnet_hdr_required = *vnet_hdr;
    } else {
        *vnet_hdr = 1;
        vnet_hdr_required = 0;
    }

    // /dev/net/tap 的文件描述符
    TFR(fd = tap_open(ifname, ifname_sz, vnet_hdr, vnet_hdr_required,
                      mq_required, errp));
    if (fd < 0) {
        return -1;
    }

    if (setup_script &&
        setup_script[0] != '\0' &&
        strcmp(setup_script, "no") != 0) {
        launch_script(setup_script, ifname, fd, &err);
        if (err) {
            error_propagate(errp, err);
            close(fd);
            return -1;
        }
    }

    return fd;
}

#define MAX_TAP_QUEUES 1024

/*
 * main() [vl.c]
 *  net_init_clients() 
 *   net_init_client() 处理-net参数
 *    net_client_init()
 *     net_client_init1()
 *      net_init_tap()
 *       net_init_tap()
 *        net_init_tap_one(model="tap")
 *
 * NetdevTapOptions在编译时产生
 * https://android.googlesource.com/platform/external/qemu/+/master/qapi-auto-generated/qapi-types.h
 */
static void net_init_tap_one(const NetdevTapOptions *tap, NetClientState *peer,
                             const char *model, const char *name,
                             const char *ifname, const char *script,
                             const char *downscript, const char *vhostfdname,
                             int vnet_hdr, int fd, Error **errp)
{
    Error *err = NULL;

	//创建TAPState
    TAPState *s = net_tap_fd_init(peer, model, name, fd, vnet_hdr);
    int vhostfd;

    //设置tap设备的发送包空间大小
    tap_set_sndbuf(s->fd, tap, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    if (tap->has_fd || tap->has_fds) {
        snprintf(s->nc.info_str, sizeof(s->nc.info_str), "fd=%d", fd);
    } else if (tap->has_helper) {
        snprintf(s->nc.info_str, sizeof(s->nc.info_str), "helper=%s",
                 tap->helper);
    } else {
        snprintf(s->nc.info_str, sizeof(s->nc.info_str),
                 "ifname=%s,script=%s,downscript=%s", ifname, script,
                 downscript);

        if (strcmp(downscript, "no") != 0) {
            snprintf(s->down_script, sizeof(s->down_script), "%s", downscript);
            snprintf(s->down_script_arg, sizeof(s->down_script_arg),
                     "%s", ifname);
        }
    }

    //要在使用vhost accelerator,代替qemu用户空间处理vring数据
    if (tap->has_vhost ? tap->vhost :
        vhostfdname || (tap->has_vhostforce && tap->vhostforce)) {
        VhostNetOptions options;

        //virtio 的后端在内核了，不在qemu层面了
        options.backend_type = VHOST_BACKEND_TYPE_KERNEL;
        options.net_backend = &s->nc;
        if (tap->has_poll_us) {
            options.busyloop_timeout = tap->poll_us;
        } else {
            options.busyloop_timeout = 0;
        }

        if (vhostfdname) {
            vhostfd = monitor_fd_param(cur_mon, vhostfdname, &err);
            if (vhostfd == -1) {
                error_propagate(errp, err);
                return;
            }
        } else {//走这里,这个设备文件就是内核代码的vhost_net_misc对象
            vhostfd = open("/dev/vhost-net", O_RDWR);
            if (vhostfd < 0) {
                error_setg_errno(errp, errno,
                                 "tap: open vhost char device failed");
                return;
            }
        }
		//保存/dev/vhost-net
        options.opaque = (void *)(uintptr_t)vhostfd;

        //创建vhost_net,初始化
        s->vhost_net = vhost_net_init(&options);
        if (!s->vhost_net) {
            error_setg(errp,
                       "vhost-net requested but could not be initialized");
            return;
        }
    } else if (vhostfdname) {
        error_setg(errp, "vhostfd(s)= is not valid without vhost");
    }
}

static int get_fds(char *str, char *fds[], int max)
{
    char *ptr = str, *this;
    size_t len = strlen(str);
    int i = 0;

    while (i < max && ptr < str + len) {
        this = strchr(ptr, ':');

        if (this == NULL) {
            fds[i] = g_strdup(ptr);
        } else {
            fds[i] = g_strndup(ptr, this - ptr);
        }

        i++;
        if (this == NULL) {
            break;
        } else {
            ptr = this + 1;
        }
    }

    return i;
}

/*
 * main() [vl.c]
 *  net_init_clients()
 *   net_init_netdev() 处理 -netdev 参数
 *    net_client_init()
 *     net_client_init1()
 *       net_init_tap()
 */
int net_init_tap(const Netdev *netdev, const char *name,
                 NetClientState *peer, Error **errp)
{
   
    const NetdevTapOptions *tap;//这个类型在编译时产生
    int fd, vnet_hdr = 0, i = 0, queues;
    /* for the no-fd, no-helper case */
    const char *script = NULL; /* suppress wrong "uninit'd use" gcc warning */
    const char *downscript = NULL;
    Error *err = NULL;
    const char *vhostfdname;
    char ifname[128];

    assert(netdev->type == NET_CLIENT_DRIVER_TAP);
	//得到NetdevTapOptions对象先,下面根据指定tap设备的方式初始化tap
    tap = &netdev->u.tap;
	//tap设备的队列
    queues = tap->has_queues ? tap->queues : 1;
    vhostfdname = tap->has_vhostfd ? tap->vhostfd : NULL;

    /* QEMU vlans does not support multiqueue tap, in this case peer is set.
     * For -netdev, peer is always NULL. */
    if (peer && (tap->has_queues || tap->has_fds || tap->has_vhostfds)) {
        error_setg(errp, "Multiqueue tap cannot be used with QEMU vlans");
        return -1;
    }

    if (tap->has_fd) { //通过fd的方式指定tap,只有一个fd
        if (tap->has_ifname || tap->has_script || tap->has_downscript ||
            tap->has_vnet_hdr || tap->has_helper || tap->has_queues ||
            tap->has_fds || tap->has_vhostfds) {
            
            error_setg(errp, "ifname=, script=, downscript=, vnet_hdr=, "
                       "helper=, queues=, fds=, and vhostfds= "
                       "are invalid with fd=");
            return -1;
        }

        fd = monitor_fd_param(cur_mon, tap->fd, &err);
        if (fd == -1) {
            error_propagate(errp, err);
            return -1;
        }

        fcntl(fd, F_SETFL, O_NONBLOCK);

        vnet_hdr = tap_probe_vnet_hdr(fd);

        net_init_tap_one(tap, peer, "tap", name, NULL,
                         script, downscript,
                         vhostfdname, vnet_hdr, fd, &err);
        if (err) {
            error_propagate(errp, err);
            return -1;
        }
    } else if (tap->has_fds) {//多个fd
        char **fds = g_new0(char *, MAX_TAP_QUEUES);
        char **vhost_fds = g_new0(char *, MAX_TAP_QUEUES);
        int nfds, nvhosts;

        if (tap->has_ifname || tap->has_script || tap->has_downscript ||
            tap->has_vnet_hdr || tap->has_helper || tap->has_queues ||
            tap->has_vhostfd) {
            error_setg(errp, "ifname=, script=, downscript=, vnet_hdr=, "
                       "helper=, queues=, and vhostfd= "
                       "are invalid with fds=");
            return -1;
        }

        nfds = get_fds(tap->fds, fds, MAX_TAP_QUEUES);
        if (tap->has_vhostfds) {
            nvhosts = get_fds(tap->vhostfds, vhost_fds, MAX_TAP_QUEUES);
            if (nfds != nvhosts) {
                error_setg(errp, "The number of fds passed does not match "
                           "the number of vhostfds passed");
                goto free_fail;
            }
        }

        for (i = 0; i < nfds; i++) {
            fd = monitor_fd_param(cur_mon, fds[i], &err);
            if (fd == -1) {
                error_propagate(errp, err);
                goto free_fail;
            }

            fcntl(fd, F_SETFL, O_NONBLOCK);

            if (i == 0) {
                vnet_hdr = tap_probe_vnet_hdr(fd);
            } else if (vnet_hdr != tap_probe_vnet_hdr(fd)) {
                error_setg(errp,
                           "vnet_hdr not consistent across given tap fds");
                goto free_fail;
            }

            net_init_tap_one(tap, peer, "tap", name, ifname,
                             script, downscript,
                             tap->has_vhostfds ? vhost_fds[i] : NULL,
                             vnet_hdr, fd, &err);
            if (err) {
                error_propagate(errp, err);
                goto free_fail;
            }
        }
        g_free(fds);
        g_free(vhost_fds);
        return 0;

free_fail:
        for (i = 0; i < nfds; i++) {
            g_free(fds[i]);
            g_free(vhost_fds[i]);
        }
        g_free(fds);
        g_free(vhost_fds);
        return -1;
    } else if (tap->has_helper) {
        if (tap->has_ifname || tap->has_script || tap->has_downscript ||
            tap->has_vnet_hdr || tap->has_queues || tap->has_vhostfds) {
            error_setg(errp, "ifname=, script=, downscript=, vnet_hdr=, "
                       "queues=, and vhostfds= are invalid with helper=");
            return -1;
        }

        fd = net_bridge_run_helper(tap->helper,
                                   tap->has_br ?
                                   tap->br : DEFAULT_BRIDGE_INTERFACE,
                                   errp);
        if (fd == -1) {
            return -1;
        }

        fcntl(fd, F_SETFL, O_NONBLOCK);
        vnet_hdr = tap_probe_vnet_hdr(fd);

        net_init_tap_one(tap, peer, "bridge", name, ifname,
                         script, downscript, vhostfdname,
                         vnet_hdr, fd, &err);
        if (err) {
            error_propagate(errp, err);
            close(fd);
            return -1;
        }
    } else { //通过命令的方式指定tap
        if (tap->has_vhostfds) {
            error_setg(errp, "vhostfds= is invalid if fds= wasn't specified");
            return -1;
        }
        script = tap->has_script ? tap->script : DEFAULT_NETWORK_SCRIPT;
        downscript = tap->has_downscript ? tap->downscript :
            DEFAULT_NETWORK_DOWN_SCRIPT;
 
        if (tap->has_ifname) { //设备名称
            pstrcpy(ifname, sizeof ifname, tap->ifname);
        } else {
            ifname[0] = '\0';
        }

        for (i = 0; i < queues; i++) {

		    /*
		     * 打开 /dev/net/tap,执行script
		     */
            fd = net_tap_init(tap, &vnet_hdr, i >= 1 ? "no" : script,
                              ifname, sizeof ifname, queues > 1, errp);
            if (fd == -1) {
                return -1;
            }

            if (queues > 1 && i == 0 && !tap->has_ifname) {
                if (tap_fd_get_ifname(fd, ifname)) {
                    error_setg(errp, "Fail to get ifname");
                    close(fd);
                    return -1;
                }
            }

			/*
			 * 创建TAPState,表示tap，设置tap的send buffer大小
			 */
            net_init_tap_one(tap, peer, "tap", name, ifname,
                             i >= 1 ? "no" : script,
                             i >= 1 ? "no" : downscript,
                             vhostfdname, vnet_hdr, fd, &err);
            if (err) {
                error_propagate(errp, err);
                close(fd);
                return -1;
            }
        }
    }

    return 0;
}

VHostNetState *tap_get_vhost_net(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);
    return s->vhost_net;
}

int tap_enable(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    int ret;

    if (s->enabled) {
        return 0;
    } else {
        ret = tap_fd_enable(s->fd);
        if (ret == 0) {
            s->enabled = true;
            tap_update_fd_handler(s);
        }
        return ret;
    }
}

int tap_disable(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    int ret;

    if (s->enabled == 0) {
        return 0;
    } else {
        ret = tap_fd_disable(s->fd);
        if (ret == 0) {
            qemu_purge_queued_packets(nc);
            s->enabled = false;
            tap_update_fd_handler(s);
        }
        return ret;
    }
}
