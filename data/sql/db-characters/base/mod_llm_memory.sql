-- Per-bot memory scratchpad: short notes the model writes and reads itself
-- (remember/forget tools). Replaces the numeric sentiment table - notes like
-- "ninja'd my loot in deadmines" carry more than a 0..1 float, and the model
-- can explain them. subject_guid scopes a note to one player (0 = general
-- note: goals, plans, world facts); prompts inject notes about the current
-- actor plus recent general notes.

DROP TABLE IF EXISTS `mod_llm_sentiment`;

CREATE TABLE IF NOT EXISTS `mod_llm_memory` (
    `bot_guid` INT UNSIGNED NOT NULL,
    `slug` VARCHAR(48) NOT NULL,
    `subject_guid` INT UNSIGNED NOT NULL DEFAULT 0,
    `content` VARCHAR(300) NOT NULL,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`bot_guid`, `slug`),
    INDEX `idx_subject` (`bot_guid`, `subject_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
