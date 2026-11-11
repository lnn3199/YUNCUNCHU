# 云存储系统三大优化实现方案

> 实施状态（2026-10-19）：核心优化已实现并在隔离 Linux 环境验证。最终实测、实现范围和部署边界见 [测试报告](reports/optimization/README.md)，简历建议见 [简历表述](reports/optimization/resume.md)。本文保留原设计讨论；头尾采样预检未实现，实际使用完整文件 MD5。本文原有行号、预估值和示例不作为最终测试结论。

> 目标：断点续传（只补传缺失分片）、秒传与存储去重（1 份物理内容）、热门排行榜消除 N+1（详情 SQL 50→1）。
> 本文档给出：现状问题定位（精确到文件和行号）→ 目标数据模型 → 逐文件代码改动 → **每个收益数字的计算公式与实测方法** → 实施计划。

---

## 0. 现状问题定位（改造前）

### 0.1 分片续传：`src_cgi/chunk_upload_cgi.c` + `chunk_init_cgi.c` + `chunk_merge_cgi.c`

| # | 问题 | 位置 | 后果 |
|---|------|------|------|
| 1 | 已上传索引存在 Hash 的 `uploaded` 字段里，用**逗号字符串读-改-写**追加 | `chunk_upload_cgi.c:142-168` | ① 客户端超时重试同一分片 → 字符串追加重复索引 `"0,1,5,5"`，前端进度计数与服务端不一致，进度条回退/虚高；② 两个并发请求（多标签页、重复点击）读-改-写竞态，后完成的覆盖前一个追加 → **索引丢失**；③ 字符串随分片数线性膨胀 |
| 2 | 分片直接写最终路径 `/tmp/chunks/{md5}/{index}`，写一半进程被杀 → 留下半截文件 | `chunk_upload_cgi.c:115-132` | 半截文件 + Redis 无该索引，续传会重传覆盖（此场景侥幸正确），但若容器重启 `/tmp` 被清，Redis 有索引、文件丢失 |
| 3 | 合并失败**无法重入**：append 到第 i 片失败即 `break`，但前 i-1 片已被 `unlink()` | `chunk_merge_cgi.c:307-326` | 重试 merge 时 `verify_all_chunks()` 发现分片缺失 → 返回 `chunks incomplete` → 用户只能**全量重传**（这正是"网络中断导致整体重传"的主因） |
| 4 | 完整性校验只 `stat()` 存在性，不校验大小 | `chunk_merge_cgi.c:232-250` | 截断/损坏分片直接进 FastDFS，产出坏文件 |

### 0.2 秒传与去重：`src_cgi/md5_cgi.c` + `src_cgi/upload_cgi.c` + `src_cgi/chunk_merge_cgi.c`

| # | 问题 | 位置 | 后果 |
|---|------|------|------|
| 5 | 秒传逻辑在 `md5_cgi.c:129-297` 和 `upload_cgi.c:59-164`（`bind_existing_file_to_user`）**复制了两份**，`chunk_merge_cgi.c:61-166` 是第三份 | 三处 | 三份实现都要各自改，必然漂移 |
| 6 | `update file_info set count = %d where md5=...`（%d = 内存里读出来的 count+1） | `md5_cgi.c:189`、`upload_cgi.c:109`、`chunk_merge_cgi.c:111` | **非原子**：两个用户同时秒传同一文件，都读到 count=5，都写 6 → 引用计数丢失 1 |
| 7 | 4 条 SQL 无事务包裹 | `md5_cgi.c:189-251` | insert `user_file_list` 成功但 update count 失败 → 计数漂移，且无回滚 |
| 8 | `user_file_list` 无 `(user, md5, file_name)` 唯一约束，靠 select 防重 | `docker/mysql/init.sql:58-68` | 并发秒传/重复提交 → 同一用户插重复行，`user_file_count` 重复累计 |
| 9 | 大文件秒传要先由前端算**全量 MD5**（SparkMD5） | `picture_bed/src/services/images.js:41,143` | 1GB 文件计算数秒~数十秒，且无法在计算完成前预检 |

### 0.3 排行榜 N+1：`src_cgi/sharefiles_cgi.c:get_ranking_filelist()`

每展示 1 条排名，循环体内做 3 次往返：

```
for 每个元素 (line 554-618):
    rop_hash_get   → 1 次 Redis 命令   (line 562)
    rop_zset_get_score → 1 次 Redis命令 (line 571)
    mysql_query 详情 → 1 条 SQL        (line 586)
```

50 条/页 = **50 条详情 SQL + 100 次 Redis 命令**，且全部串行。另外 `rop_zset_get_score` 返回 `int`（`redis_op.c:1284` 附近实现用 `reply->integer` 截断），pv 超过 2^31 会溢出。

---

## 1. 总体架构（改造后）

```
前端 React                      C FastCGI (spawn-fcgi)              存储层
─────────────                  ───────────────────────              ───────
计算 MD5 / 指纹
   │
   ├─ POST /api/md5 ────────► md5_cgi ──────────────┐
   │   {fingerprint, md5?}   ├─ file_info 指纹/md5 命中 → 秒传
   │                         └─ dedup_bind_file() 事务建关联
   │
   ├─ POST /api/chunk_init ─► chunk_init_cgi ───────► Redis
   │   ◄── uploadedChunks       SMEMBERS chunk:done:{md5}    Set(去重)
   │
   ├─ POST /api/chunk_upload ► chunk_upload_cgi ─────► /tmp/chunks/{md5}/{i}
   │   ?md5&index             写 .part → fsync → rename → SADD
   │
   └─ POST /api/chunk_merge ─► chunk_merge_cgi
       FastDFS appender 逐片合并（断点续合并，chunk:merge:{md5} 状态机）
       入库复用 dedup_bind_file()
                                 sharefiles_cgi?cmd=pvdesc
       ZREVRANGE WITHSCORES(1次) + 批量详情 SQL(1条) ──► MySQL
```

Redis key 全景（改造后）：

| Key | 类型 | 内容 | TTL |
|-----|------|------|-----|
| `chunk:meta:{md5}` | Hash | filename/filesize/chunk_count/user | 24h |
| `chunk:done:{md5}` | **Set** | 已完整落盘的分片索引 | 24h |
| `chunk:merge:{md5}` | Hash | fileid + merged（已合并到第几片） | 24h |
| `FILE_PUBLIC_ZSET` | ZSet | member=md5+filename, score=pv | 无 |
| `FILE_NAME_HASH` | Hash | fileid → filename（保留兼容） | 无 |

