/* km_ctl - see km_ctl.h. */
#define _GNU_SOURCE   /* accept4 */
#include "km_ctl.h"
#include "km_clip.h"
#include "km_json.h"
#include "km_store.h"
#include "../shared/xis_winident.h"

#include <ctype.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define REQ_MAX 4096
#define PREVIEW_BYTES 200

static Display *dpy;
static int ctl_fd = -1;
static char ctl_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static const char *ctl_version;

static const char *TYPE_NAMES[] = { "text", "link", "files", "image" };

/* Growable response buffer. */
static char *out;
static size_t out_len, out_cap;

static void out_reset(void)
{
    out_len = 0;
    if (out)
        out[0] = '\0';
}

static void out_add(const char *s)
{
    size_t n = strlen(s);
    if (out_len + n + 1 > out_cap) {
        size_t ncap = out_cap ? out_cap * 2 : 8192;
        while (ncap < out_len + n + 1)
            ncap *= 2;
        char *nb = realloc(out, ncap);
        if (!nb)
            return;
        out = nb;
        out_cap = ncap;
    }
    memcpy(out + out_len, s, n + 1);
    out_len += n;
}

/* Appends "key":"escaped value". */
static void out_str(const char *key, const char *val, size_t max)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "\"%s\":\"", key);
    km_json_escape(buf, sizeof(buf) - 2, val ? val : "", max);
    strcat(buf, "\"");
    out_add(buf);
}

/* ---- LIST filtering ---- */

typedef struct {
    int scope;            /* 0 all, 1 window, 2 app, 3 doc */
    XisWinIdent id;
    const char *app;
    char query[256];
} Filter;

static const char *ident_app(const XisWinIdent *id)
{
    if (id->wm_class[0])
        return id->wm_class;
    const char *b = strrchr(id->exe, '/');
    return b ? b + 1 : id->exe;
}

static int ref_matches(const Filter *f, const KmRef *r)
{
    switch (f->scope) {
    /* X reuses window ids (across sessions, and within one once a window
     * closes): the same program too, or a stale id matches a stranger. */
    case 1: return r->win == f->id.win && f->app[0] && strcasecmp(r->app, f->app) == 0;
    case 2: return f->app[0] && strcasecmp(r->app, f->app) == 0;
    case 3: return f->id.doc_hint[0] && strcmp(r->doc, f->id.doc_hint) == 0;
    default: return 1;
    }
}

static int contains_ci(const char *hay, const char *needle)
{
    if (!hay)
        return 0;
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0)
            return 1;
    return 0;
}

static int item_matches(const Filter *f, KmItem *it)
{
    if (f->scope) {
        int hit = 0;
        for (int i = 0; i < it->nsrc && !hit; i++) hit = ref_matches(f, &it->srcs[i]);
        for (int i = 0; i < it->ndst && !hit; i++) hit = ref_matches(f, &it->dsts[i]);
        if (!hit)
            return 0;
    }
    if (!f->query[0])
        return 1;
    if (it->type != KM_IMAGE && it->bytes <= 1024 * 1024 && contains_ci(km_item_text(it), f->query))
        return 1;
    for (int i = 0; i < it->nsrc; i++)
        if (contains_ci(it->srcs[i].app, f->query) || contains_ci(it->srcs[i].title, f->query) ||
            contains_ci(it->srcs[i].doc, f->query))
            return 1;
    for (int i = 0; i < it->ndst; i++)
        if (contains_ci(it->dsts[i].app, f->query) || contains_ci(it->dsts[i].title, f->query) ||
            contains_ci(it->dsts[i].doc, f->query))
            return 1;
    return 0;
}

/* Text of an item for previews: its text rep, or for a file manager's
 * copy (no text offered at all) the uri-list / copied-files list. */
static const char *item_preview_text(KmItem *it)
{
    const char *text = km_item_text(it);
    if (text || it->type != KM_FILES)
        return text;
    KmRep *r = km_item_find_rep(it, "text/uri-list");
    if (!r)
        r = km_item_find_rep(it, "x-special/gnome-copied-files");
    return r ? (const char *)km_rep_data(it, r) : NULL;
}

