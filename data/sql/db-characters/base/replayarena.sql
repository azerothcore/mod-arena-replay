CREATE TABLE IF NOT EXISTS `character_arena_replays` (
  `id` int NOT NULL AUTO_INCREMENT,
  `arenaTypeId` int NULL DEFAULT NULL,
  `typeId` int NULL DEFAULT NULL,
  `contentSize` int NULL DEFAULT NULL,
  `contents` longblob NULL,
  `mapId` int NULL DEFAULT NULL,
  `timestamp` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`) USING BTREE
) ENGINE = InnoDB CHARACTER SET = utf8mb4 COLLATE = utf8mb4_unicode_ci ROW_FORMAT = DYNAMIC;