---

## 2. 优化一：大文件断点续传

### 2.1 设计要点

1. **Redis Set 管理分片索引并去重**：`SADD chunk:done:{md5} {index}`。Set 语义天然保证：重复上传同一分片不产生重复索引；`SCARD` 原子得到已完成数；`SMEMBERS` 一次拿全量续传清单。
2. **临时文件 + 原子重命名**：分片先写 `{index}.part`，`fsync()` 后 `rename()` 成 `{index}`。保证磁盘上出现的 `{index}` 一定是完整分片。
3. **登记顺序**：必须先 rename 落盘、后 SADD 登记。任何时刻 Redis 中的索引都对应完整文件——反过来（先登记后写盘）会在容器重启时产生"有索引无文件"。
4. **断点续合并**：合并进度写 `chunk:merge:{md5}`（fileid + 已合并片数），append 失败重试时从断点继续，不必重传分片。

### 2.2 新增 Redis 封装：`common/redis_op.c` + `include/redis_op.h`

```c
/* redis_op.h 新增声明 */
int rop_set_add(redisContext *conn, char *key, char *member);
int rop_set_card(redisContext *conn, char *key);
int rop_set_members(redisContext *conn, char *key, RVALUES values, int *get_num);
int rop_zset_zrevrange_withscores(redisContext *conn, char *key, int from_pos,
                                  int end_pos, RVALUES values, long *scores, int *get_num);
```

```c
/* redis_op.c 新增实现（风格对齐现有 rop_zset_zrevrange, 约 1230 行） */

/* SADD：幂等，重复添加同一 member 返回 0（added=0）也视为成功 */
int rop_set_add(redisContext *conn, char *key, char *member)
{
    int retn = 0;
    redisReply *reply = NULL;

    reply = redisCommand(conn, "SADD %s %s", key, member);
    if (reply == NULL || reply->type != REDIS_REPLY_INTEGER)
    {
        LOG(REDIS_LOG_MODULE, REDIS_LOG_PROC, "[-][GMS_REDIS]SADD %s %s error! %s\n",
            key, member, conn->errstr);
        retn = -1;
        goto END;
    }
END:
    if (reply != NULL) freeReplyObject(reply);
    return retn;
}

/* SCARD：返回集合元素个数，失败 -1 */
int rop_set_card(redisContext *conn, char *key)
{
    int retn = 0;
    long n = 0;
    redisReply *reply = NULL;

    reply = redisCommand(conn, "SCARD %s", key);
    if (reply == NULL || reply->type != REDIS_REPLY_INTEGER)
    {
        LOG(REDIS_LOG_MODULE, REDIS_LOG_PROC, "[-][GMS_REDIS]SCARD %s error! %s\n",
            key, conn->errstr);
        retn = -1;
        goto END;
    }
    n = reply->integer;
END:
    if (reply != NULL) freeReplyObject(reply);
    return retn == 0 ? (int)n : -1;
}

/* SMEMBERS：取出全部成员；values 需调用方按可能的最大分片数分配 */
int rop_set_members(redisContext *conn, char *key, RVALUES values, int *get_num)
{
    int retn = 0;
    int i = 0;
    int n = 0;
    redisReply *reply = NULL;

    reply = redisCommand(conn, "SMEMBERS %s", key);
    if (reply == NULL || reply->type != REDIS_REPLY_ARRAY)
    {
        LOG(REDIS_LOG_MODULE, REDIS_LOG_PROC, "[-][GMS_REDIS]SMEMBERS %s error! %s\n",
            key, conn->errstr);
        retn = -1;
        goto END;
    }
    n = (int)reply->elements;
    for (i = 0; i < n; ++i)
    {
        strncpy(values[i], reply->element[i]->str, VALUES_ID_SIZE - 1);
        values[i][VALUES_ID_SIZE - 1] = 0;
    }
    *get_num = n;
END:
    if (reply != NULL) freeReplyObject(reply);
    return retn;
}

/* ZREVRANGE ... WITHSCORES：一次命令同时取 member 和 score。
 * 返回数组布局为 [m1, s1, m2, s2, ...]，共 2*n 个元素 */
int rop_zset_zrevrange_withscores(redisContext *conn, char *key, int from_pos,
                                  int end_pos, RVALUES values, long *scores, int *get_num)
{
    int retn = 0;
    int i = 0;
    int n = 0;
    redisReply *reply = NULL;

    reply = redisCommand(conn, "ZREVRANGE %s %d %d WITHSCORES", key, from_pos, end_pos);
    if (reply == NULL || reply->type != REDIS_REPLY_ARRAY)
    {
        LOG(REDIS_LOG_MODULE, REDIS_LOG_PROC, "[-][GMS_REDIS]ZREVRANGE %s WITHSCORES error! %s\n",
            key, conn->errstr);
        retn = -1;
        goto END;
    }
    n = (int)reply->elements / 2;
    for (i = 0; i < n; ++i)
    {
        strncpy(values[i], reply->element[2 * i]->str, VALUES_ID_SIZE - 1);
        values[i][VALUES_ID_SIZE - 1] = 0;
        scores[i] = strtol(reply->element[2 * i + 1]->str, NULL, 10);
    }
    *get_num = n;
END:
    if (reply != NULL) freeReplyObject(reply);
    return retn;
}
```

### 2.3 `chunk_upload_cgi.c` 改造（核心 20 行）

把原 111-168 行替换为：

