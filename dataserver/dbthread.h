#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "fakescript.h"
namespace Fproto
{
	struct TmpStruct;
}

class dbthread : public fthread
{
public:
	dbthread();
	virtual ~dbthread();

	virtual void run();

	bool ini();
	void stop() { m_isend = true; }

	void push_command(const Fproto::ServerNetMsg & msg);
	bool pop_result(Fproto::ServerNetMsg & msg);

	void push_result(const Fproto::ServerNetMsg & msg);
	bool pop_command(Fproto::ServerNetMsg & msg);

	Fproto::ServerNetMsg & get_sendmsg() { return m_sendmsg; }
	MYSQL * getmysql() { return &m_mysql; }
	char * get_escapebuff() { return m_escapebuff; }
	size_t get_escapebuff_size() { return sizeof(m_escapebuff); }
	char * get_marshalbuff() { return m_marshalbuff; }
	size_t get_marshalbuff_size() { return sizeof(m_marshalbuff); }

	Fproto::TmpStruct * get_tmp_struct();

private:
	bool m_isend;
	std::list<Fproto::ServerNetMsg> m_command;
	std::list<Fproto::ServerNetMsg> m_result;
	fthread_lock m_command_lock;
	fthread_lock m_result_lock;
	Fproto::ServerNetMsg m_recvmsg;
	Fproto::ServerNetMsg m_sendmsg;
	fake * m_fk;
	MYSQL m_mysql;
	char m_escapebuff[1024 * 1024];
	char m_marshalbuff[1024 * 1024];
	Fproto::TmpStruct * m_tmpstruct;
};


