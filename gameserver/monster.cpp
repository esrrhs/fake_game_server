#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "gamedef.h"
#include "ServerNetMsg.h"
#include "monster.h"

monster::monster()
{

}

monster::~monster()
{

}

bool monster::ini(Fproto::MonsterInfo * info)
{
	m_MonsterInfo = *info;
	return true;
}

uint64_t monster::get_guid()
{
	return m_MonsterInfo.GetGuid();
}

uint64_t monster::get_scenegid()
{
	return m_MonsterInfo.GetSceneGuid();
}

uint32_t monster::get_x()
{
	return m_MonsterInfo.GetX();
}

uint32_t monster::get_y()
{
	return m_MonsterInfo.GetY();
}
