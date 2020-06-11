/**
 * EQEmulator: Everquest Server Emulator
 * Copyright (C) 2001-2020 EQEmulator Development Team (https://github.com/EQEmu/Server)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY except by those people which sell it, which
 * are required to give you total support for your newly bought product;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include "expedition.h"
#include "expedition_database.h"
#include "expedition_lockout_timer.h"
#include "expedition_request.h"
#include "client.h"
#include "groups.h"
#include "raids.h"
#include "string_ids.h"
#include "worldserver.h"
#include "zonedb.h"
#include "../common/eqemu_logsys.h"
#include "../common/util/uuid.h"

extern WorldServer worldserver;
extern Zone* zone;

// message string 8271 (not in emu clients)
const char* const DZ_YOU_NOT_ASSIGNED        = "You could not use this command because you are not currently assigned to a dynamic zone.";
// message string 9265 (not in emu clients)
const char* const EXPEDITION_OTHER_BELONGS   = "{} attempted to create an expedition but {} already belongs to one.";
// lockout warnings were added to live in March 11 2020 patch
const char* const DZADD_INVITE_WARNING       = "Warning! You will be given replay timers for the following events if you enter %s:";
const char* const DZADD_INVITE_WARNING_TIMER = "%s - %sD:%sH:%sM";
const char* const KICKPLAYERS_EVERYONE       = "Everyone";

const uint32_t Expedition::REPLAY_TIMER_ID = std::numeric_limits<uint32_t>::max();
const uint32_t Expedition::EVENT_TIMER_ID  = 1;

Expedition::Expedition(
	uint32_t id, const std::string& uuid, const DynamicZone& dynamic_zone, std::string expedition_name,
	const ExpeditionMember& leader, uint32_t min_players, uint32_t max_players
) :
	m_id(id),
	m_uuid(uuid),
	m_dynamiczone(dynamic_zone),
	m_expedition_name(expedition_name),
	m_leader(leader),
	m_min_players(min_players),
	m_max_players(max_players)
{
}

Expedition* Expedition::TryCreate(
	Client* requester, DynamicZone& dynamiczone, ExpeditionRequest& request)
{
	if (!requester || !zone)
	{
		return nullptr;
	}

	// request parses leader, members list, and lockouts while validating
	if (!request.Validate(requester))
	{
		LogExpeditionsModerate(
			"Creation of [{}] by [{}] denied", request.GetExpeditionName(), requester->GetName()
		);
		return nullptr;
	}

	if (dynamiczone.GetInstanceID() == 0)
	{
		dynamiczone.CreateInstance();
	}

	if (dynamiczone.GetInstanceID() == 0)
	{
		// live uses this message when trying to enter an instance that isn't ready
		// we can use it as the client error message if instance creation fails
		requester->MessageString(Chat::Red, DZ_PREVENT_ENTERING);
		LogExpeditions("Failed to create a dynamic zone instance for expedition");
		return nullptr;
	}

	std::string expedition_uuid = EQ::Util::UUID::Generate().ToString();

	// unique expedition ids are created from database via auto-increment column
	auto expedition_id = ExpeditionDatabase::InsertExpedition(
		expedition_uuid,
		dynamiczone.GetInstanceID(),
		request.GetExpeditionName(),
		request.GetLeaderID(),
		request.GetMinPlayers(),
		request.GetMaxPlayers()
	);

	if (expedition_id)
	{
		dynamiczone.SaveToDatabase();

		ExpeditionMember leader{request.GetLeaderID(), request.GetLeaderName()};

		auto expedition = std::unique_ptr<Expedition>(new Expedition(
			expedition_id,
			expedition_uuid,
			dynamiczone,
			request.GetExpeditionName(),
			leader,
			request.GetMinPlayers(),
			request.GetMaxPlayers()
		));

		LogExpeditions(
			"Created [{}] ({}) instance id: [{}] leader: [{}] minplayers: [{}] maxplayers: [{}]",
			expedition->GetID(),
			expedition->GetName(),
			expedition->GetInstanceID(),
			expedition->GetLeaderName(),
			expedition->GetMinPlayers(),
			expedition->GetMaxPlayers()
		);

		expedition->SaveMembers(request);
		expedition->SaveLockouts(request);

		auto inserted = zone->expedition_cache.emplace(expedition_id, std::move(expedition));

		inserted.first->second->SendUpdatesToZoneMembers();
		inserted.first->second->SendWorldExpeditionUpdate(ServerOP_ExpeditionCreate); // cache in other zones

		Client* leader_client = request.GetLeaderClient();

		Client::SendCrossZoneMessageString(
			leader_client, leader.name, Chat::Yellow, EXPEDITION_AVAILABLE, { request.GetExpeditionName() }
		);

		return inserted.first->second.get();
	}

	return nullptr;
}

void Expedition::CacheExpeditions(MySQLRequestResult& results)
{
	if (!results.Success() || !zone)
	{
		return;
	}

	std::vector<uint32_t> expedition_ids;
	std::vector<uint32_t> instance_ids;
	std::vector<std::pair<uint32_t, uint32_t>> expedition_character_ids;

	using col = LoadExpeditionColumns::eLoadExpeditionColumns;

	Expedition* current_expedition = nullptr;
	uint32_t last_expedition_id = 0;

	for (auto row = results.begin(); row != results.end(); ++row)
	{
		auto expedition_id = strtoul(row[col::id], nullptr, 10);

		if (expedition_id != last_expedition_id)
		{
			// finished parsing previous expedition members, send member updates
			if (current_expedition)
			{
				current_expedition->SendUpdatesToZoneMembers();
			}

			expedition_ids.emplace_back(expedition_id);

			uint32_t leader_id = strtoul(row[col::leader_id], nullptr, 10);
			uint32_t instance_id = row[col::instance_id] ? strtoul(row[col::instance_id], nullptr, 10) : 0;
			if (instance_id) // can be null from fk constraint
			{
				instance_ids.emplace_back(instance_id);
			}

			std::unique_ptr<Expedition> expedition = std::unique_ptr<Expedition>(new Expedition(
				expedition_id,
				row[col::uuid],                                         // expedition uuid
				DynamicZone{instance_id},
				row[col::expedition_name],                              // expedition name
				ExpeditionMember{leader_id, row[col::leader_name]},     // expedition leader id, name
				strtoul(row[col::min_players], nullptr, 10),            // min_players
				strtoul(row[col::max_players], nullptr, 10)             // max_players
			));

			bool add_replay_on_join = (strtoul(row[col::add_replay_on_join], nullptr, 10) != 0);
			bool is_locked = (strtoul(row[col::is_locked], nullptr, 10) != 0);

			expedition->SetReplayLockoutOnMemberJoin(add_replay_on_join);
			expedition->SetLocked(is_locked);

			zone->expedition_cache.emplace(expedition_id, std::move(expedition));
		}

		last_expedition_id = expedition_id;

		// looping expedition members
		current_expedition = Expedition::FindCachedExpeditionByID(last_expedition_id);
		if (current_expedition)
		{
			auto member_id = strtoul(row[col::member_id], nullptr, 10);
			bool is_current_member = (strtoul(row[col::is_current_member], nullptr, 10) != 0);
			current_expedition->AddInternalMember(
				row[col::member_name], member_id, ExpeditionMemberStatus::Offline, is_current_member
			);
			expedition_character_ids.emplace_back(std::make_pair(expedition_id, member_id));
		}
	}

	// update for the last cached expedition
	if (current_expedition)
	{
		current_expedition->SendUpdatesToZoneMembers();
	}

	// ask world for online members from all cached expeditions at once
	Expedition::SendWorldGetOnlineMembers(expedition_character_ids);

	// bulk load dynamic zone data and expedition lockouts for cached expeditions
	auto dynamic_zones = DynamicZone::LoadMultipleDzFromDatabase(instance_ids);
	auto expedition_lockouts = ExpeditionDatabase::LoadMultipleExpeditionLockouts(expedition_ids);

	for (const auto& expedition_id : expedition_ids)
	{
		auto expedition = Expedition::FindCachedExpeditionByID(expedition_id);
		if (expedition)
		{
			auto dz_iter = dynamic_zones.find(expedition->GetInstanceID());
			if (dz_iter != dynamic_zones.end())
			{
				expedition->m_dynamiczone = dz_iter->second;
			}

			auto lockout_iter = expedition_lockouts.find(expedition->GetID());
			if (lockout_iter != expedition_lockouts.end())
			{
				expedition->m_lockouts = lockout_iter->second;
			}
		}
	}
}

void Expedition::CacheFromDatabase(uint32_t expedition_id)
{
	if (zone)
	{
		BenchTimer benchmark;

		auto results = ExpeditionDatabase::LoadExpedition(expedition_id);
		if (!results.Success())
		{
			LogExpeditions("Failed to load Expedition [{}] for zone cache", expedition_id);
			return;
		}

		CacheExpeditions(results);

		auto elapsed = benchmark.elapsed();
		LogExpeditions("Caching new expedition [{}] took {}s", expedition_id, elapsed);
	}
}

bool Expedition::CacheAllFromDatabase()
{
	if (!zone)
	{
		return false;
	}

	BenchTimer benchmark;

	zone->expedition_cache.clear();

	// load all active expeditions and members to current zone cache
	auto results = ExpeditionDatabase::LoadAllExpeditions();
	if (!results.Success())
	{
		LogExpeditions("Failed to load Expeditions for zone cache");
		return false;
	}

	CacheExpeditions(results);

	auto elapsed = benchmark.elapsed();
	LogExpeditions("Caching [{}] expedition(s) took {}s", zone->expedition_cache.size(), elapsed);

	return true;
}

void Expedition::SaveLockouts(ExpeditionRequest& request)
{
	m_lockouts = request.GetLockouts();
	ExpeditionDatabase::InsertLockouts(m_id, m_lockouts);
}

void Expedition::SaveMembers(ExpeditionRequest& request)
{
	m_members = request.GetMembers();
	for (const auto& member : m_members)
	{
		m_member_id_history.emplace(member.char_id);
	}

	ExpeditionDatabase::InsertMembers(m_id, m_members);
	ExpeditionDatabase::DeleteAllMembersPendingLockouts(m_members);
	m_dynamiczone.SaveInstanceMembersToDatabase(m_member_id_history); // all are current members here
}

Expedition* Expedition::FindCachedExpeditionByCharacterID(uint32_t character_id)
{
	if (zone)
	{
		for (const auto& expedition : zone->expedition_cache)
		{
			if (expedition.second->HasMember(character_id))
			{
				return expedition.second.get();
			}
		}
	}
	return nullptr;
}

Expedition* Expedition::FindCachedExpeditionByCharacterName(const std::string& char_name)
{
	if (zone)
	{
		for (const auto& expedition : zone->expedition_cache)
		{
			if (expedition.second->HasMember(char_name))
			{
				return expedition.second.get();
			}
		}
	}
	return nullptr;
}

Expedition* Expedition::FindCachedExpeditionByID(uint32_t expedition_id)
{
	if (zone && expedition_id)
	{
		auto expedition_cache_iter = zone->expedition_cache.find(expedition_id);
		if (expedition_cache_iter != zone->expedition_cache.end())
		{
			return expedition_cache_iter->second.get();
		}
	}
	return nullptr;
}

Expedition* Expedition::FindExpeditionByInstanceID(uint32_t instance_id)
{
	if (instance_id)
	{
		// ask database since it may have expired
		auto expedition_id = ExpeditionDatabase::GetExpeditionIDFromInstanceID(instance_id);
		return Expedition::FindCachedExpeditionByID(expedition_id);
	}
	return nullptr;
}

bool Expedition::HasLockout(const std::string& event_name)
{
	return (m_lockouts.find(event_name) != m_lockouts.end());
}

bool Expedition::HasReplayLockout()
{
	return HasLockout(DZ_REPLAY_TIMER_NAME);
}

bool Expedition::HasMember(uint32_t character_id)
{
	return std::any_of(m_members.begin(), m_members.end(), [&](const ExpeditionMember& member) {
		return member.char_id == character_id;
	});
}

bool Expedition::HasMember(const std::string& character_name)
{
	return std::any_of(m_members.begin(), m_members.end(), [&](const ExpeditionMember& member) {
		return (strcasecmp(member.name.c_str(), character_name.c_str()) == 0);
	});
}

ExpeditionMember Expedition::GetMemberData(uint32_t character_id)
{
	auto it = std::find_if(m_members.begin(), m_members.end(), [&](const ExpeditionMember& member) {
		return member.char_id == character_id;
	});

	ExpeditionMember member_data;
	if (it != m_members.end())
	{
		member_data = *it;
	}
	return member_data;
}

ExpeditionMember Expedition::GetMemberData(const std::string& character_name)
{
	auto it = std::find_if(m_members.begin(), m_members.end(), [&](const ExpeditionMember& member) {
		return (strcasecmp(member.name.c_str(), character_name.c_str()) == 0);
	});

	ExpeditionMember member_data;
	if (it != m_members.end())
	{
		member_data = *it;
	}
	return member_data;
}

void Expedition::SetReplayLockoutOnMemberJoin(bool add_on_join, bool update_db)
{
	m_add_replay_on_join = add_on_join;

	if (update_db)
	{
		ExpeditionDatabase::UpdateReplayLockoutOnJoin(m_id, add_on_join);
		SendWorldSettingChanged(ServerOP_ExpeditionReplayOnJoin, m_add_replay_on_join);
	}
}

void Expedition::AddReplayLockout(uint32_t seconds)
{
	AddLockout(DZ_REPLAY_TIMER_NAME, seconds);
}

void Expedition::AddLockout(const std::string& event_name, uint32_t seconds)
{
	// any current lockouts for the event are updated with new expiration time
	ExpeditionLockoutTimer lockout{m_uuid, m_expedition_name, event_name, 0, seconds};
	lockout.Reset(); // sets expire time

	ExpeditionDatabase::InsertLockout(m_id, lockout);
	ExpeditionDatabase::InsertMembersLockout(m_members, lockout);

	ProcessLockoutUpdate(lockout, false);
	SendWorldLockoutUpdate(lockout, false);
}

void Expedition::RemoveLockout(const std::string& event_name)
{
	ExpeditionDatabase::DeleteLockout(m_id, event_name);
	ExpeditionDatabase::DeleteMembersLockout(m_members, m_expedition_name, event_name);

	ExpeditionLockoutTimer lockout{m_uuid, m_expedition_name, event_name, 0, 0};
	ProcessLockoutUpdate(lockout, true);
	SendWorldLockoutUpdate(lockout, true);
}

void Expedition::AddInternalMember(
	const std::string& char_name, uint32_t character_id, ExpeditionMemberStatus status, bool is_current_member)
{
	if (is_current_member)
	{
		auto it = std::find_if(m_members.begin(), m_members.end(),
			[character_id](const ExpeditionMember& member) {
				return member.char_id == character_id;
			});

		if (it == m_members.end())
		{
			m_members.emplace_back(ExpeditionMember{character_id, char_name, status});
		}
	}

	m_member_id_history.emplace(character_id);
}

bool Expedition::AddMember(const std::string& add_char_name, uint32_t add_char_id)
{
	if (HasMember(add_char_id))
	{
		return false;
	}

	ExpeditionDatabase::InsertMember(m_id, add_char_id);
	m_dynamiczone.AddCharacter(add_char_id);

	ProcessMemberAdded(add_char_name, add_char_id);
	SendWorldMemberChanged(add_char_name, add_char_id, false);

	return true;
}

void Expedition::RemoveAllMembers(bool enable_removal_timers)
{
	m_dynamiczone.RemoveAllCharacters(enable_removal_timers);

	ExpeditionDatabase::DeleteAllMembersPendingLockouts(m_members);
	ExpeditionDatabase::UpdateAllMembersRemoved(m_id);

	SendUpdatesToZoneMembers(true);
	SendWorldExpeditionUpdate(ServerOP_ExpeditionMembersRemoved);

	m_members.clear();
}

bool Expedition::RemoveMember(const std::string& remove_char_name)
{
	auto member = GetMemberData(remove_char_name);
	if (member.char_id == 0 || member.name.empty())
	{
		return false;
	}

	ExpeditionDatabase::UpdateMemberRemoved(m_id, member.char_id);
	m_dynamiczone.RemoveCharacter(member.char_id);

	ProcessMemberRemoved(member.name, member.char_id);
	SendWorldMemberChanged(member.name, member.char_id, true);

	// live always sends a leader update but we can send only if leader changes
	if (member.char_id == m_leader.char_id)
	{
		ChooseNewLeader();
	}

	return true;
}

void Expedition::SwapMember(Client* add_client, const std::string& remove_char_name)
{
	if (!add_client || remove_char_name.empty())
	{
		return;
	}

	auto member = GetMemberData(remove_char_name);
	if (member.char_id == 0 || member.name.empty())
	{
		return;
	}

	// make remove and add atomic to avoid racing with separate world messages
	ExpeditionDatabase::UpdateMemberRemoved(m_id, member.char_id);
	ExpeditionDatabase::InsertMember(m_id, add_client->CharacterID());
	m_dynamiczone.RemoveCharacter(member.char_id);
	m_dynamiczone.AddCharacter(add_client->CharacterID());

	ProcessMemberRemoved(member.name, member.char_id);
	ProcessMemberAdded(add_client->GetName(), add_client->CharacterID());
	SendWorldMemberSwapped(member.name, member.char_id, add_client->GetName(), add_client->CharacterID());

	if (!m_members.empty() && member.char_id == m_leader.char_id)
	{
		ChooseNewLeader();
	}
}

void Expedition::SetMemberStatus(Client* client, ExpeditionMemberStatus status)
{
	if (client)
	{
		UpdateMemberStatus(client->CharacterID(), status);
		SendWorldMemberStatus(client->CharacterID(), status);
	}
}

void Expedition::UpdateMemberStatus(uint32_t update_member_id, ExpeditionMemberStatus status)
{
	auto member_data = GetMemberData(update_member_id);
	if (member_data.char_id == 0 || member_data.name.empty())
	{
		return;
	}

	auto outapp_member_status = CreateMemberListStatusPacket(member_data.name, status);

	for (auto& member : m_members)
	{
		if (member.char_id == update_member_id)
		{
			member.status = status;
		}

		Client* member_client = entity_list.GetClientByCharID(member.char_id);
		if (member_client)
		{
			member_client->QueuePacket(outapp_member_status.get());
		}
	}
}

bool Expedition::ChooseNewLeader()
{
	for (const auto& member : m_members)
	{
		if (member.char_id != m_leader.char_id)
		{
			LogExpeditionsModerate("Replacing leader [{}] with [{}]", m_leader.name, member.name);
			SetNewLeader(member.char_id, member.name);
			return true;
		}
	}
	return false;
}

void Expedition::SendClientExpeditionInvite(
	Client* client, const std::string& inviter_name, const std::string& swap_remove_name)
{
	if (!client)
	{
		return;
	}

	LogExpeditionsModerate(
		"Sending expedition [{}] invite to player [{}] inviter [{}] swap name [{}]",
		m_id, client->GetName(), inviter_name, swap_remove_name
	);

	client->SetPendingExpeditionInvite(ExpeditionInvite{m_id, inviter_name, swap_remove_name});

	client->MessageString(
		Chat::System, EXPEDITION_ASKED_TO_JOIN, m_leader.name.c_str(), m_expedition_name.c_str()
	);

	// live (as of March 11 2020 patch) sends warnings for lockouts added
	// during current expedition that client would receive on entering dz
	bool warned = false;
	for (const auto& lockout_iter : m_lockouts)
	{
		// live doesn't issue a warning for the dz's replay timer
		const ExpeditionLockoutTimer& lockout = lockout_iter.second;
		if (!lockout.IsReplayTimer() && !lockout.IsExpired() && lockout.IsFromExpedition(m_uuid) &&
		    !client->HasExpeditionLockout(m_expedition_name, lockout.GetEventName()))
		{
			if (!warned)
			{
				client->Message(Chat::System, DZADD_INVITE_WARNING, m_expedition_name.c_str());
				warned = true;
			}

			auto time_remaining = lockout.GetDaysHoursMinutesRemaining();
			client->Message(
				Chat::System, DZADD_INVITE_WARNING_TIMER,
				lockout.GetEventName().c_str(),
				time_remaining.days.c_str(),
				time_remaining.hours.c_str(),
				time_remaining.mins.c_str()
			);
		}
	}

	auto outapp = CreateInvitePacket(inviter_name, swap_remove_name);
	client->QueuePacket(outapp.get());
}

void Expedition::SendLeaderMessage(
	Client* leader_client, uint16_t chat_type, uint32_t string_id, const std::initializer_list<std::string>& parameters)
{
	Client::SendCrossZoneMessageString(leader_client, m_leader.name, chat_type, string_id, parameters);
}

bool Expedition::ProcessAddConflicts(Client* leader_client, Client* add_client, bool swapping)
{
	if (!add_client) // a null leader_client is handled by SendLeaderMessage fallback
	{
		return true;
	}

	bool has_conflict = false;

	if (m_dynamiczone.IsCurrentZoneDzInstance())
	{
		SendLeaderMessage(leader_client, Chat::Red, DZADD_LEAVE_ZONE_FIRST, { add_client->GetName() });
		has_conflict = true;
	}

	auto expedition_id = add_client->GetExpeditionID();
	if (expedition_id)
	{
		auto string_id = (expedition_id == GetID()) ? DZADD_ALREADY_PART : DZADD_ALREADY_ASSIGNED;
		SendLeaderMessage(leader_client, Chat::Red, string_id, { add_client->GetName() });
		has_conflict = true;
	}

	// client with a replay lockout is allowed only if they were a previous member
	auto member_iter = m_member_id_history.find(add_client->CharacterID());
	bool was_member = (member_iter != m_member_id_history.end());
	if (!was_member)
	{
		auto replay_lockout = add_client->GetExpeditionLockout(m_expedition_name, DZ_REPLAY_TIMER_NAME);
		if (replay_lockout)
		{
			has_conflict = true;

			auto time_remaining = replay_lockout->GetDaysHoursMinutesRemaining();
			SendLeaderMessage(leader_client, Chat::Red, DZADD_REPLAY_TIMER, {
				add_client->GetName(),
				time_remaining.days,
				time_remaining.hours,
				time_remaining.mins
			});
		}
	}

	// check any extra event lockouts for this expedition that the client has and expedition doesn't
	auto client_lockouts = add_client->GetExpeditionLockouts(m_expedition_name);
	for (const auto& client_lockout : client_lockouts)
	{
		bool is_missing_lockout = (m_lockouts.find(client_lockout.GetEventName()) == m_lockouts.end());
		if (!client_lockout.IsReplayTimer() && is_missing_lockout)
		{
			has_conflict = true;

			auto time_remaining = client_lockout.GetDaysHoursMinutesRemaining();
			SendLeaderMessage(leader_client, Chat::Red, DZADD_EVENT_TIMER, {
				add_client->GetName(),
				client_lockout.GetEventName(),
				time_remaining.days,
				time_remaining.hours,
				time_remaining.mins,
				client_lockout.GetEventName()
			});
		}
	}

	// swapping ignores the max player count check since it's a 1:1 change
	if (!swapping && GetMemberCount() >= m_max_players)
	{
		SendLeaderMessage(leader_client, Chat::Red, DZADD_EXCEED_MAX, { fmt::format_int(m_max_players).str() });
		has_conflict = true;
	}

	auto invite_id = add_client->GetPendingExpeditionInviteID();
	if (invite_id)
	{
		auto string_id = (invite_id == GetID()) ? DZADD_PENDING : DZADD_PENDING_OTHER;
		SendLeaderMessage(leader_client, Chat::Red, string_id, { add_client->GetName() });
		has_conflict = true;
	}

	return has_conflict;
}

void Expedition::DzInviteResponse(Client* add_client, bool accepted, const std::string& swap_remove_name)
{
	if (!add_client)
	{
		return;
	}

	LogExpeditionsModerate(
		"Invite response by [{}] accepted [{}] swap_name [{}]",
		add_client->GetName(), accepted, swap_remove_name
	);

	// a null leader_client is handled by SendLeaderMessage fallbacks
	// note current leader receives invite reply messages (if leader changed)
	Client* leader_client = entity_list.GetClientByCharID(m_leader.char_id);

	if (!accepted)
	{
		SendLeaderMessage(leader_client, Chat::Red, EXPEDITION_INVITE_DECLINED, { add_client->GetName() });
		return;
	}

	bool was_swap_invite = !swap_remove_name.empty();
	bool has_conflicts = m_is_locked;

	if (m_is_locked)
	{
		SendLeaderMessage(leader_client, Chat::Red, DZADD_NOT_ALLOWING);
	}
	else
	{
		has_conflicts = ProcessAddConflicts(leader_client, add_client, was_swap_invite);
	}

	// error if swapping and character was already removed before the accept
	if (was_swap_invite && !HasMember(swap_remove_name))
	{
		has_conflicts = true;
	}

	if (has_conflicts)
	{
		SendLeaderMessage(leader_client, Chat::Red, EXPEDITION_INVITE_ERROR, { add_client->GetName() });
	}
	else
	{
		SendLeaderMessage(leader_client, Chat::Yellow, EXPEDITION_INVITE_ACCEPTED, { add_client->GetName() });

		// insert pending lockouts client will receive when entering dynamic zone.
		// only lockouts missing from client when they join are added. client may
		// have a lockout that expires after joining and shouldn't receive it again
		ExpeditionDatabase::DeletePendingLockouts(add_client->CharacterID());

		std::vector<ExpeditionLockoutTimer> pending_lockouts;
		for (const auto& lockout_iter : m_lockouts)
		{
			const ExpeditionLockoutTimer& lockout = lockout_iter.second;
			if (lockout.IsFromExpedition(m_uuid) &&
			    !add_client->HasExpeditionLockout(m_expedition_name, lockout.GetEventName()))
			{
				// replay timers are optionally added to new members immediately on
				// join with a fresh expire time using the original duration.
				if (lockout.IsReplayTimer())
				{
					if (m_add_replay_on_join)
					{
						ExpeditionLockoutTimer replay_timer = lockout;
						replay_timer.Reset();
						add_client->AddExpeditionLockout(replay_timer, true);
					}
				}
				else if (!lockout.IsExpired())
				{
					pending_lockouts.emplace_back(lockout);
				}
			}
		}

		bool add_immediately = m_dynamiczone.IsCurrentZoneDzInstance();

		ExpeditionDatabase::InsertCharacterLockouts(
			add_client->CharacterID(), pending_lockouts, false, !add_immediately);

		if (was_swap_invite)
		{
			SwapMember(add_client, swap_remove_name);
		}
		else
		{
			AddMember(add_client->GetName(), add_client->CharacterID());
		}

		if (m_dynamiczone.IsCurrentZoneDzInstance())
		{
			SetMemberStatus(add_client, ExpeditionMemberStatus::InDynamicZone);
		}
	}
}

bool Expedition::ConfirmLeaderCommand(Client* requester)
{
	if (!requester)
	{
		return false;
	}

	ExpeditionMember leader;
	if (RuleB(Expedition, UseDatabaseToVerifyLeaderCommands))
	{
		leader = ExpeditionDatabase::GetExpeditionLeader(m_id);
	}
	else
	{
		leader = m_leader;
	}

	if (leader.char_id == 0)
	{
		requester->MessageString(Chat::Red, UNABLE_RETRIEVE_LEADER); // unconfirmed message
		return false;
	}

	if (leader.char_id != requester->CharacterID())
	{
		requester->MessageString(Chat::Red, EXPEDITION_NOT_LEADER, leader.name.c_str());
		return false;
	}

	return true;
}

void Expedition::TryAddClient(
	Client* add_client, std::string inviter_name, std::string orig_add_name,
	std::string swap_remove_name, Client* leader_client)
{
	if (!add_client)
	{
		return;
	}

	LogExpeditionsModerate(
		"Add player request for expedition [{}] by inviter [{}] add name [{}] swap name [{}]",
		m_id, inviter_name, orig_add_name, swap_remove_name
	);

	// null leader client handled by ProcessAddConflicts/SendLeaderMessage fallbacks
	if (!leader_client)
	{
		leader_client = entity_list.GetClientByName(inviter_name.c_str());
	}

	bool has_conflicts = ProcessAddConflicts(leader_client, add_client, !swap_remove_name.empty());
	if (!has_conflicts)
	{
		// live uses the original unsanitized input string in invite messages
		uint32_t string_id = swap_remove_name.empty() ? DZADD_INVITE : DZSWAP_INVITE;
		SendLeaderMessage(leader_client, Chat::Yellow, string_id, { orig_add_name.c_str() });
		SendClientExpeditionInvite(add_client, inviter_name.c_str(), swap_remove_name);
	}
	else if (swap_remove_name.empty()) // swap command doesn't result in this message
	{
		SendLeaderMessage(leader_client, Chat::Red, DZADD_INVITE_FAIL, { add_client->GetName() });
	}
}

void Expedition::DzAddPlayer(
	Client* requester, std::string add_char_name, std::string swap_remove_name)
{
	if (!requester || !ConfirmLeaderCommand(requester))
	{
		return;
	}

	bool invite_failed = false;

	if (m_is_locked)
	{
		requester->MessageString(Chat::Red, DZADD_NOT_ALLOWING);
		invite_failed = true;
	}
	else if (add_char_name.empty())
	{
		requester->MessageString(Chat::Red, DZADD_NOT_ONLINE, add_char_name.c_str());
		invite_failed = true;
	}
	else
	{
		// we can avoid checking online status in world if we trust member status accuracy
		auto member_data = GetMemberData(add_char_name);
		if (member_data.char_id != 0 && member_data.status != ExpeditionMemberStatus::Offline)
		{
			requester->MessageString(Chat::Red, DZADD_ALREADY_PART, add_char_name.c_str());
			invite_failed = true;
		}
	}

	if (invite_failed)
	{
		requester->MessageString(Chat::Red, DZADD_INVITE_FAIL, FormatName(add_char_name).c_str());
		return;
	}

	Client* add_client = entity_list.GetClientByName(add_char_name.c_str());
	if (add_client)
	{
		// client is online in this zone
		TryAddClient(add_client, requester->GetName(), add_char_name, swap_remove_name, requester);
	}
	else
	{
		// forward to world to check if client is online and perform cross-zone invite
		SendWorldAddPlayerInvite(requester->GetName(), swap_remove_name, add_char_name);
	}
}

void Expedition::DzAddPlayerContinue(
	std::string inviter_name, std::string add_name, std::string swap_remove_name)
{
	// continuing expedition invite from leader in another zone
	Client* add_client = entity_list.GetClientByName(add_name.c_str());
	if (add_client)
	{
		TryAddClient(add_client, inviter_name, add_name, swap_remove_name);
	}
}

void Expedition::DzMakeLeader(Client* requester, std::string new_leader_name)
{
	if (!requester || !ConfirmLeaderCommand(requester))
	{
		return;
	}

	// live uses sanitized input name for all /dzmakeleader messages
	new_leader_name = FormatName(new_leader_name);

	if (new_leader_name.empty())
	{
		requester->MessageString(Chat::Red, DZMAKELEADER_NOT_ONLINE, new_leader_name.c_str());
		return;
	}

	auto new_leader_data = GetMemberData(new_leader_name);
	if (new_leader_data.char_id == 0)
	{
		requester->MessageString(Chat::Red, EXPEDITION_NOT_MEMBER, new_leader_name.c_str());
		return;
	}

	// database is not updated until new leader client validated
	Client* new_leader_client = entity_list.GetClientByName(new_leader_name.c_str());
	if (new_leader_client)
	{
		ProcessMakeLeader(requester, new_leader_client, new_leader_name, true);
	}
	else
	{
		// new leader not in this zone, let world verify and pass to new leader's zone
		SendWorldMakeLeaderRequest(requester->GetName(), FormatName(new_leader_name));
	}
}

void Expedition::DzRemovePlayer(Client* requester, std::string char_name)
{
	if (!requester || !ConfirmLeaderCommand(requester))
	{
		return;
	}

	LogExpeditionsModerate(
		"Remove player request for expedition [{}] by [{}] leader [{}] remove name [{}]",
		m_id, requester->GetName(), m_leader.name, char_name
	);

	char_name = FormatName(char_name);

	// live only seems to enforce min_players for requesting expeditions, no need to check here
	bool removed = RemoveMember(char_name);
	if (!removed)
	{
		requester->MessageString(Chat::Red, EXPEDITION_NOT_MEMBER, char_name.c_str());
	}
	else
	{
		requester->MessageString(Chat::Yellow, EXPEDITION_REMOVED, char_name.c_str(), m_expedition_name.c_str());
	}
}

void Expedition::DzQuit(Client* requester)
{
	if (requester)
	{
		RemoveMember(requester->GetName());
	}
}

void Expedition::DzSwapPlayer(
	Client* requester, std::string remove_char_name, std::string add_char_name)
{
	if (!requester || !ConfirmLeaderCommand(requester))
	{
		return;
	}

	if (remove_char_name.empty() || !HasMember(remove_char_name))
	{
		requester->MessageString(Chat::Red, DZSWAP_CANNOT_REMOVE, FormatName(remove_char_name).c_str());
		return;
	}

	DzAddPlayer(requester, add_char_name, remove_char_name);
}

void Expedition::DzPlayerList(Client* requester)
{
	if (requester)
	{
		requester->MessageString(Chat::Yellow, EXPEDITION_LEADER, m_leader.name.c_str());

		std::string member_names;
		for (const auto& member : m_members)
		{
			fmt::format_to(std::back_inserter(member_names), "{}, ", member.name);
		}

		if (member_names.size() > 1)
		{
			member_names.erase(member_names.length() - 2); // trailing comma and space
		}

		requester->MessageString(Chat::Yellow, EXPEDITION_MEMBERS, member_names.c_str());
	}
}

void Expedition::DzKickPlayers(Client* requester)
{
	if (!requester || !ConfirmLeaderCommand(requester))
	{
		return;
	}

	RemoveAllMembers();
	requester->MessageString(Chat::Red, EXPEDITION_REMOVED, KICKPLAYERS_EVERYONE, m_expedition_name.c_str());
}

void Expedition::SetLocked(bool lock_expedition, bool update_db)
{
	m_is_locked = lock_expedition;

	if (update_db)
	{
		ExpeditionDatabase::UpdateLockState(m_id, lock_expedition);
		SendWorldSettingChanged(ServerOP_ExpeditionLockState, m_is_locked);
	}
}

void Expedition::SetNewLeader(uint32_t new_leader_id, const std::string& new_leader_name)
{
	ExpeditionDatabase::UpdateLeaderID(m_id, new_leader_id);
	ProcessLeaderChanged(new_leader_id, new_leader_name);
	SendWorldLeaderChanged();
}

void Expedition::ProcessLeaderChanged(uint32_t new_leader_id, const std::string& new_leader_name)
{
	m_leader.char_id = new_leader_id;
	m_leader.name = new_leader_name;

	// update each client's expedition window in this zone
	auto outapp_leader = CreateLeaderNamePacket();
	for (const auto& member : m_members)
	{
		Client* member_client = entity_list.GetClientByCharID(member.char_id);
		if (member_client)
		{
			member_client->QueuePacket(outapp_leader.get());
		}
	}
}

void Expedition::ProcessMakeLeader(
	Client* old_leader_client, Client* new_leader_client, const std::string& new_leader_name, bool is_online)
{
	if (old_leader_client)
	{
		// online flag is set by world to verify new leader is online or not
		if (is_online)
		{
			old_leader_client->MessageString(Chat::Yellow, DZMAKELEADER_NAME, new_leader_name.c_str());
		}
		else
		{
			old_leader_client->MessageString(Chat::Red, DZMAKELEADER_NOT_ONLINE, new_leader_name.c_str());
		}
	}

	if (!new_leader_client)
	{
		new_leader_client = entity_list.GetClientByName(new_leader_name.c_str());
	}

	if (new_leader_client)
	{
		new_leader_client->MessageString(Chat::Yellow, DZMAKELEADER_YOU);
		SetNewLeader(new_leader_client->CharacterID(), new_leader_client->GetName());
	}
}

void Expedition::ProcessMemberAdded(std::string char_name, uint32_t added_char_id)
{
	// adds the member to this expedition and notifies both leader and new member
	Client* leader_client = entity_list.GetClientByCharID(m_leader.char_id);
	if (leader_client)
	{
		leader_client->MessageString(Chat::Yellow, EXPEDITION_MEMBER_ADDED, char_name.c_str(), m_expedition_name.c_str());
	}

	Client* member_client = entity_list.GetClientByCharID(added_char_id);
	if (member_client)
	{
		member_client->SetExpeditionID(GetID());
		member_client->SendDzCompassUpdate();
		SendClientExpeditionInfo(member_client);
		member_client->MessageString(Chat::Yellow, EXPEDITION_MEMBER_ADDED, char_name.c_str(), m_expedition_name.c_str());
	}

	AddInternalMember(char_name, added_char_id, ExpeditionMemberStatus::Online);

	SendUpdatesToZoneMembers(); // live sends full update when member added
}

void Expedition::ProcessMemberRemoved(std::string removed_char_name, uint32_t removed_char_id)
{
	if (m_members.empty())
	{
		return;
	}

	auto outapp_member_name = CreateMemberListNamePacket(removed_char_name, true);

	for (auto it = m_members.begin(); it != m_members.end();)
	{
		bool is_removed = (it->name == removed_char_name);

		Client* member_client = entity_list.GetClientByCharID(it->char_id);
		if (member_client)
		{
			// all members receive the removed player name packet
			member_client->QueuePacket(outapp_member_name.get());

			if (is_removed)
			{
				// live doesn't clear expedition info on clients removed while inside dz.
				// it instead let's the dz kick timer do it even if character zones out
				// before it triggers. for simplicity we'll always clear immediately
				ExpeditionDatabase::DeletePendingLockouts(member_client->CharacterID());
				member_client->SetExpeditionID(0);
				member_client->SendDzCompassUpdate();
				member_client->QueuePacket(CreateInfoPacket(true).get());
				member_client->MessageString(
					Chat::Yellow, EXPEDITION_REMOVED, it->name.c_str(), m_expedition_name.c_str()
				);
			}
		}

		it = is_removed ? m_members.erase(it) : it + 1;
	}

	LogExpeditionsDetail(
		"Processed member [{}] ({}) removal from [{}], cache member count: [{}]",
		removed_char_name, removed_char_id, m_id, m_members.size()
	);
}

void Expedition::ProcessLockoutUpdate(const ExpeditionLockoutTimer& lockout, bool remove)
{
	if (!remove)
	{
		m_lockouts[lockout.GetEventName()] = lockout;
	}
	else
	{
		m_lockouts.erase(lockout.GetEventName());
	}

	for (const auto& member : m_members)
	{
		Client* member_client = entity_list.GetClientByCharID(member.char_id);
		if (member_client)
		{
			if (!remove)
			{
				member_client->AddExpeditionLockout(lockout);
			}
			else
			{
				member_client->RemoveExpeditionLockout(m_expedition_name, lockout.GetEventName());
			}
		}
	}

	// if this is the expedition's dz instance, all clients inside the zone need
	// to receive added lockouts. this is done on live to avoid exploits where
	// members leave the expedition but haven't been kicked from zone yet
	if (!remove && m_dynamiczone.IsCurrentZoneDzInstance())
	{
		std::vector<ExpeditionMember> non_members;
		for (const auto& client_iter : entity_list.GetClientList())
		{
			Client* client = client_iter.second;
			if (client && client->GetExpeditionID() != GetID())
			{
				non_members.emplace_back(ExpeditionMember{client->CharacterID(), client->GetName()});
				client->AddExpeditionLockout(lockout);
			}
		}

		if (!non_members.empty())
		{
			ExpeditionDatabase::InsertMembersLockout(non_members, lockout);
		}
	}
}

void Expedition::SendUpdatesToZoneMembers(bool clear, bool message_on_clear)
{
	if (!m_members.empty())
	{
		auto outapp_info = CreateInfoPacket(clear);
		auto outapp_members = CreateMemberListPacket(clear);

		for (const auto& member : m_members)
		{
			Client* member_client = entity_list.GetClientByCharID(member.char_id);
			if (member_client)
			{
				member_client->SetExpeditionID(clear ? 0 : GetID());
				member_client->SendDzCompassUpdate();
				member_client->QueuePacket(outapp_info.get());
				member_client->QueuePacket(outapp_members.get());
				member_client->SendExpeditionLockoutTimers();
				if (clear && message_on_clear)
				{
					member_client->MessageString(
						Chat::Yellow, EXPEDITION_REMOVED, member_client->GetName(), m_expedition_name.c_str()
					);
				}
			}
		}
	}
}

void Expedition::SendClientExpeditionInfo(Client* client)
{
	if (client)
	{
		client->QueuePacket(CreateInfoPacket().get());
		client->QueuePacket(CreateMemberListPacket().get());
	}
}

void Expedition::SendWorldPendingInvite(const ExpeditionInvite& invite, const std::string& add_name)
{
	LogExpeditions(
		"Character [{}] saving pending invite from [{}] to expedition [{}] in world",
		add_name, invite.inviter_name, invite.expedition_id
	);

	SendWorldAddPlayerInvite(invite.inviter_name, invite.swap_remove_name, add_name, true);
}

std::unique_ptr<EQApplicationPacket> Expedition::CreateInfoPacket(bool clear)
{
	uint32_t outsize = sizeof(ExpeditionInfo_Struct);
	auto outapp = std::unique_ptr<EQApplicationPacket>(new EQApplicationPacket(OP_DzExpeditionInfo, outsize));
	auto info = reinterpret_cast<ExpeditionInfo_Struct*>(outapp->pBuffer);
	if (!clear)
	{
		info->client_id = 0;
		info->assigned = true;
		strn0cpy(info->expedition_name, m_expedition_name.c_str(), sizeof(info->expedition_name));
		strn0cpy(info->leader_name, m_leader.name.c_str(), sizeof(info->leader_name));
		info->max_players = m_max_players;
	}
	return outapp;
}

std::unique_ptr<EQApplicationPacket> Expedition::CreateInvitePacket(
	const std::string& inviter_name, const std::string& swap_remove_name)
{
	uint32_t outsize = sizeof(ExpeditionInvite_Struct);
	auto outapp = std::unique_ptr<EQApplicationPacket>(new EQApplicationPacket(OP_DzExpeditionInvite, outsize));
	auto outbuf = reinterpret_cast<ExpeditionInvite_Struct*>(outapp->pBuffer);
	strn0cpy(outbuf->inviter_name, inviter_name.c_str(), sizeof(outbuf->inviter_name));
	strn0cpy(outbuf->expedition_name, m_expedition_name.c_str(), sizeof(outbuf->expedition_name));
	strn0cpy(outbuf->swap_name, swap_remove_name.c_str(), sizeof(outbuf->swap_name));
	outbuf->swapping = !swap_remove_name.empty();
	outbuf->dz_zone_id = m_dynamiczone.GetZoneID();
	outbuf->dz_instance_id = m_dynamiczone.GetInstanceID();
	return outapp;
}

std::unique_ptr<EQApplicationPacket> Expedition::CreateMemberListPacket(bool clear)
{
	uint32_t member_count = clear ? 0 : static_cast<uint32_t>(m_members.size());
	uint32_t member_entries_size = sizeof(ExpeditionMemberEntry_Struct) * member_count;
	uint32_t outsize = sizeof(ExpeditionMemberList_Struct) + member_entries_size;
	auto outapp = std::unique_ptr<EQApplicationPacket>(new EQApplicationPacket(OP_DzMemberList, outsize));
	auto buf = reinterpret_cast<ExpeditionMemberList_Struct*>(outapp->pBuffer);

	buf->client_id = 0;
	buf->count = member_count;

	if (!clear)
	{
		for (auto i = 0; i < m_members.size(); ++i)
		{
			strn0cpy(buf->members[i].name, m_members[i].name.c_str(), sizeof(buf->members[i].name));
			buf->members[i].status = static_cast<uint8_t>(m_members[i].status);
		}
	}

	return outapp;
}

std::unique_ptr<EQApplicationPacket> Expedition::CreateMemberListNamePacket(
	const std::string& name, bool remove_name)
{
	uint32_t outsize = sizeof(ExpeditionMemberListName_Struct);
	auto outapp = std::unique_ptr<EQApplicationPacket>(new EQApplicationPacket(OP_DzMemberListName, outsize));
	auto buf = reinterpret_cast<ExpeditionMemberListName_Struct*>(outapp->pBuffer);
	buf->client_id = 0;
	buf->add_name = !remove_name;
	strn0cpy(buf->name, name.c_str(), sizeof(buf->name));
	return outapp;
}

std::unique_ptr<EQApplicationPacket> Expedition::CreateMemberListStatusPacket(
	const std::string& name, ExpeditionMemberStatus status)
{
	// member list status uses member list struct with a single entry
	uint32_t outsize = sizeof(ExpeditionMemberList_Struct) + sizeof(ExpeditionMemberEntry_Struct);
	auto outapp = std::unique_ptr<EQApplicationPacket>(new EQApplicationPacket(OP_DzMemberListStatus, outsize));
	auto buf = reinterpret_cast<ExpeditionMemberList_Struct*>(outapp->pBuffer);
	buf->client_id = 0;
	buf->count = 1;

	auto entry = reinterpret_cast<ExpeditionMemberEntry_Struct*>(buf->members);
	strn0cpy(entry->name, name.c_str(), sizeof(entry->name));
	entry->status = static_cast<uint8_t>(status);

	return outapp;
}

std::unique_ptr<EQApplicationPacket> Expedition::CreateLeaderNamePacket()
{
	uint32_t outsize = sizeof(ExpeditionSetLeaderName_Struct);
	auto outapp = std::unique_ptr<EQApplicationPacket>(new EQApplicationPacket(OP_DzSetLeaderName, outsize));
	auto buf = reinterpret_cast<ExpeditionSetLeaderName_Struct*>(outapp->pBuffer);
	buf->client_id = 0;
	strn0cpy(buf->leader_name, m_leader.name.c_str(), sizeof(buf->leader_name));
	return outapp;
}

void Expedition::SendWorldExpeditionUpdate(uint16_t server_opcode)
{
	uint32_t pack_size = sizeof(ServerExpeditionID_Struct);
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(server_opcode, pack_size));
	auto buf = reinterpret_cast<ServerExpeditionID_Struct*>(pack->pBuffer);
	buf->expedition_id = GetID();
	buf->sender_zone_id = zone ? zone->GetZoneID() : 0;
	buf->sender_instance_id = zone ? zone->GetInstanceID() : 0;
	worldserver.SendPacket(pack.get());
}

void Expedition::SendWorldAddPlayerInvite(
	const std::string& inviter_name, const std::string& swap_remove_name, const std::string& add_name, bool pending)
{
	auto server_opcode = pending ? ServerOP_ExpeditionSaveInvite : ServerOP_ExpeditionDzAddPlayer;
	uint32_t pack_size = sizeof(ServerDzCommand_Struct);
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(server_opcode, pack_size));
	auto buf = reinterpret_cast<ServerDzCommand_Struct*>(pack->pBuffer);
	buf->expedition_id = GetID();
	buf->is_char_online = false;
	strn0cpy(buf->requester_name, inviter_name.c_str(), sizeof(buf->requester_name));
	strn0cpy(buf->target_name, add_name.c_str(), sizeof(buf->target_name));
	strn0cpy(buf->remove_name, swap_remove_name.c_str(), sizeof(buf->remove_name));
	worldserver.SendPacket(pack.get());
}

void Expedition::SendWorldLeaderChanged()
{
	uint32_t pack_size = sizeof(ServerExpeditionMemberChange_Struct);
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(ServerOP_ExpeditionLeaderChanged, pack_size));
	auto buf = reinterpret_cast<ServerExpeditionMemberChange_Struct*>(pack->pBuffer);
	buf->expedition_id = GetID();
	buf->sender_zone_id = zone ? zone->GetZoneID() : 0;
	buf->sender_instance_id = zone ? zone->GetInstanceID() : 0;
	buf->char_id = m_leader.char_id;
	strn0cpy(buf->char_name, m_leader.name.c_str(), sizeof(buf->char_name));
	worldserver.SendPacket(pack.get());
}

void Expedition::SendWorldLockoutUpdate(
	const ExpeditionLockoutTimer& lockout, bool remove)
{
	uint32_t pack_size = sizeof(ServerExpeditionLockout_Struct);
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(ServerOP_ExpeditionLockout, pack_size));
	auto buf = reinterpret_cast<ServerExpeditionLockout_Struct*>(pack->pBuffer);
	buf->expedition_id = GetID();
	buf->expire_time = lockout.GetExpireTime();
	buf->duration = lockout.GetDuration();
	buf->sender_zone_id = zone ? zone->GetZoneID() : 0;
	buf->sender_instance_id = zone ? zone->GetInstanceID() : 0;
	buf->remove = remove;
	strn0cpy(buf->event_name, lockout.GetEventName().c_str(), sizeof(buf->event_name));
	worldserver.SendPacket(pack.get());
}

void Expedition::SendWorldMakeLeaderRequest(
	const std::string& requester_name, const std::string& new_leader_name)
{
	uint32_t pack_size = sizeof(ServerDzCommand_Struct);
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(ServerOP_ExpeditionDzMakeLeader, pack_size));
	auto buf = reinterpret_cast<ServerDzCommand_Struct*>(pack->pBuffer);
	buf->expedition_id = GetID();
	buf->is_char_online = false;
	strn0cpy(buf->requester_name, requester_name.c_str(), sizeof(buf->requester_name));
	strn0cpy(buf->target_name, new_leader_name.c_str(), sizeof(buf->target_name));
	worldserver.SendPacket(pack.get());
}

void Expedition::SendWorldMemberChanged(const std::string& char_name, uint32_t char_id, bool remove)
{
	// notify other zones of added or removed member
	uint32_t pack_size = sizeof(ServerExpeditionMemberChange_Struct);
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(ServerOP_ExpeditionMemberChange, pack_size));
	auto buf = reinterpret_cast<ServerExpeditionMemberChange_Struct*>(pack->pBuffer);
	buf->expedition_id = GetID();
	buf->sender_zone_id = zone ? zone->GetZoneID() : 0;
	buf->sender_instance_id = zone ? zone->GetInstanceID() : 0;
	buf->removed = remove;
	buf->char_id = char_id;
	strn0cpy(buf->char_name, char_name.c_str(), sizeof(buf->char_name));
	worldserver.SendPacket(pack.get());
}

void Expedition::SendWorldMemberStatus(uint32_t character_id, ExpeditionMemberStatus status)
{
	uint32_t pack_size = sizeof(ServerExpeditionMemberStatus_Struct);
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(ServerOP_ExpeditionMemberStatus, pack_size));
	auto buf = reinterpret_cast<ServerExpeditionMemberStatus_Struct*>(pack->pBuffer);
	buf->expedition_id = GetID();
	buf->sender_zone_id = zone ? zone->GetZoneID() : 0;
	buf->sender_instance_id = zone ? zone->GetInstanceID() : 0;
	buf->status = static_cast<uint8_t>(status);
	buf->character_id = character_id;
	worldserver.SendPacket(pack.get());
}

void Expedition::SendWorldDzLocationUpdate(uint16_t server_opcode, const DynamicZoneLocation& location)
{
	uint32_t pack_size = sizeof(ServerDzLocation_Struct);
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(server_opcode, pack_size));
	auto buf = reinterpret_cast<ServerDzLocation_Struct*>(pack->pBuffer);
	buf->owner_id = GetID();
	buf->dz_zone_id = m_dynamiczone.GetZoneID();
	buf->dz_instance_id = m_dynamiczone.GetInstanceID();
	buf->sender_zone_id = zone ? zone->GetZoneID() : 0;
	buf->sender_instance_id = zone ? zone->GetInstanceID() : 0;
	buf->zone_id = location.zone_id;
	buf->x = location.x;
	buf->y = location.y;
	buf->z = location.z;
	buf->heading = location.heading;
	worldserver.SendPacket(pack.get());
}

void Expedition::SendWorldMemberSwapped(
	const std::string& remove_char_name, uint32_t remove_char_id, const std::string& add_char_name, uint32_t add_char_id)
{
	uint32_t pack_size = sizeof(ServerExpeditionMemberSwap_Struct);
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(ServerOP_ExpeditionMemberSwap, pack_size));
	auto buf = reinterpret_cast<ServerExpeditionMemberSwap_Struct*>(pack->pBuffer);
	buf->expedition_id = GetID();
	buf->sender_zone_id = zone ? zone->GetZoneID() : 0;
	buf->sender_instance_id = zone ? zone->GetInstanceID() : 0;
	buf->add_char_id = add_char_id;
	buf->remove_char_id = remove_char_id;
	strn0cpy(buf->add_char_name, add_char_name.c_str(), sizeof(buf->add_char_name));
	strn0cpy(buf->remove_char_name, remove_char_name.c_str(), sizeof(buf->remove_char_name));
	worldserver.SendPacket(pack.get());
}

void Expedition::SendWorldSettingChanged(uint16_t server_opcode, bool setting_value)
{
	uint32_t pack_size = sizeof(ServerExpeditionSetting_Struct);
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(server_opcode, pack_size));
	auto buf = reinterpret_cast<ServerExpeditionSetting_Struct*>(pack->pBuffer);
	buf->expedition_id = GetID();
	buf->sender_zone_id = zone ? zone->GetZoneID() : 0;
	buf->sender_instance_id = zone ? zone->GetInstanceID() : 0;
	buf->enabled = setting_value;
	worldserver.SendPacket(pack.get());
}

void Expedition::SendWorldGetOnlineMembers(
	const std::vector<std::pair<uint32_t, uint32_t>>& expedition_character_ids)
{
	// request online status of characters
	uint32_t count = static_cast<uint32_t>(expedition_character_ids.size());
	uint32_t entries_size = sizeof(ServerExpeditionCharacterEntry_Struct) * count;
	uint32_t pack_size = sizeof(ServerExpeditionCharacters_Struct) + entries_size;
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(ServerOP_ExpeditionGetOnlineMembers, pack_size));
	auto buf = reinterpret_cast<ServerExpeditionCharacters_Struct*>(pack->pBuffer);
	buf->sender_zone_id = zone ? zone->GetZoneID() : 0;
	buf->sender_instance_id = zone ? zone->GetInstanceID() : 0;
	buf->count = count;
	for (uint32_t i = 0; i < buf->count; ++i)
	{
		buf->entries[i].expedition_id = expedition_character_ids[i].first;
		buf->entries[i].character_id = expedition_character_ids[i].second;
		buf->entries[i].character_zone_id = 0;
		buf->entries[i].character_instance_id = 0;
		buf->entries[i].character_online = false;
	}
	worldserver.SendPacket(pack.get());
}

void Expedition::RemoveCharacterLockouts(
	std::string character_name, std::string expedition_name, std::string event_name)
{
	uint32_t pack_size = sizeof(ServerExpeditionCharacterLockout_Struct);
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(ServerOP_ExpeditionRemoveCharLockouts, pack_size));
	auto buf = reinterpret_cast<ServerExpeditionCharacterLockout_Struct*>(pack->pBuffer);
	strn0cpy(buf->character_name, character_name.c_str(), sizeof(buf->character_name));
	strn0cpy(buf->expedition_name, expedition_name.c_str(), sizeof(buf->expedition_name));
	strn0cpy(buf->event_name, event_name.c_str(), sizeof(buf->event_name));
	worldserver.SendPacket(pack.get());
}

void Expedition::HandleWorldMessage(ServerPacket* pack)
{
	switch (pack->opcode)
	{
	case ServerOP_ExpeditionCreate:
	{
		auto buf = reinterpret_cast<ServerExpeditionID_Struct*>(pack->pBuffer);
		if (zone && !zone->IsZone(buf->sender_zone_id, buf->sender_instance_id))
		{
			Expedition::CacheFromDatabase(buf->expedition_id);
		}
		break;
	}
	case ServerOP_ExpeditionDeleted:
	{
		// sent by world when it deletes expired or empty expeditions
		auto buf = reinterpret_cast<ServerExpeditionID_Struct*>(pack->pBuffer);
		auto expedition = Expedition::FindCachedExpeditionByID(buf->expedition_id);
		if (zone && expedition)
		{
			expedition->SendUpdatesToZoneMembers(true, false); // any members silently removed

			LogExpeditionsModerate("Deleting expedition [{}] from zone cache", buf->expedition_id);
			zone->expedition_cache.erase(buf->expedition_id);
		}
		break;
	}
	case ServerOP_ExpeditionMembersRemoved:
	{
		auto buf = reinterpret_cast<ServerExpeditionID_Struct*>(pack->pBuffer);
		if (zone && !zone->IsZone(buf->sender_zone_id, buf->sender_instance_id))
		{
			auto expedition = Expedition::FindCachedExpeditionByID(buf->expedition_id);
			if (expedition)
			{
				expedition->SendUpdatesToZoneMembers(true);
				expedition->m_members.clear();
			}
		}
		break;
	}
	case ServerOP_ExpeditionLeaderChanged:
	{
		auto buf = reinterpret_cast<ServerExpeditionMemberChange_Struct*>(pack->pBuffer);
		if (zone && !zone->IsZone(buf->sender_zone_id, buf->sender_instance_id))
		{
			auto expedition = Expedition::FindCachedExpeditionByID(buf->expedition_id);
			if (expedition)
			{
				expedition->ProcessLeaderChanged(buf->char_id, buf->char_name);
			}
		}
		break;
	}
	case ServerOP_ExpeditionLockout:
	{
		auto buf = reinterpret_cast<ServerExpeditionLockout_Struct*>(pack->pBuffer);
		if (zone && !zone->IsZone(buf->sender_zone_id, buf->sender_instance_id))
		{
			auto expedition = Expedition::FindCachedExpeditionByID(buf->expedition_id);
			if (expedition)
			{
				ExpeditionLockoutTimer lockout{
					expedition->GetUUID(), expedition->GetName(), buf->event_name, buf->expire_time, buf->duration
				};
				expedition->ProcessLockoutUpdate(lockout, buf->remove);
			}
		}
		break;
	}
	case ServerOP_ExpeditionMemberChange:
	{
		auto buf = reinterpret_cast<ServerExpeditionMemberChange_Struct*>(pack->pBuffer);

		auto expedition = Expedition::FindCachedExpeditionByID(buf->expedition_id);
		if (expedition && zone)
		{
			if (!zone->IsZone(buf->sender_zone_id, buf->sender_instance_id))
			{
				if (buf->removed)
				{
					expedition->ProcessMemberRemoved(buf->char_name, buf->char_id);
				}
				else
				{
					expedition->ProcessMemberAdded(buf->char_name, buf->char_id);
				}
			}
		}
		break;
	}
	case ServerOP_ExpeditionMemberSwap:
	{
		auto buf = reinterpret_cast<ServerExpeditionMemberSwap_Struct*>(pack->pBuffer);
		if (zone && !zone->IsZone(buf->sender_zone_id, buf->sender_instance_id))
		{
			auto expedition = Expedition::FindCachedExpeditionByID(buf->expedition_id);
			if (expedition)
			{
				expedition->ProcessMemberRemoved(buf->remove_char_name, buf->remove_char_id);
				expedition->ProcessMemberAdded(buf->add_char_name, buf->add_char_id);
			}
		}
		break;
	}
	case ServerOP_ExpeditionMemberStatus:
	{
		auto buf = reinterpret_cast<ServerExpeditionMemberStatus_Struct*>(pack->pBuffer);
		if (zone && !zone->IsZone(buf->sender_zone_id, buf->sender_instance_id))
		{
			auto expedition = Expedition::FindCachedExpeditionByID(buf->expedition_id);
			if (expedition)
			{
				expedition->UpdateMemberStatus(buf->character_id, static_cast<ExpeditionMemberStatus>(buf->status));
			}
		}
		break;
	}
	case ServerOP_ExpeditionLockState:
	case ServerOP_ExpeditionReplayOnJoin:
	{
		auto buf = reinterpret_cast<ServerExpeditionSetting_Struct*>(pack->pBuffer);
		if (zone && !zone->IsZone(buf->sender_zone_id, buf->sender_instance_id))
		{
			auto expedition = Expedition::FindCachedExpeditionByID(buf->expedition_id);
			if (expedition)
			{
				if (pack->opcode == ServerOP_ExpeditionLockState)
				{
					expedition->SetLocked(buf->enabled);
				}
				else if (pack->opcode == ServerOP_ExpeditionReplayOnJoin)
				{
					expedition->SetReplayLockoutOnMemberJoin(buf->enabled);
				}
			}
		}
		break;
	}
	case ServerOP_ExpeditionGetOnlineMembers:
	{
		// reply from world for online member statuses request (for multiple expeditions)
		auto buf = reinterpret_cast<ServerExpeditionCharacters_Struct*>(pack->pBuffer);
		for (uint32_t i = 0; i < buf->count; ++i)
		{
			auto member = reinterpret_cast<ServerExpeditionCharacterEntry_Struct*>(&buf->entries[i]);
			auto expedition = Expedition::FindCachedExpeditionByID(member->expedition_id);
			if (expedition)
			{
				auto is_online = member->character_online;
				auto status = is_online ? ExpeditionMemberStatus::Online : ExpeditionMemberStatus::Offline;
				if (is_online && expedition->GetDynamicZone().IsInstanceID(member->character_instance_id))
				{
					status = ExpeditionMemberStatus::InDynamicZone;
				}
				expedition->UpdateMemberStatus(member->character_id, status);
			}
		}
		break;
	}
	case ServerOP_ExpeditionDzAddPlayer:
	{
		auto buf = reinterpret_cast<ServerDzCommand_Struct*>(pack->pBuffer);
		if (buf->is_char_online)
		{
			auto expedition = Expedition::FindCachedExpeditionByID(buf->expedition_id);
			if (expedition)
			{
				expedition->DzAddPlayerContinue(buf->requester_name, buf->target_name, buf->remove_name);
			}
		}
		else
		{
			Client* leader = entity_list.GetClientByName(buf->requester_name);
			if (leader)
			{
				leader->MessageString(Chat::Red, DZADD_NOT_ONLINE, FormatName(buf->target_name).c_str());
				leader->MessageString(Chat::Red, DZADD_INVITE_FAIL, FormatName(buf->target_name).c_str());
			}
		}
		break;
	}
	case ServerOP_ExpeditionDzMakeLeader:
	{
		auto buf = reinterpret_cast<ServerDzCommand_Struct*>(pack->pBuffer);
		auto expedition = Expedition::FindCachedExpeditionByID(buf->expedition_id);
		if (expedition)
		{
			auto old_leader_client = entity_list.GetClientByName(buf->requester_name);
			auto new_leader_client = entity_list.GetClientByName(buf->target_name);
			expedition->ProcessMakeLeader(old_leader_client, new_leader_client, buf->target_name, buf->is_char_online);
		}
		break;
	}
	case ServerOP_ExpeditionDzCompass:
	case ServerOP_ExpeditionDzSafeReturn:
	case ServerOP_ExpeditionDzZoneIn:
	{
		auto buf = reinterpret_cast<ServerDzLocation_Struct*>(pack->pBuffer);
		if (zone && !zone->IsZone(buf->sender_zone_id, buf->sender_instance_id))
		{
			auto expedition = Expedition::FindCachedExpeditionByID(buf->owner_id);
			if (expedition)
			{
				if (pack->opcode == ServerOP_ExpeditionDzCompass)
				{
					expedition->SetDzCompass(buf->zone_id, buf->x, buf->y, buf->z, false);
				}
				else if (pack->opcode == ServerOP_ExpeditionDzSafeReturn)
				{
					expedition->SetDzSafeReturn(buf->zone_id, buf->x, buf->y, buf->z, buf->heading, false);
				}
				else if (pack->opcode == ServerOP_ExpeditionDzZoneIn)
				{
					expedition->SetDzZoneInLocation(buf->x, buf->y, buf->z, buf->heading, false);
				}
			}
		}
		break;
	}
	case ServerOP_ExpeditionRemoveCharLockouts:
	{
		auto buf = reinterpret_cast<ServerExpeditionCharacterLockout_Struct*>(pack->pBuffer);
		Client* client = entity_list.GetClientByName(buf->character_name);
		if (client)
		{
			if (buf->event_name[0] != '\0')
			{
				client->RemoveExpeditionLockout(buf->expedition_name, buf->event_name, true);
			}
			else
			{
				client->RemoveAllExpeditionLockouts(buf->expedition_name);
			}
		}
		break;
	}
	case ServerOP_ExpeditionDzDuration:
	{
		auto buf = reinterpret_cast<ServerExpeditionUpdateDuration_Struct*>(pack->pBuffer);
		auto expedition = Expedition::FindCachedExpeditionByID(buf->expedition_id);
		if (expedition)
		{
			expedition->SetDzDuration(buf->new_duration_seconds);
		}
		break;
	}
	}
}

void Expedition::SetDzCompass(uint32_t zone_id, float x, float y, float z, bool update_db)
{
	DynamicZoneLocation location{ zone_id, x, y, z, 0.0f };
	m_dynamiczone.SetCompass(location, update_db);

	for (const auto& member : m_members)
	{
		Client* member_client = entity_list.GetClientByCharID(member.char_id);
		if (member_client)
		{
			member_client->SendDzCompassUpdate();
		}
	}

	if (update_db)
	{
		SendWorldDzLocationUpdate(ServerOP_ExpeditionDzCompass, location);
	}
}

void Expedition::SetDzCompass(const std::string& zone_name, float x, float y, float z, bool update_db)
{
	auto zone_id = ZoneID(zone_name.c_str());
	SetDzCompass(zone_id, x, y, z, update_db);
}

void Expedition::SetDzSafeReturn(uint32_t zone_id, float x, float y, float z, float heading, bool update_db)
{
	DynamicZoneLocation location{ zone_id, x, y, z, heading };

	m_dynamiczone.SetSafeReturn(location, update_db);

	if (update_db)
	{
		SendWorldDzLocationUpdate(ServerOP_ExpeditionDzSafeReturn, location);
	}
}

void Expedition::SetDzSafeReturn(const std::string& zone_name, float x, float y, float z, float heading, bool update_db)
{
	auto zone_id = ZoneID(zone_name.c_str());
	SetDzSafeReturn(zone_id, x, y, z, heading, update_db);
}

void Expedition::SetDzZoneInLocation(float x, float y, float z, float heading, bool update_db)
{
	DynamicZoneLocation location{ 0, x, y, z, heading };

	m_dynamiczone.SetZoneInLocation(location, update_db);

	if (update_db)
	{
		SendWorldDzLocationUpdate(ServerOP_ExpeditionDzZoneIn, location);
	}
}