#include "config.h"
#include "libtrace.h"
#include "libtrace_int.h"
#include "format_linux_xdp.h"

#include <bpf/libbpf.h>
#include <bpf/xsk.h>
#include <bpf/bpf.h>

#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <net/if.h>
#include <assert.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/param.h>

#include <linux/ethtool.h>
#include <linux/if_xdp.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>

#include <linux/if_link.h>

#define FORMAT_DATA ((xdp_format_data_t *)(libtrace->format_data))
#define PACKET_META ((libtrace_xdp_meta_t *)(packet->header))

#define FRAME_HEADROOM     sizeof(libtrace_xdp_meta_t)
#define NUM_FRAMES         4096
#define FRAME_SIZE         XSK_UMEM__DEFAULT_FRAME_SIZE
#define RX_BATCH_SIZE      64
#define INVALID_UMEM_FRAME UINT64_MAX

struct xsk_config {
    __u32 xdp_flags;
    int ifindex;
    char ifname[IF_NAMESIZE];
    char progsec[32];
    bool do_unload;
    __u16 xsk_bind_flags;
    struct bpf_object *bpf_obj;
    struct bpf_program *bpf_prg;
};

struct xsk_umem_info {
    struct xsk_ring_cons cq;
    struct xsk_ring_prod fq;
    struct xsk_umem *umem;
    int xsk_if_queue;
    void *buffer;
};

struct xsk_socket_info {
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    struct xsk_umem_info *umem;
    struct xsk_socket *xsk;
};

struct xsk_per_stream {
    struct xsk_umem_info *umem;
    struct xsk_socket_info *xsk;

    /* keep track of the number of previously processed packets */
    unsigned int prev_rcvd;

    /* previous timestamp for this stream */
    uint64_t prev_sys_time;
};

typedef struct xdp_format_data {
    struct xsk_config cfg;
    libtrace_list_t *per_stream;
} xdp_format_data_t;


static int linux_xdp_prepare_packet(libtrace_t *libtrace, libtrace_packet_t *packet,
    void *buffer, libtrace_rt_types_t rt_type, uint32_t flags);
static int linux_xdp_start_stream(libtrace_t *libtrace, struct xsk_per_stream *stream,
    int ifqueue);
static void xsk_populate_fill_ring(struct xsk_umem_info *umem);

static int linux_xdp_send_ioctl_ethtool(struct ethtool_channels *channels, char *ifname);
static int linux_xdp_get_max_queues(char *ifname);
static int linux_xdp_get_current_queues(char *ifname);
static int linux_xdp_set_current_queues(char *ifname, int queues);

static int linux_xdp_link_attach(int ifindex, __u32 xdp_flags, int prog_fd) {

    int err;

    err = bpf_set_link_xdp_fd(ifindex, prog_fd, xdp_flags);
    if (err == -EEXIST && !(xdp_flags & XDP_FLAGS_UPDATE_IF_NOEXIST)) {
        /* Force mode didn't work, probably because a program of the
         * opposite type is loaded. Let's unload that and try loading
         * again.
         */

        __u32 old_flags = xdp_flags;

        xdp_flags &= ~XDP_FLAGS_MODES;
        xdp_flags |= (old_flags & XDP_FLAGS_SKB_MODE) ? XDP_FLAGS_DRV_MODE : XDP_FLAGS_SKB_MODE;
        err = bpf_set_link_xdp_fd(ifindex, -1, xdp_flags);
        if (!err)
            err = bpf_set_link_xdp_fd(ifindex, prog_fd, old_flags);
    }

    if (err < 0) {
        fprintf(stderr, "ERR: "
            "ifindex(%d) link set xdp fd failed (%d): %s\n",
            ifindex, -err, strerror(-err));

        switch (-err) {
        case EBUSY:
        case EEXIST:
            fprintf(stderr, "Hint: XDP already loaded on device"
                " use --force to swap/replace\n");
            break;
        case EOPNOTSUPP:
            fprintf(stderr, "Hint: Native-XDP not supported"
                " use --skb-mode or --auto-mode\n");
            break;
        default:
            break;
        }
        return EXIT_FAIL_XDP;
    }

    return EXIT_OK;

}

static struct bpf_object *linux_xdp_load_bpf_object(int ifindex) {

