#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "gamedef.h"
#include "scene.h"
#include "obj.h"

#define GEN_SCENE_GID(line, sceneid) (line * 1000000 + sceneid)
#define GEN_SCENE_ID(gid) (gid % 1000000)
#define GEN_SCENE_LINE(gid) (gid / 1000000)

class scenemgr : public singleton<scenemgr>
{
public:
	scenemgr();
	~scenemgr();

public:
	bool add_scene(scene * pScene);
	scene * get_scene(uint64_t guid);

	bool add_obj(obj * pobj, int line, int sceneid, uint64_t sceneguid);

private:
	typedef std::map<uint64_t, scene*> SceneMap;
	typedef SceneMap::iterator SceneMapIter;
	SceneMap m_SceneMap;
};
