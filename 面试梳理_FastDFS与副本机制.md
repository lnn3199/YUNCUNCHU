# 面试梳理：FastDFS 架构与副本机制（5 机生产拓扑版）

> **拓扑口径（先背死，被问拓扑先答这个）**：
> - 机器 1：Tracker1（专用，只调度不存文件）
> - 机器 2：Tracker2（专用，与 Tracker1 **对等**，挂一个没事）
> - 机器 3：Storage group1-A
> - 机器 4：Storage group1-B → **group1 两台，文件 2 副本**
> - 机器 5：Nginx + fastdfs-nginx-module（HTTP 统一入口，给前端/APP 访问文件）
>
> 业务层（FastCGI + MySQL + Redis）单独一台或复用机器 5，不影响以下结论。
> 备选口径：3 机对称部署（每台 tracker+storage，3 副本）；开发期 Docker 单机单体。

---

## 0. 30 秒电梯陈述

> "底层文件存储用 FastDFS，5 台机器：两台专用 tracker 对等做调度，group1 两台 storage 存文件、**2 副本**，前面一台 nginx 带 fastdfs 模块统一做 HTTP 入口。上传时客户端问任意一个 tracker，tracker 分配 group 和 storage，数据只写到被指定的那一台，**写成功 1 个副本就返回 file_id 算上传成功——FastDFS 没有 quorum，是单写加异步复制**，另一个副本由源 storage 写 binlog 后台推过去。节点挂掉分两种：被指派去写的那台挂了，这次上传直接失败，tracker 心跳检测到 OFFLINE 后重新分配；另一台挂了不影响写入，它恢复后从组内追 binlog，追之前是 WAIT_SYNC/SYNCING 不接新写，ACTIVE 才重新纳管。tracker 挂一个，客户端切另一个；同步延迟导致的读 404 由 nginx 模块代理到源机兜底。"

关键词：**单写 + 异步复制**、**无 quorum**、**binlog 追同步**、**状态机纳管**。

---

## 1. 拓扑判断：为什么这是小公司生产主流（面试主动讲）

| 设计 | 理由 |
|---|---|
| tracker 独立部署 | tracker 是轻量调度（只存集群元数据），不值得和存储抢磁盘 IO；挂了不影响已存数据 |
| 双 tracker 对等 | 消除调度单点。tracker 之间**不互相同步**——每台 storage 向自己 `tracker_server` 列表里的**所有** tracker 上报心跳和统计，各 tracker 独立维护集群视图，客户端/nginx 模块配两个地址自动故障转移 |
| 双副本 | 小公司性价比最主流：挂一台还有一份可读，写入不中断；比 3 副本省一台机器 |
| nginx + fastdfs-nginx-module 前置 | 统一 HTTP 入口；`response_mode=proxy` 消化"同步延迟 404"（本地没有就代理到 file_id 里源 storage IP 去取） |

**可商榷的 3 点（主动说，显水平）**：
1. 单 nginx 是 HTTP 入口单点 → 严谨做法 keepalived VIP + 双 nginx；
2. 2 副本挂 1 台就**零冗余**，任何一块盘坏就是数据全丢、没有第二次机会 → 预算允许上 3 副本，或至少做冷备（定期同步到对象存储/异地）；
3. 业务层（FastCGI + MySQL + Redis）要单独安排机器，别和存储抢 IO。

---

## 2. 逐题面试答法

### Q4 你们这个存储系统就是 FastDFS 吗？

**不是，这么说就把项目说小了。** FastDFS 只是最底层"存文件体"的一层，提供 upload/append/delete 原语。整个系统是：React 前端 + Nginx（HTTPS、路由、fastdfs-nginx-module）+ 13 个 C FastCGI 业务进程 + MySQL（元数据）+ Redis（缓存/会话/分片索引）+ FastDFS（文件体）+ DashScope/FAISS（AI 检索）。用户体系、秒传、分片续传、合并崩溃恢复、分享、排行榜、AI 语义搜索全是自研。

5 台机器里 4 台都是为 FastDFS 这层服务的，但 FastDFS 自己**不管用户、不管去重（要另配 FastDHT）、不管断点续传**——这些缺口就是业务层的工作量，也是和纯"会用 FastDFS"的人拉开差距的地方。

### Q5 之前分片文件大小是 100MB 一个吗？

**不是，分片是 10MB。100MiB 是测试文件的总大小**：10 片 × 10MiB，测试场景是传到第 6 片（60MiB）中断、续传只补 4 片（40MiB），payload 减少 60%。面试官大概率把简历里"100 MiB 文件"误读成片大了——正好展示你对自己系统的记忆精度，别顺着说"是"。

10MB 的取舍：传输可靠性与请求数的平衡；nginx `client_max_body_size 12m` 给 10MB 分片留 HTTP 头开销；片数上限 10000，服务端强校验 `chunkCount == ceil(size/CHUNK_BYTES)`。拓扑不影响这个设计。

