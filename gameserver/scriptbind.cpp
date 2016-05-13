#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "scriptbind.h"
#include "gamedef.h"
#include "objmgr.h"

void fk_send_to_db(uint64_t guid, int msgid)
{
	g_SendServerNetMsg.m_ServerNetMsgHead.m_Guid = guid;
	g_SendServerNetMsg.m_ServerNetMsgPara.m_Type = msgid;
	send_data_msg(g_SendServerNetMsg);
}

void fk_send_to_gate(uint64_t guid, int msgid)
{
	g_SendServerNetMsg.m_ServerNetMsgHead.m_Guid = guid;
	g_SendServerNetMsg.m_ServerNetMsgPara.m_Type = msgid;
	send_gate_msg(g_SendServerNetMsg);
}

uint64_t fk_get_handle_guid()
{
	return g_HandleGuid;
}

human * fk_newhuman()
{
	return gs_new<human>(GMT_Human);
}

void bind_game_script(fake * fk)
{
	// global
	fkreg(fk, "send_to_db", &fk_send_to_db);
	fkreg(fk, "send_to_gate", &fk_send_to_gate);
	fkreg(fk, "get_handle_guid", &fk_get_handle_guid);

	// human
	fkreg(fk, "newhuman", &fk_newhuman);
	fkreg(fk, "enter_scene", &human::enter_scene);
	fkreg(fk, "ini", &human::ini);
	
}
