#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "robotmgr.h"
#include "robotdef.h"

robot::robot()
{
	m_lu = 0;
	m_guid = 0;
}

robot::~robot()
{

}

bool robot::ini()
{
	luconfig gameconfig;
	gameconfig.type = lut_tcpclient;
	gameconfig.port = g_CConfigLoader.GetConfig().m_STGateserver.m_ioutport;
	gameconfig.maxpacketlen = MAX_PACKET_LEN;
	gameconfig.isencrypt = false;
	gameconfig.iscompress = false;
	gameconfig.sendbuff = MAX_NET_BUFF_LEN;
	gameconfig.recvbuff = MAX_NET_BUFF_LEN;
	gameconfig.socket_sendbuff = MAX_SOCKET_BUFF_LEN;
	gameconfig.socket_recvbuff = MAX_SOCKET_BUFF_LEN;
	gameconfig.cco = on_game_open;
	gameconfig.ccrp = on_game_recv_packet;
	gameconfig.ccc = on_game_close;
	gameconfig.userdata.params[0]._ptr = this;

	m_lu = newlu(&gameconfig);
	return true;
}

void robot::heartbeat()
{
	ticklu(m_lu);
}

bool robot::fini()
{
	dellu(m_lu);
	return true;
}
robotmgr::robotmgr()
{
}

robotmgr::~robotmgr()
{

}

bool robotmgr::ini()
{
	int num = g_CConfigLoader.GetConfig().m_STRobot.m_inum;
	for (int i = 0; i < num; i++)
	{
		robot * p = new robot;

		std::string acc;
		fcmdparser::ptr()->get("acc", acc);
		acc += fitoa(i);
		p->set_acc(acc.c_str());

		std::string name = "robot_";
		name += fitoa(i);
		p->set_name(name.c_str());

		if (!p->ini())
		{
			return false;
		}
		m_robotvec.push_back(p);
	}
	return true;
}

void robotmgr::heartbeat()
{
	for (int i = 0; i < (int)m_robotvec.size(); i++)
	{
		m_robotvec[i]->heartbeat();
	}
}

bool robotmgr::fini()
{
	for (int i = 0; i < (int)m_robotvec.size(); i++)
	{
		m_robotvec[i]->fini();
	}
	return true;
}