### Q6 你对 FastDFS 整个文件系统了解吗，不止你做过的这块？

分三层说，这题在试探吹没吹牛：

- **客户端 API 层（用得最深）**：`upload_appender_by_filename1` 建追加文件、`append_by_filename1` 逐片追加、`query_file_info1` 查远程长度、`truncate_file1` 回退半片、`delete_file1` 补偿删除——全在分片合并的崩溃恢复里；
- **集群运维层（5 机拓扑）**：
  - `storage.conf` 里 `tracker_server` **要配两条**（两台 tracker），storage 向所有 tracker 上报；`client.conf`、`mod_fastdfs.conf` 同样配两条，故障自动转移——这才是双 tracker 对等的配置关键；
  - binlog **只记文件级操作**（C/c 源创建/副本创建、A/a 源追加/副本追加、D/d 源删除/副本删除），不记内容；每台 storage 有自己的 binlog，**只推"源记录"，副本记录不二次传播**（防环形同步）；2 副本组内同步链路最简单：一对一 push；
  - 新机器入组：INIT → tracker 指定**唯一**全量同步源 + 截止时间戳 → WAIT_SYNC → SYNCING（先全量）→ 取不到更多 binlog 转增量 → ONLINE → 心跳后 **ACTIVE 才接新写入**。组内从 2 台加到 3 台就是走这套流程，在线扩容不停服；
  - tracker 的 `base_path` 持久化 `storage_groups.dat`/`storage_servers.dat`，重启后从磁盘恢复集群视图，所以 tracker 机器也要挂持久盘；
  - 运维：`fdfs_monitor` 看各 storage 状态和同步延迟；挂太久追不齐增量 binlog 的节点，标准动作是摘机、清数据、重新入组全量同步；
- **架构层**：上传四级分配（group → storage → store_path → 两级 256×256 子目录）；file_id 编了源 storage IP、创建时间、大小、CRC32、随机数再 base64；storage 状态机 INIT/WAIT_SYNC/SYNCING/OFFLINE/ONLINE/ACTIVE/DELETED；小文件可 trunk 合并存储（slot 256B~16MB）；去重要开 `check_file_duplicate` 配 FastDHT（我们没开，去重在 MySQL 层做）；
- **边界主动交底**：没改过 FastDFS 源码；集群是课程/测试环境规模，没经过生产流量。

### Q7 FastDFS 是分布式还是单机存储系统？

**分布式，我们就是 5 台机器的标准生产拓扑**：tracker 集群（2 台对等）+ storage group（group1 两台）+ 反向代理层（nginx + module）。扩容路径：容量不够**加 group**；副本/读性能不够**组内加第三台**（走入组全量同步流程，追完 ACTIVE 自动纳管，不停服）。

两个加分点：
1. **系统是分布式的，部署形态看你怎么搭**。开发期 Docker 单机版是"把分布式系统缩成单体"：tracker 单点、单副本零冗余；5 机版把这两个短板都补上了；
2. FastDFS 是**应用级文件系统**，不是 Ceph 那种强一致分布式存储：没有 EC、没有多副本 quorum、组间不互备（Group 是数据隔离边界）、组容量以**最小机器**为准。

### Q8 一个文件存几个副本？

**2 个。** FastDFS 的副本数 = 文件所在 Group 内的 storage server 数量，**没有独立的"副本数"配置参数**——group1 两台，每台各存一份完整数据。相关事实：

- 组内容量以**较小那台**的容量为准，选机器时尽量同规格；
- 写路径代价：每上传 1 份，源 storage 写 binlog 再推给另一台，源机有写放大；读可分散到两台（`download_server=0` 轮询，tracker 选读节点参考同步进度）；
- 组间不互备：group1 的文件不会同步到其他 group。

### Q9 写副本的时候，写成功几个副本才算写入成功？

**1 个。** 2 副本不改变 FastDFS"单写 + 异步复制"的语义：客户端把文件写到 tracker 指定的那一台 storage，落盘返回 file_id，上传即成功；另一个副本由源 storage 写 binlog、后台同步线程推过去。

- **没有 quorum、没有多数派**——这是它和 HDFS（流水线复制，3 副本默认要 ack 策略确认）、Raft（多数派）的本质区别。2 副本下更没有"2 个都写成功才算成功"这回事；
- 代价：返回成功时第二个副本可能还没同步完，存在不一致窗口；读请求被 tracker 分到没同步到的节点会 404——**机器 5 的 nginx + fastdfs-nginx-module（`response_mode=proxy`）就是为这个存在的**：本地没有就代理到 file_id 里源 storage IP 去取，把同步延迟的 404 在接入层消化掉；
- 再严谨一层：单机写入成功也只是进文件系统，默认 `fsync_after_written_bytes=0` 不强制 fsync，掉电仍有窗口；
- 同步进度按**时间戳**记录 → 集群内必须 NTP 对钟。

