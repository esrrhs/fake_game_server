#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "gamedef.h"
#include "ServerNetMsg.h"
#include "objmgr.h"

objmgr::objmgr()
{

}

objmgr::~objmgr()
{

}

bool objmgr::add_human(human * pHuman)
{
	human * pOld = get_human(pHuman->get_guid());
	if (pOld)
	{
		assert(0);
		LOG_ERROR("add_human error %llu", pHuman->get_guid());
		return false;
	}

	m_ObjMap[OT_Human][pHuman->get_guid()] = pHuman;
	m_TotalObjMap[pHuman->get_guid()] = pHuman;
	return true;
}

human * objmgr::get_human(uint64_t guid)
{
	ObjMapIter it = m_ObjMap[OT_Human].find(guid);
	if (it != m_ObjMap[OT_Human].end())
	{
		human * pHuman = dynamic_cast<human*>(it->second);
		return pHuman;
	}
	return 0;
}
