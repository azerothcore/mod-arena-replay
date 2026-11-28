ALTER TABLE `character_arena_replays` ADD COLUMN `saveToken` BIGINT UNSIGNED NULL DEFAULT NULL,
  ADD UNIQUE INDEX `idx_saveToken` (`saveToken`);
