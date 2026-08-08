-- mod-llm exchange trace (characters database). Applied automatically by the
-- worldserver DB updater at startup.
--
-- One row per LLM request: the exact body POSTed to the endpoint, the raw
-- response, and the say-text extracted from it. Always on, so when a bot says
-- something odd in playtesting the prompt that produced it is already
-- captured. Purged at startup by LLM.Trace.RetentionDays.

CREATE TABLE IF NOT EXISTS `mod_llm_trace` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `time` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `bot_guid` INT UNSIGNED NOT NULL DEFAULT 0,
    `kind` VARCHAR(16) NOT NULL,
    `trigger_kind` VARCHAR(16) NOT NULL,
    `chain_depth` INT UNSIGNED NOT NULL DEFAULT 0,
    `round` INT UNSIGNED NOT NULL DEFAULT 0,
    `status` VARCHAR(16) NOT NULL,
    `latency_ms` INT UNSIGNED NOT NULL DEFAULT 0,
    `request` MEDIUMTEXT NOT NULL,
    `response` MEDIUMTEXT NOT NULL,
    `said` TEXT NOT NULL,
    PRIMARY KEY (`id`),
    INDEX `idx_trace_bot` (`bot_guid`, `id`),
    INDEX `idx_trace_time` (`time`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
