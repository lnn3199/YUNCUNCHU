#include "chunk_store.h"
#include "cfg.h"
#include "content_store.h"
#include "fcgi_stdio.h"
#include "md5.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static char chunk_directory[401] = "/tmp/chunks";

/* libfcgi replaces environ with request parameters on Accept. Capture the
 * trusted process configuration once, before accepting any request. */
void chunk_configure(void) {
    const char *value = getenv("YCC_CHUNK_DIR");
    if (value && value[0] == '/' && strlen(value) < sizeof(chunk_directory))
        strcpy(chunk_directory, value);
}

const char *chunk_base(void) { return chunk_directory; }

redisContext *chunk_redis(void) {
    char host[128] = {0}, port[16] = {0};
    if (get_cfg_value(CFG_PATH, "redis", "ip", host) ||
        get_cfg_value(CFG_PATH, "redis", "port", port))
        return NULL;
    return rop_connectdb_nopwd(host, port);
}
cJSON *chunk_read_json(void) {
    const char *s = getenv("CONTENT_LENGTH");
    char b[4096];
    long n = s ? strtol(s, NULL, 10) : 0;
    if (n <= 0 || n >= (long)sizeof(b) || fread(b, 1, n, stdin) != (size_t)n)
        return NULL;
    b[n] = 0;
    return cJSON_Parse(b);
}
int chunk_json_string(cJSON *r, const char *key, char *out, size_t cap) {
    cJSON *v = cJSON_GetObjectItem(r, key);
    if (!v || v->type != cJSON_String || !v->valuestring || !*v->valuestring ||
        strlen(v->valuestring) >= cap)
        return -1;
    strcpy(out, v->valuestring);
    return 0;
}
static void hex_digest(MD5_CTX *ctx, char out[33]) {
    unsigned char digest[16];
    MD5Final(ctx, digest);
    for (int i = 0; i < 16; i++)
        sprintf(out + i * 2, "%02x", digest[i]);
}
int chunk_paths(ChunkSession *s) {
    const char *base = chunk_base();
    if (strlen(base) > 400 || (mkdir(base, 0700) && errno != EEXIST))
        return -1;
    MD5_CTX ctx;
    MD5Init(&ctx);
    MD5Update(&ctx, (unsigned char *)s->user, strlen(s->user) + 1);
    MD5Update(&ctx, (unsigned char *)s->md5, 32);
    MD5Update(&ctx, (unsigned char *)s->name, strlen(s->name));
    hex_digest(&ctx, s->id);
    snprintf(s->dir, sizeof(s->dir), "%s/%s", base, s->id);
    snprintf(s->key, sizeof(s->key), "chunk:v2:%s", s->id);
    snprintf(s->set, sizeof(s->set), "%s:received", s->key);
    return 0;
}
int chunk_parse(cJSON *r, ChunkSession *s, int init) {
    memset(s, 0, sizeof(*s));
    if (!r || chunk_json_string(r, "user", s->user, sizeof(s->user)) || strlen(s->user) > 32 ||
        chunk_json_string(r, "token", s->token, sizeof(s->token)) ||
        chunk_json_string(r, "md5", s->md5, sizeof(s->md5)) || !content_valid_md5(s->md5) ||
        chunk_json_string(r, "filename", s->name, sizeof(s->name)))
        return -1;
    if (init) {
        cJSON *size = cJSON_GetObjectItem(r, "size"), *count = cJSON_GetObjectItem(r, "chunkCount");
        if (!size || size->type != cJSON_Number || !count || count->type != cJSON_Number ||
            size->valuedouble <= 0 || size->valuedouble > CHUNK_BYTES * CHUNK_MAX_COUNT ||
            count->valuedouble != count->valueint)
            return -1;
        s->size = (long long)size->valuedouble;
        s->count = count->valueint;
        if (size->valuedouble != s->size || s->count != (s->size + CHUNK_BYTES - 1) / CHUNK_BYTES)
            return -1;
    }
    return chunk_paths(s);
}
int chunk_lock(ChunkSession *s) {
    char p[544];
    snprintf(p, sizeof(p), "%s.lock", s->dir);
    int fd = open(p, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX)) {
        close(fd);
        return -1;
    }
    return fd;
}
void chunk_unlock(int fd) {
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
}
long long chunk_length(const ChunkSession *s, int i) {
    return i == s->count - 1 ? s->size - CHUNK_BYTES * i : CHUNK_BYTES;
}
int chunk_refresh(redisContext *r, ChunkSession *s) {
    redisReply *a = redisCommand(
        r, "EVAL %s 2 %s %s",
        "redis.call('EXPIRE',KEYS[1],86400); redis.call('EXPIRE',KEYS[2],86400); return 1", s->key,
        s->set);
    int ok = a && a->type == REDIS_REPLY_INTEGER;
    if (a)
        freeReplyObject(a);
    return ok ? 0 : -1;
}
int chunk_record(redisContext *r, ChunkSession *s, int i) {
    redisReply *a =
        redisCommand(r, "EVAL %s 2 %s %s %d",
                     "if redis.call('EXISTS',KEYS[1])==0 then return -1 end; local "
                     "n=redis.call('SADD',KEYS[2],ARGV[1]); redis.call('EXPIRE',KEYS[1],86400); "
                     "redis.call('EXPIRE',KEYS[2],86400); return n",
                     s->key, s->set, i);
    int ok = a && a->type == REDIS_REPLY_INTEGER && a->integer >= 0;
    if (a)
        freeReplyObject(a);
    return ok ? 0 : -1;
}
int chunk_metadata(redisContext *r, ChunkSession *s, int create) {
    redisReply *a = redisCommand(r, "HMGET %s filesize chunk_count", s->key);
    if (!a || a->type != REDIS_REPLY_ARRAY || a->elements != 2) {
        if (a)
            freeReplyObject(a);
        return -1;
    }
    if (a->element[0]->type == REDIS_REPLY_NIL) {
        freeReplyObject(a);
        if (!create)
            return -1;
        a = redisCommand(r, "HSET %s filesize %lld chunk_count %d user %s md5 %s", s->key, s->size,
                         s->count, s->user, s->md5);
        int ok = a && a->type == REDIS_REPLY_INTEGER;
        if (a)
            freeReplyObject(a);
        if (!ok)
            return -1;
        if (mkdir(s->dir, 0700) && errno != EEXIST)
            return -1;
    } else {
        if (a->element[0]->type != REDIS_REPLY_STRING ||
            a->element[1]->type != REDIS_REPLY_STRING) {
            freeReplyObject(a);
            return -1;
        }
        long long size = atoll(a->element[0]->str);
        int count = atoi(a->element[1]->str);
        freeReplyObject(a);
        if (size <= 0 || size > CHUNK_BYTES * CHUNK_MAX_COUNT ||
            count != (size + CHUNK_BYTES - 1) / CHUNK_BYTES ||
            (create && (size != s->size || count != s->count)))
            return -1;
        s->size = size;
        s->count = count;
    }
    return chunk_refresh(r, s);
}
int chunk_hash_file(const char *path, char hex[33]) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    unsigned char b[65536];
    ssize_t n;
    MD5_CTX ctx;
    MD5Init(&ctx);
    while ((n = read(fd, b, sizeof(b))) > 0)
        MD5Update(&ctx, b, n);
    close(fd);
    if (n < 0)
        return -1;
    hex_digest(&ctx, hex);
    return 0;
}
int chunk_verify(ChunkSession *s) {
    MD5_CTX ctx;
    MD5Init(&ctx);
    unsigned char b[65536];
    char p[544], hex[33];
    for (int i = 0; i < s->count; i++) {
        snprintf(p, sizeof(p), "%s/%d", s->dir, i);
        int fd = open(p, O_RDONLY | O_NOFOLLOW);
        struct stat st;
        if (fd < 0)
            return -1;
        if (fstat(fd, &st) || st.st_size != chunk_length(s, i)) {
            close(fd);
            return -1;
        }
        ssize_t n;
        while ((n = read(fd, b, sizeof(b))) > 0)
            MD5Update(&ctx, b, n);
        close(fd);
        if (n < 0)
            return -1;
    }
    hex_digest(&ctx, hex);
    return strcmp(hex, s->md5) ? -1 : 0;
}
int chunk_atomic_text(const char *path, const char *text) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0)
        return -1;
    size_t n = strlen(text);
    int ok = write(fd, text, n) == (ssize_t)n && !fsync(fd);
    close(fd);
    if (ok && !rename(tmp, path))
        return 0;
    unlink(tmp);
    return -1;
}
void chunk_cleanup(redisContext *r, ChunkSession *s) {
    /* Caller owns the session flock; never remove the lock inode. */
    char p[544];
    for (int i = 0; i < s->count; i++) {
        snprintf(p, sizeof(p), "%s/%d", s->dir, i);
        unlink(p);
    }
    snprintf(p, sizeof(p), "%s/appender", s->dir);
    unlink(p);
    rmdir(s->dir);
    redisReply *a = redisCommand(r, "DEL %s %s", s->key, s->set);
    if (a)
        freeReplyObject(a);
}
int chunk_query(const char *q, const char *name, char *out, size_t cap) {
    if (!q)
        return -1;
    size_t keylen = strlen(name);
    while (*q) {
        const char *end = strchr(q, '&');
        if (!end)
            end = q + strlen(q);
        if ((size_t)(end - q) > keylen && !strncmp(q, name, keylen) && q[keylen] == '=') {
            const char *v = q + keylen + 1;
            size_t n = end - v;
            if (!n || n >= cap)
                return -1;
            size_t j = 0;
            for (size_t i = 0; i < n; i++) {
                unsigned char c = v[i];
                if (c == '%') {
                    if (i + 2 >= n)
                        return -1;
                    char hex[3] = {v[i + 1], v[i + 2], 0}, *endhex;
                    long decoded = strtol(hex, &endhex, 16);
                    if (*endhex || !decoded)
                        return -1;
                    c = decoded;
                    i += 2;
                } else if (c == '+')
                    c = ' ';
                out[j++] = c;
            }
            out[j] = 0;
            return 0;
        }
        q = *end ? end + 1 : end;
    }
    return -1;
}
