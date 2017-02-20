


#include "config.h"
#include "mutt.h"
#include "mx.h"
#include "url.h"
#include "mailbox.h"
#include "browser.h"
#include "msqlite.h"
#include "string.h"
#include "strings.h"

#include "sqlite3.h"


typedef struct msqlite {
	CONTEXT *msql_ctx;
	int msql_checks;

	sqlite3 *msql_db;

	char *msql_file;
	char *msql_mbox;
} msqlite_t;

static int
mx_parse_sqlite_path(const char *path, char **file, char **mbox)
{
	char *t = strdup(path);
	char *t_scheme = NULL, *t_file = NULL, *t_mbox = NULL;
	char *a;

	t_scheme = t;
	if ((a = strchr(t_scheme, '|')) == NULL) {
		mutt_error("invalid sqlite path (1): %s", path);
		free(t);
		return (-1);
	}
	*a = '\0';

	t_file = a + 1;
	if ((a = strchr(t_file, '|')) == NULL) {
		mutt_error("invalid sqlite path (2): %s", path);
		free(t);
		return (-1);
	}
	*a = '\0';

	t_mbox = a + 1;

	if (strcmp(t_scheme, "sqlite") != 0 || strlen(t_file) < 1) {
		mutt_error("invalid sqlite path (3): %s", path);
		free(t);
		return (-1);
	}

	if (file != NULL) {
		*file = safe_strdup(t_file);
	}
	if (mbox != NULL) {
		while (*t_mbox == '/') /* XXX mutt seems to add a "/" */
			t_mbox++;
		*mbox = safe_strdup(t_mbox);
	}

	free(t);
	return (0);
}

int
mx_is_msqlite(const char *path)
{
	if (path == NULL) {
		return (0);
	}

	return (mx_parse_sqlite_path(path, NULL, NULL) == 0);
}

static void
msqlite_add_folder(struct browser_state *state, const char *file,
    const char *name, const char *typ)
{
	if (state->entrylen + 1 >= state->entrymax) {
		safe_realloc(&state->entry, sizeof (struct folder_file) *
		    (state->entrymax += 256));
		bzero(state->entry + state->entrylen,
		    (sizeof (struct folder_file) * (state->entrymax -
		    state->entrylen)));
	}

	int idx = state->entrylen;
	state->entrylen++;

	char tmp[500];
	snprintf(tmp, sizeof (tmp), "%s", name);
	state->entry[idx].name = safe_strdup(tmp);

	snprintf(tmp, sizeof (tmp), "%s (%s)", name, typ);
	state->entry[idx].desc = safe_strdup(tmp);
}

#ifdef	USE_HCACHE
static int
msqlite_hcache_name(const char *path, char *dest, size_t destsz)
{
	return (snprintf(dest, destsz, "%s.hcache", path));
}

static header_cache_t *
msqlite_hcache_open(msqlite_t *msql, const char *path)
{
	return (mutt_hcache_open(HeaderCache, cachepath, msqlite_hcache_name));
}

static void
msqlite_hcache_close(msqlite_t *msql)
{
	if (msql->msql_hcache == NULL) {
		return;
	}

	mutt_hcache_close(msql->msql_hcache);
	msql->msql_hcache = NULL;
}

static void
msqlite_hcache_make_key(const char *msg_id, char *key, size_t keysz)
{
	snprintf(key, keysz, "/msg/%s", msg_id);
}

static HEADER *
msqlite_hcache_get(msqlite_t *msql, const char *msg_id)
{
	char key[256];

	if (msql->msql_hcache == NULL) {
		return (NULL);
	}

	msqlite_hcache_make_key(msg_id, key, sizeof (key));

	unsigned int *uv = mutt_hcache_fetch(msql->msql_hcache, key,
	    mutt_strlen);
	if (uv == NULL) {
		return (NULL);
	}

	if (*uv != 1) {
		mutt_error("unexpected validity %u", *uv);
		mutt_sleep(1);
		return (NULL);
	}

	return (mutt_hcache_restore((unsigned char *)uv, NULL));
}

static int
msqlite_hcache_put(msqlite_t *msql, HEADER *h)
{
	char key[256];

	if (msql->msql_hcache == NULL) {
		return (NULL);
	}

	msqlite_hcache_make_key(msg_id, key, sizeof (key));

	/*
	 * XXX hard-coded "uidvalidity", as the message IDs do not change...
	 */
	return (mutt_hcache_store(msql->msql_hcache, key, h, 1,
	    mutt_strlen, 0));
}

