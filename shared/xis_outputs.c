/* See xis_outputs.h for the design this implements. */
#include "xis_outputs.h"

#include <stdio.h>
#include <string.h>

int xis_output_edid_id(Display *dpy, RROutput output, char *out, size_t outsz)
{
    Atom edid_atom = XInternAtom(dpy, "EDID", False);
    if (edid_atom == None) {
        return 0;
    }
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    /* long_length is in 32-bit units, same XGetWindowProperty-style unit
     * EDID readers everywhere use this call with -- 32 of them covers the
     * mandatory 128-byte base EDID block, all we need (manufacturer,
     * product, serial live in its first 16 bytes). */
    if (XRRGetOutputProperty(dpy, output, edid_atom, 0, 32, False, False, AnyPropertyType, &actual_type, &actual_format, &nitems, &bytes_after, &prop) != Success) {
        return 0;
    }
    int ok = 0;
    if (prop && nitems >= 16 && actual_format == 8 && prop[0] == 0x00 && prop[1] == 0xFF && prop[2] == 0xFF && prop[3] == 0xFF && prop[4] == 0xFF && prop[5] == 0xFF && prop[6] == 0xFF && prop[7] == 0x00) {
        /* Manufacturer ID: 16 bits, big-endian, three 5-bit letters
         * (1=A..26=Z) packed MSB-first with the top bit unused. Product
         * code and serial number are both little-endian, unlike the
         * manufacturer field -- this asymmetry is the EDID spec's own,
         * not a bug here. */
        unsigned id16 = ((unsigned)prop[8] << 8) | prop[9];
        char vendor[4];
        vendor[0] = (char)('A' + (((id16 >> 10) & 0x1F) - 1));
        vendor[1] = (char)('A' + (((id16 >> 5) & 0x1F) - 1));
        vendor[2] = (char)('A' + ((id16 & 0x1F) - 1));
        vendor[3] = '\0';
        unsigned product = (unsigned)prop[10] | ((unsigned)prop[11] << 8);
        unsigned serial = (unsigned)prop[12] | ((unsigned)prop[13] << 8) | ((unsigned)prop[14] << 16) | ((unsigned)prop[15] << 24);
        snprintf(out, outsz, "edid:%s:%04x:%08x", vendor, product, serial);
        ok = 1;
    }
    if (prop) {
        XFree(prop);
    }
    return ok;
}

int xis_list_outputs(Display *dpy, XisOutput *outs, int max, int forced)
{
    Window root = DefaultRootWindow(dpy);
    XRRScreenResources *res = forced ? XRRGetScreenResources(dpy, root) : XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < res->noutput && n < max; i++) {
        XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (oi && oi->connection == RR_Connected && oi->crtc) {
            snprintf(outs[n].name, sizeof(outs[n].name), "%s", oi->name);
            if (!xis_output_edid_id(dpy, res->outputs[i], outs[n].id, sizeof(outs[n].id))) {
                outs[n].id[0] = '\0';
            }
            n++;
        }
        if (oi) {
            XRRFreeOutputInfo(oi);
        }
    }
    XRRFreeScreenResources(res);
    return n;
}

int xis_resolve_output(Display *dpy, const char *saved_id, char *out_name, size_t outsz, int forced)
{
    if (!saved_id || !*saved_id || strcmp(saved_id, "*") == 0) {
        return 0;
    }
    XisOutput outs[XIS_MAX_OUTPUTS];
    int n = xis_list_outputs(dpy, outs, XIS_MAX_OUTPUTS, forced);
    int is_edid = strncmp(saved_id, "edid:", 5) == 0;
    for (int i = 0; i < n; i++) {
        int match = is_edid ? (outs[i].id[0] && strcmp(outs[i].id, saved_id) == 0) : (strcmp(outs[i].name, saved_id) == 0);
        if (match) {
            snprintf(out_name, outsz, "%s", outs[i].name);
            return 1;
        }
    }
    return 0;
}

int xis_build_output_rename_map(Display *dpy, const char *const *saved_ids, int n_saved, XisOutputRename *map, int max_map)
{
    XisOutput real[XIS_MAX_OUTPUTS];
    int n_real = xis_list_outputs(dpy, real, XIS_MAX_OUTPUTS, 1);
    int claimed[XIS_MAX_OUTPUTS] = {0};

    /* Dedup saved_ids into the plain-literal-name subset this function
     * actually handles -- "edid:..." entries are resolved live by
     * xis_resolve_output() instead and never participate here (see this
     * file's header comment), and "*" is never a real output. A config
     * with the same output repeated across many entries (xisback: one
     * per desktop; xispanel: rare but possible) must only claim one real
     * output for it, not one per repetition. */
    char uniq[XIS_MAX_OUTPUTS][XIS_OUTPUT_STR_LEN];
    int n_uniq = 0;
    for (int i = 0; i < n_saved && n_uniq < XIS_MAX_OUTPUTS; i++) {
        const char *id = saved_ids[i];
        if (!id || !*id || strcmp(id, "*") == 0 || strncmp(id, "edid:", 5) == 0) {
            continue;
        }
        int dup = 0;
        for (int j = 0; j < n_uniq; j++) {
            if (strcmp(uniq[j], id) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            snprintf(uniq[n_uniq], sizeof(uniq[n_uniq]), "%s", id);
            n_uniq++;
        }
    }

    int n_map = 0;

    /* Exact name matches: claim that real output (so pass 2 below can't
     * reuse it for someone else) but need no map entry -- already
     * correct. */
    int resolved[XIS_MAX_OUTPUTS] = {0};
    for (int i = 0; i < n_uniq; i++) {
        for (int j = 0; j < n_real; j++) {
            if (!claimed[j] && strcmp(real[j].name, uniq[i]) == 0) {
                claimed[j] = 1;
                resolved[i] = 1;
                break;
            }
        }
    }

    /* Positional last resort: only when what's left on both sides is the
     * same count. */
    int n_rem = 0;
    for (int i = 0; i < n_uniq; i++) {
        if (!resolved[i]) {
            n_rem++;
        }
    }
    int n_rem_real = 0;
    for (int j = 0; j < n_real; j++) {
        if (!claimed[j]) {
            n_rem_real++;
        }
    }
    if (n_rem > 0 && n_rem == n_rem_real) {
        int rj = 0;
        for (int i = 0; i < n_uniq && n_map < max_map; i++) {
            if (resolved[i]) {
                continue;
            }
            while (claimed[rj]) {
                rj++;
            }
            snprintf(map[n_map].from, sizeof(map[n_map].from), "%s", uniq[i]);
            snprintf(map[n_map].to, sizeof(map[n_map].to), "%s", real[rj].name);
            claimed[rj] = 1;
            n_map++;
        }
    }

    return n_map;
}

const char *xis_apply_output_rename(const XisOutputRename *map, int n_map, const char *saved_id)
{
    for (int i = 0; i < n_map; i++) {
        if (strcmp(map[i].from, saved_id) == 0) {
            return map[i].to;
        }
    }
    return saved_id;
}