static void out_item(KmItem *it)
{
    char head[256];
    snprintf(head, sizeof(head), "{\"id\":%u,\"ts\":%ld,\"type\":\"%s\",\"bytes\":%zu,\"fav\":%d,\"current\":%d,",
             it->id, it->ts, TYPE_NAMES[it->type >= 0 && it->type <= KM_IMAGE ? it->type : 0], it->bytes,
             it->fav, it->id == km_clip_current());
    out_add(head);
    out_str("preview", it->type == KM_IMAGE ? "" : item_preview_text(it), PREVIEW_BYTES);
    out_add(",");
    const KmRef *src = it->nsrc ? &it->srcs[it->nsrc - 1] : NULL;
    out_str("src_app", src ? src->app : "", 64);
    out_add(",");
    out_str("src_title", src ? src->title : "", 200);
    out_add(",");
    out_str("src_doc", src ? src->doc : "", 200);
    out_add(",");
    /* Destination apps, most recent first, each once. */
    char dsts[512] = "";
    for (int i = it->ndst - 1; i >= 0; i--) {
        char probe[80];
        snprintf(probe, sizeof(probe), ",%s,", it->dsts[i].app);
        char have[520];
        snprintf(have, sizeof(have), ",%s,", dsts);
        if (strstr(have, probe))
            continue;
        if (strlen(dsts) + strlen(it->dsts[i].app) + 2 >= sizeof(dsts))
            break;
        if (dsts[0])
            strcat(dsts, ",");
        strcat(dsts, it->dsts[i].app);
    }
    out_str("dst_apps", dsts, sizeof(dsts));
    char tail[64];
    snprintf(tail, sizeof(tail), ",\"pastes\":%d}", it->ndst);
    out_add(tail);
}

static void cmd_list(const char *req)
{
    Filter f;
    memset(&f, 0, sizeof(f));
    char scope[16] = "", win[32] = "";
    long limit = 50;
    km_json_get_str(req, "scope", scope, sizeof(scope));
    km_json_get_str(req, "win", win, sizeof(win));
    km_json_get_str(req, "query", f.query, sizeof(f.query));
    km_json_get_long(req, "limit", &limit);
    f.scope = !strcmp(scope, "window") ? 1 : !strcmp(scope, "app") ? 2 : !strcmp(scope, "doc") ? 3 : 0;

    if (f.scope) {
        Window w = !strcmp(win, "active") || !win[0] ? xis_winident_active(dpy)
                                                     : (Window)strtoul(win, NULL, 0);
        Window top = xis_winident_toplevel_for(dpy, w);
        if (!xis_winident_get(dpy, top != None ? top : w, &f.id)) {
            out_add("{\"ok\":false,\"error\":\"no such window\"}\n");
            return;
        }
        f.app = ident_app(&f.id);
    }

    out_add("{\"ok\":true,");
    if (f.scope) {
        out_str("for_app", f.app, 64);
        out_add(",");
        out_str("for_title", f.id.title, 200);
        out_add(",");
        out_str("for_doc", f.id.doc_hint, 200);
        out_add(",");
    }
    out_add("\"items\":[\n");
    int shown = 0;
    for (int i = 0; i < km_store_count() && (limit <= 0 || shown < limit); i++) {
        KmItem *it = km_store_at(i);
        if (item_matches(&f, it)) {
            if (shown++)
                out_add(",\n");
            out_item(it);
        }
        km_item_unload(it);   /* the text search may have read it from disk */
    }
    out_add("\n]}\n");
}

static void cmd_get(unsigned id)
{
    KmItem *it = km_store_get(id);
    if (!it) {
        out_add("{\"ok\":false,\"error\":\"no such item\"}\n");
        return;
    }
    out_add("{\"ok\":true,");
    if (it->type == KM_IMAGE) {
        KmRep *png = km_item_find_rep(it, "image/png");
        char path[1024];
        if (png && km_rep_path(it, png, path, sizeof(path)))
            out_str("file", path, sizeof(path));
        else
            out_str("error", "image not on disk (persist setting)", 64);
    } else {
        const char *text = item_preview_text(it);
        /* Escaping can take up to 6x; build it in its own buffer. */
        size_t n = text ? strlen(text) : 0;
        char *esc = calloc(1, n * 6 + 16);
        if (esc) {
            km_json_escape(esc, n * 6 + 16, text ? text : "", n);
            out_add("\"text\":\"");
            out_add(esc);
            out_add("\"");
            free(esc);
        }
    }
    out_add("}\n");
    km_item_unload(it);
}