```c
        /* ---- 写入临时文件，保证磁盘上的 {index} 永远完整 ---- */
        char part_path[512]  = {0};
        char final_path[512] = {0};
        sprintf(part_path,  "%s/%s/%d.part", CHUNK_TEMP_DIR, file_md5, chunk_index);
        sprintf(final_path, "%s/%s/%d",       CHUNK_TEMP_DIR, file_md5, chunk_index);

        int fd = open(part_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) { /* 同原逻辑，返回 code:1 */ }

        long total_written = 0;
        while (total_written < len)
        {
            int n = write(fd, chunk_buf + total_written, len - total_written);
            if (n <= 0) break;
            total_written += n;
        }
        if (total_written == len) fsync(fd);   /* 落盘，防容器重启丢半截文件 */
        close(fd);
        free(chunk_buf);

        if (total_written != len)
        {
            unlink(part_path);                 /* 清理残file */
            printf("{\"code\":1}");
            continue;
        }

        /* ---- 原子重命名：rename 后 {index} 即为完整分片 ---- */
        if (rename(part_path, final_path) != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                "rename %s -> %s err: %s\n", part_path, final_path, strerror(errno));
            unlink(part_path);
            printf("{\"code\":1}");
            continue;
        }

        /* ---- 登记索引：Set 去重，重复上传同一分片不产生重复记录 ---- */
        redisContext *redis_conn = rop_connectdb_nopwd(redis_ip, redis_port);
        if (redis_conn != NULL)
        {
            char done_key[512] = {0};
            char index_str[32] = {0};
            sprintf(done_key, "chunk:done:%s", file_md5);
            sprintf(index_str, "%d", chunk_index);

            if (rop_set_add(redis_conn, done_key, index_str) != 0)
            {
                LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "SADD %s %s err\n", done_key, index_str);
                /* 登记失败不返回错误：文件已完整落盘，客户端重试即可幂等补齐 */
            }
            /* 度量埋点：续传节省量统计的原始数据（见第 5 章） */
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                "CHUNK_OK md5=%s index=%d bytes=%ld done_total=%d\n",
                file_md5, chunk_index, len, rop_set_card(redis_conn, done_key));

            rop_disconnect(redis_conn);
        }

        printf("{\"code\":0}");
```

关键点：
- 删掉原 142-168 行整段（uploaded 字符串读-改-写）。
- 续传重传同一分片：文件被覆盖重写（rename 覆盖），SADD 幂等，`uploaded` 永不出现 `"5,5"`。
- 续传跳过已传分片由前端依据 init 返回的清单完成（前端已有此逻辑，`images.js:178-193`），无需改动。

### 2.4 `chunk_init_cgi.c` 改造

```c
        char done_key[512] = {0};
        char meta_key[512] = {0};
        sprintf(done_key, "chunk:done:%s", file_md5);
        sprintf(meta_key, "chunk:meta:%s", file_md5);

        if (rop_is_key_exist(redis_conn, meta_key) == 1)
        {
            /* ---- 断点续传：从 Set 直接拿已完成索引（天然去重） ---- */
            RVALUES idx = (RVALUES)calloc(chunk_count > 0 ? chunk_count : 1, VALUES_ID_SIZE);
            int n = 0;
            char uploaded[4096] = {0};

            if (rop_set_members(redis_conn, done_key, idx, &n) == 0)
            {
                for (int i = 0; i < n; i++)
                {
                    if (i > 0) strcat(uploaded, ",");
                    strcat(uploaded, idx[i]);
                }
            }
            free(idx);

            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                "resume md5=%s, done=%d/%d, uploaded=%s\n", file_md5, n, chunk_count, uploaded);

            /* 响应格式与前端现有解析完全兼容（images.js:179 读 uploadedChunks/uploaded） */
            printf("{\"code\":0,\"chunkCount\":%d,\"uploadedChunks\":\"%s\"}",
                   chunk_count, uploaded);
            rop_disconnect(redis_conn);
            continue;
        }

        /* ---- 新上传：初始化 meta + done 两个 key ---- */
        rop_hash_set(redis_conn, meta_key, "filename", filename);
        char size_str[64] = {0};  sprintf(size_str, "%ld", filesize);
        rop_hash_set(redis_conn, meta_key, "filesize", size_str);
        char count_str[32] = {0}; sprintf(count_str, "%d", chunk_count);
        rop_hash_set(redis_conn, meta_key, "chunk_count", count_str);
        rop_hash_set(redis_conn, meta_key, "user", user);

        rop_del_key(redis_conn, done_key);   /* 防御：清掉 md5 相同但 meta 已过期的残留 */
        redisReply *reply = redisCommand(redis_conn,
            "MULTI"); /* 用 pipeline 设两个 TTL，减少往返 */
        if (reply) { freeReplyObject(reply); reply = NULL; }
        reply = redisCommand(redis_conn, "EXPIRE %s 86400", meta_key);
        if (reply) { freeReplyObject(reply); reply = NULL; }
        reply = redisCommand(redis_conn, "EXPIRE %s 86400", done_key);
        if (reply) { freeReplyObject(reply); reply = NULL; }
        reply = redisCommand(redis_conn, "EXEC");
        if (reply) freeReplyObject(reply);
```

### 2.5 `chunk_merge_cgi.c` 改造：断点续合并

替换 `merge_chunks_to_fastdfs()`（原 253-336 行）：

```c
/* 返回 0 成功；-1 失败（可重入重试） */
int merge_chunks_to_fastdfs(redisContext *redis_conn, char *file_md5,
                            int chunk_count, char *filename, char *fileid)
{
    int result = 0;
    char path[512] = {0};
    char mkey[512] = {0};
    char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
    ConnectionInfo *pTrackerServer = NULL;
    ConnectionInfo storageServer;
    int store_path_index;
    int start_index = 1;
    int resumed = 0;

    sprintf(mkey, "chunk:merge:%s", file_md5);

    char suffix[SUFFIX_LEN] = {0};
    get_file_suffix(filename, suffix);

    pTrackerServer = tracker_get_connection();
    if (pTrackerServer == NULL) return -1;

    /* ---- 重入检查：是否已有 appender 文件和合并进度 ---- */
    char merged_str[32] = {0};
    if (rop_hash_get(redis_conn, mkey, "fileid", fileid) == 0 &&
        strlen(fileid) > 0)
    {
        resumed = 1;
        if (rop_hash_get(redis_conn, mkey, "merged", merged_str) != 0)
            merged_str[0] = '\0';
        start_index = atoi(merged_str);   /* 从下一片继续，前面的不重传 */
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
            "resume merge: fileid=%s, from chunk %d\n", fileid, start_index);

        /* appender 文件在某个 storage 上，按 fileid 反查连接 */
        if (tracker_query_storage_fetch(pTrackerServer, &storageServer, fileid) != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "tracker_query_storage_fetch err\n");
            tracker_close_connection_ex(pTrackerServer, true);
            return -1;
        }
    }
    else
    {
        /* ---- 首次：分片 0 创建 appender 文件 ---- */
        *group_name = '\0';
        result = tracker_query_storage_store(pTrackerServer, &storageServer,
                                             group_name, &store_path_index);
        if (result != 0) { tracker_close_connection_ex(pTrackerServer, true); return -1; }

        sprintf(path, "%s/%s/0", CHUNK_TEMP_DIR, file_md5);
        result = storage_upload_appender_by_filename1(
                     pTrackerServer, &storageServer, store_path_index,
                     path, suffix, NULL, 0, group_name, fileid);
        if (result != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "upload appender chunk 0 err: %d\n", result);
            tracker_close_connection_ex(pTrackerServer, true);
            return -1;
        }
        unlink(path);
        rop_hash_set(redis_conn, mkey, "fileid", fileid);
        rop_hash_set(redis_conn, mkey, "merged", "1");
        start_index = 1;
    }

    /* ---- 追加 start_index ~ chunk_count-1 ---- */
    for (int i = start_index; i < chunk_count; i++)
    {
        sprintf(path, "%s/%s/%d", CHUNK_TEMP_DIR, file_md5, i);

        /* 合并前校验分片大小，防截断文件进 FastDFS */
        struct stat st;
        if (stat(path, &st) != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "chunk %d missing: %s\n", i, path);
            result = -1;
            break;
        }

        result = storage_append_by_filename1(pTrackerServer, &storageServer, path, fileid);
        if (result != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                "append chunk %d err: %d (will resume later)\n", i, result);
            break;   /* 不删未合并的分片，保留现场供重入续合并 */
        }

        unlink(path);   /* 已合并的才删 */
        sprintf(merged_str, "%d", i + 1);
        rop_hash_set(redis_conn, mkey, "merged", merged_str);   /* 每片提交进度 */
    }

    tracker_close_connection_ex(pTrackerServer, true);

    if (result != 0)
    {
        return -1;   /* 保留 chunk:merge:{md5} 和剩余分片，允许客户端重试 merge */
    }

    /* ---- 全部合并完成：清理 ---- */
    char chunk_dir[512] = {0};
    sprintf(chunk_dir, "%s/%s", CHUNK_TEMP_DIR, file_md5);
    DIR *dir = opendir(chunk_dir);
    if (dir)
    {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char cp[512] = {0};
            sprintf(cp, "%s/%s", chunk_dir, entry->d_name);
            unlink(cp);
        }
        closedir(dir);
        rmdir(chunk_dir);
    }
    rop_del_key(redis_conn, mkey);
    return 0;
}
```

