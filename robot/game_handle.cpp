#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "robotdef.h"
#include "robotmgr.h"

void on_game_open(lu * l, int connid, luuserdata & userdata)
{
	LOG_SYS("on_game_open %d", connid);

	luuserdata * ludata = getlu_userdata(l);
	robot * r = (robot *)ludata->params[0]._ptr;
	std::string acc = r->get_acc();

	Fproto::NetMsg & tmp = g_NetMsg;
	tmp.m_NetMsgPara.m_Type = Fproto::CS_REQ_LOGIN;
	fstrcopy(tmp.m_NetMsgPara.m_CSReqLogin.m_Acc, acc);

	send_inter_msg(l, tmp);
}

void on_game_recv_packet(lu * l, int connid, const char * buff, size_t size, luuserdata & userdata)
{
	g_isActive = true;

	Fproto::NetMsg & tmp = g_NetMsg;
	int ret = tmp.Unmarshal(buff, size);
	if (ret < 0)
	{
		LOG_ERROR("on_game_recv_packet %d", connid);
		return;
	}

	LOG_DEBUG("on_game_recv_packet %d %s", connid, Fproto::GetMsgIDName(tmp.m_NetMsgPara.m_Type));

	luuserdata * ludata = getlu_userdata(l);
	robot * r = (robot *)ludata->params[0]._ptr;

	g_SendNetMsg.Clear();
	const char * funcname = Fproto::GetMsgIDFuncName(tmp.m_NetMsgPara.m_Type);
	fkrun<void>(g_fk, funcname, r, g_NetMsg.GetNetMsgPara(), g_SendNetMsg.GetNetMsgPara());
}

void on_game_close(lu * l, int connid, luuserdata & userdata, int reason)
{
	LOG_ERROR("on_game_close %d %d", connid, reason);
}
