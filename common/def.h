#pragma once

#include "configloader.h"

enum ServerType
{
	ST_Gameserver,
	ST_Dataserver,
	ST_Gateserver,
};

#define MAX_SERVER_PACKET_LEN (1024 * 1024)
#define MAX_PACKET_LEN (100 * 1024)

#define MAX_SERVER_NET_BUFF_LEN (16 * 1024 * 1024)
#define MAX_SERVER_SOCKET_BUFF_LEN (1024 * 1024)

#define MAX_NET_BUFF_LEN (300 * 1024)
#define MAX_SOCKET_BUFF_LEN (8 * 1024)

#define IP_SIZE 24

#define GROUP_MOVE      (49)
#define SERVER_MOVE     (45)
#define COUNTER_MOVE    (29)
#define MAX_GUID_COUNT  (65535)

extern CConfigLoader g_CConfigLoader;
extern bool g_refresh;
extern bool g_exit;
extern bool g_isActive;

uint64_t make_guid(uint32_t groupid, uint32_t srvid, uint64_t & conter);

void sSignalProcess(int nSig);

bool common_ini(const char * name, int argc, char *argv[]);

struct fake;
bool fake_ini(fake * & fk, const std::string & name, int argc, char *argv[]);