`main()` 中的调用改为：合并成功后清理 `chunk:meta:{md5}` 和 `chunk:done:{md5}`（原 612 行 `rop_del_key(redis_conn, redis_key)` 处，改成删三个 key）；合并失败时返回 `{"code":1,"msg":"merge failed, retryable"}`，前端对可重试错误自动重试 merge（前端 `images.js:231` 处加一个重试循环，最多 3 次，间隔 2s）。

### 2.6 前端改动（`picture_bed/src/services/images.js`）

- 无需大改：`images.js:178-193` 已按 `uploadedChunks` 跳过已完成分片。
- 补两处：
  1. merge 失败（`mergeData.code !== 0`）时自动重试，最多 3 次；
  2. 分片上传从串行改为**小并发**（可选优化）：把 for 循环改成每批 3 个 `Promise.all`，100MB/10MB 片从 10 个 RTT 降为 ~4 个 RTT。注意 `chunk_upload` 的 FCGI 进程数（`docker/fastcgi_app/start.sh` 中 spawn-fcgi 数量）要 ≥ 并发数。

---

## 3. 优化二：秒传与存储去重

### 3.1 增量 MD5 指纹设计

**问题**：大文件秒传要先算全量 MD5（SparkMD5 遍历全文件）。
**方案**：抽样指纹 = 前 16MB + 后 16MB（可配置）拼接后 MD5，作为 `file_info.fingerprint`。

```js
// picture_bed/src/services/images.js 新增
export const calculateFingerprint = async (file) => {
  const HEAD = 16 * 1024 * 1024, TAIL = 16 * 1024 * 1024;
  const spark = new SparkMD5.ArrayBuffer();
  spark.append(await file.slice(0, Math.min(HEAD, file.size)).arrayBuffer());
  if (file.size > HEAD + TAIL) {
    spark.append(await file.slice(file.size - TAIL, file.size).arrayBuffer());
  }
  return spark.end();   // 32 位 hex
};
```

秒传判定链（`md5_cgi`）：
1. 带全量 md5 请求 → `file_info.md5` 唯一索引命中 → **精确秒传**（与现网一致）；
2. 只带 fingerprint（大文件未算完全量 MD5）→ `file_info.fingerprint` 命中 → **预检秒传**：立即建关联返回成功，同时投递一个后台全量 MD5 校验任务，不一致则回滚关联并通知客户端重传（防碰撞，概率约 2^-128 × 文件数，工程上仅作防御）。
3. 均未命中 → 正常上传；上传完成后 `UPDATE file_info SET fingerprint='...' WHERE md5='...'`。

指纹索引的代价：多 32 字节/文件 + 一个二级索引；收益：GB 级文件省掉全量 MD5 计算时间（本地磁盘读 1GB ≈ 2-5s）。

### 3.2 DDL（`docker/mysql/init.sql` 追加）

```sql
-- 1) 关联唯一约束：并发秒传的最后防线（执行前必须先做重复数据检测，见 6.2）
ALTER TABLE user_file_list
  ADD UNIQUE KEY uq_user_md5_name (user, md5(191), file_name(191));

-- 2) 增量指纹字段 + 索引
ALTER TABLE file_info
  ADD COLUMN fingerprint varchar(64) NOT NULL DEFAULT '' COMMENT '增量MD5指纹(头尾抽样)',
  ADD KEY idx_fingerprint (fingerprint);

-- 3) 用户文件数 upsert 依赖 user 唯一键（已存在 user_UNIQUE，无需改）
```

### 3.3 公共去重模块（消除三份重复实现）

新增 `common/deal_dedup.c` + `include/deal_dedup.h`，`md5_cgi` / `upload_cgi` / `chunk_merge` 三处共用：

```c
/* deal_dedup.h */
#ifndef _DEAL_DEDUP_H_
#define _DEAL_DEDUP_H_
#include <mysql.h>

#define DEDUP_BIND_OK   0   /* 复用成功：仅建立用户关联，无物理传输 */
#define DEDUP_NO_FILE   1   /* 服务端无此内容，调用方继续物理上传 */
#define DEDUP_USER_HAS  2   /* 该用户已拥有此文件 */
#define DEDUP_ERR      -1   /* 数据库错误（已回滚） */

/* conn 必须已连接且 set names utf8；函数内部自带事务 */
int dedup_bind_file(MYSQL *conn, const char *user,
                    const char *filename, const char *md5);
#endif
```

