#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"

extern fake * g_fk;
extern Fproto::NetMsg g_NetMsg;
extern Fproto::NetMsg g_SendNetMsg;
extern char g_NetMsgBuff[MAX_PACKET_LEN];

void on_game_open(lu * l, int connid, luuserdata & userdata);
void on_game_recv_packet(lu * l, int connid, const char * buff, size_t size, luuserdata & userdata);
void on_game_close(lu * l, int connid, luuserdata & userdata, int reason);

static void send_inter_msg(lu * l, const Fproto::NetMsg & msg)
{
	char tmpbuff[MAX_PACKET_LEN];
	int ret = msg.Marshal(tmpbuff, MAX_PACKET_LEN);
	if (ret < 0)
	{
		LOG_ERROR("tmp Marshal %d", msg.m_NetMsgPara.m_Type);
		return;
	}

	ret = sendlu(l, tmpbuff, ret);
	if (ret != 0)
	{
		LOG_ERROR("sendlu error %d", ret);
	}
}
