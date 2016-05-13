#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "gamedef.h"

void * gs_malloc(size_t size, int type)
{
	void * ret = malloc(size);
#ifdef _DEBUG
	memset(ret, 0xFB, size);
#endif
	return ret;
}

void gs_free(void * p)
{
	return free(p);
}