    struct bpf_object *obj;
    int first_prog_fd = -1;
    int err;

    struct bpf_prog_load_attr prog_load_attr = {
        .prog_type = BPF_PROG_TYPE_XDP,
        .ifindex = ifindex,
        .file = xdp_filename,
    };

    err = bpf_prog_load_xattr(&prog_load_attr, &obj, &first_prog_fd);
    if (err) {
        fprintf(stderr, "Error loading BPF-OBJ file\n");
        return NULL;
    }

    return obj;
}

struct bpf_object *linux_xdp_load_bpf_and_attach(struct xsk_config *cfg) {

    int err;
    int prog_fd;

    cfg->bpf_obj = linux_xdp_load_bpf_object(cfg->ifindex);
    if (cfg->bpf_obj == NULL) {
        fprintf(stderr, "Error loading bpf file\n");
        exit(1);
    }

    cfg->bpf_prg = bpf_object__find_program_by_title(cfg->bpf_obj, xdp_progname);
    if (cfg->bpf_prg == NULL) {
        fprintf(stderr, "Error finding bpf program %s\n", xdp_progname);
        exit (1);
    }

    prog_fd = bpf_program__fd(cfg->bpf_prg);
    if (prog_fd <= 0) {
        fprintf(stderr, "Error getting bpf program FD\n");
        exit (1);
    }

    err = linux_xdp_link_attach(cfg->ifindex, cfg->xdp_flags, prog_fd);

    return cfg->bpf_obj;
}

static int linux_xdp_send_ioctl_ethtool(struct ethtool_channels *channels, char *ifname) {

    struct ifreq ifr = {};
    int fd, err, ret;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -errno;

    ifr.ifr_data = (void *)channels;
    memcpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    err = ioctl(fd, SIOCETHTOOL, &ifr);
    if (err && errno != EOPNOTSUPP) {
        ret = -errno;
        goto out;
    }

    if (err) {
        ret = 1;
    } else {
        ret = 0;
    }

out:
    close(fd);
    return ret;
}

static int linux_xdp_get_max_queues(char *ifname) {

    struct ethtool_channels channels = { .cmd = ETHTOOL_GCHANNELS };
    int ret = -1;

    if (linux_xdp_send_ioctl_ethtool(&channels, ifname) == 0) {
        ret = MAX(channels.max_rx, channels.max_tx);
        ret = MAX(ret, (int)channels.max_combined);
    }

    return ret;
}

static int linux_xdp_get_current_queues(char *ifname) {
    struct ethtool_channels channels = { .cmd = ETHTOOL_GCHANNELS };
    int ret = -1;

    if (linux_xdp_send_ioctl_ethtool(&channels, ifname) == 0) {
        ret = MAX(channels.rx_count, channels.tx_count);
        ret = MAX(ret, (int)channels.combined_count);
    }

    return ret;
}

static int linux_xdp_set_current_queues(char *ifname, int queues) {
    struct ethtool_channels channels = { .cmd = ETHTOOL_GCHANNELS };
    __u32 org_combined;

    /* get the current settings */
    if (linux_xdp_send_ioctl_ethtool(&channels, ifname) == 0) {

        org_combined = channels.combined_count;
        channels.cmd = ETHTOOL_SCHANNELS;
        channels.combined_count = queues;
        /* try update */
        if (linux_xdp_send_ioctl_ethtool(&channels, ifname) == 0) {
            /* success */
            return channels.combined_count;
        }

        /* try set rx and tx individually */
        channels.rx_count = queues;
        channels.tx_count = queues;
        channels.combined_count = org_combined;
        /* try again */
        if (linux_xdp_send_ioctl_ethtool(&channels, ifname) == 0) {
            /* success */
            return channels.rx_count;
        }
    }

    /* could not set the number of queues */
    return -1;
}

