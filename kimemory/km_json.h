/* km_json - the few flat-JSON helpers kimemoryd's control socket and the
 * kimemory CLI share. Requests and response items are single-level objects
 * on one line each; values are strings or integers. Same shape as
 * xisguard's control protocol. */
#ifndef KM_JSON_H
#define KM_JSON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Appends `src` JSON-escaped (without quotes) to `out` (at most `cap`
 * bytes, always NUL-terminated); stops after `max_src` source bytes,
 * never splitting a UTF-8 sequence. */
static inline void km_json_escape(char *out, size_t cap, const char *src, size_t max_src)
{
    size_t o = strlen(out);
    if (!src)
        return;
    size_t n = strnlen(src, max_src);
    if (n == max_src)   /* cut: back up to the start of a UTF-8 character */
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    for (size_t i = 0; i < n && o + 8 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\t') {
            out[o++] = '\\'; out[o++] = 't';
        } else if (c == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static inline const char *km_json_find(const char *msg, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(msg, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':')
        p++;
    return p;
}

/* String value, unescaped. Returns 1 if the key was present. */
static inline int km_json_get_str(const char *msg, const char *key, char *out, size_t cap)
{
    const char *p = km_json_find(msg, key);
    if (!p || *p != '"')
        return 0;
    size_t o = 0;
    for (p++; *p && *p != '"' && o + 1 < cap; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p;
            if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
                char hex[5] = { p[1], p[2], p[3], p[4], 0 };
                c = (char)strtol(hex, NULL, 16);
                p += 4;
            }
            out[o++] = c;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
    return 1;
}

static inline int km_json_get_long(const char *msg, const char *key, long *out)
{
    const char *p = km_json_find(msg, key);
    if (!p || (*p != '-' && (*p < '0' || *p > '9')))
        return 0;
    *out = strtol(p, NULL, 10);
    return 1;
}

#endif /* KM_JSON_H */
