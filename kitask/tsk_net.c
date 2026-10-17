/* tsk_net.c - per-process network traffic without root.
 *
 * Linux counts bytes per socket only for TCP (tcp_info's bytes_received and
 * bytes_acked), and hands them to anyone through sock_diag netlink; which
 * process owns a socket comes from the inodes in /proc/PID/fd, readable only
 * for our own processes. So the network columns are TCP only, for this
 * user's processes -- UDP (QUIC, DNS, games) is not counted anywhere. A socket
 * shared by several processes (after a fork) counts for each of them. */
#include "tsk.h"
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* state: 1 known from an earlier dump, 2 seen in the current one */
typedef struct { guint64 rx, tx, prev_rx, prev_tx; int state; } Counters;

static GHashTable *socks;   /* inode -> Counters* */
static int nl = -1;
static gboolean first_dump = TRUE;

static void dump_family(int family)
{
    struct {
        struct nlmsghdr nh;
        struct inet_diag_req_v2 req;
    } msg;
    memset(&msg, 0, sizeof(msg));
    msg.nh.nlmsg_len = sizeof(msg);
    msg.nh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    msg.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    msg.req.sdiag_family = (unsigned char)family;
    msg.req.sdiag_protocol = IPPROTO_TCP;
    msg.req.idiag_states = ~0U;
    msg.req.idiag_ext = 1 << (INET_DIAG_INFO - 1);
    struct sockaddr_nl sa = {.nl_family = AF_NETLINK};
    if (sendto(nl, &msg, sizeof(msg), 0, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        return;
    static char buf[32768];
    for (;;) {
        ssize_t n = recv(nl, buf, sizeof(buf), 0);
        if (n <= 0)
            return;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (size_t)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR)
                return;
            struct inet_diag_msg *m = NLMSG_DATA(h);
            if (!m->idiag_inode)
                continue;
            int len = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(*m)));
            for (struct rtattr *a = (struct rtattr *)(m + 1); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
                if (a->rta_type != INET_DIAG_INFO)
                    continue;
                struct tcp_info ti;
                memset(&ti, 0, sizeof(ti));
                memcpy(&ti, RTA_DATA(a), MIN(sizeof(ti), (size_t)RTA_PAYLOAD(a)));
                guint64 ino = m->idiag_inode;
                Counters *c = g_hash_table_lookup(socks, &ino);
                if (!c) {
                    c = g_new0(Counters, 1);
                    guint64 *key = g_new(guint64, 1);
                    *key = ino;
                    g_hash_table_insert(socks, key, c);
                }
                c->rx = ti.tcpi_bytes_received;
                c->tx = ti.tcpi_bytes_acked;
                if (first_dump) {   /* history before kitask started isn't traffic now */
                    c->prev_rx = c->rx;
                    c->prev_tx = c->tx;
                }
                c->state = 2;
            }
        }
    }
}

void tsk_net_sample(void)
{
    if (!socks)
        socks = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    if (nl < 0)
        nl = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (nl < 0)
        return;
    /* the last dump's numbers become the previous ones; a socket new since
     * then starts from 0, so its whole traffic counts */
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, socks);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
        Counters *c = v;
        c->prev_rx = c->rx;
        c->prev_tx = c->tx;
        c->state = 1;
    }
    dump_family(AF_INET);
    dump_family(AF_INET6);
    g_hash_table_iter_init(&it, socks);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
        Counters *c = v;
        if (c->state == 1)
            g_hash_table_iter_remove(&it);   /* closed */
    }
    first_dump = FALSE;
}

gboolean tsk_net_bytes(guint64 inode, guint64 *rx, guint64 *tx)
{
    Counters *c = socks ? g_hash_table_lookup(socks, &inode) : NULL;
    if (!c)
        return FALSE;
    *rx = c->rx;
    *tx = c->tx;
    return TRUE;
}

void tsk_net_delta(guint64 inode, double *rx, double *tx)
{
    *rx = *tx = 0;
    Counters *c = socks ? g_hash_table_lookup(socks, &inode) : NULL;
    if (!c)
        return;
    *rx = (double)(c->rx >= c->prev_rx ? c->rx - c->prev_rx : 0);
    *tx = (double)(c->tx >= c->prev_tx ? c->tx - c->prev_tx : 0);
}