static int
msqlite_hcache_del(msqlite_t *msql, const char *msg_id)
{
	char key[256];

	if (msql->msql_hcache == NULL) {
		return (NULL);
	}

	msqlite_hcache_make_key(msg_id, key, sizeof (key));

	return (mutt_hcache_delete(msql->msql_hcache, key, mutt_strlen));
}
#endif

static int
msqlite_close_database(sqlite3 **dbp)
{
	if (sqlite3_close(*dbp) != SQLITE_OK) {
		mutt_message("sqlite3_close failed: %s", sqlite3_errmsg(*dbp));
		return (-1);
	}

	*dbp = NULL;
	return (0);
}

static int
msqlite_open_database(const char *path, sqlite3 **dbp)
{
	int r;

	if ((r = sqlite3_open_v2(path, dbp, SQLITE_OPEN_READWRITE, NULL)) !=
	    SQLITE_OK) {
		mutt_message("sqlite3_open_v2 (%s) failed: %s", path,
		    sqlite3_errstr(r));

		*dbp = NULL;
		return (-1);
	}

	return (0);
}

#define	QUERY_LABELS \
	"SELECT\n" \
	"	l.name,\n" \
	"	l.type\n" \
	"FROM\n" \
	"	label l\n" \
	"ORDER BY\n" \
	"	l.type ASC,\n" \
	"	l.name ASC\n"

int
msqlite_browse(const char *f, struct browser_state *state)
{
	char *file = NULL, *mbox = NULL;
	sqlite3 *db;
	sqlite3_stmt *stmt;
	int rc = 0;
	int rr;

	if (mx_parse_sqlite_path(f, &file, &mbox) != 0) {
		return (-1);
	}

	if (msqlite_open_database(file, &db) != 0) {
		free(file);
		free(mbox);
		return (-1);
	}

	if (sqlite3_prepare(db, QUERY_LABELS, strlen(QUERY_LABELS), &stmt,
	    NULL) != SQLITE_OK) {
		mutt_error("prepare failed: %s", sqlite3_errmsg(db));
		mutt_sleep(1);
		rc = -1;
		goto skip_query;
	}

	while ((rr = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *name = (char *)sqlite3_column_text(stmt, 0);
		const char *type = (char *)sqlite3_column_text(stmt, 1);

		if (strncmp(name, "CATEGORY_", strlen("CATEGORY_")) == 0) {
			continue;
		}

		msqlite_add_folder(state, file, name, type);
	}

	if (rr != SQLITE_DONE) {
		mutt_error("query failed: %s", sqlite3_errmsg(db));
		mutt_sleep(1);
	}

skip_query:
	sqlite3_finalize(stmt);

	if (msqlite_close_database(&db) != 0) {
		return (-1);
	}

	return (rc);
}

int
msqlite_open_mailbox_append(CONTEXT *ctx)
{
	fprintf(stderr, "ENTER: %s\n", __func__);
	abort();
}

static int
msqlite_close_mailbox(CONTEXT *ctx)
{
	msqlite_t *msql = ctx->data;

	if (msql == NULL) {
		return (0);
	}

	if (sqlite3_close(msql->msql_db) != SQLITE_OK) {
		mutt_message("sqlite3_close failed");
		return (-1);
	}

	free(msql->msql_file);
	free(msql->msql_mbox);
	free(msql);
	ctx->data = NULL;

	return (0);
}

int
msqlite_open_mailbox(CONTEXT *ctx)
{
	msqlite_t *msql;
	int r;

	/*
	 * XXX we get here first!
	 */

	if ((msql = calloc(1, sizeof (*msql))) == NULL) {
		mutt_perror("msqlite_open_mailbox");
		return (-1);
	}

	if (mx_parse_sqlite_path(ctx->path, &msql->msql_file,
	    &msql->msql_mbox) != 0) {
		free(msql);
		mutt_sleep(1);
		return (-1);
	}

	msql->msql_ctx = ctx;
	ctx->data = msql;
	ctx->mx_close = msqlite_close_mailbox;

	mutt_message(_("opening sqlite db..."));

	if ((r = sqlite3_open_v2(msql->msql_file, &msql->msql_db,
	    SQLITE_OPEN_READWRITE, NULL)) != SQLITE_OK) {
		mutt_message("sqlite3_open_v2 (%s) failed: %s",
		    msql->msql_file, sqlite3_errstr(r));
		free(msql);
		ctx->data = NULL;
		return (-1);
	}

	mutt_message(_("open ok"));

	bzero(ctx->rights, sizeof (ctx->rights));

	return (0);
}

int
msqlite_sync_mailbox(CONTEXT *ctx, int *index_hint)
{
	fprintf(stderr, "ENTER: %s\n", __func__);
	abort();
}

int
msqlite_open_new_message(MESSAGE *msg, CONTEXT *ctx, HEADER *hdr)
{
	fprintf(stderr, "ENTER: %s\n", __func__);
	abort();
}

static int
make_message_tempfile(FILE **fpp)
{
	FILE *fp;
	char tempfile[_POSIX_PATH_MAX];

	/*
	 * Open a temporary file...?
	 */
	mutt_mktemp(tempfile, sizeof (tempfile));
	if ((fp = safe_fopen(tempfile, "w+")) == NULL) {
		mutt_error(_("could not create temporary file %s"), tempfile);
		mutt_sleep(2);
		return (-1);
	}
	unlink(tempfile);

	*fpp = fp;
	return (0);
}

#define	QUERY_GET_LABEL_ID						\
	"SELECT\n"							\
	"	l.id\n"							\
	"FROM\n"							\
	"	label l\n"						\
	"WHERE\n"							\
	"	l.name = ?\n"

static char *
get_label_id(msqlite_t *msql, const char *name)
{
	sqlite3_stmt *stmt = NULL;
	char *label_id = NULL;
	int sqlite_fail = 1;
	int e = 0;

	if (sqlite3_prepare(msql->msql_db, QUERY_GET_LABEL_ID,
	    strlen(QUERY_GET_LABEL_ID), &stmt, NULL) != SQLITE_OK) {
		goto out;
	}

	if (sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC) != SQLITE_OK) {
		goto out;
	}

	switch (sqlite3_step(stmt)) {
	case SQLITE_ROW:
		sqlite_fail = 0;
		label_id = safe_strdup((char *)sqlite3_column_text(stmt, 0));
		break;

	case SQLITE_DONE:
		/*
		 * No match.
		 */
		sqlite_fail = 0;
		e = ENOENT;
		break;
	}

out:
	if (sqlite_fail) {
		mutt_error("get_label_id: %s", sqlite3_errmsg(msql->msql_db));
		mutt_sleep(1);
		e = EPROTO;
	}
	sqlite3_finalize(stmt);
	errno = e;
	return (label_id);
}

