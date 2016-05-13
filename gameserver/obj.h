#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "gamedef.h"
#include "mod.h"

enum ObjType
{
	OT_Human,
	OT_Monster,

	OT_MAX
};

// ³¡¾°ÖÐµÄobj
class obj
{
public:
	obj();
	virtual ~obj();

	virtual ObjType get_type() = 0;

public:
	virtual uint64_t get_guid() = 0;
	virtual uint64_t get_scenegid() = 0;
	virtual uint32_t get_x() = 0;
	virtual uint32_t get_y() = 0;

	mod * get_mod(int i)
	{
		if (i >= 0 && i < MOD_MAX)
		{
			return m_mod[i];
		}
		return 0;
	}

public:
	bool enter_scene(int line, int sceneid, uint64_t sceneguid);

protected:
	mod * m_mod[MOD_MAX];
};
