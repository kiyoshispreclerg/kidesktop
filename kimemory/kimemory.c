/* kimemory - command-line client for kimemoryd's control socket. */
#include "km_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define KIMEMORY_VERSION "0.1.1"

static void usage(void)
{
    printf("Usage: kimemory COMMAND\n"
           "  list [-n N] [--active | --window ID] [--scope window|app|doc] [QUERY]\n"
           "                   history, newest first; --active/--window keep only items\n"
           "                   copied from or pasted into that window's app (default scope)\n"
           "  show ID          print an item's text (or an image's file path)\n"
           "  paste ID         put an item back on the clipboard\n"
           "  fav ID | unfav ID\n"
           "  rm ID\n"
           "  clear [--all]    remove every non-favourite (--all: favourites too)\n"
           "  status\n"
           "  --version\n");
}

static char *request(const char *req)
{
    const char *run = getenv("XDG_RUNTIME_DIR");
    const char *d = getenv("DISPLAY");
    const char *colon = d ? strrchr(d, ':') : NULL;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/kimemory-ctl.%d.sock",
             run && *run ? run : "/tmp", colon ? atoi(colon + 1) : 0);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "kimemory: kimemoryd is not running (%s)\n", addr.sun_path);
        exit(1);
    }
    if (write(fd, req, strlen(req)) < 0 || write(fd, "\n", 1) < 0) {
        close(fd);
        exit(1);
    }
    size_t len = 0, cap = 65536;
    char *buf = malloc(cap);
    ssize_t r;
    while (buf && (r = read(fd, buf + len, cap - len - 1)) > 0) {
        len += (size_t)r;
        if (len + 1 == cap) {
            char *nb = realloc(buf, cap *= 2);
            if (!nb)
                break;
            buf = nb;
        }
    }
    close(fd);
    if (!buf)
        exit(1);
    buf[len] = '\0';
    return buf;
}

static int check_ok(const char *resp)
{
    if (strstr(resp, "\"ok\":true"))
        return 1;
    char err[256] = "unknown error";
    km_json_get_str(resp, "error", err, sizeof(err));
    fprintf(stderr, "kimemory: %s\n", err);
    return 0;
}

/* Preview on one line, at most `max` columns-ish (bytes, UTF-8 safe). */
static void print_oneline(const char *s, size_t max)
{
    size_t n = 0;
    for (; *s && n < max; s++, n++)
        putchar(*s == '\n' || *s == '\t' ? ' ' : *s);
    while (*s && ((unsigned char)*s & 0xC0) == 0x80)
        putchar(*s++);
    if (*s)
        fputs("…", stdout);
}

static int cmd_list(int argc, char **argv)
{
    long limit = 30;
    const char *win = NULL, *scope = NULL, *query = "";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc)
            limit = atol(argv[++i]);
        else if (!strcmp(argv[i], "--active"))
            win = "active";
        else if (!strcmp(argv[i], "--window") && i + 1 < argc)
            win = argv[++i];
        else if (!strcmp(argv[i], "--scope") && i + 1 < argc)
            scope = argv[++i];
        else
            query = argv[i];
    }
    if (win && !scope)
        scope = "app";

    char req[1024] = "{\"cmd\":\"LIST\",\"query\":\"";
    km_json_escape(req, sizeof(req) - 200, query, 255);
    size_t l = strlen(req);
    snprintf(req + l, sizeof(req) - l, "\",\"limit\":%ld,\"scope\":\"%s\",\"win\":\"%s\"}",
             limit, scope ? scope : "all", win ? win : "");

    char *resp = request(req);
    if (!check_ok(resp)) {
        free(resp);
        return 1;
    }
    char v[512];
    if (km_json_get_str(resp, "for_app", v, sizeof(v))) {
        char doc[512] = "";
        km_json_get_str(resp, "for_doc", doc, sizeof(doc));
        printf("for %s%s%s (scope %s)\n", v, doc[0] ? ": " : "", doc, scope);
    }
    /* One item object per line. */
    for (char *line = strtok(resp, "\n"); line; line = strtok(NULL, "\n")) {
        if (strncmp(line, "{\"id\":", 6) != 0 && strncmp(line, ",{\"id\":", 7) != 0)
            continue;
        long id = 0, ts = 0, fav = 0, cur = 0, pastes = 0;
        char type[16] = "", preview[1024] = "", src[128] = "", dsts[512] = "";
        km_json_get_long(line, "id", &id);
        km_json_get_long(line, "ts", &ts);
        km_json_get_long(line, "fav", &fav);
        km_json_get_long(line, "current", &cur);
        km_json_get_long(line, "pastes", &pastes);
        km_json_get_str(line, "type", type, sizeof(type));
        km_json_get_str(line, "preview", preview, sizeof(preview));
        km_json_get_str(line, "src_app", src, sizeof(src));
        km_json_get_str(line, "dst_apps", dsts, sizeof(dsts));

        char when[32];
        time_t t = (time_t)ts;
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&t));
        printf("%c%c%4ld  %s  %-5s  %s", cur ? '>' : ' ', fav ? '*' : ' ', id, when, type,
               src[0] ? src : "?");
        if (dsts[0])
            printf(" -> %s", dsts);
        fputs("  ", stdout);
        if (!strcmp(type, "image"))
            fputs("[image]", stdout);
        else
            print_oneline(preview, 60);
        putchar('\n');
    }
    free(resp);
    return 0;
}