#define	QUERY_MESSAGE_LABELS \
	"SELECT\n" \
	"	mtl.label_id\n" \
	"FROM\n" \
	"	message_to_label mtl\n" \
	"WHERE\n" \
	"	mtl.message_id = ?\n"

#define	QUERY_MESSAGE_RAW \
	"SELECT\n" \
	"	mr.raw\n" \
	"FROM\n" \
	"	message_raw mr\n" \
	"WHERE\n" \
	"	mr.id = ?\n"

static int
flags_from_db(msqlite_t *msql, const char *msg_id, HEADER *h)
{
	sqlite3_stmt *stmt;

	if (sqlite3_prepare_v2(msql->msql_db, QUERY_MESSAGE_LABELS,
	    strlen(QUERY_MESSAGE_LABELS), &stmt, NULL) != SQLITE_OK) {
		sqlite3_finalize(stmt);
		mutt_error("fetch_from_db(%s): %s", msg_id,
		    sqlite3_errmsg(msql->msql_db));
		mutt_sleep(1);
		return (-1);
	}

	if (sqlite3_bind_text(stmt, 1, msg_id, -1,
	    SQLITE_STATIC) != SQLITE_OK) {
		mutt_error("sqlite3_bind_text: %s", sqlite3_errmsg(
		    msql->msql_db));
		mutt_sleep(1);
		return (-1);
	}

	h->read = 1;
	h->flagged = 0;

	int r;
	while ((r = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *l = (const char *)sqlite3_column_text(stmt, 0);

		if (strcmp(l, "UNREAD") == 0) {
			h->read = 0;
		} else if (strcmp(l, "STARRED") == 0) {
			h->flagged = 1;
		}
	}

	if (r != SQLITE_DONE) {
		mutt_error("msg %s flags error: %s", msg_id, sqlite3_errmsg(
		    msql->msql_db));
		mutt_sleep(2);
		sqlite3_finalize(stmt);
		return (-1);
	}

	sqlite3_finalize(stmt);
	return (0);
}

static int
fetch_from_db(msqlite_t *msql, const char *msg_id, FILE *fp)
{
	sqlite3_stmt *stmt;

	if (sqlite3_prepare_v2(msql->msql_db, QUERY_MESSAGE_RAW,
	    strlen(QUERY_MESSAGE_RAW), &stmt, NULL) != SQLITE_OK) {
		sqlite3_finalize(stmt);
		mutt_error("fetch_from_db(%s): %s", msg_id,
		    sqlite3_errmsg(msql->msql_db));
		mutt_sleep(1);
		return (-1);
	}

	if (sqlite3_bind_text(stmt, 1, msg_id, -1,
	    SQLITE_STATIC) != SQLITE_OK) {
		mutt_error("sqlite3_bind_text: %s", sqlite3_errmsg(
		    msql->msql_db));
		mutt_sleep(1);
		return (-1);
	}

	switch (sqlite3_step(stmt)) {
	case SQLITE_ROW:
		break;

	case SQLITE_DONE:
		mutt_error("msg %s not found in DB", msg_id);
		sqlite3_finalize(stmt);
		return (-1);

	default:
		mutt_error("msg %s error: %s", msg_id, sqlite3_errmsg(
		    msql->msql_db));
		mutt_sleep(2);
		sqlite3_finalize(stmt);
		return (-1);
	}

	char *raw = safe_strdup((const char *)sqlite3_column_text(stmt, 0));
	int outlen;

	sqlite3_finalize(stmt);

	for (char *cp = raw; *cp != '\0'; cp++) {
		switch (*cp) {
		case '-':
			*cp = '+';
			break;
		case '_':
			*cp = '/';
			break;
		}
	}

	if ((outlen = mutt_from_base64(raw, raw)) < 0) {
		extern int base64_last_offset;

		mutt_message("mutt_from_base64 fail (last %d)",
		    base64_last_offset);
		mutt_sleep(2);

		safe_free(&raw);
		return (-1);
	}

	char *xxx = malloc(outlen);
	int xxxi = 0;
	for (int i = 0; i < outlen; i++) {
		if (raw[i] != '\r') {
			xxx[xxxi++] = raw[i];
			//fputc(raw[i], fp);
		}
	}
	fwrite(xxx, xxxi, 1, fp);
	fflush(fp);
	free(xxx);

	safe_free(&raw);
	return (0);
}

#if 0
#define	QUERY_LABEL_MESSAGE_LIST \
	"SELECT\n" \
	"	m.id\n" \
	"FROM\n" \
	"	message m INNER JOIN message_to_label mtl\n" \
	"		ON (m.id = mtl.message_id)\n" \
	"WHERE\n" \
	"	mtl.label_id = ?\n" \
	"ORDER BY\n" \
	"	m.id ASC\n"
#else
#define	QUERY_LABEL_MESSAGE_LIST \
	"SELECT\n" \
	"	mtl.message_id AS id\n" \
	"FROM\n" \
	"	message_to_label mtl\n" \
	"WHERE\n" \
	"	mtl.label_id = ?\n" \
	"ORDER BY\n" \
	"	mtl.message_id ASC\n"
#endif

#define	QUERY_MESSAGE_LIST \
	"SELECT\n" \
	"	m.id\n" \
	"FROM\n" \
	"	message m\n" \
	"ORDER BY\n" \
	"	m.id ASC\n"

static int
get_from_db(msqlite_t *msql, const char *label_id)
{
	CONTEXT *ctx = msql->msql_ctx;
	sqlite3_stmt *stmt;
	int r;
	FILE *fp;

	if (make_message_tempfile(&fp) != 0) {
		return (-1);
	}

	mutt_message(_("prepare"));

	const char *q = (label_id == NULL) ? QUERY_MESSAGE_LIST :
	    QUERY_LABEL_MESSAGE_LIST;

	if (sqlite3_prepare_v2(msql->msql_db, q, strlen(q), &stmt,
	    NULL) != SQLITE_OK) {
		mutt_error("sqlite3_prepare fail");
		mutt_sleep(1);
		return (-1);
	}
	if (label_id != NULL && sqlite3_bind_text(stmt, 1, label_id, -1,
	    SQLITE_STATIC) != SQLITE_OK) {
		mutt_error("sqlite3_bind_text fail: %s", sqlite3_errmsg(
		    msql->msql_db));
		mutt_sleep(1);
		return (-1);
	}

	mutt_message(_("rows"));

	int count = 0;
	while ((r = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *msg_id = (const char *)sqlite3_column_text(
		    stmt, 0);

		rewind(fp);
		if (fetch_from_db(msql, msg_id, fp) != 0) {
			continue;
		}

		ctx->msgcount++;
		int idx = ctx->msgcount - 1;

		if (idx % 1000 == 0 && idx > 0) {
			mutt_message("getting message headers (%d)...", idx);
		}

		if (ctx->hdrmax < idx + 25) {
			mx_alloc_memory(ctx);
		}

		ctx->hdrs[idx] = mutt_new_header();

		ctx->hdrs[idx]->index = idx;

		/*
		 * Get "read" and "flagged" from the DB:
		 */
		if (flags_from_db(msql, msg_id, ctx->hdrs[idx]) != 0) {
			safe_free(&ctx->hdrs[idx]);
			ctx->msgcount--;
			continue;
		}

		ctx->hdrs[idx]->active = 1;
		ctx->hdrs[idx]->old = 0;
		ctx->hdrs[idx]->deleted = 0;
		ctx->hdrs[idx]->replied = 0;
		ctx->hdrs[idx]->changed = 0;
		ctx->hdrs[idx]->received = 1462346271;
		ctx->hdrs[idx]->data = safe_strdup(msg_id);

		rewind(fp);
		ctx->hdrs[idx]->env = mutt_read_rfc822_header(fp, ctx->hdrs[idx], 0, 0);

		count++;
	}

	if (r != SQLITE_DONE) {
		mutt_error("sqlite3_step fail");
		mutt_sleep(2);
	}

	mutt_message(_("update ctx"));
	mx_update_context(ctx, count);

	mutt_message(_("finalize"));

	sqlite3_finalize(stmt);

	mutt_message(_("fclose"));

	safe_fclose(&fp);
	return (0);
}

int
msqlite_check_mailbox(CONTEXT *ctx, int *index_hint)
{
	msqlite_t *msql = ctx->data;

	/*
	 * We get here second!
	 */

	if (msql->msql_checks == 0) {
#if 1
		char *label_id;

		/*
		 * Determine the Label ID of the "folder" we are viewing:
		 */
		if ((label_id = get_label_id(msql, msql->msql_mbox)) == NULL) {
			mutt_error("could not find label \"%s\"",
			    msql->msql_mbox);
			mutt_sleep(2);
			return (-1);
		}

		/*
		 * XXX Report new mail the first time!
		 */
		msql->msql_checks++;
		get_from_db(msql, label_id);
		safe_free(&label_id);
		return (M_NEW_MAIL);
#else
		msql->msql_checks++;
		get_from_db(msql, NULL);
		return (M_NEW_MAIL);
#endif
	}

	/*
	 * XXX Report no change.
	 */
	return (0);
}

int
msqlite_fetch_message(MESSAGE *msg, CONTEXT *ctx, int msgno)
{
	msqlite_t *msql = ctx->data;
	HEADER *h = ctx->hdrs[msgno];
	ENVELOPE *newenv;

	char buf[LONG_STRING];

	/*
	 * Get here third!
	 */

	if (msg->fp == NULL) {
		if (make_message_tempfile(&msg->fp) != 0) {
			return (-1);
		}

		if (fetch_from_db(msql, (const char *)h->data,
		    msg->fp) != 0) {
			return (-1);
		}
	}

	fflush(msg->fp);
	if (ferror(msg->fp)) {
		mutt_perror("cache file");
		safe_fclose(&msg->fp);
		return (-1);
	}

	rewind(msg->fp);

	newenv = mutt_read_rfc822_header(msg->fp, h, 0, 0);
	mutt_merge_envelopes(h->env, &newenv);

	h->lines = 0;
	fgets(buf, sizeof (buf), msg->fp);
	while (!feof(msg->fp)) {
		h->lines++;
		fgets(buf, sizeof (buf), msg->fp);
	}

	h->content->length = ftell(msg->fp) - h->content->offset;

	mutt_clear_error();
	rewind(msg->fp);

	return (0);
}

int
msqlite_commit_message(CONTEXT *ctx, MESSAGE *msg, HEADER *hdr)
{
	fprintf(stderr, "ENTER: %s\n", __func__);
	abort();
}

int
msqlite_check_empty(const char *path)
{
	fprintf(stderr, "ENTER: %s\n", __func__);
	abort();
}