```c
/* deal_dedup.c */
#include "deal_dedup.h"
#include "deal_mysql.h"
#include "make_log.h"

#define DEDUP_LOG_MODULE "cgi"
#define DEDUP_LOG_PROC   "dedup"

int dedup_bind_file(MYSQL *conn, const char *user,
                    const char *filename, const char *md5)
{
    char sql_cmd[SQL_MAX_LEN] = {0};
    char tmp[512] = {0};
    int ret2 = 0;

    if (mysql_query(conn, "START TRANSACTION") != 0)
    {
        LOG(DEDUP_LOG_MODULE, DEDUP_LOG_PROC, "START TRANSACTION err: %s\n", mysql_error(conn));
        return DEDUP_ERR;
    }

    /* 1) file_info 行锁：并发秒传同一 md5 在此串行化，count 永远准确 */
    sprintf(sql_cmd, "SELECT count FROM file_info WHERE md5 = '%s' FOR UPDATE", md5);
    ret2 = process_result_one(conn, sql_cmd, tmp);
    if (ret2 == 1)          /* 无记录 → 服务端没有此内容 */
    {
        mysql_query(conn, "ROLLBACK");
        return DEDUP_NO_FILE;
    }
    if (ret2 != 0)
    {
        mysql_query(conn, "ROLLBACK");
        return DEDUP_ERR;
    }

    /* 2) 用户是否已有此文件（友好返回值；并发下由唯一键兜底） */
    sprintf(sql_cmd,
        "SELECT id FROM user_file_list WHERE user = '%s' AND md5 = '%s' AND file_name = '%s' LIMIT 1",
        user, md5, filename);
    memset(tmp, 0, sizeof(tmp));
    if (process_result_one(conn, sql_cmd, tmp) == 0)
    {
        mysql_query(conn, "ROLLBACK");
        return DEDUP_USER_HAS;
    }

    /* 3) 建立用户-文件关联（无物理内容传输 = 秒传） */
    sprintf(sql_cmd,
        "INSERT INTO user_file_list(user, md5, create_time, file_name, shared_status, pv) "
        "VALUES('%s', '%s', NOW(), '%s', 0, 0)", user, md5, filename);
    if (mysql_query(conn, sql_cmd) != 0)
    {
        if (mysql_errno(conn) == 1062)   /* 唯一键冲突：并发下别的请求已插入 */
        {
            mysql_query(conn, "ROLLBACK");
            return DEDUP_USER_HAS;
        }
        LOG(DEDUP_LOG_MODULE, DEDUP_LOG_PROC, "%s err: %s\n", sql_cmd, mysql_error(conn));
        mysql_query(conn, "ROLLBACK");
        return DEDUP_ERR;
    }

    /* 4) 引用计数原子 +1：count = count + 1，由 MySQL 行锁保证不丢更新 */
    sprintf(sql_cmd, "UPDATE file_info SET count = count + 1 WHERE md5 = '%s'", md5);
    if (mysql_query(conn, sql_cmd) != 0)
    {
        mysql_query(conn, "ROLLBACK");
        return DEDUP_ERR;
    }

    /* 5) 用户文件数 upsert 原子 +1 */
    sprintf(sql_cmd,
        "INSERT INTO user_file_count(user, count) VALUES('%s', 1) "
        "ON DUPLICATE KEY UPDATE count = count + 1", user);
    if (mysql_query(conn, sql_cmd) != 0)
    {
        mysql_query(conn, "ROLLBACK");
        return DEDUP_ERR;
    }

    if (mysql_query(conn, "COMMIT") != 0)
    {
        mysql_query(conn, "ROLLBACK");
        return DEDUP_ERR;
    }
    return DEDUP_BIND_OK;
}
```

### 3.4 三个 CGI 的接入点

| 文件 | 现状 | 改为 |
|------|------|------|
| `md5_cgi.c` | `deal_md5()` 167-251 行自己拼 4 条 SQL | 保留 md5 存在性查询；命中 → 调 `dedup_bind_file(conn, user, filename, md5)`；新增 fingerprint 预检分支 |
| `upload_cgi.c` | `bind_existing_file_to_user()` 59-164 行 | 整个函数删除，调用点（1077 行）改为 `dedup_bind_file()`；`store_fileinfo_to_mysql()` 中 count 更新改为 `count = count + 1` 原子写法 |
| `chunk_merge_cgi.c` | `bind_existing_file_to_user()` 61-166 行（第三份拷贝） | 同上删除，改为公共模块 |

`store_fileinfo_to_mysql()` 里的物理上传后入库，也统一收敛为：
```
insert file_info (md5 唯一键冲突则跳过)
→ insert user_file_list (唯一键冲突 → 1062 → 跳过且不 +count)
→ update file_info set count = count + 1
→ upsert user_file_count
```
与 `dedup_bind_file` 的差别仅在"是否已有物理内容"，可再抽一个 `store_new_file()`，本文不展开。

### 3.5 前端改动

`tryInstantUpload()`（`images.js:71-96`）改为先传 fingerprint，请求体增加 `fingerprint` 和 `size` 字段；`code:5`（已拥有）语义不变。`uploadChunked()` 里 `tryInstantUpload` 调用位置不变（chunk_init 前也做一次预检）。

---

## 4. 优化三：热门文件排行榜 N+1 消除

### 4.1 改造后的 `get_ranking_filelist()` 核心（`sharefiles_cgi.c:386` 起）

