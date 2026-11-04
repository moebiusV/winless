/* store.c -- the annotation database (SQLite).  Copyright (C) 2026 David Walther.  GPL-3.0-or-later.
 *
 * One table, annotations: the file, when it was made, where it is (row and column of the beginning and of the end -- the same
 * coordinates edits use: rows and columns from 1, code points, the end excluded), what kind of thing it covers, the selected text,
 * and the note.  scope is 'text' (inside one paragraph: selected_text is kept), 'paragraphs' (several paragraphs: only which ones,
 * in `paragraphs`, are kept) or 'file' (the whole file: no position).  Nothing is ever deleted: `deleted` hides a note, and it can
 * be brought back.  Statements are always prepared and bound, never built from text.
 */
#define _POSIX_C_SOURCE 200809L
#include "winless.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static char g_err[320];

static const char *fail(sqlite3 *db, const char *what) {
    snprintf(g_err, sizeof g_err, "%s: %s", what, db ? sqlite3_errmsg(db) : "no database");
    return g_err;
}

static const char *open_db(sqlite3 **db) {
    char dir[1024]; winless_data_dir(dir, sizeof dir);
    char path[1100]; snprintf(path, sizeof path, "%s/winless.db", dir);
    if (sqlite3_open_v2(path, db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        const char *e = fail(*db, "cannot open the annotation database"); sqlite3_close(*db); *db = NULL; return e;
    }
    sqlite3_busy_timeout(*db, 3000);                      /* several winless servers may use the file at once */
    const char *schema =
        "CREATE TABLE IF NOT EXISTS annotations ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " file TEXT NOT NULL,"
        " created TEXT NOT NULL,"
        " row_beg INTEGER, col_beg INTEGER, row_end INTEGER, col_end INTEGER,"
        " scope TEXT NOT NULL CHECK (scope IN ('text','paragraphs','file')),"
        " selected_text TEXT, paragraphs TEXT,"
        " note TEXT NOT NULL,"
        " deleted INTEGER NOT NULL DEFAULT 0,"
        " kind TEXT NOT NULL DEFAULT 'note' CHECK (kind IN ('note','slop','keep')),"
        " status TEXT NOT NULL DEFAULT 'open' CHECK (status IN ('open','done')))";
    const char *reviews_schema =
        "CREATE TABLE IF NOT EXISTS reviews ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, file TEXT NOT NULL, created TEXT NOT NULL,"
        " backend TEXT, model TEXT, scope TEXT, annotations TEXT,"
        " accepted INTEGER, rejected INTEGER, pending INTEGER, written INTEGER, commit_hash TEXT, note TEXT)";
    char *msg = NULL;
    if (sqlite3_exec(*db, reviews_schema, NULL, NULL, &msg) != SQLITE_OK) {
        snprintf(g_err, sizeof g_err, "cannot prepare the annotation database: %s", msg ? msg : "?");
        sqlite3_free(msg); sqlite3_close(*db); *db = NULL; return g_err;
    }
    if (sqlite3_exec(*db, schema, NULL, NULL, &msg) != SQLITE_OK) {
        snprintf(g_err, sizeof g_err, "cannot prepare the annotation database: %s", msg ? msg : "?");
        sqlite3_free(msg); sqlite3_close(*db); *db = NULL; return g_err;
    }
    /* A database made before kinds and status existed: add the columns (existing notes become open notes). */
    int has_kind = 0, has_status = 0;
    sqlite3_stmt *pi = NULL;
    if (sqlite3_prepare_v2(*db, "PRAGMA table_info(annotations)", -1, &pi, NULL) == SQLITE_OK) {
        while (sqlite3_step(pi) == SQLITE_ROW) {
            const char *col = (const char *)sqlite3_column_text(pi, 1);
            if (col && !strcmp(col, "kind")) has_kind = 1;
            if (col && !strcmp(col, "status")) has_status = 1;
        }
        sqlite3_finalize(pi);
    }
    if (!has_kind && sqlite3_exec(*db, "ALTER TABLE annotations ADD COLUMN kind TEXT NOT NULL DEFAULT 'note' CHECK (kind IN ('note','slop','keep'))", NULL, NULL, &msg) != SQLITE_OK) {
        snprintf(g_err, sizeof g_err, "cannot upgrade the annotation database: %s", msg ? msg : "?"); sqlite3_free(msg); sqlite3_close(*db); *db = NULL; return g_err;
    }
    if (!has_status && sqlite3_exec(*db, "ALTER TABLE annotations ADD COLUMN status TEXT NOT NULL DEFAULT 'open' CHECK (status IN ('open','done'))", NULL, NULL, &msg) != SQLITE_OK) {
        snprintf(g_err, sizeof g_err, "cannot upgrade the annotation database: %s", msg ? msg : "?"); sqlite3_free(msg); sqlite3_close(*db); *db = NULL; return g_err;
    }
    return NULL;
}

static void bind_text_or_null(sqlite3_stmt *st, int i, const char *s) {
    if (s) sqlite3_bind_text(st, i, s, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, i);
}
static void bind_long_or_null(sqlite3_stmt *st, int i, long v, int present) {
    if (present) sqlite3_bind_int64(st, i, v); else sqlite3_bind_null(st, i);
}

/* Add a note.  r1 == 0 means no position (scope 'file').  Returns NULL and the new id, or an error message. */
const char *store_add(const char *file, long r1, long c1, long r2, long c2, const char *scope, const char *selected,
                      const char *paragraphs, const char *note, const char *kind, long *id) {
    sqlite3 *db; const char *err = open_db(&db); if (err) return err;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO annotations (file, created, row_beg, col_beg, row_end, col_end, scope, selected_text, paragraphs, note, kind)"
            " VALUES (?1, strftime('%Y-%m-%dT%H:%M:%SZ','now'), ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)", -1, &st, NULL) != SQLITE_OK) {
        err = fail(db, "cannot save the note"); sqlite3_close(db); return err;
    }
    int pos = r1 > 0;
    bind_text_or_null(st, 1, file);
    bind_long_or_null(st, 2, r1, pos); bind_long_or_null(st, 3, c1, pos); bind_long_or_null(st, 4, r2, pos); bind_long_or_null(st, 5, c2, pos);
    bind_text_or_null(st, 6, scope); bind_text_or_null(st, 7, selected); bind_text_or_null(st, 8, paragraphs); bind_text_or_null(st, 9, note ? note : ""); bind_text_or_null(st, 10, kind ? kind : "note");
    if (sqlite3_step(st) != SQLITE_DONE) err = fail(db, "cannot save the note");
    else if (id) *id = (long)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(st); sqlite3_close(db);
    return err;
}

