/* kiconf - "Sobre" module: system info panel. See sysinfo.h. */
#include "common.h"
#include "sysinfo.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/utsname.h>
#include <unistd.h>

/* "Full Name (login)" when the passwd GECOS field has a name (first
 * comma-separated subfield), plain login otherwise. */
static void get_user(char *out, size_t outsz)
{
    struct passwd *pw = getpwuid(getuid());
    const char *login = getenv("USER");
    if (!login || !*login) {
        login = (pw && pw->pw_name) ? pw->pw_name : "?";
    }
    char full[256] = "";
    if (pw && pw->pw_gecos) {
        snprintf(full, sizeof(full), "%s", pw->pw_gecos);
        char *comma = strchr(full, ',');
        if (comma) {
            *comma = '\0';
        }
    }
    if (full[0] && strcmp(full, login)) {
        snprintf(out, outsz, "%s (%s)", full, login);
    } else {
        snprintf(out, outsz, "%s", login);
    }
}

static void get_hostname(char *out, size_t outsz)
{
    if (gethostname(out, outsz) != 0) {
        snprintf(out, outsz, "?");
    }
}

/* PRETTY_NAME="Devuan GNU/Linux 5 (daedalus)" -> Devuan GNU/Linux 5
 * (daedalus). Falls back to NAME+VERSION, then to uname()'s sysname if
 * /etc/os-release itself is missing (non-Linux, or a very stripped-down
 * rootfs). */
static void get_distro(char *out, size_t outsz)
{
    out[0] = '\0';
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) {
        return;
    }
    char line[512], name[256] = "", version[256] = "";
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        char *val = strchr(l, '=');
        if (!val) {
            continue;
        }
        *val++ = '\0';
        size_t vlen = strlen(val);
        if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
            val[vlen - 1] = '\0';
            val++;
        }
        if (!strcmp(l, "PRETTY_NAME")) {
            snprintf(out, outsz, "%s", val);
        } else if (!strcmp(l, "NAME")) {
            snprintf(name, sizeof(name), "%s", val);
        } else if (!strcmp(l, "VERSION")) {
            snprintf(version, sizeof(version), "%s", val);
        }
    }
    fclose(f);
    if (!out[0] && name[0]) {
        snprintf(out, outsz, "%s%s%s", name, version[0] ? " " : "", version);
    }
}

static void get_kernel(char *out, size_t outsz)
{
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(out, outsz, "%s %s", u.sysname, u.release);
    } else {
        snprintf(out, outsz, "?");
    }
}

/* xdpyinfo's "vendor string:"/"vendor release number:". The release
 * number packs the server version as 1|MAJOR|MM|PPP (XLibre 25.2.0 ->
 * 12502000, X.Org 21.1.8 -> 12101008), so it decodes to the real
 * server version; the protocol "version number:" is always 11.0. */
static void get_xserver(char *out, size_t outsz)
{
    char raw[16384];
    char *argv[] = {"xdpyinfo", NULL};
    if (!run_capture(argv, raw, sizeof(raw))) {
        snprintf(out, outsz, "%s", getenv("DISPLAY") ? "X11" : "?");
        return;
    }
    char vendor[128] = "";
    long release = 0;
    char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *l = trim(line);
        if (!strncmp(l, "vendor string:", 14)) {
            snprintf(vendor, sizeof(vendor), "%s", trim(l + 14));
        } else if (!strncmp(l, "vendor release number:", 22)) {
            release = atol(trim(l + 22));
        }
    }
    char ver[48] = "";
    if (release >= 10000000) {
        snprintf(ver, sizeof(ver), " %ld.%ld.%ld", (release / 100000) % 100,
                 (release / 1000) % 100, release % 1000);
    }
    snprintf(out, outsz, "%s%sX11%s", vendor, vendor[0] ? " " : "", ver);
}

/* Memory type ("DDR5") from the SMBIOS type-17 records. Only dmidecode
 * can read them and it needs root, so this stays empty for a normal user
 * unless dmidecode is set up to run unprivileged. */
