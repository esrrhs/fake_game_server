#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "gamedef.h"
#include "ServerNetMsg.h"
#include "human.h"

human::human()
{
	m_RoleInfo.Clear();
}

human::~human()
{

}

bool human::ini(Fproto::RoleInfo * info)
{
	m_RoleInfo = *info;
	return true;
}

uint64_t human::get_guid()
{
	return m_RoleInfo.GetGuid();
}

uint64_t human::get_scenegid()
{
	return m_RoleInfo.GetSceneGuid();
}

uint32_t human::get_x()
{
	return m_RoleInfo.GetX();
}

uint32_t human::get_y()
{
	return m_RoleInfo.GetY();
}
