#include "content_store.h"
#include "cfg.h"
#include "deal_mysql.h"
#include "redis_keys.h"
#include "util_cgi.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

MYSQL *content_db(void) {
    char u[128] = {0}, p[128] = {0}, d[128] = {0};
    if (get_mysql_info(u, p, d))
        return NULL;
    MYSQL *db = msql_conn(u, p, d);
    if (db && mysql_set_character_set(db, "utf8")) {
        mysql_close(db);
        return NULL;
    }
    return db;
}
int content_valid_md5(const char *s) {
    if (!s || strlen(s) != 32)
        return 0;
    for (int i = 0; i < 32; i++)
        if (!isdigit((unsigned char)s[i]) && (s[i] < 'a' || s[i] > 'f'))
            return 0;
    return 1;
}
static int scalar(MYSQL *db, const char *sql, char *out, size_t cap) {
    if (mysql_query(db, sql))
        return -1;
    MYSQL_RES *r = mysql_store_result(db);
    if (!r)
        return -1;
    MYSQL_ROW row = mysql_fetch_row(r);
    int rc = row && row[0] ? 0 : 1;
    if (!rc && out)
        snprintf(out, cap, "%s", row[0]);
    mysql_free_result(r);
    return rc;
}
int content_lock(MYSQL *db, const char *md5) {
    char q[160], v[8];
    if (!content_valid_md5(md5))
        return -1;
    snprintf(q, sizeof(q), "SELECT GET_LOCK('content:%s',30)", md5);
    return scalar(db, q, v, sizeof(v)) == 0 && !strcmp(v, "1") ? 0 : -1;
}
void content_unlock(MYSQL *db, const char *md5) {
    char q[160];
    if (!content_valid_md5(md5))
        return;
    snprintf(q, sizeof(q), "SELECT RELEASE_LOCK('content:%s')", md5);
    scalar(db, q, NULL, 0);
}
int content_queue_gc(MYSQL *db, const char *id) {
    char e[1025], q[1200];
    if (!id || !*id || strlen(id) > 256)
        return -1;
    mysql_real_escape_string(db, e, id, strlen(id));
    snprintf(q, sizeof(q),
             "INSERT INTO storage_gc(file_id) VALUES('%s') ON DUPLICATE KEY UPDATE "
             "file_id=VALUES(file_id)",
             e);
    return mysql_query(db, q) ? -1 : 0;
}
int content_bind(MYSQL *db, const char *user, const char *md5, const char *name, long long size,
                 const char *id, const char *url) {
    char u[257], n[1025], ei[1025], eu[2049], suffix[SUFFIX_LEN] = {0}, es[33];
    char q[8192], existing[257];
    int rc = -1, inserted = 0;
    if (!db || !content_valid_md5(md5) || !user || !*user || strlen(user) > 32 || !name || !*name ||
        strlen(name) > 128 || (id && (strlen(id) > 256 || !url || strlen(url) > 512 || size < 0)))
        return -1;
    mysql_real_escape_string(db, u, user, strlen(user));
    mysql_real_escape_string(db, n, name, strlen(name));
    if (mysql_query(db, "START TRANSACTION"))
        return -1;
    if (id) {
        mysql_real_escape_string(db, ei, id, strlen(id));
        mysql_real_escape_string(db, eu, url, strlen(url));
        get_file_suffix(name, suffix);
        mysql_real_escape_string(db, es, suffix, strlen(suffix));
        snprintf(q, sizeof(q),
                 "INSERT INTO file_info(md5,file_id,url,size,type,count) "
                 "VALUES('%s','%s','%s',%lld,'%s',0) ON DUPLICATE KEY UPDATE md5=VALUES(md5)",
                 md5, ei, eu, size, es);
        if (mysql_query(db, q))
            goto end;
    }
    snprintf(q, sizeof(q), "SELECT file_id FROM file_info WHERE md5='%s' FOR UPDATE", md5);
    int found = scalar(db, q, existing, sizeof(existing));
    if (found) {
        rc = found == 1 ? 1 : -1;
        goto end;
    }
    /* A concurrent publisher may have won the unique fingerprint. Its content is
     * canonical; persist cleanup for the losing physical object in this commit. */
    if (id && strcmp(existing, id) && content_queue_gc(db, id))
        goto end;
    snprintf(q, sizeof(q),
             "INSERT INTO user_file_list(user,md5,file_name,create_time,shared_status,pv) "
             "VALUES('%s','%s','%s',NOW(),0,0)",
             u, md5, n);
    if (mysql_query(db, q)) {
        if (mysql_errno(db) != 1062)
            goto end;
    } else
        inserted = 1;
    if (inserted) {
        snprintf(q, sizeof(q), "UPDATE file_info SET count=count+1 WHERE md5='%s'", md5);
        if (mysql_query(db, q))
            goto end;
        snprintf(q, sizeof(q),
                 "INSERT INTO user_file_count(user,count) VALUES('%s',1) ON DUPLICATE KEY UPDATE "
                 "count=count+1",
                 u);
        if (mysql_query(db, q))
            goto end;
    }
    if (mysql_commit(db))
        goto end;
    return inserted ? 0 : 2;
end:
    mysql_rollback(db);
    return rc;
}
int content_reuse(const char *u, const char *m, const char *n) {
    MYSQL *db = content_db();
    int rc = -1;
    if (!db)
        return -1;
    if (!content_lock(db, m)) {
        rc = content_bind(db, u, m, n, 0, NULL, NULL);
        content_unlock(db, m);
    }
    mysql_close(db);
    return rc;
}
int content_detach(MYSQL *db, const char *user, const char *md5, const char *name) {
    char u[257], n[1025], q[4096], id[257], v[32];
    if (!db || !content_valid_md5(md5) || strlen(user) > 32 || strlen(name) > 128)
        return -1;
    mysql_real_escape_string(db, u, user, strlen(user));
    mysql_real_escape_string(db, n, name, strlen(name));
    if (mysql_query(db, "START TRANSACTION"))
        return -1;
    snprintf(q, sizeof(q), "SELECT file_id FROM file_info WHERE md5='%s' FOR UPDATE", md5);
    int found = scalar(db, q, id, sizeof(id));
    if (found < 0)
        goto fail;
    if (found == 1) {
        mysql_rollback(db);
        return 0;
    }
    snprintf(q, sizeof(q),
             "DELETE FROM user_file_list WHERE user='%s' AND md5='%s' AND file_name='%s'", u, md5,
             n);
    if (mysql_query(db, q))
        goto fail;
    if (mysql_affected_rows(db) == 0) {
        mysql_rollback(db);
        return 0;
    }
    snprintf(q, sizeof(q),
             "DELETE FROM share_file_list WHERE user='%s' AND md5='%s' AND file_name='%s'", u, md5,
             n);
    if (mysql_query(db, q))
        goto fail;
    unsigned long long shared = mysql_affected_rows(db);
    if (shared) {
        snprintf(q, sizeof(q),
                 "UPDATE user_file_count SET count=GREATEST(count-%llu,0) WHERE user='%s'", shared,
                 FILE_PUBLIC_COUNT);
        if (mysql_query(db, q))
            goto fail;
    }
    snprintf(q, sizeof(q), "UPDATE user_file_count SET count=GREATEST(count-1,0) WHERE user='%s'",
             u);
    if (mysql_query(db, q))
        goto fail;
    /* Keep the user's AI entry when the same content is still present under another name. */
    snprintf(q, sizeof(q),
             "DELETE FROM user_file_ai_desc WHERE user='%s' AND md5='%s' AND NOT EXISTS(SELECT 1 "
             "FROM user_file_list WHERE user='%s' AND md5='%s')",
             u, md5, u, md5);
    if (mysql_query(db, q))
        goto fail;
    snprintf(q, sizeof(q), "UPDATE file_info SET count=count-1 WHERE md5='%s' AND count>0", md5);
    if (mysql_query(db, q))
        goto fail;
    snprintf(q, sizeof(q), "SELECT count FROM file_info WHERE md5='%s'", md5);
    if (scalar(db, q, v, sizeof(v)))
        goto fail;
    if (atoll(v) == 0) {
        if (content_queue_gc(db, id))
            goto fail;
        snprintf(q, sizeof(q), "DELETE FROM file_info WHERE md5='%s' AND count=0", md5);
        if (mysql_query(db, q))
            goto fail;
    }
    if (mysql_commit(db))
        goto fail;
    return 0;
fail:
    mysql_rollback(db);
    return -1;
}