static struct xsk_umem_info *configure_xsk_umem(void *buffer, uint64_t size,
    int interface_queue) {

    struct xsk_umem_info *umem;
    struct xsk_umem_config umem_cfg;
    int ret;

    umem = calloc(1, sizeof(*umem));
    if (umem == NULL) {
        return NULL;
    }

    umem_cfg.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    umem_cfg.comp_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    umem_cfg.frame_size = FRAME_SIZE;
    umem_cfg.frame_headroom = FRAME_HEADROOM;
    umem_cfg.flags = XSK_UMEM__DEFAULT_FLAGS;

    ret = xsk_umem__create(&umem->umem,
                           buffer,
                           size,
                           &umem->fq,
                           &umem->cq,
                           &umem_cfg);
    if (ret) {
        errno = -ret;
        return NULL;
    }

    umem->buffer = buffer;
    umem->xsk_if_queue = interface_queue;

    /* populate fill ring */
    xsk_populate_fill_ring(umem);

    return umem;
}

static struct xsk_socket_info *xsk_configure_socket(struct xsk_config *cfg,
                                                    struct xsk_umem_info *umem) {

    struct xsk_socket_config xsk_cfg;
    struct xsk_socket_info *xsk_info;
    uint32_t prog_id = 0;
    int ret;

    xsk_info = calloc(1, sizeof(*xsk_info));
    if (xsk_info == NULL) {
        return NULL;
    }

    xsk_info->umem = umem;
    xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    xsk_cfg.libbpf_flags = 0;
    xsk_cfg.xdp_flags = cfg->xdp_flags;
    xsk_cfg.bind_flags = cfg->xsk_bind_flags;

    ret = xsk_socket__create(&xsk_info->xsk,
                             cfg->ifname,
                             umem->xsk_if_queue,
                             umem->umem,
                             &xsk_info->rx,
                             NULL,
                             &xsk_cfg);
    if (ret) {
        errno = -ret;
        return NULL;
    }

    ret = bpf_get_link_xdp_id(cfg->ifindex, &prog_id, cfg->xdp_flags);
    if (ret) {
        errno = -ret;
        return NULL;
    }

    return xsk_info;
}

static void xsk_populate_fill_ring(struct xsk_umem_info *umem) {

    int ret, i;
    uint32_t idx;

    ret = xsk_ring_prod__reserve(&umem->fq,
                                 XSK_RING_PROD__DEFAULT_NUM_DESCS,
                                 &idx);
    if (ret != XSK_RING_PROD__DEFAULT_NUM_DESCS) {
        fprintf(stderr," error allocating xdp fill queue\n");
        return;
    }

    for (i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS; i++) {
        *xsk_ring_prod__fill_addr(&umem->fq, idx++) =
            i * FRAME_SIZE;
    }

    xsk_ring_prod__submit(&umem->fq, XSK_RING_PROD__DEFAULT_NUM_DESCS);

}

static uint64_t linux_xdp_get_time(struct xsk_per_stream *stream) {

    uint64_t sys_time;
    struct timespec ts = {0};
    clock_gettime(CLOCK_REALTIME, &ts);

    sys_time = ((uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec);

    if (stream->prev_sys_time >= sys_time) {
        sys_time = stream->prev_sys_time + 1;
    }

    stream->prev_sys_time = sys_time;

    return sys_time;
}

static int linux_xdp_init_input(libtrace_t *libtrace) {

    struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
    char *scan = NULL;

    if (setrlimit(RLIMIT_MEMLOCK, &r)) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable "
            "to setrlimit(RLIMIT_MEMLOCK) in linux_xdp_init_input");
        return -1;
    }

    // allocate space for the format data
    libtrace->format_data = (xdp_format_data_t *)calloc(1,
        sizeof(xdp_format_data_t));
    if (libtrace->format_data == NULL) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable "
            "to allocate memory for format data in linux_xdp_init_input()");
        return -1;
    }

    /* setup XDP config */
    scan = strchr(libtrace->uridata, ':');
    if (scan == NULL) {
        memcpy(FORMAT_DATA->cfg.ifname, libtrace->uridata, strlen(libtrace->uridata));
    } else {
        memcpy(FORMAT_DATA->cfg.ifname, scan + 1, strlen(scan + 1));
    }
    FORMAT_DATA->cfg.ifindex = if_nametoindex(FORMAT_DATA->cfg.ifname);
    if (FORMAT_DATA->cfg.ifindex == -1) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Invalid interface "
            "name.");
        return -1;
    }

    /* setup list to hold the streams */
    FORMAT_DATA->per_stream = libtrace_list_init(sizeof(struct xsk_per_stream));
    if (FORMAT_DATA->per_stream == NULL) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable to create list "
            "for stream data in linux_xdp_init_input()");
        return -1;
    }

    return 0;
}

