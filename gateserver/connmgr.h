#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "gatedef.h"

struct conndata
{
	void clear()
	{
		memset(this, 0, sizeof(*this));
	}
	uint64_t guid;
	int connid;
	char ip[IP_SIZE];
	uint16_t port;
	char acc[Fproto::ACC_LEN];
};

class connmgr : public singleton<connmgr>
{
public:
	connmgr()
	{

	}
	~connmgr()
	{

	}

	void add_conn(const conndata & data)
	{
		m_connmap[data.guid] = data;
	}

	void del_conn(uint64_t guid)
	{
		m_connmap.erase(guid);
	}

	conndata * get_conn(uint64_t guid)
	{
		connmap::iterator it = m_connmap.find(guid);
		if (it == m_connmap.end())
		{
			return 0;
		}

		return &it->second;
	}

private:
	typedef std::map<uint64_t, conndata> connmap;
	connmap m_connmap;
};
