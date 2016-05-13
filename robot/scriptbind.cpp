#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "scriptbind.h"
#include "robotdef.h"
#include "robotmgr.h"

void fk_send_to_game(robot * r, int msgid)
{
	g_SendNetMsg.m_NetMsgPara.m_Type = msgid;
	send_inter_msg(r->get_lu(), g_SendNetMsg);
}

void bind_robot_script(fake * fk)
{
	fkreg(fk, "get_acc", &robot::get_acc);
	fkreg(fk, "set_acc", &robot::set_acc);
	fkreg(fk, "get_guid", &robot::get_guid);
	fkreg(fk, "set_guid", &robot::set_guid);
	fkreg(fk, "get_name", &robot::get_name);
	fkreg(fk, "set_name", &robot::set_name);

	fkreg(fk, "send_to_game", fk_send_to_game);
}
