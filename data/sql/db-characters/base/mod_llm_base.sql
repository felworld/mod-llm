-- mod-llm base schema (characters database). Applied automatically by the
-- worldserver DB updater at startup.

CREATE TABLE IF NOT EXISTS `mod_llm_history_pair` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_guid` INT UNSIGNED NOT NULL,
    `player_guid` INT UNSIGNED NOT NULL,
    `speaker` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `message` TEXT NOT NULL,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    INDEX `idx_pair` (`bot_guid`, `player_guid`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `mod_llm_guild_flavor` (
    `guildid` INT UNSIGNED NOT NULL,
    `flavors` VARCHAR(64) NOT NULL,
    PRIMARY KEY (`guildid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `mod_llm_history_room` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `room_key` VARCHAR(96) NOT NULL,
    `speaker_guid` INT UNSIGNED NOT NULL,
    `speaker_name` VARCHAR(12) NOT NULL,
    `message` TEXT NOT NULL,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    INDEX `idx_room` (`room_key`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
