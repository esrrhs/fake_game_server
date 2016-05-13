#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "dbthread.h"

class dbthreadmgr : public singleton<dbthreadmgr>
{
public:
	dbthreadmgr();
	~dbthreadmgr();

	bool ini();
	void start();
	void stop();

	void push_command(const Fproto::ServerNetMsg & msg);

	void heartbeat();

private:
	std::vector<dbthread*> m_threads;
};

