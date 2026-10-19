#include "xis_match.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

void xis_match_from_mask(XisMatch *m, uint32_t types)
{
    memset(m, 0, sizeof(*m));
    m->types = types;
}

static void add_class(char (*list)[XIS_MATCH_NAME_MAX], int *n, const char *name, size_t len,
                      const char *what)
{
    if (len == 0)
        return;
    if (*n >= XIS_MATCH_MAX_CLASSES || len >= XIS_MATCH_NAME_MAX) {
        fprintf(stderr, "%s 'class:%.*s' (too many or too long)\n", what, (int)len, name);
        return;
    }
    memcpy(list[*n], name, len);
    list[*n][len] = '\0';
    (*n)++;
}

void xis_match_parse(XisMatch *m, const char *list, XisMatchLookup lookup, void *ud,
                     const char *what)
{
    memset(m, 0, sizeof(*m));
    if (strcmp(list, "all") == 0) {
        m->types = 0xffffffffu;
        return;
    }
    if (strcmp(list, "none") == 0)
        return;

    bool positive = false;
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t')
            p++;
        size_t len = (size_t)(p - start);
        if (len == 0)
            continue;

        bool neg = false;
        if (*start == '!') {
            neg = true;
            start++;
            len--;
        }
        if (len > 6 && strncmp(start, "class:", 6) == 0) {
            if (neg)
                add_class(m->neg, &m->nneg, start + 6, len - 6, what);
            else {
                add_class(m->pos, &m->npos, start + 6, len - 6, what);
                positive = true;
            }
            continue;
        }
        uint32_t bits = len ? lookup(start, len, ud) : 0;
        if (!bits) {
            fprintf(stderr, "%s '%.*s'\n", what, (int)len, start);
            continue;
        }
        if (neg)
            m->neg_types |= bits;
        else {
            m->types |= bits;
            positive = true;
        }
    }
    if (!positive)
        m->types = 0xffffffffu;
}

static bool class_in(char (*list)[XIS_MATCH_NAME_MAX], int n, const char *cls, const char *inst)
{
    for (int i = 0; i < n; i++)
        if ((cls && cls[0] && strcasecmp(list[i], cls) == 0) ||
            (inst && inst[0] && strcasecmp(list[i], inst) == 0))
            return true;
    return false;
}

bool xis_match_test(const XisMatch *m, uint32_t type_bit, const char *cls, const char *inst)
{
    if (m->neg_types & type_bit)
        return false;
    /* the lists are only read, but class_in() takes a mutable pointer type */
    if (m->nneg && class_in((char (*)[XIS_MATCH_NAME_MAX])m->neg, m->nneg, cls, inst))
        return false;
    if (m->types & type_bit)
        return true;
    return m->npos && class_in((char (*)[XIS_MATCH_NAME_MAX])m->pos, m->npos, cls, inst);
}

bool xis_match_uses_classes(const XisMatch *m)
{
    return m->npos || m->nneg;
}