static int linux_xdp_pstart_input(libtrace_t *libtrace) {

    int i;
    int threads = libtrace->perpkt_thread_count;
    struct xsk_per_stream empty_stream = {NULL,NULL,0,0};
    struct xsk_per_stream *stream;

    /* set the number of nic queues to match number of threads */
    if (linux_xdp_set_current_queues(FORMAT_DATA->cfg.ifname, threads) < 0) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable to set number of NIC queues "
            "to match the number of processing threads, try reduce the number of threads");
        return -1;
    }

    /* create a stream for each processing thread */
    for (i = 0; i < threads; i++) {
        libtrace_list_push_back(FORMAT_DATA->per_stream, &empty_stream);

        stream = libtrace_list_get_index(FORMAT_DATA->per_stream, i)->data;

        /* start the stream */
        linux_xdp_start_stream(libtrace, stream, i);
    }

    return 0;
}

static int linux_xdp_start_input(libtrace_t *libtrace) {

    struct xsk_per_stream empty_stream = {NULL,NULL,0,0};
    struct xsk_per_stream *stream;

    /* insert empty stream into the list */
    libtrace_list_push_back(FORMAT_DATA->per_stream, &empty_stream);

    /* get the stream from the list */
    stream = libtrace_list_get_index(FORMAT_DATA->per_stream, 0)->data;

    /* start the stream */
    return linux_xdp_start_stream(libtrace, stream, 0);

}

static int linux_xdp_start_stream(libtrace_t *libtrace, struct xsk_per_stream *stream, int ifqueue) {

    uint64_t pkt_buf_size;
    void *pkt_buf;

    /* Allocate memory for NUM_FRAMES of default XDP frame size */
    pkt_buf_size = NUM_FRAMES * FRAME_SIZE;
    pkt_buf = mmap(NULL, pkt_buf_size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    /* setup umem */
    stream->umem = configure_xsk_umem(pkt_buf, pkt_buf_size, ifqueue);
    if (stream->umem == NULL) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable "
            "to setup BPF umem in linux_xdp_start_stream()");
        return -1;
    }

    /* configure socket */
    stream->xsk = xsk_configure_socket(&FORMAT_DATA->cfg, stream->umem);
    if (stream->xsk == NULL) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable "
            "to configure AF_XDP socket interface:%s queue:%d "
            " in linux_xdp_start_stream()",
            FORMAT_DATA->cfg.ifname,
            stream->umem->xsk_if_queue);
        return -1;
    }

    return 0;
}

