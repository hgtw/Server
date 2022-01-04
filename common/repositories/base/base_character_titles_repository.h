/**
 * DO NOT MODIFY THIS FILE
 *
 * This repository was automatically generated and is NOT to be modified directly.
 * Any repository modifications are meant to be made to the repository extending the base.
 * Any modifications to base repositories are to be made by the generator only
 *
 * @generator ./utils/scripts/generators/repository-generator.pl
 * @docs https://eqemu.gitbook.io/server/in-development/developer-area/repositories
 */

#ifndef EQEMU_BASE_CHARACTER_TITLES_REPOSITORY_H
#define EQEMU_BASE_CHARACTER_TITLES_REPOSITORY_H

#include "../../database.h"
#include "../../string_util.h"
#include <ctime>

class BaseCharacterTitlesRepository {
public:
	struct CharacterTitles {
		int id;
		int character_id;
		int title_id;
	};

	static std::string PrimaryKey()
	{
		return std::string("id");
	}

	static std::vector<std::string> Columns()
	{
		return {
			"id",
			"character_id",
			"title_id",
		};
	}

	static std::vector<std::string> SelectColumns()
	{
		return {
			"id",
			"character_id",
			"title_id",
		};
	}

	static std::string ColumnsRaw()
	{
		return std::string(implode(", ", Columns()));
	}

	static std::string SelectColumnsRaw()
	{
		return std::string(implode(", ", SelectColumns()));
	}

	static std::string TableName()
	{
		return std::string("character_titles");
	}

	static std::string BaseSelect()
	{
		return fmt::format(
			"SELECT {} FROM {}",
			SelectColumnsRaw(),
			TableName()
		);
	}

	static std::string BaseInsert()
	{
		return fmt::format(
			"INSERT INTO {} ({}) ",
			TableName(),
			ColumnsRaw()
		);
	}

	static CharacterTitles NewEntity()
	{
		CharacterTitles entry{};

		entry.id           = 0;
		entry.character_id = 0;
		entry.title_id     = 0;

		return entry;
	}

	static CharacterTitles GetCharacterTitlesEntry(
		const std::vector<CharacterTitles> &character_titless,
		int character_titles_id
	)
	{
		for (auto &character_titles : character_titless) {
			if (character_titles.id == character_titles_id) {
				return character_titles;
			}
		}

		return NewEntity();
	}

	static CharacterTitles FindOne(
		Database& db,
		int character_titles_id
	)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"{} WHERE id = {} LIMIT 1",
				BaseSelect(),
				character_titles_id
			)
		);

		auto row = results.begin();
		if (results.RowCount() == 1) {
			CharacterTitles entry{};

			entry.id           = atoi(row[0]);
			entry.character_id = atoi(row[1]);
			entry.title_id     = atoi(row[2]);

			return entry;
		}

		return NewEntity();
	}

	static int DeleteOne(
		Database& db,
		int character_titles_id
	)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"DELETE FROM {} WHERE {} = {}",
				TableName(),
				PrimaryKey(),
				character_titles_id
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static int UpdateOne(
		Database& db,
		CharacterTitles character_titles_entry
	)
	{
		std::vector<std::string> update_values;

		auto columns = Columns();

		update_values.push_back(columns[1] + " = " + std::to_string(character_titles_entry.character_id));
		update_values.push_back(columns[2] + " = " + std::to_string(character_titles_entry.title_id));

		auto results = db.QueryDatabase(
			fmt::format(
				"UPDATE {} SET {} WHERE {} = {}",
				TableName(),
				implode(", ", update_values),
				PrimaryKey(),
				character_titles_entry.id
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static CharacterTitles InsertOne(
		Database& db,
		CharacterTitles character_titles_entry
	)
	{
		std::vector<std::string> insert_values;

		insert_values.push_back(std::to_string(character_titles_entry.id));
		insert_values.push_back(std::to_string(character_titles_entry.character_id));
		insert_values.push_back(std::to_string(character_titles_entry.title_id));

		auto results = db.QueryDatabase(
			fmt::format(
				"{} VALUES ({})",
				BaseInsert(),
				implode(",", insert_values)
			)
		);

		if (results.Success()) {
			character_titles_entry.id = results.LastInsertedID();
			return character_titles_entry;
		}

		character_titles_entry = NewEntity();

		return character_titles_entry;
	}

	static int InsertMany(
		Database& db,
		std::vector<CharacterTitles> character_titles_entries
	)
	{
		std::vector<std::string> insert_chunks;

		for (auto &character_titles_entry: character_titles_entries) {
			std::vector<std::string> insert_values;

			insert_values.push_back(std::to_string(character_titles_entry.id));
			insert_values.push_back(std::to_string(character_titles_entry.character_id));
			insert_values.push_back(std::to_string(character_titles_entry.title_id));

			insert_chunks.push_back("(" + implode(",", insert_values) + ")");
		}

		std::vector<std::string> insert_values;

		auto results = db.QueryDatabase(
			fmt::format(
				"{} VALUES {}",
				BaseInsert(),
				implode(",", insert_chunks)
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static std::vector<CharacterTitles> All(Database& db)
	{
		std::vector<CharacterTitles> all_entries;

		auto results = db.QueryDatabase(
			fmt::format(
				"{}",
				BaseSelect()
			)
		);

		all_entries.reserve(results.RowCount());

		for (auto row = results.begin(); row != results.end(); ++row) {
			CharacterTitles entry{};

			entry.id           = atoi(row[0]);
			entry.character_id = atoi(row[1]);
			entry.title_id     = atoi(row[2]);

			all_entries.push_back(entry);
		}

		return all_entries;
	}

	static std::vector<CharacterTitles> GetWhere(Database& db, std::string where_filter)
	{
		std::vector<CharacterTitles> all_entries;

		auto results = db.QueryDatabase(
			fmt::format(
				"{} WHERE {}",
				BaseSelect(),
				where_filter
			)
		);

		all_entries.reserve(results.RowCount());

		for (auto row = results.begin(); row != results.end(); ++row) {
			CharacterTitles entry{};

			entry.id           = atoi(row[0]);
			entry.character_id = atoi(row[1]);
			entry.title_id     = atoi(row[2]);

			all_entries.push_back(entry);
		}

		return all_entries;
	}

	static int DeleteWhere(Database& db, std::string where_filter)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"DELETE FROM {} WHERE {}",
				TableName(),
				where_filter
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static int Truncate(Database& db)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"TRUNCATE TABLE {}",
				TableName()
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

};

#endif //EQEMU_BASE_CHARACTER_TITLES_REPOSITORY_H