### Q10 节点异常挂掉，最少写成几个副本就算成功，剩下后台恢复？

**仍然是最少 1 个副本写成功就算成功，剩下 1 个后台恢复。** 分情况：

1. **被指派去写的那台 storage 挂了** → 这次上传**直接失败**，不存在"部分成功"。客户端重试；tracker 靠心跳（storage 每 30s 上报，tracker `check_active_interval=120s` 检测）把它标 OFFLINE，重新分配到另一台 ACTIVE 的 storage。**2 副本组的残酷点：只剩一个选择，另一台也挂就彻底写不进去**；
2. **另一台（副本）挂了** → 不影响写入成功性，源机成功就算成功。挂掉的机器恢复后从源机**追 binlog**（tracker 指定同步源 + 截止时间戳，先全量后增量），追之前状态 WAIT_SYNC→SYNCING，tracker 不派新上传给它，直到 ACTIVE 才重新纳管；
3. **tracker 挂一个** → 调度不受影响：tracker 之间不对等同步、各自独立服务，storage 同时向两台上报，客户端/nginx 模块配了两个 tracker 地址，自动切另一个。两台全挂才影响新上传（已存文件的读取有 nginx 模块按 file_id 内源 IP 代理兜底）；
4. **nginx（机器 5）挂** → HTTP 访问全停，这是当前拓扑唯一明显单点，严谨做法 keepalived VIP + 双 nginx。

**2 副本的硬账（主动说）**：挂 1 台 → 只剩 1 副本，写入仍成功但**零冗余**——任何一块盘坏就是数据全丢，没有第二次机会。这就是 2 副本和 3 副本的本质区别；小公司预算紧可以 2 副本，但要清醒接受这个窗口，或用冷备策略补。

**如果面试官期待"多数派"**：直接纠正——FastDFS 原生做不到，它的写入成功条件恒为 1，剩下 N−1 个后台恢复，是"单写 + 最终一致"。要多数派语义，要么应用层写完主动回读验证其他节点，要么换强一致存储。

**收尾升华**：FastDFS 这层 binlog 追同步（只记文件级操作、按时间戳记进度、全集群对钟）和我在业务层做的分片合并崩溃恢复（journal 记远程 file_id + 远程长度权威游标 + truncate 回退到片边界）是同一种思想——**不信任本地进度，用权威状态恢复**。

---

## 3. 数字速查表（被追问时一个数都不许错）

| 项 | 值 |
|---|---|
| 集群拓扑 | 2 专用 tracker + group1 两 storage + 1 nginx（module） |
| tracker | 对等无状态，彼此不互同步；storage 向所有 tracker 上报 |
| 副本数 | **2**（= 组内 storage 数，无副本参数） |
| 写入成功条件 | **1 个副本**（tracker 指定的那台），无 quorum |
| 副本同步 | binlog 异步复制，只记文件级操作，按时间戳记进度 |
| 心跳 | storage→tracker 每 30s；tracker `check_active_interval=120s` 判活 |
| 入组流程 | INIT→WAIT_SYNC→SYNCING→OFFLINE→ONLINE→ACTIVE（ACTIVE 才接写） |
| 同步延迟容忍 | `storage_sync_file_max_delay` 默认 86400s，超了可能追不齐增量，需重新入组全量同步 |
| 预留空间 | `reserved_storage_space = 10%` |
| 选组/选机策略 | `store_lookup=2`（最大剩余空间组）、`store_server=0`（组内轮询）、`download_server=0`（读轮询） |
| HTTP 接入 | nginx + fastdfs-nginx-module，`response_mode=proxy`，`url_have_group_name=true` |
| 本项目分片 | 10MB/片，>10MB 启用，上限 10000 片 |
| 续传实测 | 100MiB 断在 60MiB，只补 40MiB，payload −60%（内容字节口径，非耗时） |

## 4. 已知边界 / 诚实清单（主动说，加分）

1. 没改过 FastDFS 源码，集群是课程/测试规模，无生产流量；
2. `fsync_after_written_bytes=0` 默认不强制 fsync，单副本写入成功 ≠ 断电不丢；
3. 2 副本是最终一致、非多数派：写节点挂掉的瞬间正在写的文件就是失败；挂 1 台即零冗余，靠冷备/监控补；
4. 挂太久的节点可能追不齐增量 binlog，要摘机重新入组全量同步（期间占带宽/IO，挑低谷操作）；
5. 单 nginx 是 HTTP 入口单点，生产应 keepalived 双机；
6. 业务层若也上多台 FastCGI，`/tmp/chunks` 分片目录在本地盘，需按 uploadId 会话路由或共享文件系统——这是分片续传的已知限制；
7. 集群时钟必须 NTP 对钟，binlog 同步进度按时间戳记录。
