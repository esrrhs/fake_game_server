#include "libcommon.h"
#include "lu.h"
#include "configloader.h"
#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "scriptbind.h"
#include "datadef.h"
#include "dbthreadmgr.h"
#include "mysql.h"
#include "DBNetMsg.h"

void fk_dbthread_send_to_db(dbthread * t, uint64_t guid, int msgid)
{
	t->get_sendmsg().m_ServerNetMsgHead.m_Guid = guid;
	t->get_sendmsg().m_ServerNetMsgPara.m_Type = msgid;
	t->push_result(t->get_sendmsg());
}

uint64_t fk_make_guid()
{
	return make_guid(g_CConfigLoader.GetConfig().m_STCommon.m_igroupid, ST_Dataserver, g_GuidCounter);
}

void bind_data_script(fake * fk)
{
	Fproto::bindscript_dbnetmsg(fk);

	fkreg(fk, "send_to_game", &fk_dbthread_send_to_db);
	fkreg(fk, "make_guid", &fk_make_guid);
}
