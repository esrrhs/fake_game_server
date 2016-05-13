#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "TagNetMsg.h"
#include "gatedef.h"

lu * g_ClientLu = 0;
lu * g_GameLu = 0;
fake * g_fk = 0;
uint64_t g_GuidCounter = 0;
Fproto::NetMsg g_NetMsg;
Fproto::ServerNetMsg g_ServerNetMsg;
char g_ServerNetMsgBuff[MAX_SERVER_PACKET_LEN];
char g_NetMsgBuff[MAX_PACKET_LEN];
int g_GameConnid = -1;

bool ini(int argc, char *argv[])
{
	SAFE_TEST_RET_VAL(common_ini("gate", argc, argv), false, false);
	SAFE_TEST_RET_VAL(fake_ini(g_fk, g_CConfigLoader.GetConfig().m_STGateserver.m_strscript, argc, argv), false, false);

	{
		luconfig gameconfig;
		gameconfig.type = lut_tcpserver;
		fstrcopy(gameconfig.ip, g_CConfigLoader.GetConfig().m_STGateserver.m_strip);
		gameconfig.port = g_CConfigLoader.GetConfig().m_STGateserver.m_iport;
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

	{
		luconfig clientconfig;
		clientconfig.type = lut_tcpserver;
		fstrcopy(clientconfig.ip, g_CConfigLoader.GetConfig().m_STGateserver.m_stroutip);
		clientconfig.port = g_CConfigLoader.GetConfig().m_STGateserver.m_ioutport;
		clientconfig.maxpacketlen = MAX_PACKET_LEN;
		clientconfig.maxconnnum = MAX_CONN_NUM;
		clientconfig.isencrypt = false;
		clientconfig.iscompress = false;
		clientconfig.sendbuff = MAX_NET_BUFF_LEN;
		clientconfig.recvbuff = MAX_NET_BUFF_LEN;
		clientconfig.socket_sendbuff = MAX_SOCKET_BUFF_LEN;
		clientconfig.socket_recvbuff = MAX_SOCKET_BUFF_LEN;
		clientconfig.cco = on_client_open;
		clientconfig.ccrp = on_client_recv_packet;
		clientconfig.ccc = on_client_close;

		g_ClientLu = newlu(&clientconfig);
	}

	return true;
}

void fini(int argc, char *argv[])
{
	delfake(g_fk);
	dellu(g_GameLu);
	dellu(g_ClientLu);
}

void run(int argc, char *argv[])
{
	while (!g_exit)
	{
		g_isActive = false;
		fclock::ptr()->heartbeat();
		ticklu(g_GameLu);
		ticklu(g_ClientLu);
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