```c
    /*===5、从redis读取排名：一次 ZREVRANGE WITHSCORES 拿 member + score===*/
    value = (RVALUES)calloc(count, VALUES_ID_SIZE);
    long *scores = (long *)calloc(count, sizeof(long));
    if (value == NULL || scores == NULL) { ret = -1; goto END; }

    int n = 0;
    int end = start + count - 1;
    ret = rop_zset_zrevrange_withscores(redis_conn, FILE_PUBLIC_ZSET, start, end,
                                        value, scores, &n);
    if (ret != 0) { goto END; }

    /*===6、一条批量 SQL 查全部详情（原来 50 条）===*/
    /* member = md5(32位) + filename，本地切分，连 HGET 都省掉 */
    char in_list[SQL_MAX_LEN] = {0};
    int in_len = 0;
    for (int i = 0; i < n; i++)
    {
        char md5[33] = {0};
        strncpy(md5, value[i], 32);
        const char *fn = value[i] + 32;

        char md5_esc[66] = {0}, fn_esc[512] = {0};
        mysql_real_escape_string(conn, md5_esc, md5, strlen(md5));
        mysql_real_escape_string(conn, fn_esc, fn, strlen(fn));

        in_len += sprintf(in_list + in_len, "%s('%s','%s')",
                          i > 0 ? "," : "", md5_esc, fn_esc);
        if (in_len > SQL_MAX_LEN - 600) break;   /* 防超长，兜底截断 */
    }

    sprintf(sql_cmd,
        "SELECT s.user, f.url, f.size, f.type, s.create_time, s.md5, s.file_name "
        "FROM share_file_list s JOIN file_info f ON f.md5 = s.md5 "
        "WHERE (s.md5, s.file_name) IN (%s)", in_list);

    if (mysql_query(conn, sql_cmd) != 0) { ret = -1; goto END; }
    res_set = mysql_store_result(conn);
    if (res_set == NULL) { ret = -1; goto END; }

    /*===7、按 Redis 排名顺序组装（md5+filename → 行 的线性查找，50 条规模 O(n^2) 可忽略）===*/
    array = cJSON_CreateArray();
    for (int i = 0; i < n; i++)
    {
        char md5[33] = {0};
        strncpy(md5, value[i], 32);
        const char *fn = value[i] + 32;

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "md5", md5);
        cJSON_AddStringToObject(item, "file_name", fn);
        cJSON_AddNumberToObject(item, "share_status", 1);
        cJSON_AddNumberToObject(item, "pv", scores[i]);   /* score 直接来自 WITHSCORES */

        MYSQL_ROW row2 = NULL;
        mysql_data_seek(res_set, 0);
        while ((row2 = mysql_fetch_row(res_set)) != NULL)
        {
            if (row2[5] && row2[6] &&
                strcmp(row2[5], md5) == 0 && strcmp(row2[6], fn) == 0)
            {
                if (row2[0]) cJSON_AddStringToObject(item, "user", row2[0]);
                if (row2[1]) cJSON_AddStringToObject(item, "url", row2[1]);
                if (row2[2]) cJSON_AddNumberToObject(item, "size", atol(row2[2]));
                if (row2[3]) cJSON_AddStringToObject(item, "type", row2[3]);
                if (row2[4]) cJSON_AddStringToObject(item, "create_time", row2[4]);
                break;
            }
        }
        cJSON_AddItemToArray(array, item);
    }
    cJSON_AddItemToObject(root, "files", array);
```

改造前后对比（每页 50 条）：

| 项目 | 改造前 | 改造后 |
|------|--------|--------|
| MySQL 详情查询 | 50 条（循环内） | **1 条**（`(md5,file_name) IN (...)`） |
| MySQL 总查询 | 51 条（含 count 查询） | 2 条 |
| Redis 命令 | 1 ZREVRANGE + 50 HGET + 50 ZSCORE = **101 次** | **1 次**（WITHSCORES） |
| 文件名来源 | HGET FILE_NAME_HASH | member 本地截取（0 次 RTT） |
| 大 pv 溢出 | `int` score 有 2^31 风险 | `long` score |

同时把 `dealsharefile_cgi.c:pv_file()` 的 `update share_file_list set pv = %d`（174 行）改为 `set pv = pv + 1`，与 Redis ZINCRBY 保持一致。

### 4.2 批量 SQL 备选方案

`(s.md5, s.file_name) IN (('a','b'),...)` 行构造符 IN 需要 MySQL 5.7+。若目标库更老或优化器选错索引，退化为：
```sql
SELECT s.user, f.url, f.size, f.type, s.create_time, s.md5, s.file_name
FROM share_file_list s JOIN file_info f ON f.md5 = s.md5
WHERE s.md5 IN ('m1','m2',...)   -- md5 去重后 IN
```
再在内存里按 (md5, file_name) 匹配。效果等价（仍是 1 条 SQL）。

---

## 5. 收益度量：每个数字怎么得出来

> 原则：**不引用任何无法复现的数字**。每个指标给出计算公式、采集方法、实验步骤、预期输出。改造前后的 A/B 数据由实验实测填写，下表最后一列是记录模板。

### 5.1 度量基础设施（先埋点，后优化）

**埋点 1：CGI 结构化日志**（`chunk_upload_cgi.c` 已在 2.3 节加入）：
```
CHUNK_OK md5=<md5> index=<i> bytes=<本片字节> done_total=<SCARD>
```
一条完整上传（含续传增量）累加 `bytes` 即**客户端实际上传量**；`done_total × 分片大小` 即**服务端已持有量**。

**埋点 2：MySQL SQL 计数**（无需改代码，用全局状态差值）：
```sql
SHOW GLOBAL STATUS LIKE 'Com_select';   -- 请求前后各取一次，差值 = 本次请求的 select 条数
```
（对压测场景更准的替代：`performance_schema.events_statements_summary_by_digest` 按 digest 聚合。）

**埋点 3：Redis 命令计数**：
```
INFO commandstats      -- 看 ZREVRANGE / HGET / ZSCORE / SMEMBERS 的 calls 增量
```

**埋点 4：nginx 层**（`docker/nginx_fastdfs/nginx.conf` 的 http 块）：
```nginx
log_format metric '$status $request_time $upstream_response_time $uri';
access_log /var/log/nginx/access_metric.log metric;
```

### 5.2 指标一：断点续传「只补传 40MB，减少 60% 重传」

**公式**（对一次"中断后恢复"的上传会话）：

```
saved_bytes   = Σ(已完成分片大小)          = SCARD(chunk:done:{md5}) × chunk_size（末片按实际）
saved_ratio   = saved_bytes / total_size
retrans_bytes = total_size − saved_bytes
```

100MB、10MB/片、完成 60MB 后中断：`SCARD=6` → `saved=60MB` → `saved_ratio=60%`，客户端重连后只发 40MB。若中断点在第 7 片传一半，那半片作废（`.part` 被清理），节省 60% 是下限，上限 99%（断在第 10 片末尾）。

**实验步骤**（改造前后各跑一遍，对比 `retrans_bytes`）：