static int linux_xdp_read_stream(libtrace_t *libtrace,
                                 libtrace_packet_t *packet[],
                                 struct xsk_per_stream *stream,
                                 size_t nb_packets) {

    unsigned int rcvd = 0;
    uint32_t idx_rx = 0;
    uint32_t pkt_len;
    uint64_t pkt_addr;
    uint8_t *pkt_buffer;
    unsigned int i;
    libtrace_xdp_meta_t *meta;
    struct pollfd fds;
    int ret;

    if (libtrace->format_data == NULL) {
        trace_set_err(libtrace, TRACE_ERR_BAD_FORMAT, "Trace format data missing, "
            "call trace_create() before calling trace_read_packet()");
        return -1;
    }

    /* free previously used frames */
    if (stream->prev_rcvd != 0) {
        xsk_ring_prod__submit(&stream->umem->fq, stream->prev_rcvd);
        xsk_ring_cons__release(&stream->xsk->rx, stream->prev_rcvd);
    }

    /* check nb_packets (request packets) is not larger than the max RX_BATCH_SIZE */
    if (nb_packets > RX_BATCH_SIZE) {
        nb_packets = RX_BATCH_SIZE;
    }

    fds.fd = xsk_socket__fd(stream->xsk->xsk);
    fds.events = POLLIN;

    /* try get nb_packets */
    while (rcvd < 1) {
        rcvd = xsk_ring_cons__peek(&stream->xsk->rx, nb_packets, &idx_rx);

        /* check if libtrace has halted */
        if ((ret = is_halted(libtrace)) != -1) {
            return ret;
        }

        /* was a packet received? if not poll for a short amount of time */
        if (rcvd < 1) {
            /* poll will return 0 on timeout or a positive on a event */
            ret = poll(&fds, 1, 500);

            /* poll encountered a error */
            if (ret < 0) {
                trace_set_err(libtrace, errno, "poll error() XDP");
                return -1;
            }
        }
    }

    for (i = 0; i < rcvd; i++) {

        /* got a packet. Get the address and length from the rx descriptor */
        pkt_addr = xsk_ring_cons__rx_desc(&stream->xsk->rx, idx_rx)->addr;
        pkt_len = xsk_ring_cons__rx_desc(&stream->xsk->rx, idx_rx)->len;

        /* get pointer to its contents, this gives us pointer to packet payload
         * and not the start of the headroom allocated?? */
        pkt_buffer = xsk_umem__get_data(stream->xsk->umem->buffer, pkt_addr);

        /* prepare the packet */
        packet[i]->buf_control = TRACE_CTRL_EXTERNAL;
        packet[i]->type = TRACE_RT_DATA_XDP;
        packet[i]->buffer = (uint8_t *)pkt_buffer - FRAME_HEADROOM;
        packet[i]->header = (uint8_t *)pkt_buffer - FRAME_HEADROOM;
        packet[i]->payload = pkt_buffer;
        packet[i]->trace = libtrace;
        packet[i]->error = 1;

        meta = (libtrace_xdp_meta_t *)packet[i]->buffer;
        meta->timestamp = linux_xdp_get_time(stream);
        meta->packet_len = pkt_len;

        /* next packet */
        idx_rx++;
    }

    /* set number of received packets on this batch */
    stream->prev_rcvd = rcvd;

    return rcvd;
}

static int linux_xdp_read_packet(libtrace_t *libtrace, libtrace_packet_t *packet) {

    struct xsk_per_stream *stream;
    libtrace_list_node_t *node;

    node = libtrace_list_get_index(FORMAT_DATA->per_stream, 0);
    if (node == NULL) {
        trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Unable to get XDP "
            "input stream in linux_xdp_read_packet()");
        return -1;
    }

    stream = (struct xsk_per_stream *)node->data;

    return linux_xdp_read_stream(libtrace, &packet, stream, 1);

}

static int linux_xdp_pread_packets(libtrace_t *libtrace,
                                   libtrace_thread_t *thread,
                                   libtrace_packet_t **packets,
                                   size_t nb_packets) {

    int nb_rx;
    struct xsk_per_stream *stream = thread->format_data;

    nb_rx = linux_xdp_read_stream(libtrace, packets, stream, nb_packets);

    return nb_rx;
}

static int linux_xdp_prepare_packet(libtrace_t *libtrace UNUSED, libtrace_packet_t *packet,
    void *buffer, libtrace_rt_types_t rt_type, uint32_t flags) {

    if (packet->buffer != buffer && packet->buf_control == TRACE_CTRL_PACKET) {
        free(packet->buffer);
    }

    if ((flags & TRACE_PREP_OWN_BUFFER) == TRACE_PREP_OWN_BUFFER) {
        packet->buf_control = TRACE_CTRL_PACKET;
    } else {
        packet->buf_control = TRACE_CTRL_EXTERNAL;
    }

    packet->buf_control = TRACE_CTRL_EXTERNAL;
    packet->type = rt_type;
    packet->buffer = buffer;
    packet->header = buffer;
    packet->payload = (uint8_t *)buffer + FRAME_HEADROOM;

    return 0;
}

static int linux_xdp_fin_input(libtrace_t *libtrace) {

    size_t i;
    struct xsk_per_stream *stream;

    if (FORMAT_DATA != NULL) {
        for (i = 0; i < libtrace_list_get_size(FORMAT_DATA->per_stream); i++) {
            stream = libtrace_list_get_index(FORMAT_DATA->per_stream, i)->data;

            xsk_socket__delete(stream->xsk->xsk);
            xsk_umem__delete(stream->umem->umem);
        }

        /* destroy per stream list */
        libtrace_list_deinit(FORMAT_DATA->per_stream);

        free(FORMAT_DATA);
    }

    return 0;
}


