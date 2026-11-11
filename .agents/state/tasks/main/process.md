# Current Task
- Task id main. Latest request (2026-10-23): explain cache rebuilding, PV dual writes, ZINCRBY reply bug and benchmark methodology much more slowly with actual code and concrete examples; user found previous explanation too coarse. Earlier STAR resume work remains completed context.
# Done
- 2026-10-23: rechecked live cache check/rebuild and missing-detail paths, PV writer, frontend error handling, ZINCRBY wrapper, benchmark timing/general-log/warmup code. Prepared concrete numerical examples for source_count limitations, transaction publishing, rebuild overwriting newer PV, failure between DB/Redis, and RESP2 score misclassification. No application-code changes or remote benchmark run.
- 2026-10-22 resumed: re-read live ranking/baseline C code, frontend request/display pagination, PV writer, schema and benchmark script; prepared a source-linked business explanation for the user's correction to use their actual code. Existing detailed guide remains the extended reference.
- 2026-10-22: added 面试梳理_热门文件排行榜.md with frontend-to-CGI flow, baseline comparison, WITHSCORES parsing, UNION ALL + pos batch detail SQL, cache rebuild limits, PV updates, metrics methodology and interview answers.
- Reviewed chunk upload/init/merge, content_store, ranking code, and reports/optimization evidence.
- Confirmed most core claims are evidence-backed, but identified wording issues: “共享计算的完整文件 MD5” is incorrect/unclear; 100 MiB case is an interrupted transfer and re-init, not arbitrary crash recovery; “详情 SQL 50→1” is accurate only for detail lookup and not total endpoint SQL; GC queue is transactionally recorded, while physical deletion is a separate worker and cross-system atomicity is not guaranteed.
# Key Files
- 面试梳理_热门文件排行榜.md; tests/baseline/sharefiles_cgi.c; picture_bed/src/pages/TopDownloads.js; picture_bed/src/services/share.js; common/redis_op.c:1284.
- src_cgi/chunk_upload_cgi.c, src_cgi/chunk_init_cgi.c, src_cgi/chunk_merge_cgi.c
- common/chunk_store.c, common/content_store.c, src_cgi/sharefiles_cgi.c
- reports/optimization/README.md, reports/optimization/resume.md
# Verification
- Resume verification: recomputed P95 from final-measurements.json raw arrays (1,000 samples each, zero reported errors): 34.947117 -> 6.916366 ms, 80.209051% decrease. Confirmed frontend count=50 and pageSize=10; no business-code edits or remote rerun.
- Recomputed P95 from both 1000-sample arrays in reports/optimization/final-measurements.json: 34.947117 -> 6.916366 ms, 80.209051% reduction; reviewed three repeats and unresolved initial regression. Code inspection only; no remote benchmark rerun or application code edits this turn.
- Code and report cross-check completed; metrics 60 MiB/40 MiB, 90% dedup, 50→1 detail SQL, P95 34.95→6.92 ms match stored evidence with stated scope.
- Produced a STAR-oriented, copy-ready three-bullet version: problem/goal -> implementation -> measured result.
# Current Constraints
- source_count stores the tracked DB shared-row count at rebuild, not PV, a timestamp or content version; it cannot detect a pure score mismatch. No guaranteed convergence exists for a persistent score mismatch without another invalidation/rebuild. Benchmark measures ranking reads on static fixtures and does not validate concurrent PV/rebuild consistency.
- Old code already used ZSet. 50 -> 1 is detail SQL only; frontend requests 50 then displays 10/page. Actual SQL uses UNION ALL with pos and MIN(id) representative row, not plain IN. PV records download initiation; dual writes lack full compensation, and source_count is heuristic.
- Identified but not fixed: common/redis_op.c:1284 ZINCRBY wrapper compares returned score string to "OK" and dereferences reply without a NULL check; actual increment can succeed but be reported as failure. Cache missing-member initialization and reconstruction can race with updates. Explain these as current limitations.
- Do not overclaim production deployment, arbitrary crash guarantees, full cross-service atomicity, or all-interface SQL count.
# Next Step
- Deliver detailed Chinese teaching answer using one A.pdf / PV 100 example, actual code links and step tables; distinguish existing implementation from possible improvements. Explain SQL count vs latency, 1,000 records vs requests vs concurrency=4, warmup and P95. Preserve unrelated files.