static void put_col_text(sb_t *b, sqlite3_stmt *st, int i) {
    if (sqlite3_column_type(st, i) == SQLITE_NULL) sb_puts(b, "null");
    else { const char *s = (const char *)sqlite3_column_text(st, i); json_put_string(b, s ? s : "", s ? strlen(s) : 0); }
}
static void put_col_num(sb_t *b, sqlite3_stmt *st, int i) {
    if (sqlite3_column_type(st, i) == SQLITE_NULL) sb_puts(b, "null"); else sb_printf(b, "%lld", (long long)sqlite3_column_int64(st, i));
}

/* A JSON array of the notes of one file (file == NULL: of every file), hidden ones only if include_deleted. */
const char *store_list_json(sb_t *out, const char *file, int include_deleted) {
    sqlite3 *db; const char *err = open_db(&db); if (err) return err;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id, file, created, row_beg, col_beg, row_end, col_end, scope, selected_text, paragraphs, note, deleted, kind, status"
            " FROM annotations WHERE (?1 IS NULL OR file = ?1) AND (?2 OR deleted = 0)"
            " ORDER BY file, COALESCE(row_beg, 0), COALESCE(col_beg, 0), id", -1, &st, NULL) != SQLITE_OK) {
        err = fail(db, "cannot read the notes"); sqlite3_close(db); return err;
    }
    bind_text_or_null(st, 1, file); sqlite3_bind_int(st, 2, include_deleted ? 1 : 0);
    sb_putc(out, '[');
    int first = 1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!first) sb_putc(out, ',');
        first = 0;
        sb_printf(out, "{\"id\":%lld,\"file\":", (long long)sqlite3_column_int64(st, 0)); put_col_text(out, st, 1);
        sb_puts(out, ",\"created\":"); put_col_text(out, st, 2);
        sb_puts(out, ",\"r1\":"); put_col_num(out, st, 3); sb_puts(out, ",\"c1\":"); put_col_num(out, st, 4);
        sb_puts(out, ",\"r2\":"); put_col_num(out, st, 5); sb_puts(out, ",\"c2\":"); put_col_num(out, st, 6);
        sb_puts(out, ",\"scope\":"); put_col_text(out, st, 7);
        sb_puts(out, ",\"text\":"); put_col_text(out, st, 8);
        sb_puts(out, ",\"paragraphs\":"); put_col_text(out, st, 9);
        sb_puts(out, ",\"note\":"); put_col_text(out, st, 10);
        sb_printf(out, ",\"deleted\":%s", sqlite3_column_int(st, 11) ? "true" : "false");
        sb_puts(out, ",\"kind\":"); put_col_text(out, st, 12);
        sb_puts(out, ",\"status\":"); put_col_text(out, st, 13); sb_putc(out, '}');
    }
    sb_putc(out, ']');
    sqlite3_finalize(st); sqlite3_close(db);
    return NULL;
}

