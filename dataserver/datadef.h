#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "mysql.h"

extern lu * g_GameLu;
extern fake * g_fk;
extern Fproto::ServerNetMsg g_ServerNetMsg;
extern Fproto::ServerNetMsg g_SendServerNetMsg;
extern char g_ServerNetMsgBuff[MAX_SERVER_PACKET_LEN];
extern int g_GameConnid;
extern uint64_t g_GuidCounter;

void on_game_open(lu * l, int connid, luuserdata & userdata);
void on_game_recv_packet(lu * l, int connid, const char * buff, size_t size, luuserdata & userdata);
void on_game_close(lu * l, int connid, luuserdata & userdata, int reason);

static void send_game_msg(const Fproto::ServerNetMsg & msg)
{
	int ret = msg.Marshal(g_ServerNetMsgBuff, MAX_SERVER_PACKET_LEN);
	if (ret < 0)
	{
		LOG_ERROR("tmp Marshal %d", msg.m_ServerNetMsgPara.m_Type);
		return;
	}

	ret = sendlu(g_GameLu, g_ServerNetMsgBuff, ret, g_GameConnid);
	if (ret != 0)
	{
		LOG_ERROR("sendlu error %d", ret);
	}
}

static bool ini_db(MYSQL * mysql)
{
	mysql_init(mysql);

	if (mysql_options(mysql, MYSQL_OPT_COMPRESS, 0) != 0)
	{
		LOG_ERROR("mysql_options MYSQL_OPT_RECONNECT %s", mysql_error(mysql));
		return false;
	}

	char value = 1;
	if (mysql_options(mysql, MYSQL_OPT_RECONNECT, (char *)&value) != 0)
	{
		LOG_ERROR("mysql_options MYSQL_OPT_RECONNECT %s", mysql_error(mysql));
		return false;
	}

	if (!mysql_real_connect(mysql, g_CConfigLoader.GetConfig().m_STDataserver.m_STMysql.m_strhost.c_str(),
		g_CConfigLoader.GetConfig().m_STDataserver.m_STMysql.m_struser.c_str(),
		g_CConfigLoader.GetConfig().m_STDataserver.m_STMysql.m_strpwd.c_str(),
		g_CConfigLoader.GetConfig().m_STDataserver.m_STMysql.m_strdb.c_str(),
		g_CConfigLoader.GetConfig().m_STDataserver.m_STMysql.m_iport,
		NULL, 0))
	{
		LOG_ERROR("Couldn't connect to mysql!\n%s", mysql_error(mysql));
		return false;
	}

	if (mysql_set_character_set(mysql, "utf8") != 0)
	{
		LOG_ERROR("client character %s", mysql_error(mysql));
		return false;
	}

	LOG_SYS("mysql ini db ok %s %s %s %s %d", g_CConfigLoader.GetConfig().m_STDataserver.m_STMysql.m_strhost.c_str(),
		g_CConfigLoader.GetConfig().m_STDataserver.m_STMysql.m_struser.c_str(),
		g_CConfigLoader.GetConfig().m_STDataserver.m_STMysql.m_strpwd.c_str(),
		g_CConfigLoader.GetConfig().m_STDataserver.m_STMysql.m_strdb.c_str(),
		g_CConfigLoader.GetConfig().m_STDataserver.m_STMysql.m_iport);

	return true;
}
