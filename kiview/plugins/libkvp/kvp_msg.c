/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kvp_msg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static gboolean plain(unsigned char c)
{
    return g_ascii_isalnum(c) || strchr("._~/:+,-", c) != NULL;
}

char *kvp_escape(const char *s)
{
    GString *o = g_string_new(NULL);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (plain(*p))
            g_string_append_c(o, (char)*p);
        else
            g_string_append_printf(o, "%%%02X", *p);
    }
    return g_string_free(o, FALSE);
}

char *kvp_unescape(const char *s)
{
    GString *o = g_string_new(NULL);
    for (const char *p = s; *p; p++) {
        if (*p == '%' && g_ascii_isxdigit(p[1]) && g_ascii_isxdigit(p[2])) {
            g_string_append_c(o, (char)(g_ascii_xdigit_value(p[1]) * 16 + g_ascii_xdigit_value(p[2])));
            p += 2;
        } else {
            g_string_append_c(o, *p);
        }
    }
    return g_string_free(o, FALSE);
}

KvpMsg *kvp_msg_parse(const char *line)
{
    char **tok = g_strsplit(line, " ", -1);
    KvpMsg *m = NULL;
    for (int i = 0; tok[i]; i++) {
        if (!*tok[i])
            continue;
        if (!m) {
            m = g_new0(KvpMsg, 1);
            m->verb = g_strdup(tok[i]);
            m->kv = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
            continue;
        }
        char *eq = strchr(tok[i], '=');
        if (!eq)
            continue; /* unknown shapes are ignored, never an error */
        *eq = '\0';
        g_hash_table_replace(m->kv, g_strdup(tok[i]), kvp_unescape(eq + 1));
    }
    g_strfreev(tok);
    return m;
}

void kvp_msg_free(KvpMsg *m)
{
    if (!m)
        return;
    g_free(m->verb);
    g_hash_table_destroy(m->kv);
    g_free(m);
}

const char *kvp_msg_get(const KvpMsg *m, const char *key)
{
    return g_hash_table_lookup(m->kv, key);
}

long kvp_msg_get_int(const KvpMsg *m, const char *key, long def)
{
    const char *v = kvp_msg_get(m, key);
    char *end;
    if (!v || !*v)
        return def;
    long r = strtol(v, &end, 10);
    return *end ? def : r;
}

double kvp_msg_get_double(const KvpMsg *m, const char *key, double def)
{
    const char *v = kvp_msg_get(m, key);
    char *end;
    if (!v || !*v)
        return def;
    double r = g_ascii_strtod(v, &end);
    return *end ? def : r;
}

GString *kvp_out(const char *verb)
{
    return g_string_new(verb);
}

void kvp_out_s(GString *m, const char *key, const char *val)
{
    char *e = kvp_escape(val ? val : "");
    g_string_append_printf(m, " %s=%s", key, e);
    g_free(e);
}

void kvp_out_i(GString *m, const char *key, long val)
{
    g_string_append_printf(m, " %s=%ld", key, val);
}

void kvp_out_d(GString *m, const char *key, double val)
{
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buf, sizeof buf, "%.6g", val);
    g_string_append_printf(m, " %s=%s", key, buf);
}

gboolean kvp_out_send(GString *m, int sock, int fd)
{
    struct iovec iov = {.iov_base = m->str, .iov_len = m->len};
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } ctl = {{0}};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
    if (fd >= 0) {
        mh.msg_control = ctl.buf;
        mh.msg_controllen = sizeof ctl.buf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    gboolean ok = m->len < KVP_MSG_MAX;
    if (ok) {
        ssize_t n;
        do
            n = sendmsg(sock, &mh, MSG_NOSIGNAL);
        while (n < 0 && errno == EINTR);
        ok = n == (ssize_t)m->len;
    }
    g_string_free(m, TRUE);
    return ok;
}

int kvp_recv(int sock, char *buf, size_t cap, int *fd)
{
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } ctl;
    struct iovec iov = {.iov_base = buf, .iov_len = cap - 1};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl.buf,
                        .msg_controllen = sizeof ctl.buf};
    ssize_t n;
    *fd = -1;
    do
        n = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
    while (n < 0 && errno == EINTR);
    if (n < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : -2;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int f;
            memcpy(&f, CMSG_DATA(c), sizeof f);
            if (*fd >= 0)
                close(*fd);
            *fd = f;
        }
    }
    if (mh.msg_flags & MSG_TRUNC) {
        if (*fd >= 0)
            close(*fd);
        *fd = -1;
        return -2;
    }
    buf[n] = '\0';
    return (int)n;
}
