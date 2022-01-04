/*	EQEMu: Everquest Server Emulator
	Copyright (C) 2001-2005 EQEMu Development Team (http://eqemulator.net)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; version 2 of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY except by those people which sell it, which
	are required to give you total support for your newly bought product;
	without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE. See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "../common/eq_packet_structs.h"
#include "../common/string_util.h"
#include "../common/misc_functions.h"

#include "client.h"
#include "entity.h"
#include "mob.h"

#include "titles.h"
#include "worldserver.h"
#include "../common/repositories/character_titles_repository.h"


extern WorldServer worldserver;

bool TitleManager::LoadTitles()
{
	m_titles = TitlesRepository::All(database);

	return true;
}

EQApplicationPacket *TitleManager::MakeTitlesPacket(Client& client)
{
	// TitleList_Struct
	SerializeBuffer buf;
	buf.WriteInt32(0); // count, updated after serializing

	uint32_t count = 0;
	for (const auto& title : m_titles)
	{
		if (IsClientEligibleForTitle(client, title))
		{
			buf.WriteInt32(title.id);
			buf.WriteString(title.prefix);
			buf.WriteString(title.suffix);
			++count;
		}
	}

	auto outapp = new EQApplicationPacket(OP_SendTitleList, buf);
	outapp->SetWritePosition(0); // go back and write the count
	outapp->WriteUInt32(count);

	return outapp;
}

const TitlesRepository::Titles* TitleManager::GetTitle(int title_id) const
{
	auto it = std::find_if(m_titles.begin(), m_titles.end(),
		[&](const TitlesRepository::Titles& title) { return title.id == title_id; });

	return it != m_titles.end() ? &(*it) : nullptr;
}

bool TitleManager::IsClientEligibleForTitle(Client& c, const TitlesRepository::Titles& title)
{
	// early check for character title if title has no other requirements
	if (!IsTitleRestricted(title))
	{
		if (!c.HasTitleID(title.id))
			return false;

		return true;
	}

	if (title.status >= 0 && c.Admin() < title.status)
		return false;

	if (title.gender >= 0 && c.GetBaseGender() != title.gender)
		return false;

	if (title.class_ >= 0 && c.GetBaseClass() != title.class_)
		return false;

	if (title.min_aa_points >= 0 && c.GetSpentAA() < title.min_aa_points)
		return false;

	if (title.max_aa_points >= 0 && c.GetSpentAA() > title.max_aa_points)
		return false;

	if (title.skill_id >= 0)
	{
		if (title.min_skill_value >= 0 && c.GetRawSkill(static_cast<EQ::skills::SkillType>(title.skill_id)) < static_cast<uint32>(title.max_skill_value))
			return false;

		if (title.max_skill_value >= 0 && c.GetRawSkill(static_cast<EQ::skills::SkillType>(title.skill_id)) > static_cast<uint32>(title.max_skill_value))
			return false;
	}

	if (title.item_id >= 1 && c.GetInv().HasItem(title.item_id, 0, 0xFF) == INVALID_INDEX)
		return false;

	if (title.title_set > 0 && !c.CheckTitle(title.title_set))
		return false;

	return true;
}

bool TitleManager::IsNewAATitleAvailable(int aa_points, int client_class)
{
	for (const auto& title : m_titles)
	{
		if ((title.class_ == -1 || title.class_ == client_class) && title.min_aa_points == aa_points)
		{
			return true;
		}
	}

	return false;
}

bool TitleManager::IsNewTradeSkillTitleAvailable(int skill_id, int skill_value)
{
	for (const auto& title : m_titles)
	{
		if (title.skill_id == skill_id && title.min_skill_value == skill_value)
		{
			return true;
		}
	}

	return false;
}

bool TitleManager::IsTitleRestricted(const TitlesRepository::Titles& title)
{
	// returns true if title has any requirements set
	return title.skill_id != -1 ||
	       title.min_aa_points != -1 ||
	       title.max_aa_points != -1 ||
	       title.class_ != -1 ||
	       title.gender != -1 ||
	       title.status != -1 ||
	       title.item_id > 0 ||
	       title.title_set != 0;
}

void TitleManager::CreateCharacterTitle(Client& client, fmt::string_view title_str, bool is_suffix)
{
	if (!title_str.size() == 0)
	{
		return;
	}

	// if (is_suffix)
	// todo: why was this called client->SetAATitle(prefix.data());
	// todo: why was this called client->SetTitleSuffix(suffix);

	uint32_t title_id = 0;

	// search for an existing title entry where all requirement fields are unset
	auto it = std::find_if(m_titles.begin(), m_titles.end(),
		[&](const TitlesRepository::Titles& title) {
			return !IsTitleRestricted(title) && (is_suffix ? title.suffix == title_str : title.prefix == title_str);
		});

	if (it != m_titles.begin())
	{
		title_id = it->id; // existing title
	}
	else
	{
		auto entry = TitlesRepository::NewEntity();
		entry.prefix = title_str.data();

		auto new_title = TitlesRepository::InsertOne(database, entry);
		if (new_title.id != 0)
		{
			title_id = new_title.id;

			// new title was inserted so all zones need to reload
			auto pack = std::make_unique<ServerPacket>(ServerOP_ReloadTitles, 0);
			worldserver.SendPacket(pack.get());
		}
	}

	// add the title id to the character title table and cache if necessary
	client.AddCharacterTitleID(title_id);
}

void TitleManager::CreateNewPlayerTitle(Client *client, fmt::string_view prefix)
{
	if (!client || prefix.size() == 0)
		return;

	// todo: why was this called client->SetAATitle(prefix.data());
	CreateCharacterTitle(*client, prefix, false);
}

void TitleManager::CreateNewPlayerSuffix(Client *client, fmt::string_view suffix)
{
	if (!client || suffix.size() == 0)
		return;

	// todo: why was this called client->SetTitleSuffix(suffix);
	CreateCharacterTitle(*client, suffix, true);
}

void Client::SetAATitle(const char *Title)
{
	strn0cpy(m_pp.title, Title, sizeof(m_pp.title));

	auto outapp = new EQApplicationPacket(OP_SetTitleReply, sizeof(SetTitleReply_Struct));

	SetTitleReply_Struct *strs = (SetTitleReply_Struct *)outapp->pBuffer;

	strn0cpy(strs->title, Title, sizeof(strs->title));

	strs->entity_id = GetID();

	entity_list.QueueClients(this, outapp, false);

	safe_delete(outapp);
}

void Client::SetTitleSuffix(const char *Suffix)
{
	strn0cpy(m_pp.suffix, Suffix, sizeof(m_pp.suffix));

	auto outapp = new EQApplicationPacket(OP_SetTitleReply, sizeof(SetTitleReply_Struct));

	SetTitleReply_Struct *strs = (SetTitleReply_Struct *)outapp->pBuffer;

	strs->is_suffix = 1;

	strn0cpy(strs->title, Suffix, sizeof(strs->title));

	strs->entity_id = GetID();

	entity_list.QueueClients(this, outapp, false);

	safe_delete(outapp);
}

void Client::EnableTitle(int titleSet) {

	if (CheckTitle(titleSet))
		return;

	std::string query = StringFormat("INSERT INTO player_titlesets "
                                    "(char_id, title_set) VALUES (%i, %i)",
                                    CharacterID(), titleSet);
    auto results = database.QueryDatabase(query);
	if(!results.Success())
		LogError("Error in EnableTitle query for titleset [{}] and charid [{}]", titleSet, CharacterID());

}

bool Client::CheckTitle(int titleSet) {

	std::string query = StringFormat("SELECT `id` FROM player_titlesets "
                                    "WHERE `title_set`=%i AND `char_id`=%i LIMIT 1",
                                    titleSet, CharacterID());
    auto results = database.QueryDatabase(query);
	if (!results.Success()) {
        return false;
	}

	if (results.RowCount() == 0)
        return false;

	return true;
}

void Client::RemoveTitle(int titleSet) {

	if (!CheckTitle(titleSet))
		return;

	std::string query = StringFormat("DELETE FROM player_titlesets "
                                    "WHERE `title_set` = %i AND `char_id` = %i",
                                    titleSet, CharacterID());
   database.QueryDatabase(query);
}

void Client::LoadCharacterTitleIDs()
{
	m_title_ids.clear();

	auto character_titles = CharacterTitlesRepository::GetWhere(database,
		fmt::format("character_id = {}", CharacterID()));

	for (const auto& character_title : character_titles)
	{
		m_title_ids.emplace_back(character_title.title_id);
	}
}

void Client::AddCharacterTitleID(uint32_t title_id)
{
	if (title_id != 0 && !HasTitleID(title_id))
	{
		auto entry = CharacterTitlesRepository::NewEntity();
		entry.character_id = CharacterID();
		entry.title_id = title_id;

		CharacterTitlesRepository::InsertOne(database, entry);

		m_title_ids.emplace_back(title_id);
	}
}
