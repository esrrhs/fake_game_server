#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "datadef.h"
#include "NetMsg.h"
#include "dbthread.h"
#include "scriptbind.h"
#include "DBNetMsg.h"

dbthread::dbthread()
{
	m_isend = false;
	m_tmpstruct = new Fproto::TmpStruct;
}

dbthread::~dbthread()
{
	mysql_close(&m_mysql);
	delfake(m_fk);
	delete m_tmpstruct;
}

bool dbthread::ini()
{
	if (!ini_db(&m_mysql))
	{
		return false;
	}

	fake_ini(m_fk, g_CConfigLoader.GetConfig().m_STDataserver.m_strscript, fcmdparser::ptr()->argc(), fcmdparser::ptr()->argv());
	bind_data_script(m_fk);

	return true;
}

void dbthread::run()
{
	m_isend = false;

	while (!m_isend)
	{
		if (!pop_command(m_recvmsg))
		{
			fsleep(1);
			continue;
		}

		uint64_t guid = m_recvmsg.m_ServerNetMsgHead.m_Guid;

		LOG_DEBUG("on_game_recv_packet %d %s %llu", m_recvmsg.m_ServerNetMsgPara.m_Type, Fproto::GetServerMsgIDName(m_recvmsg.m_ServerNetMsgPara.m_Type), guid);

		m_sendmsg.Clear();
		const char * funcname = Fproto::GetServerMsgIDFuncName(m_recvmsg.m_ServerNetMsgPara.m_Type);
		fkrun<void>(m_fk, funcname, this, guid, m_recvmsg.GetServerNetMsgPara(), m_sendmsg.GetServerNetMsgPara());
	}

	m_isend = true;
}

void dbthread::push_command(const Fproto::ServerNetMsg & msg)
{
	fauto_lock<fthread_lock> tmp(m_command_lock);
	m_command.push_back(msg);
}

bool dbthread::pop_result(Fproto::ServerNetMsg & msg)
{
	fauto_lock<fthread_lock> tmp(m_result_lock);
	if (m_result.empty())
	{
		return false;
	}

	msg = m_result.front();
	m_result.pop_front();
	return true;
}

void dbthread::push_result(const Fproto::ServerNetMsg & msg)
{
	fauto_lock<fthread_lock> tmp(m_result_lock);
	m_result.push_back(msg);
}

bool dbthread::pop_command(Fproto::ServerNetMsg & msg)
{
	fauto_lock<fthread_lock> tmp(m_command_lock);
	if (m_command.empty())
	{
		return false;
	}

	msg = m_command.front();
	m_command.pop_front();
	return true;
}

Fproto::TmpStruct * dbthread::get_tmp_struct()
{
	return m_tmpstruct;
}