static int simple_cmd(const char *fmt, const char *arg)
{
    char req[256];
    snprintf(req, sizeof(req), fmt, arg ? atol(arg) : 0L);
    char *resp = request(req);
    int ok = check_ok(resp);
    free(resp);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        usage();
        return argc < 2;
    }
    const char *cmd = argv[1];
    const char *arg = argc > 2 ? argv[2] : NULL;

    if (!strcmp(cmd, "--version") || !strcmp(cmd, "-V")) {
        printf("kimemory %s\n", KIMEMORY_VERSION);
        return 0;
    }
    if (!strcmp(cmd, "list"))
        return cmd_list(argc - 2, argv + 2);

    int needs_id = !strcmp(cmd, "show") || !strcmp(cmd, "paste") || !strcmp(cmd, "fav") ||
                   !strcmp(cmd, "unfav") || !strcmp(cmd, "rm");
    if (needs_id && !arg) {
        fprintf(stderr, "kimemory: %s needs an item id\n", cmd);
        return 2;
    }

    if (!strcmp(cmd, "show")) {
        char req[64];
        snprintf(req, sizeof(req), "{\"cmd\":\"GET\",\"id\":%ld}", atol(arg));
        char *resp = request(req);
        if (!check_ok(resp)) {
            free(resp);
            return 1;
        }
        size_t cap = strlen(resp) + 1;
        char *text = malloc(cap);
        if (text && km_json_get_str(resp, "text", text, cap))
            fputs(text, stdout);
        else if (text && km_json_get_str(resp, "file", text, cap))
            puts(text);
        free(text);
        free(resp);
        return 0;
    }
    if (!strcmp(cmd, "paste"))
        return simple_cmd("{\"cmd\":\"SET\",\"id\":%ld}", arg);
    if (!strcmp(cmd, "fav"))
        return simple_cmd("{\"cmd\":\"FAV\",\"id\":%ld,\"fav\":1}", arg);
    if (!strcmp(cmd, "unfav"))
        return simple_cmd("{\"cmd\":\"FAV\",\"id\":%ld,\"fav\":0}", arg);
    if (!strcmp(cmd, "rm"))
        return simple_cmd("{\"cmd\":\"REMOVE\",\"id\":%ld}", arg);
    if (!strcmp(cmd, "clear")) {
        int all = arg && !strcmp(arg, "--all");
        char *resp = request(all ? "{\"cmd\":\"CLEAR\",\"keep_favs\":0}" : "{\"cmd\":\"CLEAR\",\"keep_favs\":1}");
        int ok = check_ok(resp);
        long removed = 0;
        km_json_get_long(resp, "removed", &removed);
        if (ok)
            printf("removed %ld\n", removed);
        free(resp);
        return ok ? 0 : 1;
    }
    if (!strcmp(cmd, "status")) {
        char *resp = request("{\"cmd\":\"STATUS\"}");
        if (!check_ok(resp)) {
            free(resp);
            return 1;
        }
        char ver[32] = "";
        long count = 0, current = 0, owned = 0, disk = 0;
        km_json_get_str(resp, "version", ver, sizeof(ver));
        km_json_get_long(resp, "count", &count);
        km_json_get_long(resp, "current", &current);
        km_json_get_long(resp, "owned", &owned);
        km_json_get_long(resp, "disk_bytes", &disk);
        printf("kimemoryd %s: %ld items, %.1f KiB on disk, clipboard #%ld%s\n", ver, count,
               disk / 1024.0, current, owned ? " (served by kimemoryd)" : "");
        free(resp);
        return 0;
    }
    fprintf(stderr, "kimemory: unknown command %s\n", cmd);
    usage();
    return 2;
}
