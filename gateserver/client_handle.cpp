#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "connmgr.h"
#include "gatedef.h"

void on_client_open(lu * l, int connid, luuserdata & userdata)
{
	LOG_SYS("on_client_open %d", connid);
	conndata tmp;
	tmp.clear();
	tmp.connid = connid;
	tmp.guid = make_guid(g_CConfigLoader.GetConfig().m_STCommon.m_igroupid, ST_Gateserver, g_GuidCounter);
	userdata.params[0].u64 = tmp.guid;
	connmgr::ptr()->add_conn(tmp);
}

void on_client_recv_packet(lu * l, int connid, const char * buff, size_t size, luuserdata & userdata)
{
	g_isActive = true;

	Fproto::NetMsg & tmp = g_NetMsg;
	int ret = tmp.Unmarshal(buff, size);
	if (ret < 0)
	{
		LOG_ERROR("on_client_recv_packet Unmarshal error %d", connid);
		return;
	}

	uint64_t guid = userdata.params[0].u64;

	LOG_DEBUG("on_client_recv_packet %d %s %llu", connid, Fproto::GetMsgIDName(tmp.m_NetMsgPara.m_Type), guid);

	Fproto::ServerNetMsg & tmpex = g_ServerNetMsg;
	tmpex.m_ServerNetMsgHead.m_Guid = guid;
	memcpy(&tmpex.m_ServerNetMsgPara, &tmp.m_NetMsgPara, sizeof(tmp.m_NetMsgPara));

	char * tmpbuff = g_NetMsgBuff;
	ret = tmpex.Marshal(tmpbuff, MAX_SERVER_PACKET_LEN);
	if (ret < 0)
	{
		LOG_ERROR("tmpex Marshal %d", connid);
		return;
	}

	ret = sendlu(g_GameLu, tmpbuff, ret, g_GameConnid);
	if (ret != 0)
	{
		LOG_ERROR("sendlu error %d", ret);
	}
}

void on_client_close(lu * l, int connid, luuserdata & userdata, int reason)
{
	LOG_ERROR("on_client_close %d %d", connid, reason);
	connmgr::ptr()->del_conn(userdata.params[0].u64);
}
