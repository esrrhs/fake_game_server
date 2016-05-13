#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"

extern lu * g_GateLu;
extern lu * g_DataLu;
extern fake * g_fk;
extern Fproto::ServerNetMsg g_ServerNetMsg;
extern Fproto::ServerNetMsg g_SendServerNetMsg;
extern char g_ServerNetMsgBuff[MAX_SERVER_PACKET_LEN];
extern uint64_t g_HandleGuid;

void on_data_open(lu * l, int connid, luuserdata & userdata);
void on_data_recv_packet(lu * l, int connid, const char * buff, size_t size, luuserdata & userdata);
void on_data_close(lu * l, int connid, luuserdata & userdata, int reason);

void on_gate_open(lu * l, int connid, luuserdata & userdata);
void on_gate_recv_packet(lu * l, int connid, const char * buff, size_t size, luuserdata & userdata);
void on_gate_close(lu * l, int connid, luuserdata & userdata, int reason);

static void send_data_msg(const Fproto::ServerNetMsg & msg)
{
	int ret = msg.Marshal(g_ServerNetMsgBuff, MAX_SERVER_PACKET_LEN);
	if (ret < 0)
	{
		LOG_ERROR("tmp Marshal %d", msg.m_ServerNetMsgPara.m_Type);
		return;
	}

	ret = sendlu(g_DataLu, g_ServerNetMsgBuff, ret);
	if (ret != 0)
	{
		LOG_ERROR("sendlu error %d", ret);
	}
}

static void send_gate_msg(const Fproto::ServerNetMsg & msg)
{
	int ret = msg.Marshal(g_ServerNetMsgBuff, MAX_SERVER_PACKET_LEN);
	if (ret < 0)
	{
		LOG_ERROR("tmp Marshal %d", msg.m_ServerNetMsgPara.m_Type);
		return;
	}

	ret = sendlu(g_GateLu, g_ServerNetMsgBuff, ret);
	if (ret != 0)
	{
		LOG_ERROR("sendlu error %d", ret);
	}
}

enum GS_MALLOC_TYPE
{
	GMT_Human,
};

void * gs_malloc(size_t size, int type);
void gs_free(void * p);

template <typename T>
T * gs_new(int type)
{
	T * p = (T*)gs_malloc(sizeof(T), type);
	new (p) T;
	return p;
}
