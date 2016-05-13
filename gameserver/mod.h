#pragma once

#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "gamedef.h"

enum ModType
{
	MOD_Buff,
	MOD_Equip,

	MOD_MAX
};

class mod
{
public:
	mod() {}
	virtual ~mod() {}

};