/* link per stream data with each threads format data */
static int linux_xdp_pregister_thread(libtrace_t *libtrace,
                               libtrace_thread_t *t,
                               bool reading) {

    /* test if they nic supports multiple queues? if not use a hasher thread to disperse
     * packets across processing threads?? */

    if (reading) {
        if (t->type == THREAD_PERPKT) {
            t->format_data = libtrace_list_get_index(FORMAT_DATA->per_stream, t->perpkt_num)->data;
            if (t->format_data == NULL) {
                trace_set_err(libtrace, TRACE_ERR_INIT_FAILED, "Too many threads registered");
                return -1;
            }
        }
    }

    return 0;
}

static libtrace_linktype_t linux_xdp_get_link_type(const libtrace_packet_t *packet UNUSED) {
    return TRACE_TYPE_ETH;
}

static struct timeval linux_xdp_get_timeval(const libtrace_packet_t *packet) {

    struct timeval tv;

    tv.tv_sec = PACKET_META->timestamp / (uint64_t) 1000000000;
    tv.tv_usec = (PACKET_META->timestamp % (uint64_t) 1000000000) / 1000;

    return tv;

}

static struct timespec linux_xdp_get_timespec(const libtrace_packet_t *packet) {

    struct timespec ts;

    ts.tv_sec = PACKET_META->timestamp / (uint64_t) 1000000000;
    ts.tv_nsec = PACKET_META->timestamp % (uint64_t) 1000000000;

    return ts;
}

static int linux_xdp_get_framing_length(const libtrace_packet_t *packet UNUSED) {
    return FRAME_SIZE;
}

static int linux_xdp_get_wire_length(const libtrace_packet_t *packet) {

    return PACKET_META->packet_len;
}

static int linux_xdp_get_capture_length(const libtrace_packet_t *packet) {

    return PACKET_META->packet_len;

}

/* called when trace_destroy_packet is called */
static void linux_xdp_fin_packet(libtrace_packet_t *packet UNUSED) {



}

static struct libtrace_format_t xdp = {
    "xdp",
    "$Id$",
    TRACE_FORMAT_XDP,
    NULL,                           /* probe filename */
    NULL,                           /* probe magic */
    linux_xdp_init_input,            /* init_input */
    NULL,			        /* config_input */
    linux_xdp_start_input,           /* start_input */
    NULL,           /* pause */
    NULL,           /* init_output */
    NULL,                           /* config_output */
    NULL,          /* start_output */
    linux_xdp_fin_input,             /* fin_input */
    NULL,            /* fin_output */
    linux_xdp_read_packet,           /* read_packet */
    linux_xdp_prepare_packet,        /* prepare_packet */
    linux_xdp_fin_packet,            /* fin_packet */
    NULL,          /* write_packet */
    NULL,                           /* flush_output */
    linux_xdp_get_link_type,        /* get_link_type */
    NULL,                           /* get_direction */
    NULL,                           /* set_direction */
    NULL,			        /* get_erf_timestamp */
    linux_xdp_get_timeval,          /* get_timeval */
    linux_xdp_get_timespec,         /* get_timespec */
    NULL,                           /* get_seconds */
	NULL,				/* get_meta_section */
    NULL,                           /* seek_erf */
    NULL,                           /* seek_timeval */
    NULL,                           /* seek_seconds */
    linux_xdp_get_capture_length,   /* get_capture_length */
    linux_xdp_get_wire_length,      /* get_wire_length */
    linux_xdp_get_framing_length,   /* get_framing_length */
    NULL,                           /* set_capture_length */
    NULL,                           /* get_received_packets */
	NULL,                           /* get_filtered_packets */
    NULL,                           /* get_dropped_packets */
    NULL,                           /* get_statistics */
    NULL,                           /* get_fd */
    NULL,				/* trace_event */
    NULL,                           /* help */
    NULL,                       /* next pointer */
    {true, -1},                 /* Live, no thread limit */
    linux_xdp_pstart_input,		/* pstart_input */
    linux_xdp_pread_packets,	/* pread_packets */
    NULL,                       /* ppause */
    linux_xdp_fin_input,		/* p_fin */
    linux_xdp_pregister_thread,	/* register thread */
    NULL,				        /* unregister thread */
    NULL				        /* get thread stats */
};

void linux_xdp_constructor(void) {
    register_format(&xdp);
}