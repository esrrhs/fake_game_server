#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "gamedef.h"
#include "obj.h"

// ³¡¾°
class scene
{
public:
	scene();
	virtual ~scene();

public:
	uint64_t get_guid() { return m_guid; }

public:
	bool add_obj(obj * pobj);
	bool del_obj(obj * pobj);

protected:
	uint64_t m_guid;
};