static void get_ram_type(char *out, size_t outsz)
{
    out[0] = '\0';
    char raw[65536];
    char *argv[] = {"dmidecode", "-t", "17", NULL};
    if (!run_capture(argv, raw, sizeof(raw))) {
        return;
    }
    char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *l = trim(line);
        if (strncmp(l, "Type:", 5)) {
            continue;
        }
        char *t = trim(l + 5);
        if (!strncmp(t, "DDR", 3) || !strncmp(t, "LPDDR", 5) || !strncmp(t, "SDRAM", 5)) {
            snprintf(out, outsz, "%s", t);
            return;
        }
    }
}

static void get_ram(char *out, size_t outsz)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) {
        snprintf(out, outsz, "?");
        return;
    }
    char line[256];
    long kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "MemTotal:", 9) && sscanf(line + 9, "%ld", &kb) == 1) {
            break;
        }
    }
    fclose(f);
    if (kb > 0) {
        char type[32];
        get_ram_type(type, sizeof(type));
        snprintf(out, outsz, "%.1f GiB%s%s", kb / (1024.0 * 1024.0),
                 type[0] ? " " : "", type);
    } else {
        snprintf(out, outsz, "?");
    }
}

/* First "model name" line (every core reports the same model on any
 * machine this matters for) plus a core count from counting "processor"
 * lines -- same two fields fastfetch/hardinfo show, without pulling in a
 * full CPU-topology parser for hyperthreading/sockets. */
static void get_cpu(char *out, size_t outsz)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) {
        snprintf(out, outsz, "?");
        return;
    }
    char line[512], model[256] = "";
    int cores = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "processor", 9)) {
            cores++;
        } else if (!model[0] && !strncmp(line, "model name", 10)) {
            char *colon = strchr(line, ':');
            if (colon) {
                snprintf(model, sizeof(model), "%s", trim(colon + 1));
            }
        }
    }
    fclose(f);
    struct utsname u;
    char arch[96] = "";
    if (uname(&u) == 0) {
        int bits = strstr(u.machine, "64") ? 64 : (strstr(u.machine, "86") || !strncmp(u.machine, "arm", 3)) ? 32 : 0;
        if (bits) {
            snprintf(arch, sizeof(arch), "%s, %d-bit, ", u.machine, bits);
        } else {
            snprintf(arch, sizeof(arch), "%s, ", u.machine);
        }
    }
    if (model[0]) {
        snprintf(out, outsz, "%s (%s%d)", model, arch, cores);
    } else {
        snprintf(out, outsz, "%s%d nucleo(s)", arch, cores);
    }
}

/* True when the PCI device `slot` ("0000:00:02.0") is bound to a DRM
 * card, i.e. it really drives graphics. Some integrated GPUs expose a
 * second display-class function (or a render-only twin) that lspci lists
 * as a controller too, which showed up as a phantom second GPU. */
static int pci_has_drm_card(const char *slot)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/drm", slot);
    return access(path, F_OK) == 0;
}

static int any_drm_card_on_pci(void)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    if (!d) {
        return 0;
    }
    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] != '.' && pci_has_drm_card(e->d_name)) {
            found = 1;
        }
    }
    closedir(d);
    return found;
}

/* Next double-quoted field of an `lspci -mm` line; NULL at end of line. */
static char *next_quoted(char **cursor)
{
    char *start = strchr(*cursor, '"');
    if (!start) {
        return NULL;
    }
    char *end = strchr(start + 1, '"');
    if (!end) {
        return NULL;
    }
    *end = '\0';
    *cursor = end + 1;
    return start + 1;
}

/* One line per graphics adapter: PCI class 03xx entries from
 * `lspci -mm -D` that are bound to a DRM card (all of them when sysfs
 * shows no DRM cards at all, e.g. a bare VM), as "vendor model", with
 * identical lines collapsed. Discrete+integrated machines list both. */