```bash
# 0. 准备
dd if=/dev/urandom of=/tmp/bench.bin bs=1M count=100
MD5=$(md5sum /tmp/bench.bin | cut -d' ' -f1)

# 1. 模拟"上传到 60MB 中断"：只发 6 片然后强杀
for i in 0 1 2 3 4 5; do
  dd if=/tmp/bench.bin bs=1M skip=$i count=1 2>/dev/null | \
  curl -s -X POST --data-binary @- "http://127.0.0.1/api/chunk_upload?md5=$MD5&index=$i"
done
kill %1   # 或 Ctrl-C：模拟网络中断/关闭页面

# 2. 恢复上传：前端/脚本先查 init 拿 uploadedChunks，只发缺失分片
curl -s -X POST -H 'Content-Type: application/json' \
  -d "{\"user\":\"bench\",\"token\":\"$TOKEN\",\"filename\":\"bench.bin\",\"md5\":\"$MD5\",\"size\":104857600,\"chunkCount\":10}" \
  "http://127.0.0.1/api/chunk_init"
# 响应 uploadedChunks="0,1,2,3,4,5" → 只发 6,7,8,9

# 3. 统计：本次会话服务端实际接收的分片字节（埋点日志累加）
grep "CHUNK_OK md5=$MD5" /path/to/cgi.log | awk -F'bytes=' '{s+=$2} END {print "retrans_bytes="s}'
# 改造前(基线，禁用续传全量重传)：retrans_bytes ≈ 104857600
# 改造后：retrans_bytes ≈ 41943040  → saved_ratio = 60%
```

**配套验证**（正确性，不只是性能）：
- 重复上传同一分片后，`SMEMBERS chunk:done:{md5}` 元素个数不变（Set 去重生效）；
- 合并中途 `kill -9` merge 进程，重试 merge 能成功（断点续合并生效），`verify_all_chunks` 不再误报；
- 造一个半截 `.part`（写一半杀进程），确认 init 不把它算进 uploadedChunks。

### 5.3 指标二：去重「10 个用户复用，节省 90% 存储」

**公式**：
```
physical_files = COUNT(DISTINCT md5) in file_info      -- 物理内容份数
logical_files  = COUNT(*) in user_file_list            -- 用户可见文件数
saved_ratio    = 1 − physical_files / logical_files
```
N 个用户同一内容：`saved = 1 − 1/N`。N=10 → 90%。N=2 → 50%。**注意该值完全由复用度 N 决定，不是固定 90%**，度量时按实际 N 计算。

**实验步骤**：
```bash
# 1. 同一 50MB 文件，10 个用户依次秒传（或物理上传）
for u in u1 u2 ... u10; do
  curl -s -X POST -H 'Content-Type: application/json' \
    -d "{\"user\":\"$u\",\"token\":\"$TOKEN\",\"md5\":\"$MD5\",\"fileName\":\"same.bin\"}" \
    "http://127.0.0.1/api/md5"
done

# 2. 统计去重率
mysql yuncunchu -e "
SELECT COUNT(DISTINCT md5) AS physical, COUNT(*) AS logical
FROM file_info, user_file_list;"   -- 分开两条查更直观：
mysql yuncunchu -e "SELECT COUNT(*) physical FROM file_info;"
mysql yuncunchu -e "SELECT COUNT(*) logical  FROM user_file_list;"
# saved_ratio = 1 - physical/logical → 10 用户 → 0.9

# 3. 磁盘实证（最硬的证据）：FastDFS 数据目录增量
du -sb /data/fastdfs/storage/data   # 上传第一个文件后记一次
# ... 10 个用户全部完成后 ...
du -sb /data/fastdfs/storage/data   # 增量 ≈ 0（只有 user_file_list 行，无新物理内容）
```

**引用计数一致性校验**（改造前跑一次 vs 改造后跑一次，本身就是收益证据）：
```sql
SELECT f.md5, f.count, IFNULL(c.real_cnt, 0) AS real_cnt
FROM file_info f
LEFT JOIN (SELECT md5, COUNT(*) real_cnt FROM user_file_list GROUP BY md5) c
  ON f.md5 = c.md5
WHERE f.count <> IFNULL(c.real_cnt, 0);
```
- 改造前：非原子 `count=%d` + 无事务 + 无唯一键，并发秒传下此查询会列出**计数漂移行**；
- 改造后：`count = count + 1` + 事务 + 唯一键，结果集为**空**。

**并发竞态验证**（证明 count+1 原子化生效）：
```bash
# 20 个并发秒传同一 md5
for i in $(seq 20); do curl -s -X POST ... /api/md5 & done; wait
mysql yuncunchu -e "SELECT count FROM file_info WHERE md5='$MD5';"   -- 应恰好 = 初始+20
```
改造前常见结果是 < +20（丢更新）。

### 5.4 指标三：排行榜「详情 SQL 50→1，P95 A→B」

**公式**：
```
sql_per_request = Com_select 差值（一次 /api/sharefiles?cmd=pvdesc 请求）
redis_cmds      = INFO commandstats 的 calls 差值
```

**SQL 条数实测**：
```bash
# 请求前后取 Com_select 差值
mysql -e "SHOW GLOBAL STATUS LIKE 'Com_select'" > /tmp/before.txt
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"start":0,"count":50}' "http://127.0.0.1/api/sharefiles?cmd=pvdesc" > /dev/null
mysql -e "SHOW GLOBAL STATUS LIKE 'Com_select'" > /tmp/after.txt
# 差值：改造前 = 51（1 count + 50 详情）；改造后 = 2（1 count + 1 批量详情）
```

**P95 实测**（`ab`，注意 `?cmd=pvdesc` 的 POST body）：
```bash
cat > pvdesc.json <<'EOF'
{"start":0,"count":50}
EOF
ab -k -c 50 -n 2000 -T application/json -p pvdesc.json \
   "http://127.0.0.1/api/sharefiles?cmd=pvdesc" | tee ab_result.txt
# 关注输出尾部 "Percentage of the requests served within a certain time (ms)"
# 取 95% 行 → 即 P95
```
或 `wrk`（ percentile 更细）：
```bash
wrk -t4 -c50 -d30s --latency -s post.lua http://127.0.0.1/api/sharefiles?cmd=pvdesc
```

**P95 改善的推导依据**（说明 A→B 不是玄学）：
改造前每元素串行 3 次往返：1 条 MySQL 查询（同机房 RTT ≈ 0.2-1ms + 解析执行）+ 2 次 Redis 命令。50 条 ≈ 50×(0.5ms) + 100×(0.05ms) ≈ 25-50ms 纯 RTT 累加，再叠加 FCGI 单请求串行处理。改造后：1 次 ZREVRANGE（O(logN+50)）+ 1 条带 50 组行构造符的 JOIN（主键/唯一索引点查，执行计划近似 50 次索引合并），总往返从 151 次降到 2 次。**省下的主要是循环内串行 RTT**，因此 P95 降幅与页大小正相关；页越小收益越弱（这是必须说明的边界）。

