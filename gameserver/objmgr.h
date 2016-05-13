#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "gamedef.h"
#include "obj.h"
#include "human.h"
#include "monster.h"

class objmgr : public singleton<objmgr>
{
public:
	objmgr();
	~objmgr();

public:
	bool add_human(human * pHuman);
	human * get_human(uint64_t guid);

private:
	typedef std::map<uint64_t, obj*> ObjMap;
	typedef ObjMap::iterator ObjMapIter;
	ObjMap m_ObjMap[OT_MAX];
	ObjMap m_TotalObjMap;
};