/* Hide a note (deleted = 1) or bring it back (deleted = 0).  Nothing is removed from the database. */
const char *store_set_deleted(long id, int deleted) {
    sqlite3 *db; const char *err = open_db(&db); if (err) return err;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE annotations SET deleted = ?1 WHERE id = ?2", -1, &st, NULL) != SQLITE_OK) { err = fail(db, "cannot update the note"); sqlite3_close(db); return err; }
    sqlite3_bind_int(st, 1, deleted ? 1 : 0); sqlite3_bind_int64(st, 2, id);
    if (sqlite3_step(st) != SQLITE_DONE) err = fail(db, "cannot update the note");
    else if (sqlite3_changes(db) == 0) { snprintf(g_err, sizeof g_err, "no note number %ld", id); err = g_err; }
    sqlite3_finalize(st); sqlite3_close(db);
    return err;
}

/* Mark a note done (its change was accepted) or open again.  Like hiding, this never removes anything. */
const char *store_set_status(long id, const char *status) {
    if (!status || (strcmp(status, "open") && strcmp(status, "done"))) return "status must be open or done";
    sqlite3 *db; const char *err = open_db(&db); if (err) return err;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE annotations SET status = ?1 WHERE id = ?2", -1, &st, NULL) != SQLITE_OK) { err = fail(db, "cannot update the note"); sqlite3_close(db); return err; }
    sqlite3_bind_text(st, 1, status, -1, SQLITE_TRANSIENT); sqlite3_bind_int64(st, 2, id);
    if (sqlite3_step(st) != SQLITE_DONE) err = fail(db, "cannot update the note");
    else if (sqlite3_changes(db) == 0) { snprintf(g_err, sizeof g_err, "no note number %ld", id); err = g_err; }
    sqlite3_finalize(st); sqlite3_close(db);
    return err;
}

/* One row per finished review: what was asked, what was accepted, and whether and how it was written and committed. */
const char *store_add_review(const char *file, const char *backend, const char *model, const char *scope, const char *annotations,
                             long accepted, long rejected, long pending, int written, const char *commit, const char *note) {
    sqlite3 *db; const char *err = open_db(&db); if (err) return err;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO reviews (file, created, backend, model, scope, annotations, accepted, rejected, pending, written, commit_hash, note)"
            " VALUES (?1, strftime('%Y-%m-%dT%H:%M:%SZ','now'), ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)", -1, &st, NULL) != SQLITE_OK) {
        err = fail(db, "cannot record the review"); sqlite3_close(db); return err;
    }
    bind_text_or_null(st, 1, file); bind_text_or_null(st, 2, backend); bind_text_or_null(st, 3, model); bind_text_or_null(st, 4, scope);
    bind_text_or_null(st, 5, annotations); sqlite3_bind_int64(st, 6, accepted); sqlite3_bind_int64(st, 7, rejected); sqlite3_bind_int64(st, 8, pending);
    sqlite3_bind_int(st, 9, written ? 1 : 0); bind_text_or_null(st, 10, commit); bind_text_or_null(st, 11, note);
    if (sqlite3_step(st) != SQLITE_DONE) err = fail(db, "cannot record the review");
    sqlite3_finalize(st); sqlite3_close(db);
    return err;
}
