#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"

extern lu * g_ClientLu;
extern lu * g_GameLu;
extern fake * g_fk;
extern uint64_t g_GuidCounter;
extern Fproto::NetMsg g_NetMsg;
extern Fproto::ServerNetMsg g_ServerNetMsg;
extern char g_ServerNetMsgBuff[MAX_SERVER_PACKET_LEN];
extern char g_NetMsgBuff[MAX_PACKET_LEN];
extern int g_GameConnid;

void on_game_open(lu * l, int connid, luuserdata & userdata);
void on_game_recv_packet(lu * l, int connid, const char * buff, size_t size, luuserdata & userdata);
void on_game_close(lu * l, int connid, luuserdata & userdata, int reason);

void on_client_open(lu * l, int connid, luuserdata & userdata);
void on_client_recv_packet(lu * l, int connid, const char * buff, size_t size, luuserdata & userdata);
void on_client_close(lu * l, int connid, luuserdata & userdata, int reason);

#define MAX_CONN_NUM (3000)
