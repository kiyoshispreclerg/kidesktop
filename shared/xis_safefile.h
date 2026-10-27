/* xis_safefile - power-failure-safe writing of the user's config and data
 * files, with an automatic <file>.backup to fall back on.
 *
 * Writing (a critical file): the new content goes to <file>.tmp, is
 * fflush+fsync'd, the CURRENT file is copied to <file>.backup (itself
 * atomic, same permissions) provided it is intact, and only then is the
 * tmp renamed over <file>. Without the fsync a power cut can leave the
 * renamed file empty; the backup covers whatever still goes wrong.
 *
 * Reading: if <file> is missing or not usable (empty, or NULs), the
 * <file>.backup is used instead and a warning is logged. If neither is any
 * good, the result is exactly what a plain read of <file> would have given.
 *
 * "Intact" depends on the format, so every call takes a check function.
 * Use the same one for the backup and the read, except for hand-edited
 * files: there the backup wants xis_check_conf_clean (so a typo does not
 * replace a good backup) while the read only wants xis_check_usable (so a
 * typo does not make the program swap the user's file for the backup).
 *
 * To "reset" by deleting a file, delete its .backup too, or it comes back.
 * Files the user exports to a path of their choice get no backup (see
 * xis_write_export). Caches and logs should not use this at all. */
#ifndef XIS_SAFEFILE_H
#define XIS_SAFEFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef bool (*xis_check_fn)(const char *text, size_t len);

/* text is always NUL-terminated at text[len]. */
bool xis_check_usable(const char *text, size_t len);     /* len > 0, no NUL byte */
bool xis_check_lines(const char *text, size_t len);      /* usable and ends in '\n' */
bool xis_check_pem(const char *text, size_t len);        /* usable, has BEGIN and END */
bool xis_check_conf_clean(const char *text, size_t len); /* usable; every line blank, # comment,
                                                          * [section] or has an '=' */

/* Warnings go to stderr unless a program routes them to its own log. */
void xis_safefile_set_logger(void (*fn)(const char *msg));

/* Copies `path` to `path`.backup (atomically, same mode) if it exists and
 * ok() accepts it. 0 on success or when there is nothing to copy; -1 when
 * the file is corrupt (the previous backup is kept) or the copy failed
 * (both are logged; callers write anyway). ok NULL = xis_check_usable. */
int xis_backup_file(const char *path, xis_check_fn ok);

/* Whole file (NUL-terminated, malloc'd) or its backup, see above. NULL with
 * errno set when `path` itself cannot be read. `ok` NULL = xis_check_usable. */
char *xis_read_file_or_backup(const char *path, xis_check_fn ok, size_t *len);

/* fopen(path, "r") on whichever of `path` and `path`.backup is usable;
 * falls back to the plain fopen(path, "r") result. */
FILE *xis_fopen_read(const char *path, xis_check_fn ok);

/* True when `path` or `path`.backup exists. Use it instead of access(path)
 * to decide "first run, write the defaults", so a lost main file does not
 * hide a backup that the read would have used. */
bool xis_file_or_backup_exists(const char *path);

/* fflush + fsync. 0 on success. */
int xis_flush_sync(FILE *f);

/* Streaming writes: tmp is "<path>.tmp" (xis_write_tmpname fills it).
 * xis_write_open creates it with the permissions of `path` if that exists.
 * xis_write_commit syncs and closes f, backs `path` up (unless ok is NULL),
 * renames; on failure the tmp is removed and -1 returned (errno set).
 * An empty result is written as a single "\n" when a backup is kept, so
 * it is not mistaken for a power-cut leftover. Use xis_write_abort to give up. */
void xis_write_tmpname(const char *path, char *tmp, size_t tmpsz);
FILE *xis_write_open(const char *tmp, const char *path);
int xis_write_commit(FILE *f, const char *tmp, const char *path, xis_check_fn ok);
void xis_write_abort(FILE *f, const char *tmp);

/* One-shot versions. xis_write_file keeps a backup (ok NULL = usable);
 * xis_write_export is for files the user sent somewhere of their choice:
 * atomic and synced, no backup. */
int xis_write_file(const char *path, const char *data, size_t len, xis_check_fn ok);
int xis_write_export(const char *path, const char *data, size_t len);

#endif