**A/B 记录模板**（改造后实测填写）：

| 指标 | 基线（改造前） | 优化后 | 采集方式 |
|------|------|------|------|
| 续传重传字节（100MB 断在 60MB） | 104857600 | ____ | 埋点日志 awk 累加 |
| 续传节省比例 | 0% | ____ | 公式 5.2 |
| 10 用户复用物理份数 | 10 | ____ | `COUNT(*) file_info` |
| 存储节省比例 | 0% | ____ | 公式 5.3 |
| file_info.count 漂移行数 | ____ | 0 | 一致性 SQL |
| 排行榜详情 SQL/请求 | 51 | 2 | Com_select 差值 |
| 排行榜 Redis 命令/请求 | 101 | 1 | INFO commandstats |
| 排行榜 P95 (ms, count=50) | A | B | ab 95% 行 |
| 排行榜 P95 (ms, count=10) | — | — | 同上（页大小敏感性） |

---

## 6. 实施计划与风险

### 6.1 分期（每期独立上线、独立度量）

| 期 | 内容 | 依赖 | 度量验收 |
|----|------|------|----------|
| P0 | 排行榜 WITHSCORES + 批量 SQL + pv 原子自增 | 无 | 5.4 表：51→2、101→1、P95 下降 |
| P1 | dedup 公共模块 + 事务 + 原子 count + 唯一键 DDL | DDL 前置检查 | 5.3 表：漂移行=0、并发 20 次 +20 |
| P2 | 分片 Set + 原子 rename + 断点续合并 + merge 重试 | redis_op 新接口 | 5.2 表：重传 60%↓ |
| P3 | 增量指纹秒传 | P1 | GB 级文件预检耗时对比 |

建议 P0 先做：不碰上传链路、无 DDL、风险最低，且能立即验证整套度量方法是否可用。

### 6.2 风险与回滚

| 风险 | 缓解 |
|------|------|
| 加唯一索引失败：存量 `user_file_list` 已有重复行 | **DDL 前先执行**：`SELECT user,md5,file_name,COUNT(*) c FROM user_file_list GROUP BY user,md5,file_name HAVING c>1;` 清理后再建索引；索引失败不阻塞其他 P1 改动上线（代码侧 1062 分支已兼容） |
| 事务+`FOR UPDATE` 增加秒传延迟 | 锁持有时间 = 3 条短 SQL（ms 级）；秒传是低频操作，可接受；若实测有压力，退化方案：去掉 FOR UPDATE，仅靠唯一键 + `INSERT IGNORE` 语义 |
| 断点续合并依赖 `tracker_query_storage_fetch` 按 fileid 反查 storage | 需 FastDFS ≥ 5.05（appender 版本）；上线前在测试环境验证 append 续写 |
| `chunk:done` Set 的 24h TTL 到期但 meta 未到期 | 两个 key 同时设置相同 TTL；极端不一致时 init 走新建分支并 `DEL done`（2.4 节已防御） |
| 前端 merge 自动重试放大服务端压力 | 最多 3 次、间隔 2s；merge 幂等（状态机），重试无副作用 |
| `(md5,file_name) IN (...)` 行构造符兼容性 | 5.4.2 备选 `md5 IN (...)` 内存匹配方案 |
| 回滚 | P0 纯代码回滚 git revert；P1 DDL 索引可保留（不影响旧代码路径，1062 分支缺失时旧代码并发才会出问题，回滚窗口期可临时停秒传入口）；P2 回滚后旧 merge 逻辑对新状态机不兼容，需清理 `chunk:*` key（`redis-cli --scan --pattern 'chunk:*' | xargs redis-cli del`） |

### 6.3 改动文件清单

```
common/redis_op.c           +rop_set_add / rop_set_card / rop_set_members / rop_zset_zrevrange_withscores
include/redis_op.h          对应声明
common/deal_dedup.c         新增：事务化秒传绑定
include/deal_dedup.h        新增
src_cgi/chunk_init_cgi.c    meta/done 双 key + SMEMBERS 续传清单
src_cgi/chunk_upload_cgi.c  .part + fsync + rename + SADD + CHUNK_OK 埋点
src_cgi/chunk_merge_cgi.c   断点续合并状态机 + 接入 dedup 公共模块
src_cgi/md5_cgi.c           接入 dedup + fingerprint 预检分支
src_cgi/upload_cgi.c        删除 bind_existing_file_to_user，接入 dedup
src_cgi/sharefiles_cgi.c    get_ranking_filelist 批量改造
src_cgi/dealsharefile_cgi.c pv = pv + 1 原子自增
docker/mysql/init.sql       user_file_list 唯一键 + file_info.fingerprint
docker/nginx_fastdfs/nginx.conf   metric 日志格式（可选）
picture_bed/src/services/images.js  merge 重试 + calculateFingerprint + 并发分片（可选）
Makefile                    （新增 deal_dedup.o 编译目标）
```

---

## 7. 附录

### 7.1 Redis key 总览

```
chunk:meta:{md5}   Hash   filename/filesize/chunk_count/user   TTL 24h
chunk:done:{md5}   Set    已完成分片索引（去重）              TTL 24h
chunk:merge:{md5}  Hash   fileid/merged（续合并进度）          TTL 24h
FILE_PUBLIC_ZSET   ZSet   member=md5+filename, score=pv
FILE_NAME_HASH     Hash   fileid→filename（保留兼容，新代码不再依赖）
```

### 7.2 秒传判定流程

```
客户端: 算 fingerprint(头尾16MB) → [大文件可跳过全量MD5]
   │
   ▼
/api/md5 {fingerprint, md5?, size, fileName}
   │
   ├─ file_info.md5 命中 ─────────► dedup_bind_file() → 秒传成功
   ├─ file_info.fingerprint 命中 ─► dedup_bind_file() + 后台全量MD5校验
   └─ 未命中 ─────────────────────► 正常上传 → 落库时写 fingerprint
```

### 7.3 压测环境注意事项

- 压测 `chunk_upload` 前确认 `nginx.conf: client_max_body_size 12m` 与分片大小匹配；
- 压 `/api/sharefiles?cmd=pvdesc` 前先预热：首次请求会触发 MySQL→Redis 全量导入（`sharefiles_cgi.c:473-533`），P95 统计应排除首次；
- 所有 A/B 对比在同一硬件、同一数据量（建议 seed ≥ 500 个共享文件）下进行。
