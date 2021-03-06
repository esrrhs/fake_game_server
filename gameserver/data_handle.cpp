#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "gamedef.h"
#include "objmgr.h"

void on_data_open(lu * l, int connid, luuserdata & userdata)
{
	LOG_SYS("on_data_open %d", connid);
}

void on_data_recv_packet(lu * l, int connid, const char * buff, size_t size, luuserdata & userdata)
{
	g_isActive = true;

	Fproto::ServerNetMsg & tmp = g_ServerNetMsg;
	int ret = tmp.Unmarshal(buff, size);
	if (ret < 0)
	{
		LOG_ERROR("on_client_recv_packet %d", connid);
		return;
	}

	uint64_t guid = tmp.m_ServerNetMsgHead.m_Guid;
	human * pHuman = objmgr::ptr()->get_human(guid);
	g_HandleGuid = guid;

	LOG_DEBUG("on_data_recv_packet %d %s %llu", tmp.m_ServerNetMsgPara.m_Type, Fproto::GetServerMsgIDName(tmp.m_ServerNetMsgPara.m_Type), guid);

	g_SendServerNetMsg.Clear();
	const char * funcname = Fproto::GetServerMsgIDFuncName(tmp.m_ServerNetMsgPara.m_Type);
	fkrun<void>(g_fk, funcname, pHuman, g_ServerNetMsg.GetServerNetMsgPara(), g_SendServerNetMsg.GetServerNetMsgPara());

	g_HandleGuid = 0;
}

void on_data_close(lu * l, int connid, luuserdata & userdata, int reason)
{
	LOG_ERROR("on_data_close %d %d", connid, reason);
}
