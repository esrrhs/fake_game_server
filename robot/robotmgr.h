#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"

class robot
{
public:
	robot();
	~robot();

	bool ini();
	void heartbeat();
	bool fini();

	lu * get_lu() { return m_lu; }

	void set_acc(const char * acc) { m_acc = acc; }
	const char * get_acc() { return m_acc.c_str(); }

	void set_guid(uint64_t guid) { m_guid = guid; }
	uint64_t get_guid() { return m_guid; }

	void set_name(const char * name) { m_name = name; }
	const char * get_name() { return m_name.c_str(); }

private:
	lu * m_lu;
	std::string m_acc;
	std::string m_name;
	uint64_t m_guid;
};

class robotmgr : public singleton<robotmgr>
{
public:
	robotmgr();
	~robotmgr();

	bool ini();
	void heartbeat();
	bool fini();

private:
	typedef std::vector<robot*> robotvec;
	robotvec m_robotvec;
};
