-- backup the main table since this is going to be destructive, can delete later in a followup
CREATE TABLE `titles_backup` LIKE `titles`;
INSERT INTO `titles_backup` SELECT * FROM `titles`;

-- copy character specific titles to a temporary table
CREATE TABLE `titles_temporary` LIKE `titles`;
INSERT INTO `titles_temporary` SELECT * FROM `titles` where char_id != -1;

-- drop all character titles from existing titles table and remove the char_id field
DELETE FROM `titles` where char_id != -1;
ALTER TABLE `titles` DROP COLUMN `char_id`;

-- create new title entries for distinct character titles that don't already exist
-- source only inserts prefix/suffix fields for character titles so no need to select on other fields
-- we do insert the new fields intended for character titles with all other requirement fields unset
INSERT INTO `titles` (`prefix`, `suffix`)
SELECT DISTINCT `prefix`, `suffix`
FROM `titles_temporary` AS `character_titles`
WHERE
  (`prefix` != '' OR `suffix` != '')
  AND NOT EXISTS(
     SELECT `prefix`,`suffix` FROM `titles`
     WHERE
       `prefix` = `character_titles`.`prefix`
       AND `suffix` = `character_titles`.`suffix`
       AND `skill_id` = -1
       AND `min_skill_value` = -1
       AND `max_skill_value` = -1
       AND `min_aa_points` = -1
       AND `max_aa_points` = -1
       AND `class` = -1
       AND `gender` = -1
       AND `status` = -1
       AND `item_id` = -1
       AND `title_set` = 0
   );

-- link character titles to new title entries
CREATE TABLE `character_titles` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT,
  `character_id` int(11) unsigned NOT NULL,
  `title_id` int(11) unsigned NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `character_id_title_id` (`character_id`,`title_id`)
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

-- select the new titles that that have all other requirements unset
INSERT INTO `character_titles` (`character_id`, `title_id`)
SELECT `titles_temporary`.`char_id`, `titles`.`id`
FROM `titles_temporary`
INNER JOIN `titles`
  ON (`titles_temporary`.`prefix` != '' AND `titles_temporary`.`prefix` = `titles`.`prefix`)
  OR (`titles_temporary`.`suffix` != '' AND `titles_temporary`.`suffix` = `titles`.`suffix`)
WHERE `titles`.`skill_id` = -1
  AND `titles`.`min_skill_value` = -1
  AND `titles`.`max_skill_value` = -1
  AND `titles`.`min_aa_points` = -1
  AND `titles`.`max_aa_points` = -1
  AND `titles`.`class` = -1
  AND `titles`.`gender` = -1
  AND `titles`.`status` = -1
  AND `titles`.`item_id` = -1
  AND `titles`.`title_set` = 0;

-- drop temp table
DROP TABLE `titles_temporary`;
