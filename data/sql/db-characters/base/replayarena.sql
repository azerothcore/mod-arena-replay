CREATE TABLE IF NOT EXISTS `character_arena_replays` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `arenaTypeId` INT NULL DEFAULT NULL,
  `typeId` INT NULL DEFAULT NULL,
  `contentSize` INT NULL DEFAULT NULL,
  `contents` LONGTEXT NULL,
  `mapId` INT NULL DEFAULT NULL,
  `timestamp` TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`) USING BTREE
) ENGINE = InnoDB CHARACTER SET = utf8mb4 COLLATE = utf8mb4_unicode_ci ROW_FORMAT = DYNAMIC;
