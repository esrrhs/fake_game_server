#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "datadef.h"
#include "NetMsg.h"
#include "dbthreadmgr.h"

dbthreadmgr::dbthreadmgr()
{

}

dbthreadmgr::~dbthreadmgr()
{
	int num = m_threads.size();
	for (int i = 0; i < num; i++)
	{
		dbthread * pthread = m_threads[i];
		delete pthread;
	}
}

bool dbthreadmgr::ini()
{
	int num = g_CConfigLoader.GetConfig().m_STDataserver.m_STMysql.m_ithreadnum;
	for (int i = 0; i < num; i++)
	{
		dbthread * pthread = new dbthread;
		m_threads.push_back(pthread);
		if (!pthread->ini())
		{
			return false;
		}
	}

	return true;
}

void dbthreadmgr::start()
{
	int num = m_threads.size();
	for (int i = 0; i < num; i++)
	{
		dbthread * pthread = m_threads[i];
		pthread->start();
	}
}

void dbthreadmgr::stop()
{
	int num = m_threads.size();
	for (int i = 0; i < num; i++)
	{
		dbthread * pthread = m_threads[i];
		pthread->stop();
	}

	for (int i = 0; i < num; i++)
	{
		dbthread * pthread = m_threads[i];
		pthread->join();
	}
}

void dbthreadmgr::push_command(const Fproto::ServerNetMsg & msg)
{
	int index = msg.m_ServerNetMsgHead.m_Guid % m_threads.size();
	m_threads[index]->push_command(msg);
}

void dbthreadmgr::heartbeat()
{
	int num = m_threads.size();
	for (int i = 0; i < num; i++)
	{
		dbthread * pthread = m_threads[i];
		if (pthread->pop_result(g_SendServerNetMsg))
		{
			send_game_msg(g_SendServerNetMsg);
		}
	}
}
