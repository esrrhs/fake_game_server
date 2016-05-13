#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "gamedef.h"
#include "obj.h"

class human : public obj
{
public:
	human();
	virtual ~human();

	virtual ObjType get_type() { return OT_Human; }

	virtual uint64_t get_guid();
	virtual uint64_t get_scenegid();
	virtual uint32_t get_x();
	virtual uint32_t get_y();

public:
	bool ini(Fproto::RoleInfo * info);

	bool enter_scene(int line, int sceneid, uint64_t sceneguid) { return obj::enter_scene(line, sceneid, sceneguid); }

protected:
	Fproto::RoleInfo m_RoleInfo;
};

