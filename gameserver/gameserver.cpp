#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "TagNetMsg.h"
#include "gamedef.h"
#include "scriptbind.h"

lu * g_GateLu = 0;
lu * g_DataLu = 0;
fake * g_fk = 0;
Fproto::ServerNetMsg g_ServerNetMsg;
Fproto::ServerNetMsg g_SendServerNetMsg;
char g_ServerNetMsgBuff[MAX_SERVER_PACKET_LEN];
uint64_t g_HandleGuid = 0;

bool ini(int argc, char *argv[])
{
	SAFE_TEST_RET_VAL(common_ini("game", argc, argv), false, false);
	SAFE_TEST_RET_VAL(fake_ini(g_fk, g_CConfigLoader.GetConfig().m_STGameserver.m_strscript, argc, argv), false, false);
	bind_game_script(g_fk);

	{
		luconfig gateconfig;
		gateconfig.type = lut_tcpclient;
		gateconfig.port = g_CConfigLoader.GetConfig().m_STGateserver.m_iport;
		gateconfig.maxpacketlen = MAX_SERVER_PACKET_LEN;
		gateconfig.isencrypt = false;
		gateconfig.iscompress = false;
		gateconfig.sendbuff = MAX_SERVER_NET_BUFF_LEN;
		gateconfig.recvbuff = MAX_SERVER_NET_BUFF_LEN;
		gateconfig.socket_sendbuff = MAX_SERVER_SOCKET_BUFF_LEN;
		gateconfig.socket_recvbuff = MAX_SERVER_SOCKET_BUFF_LEN;
		gateconfig.cco = on_gate_open;
		gateconfig.ccrp = on_gate_recv_packet;
		gateconfig.ccc = on_gate_close;

		g_GateLu = newlu(&gateconfig);
	}

	{
		luconfig dataconfig;
		dataconfig.type = lut_tcpclient;
		dataconfig.port = g_CConfigLoader.GetConfig().m_STDataserver.m_iport;
		dataconfig.maxpacketlen = MAX_SERVER_PACKET_LEN;
		dataconfig.isencrypt = false;
		dataconfig.iscompress = false;
		dataconfig.sendbuff = MAX_SERVER_NET_BUFF_LEN;
		dataconfig.recvbuff = MAX_SERVER_NET_BUFF_LEN;
		dataconfig.socket_sendbuff = MAX_SERVER_SOCKET_BUFF_LEN;
		dataconfig.socket_recvbuff = MAX_SERVER_SOCKET_BUFF_LEN;
		dataconfig.cco = on_data_open;
		dataconfig.ccrp = on_data_recv_packet;
		dataconfig.ccc = on_data_close;

		g_DataLu = newlu(&dataconfig);
	}

	return true;
}

void fini(int argc, char *argv[])
{
	delfake(g_fk);
	dellu(g_GateLu);
	dellu(g_DataLu);
}

void run(int argc, char *argv[])
{
	while (!g_exit)
	{
		fclock::ptr()->heartbeat();
		ticklu(g_GateLu);
		ticklu(g_DataLu);
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
