/**
 * EQEmulator: Everquest Server Emulator
 * Copyright (C) 2001-2019 EQEmulator Development Team (https://github.com/EQEmu/Server)
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

#ifndef EQEMU_CLIENT_H
#define EQEMU_CLIENT_H

#include "../common/global_define.h"
#include "../common/opcodemgr.h"
#include "../common/random.h"
#include "../common/eq_stream_intf.h"
#include "../common/net/dns.h"
#include "../common/net/daybreak_connection.h"
#include "login_structures.h"
#include <memory>

enum LSClientVersion {
	cv_titanium,
	cv_sod
};

enum LSClientStatus {
	cs_not_sent_session_ready,
	cs_waiting_for_login,
	cs_creating_account,
	cs_failed_to_login,
	cs_logged_in
};

/**
 * Client class, controls a single client and it's connection to the login server
 */
class Client {
public:

	/**
	 * Constructor, sets our connection to c and version to v
	 *
	 * @param c
	 * @param v
	 */
	Client(std::shared_ptr<EQStreamInterface> c, LSClientVersion v);

	/**
	 * Destructor
	 */
	~Client() {}

	/**
	 * Processes the client's connection and does various actions
	 *
	 * @return
	 */
	bool Process();

	/**
	 * Sends our reply to session ready packet
	 *
	 * @param data
	 * @param size
	 */
	void Handle_SessionReady(const char *data, unsigned int size);

	/**
	 * Verifies login and send a reply
	 *
	 * @param data
	 * @param size
	 */
	void Handle_Login(const char *data, unsigned int size);

	/**
	 * Sends a packet to the requested server to see if the client is allowed or not
	 *
	 * @param data
	 */
	void Handle_Play(const char *data);

	/**
	 * Sends a server list packet to the client
	 *
	 * @param seq
	 */
	void SendServerListPacket(uint32 seq);

	/**
	 * Sends the input packet to the client and clears our play response states
	 *
	 * @param outapp
	 */
	void SendPlayResponse(EQApplicationPacket *outapp);

	/**
	 * Generates a random login key for the client during login
	 */
	void GenerateKey();

	/**
	 * Gets the account id of this client
	 *
	 * @return
	 */
	unsigned int GetAccountID() const { return account_id; }

	/**
	 * Gets the loginserver name of this client
	 *
	 * @return
	 */
	std::string GetLoginServerName() const { return loginserver_name; }

	/**
	 * Gets the account name of this client
	 *
	 * @return
	 */
	std::string GetAccountName() const { return account_name; }

	/**
	 * Gets the key generated at login for this client
	 *
	 * @return
	 */
	std::string GetKey() const { return key; }

	/**
	 * Gets the server selected to be played on for this client
	 *
	 * @return
	 */
	unsigned int GetPlayServerID() const { return play_server_id; }

	/**
	 * Gets the play sequence state for this client
	 *
	 * @return
	 */
	unsigned int GetPlaySequence() const { return play_sequence_id; }

	/**
	 * Gets the connection for this client
	 *
	 * @return
	 */
	std::shared_ptr<EQStreamInterface> GetConnection() { return connection; }

	/**
	 * Attempts to create a login account
	 *
	 * @param user
	 * @param pass
	 * @param loginserver
	 */
	void AttemptLoginAccountCreation(const std::string &user, const std::string &pass, const std::string &loginserver);

	/**
	 * Does a failed login
	 */
	void DoFailedLogin();

	/**
	 * Verifies a login hash, will also attempt to update a login hash if needed
	 *
	 * @param user
	 * @param loginserver
	 * @param cred
	 * @param hash
	 * @return
	 */
	bool VerifyLoginHash(
		const std::string &user,
		const std::string &loginserver,
		const std::string &cred,
		const std::string &hash
	);

	void DoSuccessfulLogin(const std::string in_account_name, int db_account_id, const std::string &db_loginserver);
	void CreateLocalAccount(const std::string &user, const std::string &pass);
	void CreateEQEmuAccount(const std::string &in_account_name, const std::string &in_account_password, unsigned int loginserver_account_id);

private:
	EQEmu::Random                      random;
	std::shared_ptr<EQStreamInterface> connection;
	LSClientVersion                    version;
	LSClientStatus                     status;

	std::string  account_name;
	unsigned int account_id;
	std::string  loginserver_name;
	unsigned int play_server_id;
	unsigned int play_sequence_id;
	std::string  key;

	std::unique_ptr<EQ::Net::DaybreakConnectionManager> login_connection_manager;
	std::shared_ptr<EQ::Net::DaybreakConnection>        login_connection;
	LoginLoginRequest_Struct                            llrs;

	std::string stored_user;
	std::string stored_pass;
	void LoginOnNewConnection(std::shared_ptr<EQ::Net::DaybreakConnection> connection);
	void LoginOnStatusChange(
		std::shared_ptr<EQ::Net::DaybreakConnection> conn,
		EQ::Net::DbProtocolStatus from,
		EQ::Net::DbProtocolStatus to
	);
	void LoginOnStatusChangeIgnored(
		std::shared_ptr<EQ::Net::DaybreakConnection> conn,
		EQ::Net::DbProtocolStatus from,
		EQ::Net::DbProtocolStatus to
	);
	void LoginOnPacketRecv(std::shared_ptr<EQ::Net::DaybreakConnection> conn, const EQ::Net::Packet &p);
	void LoginSendSessionReady();
	void LoginSendLogin();
	void LoginProcessLoginResponse(const EQ::Net::Packet &p);
};

#endif

