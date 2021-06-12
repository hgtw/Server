#ifndef EQEMU_SHARED_TASK_MANAGER_H
#define EQEMU_SHARED_TASK_MANAGER_H

#include "../common/database.h"
#include "../common/shared_tasks.h"

class SharedTaskManager {
public:
	SharedTaskManager *SetDatabase(Database *db);
	SharedTaskManager *SetContentDatabase(Database *db);

	// loads task data into memory
	SharedTaskManager * LoadTaskData();

	// loads shared task state into memory
	void LoadSharedTaskState();

	// helper, references task memory data
	TasksRepository::Tasks GetSharedTaskDataByTaskId(uint32 task_id);
	std::vector<TaskActivitiesRepository::TaskActivities> GetSharedTaskActivityDataByTaskId(uint32 task_id);

	// gets group / raid members belonging to requested character
	// this may change later depending on how shared tasks develop
	std::vector<SharedTaskMember> GetRequestMembers(uint32 requestor_character_id);

	// client attempting to create a shared task
	void AttemptSharedTaskCreation(uint32 requested_task_id, uint32 requested_character_id, uint32 npc_type_id);
	void AttemptSharedTaskRemoval(uint32 requested_task_id, uint32 requested_character_id, bool remove_from_db);

	// shared task activity update middleware
	void SharedTaskActivityUpdate(
		uint32 source_character_id,
		uint32 task_id,
		uint32 activity_id,
		uint32 done_count,
		bool ignore_quest_update
	);

	SharedTask * FindSharedTaskByTaskIdAndCharacterId(uint32 task_id, uint32 character_id);

	void DeleteSharedTask(int64 shared_task_id, uint32 requested_character_id);
	void SaveSharedTaskActivityState(int64 shared_task_id, std::vector<SharedTaskActivityStateEntry> activity_state);

	bool IsSharedTaskLeader(SharedTask *s, uint32 character_id);
	void SendAcceptNewSharedTaskPacket(uint32 character_id, uint32 task_id, uint32 npc_type_id);
	void SendRemovePlayerFromSharedTaskPacket(uint32 character_id, uint32 task_id, bool remove_from_db);
	void SendSharedTaskMemberList(uint32 character_id, int64 shared_task_id);
	void RemovePlayerFromSharedTask(SharedTask *s, uint32 character_id);
	void PrintSharedTaskState();
	void RemovePlayerFromSharedTaskByPlayerName(SharedTask *s, const std::string& character_name);

	void MakeLeaderByPlayerName(SharedTask *s, const std::string& character_name);
	void AddPlayerByPlayerName(SharedTask *s, const std::string& character_name);
protected:
	// reference to database
	Database *m_database;
	Database *m_content_database;

	// reference to task data (all)
	std::vector<TasksRepository::Tasks> m_task_data;
	std::vector<TaskActivitiesRepository::TaskActivities> m_task_activity_data;

	// internal shared tasks list
	std::vector<SharedTask> m_shared_tasks;

	void SendSharedTaskMemberListToAllMembers(SharedTask *s);
	void SaveMembers(SharedTask *s, std::vector<SharedTaskMember> members);
};

#endif //EQEMU_SHARED_TASK_MANAGER_H
