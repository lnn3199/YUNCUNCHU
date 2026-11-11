#ifndef CONTENT_STORE_H
#define CONTENT_STORE_H
#include <mysql/mysql.h>
/* All writers take the same connection-scoped fingerprint lock, including deletion.
 * Return values: 0=new association, 1=content absent, 2=already associated, -1=error. */
MYSQL *content_db(void);
int content_valid_md5(const char *md5);
int content_lock(MYSQL *db, const char *md5);
void content_unlock(MYSQL *db, const char *md5);
int content_bind(MYSQL *db, const char *user, const char *md5, const char *name, long long size,
                 const char *file_id, const char *url);
int content_reuse(const char *user, const char *md5, const char *name);
/* Caller holds content_lock. Transactionally detaches and queues unreferenced storage. */
int content_detach(MYSQL *db, const char *user, const char *md5, const char *name);
int content_queue_gc(MYSQL *db, const char *file_id);
#endif
