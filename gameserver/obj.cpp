#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "gamedef.h"
#include "ServerNetMsg.h"
#include "obj.h"
#include "scenemgr.h"

obj::obj()
{
	memset(m_mod, 0, sizeof(m_mod));
}

obj::~obj()
{

} 

bool obj::enter_scene(int line, int sceneid, uint64_t sceneguid)
{
	scenemgr::ptr()->add_obj(this, line, sceneid, sceneguid);
	return true;
}
