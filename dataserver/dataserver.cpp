#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "TagNetMsg.h"
#include "datadef.h"
#include "dbthreadmgr.h"
#include "scriptbind.h"
#include "DBNetMsg.h"

lu * g_GameLu = 0;
fake * g_fk = 0;
Fproto::ServerNetMsg g_ServerNetMsg;
Fproto::ServerNetMsg g_SendServerNetMsg;
char g_ServerNetMsgBuff[MAX_SERVER_PACKET_LEN];
int g_GameConnid = -1;
uint64_t g_GuidCounter;

bool ini(int argc, char *argv[])
{
	SAFE_TEST_RET_VAL(common_ini("data", argc, argv), false, false);
	SAFE_TEST_RET_VAL(fake_ini(g_fk, g_CConfigLoader.GetConfig().m_STDataserver.m_strscript, argc, argv), false, false);
	bind_data_script(g_fk);
	LOG_SYS("%s", Fproto::create_all_table().c_str());
	SAFE_TEST_RET_VAL(dbthreadmgr::ptr()->ini(), false, false);
	
	{
		luconfig gameconfig;
		gameconfig.type = lut_tcpserver;
		fstrcopy(gameconfig.ip, g_CConfigLoader.GetConfig().m_STDataserver.m_strip);
		gameconfig.port = g_CConfigLoader.GetConfig().m_STDataserver.m_iport;
		gameconfig.maxpacketlen = MAX_SERVER_PACKET_LEN;
		gameconfig.maxconnnum = 1;
		gameconfig.isencrypt = false;
		gameconfig.iscompress = false;
		gameconfig.sendbuff = MAX_SERVER_NET_BUFF_LEN;
		gameconfig.recvbuff = MAX_SERVER_NET_BUFF_LEN;
		gameconfig.socket_sendbuff = MAX_SERVER_SOCKET_BUFF_LEN;
		gameconfig.socket_recvbuff = MAX_SERVER_SOCKET_BUFF_LEN;
		gameconfig.cco = on_game_open;
		gameconfig.ccrp = on_game_recv_packet;
		gameconfig.ccc = on_game_close;

		g_GameLu = newlu(&gameconfig);
	}

	dbthreadmgr::ptr()->start();

	return true;
}

void fini(int argc, char *argv[])
{
	delfake(g_fk);
	dellu(g_GameLu);
	dbthreadmgr::ptr()->stop();
}

void run(int argc, char *argv[])
{
	while (!g_exit)
	{
		fclock::ptr()->heartbeat();
		ticklu(g_GameLu);
		dbthreadmgr::ptr()->heartbeat();
		if (!g_isActive)
		{
			fsleep(1);
		}
	}
}

int main(int argc, char *argv[])
{
	if (!ini(argc, argv))
	{
		return false;
	}

	run(argc, argv);

	fini(argc, argv);

	return 0;
}
