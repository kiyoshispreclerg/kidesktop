/* kiconf - home page "system info" panel. See sysinfo.h. */
#include "common.h"
#include "sysinfo.h"

#include <ctype.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

static void get_user(char *out, size_t outsz)
{
    const char *env = getenv("USER");
    if (env && *env) {
        snprintf(out, outsz, "%s", env);
        return;
    }
    struct passwd *pw = getpwuid(getuid());
    snprintf(out, outsz, "%s", (pw && pw->pw_name) ? pw->pw_name : "?");
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

/* xdpyinfo's "vendor string:"/"vendor release number:" -- the XLibre
 * fork stamps its own fork name/build number there (see
 * reference_xis_extensions in this repo's design notes), which plain
 * "X.Org version number:" alone wouldn't distinguish from upstream. */
static void get_xserver(char *out, size_t outsz)
{
    char raw[16384];
    char *argv[] = {"xdpyinfo", NULL};
    if (!run_capture(argv, raw, sizeof(raw))) {
        snprintf(out, outsz, "%s", getenv("DISPLAY") ? "X11" : "?");
        return;
    }
    char vendor[128] = "", version[32] = "", release[32] = "";
    char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *l = trim(line);
        if (!strncmp(l, "vendor string:", 14)) {
            snprintf(vendor, sizeof(vendor), "%s", trim(l + 14));
        } else if (!strncmp(l, "version number:", 16)) {
            snprintf(version, sizeof(version), "%s", trim(l + 16));
        } else if (!strncmp(l, "vendor release number:", 23)) {
            snprintf(release, sizeof(release), "%s", trim(l + 23));
        }
    }
    snprintf(out, outsz, "%s%sX11%s%s%s%s",
             vendor, vendor[0] ? " " : "",
             version[0] ? " " : "", version,
             release[0] ? ", build " : "", release);
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
        snprintf(out, outsz, "%.1f GiB", kb / (1024.0 * 1024.0));
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
    if (model[0]) {
        snprintf(out, outsz, "%s (%d)", model, cores);
    } else {
        snprintf(out, outsz, "%d nucleo(s)", cores);
    }
}

/* One line per VGA/3D/display PCI controller lspci finds, vendor+model
 * text only (drops the leading "NN:NN.N " bus address and the trailing
 * " (rev NN)" -- neither means anything to whoever's reading this
 * panel). Multiple GPUs (the common discrete+integrated laptop/desktop
 * case) all get listed, newline-separated. */
static void get_gpu(char *out, size_t outsz)
{
    out[0] = '\0';
    char raw[16384];
    char *argv[] = {"lspci", NULL};
    if (!run_capture(argv, raw, sizeof(raw))) {
        snprintf(out, outsz, "?");
        return;
    }
    static const char *const kinds[] = {"VGA compatible controller: ", "3D controller: ", "Display controller: ", NULL};
    char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        for (int k = 0; kinds[k]; k++) {
            char *p = strstr(line, kinds[k]);
            if (!p) {
                continue;
            }
            char model[256];
            snprintf(model, sizeof(model), "%s", p + strlen(kinds[k]));
            size_t n = strlen(model);
            while (n > 0 && (model[n - 1] == '\n' || model[n - 1] == '\r')) {
                model[--n] = '\0';
            }
            char *rev = strstr(model, " (rev ");
            if (rev) {
                *rev = '\0';
            }
            size_t used = strlen(out);
            snprintf(out + used, outsz - used, "%s%s", used ? "\n" : "", model);
            break;
        }
    }
    if (!out[0]) {
        snprintf(out, outsz, "?");
    }
}

/* One left-aligned "Campo: valor" label per fact, packed tight -- this
 * panel is read-only display, not a form, so it doesn't need
 * labeled_row()'s two-column table (that exists for label+editable-widget
 * pairs elsewhere). GPU/CPU values can wrap or span multiple lines
 * (multi-GPU machines), so line-wrap is on for every value. */
static void add_fact(GtkWidget *box, const char *label, const char *value)
{
    char *markup = g_markup_printf_escaped("<b>%s</b> %s", label, value);
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), markup);
    g_free(markup);
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_misc_set_alignment(GTK_MISC(lbl), 0.0, 0.0);
    /* GtkLabel only actually wraps once it has a fixed width to wrap
     * against -- left to negotiate its own size inside a plain vbox, it
     * requests one unbroken line and gets clipped instead by whatever
     * width the sysinfo panel ends up with. */
    gtk_widget_set_size_request(lbl, 240, -1);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
}

GtkWidget *build_sysinfo_panel(void)
{
    GtkWidget *box = gtk_vbox_new(FALSE, 4);

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
    add_fact(box, "Distribuicao:", distro[0] ? distro : "?");
    add_fact(box, "Kernel:", kernel);
    add_fact(box, "Servidor X:", xserver);
    add_fact(box, "Processador:", cpu);
    add_fact(box, "Memoria RAM:", ram);
    add_fact(box, "GPU:", gpu);

    return frame_with(_("Sobre este computador"), box);
}