static void handle(const char *req)
{
    char cmd[16] = "";
    long id = 0, v = 0;
    km_json_get_str(req, "cmd", cmd, sizeof(cmd));
    km_json_get_long(req, "id", &id);

    char buf[256];
    if (!strcasecmp(cmd, "PING")) {
        out_add("{\"ok\":true,\"pong\":true}\n");
    } else if (!strcasecmp(cmd, "STATUS")) {
        snprintf(buf, sizeof(buf),
                 "{\"ok\":true,\"version\":\"%s\",\"count\":%d,\"current\":%u,\"owned\":%d,\"disk_bytes\":%zu}\n",
                 ctl_version, km_store_count(), km_clip_current(), km_clip_owned(), km_store_disk_bytes());
        out_add(buf);
    } else if (!strcasecmp(cmd, "LIST")) {
        cmd_list(req);
    } else if (!strcasecmp(cmd, "GET")) {
        cmd_get((unsigned)id);
    } else if (!strcasecmp(cmd, "SET")) {
        if (!km_store_get((unsigned)id))
            out_add("{\"ok\":false,\"error\":\"no such item\"}\n");
        else
            out_add(km_clip_set_current((unsigned)id) ? "{\"ok\":true}\n"
                                                      : "{\"ok\":false,\"error\":\"could not own the clipboard\"}\n");
    } else if (!strcasecmp(cmd, "FAV")) {
        if (!km_json_get_long(req, "fav", &v))
            v = 1;
        out_add(km_store_set_fav((unsigned)id, (int)v) ? "{\"ok\":true}\n" : "{\"ok\":false,\"error\":\"no such item\"}\n");
    } else if (!strcasecmp(cmd, "REMOVE")) {
        out_add(km_store_remove((unsigned)id) ? "{\"ok\":true}\n" : "{\"ok\":false,\"error\":\"no such item\"}\n");
    } else if (!strcasecmp(cmd, "CLEAR")) {
        if (!km_json_get_long(req, "keep_favs", &v))
            v = 1;
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"removed\":%d}\n", km_store_clear((int)v));
        out_add(buf);
    } else {
        out_add("{\"ok\":false,\"error\":\"unknown command\"}\n");
    }
}

int km_ctl_init(Display *d, const char *path, const char *version)
{
    dpy = d;
    ctl_version = version;
    if (strlen(path) >= sizeof(ctl_path)) {
        fprintf(stderr, "kimemoryd: control socket path too long for a unix socket: %s\n", path);
        return 0;
    }
    snprintf(ctl_path, sizeof(ctl_path), "%s", path);
    ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (ctl_fd < 0)
        return 0;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ctl_path);
    unlink(ctl_path);   /* the instance lock is already ours: any socket here is stale */
    mode_t old = umask(0077);
    int ok = bind(ctl_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    umask(old);
    if (!ok || listen(ctl_fd, 4) != 0) {
        close(ctl_fd);
        ctl_fd = -1;
        return 0;
    }
    chmod(ctl_path, 0600);
    return 1;
}

int km_ctl_fd(void)
{
    return ctl_fd;
}

void km_ctl_accept(void)
{
    int conn = accept4(ctl_fd, NULL, NULL, SOCK_CLOEXEC);
    if (conn < 0)
        return;

    /* Clients send their one line right away; don't let a silent one
     * stall the X loop. */
    char req[REQ_MAX];
    size_t n = 0;
    struct pollfd p = { .fd = conn, .events = POLLIN };
    while (n < sizeof(req) - 1 && poll(&p, 1, 300) > 0) {
        ssize_t r = read(conn, req + n, sizeof(req) - 1 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
        if (memchr(req, '\n', n))
            break;
    }
    req[n] = '\0';

    if (n) {
        out_reset();
        handle(req);
        size_t off = 0;
        while (off < out_len) {
            ssize_t w = write(conn, out + off, out_len - off);
            if (w <= 0)
                break;
            off += (size_t)w;
        }
    }
    close(conn);
}

void km_ctl_close(void)
{
    if (ctl_fd >= 0) {
        close(ctl_fd);
        unlink(ctl_path);
        ctl_fd = -1;
    }
}
