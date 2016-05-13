#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "gamedef.h"
#include "ServerNetMsg.h"
#include "scenemgr.h"

scenemgr::scenemgr()
{

}

scenemgr::~scenemgr()
{

}

bool scenemgr::add_scene(scene * pScene)
{
	scene * pOld = get_scene(pScene->get_guid());
	if (pOld)
	{
		assert(0);
		LOG_ERROR("add_scene error %llu", pScene->get_guid());
		return false;
	}

	m_SceneMap[pScene->get_guid()] = pScene;
	return true;
}

scene * scenemgr::get_scene(uint64_t guid)
{
	SceneMapIter it = m_SceneMap.find(guid);
	if (it != m_SceneMap.end())
	{
		scene * pScene = it->second;
		return pScene;
	}
	return 0;
}

bool scenemgr::add_obj(obj * pobj, int line, int sceneid, uint64_t sceneguid)
{
	// 普通场景
	if (sceneguid == 0)
	{
		scene * pOldScene = get_scene(pobj->get_scenegid());
		if (pOldScene)
		{
			pOldScene->del_obj(pobj);
		}

		uint64_t gid = GEN_SCENE_GID(line, sceneid);
		scene * pScene = get_scene(gid);
		if (!pScene)
		{
			LOG_ERROR("add_obj get_scene fail %d %d %llu", line, sceneid, gid);
			return false;
		}

		pScene->add_obj(pobj);
	}
	// 副本
	else
	{
		// TODO
	}
	return true;
}
