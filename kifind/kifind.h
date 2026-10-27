/* kifind.h - shared declarations between the kifind source files. */
#ifndef KIFIND_H
#define KIFIND_H

#include <gtk/gtk.h>
#include <sys/types.h>

#define KIFIND_VERSION "0.1.1"

/* File type categories (bits of KfdQuery.types). A file can be in several:
 * a shell script is both text and executable. */
enum {
    KFD_T_DIR = 1 << 0,
    KFD_T_IMAGE = 1 << 1,
    KFD_T_AUDIO = 1 << 2,
    KFD_T_VIDEO = 1 << 3,
    KFD_T_TEXT = 1 << 4,
    KFD_T_DOC = 1 << 5,
    KFD_T_ARCHIVE = 1 << 6,
    KFD_T_EXEC = 1 << 7,
    KFD_T_OTHER = 1 << 8,       /* none of the above */
};

/* What to look for. Limits are inclusive; an unset one is KFD_NO_LIMIT. */
#define KFD_NO_LIMIT G_MININT64
typedef struct {
    char *root;                 /* folder to search, file system path */
    char *pattern;              /* name, '*' matches any run of characters; "" = any name */
    gboolean subfolders, case_sensitive, hidden;
    guint types;                /* KFD_T_* bits, 0 = any type */
    gint64 btime_from, btime_to;    /* creation time, unix seconds */
    gint64 mtime_from, mtime_to;
    gint64 size_from, size_to;      /* bytes; with either set, folders don't match */
    GArray *uids, *gids;        /* uid_t / gid_t; NULL = anyone */
} KfdQuery;

typedef struct {
    char *dir, *name;           /* file system encoding */
    gint64 size, mtime;
    gboolean is_dir;
} KfdResult;

/* kfd_search.c -- runs in a thread; the UI polls kfd_search_take(). */
typedef struct KfdSearch KfdSearch;
KfdSearch *kfd_search_start(KfdQuery *q);           /* takes over q's contents */
/* Results found since the last call (KfdResult*, free with kfd_result_free),
 * the folder being read now (g_free), and whether the search has ended. */
GPtrArray *kfd_search_take(KfdSearch *s, char **current, gboolean *done);
void kfd_search_cancel(KfdSearch *s);
void kfd_search_unref(KfdSearch *s);
void kfd_result_free(gpointer r);
void kfd_query_clear(KfdQuery *q);
gboolean kfd_name_matches(const char *pattern, gboolean case_sensitive, const char *name);   /* for tests */

/* kfd_multi.c -- a button whose menu ticks any number of choices */
GtkWidget *kfd_multi_new(const char *none_label);
void kfd_multi_add(GtkWidget *b, const char *label, int id, gboolean secondary);
void kfd_multi_set_secondary_label(GtkWidget *b, const char *label);   /* submenu holding the secondary ones */
GArray *kfd_multi_selected(GtkWidget *b);           /* int ids, NULL when nothing is ticked */
void kfd_multi_clear(GtkWidget *b);

/* kfd_date.c -- date entry with a calendar popup */
GtkWidget *kfd_date_new(void);
/* FALSE when the text is not a date. An empty entry is OK and gives KFD_NO_LIMIT.
 * end_of_day: the last second of that day instead of the first. */
gboolean kfd_date_get(GtkWidget *e, gboolean end_of_day, gint64 *out);

#endif
