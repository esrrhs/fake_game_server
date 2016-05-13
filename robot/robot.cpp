#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "TagNetMsg.h"
#include "robotdef.h"
#include "robotmgr.h"
#include "scriptbind.h"

fake * g_fk = 0;
Fproto::NetMsg g_NetMsg;
Fproto::NetMsg g_SendNetMsg;
char g_NetMsgBuff[MAX_PACKET_LEN];

bool ini(int argc, char *argv[])
{
	SAFE_TEST_RET_VAL(common_ini("robot", argc, argv), false, false);
	SAFE_TEST_RET_VAL(fake_ini(g_fk, g_CConfigLoader.GetConfig().m_STRobot.m_strscript, argc, argv), false, false);
	bind_robot_script(g_fk);

	SAFE_TEST_RET_VAL(robotmgr::ptr()->ini(), false, false);

	return true;
}

void fini(int argc, char *argv[])
{
	delfake(g_fk);
	robotmgr::ptr()->fini();
}

void run(int argc, char *argv[])
{
	while (!g_exit)
	{
		fclock::ptr()->heartbeat();
		robotmgr::ptr()->heartbeat();
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
