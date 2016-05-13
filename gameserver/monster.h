#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "gamedef.h"
#include "obj.h"

class monster : public obj
{
public:
	monster();
	virtual ~monster();

	virtual ObjType get_type() { return OT_Monster; }

	virtual uint64_t get_guid();
	virtual uint64_t get_scenegid();
	virtual uint32_t get_x();
	virtual uint32_t get_y();

public:
	bool ini(Fproto::MonsterInfo * info);

protected:
	Fproto::MonsterInfo m_MonsterInfo;
};
