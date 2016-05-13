#pragma once

#include "def.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"
#include "TagNetMsg.h"
#include "mysql.h"
#include "dbthread.h"

namespace Fproto
{

static std::string escape_string(dbthread * t, const char * src)
{
	mysql_real_escape_string(t->getmysql(), t->get_escapebuff(), src, strlen(src));
	return t->get_escapebuff();
}

template <typename T>
static std::string escape_blob(dbthread * t, T * src)
{
	int ret = MarshalTag(src, t->get_marshalbuff(), t->get_marshalbuff_size());
	if (ret < 0)
	{
		return "error";
	}
	mysql_real_escape_string(t->getmysql(), t->get_escapebuff(), t->get_marshalbuff(), ret);
	return t->get_escapebuff();
}

  
// 账号信息 
static std::string create_tb_acc()
{
	std::stringstream ret;
	ret << "\
		DROP TABLE IF EXISTS `tb_acc`;\n\
		CREATE TABLE `tb_acc` (\n\
		`Acc` varchar(" << ACC_LEN << ") NOT NULL COMMENT '用户名',\n\
		`diamond` int(11) NOT NULL DEFAULT '0' COMMENT '钻石',\n\
		`RoleGuid` bigint(20) NOT NULL COMMENT '角色',\n\
		`last_updatetime` datetime NOT NULL,\n\
		PRIMARY KEY (`Acc`)\
	) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='账号信息';\n";
	
	return ret.str();
}
  
// 玩家信息 
static std::string create_tb_roleinfo()
{
	std::stringstream ret;
	ret << "\
		DROP TABLE IF EXISTS `tb_roleinfo`;\n\
		CREATE TABLE `tb_roleinfo` (\n\
		`Guid` bigint(20) NOT NULL COMMENT '唯一id',\n\
		`Acc` varchar(" << ACC_LEN << ") NOT NULL COMMENT '用户名',\n\
		`Name` varchar(" << ROLE_NAME_MAX_LEN << ") NOT NULL COMMENT '角色名',\n\
		`Hp` int(11) NOT NULL DEFAULT '0' COMMENT '血量',\n\
		`Mp` int(11) NOT NULL DEFAULT '0' COMMENT '魔量',\n\
		`X` double NOT NULL DEFAULT '0' COMMENT 'x',\n\
		`Y` double NOT NULL DEFAULT '0' COMMENT 'y',\n\
		`Line` int(11) NOT NULL DEFAULT '0' COMMENT '线路',\n\
		`SceneId` int(11) NOT NULL DEFAULT '0' COMMENT '场景id',\n\
		`SceneGuid` bigint(20) NOT NULL COMMENT '场景guid',\n\
		`FriendsInfo` blob NOT NULL COMMENT '好友信息',\n\
		`ItemsInfo` blob NOT NULL COMMENT '好友信息',\n\
		`last_updatetime` datetime NOT NULL,\n\
		PRIMARY KEY (`Guid`)\
	) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='玩家信息';\n";
	
	return ret.str();
}
 

static std::string create_all_table()
{
	std::string ret;
	  
	// 账号信息 
	ret += create_tb_acc();
	  
	// 玩家信息 
	ret += create_tb_roleinfo();
	
	return ret;
} 

  
// 账号信息 
static bool select_tb_acc(dbthread * t, AccInfo * pdata)
{
	MYSQL *mysql = t->getmysql();
	AccInfo & data = *pdata;
	
	std::stringstream ss;
	ss << "select Acc,diamond,RoleGuid,Acc from tb_acc where Acc = '" << escape_string(t, data.m_Acc) << "'";
	
	int ret = mysql_real_query(mysql, ss.str().c_str(), ss.str().size());
	if (ret != 0)
	{
		LOG_ERROR("select_tb_acc error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}
	
	MYSQL_RES * res_ptr = mysql_store_result(mysql);
	if (!res_ptr)
	{
		LOG_ERROR("select_tb_acc mysql_store_result error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}

	int num = mysql_num_rows(res_ptr);
	if (num <= 0)
	{
		mysql_free_result(res_ptr);
		LOG_DEBUG("select_tb_acc empty %s", ss.str().c_str());
		return false;
	}

	MYSQL_ROW sqlrow = mysql_fetch_row(res_ptr);
	if (!sqlrow) 
	{
		mysql_free_result(res_ptr);
		LOG_ERROR("select_tb_acc mysql_fetch_row error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}
	
	unsigned long * lengths = mysql_fetch_lengths(res_ptr);
	if (!lengths) 
	{
		mysql_free_result(res_ptr);
		LOG_ERROR("select_tb_acc mysql_fetch_lengths error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}
	
	int i = 0;
	
	
	// 用户名
	fstrcopy(data.m_Acc, sqlrow[i++]);
	
	
	// 钻石
	data.m_diamond = fatoi(sqlrow[i++]);
	
	
	// 角色
	data.m_RoleGuid = fatol(sqlrow[i++]);
	
	
	mysql_free_result(res_ptr);
	
	return true;
}
  
// 玩家信息 
static bool select_tb_roleinfo(dbthread * t, RoleInfo * pdata)
{
	MYSQL *mysql = t->getmysql();
	RoleInfo & data = *pdata;
	
	std::stringstream ss;
	ss << "select Guid,Acc,Name,Hp,Mp,X,Y,Line,SceneId,SceneGuid,FriendsInfo,ItemsInfo,Guid from tb_roleinfo where Guid = " << data.m_Guid;
	
	int ret = mysql_real_query(mysql, ss.str().c_str(), ss.str().size());
	if (ret != 0)
	{
		LOG_ERROR("select_tb_roleinfo error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}
	
	MYSQL_RES * res_ptr = mysql_store_result(mysql);
	if (!res_ptr)
	{
		LOG_ERROR("select_tb_roleinfo mysql_store_result error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}

	int num = mysql_num_rows(res_ptr);
	if (num <= 0)
	{
		mysql_free_result(res_ptr);
		LOG_DEBUG("select_tb_roleinfo empty %s", ss.str().c_str());
		return false;
	}

	MYSQL_ROW sqlrow = mysql_fetch_row(res_ptr);
	if (!sqlrow) 
	{
		mysql_free_result(res_ptr);
		LOG_ERROR("select_tb_roleinfo mysql_fetch_row error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}
	
	unsigned long * lengths = mysql_fetch_lengths(res_ptr);
	if (!lengths) 
	{
		mysql_free_result(res_ptr);
		LOG_ERROR("select_tb_roleinfo mysql_fetch_lengths error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}
	
	int i = 0;
	
	
	// 唯一id
	data.m_Guid = fatol(sqlrow[i++]);
	
	
	// 用户名
	fstrcopy(data.m_Acc, sqlrow[i++]);
	
	
	// 角色名
	fstrcopy(data.m_Name, sqlrow[i++]);
	
	
	// 血量
	data.m_Hp = fatoi(sqlrow[i++]);
	
	
	// 魔量
	data.m_Mp = fatoi(sqlrow[i++]);
	
	
	// x
	data.m_X = fatof(sqlrow[i++]);
	
	
	// y
	data.m_Y = fatof(sqlrow[i++]);
	
	
	// 线路
	data.m_Line = fatoi(sqlrow[i++]);
	
	
	// 场景id
	data.m_SceneId = fatoi(sqlrow[i++]);
	
	
	// 场景guid
	data.m_SceneGuid = fatol(sqlrow[i++]);
	
	
	// 好友信息
	data.m_FriendsInfo.Clear();
	if (lengths[i] > 0)
	{
		int ret = UnmarshalTag(&data.m_FriendsInfo, sqlrow[i], lengths[i]);
		if (ret < 0)
		{
			mysql_free_result(res_ptr);
			LOG_ERROR("select_tb_roleinfo UnmarshalTag m_FriendsInfo Fail %d", ret);
			return false;
		}
	}
	i++;
	
	
	// 好友信息
	data.m_ItemsInfo.Clear();
	if (lengths[i] > 0)
	{
		int ret = UnmarshalTag(&data.m_ItemsInfo, sqlrow[i], lengths[i]);
		if (ret < 0)
		{
			mysql_free_result(res_ptr);
			LOG_ERROR("select_tb_roleinfo UnmarshalTag m_ItemsInfo Fail %d", ret);
			return false;
		}
	}
	i++;
	
	
	mysql_free_result(res_ptr);
	
	return true;
}
  

  
// 账号信息 
static bool update_tb_acc(dbthread * t, AccInfo * pdata)
{
	MYSQL *mysql = t->getmysql();
	AccInfo & data = *pdata;
	
	
	
	std::stringstream ss;
	ss << "insert into tb_acc(Acc,diamond,RoleGuid,last_updatetime) "
	"values('" << data.m_Acc << "'," << data.m_diamond << "," << data.m_RoleGuid << ",NOW()) "
	"on duplicate key update Acc='" << data.m_Acc << "',diamond=" << data.m_diamond << ",RoleGuid=" << data.m_RoleGuid << ",last_updatetime=NOW();";
	
	int ret = mysql_real_query(mysql, ss.str().c_str(), ss.str().size());
	if(ret < 0)
	{
		LOG_ERROR("update_tb_acc error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}
	
	return true;
}
  
// 玩家信息 
static bool update_tb_roleinfo(dbthread * t, RoleInfo * pdata)
{
	MYSQL *mysql = t->getmysql();
	RoleInfo & data = *pdata;
	
	
	std::string strFriendsInfo = escape_blob(t, &data.m_FriendsInfo);
	if (strFriendsInfo == "error")
	{
		LOG_ERROR("update_tb_roleinfo escape_blob m_FriendsInfo Fail");
		return false;
	}
	
	std::string strItemsInfo = escape_blob(t, &data.m_ItemsInfo);
	if (strItemsInfo == "error")
	{
		LOG_ERROR("update_tb_roleinfo escape_blob m_ItemsInfo Fail");
		return false;
	}
	
	
	std::stringstream ss;
	ss << "insert into tb_roleinfo(Guid,Acc,Name,Hp,Mp,X,Y,Line,SceneId,SceneGuid,FriendsInfo,ItemsInfo,last_updatetime) "
	"values(" << data.m_Guid << ",'" << data.m_Acc << "','" << data.m_Name << "'," << data.m_Hp << "," << data.m_Mp << "," << data.m_X << "," << data.m_Y << "," << data.m_Line << "," << data.m_SceneId << "," << data.m_SceneGuid << ",'" << strFriendsInfo << "','" << strItemsInfo << "',NOW()) "
	"on duplicate key update Guid=" << data.m_Guid << ",Acc='" << data.m_Acc << "',Name='" << data.m_Name << "',Hp=" << data.m_Hp << ",Mp=" << data.m_Mp << ",X=" << data.m_X << ",Y=" << data.m_Y << ",Line=" << data.m_Line << ",SceneId=" << data.m_SceneId << ",SceneGuid=" << data.m_SceneGuid << ",FriendsInfo='" << strFriendsInfo << "',ItemsInfo='" << strItemsInfo << "',last_updatetime=NOW();";
	
	int ret = mysql_real_query(mysql, ss.str().c_str(), ss.str().size());
	if(ret < 0)
	{
		LOG_ERROR("update_tb_roleinfo error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}
	
	return true;
}
  

struct TmpStruct
{
  
	// 账号信息 
	AccInfo m_AccInfo;
  
	// 玩家信息 
	RoleInfo m_RoleInfo;

};

  
// 账号信息 
static AccInfo * get_tmp_AccInfo(dbthread * t)
{
	t->get_tmp_struct()->m_AccInfo.Clear();
	return &(t->get_tmp_struct()->m_AccInfo);
}
  
// 玩家信息 
static RoleInfo * get_tmp_RoleInfo(dbthread * t)
{
	t->get_tmp_struct()->m_RoleInfo.Clear();
	return &(t->get_tmp_struct()->m_RoleInfo);
}


static void bindscript_dbnetmsg(fake * fk)
{
  
	// 账号信息 
	fkreg(fk, "create_tb_acc", create_tb_acc);
	fkreg(fk, "select_tb_acc", select_tb_acc);
	fkreg(fk, "update_tb_acc", update_tb_acc);
	fkreg(fk, "get_tmp_AccInfo", get_tmp_AccInfo);
  
	// 玩家信息 
	fkreg(fk, "create_tb_roleinfo", create_tb_roleinfo);
	fkreg(fk, "select_tb_roleinfo", select_tb_roleinfo);
	fkreg(fk, "update_tb_roleinfo", update_tb_roleinfo);
	fkreg(fk, "get_tmp_RoleInfo", get_tmp_RoleInfo);

}

}
