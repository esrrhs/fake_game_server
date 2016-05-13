#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "datadef.h"
#include "dbthreadmgr.h"

void on_game_open(lu * l, int connid, luuserdata & userdata)
{
	LOG_SYS("on_game_open %d", connid);
	g_GameConnid = connid;
}

void on_game_recv_packet(lu * l, int connid, const char * buff, size_t size, luuserdata & userdata)
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

	LOG_DEBUG("on_game_recv_packet %d %s %llu", tmp.m_ServerNetMsgPara.m_Type, Fproto::GetServerMsgIDName(tmp.m_ServerNetMsgPara.m_Type), guid);

	dbthreadmgr::ptr()->push_command(tmp);
}

void on_game_close(lu * l, int connid, luuserdata & userdata, int reason)
{
	LOG_ERROR("on_game_close %d %d", connid, reason);
	g_GameConnid = -1;
}
