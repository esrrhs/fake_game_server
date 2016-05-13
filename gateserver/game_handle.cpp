#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "gatedef.h"
#include "connmgr.h"

void on_game_open(lu * l, int connid, luuserdata & userdata)
{
	LOG_SYS("on_game_open %d", connid);
	g_GameConnid = connid;
}

void process_gg_msg()
{
	switch (g_ServerNetMsg.m_ServerNetMsgPara.m_Type)
	{
	case Fproto::GG_REQ_UPDATE_GUID:
		{
			Fproto::GGReqUpdateGuid & msg = g_ServerNetMsg.m_ServerNetMsgPara.m_GGReqUpdateGuid;
			conndata * p = connmgr::ptr()->get_conn(msg.m_OldGuid);
			if (p)
			{
				conndata tmp = *p;
				connmgr::ptr()->del_conn(msg.m_OldGuid);
				tmp.guid = msg.m_NewGuid;
				connmgr::ptr()->add_conn(tmp);

				luuserdata * ludata = getlu_conn_userdata(g_ClientLu, tmp.connid);
				if (ludata)
				{
					if (ludata->params[0].u64 != msg.m_OldGuid)
					{
						LOG_ERROR("GG_REQ_UPDATE_GUID diff conn guid %llu %llu %llu", msg.m_OldGuid, ludata->params[0].u64, msg.m_NewGuid);
					}
					else
					{
						ludata->params[0].u64 = msg.m_NewGuid;
					}
				}
				LOG_SYS("GG_REQ_UPDATE_GUID %llu %llu", msg.m_OldGuid, msg.m_NewGuid);
			}
		}
		break;
	default:
		break;
	}
}

void on_game_recv_packet(lu * l, int connid, const char * buff, size_t size, luuserdata & userdata)
{
	g_isActive = true;

	Fproto::ServerNetMsg & tmp = g_ServerNetMsg;
	int ret = tmp.Unmarshal(buff, size);
	if (ret < 0)
	{
		LOG_ERROR("on_game_recv_packet Unmarshal error %d", connid);
		return;
	}

	uint64_t guid = tmp.m_ServerNetMsgHead.m_Guid;
	conndata * pcdata = connmgr::ptr()->get_conn(guid);
	if (!pcdata)
	{
		LOG_ERROR("on_game_recv_packet get_conn fail %d %llu", connid, guid);
		return;
	}
	conndata & cdata = *pcdata;

	LOG_DEBUG("on_game_recv_packet %d %s %llu", cdata.connid, Fproto::GetServerMsgIDName(tmp.m_ServerNetMsgPara.m_Type), guid);

	if (tmp.m_ServerNetMsgPara.m_Type > Fproto::GG_MSG_START)
	{
		process_gg_msg();
		return;
	}

	Fproto::NetMsg & tmpex = g_NetMsg;
	memcpy(&tmpex.m_NetMsgPara, &tmp.m_ServerNetMsgPara, sizeof(tmpex.m_NetMsgPara));

	char * tmpbuff = g_NetMsgBuff;
	ret = tmpex.Marshal(tmpbuff, MAX_PACKET_LEN);
	if (ret < 0)
	{
		LOG_ERROR("tmpex Marshal %d", connid);
		return;
	}

	ret = sendlu(g_ClientLu, tmpbuff, ret, cdata.connid);
	if (ret != 0)
	{
		LOG_ERROR("sendlu error %d", ret);
	}
}

void on_game_close(lu * l, int connid, luuserdata & userdata, int reason)
{
	LOG_ERROR("on_game_close %d %d", connid, reason);
	g_GameConnid = -1;
}
