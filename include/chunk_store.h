#ifndef CHUNK_STORE_H
#define CHUNK_STORE_H
#include "cJSON.h"
#include "redis_op.h"
#include <stddef.h>
#define CHUNK_BYTES (10LL * 1024 * 1024)
#define CHUNK_MAX_COUNT 10000
#define CHUNK_TTL 86400
typedef struct {
    char user[129], token[129], md5[33], name[129], id[33];
    char dir[512], key[80], set[96];
    long long size;
    int count;
} ChunkSession;
redisContext *chunk_redis(void);
void chunk_configure(void);
const char *chunk_base(void);
cJSON *chunk_read_json(void);
int chunk_json_string(cJSON *root, const char *key, char *out, size_t capacity);
int chunk_parse(cJSON *root, ChunkSession *s, int init);
int chunk_paths(ChunkSession *s);
int chunk_lock(ChunkSession *s);
void chunk_unlock(int fd);
long long chunk_length(const ChunkSession *s, int index);
int chunk_metadata(redisContext *r, ChunkSession *s, int create);
int chunk_refresh(redisContext *r, ChunkSession *s);
int chunk_record(redisContext *r, ChunkSession *s, int index);
int chunk_verify(ChunkSession *s);
void chunk_cleanup(redisContext *r, ChunkSession *s);
int chunk_hash_file(const char *path, char hex[33]);
int chunk_atomic_text(const char *path, const char *text);
int chunk_query(const char *query, const char *name, char *out, size_t cap);
#endif
