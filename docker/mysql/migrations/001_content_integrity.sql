-- Existing installations: back up first and stop ALL CGI writers for migration.
-- Run once against the configured database. Never use init.sql on existing data.
-- Preflight aborts instead of truncating invalid fingerprints/filenames.
DELIMITER //
CREATE PROCEDURE migrate_content_integrity()
BEGIN
  IF EXISTS(SELECT 1 FROM file_info WHERE BINARY md5 NOT REGEXP '^[0-9a-f]{32}$')
     OR EXISTS(SELECT 1 FROM user_file_list WHERE BINARY md5 NOT REGEXP '^[0-9a-f]{32}$' OR file_name IS NULL OR file_name='' OR user='')
     OR EXISTS(SELECT 1 FROM share_file_list WHERE BINARY md5 NOT REGEXP '^[0-9a-f]{32}$' OR file_name IS NULL OR file_name='')
     OR EXISTS(SELECT 1 FROM user_file_list u LEFT JOIN file_info f ON f.md5=u.md5 WHERE f.id IS NULL)
     OR EXISTS(SELECT 1 FROM share_file_list s LEFT JOIN file_info f ON f.md5=s.md5 WHERE f.id IS NULL)
  THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Invalid legacy identity or orphan association; repair before migrating';
  END IF;
  -- Preserve the strongest sharing/PV state while consolidating exact duplicates.
  CREATE TEMPORARY TABLE association_keep AS
    SELECT MIN(id) AS keep_id,user,md5,BINARY file_name AS name,
           MAX(COALESCE(shared_status,0)) AS shared,MAX(COALESCE(pv,0)) AS visits
    FROM user_file_list GROUP BY user,md5,BINARY file_name;
  UPDATE user_file_list u JOIN association_keep k ON u.id=k.keep_id
    SET u.shared_status=k.shared,u.pv=k.visits;
  DELETE u FROM user_file_list u JOIN association_keep k
    ON u.user=k.user AND u.md5=k.md5 AND BINARY u.file_name=k.name WHERE u.id<>k.keep_id;
  DROP TEMPORARY TABLE association_keep;
  ALTER TABLE file_info MODIFY md5 CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL;
  ALTER TABLE user_file_list
    MODIFY md5 CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    MODIFY file_name VARCHAR(128) CHARACTER SET utf8 COLLATE utf8_bin NOT NULL,
    ADD UNIQUE KEY uq_user_content_name(user,md5,file_name);
  ALTER TABLE share_file_list
    MODIFY md5 CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    MODIFY file_name VARCHAR(128) CHARACTER SET utf8 COLLATE utf8_bin NOT NULL,
    ADD KEY idx_share_content_name(md5,file_name);
  UPDATE file_info f LEFT JOIN (SELECT md5,COUNT(*) AS n FROM user_file_list GROUP BY md5) x ON x.md5=f.md5
    SET f.count=COALESCE(x.n,0);
  UPDATE user_file_count c SET c.count=0 WHERE c.user<>'FILE_PUBLIC_COUNT';
  INSERT INTO user_file_count(user,count) SELECT user,COUNT(*) FROM user_file_list GROUP BY user
    ON DUPLICATE KEY UPDATE count=VALUES(count);
  INSERT INTO user_file_count(user,count) SELECT 'FILE_PUBLIC_COUNT',COUNT(*) FROM share_file_list
    ON DUPLICATE KEY UPDATE count=VALUES(count);
END//
DELIMITER ;
CALL migrate_content_integrity();
DROP PROCEDURE migrate_content_integrity;

CREATE TABLE IF NOT EXISTS storage_gc (
  file_id VARCHAR(256) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  attempts INT NOT NULL DEFAULT 0,
  last_error INT NOT NULL DEFAULT 0
) ENGINE=InnoDB;
