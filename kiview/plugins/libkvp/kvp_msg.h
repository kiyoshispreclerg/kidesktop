/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kvp_msg - the kiview <-> plugin wire format (see kiview/PROTOCOL.md):
 * one text message per SOCK_SEQPACKET packet, "verb key=value key=value",
 * values escaped as %XX, an optional file descriptor passed along.
 * Shared by the frontend and libkvp. Numbers are always written and read
 * in the C locale. */
#ifndef KVP_MSG_H
#define KVP_MSG_H

#include <glib.h>
#include <stddef.h>

#define KVP_MSG_MAX 8192

typedef struct {
    char *verb;
    GHashTable *kv; /* key -> decoded value */
} KvpMsg;

/* NULL for an empty line. */
KvpMsg *kvp_msg_parse(const char *line);
void kvp_msg_free(KvpMsg *m);
const char *kvp_msg_get(const KvpMsg *m, const char *key);
long kvp_msg_get_int(const KvpMsg *m, const char *key, long def);
double kvp_msg_get_double(const KvpMsg *m, const char *key, double def);

char *kvp_escape(const char *s);
char *kvp_unescape(const char *s);

/* Building: kvp_out("view"), kvp_out_*(), then kvp_out_send(), which frees. */
GString *kvp_out(const char *verb);
void kvp_out_s(GString *m, const char *key, const char *val);
void kvp_out_i(GString *m, const char *key, long val);
void kvp_out_d(GString *m, const char *key, double val);
gboolean kvp_out_send(GString *m, int sock, int fd);

/* Receives one packet into buf (NUL-terminated) and the fd it carried, if
 * any (-1 otherwise). Returns the length, 0 when the peer is gone, -1 when
 * nothing is waiting (non-blocking socket), -2 on any other error. */
int kvp_recv(int sock, char *buf, size_t cap, int *fd);

#endif
