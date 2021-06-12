#include "shared_task_world_messaging.h"
#include "cliententry.h"
#include "worlddb.h"
#include "../common/shared_tasks.h"
#include "../common/eqemu_logsys.h"
#include "../common/repositories/tasks_repository.h"
#include "../common/tasks.h"
#include "cliententry.h"
#include "clientlist.h"
#include "zonelist.h"
#include "zoneserver.h"
#include "shared_task_manager.h"
#include "../common/repositories/task_activities_repository.h"
#include "dynamic_zone.h"
#include "dynamic_zone_manager.h"

extern ClientList        client_list;
extern ZSList            zoneserver_list;
extern SharedTaskManager shared_task_manager;

void SharedTaskWorldMessaging::HandleZoneMessage(ServerPacket *pack)
{
	switch (pack->opcode) {
		case ServerOP_SharedTaskRequest: {
			auto *r = (ServerSharedTaskRequest_Struct *) pack->pBuffer;
			LogTasksDetail(
				"[ServerOP_SharedTaskRequest] Received request from character [{}] task_id [{}] npc_type_id [{}]",
				r->requested_character_id,
				r->requested_task_id,
				r->requested_npc_type_id
			);

			shared_task_manager.AttemptSharedTaskCreation(r->requested_task_id, r->requested_character_id, r->requested_npc_type_id);

			break;
		}
		case ServerOP_SharedTaskAttemptRemove: {
			auto *r = (ServerSharedTaskAttemptRemove_Struct *) pack->pBuffer;
			LogTasksDetail(
				"[ServerOP_SharedTaskAttemptRemove] Received request from character [{}] task_id [{}] remove_from_db [{}]",
				r->requested_character_id,
				r->requested_task_id,
				r->remove_from_db
			);

			shared_task_manager.AttemptSharedTaskRemoval(
				r->requested_task_id,
				r->requested_character_id,
				r->remove_from_db
			);

			break;
		}
		case ServerOP_SharedTaskUpdate: {
			auto *r = (ServerSharedTaskActivityUpdate_Struct *) pack->pBuffer;

			LogTasksDetail(
				"[ServerOP_SharedTaskUpdate] Received request from character [{}] task_id [{}] activity_id [{}] donecount [{}] ignore_quest_update [{}]",
				r->source_character_id,
				r->task_id,
				r->activity_id,
				r->done_count,
				(r->ignore_quest_update ? "true" : "false")
			);

			shared_task_manager.SharedTaskActivityUpdate(
				r->source_character_id,
				r->task_id,
				r->activity_id,
				r->done_count,
				r->ignore_quest_update
			);

			break;
		}
		case ServerOP_SharedTaskRequestMemberlist: {
			auto *r = (ServerSharedTaskRequestMemberlist_Struct *) pack->pBuffer;

			LogTasksDetail(
				"[ServerOP_SharedTaskRequestMemberlist] Received request from character [{}] task_id [{}]",
				r->source_character_id,
				r->task_id
			);

			auto t = shared_task_manager.FindSharedTaskByTaskIdAndCharacterId(r->task_id, r->source_character_id);
			if (t) {
				LogTasksDetail(
					"[ServerOP_SharedTaskRequestMemberlist] Found shared task character [{}] shared_task_id [{}]",
					r->source_character_id,
					t->GetDbSharedTask().id
				);

				shared_task_manager.SendSharedTaskMemberList(
					r->source_character_id,
					t->GetDbSharedTask().id
				);
			}

			break;
		}
		case ServerOP_SharedTaskRemovePlayer: {
			auto *r = (ServerSharedTaskRemovePlayer_Struct *) pack->pBuffer;

			LogTasksDetail(
				"[ServerOP_SharedTaskRemovePlayer] Received request from character [{}] task_id [{}] player_name [{}]",
				r->source_character_id,
				r->task_id,
				r->player_name
			);

			auto t = shared_task_manager.FindSharedTaskByTaskIdAndCharacterId(r->task_id, r->source_character_id);
			if (t) {
				LogTasksDetail(
					"[ServerOP_SharedTaskRemovePlayer] Found shared task character [{}] shared_task_id [{}]",
					r->source_character_id,
					t->GetDbSharedTask().id
				);

				if (shared_task_manager.IsSharedTaskLeader(t, r->source_character_id)) {
					LogTasksDetail(
						"[ServerOP_SharedTaskRemovePlayer] character_id [{}] shared_task_id [{}] is_leader",
						r->source_character_id,
						t->GetDbSharedTask().id
					);

					// TODO: Clean this up a bit more later as other functionality is A-Z'ed
					std::string character_name = r->player_name;
					shared_task_manager.RemovePlayerFromSharedTaskByPlayerName(t, character_name);
				}
			}

			break;
		}
		case ServerOP_SharedTaskMakeLeader: {
			auto *r = (ServerSharedTaskMakeLeader_Struct *) pack->pBuffer;

			LogTasksDetail(
				"[ServerOP_SharedTaskMakeLeader] Received request from character [{}] task_id [{}] player_name [{}]",
				r->source_character_id,
				r->task_id,
				r->player_name
			);

			auto t = shared_task_manager.FindSharedTaskByTaskIdAndCharacterId(r->task_id, r->source_character_id);
			if (t) {
				LogTasksDetail(
					"[ServerOP_SharedTaskMakeLeader] Found shared task character [{}] shared_task_id [{}]",
					r->source_character_id,
					t->GetDbSharedTask().id
				);

				if (shared_task_manager.IsSharedTaskLeader(t, r->source_character_id)) {
					LogTasksDetail(
						"[ServerOP_SharedTaskMakeLeader] character_id [{}] shared_task_id [{}] is_leader",
						r->source_character_id,
						t->GetDbSharedTask().id
					);

					std::string character_name = r->player_name;
					shared_task_manager.MakeLeaderByPlayerName(t, character_name);
				}
			}

			break;
		}
		case ServerOP_SharedTaskAddPlayer: {
			auto *r = (ServerSharedTaskAddPlayer_Struct *) pack->pBuffer;

			LogTasksDetail(
				"[ServerOP_SharedTaskAddPlayer] Received request from character [{}] task_id [{}] player_name [{}]",
				r->source_character_id,
				r->task_id,
				r->player_name
			);

			auto t = shared_task_manager.FindSharedTaskByTaskIdAndCharacterId(r->task_id, r->source_character_id);
			if (t) {
				LogTasksDetail(
					"[ServerOP_SharedTaskAddPlayer] Found shared task character [{}] shared_task_id [{}]",
					r->source_character_id,
					t->GetDbSharedTask().id
				);

				if (shared_task_manager.IsSharedTaskLeader(t, r->source_character_id)) {
					LogTasksDetail(
						"[ServerOP_SharedTaskAddPlayer] character_id [{}] shared_task_id [{}] is_leader",
						r->source_character_id,
						t->GetDbSharedTask().id
					);

					std::string character_name = r->player_name;
					shared_task_manager.AddPlayerByPlayerName(t, character_name);
				}
			}

			break;
		}
		case ServerOP_SharedTaskCreateDynamicZone: {
			auto buf = reinterpret_cast<ServerSharedTaskCreateDynamicZone_Struct*>(pack->pBuffer);

			LogTasksDetail(
				"[ServerOP_SharedTaskCreateDynamicZone] Received dynamic zone creation request from character [{}] task_id [{}]",
				buf->source_character_id,
				buf->task_id
			);

			auto t = shared_task_manager.FindSharedTaskByTaskIdAndCharacterId(buf->task_id, buf->source_character_id);
			if (t)
			{
				DynamicZone dz;
				dz.LoadSerializedDzPacket(buf->cereal_data, buf->cereal_size);

				// these aren't strictly necessary since live's window behavior isn't implemented
				// todo: dz name should be version-based zone name (Thundercrest Isles: The Creator)
				// todo: dz.SetName(t->GetTaskData().title);
				// todo: dz.SetMinPlayers(t->GetTaskData().min_players);
				// todo: dz.SetMaxPlayers(t->GetTaskData().max_players);

				std::vector<DynamicZoneMember> dz_members;
				for (const auto& member : t->GetMembers())
				{
					dz_members.emplace_back(member.character_id, member.character_name);
					if (member.is_leader)
					{
						dz.SetLeader({ member.character_id, member.character_name });
					}
				}

				auto new_dz = dynamic_zone_manager.CreateNew(dz, dz_members);
				if (new_dz)
				{
					// todo: add dz id to shared tasks db
					LogTasks("Created task dz id: [{}]", new_dz->GetID());
					t->dynamic_zone_ids.emplace_back(new_dz->GetID());
				}
			}
			break;
		}
		default:
			break;
	}
}