static void get_gpu(char *out, size_t outsz)
{
    out[0] = '\0';
    char raw[16384];
    char *argv[] = {"lspci", "-mm", "-D", NULL};
    if (!run_capture(argv, raw, sizeof(raw))) {
        snprintf(out, outsz, "?");
        return;
    }
    int filter_drm = any_drm_card_on_pci();
    char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *sp = strchr(line, ' ');
        if (!sp) {
            continue;
        }
        *sp = '\0';
        const char *slot = line;
        char *cursor = sp + 1;
        const char *class_name = next_quoted(&cursor);
        const char *vendor = next_quoted(&cursor);
        const char *device = next_quoted(&cursor);
        if (!class_name || !vendor || !device) {
            continue;
        }
        if (strcmp(class_name, "VGA compatible controller") && strcmp(class_name, "3D controller") &&
            strcmp(class_name, "Display controller")) {
            continue;
        }
        if (filter_drm && !pci_has_drm_card(slot)) {
            continue;
        }
        char model[320];
        snprintf(model, sizeof(model), "%s %s", vendor, device);
        /* Whole-line match, so one model name never hides inside another. */
        char padded_out[1100], padded_model[330];
        snprintf(padded_out, sizeof(padded_out), "\n%s\n", out);
        snprintf(padded_model, sizeof(padded_model), "\n%s\n", model);
        if (strstr(padded_out, padded_model)) {
            continue;
        }
        size_t used = strlen(out);
        snprintf(out + used, outsz - used, "%s%s", used ? "\n" : "", model);
    }
    if (!out[0]) {
        snprintf(out, outsz, "?");
    }
}

/* One left-aligned label per fact, the name in bold with the value on the
 * line below -- this is read-only display, not a form, so it doesn't need
 * labeled_row()'s two-column table (that exists for label+editable-widget
 * pairs elsewhere). GPU/CPU values can wrap or span multiple lines
 * (multi-GPU machines), so line-wrap is on for every value. */
static void add_fact(GtkWidget *box, const char *label, const char *value)
{
    char *markup = g_markup_printf_escaped("<b>%s</b>\n%s", label, value);
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), markup);
    g_free(markup);
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_misc_set_alignment(GTK_MISC(lbl), 0.0, 0.0);
    /* GtkLabel only actually wraps once it has a fixed width to wrap
     * against -- left to negotiate its own size inside a plain vbox, it
     * requests one unbroken line and gets clipped instead by whatever
     * width the sysinfo panel ends up with. */
    gtk_widget_set_size_request(lbl, 460, -1);
    gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
}

/* Set by kiconf/Makefile from ../VERSION; the fallback only matters for
 * a hand-rolled build that bypasses the Makefile. */
#ifndef KIDESKTOP_VERSION
#define KIDESKTOP_VERSION "?"
#endif

GtkWidget *build_sobre_tab(void)
{
    GtkWidget *box = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);

    char user[NAME_LEN], host[NAME_LEN], distro[256], kernel[256];
    char xserver[256], ram[64], cpu[320], gpu[512];
    get_user(user, sizeof(user));
    get_hostname(host, sizeof(host));
    get_distro(distro, sizeof(distro));
    get_kernel(kernel, sizeof(kernel));
    get_xserver(xserver, sizeof(xserver));
    get_ram(ram, sizeof(ram));
    get_cpu(cpu, sizeof(cpu));
    get_gpu(gpu, sizeof(gpu));

    add_fact(box, "Usuario:", user);
    add_fact(box, "Computador:", host);
    /* Anything but our own session shows the running desktop instead of
     * the KiDesktop version. */
    const char *session = getenv("XDG_CURRENT_DESKTOP");
    if (!session || !*session) {
        session = getenv("DESKTOP_SESSION");
    }
    if (session && *session && strcasecmp(session, "KiDesktop")) {
        add_fact(box, "Sessão:", session);
    } else {
        add_fact(box, "KiDesktop:", KIDESKTOP_VERSION);
    }
    add_fact(box, "Distribuicao:", distro[0] ? distro : "?");
    add_fact(box, "Kernel:", kernel);
    add_fact(box, "Servidor X:", xserver);
    add_fact(box, "Processador:", cpu);
    add_fact(box, "Memoria RAM:", ram);
    add_fact(box, "GPU:", gpu);

    return box;
}
