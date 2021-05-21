#include "shared_task_manager.h"
#include "../common/repositories/character_data_repository.h"
#include "../common/repositories/task_activities_repository.h"
#include "cliententry.h"
#include "clientlist.h"
#include "zonelist.h"
#include "zoneserver.h"
#include "shared_task_world_messaging.h"

extern ClientList client_list;
extern ZSList     zoneserver_list;

SharedTaskManager *SharedTaskManager::SetDatabase(Database *db)
{
	SharedTaskManager::m_database = db;

	return this;
}

SharedTaskManager *SharedTaskManager::SetContentDatabase(Database *db)
{
	SharedTaskManager::m_content_database = db;

	return this;
}

Database *SharedTaskManager::GetDatabase() const
{
	return m_database;
}

std::vector<SharedTaskMember> SharedTaskManager::GetRequestMembers(uint32 requestor_character_id)
{
	std::vector<SharedTaskMember> request_members = {};

	// raid
	auto raid_characters = CharacterDataRepository::GetWhere(
		*GetDatabase(),
		fmt::format(
			"id IN (select charid from raid_members where raidid = (select raidid from raid_members where charid = {}))",
			requestor_character_id
		)
	);

	if (!raid_characters.empty()) {
		request_members.reserve(raid_characters.size());
		for (auto &c: raid_characters) {
			SharedTaskMember member = {};
			member.character_id   = c.id;
			member.character_name = c.name;
			member.is_raided      = true;
			member.level          = c.level;

			request_members.emplace_back(member);
		}

		return request_members;
	}

	// group
	auto group_characters = CharacterDataRepository::GetWhere(
		*GetDatabase(),
		fmt::format(
			"id IN (select charid from group_id where groupid = (select groupid from group_id where charid = {}))",
			requestor_character_id
		)
	);

	if (!group_characters.empty()) {
		request_members.reserve(request_members.size());
		for (auto &c: group_characters) {
			SharedTaskMember member = {};
			member.character_id   = c.id;
			member.character_name = c.name;
			member.is_grouped     = true;
			member.level          = c.level;

			request_members.emplace_back(member);
		}
	}

	// if we didn't pull the requested character from the db, let's pull it by now
	// most shared tasks are not going to be single character / solo for us to typically get here
	// maybe we're a GM testing a shared task solo
	bool list_has_leader = false;
	for (auto &m: request_members) {
		if (m.character_id == requestor_character_id) {
			list_has_leader = true;
		}
	}

	if (!list_has_leader) {
		auto leader = CharacterDataRepository::FindOne(*GetDatabase(), requestor_character_id);
		if (leader.id != 0) {
			SharedTaskMember member = {};
			member.character_id   = leader.id;
			member.character_name = leader.name;
			member.is_grouped     = false;
			member.level          = leader.level;

			request_members.emplace_back(member);
		}
	}

	return request_members;
}

void SharedTaskManager::AttemptSharedTaskCreation(uint32 requested_task_id, uint32 requested_character_id)
{
	auto task = TasksRepository::FindOne(*m_content_database, requested_task_id);
	if (task.id != 0 && task.type == TASK_TYPE_SHARED) {
		LogTasksDetail(
			"[AttemptSharedTaskCreation] Found Shared Task ({}) [{}]",
			requested_task_id,
			task.title
		);
	}

	auto request_members = GetRequestMembers(requested_character_id);
	if (!request_members.empty()) {
		for (auto &member: request_members) {
			LogTasksDetail(
				"[AttemptSharedTaskCreation] Request Members ({}) [{}] level [{}] grouped [{}] raided [{}]",
				member.character_id,
				member.character_name,
				member.level,
				(member.is_grouped ? "true" : "false"),
				(member.is_raided ? "true" : "false")
			);
		}
	}

	if (request_members.empty()) {
		LogTasksDetail("[AttemptSharedTaskCreation] No additional request members found... Just leader");
	}

	// confirm shared task request
	auto p = std::make_unique<ServerPacket>(
		ServerOP_SharedTaskAcceptNewTask,
		sizeof(ServerSharedTaskRequest_Struct)
	);
	auto d = reinterpret_cast<ServerSharedTaskRequest_Struct *>(p->pBuffer);
	d->requested_character_id = requested_character_id;
	d->requested_task_id      = requested_task_id;

	// get requested character zone server
	ClientListEntry *requested_character_cle = client_list.FindCLEByCharacterID(d->requested_character_id);
	if (requested_character_cle && requested_character_cle->Server()) {
		requested_character_cle->Server()->SendPacket(p.get());
	}

	// new shared task
	auto new_shared_task = SharedTask{};
	auto activities      = TaskActivitiesRepository::GetWhere(*m_content_database, fmt::format("taskid = {}", task.id));

	// activity state
	std::vector<SharedTaskActivityStateEntry> shared_task_activity_state = {};
	shared_task_activity_state.reserve(activities.size());
	for (auto &activity: activities) {

		// entry
		auto shared_task_activity_state_entry = SharedTaskActivityStateEntry{};
		shared_task_activity_state_entry.activity_id    = activity.activityid;
		shared_task_activity_state_entry.done_count     = 0;
		shared_task_activity_state_entry.max_done_count = activity.goalcount;

		shared_task_activity_state.emplace_back(shared_task_activity_state_entry);
	}

	new_shared_task.SetSharedTaskActivityState(shared_task_activity_state);

	// add to shared tasks list
	m_shared_tasks.emplace_back(new_shared_task);

	LogTasks(
		"[AttemptSharedTaskCreation] Task [{}] created successfully | member_count [{}] activity_count [{}] current tasks in state [{}]",
		task.id,
		request_members.size(),
		shared_task_activity_state.size(),
		m_shared_tasks.size()
	);
}
