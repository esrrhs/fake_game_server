#pragma once

#include "libcommon.h"
#include "fakescript.h"

#define PROTO_MIN(a,b) Min<int32_t>((int32_t)a, (int32_t)b)

namespace Fproto
{

    
// 消息id  
enum MsgID
{
	 
	// 请求登录  
	CS_REQ_LOGIN = 1,
	 
	// 返回登录  
	SC_RES_LOGIN = 2,
	 
	// 请求创建角色  
	CS_REQ_CREATE_ROLE = 3,
	 
	// 返回创建角色  
	SC_RES_CREATE_ROLE = 4,
	 
	// 请求登入游戏  
	CS_REQ_ENTER = 5,
	 
	// 返回登入游戏  
	SC_RES_ENTER = 6,
	 
	// 推送附近的信息  
	SC_NTY_NEAR = 7,
	  
};
    
// 常量  
enum ConstDefine
{
	 
	// 用户名  
	ACC_LEN = 32,
	 
	// 角色名  
	ROLE_NAME_MAX_LEN = 32,
	 
	// 密码  
	PWD_LEN = 32,
	 
	// 角色数量  
	ROLE_INFO_NUM = 1,
	 
	// 好友  
	FRIEND_LEN = 50,
	 
	// 道具  
	ITEM_LEN = 100,
	 
	// 附近信息  
	NEAR_INFO_NUM = 100,
	 
	// 变更数据  
	EVENT_DATA_NUM = 100,
	 
	// 移动信息  
	MOVE_DATA_NUM = 10,
	  
};
    
// 变更的数据类型  
enum EventDataType
{
	 
	// 进入视野  
	EDT_Enter = 0,
	 
	// 离开视野  
	EDT_Leave = 1,
	 
	// 移动  
	EDT_Move = 2,
	 
	// 属性变化  
	EDT_Property = 3,
	 
	// 释放技能  
	EDT_SkillCast = 4,
	  
};
    
// 变更的数据 进入视野的数据类型  
enum EventDataEnterType
{
	 
	// 进入视野  
	EDET_Human = 0,
	 
	// 离开视野  
	EDET_Monster = 1,
	  
};
  

    
// 消息id  
static const char * GetMsgIDName(int i)
{
	switch (i)
	{
	 
	// 请求登录  
	case 1:
		return "CS_REQ_LOGIN";
	 
	// 返回登录  
	case 2:
		return "SC_RES_LOGIN";
	 
	// 请求创建角色  
	case 3:
		return "CS_REQ_CREATE_ROLE";
	 
	// 返回创建角色  
	case 4:
		return "SC_RES_CREATE_ROLE";
	 
	// 请求登入游戏  
	case 5:
		return "CS_REQ_ENTER";
	 
	// 返回登入游戏  
	case 6:
		return "SC_RES_ENTER";
	 
	// 推送附近的信息  
	case 7:
		return "SC_NTY_NEAR";
	  
	default:
		return "";
	}
}

static const char * GetMsgIDFuncName(int i)
{
	switch (i)
	{
	 
	// 请求登录  
	case 1:
		return "on_CS_REQ_LOGIN";
	 
	// 返回登录  
	case 2:
		return "on_SC_RES_LOGIN";
	 
	// 请求创建角色  
	case 3:
		return "on_CS_REQ_CREATE_ROLE";
	 
	// 返回创建角色  
	case 4:
		return "on_SC_RES_CREATE_ROLE";
	 
	// 请求登入游戏  
	case 5:
		return "on_CS_REQ_ENTER";
	 
	// 返回登入游戏  
	case 6:
		return "on_SC_RES_ENTER";
	 
	// 推送附近的信息  
	case 7:
		return "on_SC_NTY_NEAR";
	  
	default:
		return "";
	}
}  
  
  
  
  



		
		
	


	
		
       
// 账号信息  
enum E_AccInfo  
{
	
	 
	// 用户名  
	E_AccInfo_Acc,
	 
	// 钻石  
	E_AccInfo_diamond,
	 
	// 角色  
	E_AccInfo_RoleGuid,
	  
	
};

// 账号信息  
struct AccInfo  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 用户名  
	char m_Acc[ACC_LEN];  
	  
	// 钻石  
	uint32_t m_diamond;  
	  
	// 角色  
	uint64_t m_RoleGuid;  
	  
	
	
	
	// 用户名  
	const char * GetAcc()
	{
		return m_Acc;
	}
	
	// 用户名  
	void SetAcc(const char * _Acc)
	{
		fstrcopy(m_Acc, _Acc);
	}
	
	
	// 钻石  
	uint32_t Getdiamond()
	{
		return m_diamond;
	}
	
	// 钻石  
	void Setdiamond(uint32_t _diamond)
	{
		m_diamond = _diamond;
	}
	
	
	// 角色  
	uint64_t GetRoleGuid()
	{
		return m_RoleGuid;
	}
	
	// 角色  
	void SetRoleGuid(uint64_t _RoleGuid)
	{
		m_RoleGuid = _RoleGuid;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 用户名	
		if (ret + (int32_t)sizeof(m_Acc) > size)
		{
			return -ret;
		}
		fstrcopy(destbuffer, m_Acc, sizeof(m_Acc));
		ret += sizeof(m_Acc);
		destbuffer += sizeof(m_Acc);
		 
		 
		// 钻石	
		if (ret + (int32_t)sizeof(m_diamond) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_diamond;
		ret += sizeof(m_diamond);
		destbuffer += sizeof(m_diamond);
		 
		 
		// 角色	
		if (ret + (int32_t)sizeof(m_RoleGuid) > size)
		{
			return -ret;
		}
		*(uint64_t*)destbuffer = m_RoleGuid;
		ret += sizeof(m_RoleGuid);
		destbuffer += sizeof(m_RoleGuid);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 用户名	
		if (ret + (int32_t)sizeof(m_Acc) > size)
		{
			return -ret;
		}
		fstrcopy(m_Acc, srcbuffer);
		ret += sizeof(m_Acc);
		srcbuffer += sizeof(m_Acc);
		 
		 
		// 钻石	
		if (ret + (int32_t)sizeof(m_diamond) > size)
		{
			return -ret;
		}
		m_diamond = *(uint32_t*)srcbuffer;
		ret += sizeof(m_diamond);
		srcbuffer += sizeof(m_diamond);
		 
		 
		// 角色	
		if (ret + (int32_t)sizeof(m_RoleGuid) > size)
		{
			return -ret;
		}
		m_RoleGuid = *(uint64_t*)srcbuffer;
		ret += sizeof(m_RoleGuid);
		srcbuffer += sizeof(m_RoleGuid);
		 
		 
		return ret;
	}
	
};  
       
// 好友信息  
enum E_FriendInfo  
{
	
	 
	// 唯一id  
	E_FriendInfo_Guid,
	 
	// 好感度  
	E_FriendInfo_Nice,
	  
	
};

// 好友信息  
struct FriendInfo  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 唯一id  
	uint64_t m_Guid;  
	  
	// 好感度  
	uint32_t m_Nice;  
	  
	
	
	
	// 唯一id  
	uint64_t GetGuid()
	{
		return m_Guid;
	}
	
	// 唯一id  
	void SetGuid(uint64_t _Guid)
	{
		m_Guid = _Guid;
	}
	
	
	// 好感度  
	uint32_t GetNice()
	{
		return m_Nice;
	}
	
	// 好感度  
	void SetNice(uint32_t _Nice)
	{
		m_Nice = _Nice;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 唯一id	
		if (ret + (int32_t)sizeof(m_Guid) > size)
		{
			return -ret;
		}
		*(uint64_t*)destbuffer = m_Guid;
		ret += sizeof(m_Guid);
		destbuffer += sizeof(m_Guid);
		 
		 
		// 好感度	
		if (ret + (int32_t)sizeof(m_Nice) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_Nice;
		ret += sizeof(m_Nice);
		destbuffer += sizeof(m_Nice);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 唯一id	
		if (ret + (int32_t)sizeof(m_Guid) > size)
		{
			return -ret;
		}
		m_Guid = *(uint64_t*)srcbuffer;
		ret += sizeof(m_Guid);
		srcbuffer += sizeof(m_Guid);
		 
		 
		// 好感度	
		if (ret + (int32_t)sizeof(m_Nice) > size)
		{
			return -ret;
		}
		m_Nice = *(uint32_t*)srcbuffer;
		ret += sizeof(m_Nice);
		srcbuffer += sizeof(m_Nice);
		 
		 
		return ret;
	}
	
};  
       
// 好友信息  
enum E_FriendsInfo  
{
	
	 
	// 数目  
	E_FriendsInfo_Num,
	 
	// 好友信息  
	E_FriendsInfo_FriendInfo,
	  
	
};

// 好友信息  
struct FriendsInfo  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 数目  
	uint8_t m_Num;  
	  
	// 好友信息  
	FriendInfo m_FriendInfo[FRIEND_LEN];  
	  
	
	
	
	// 数目  
	uint8_t GetNum()
	{
		return m_Num;
	}
	
	// 数目  
	void SetNum(uint8_t _Num)
	{
		m_Num = _Num;
	}
	
	
	// 好友信息  
	FriendInfo * GetFriendInfo(int i)
	{
		assert(i >= 0 && i < FRIEND_LEN);
		if (i >= 0 && i < FRIEND_LEN)
		{
			return &m_FriendInfo[i];
		}
		return &m_FriendInfo[0];
	}
	
	// 好友信息  
	void SetFriendInfo(int i, FriendInfo * pFriendInfo)
	{
		assert(i >= 0 && i < FRIEND_LEN);
		if (i >= 0 && i < FRIEND_LEN && pFriendInfo)
		{
			m_FriendInfo[i] = *pFriendInfo;
		}
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 数目	
		if (ret + (int32_t)sizeof(m_Num) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_Num;
		ret += sizeof(m_Num);
		destbuffer += sizeof(m_Num);
		 
		 
		// 好友信息	
		for (int32_t i = 0; i < FRIEND_LEN && i < m_Num; i++)
		{
			int32_t retFriendInfo = m_FriendInfo[i].Marshal(destbuffer, size - ret);
			if (retFriendInfo <= 0)
			{
				return -ret + retFriendInfo;
			}
			ret += retFriendInfo;
			destbuffer += retFriendInfo;
		}
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 数目	
		if (ret + (int32_t)sizeof(m_Num) > size)
		{
			return -ret;
		}
		m_Num = *(uint8_t*)srcbuffer;
		ret += sizeof(m_Num);
		srcbuffer += sizeof(m_Num);
		 
		 
		// 好友信息	
		for (int32_t i = 0; i < FRIEND_LEN && i < m_Num; i++)
		{
			int32_t retFriendInfo = m_FriendInfo[i].Unmarshal(srcbuffer, size - ret);
			if (retFriendInfo <= 0)
			{
				return -ret + retFriendInfo;
			}
			ret += retFriendInfo;
			srcbuffer += retFriendInfo;
		}
		m_Num = PROTO_MIN(m_Num, FRIEND_LEN);
		 
		 
		return ret;
	}
	
};  
       
// 道具信息  
enum E_ItemInfo  
{
	
	 
	// 唯一id  
	E_ItemInfo_Guid,
	 
	// 表格id  
	E_ItemInfo_ItemId,
	 
	// 堆叠数目  
	E_ItemInfo_Num,
	 
	// 是否绑定  
	E_ItemInfo_IsBind,
	  
	
};

// 道具信息  
struct ItemInfo  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 唯一id  
	uint64_t m_Guid;  
	  
	// 表格id  
	uint32_t m_ItemId;  
	  
	// 堆叠数目  
	uint32_t m_Num;  
	  
	// 是否绑定  
	uint8_t m_IsBind;  
	  
	
	
	
	// 唯一id  
	uint64_t GetGuid()
	{
		return m_Guid;
	}
	
	// 唯一id  
	void SetGuid(uint64_t _Guid)
	{
		m_Guid = _Guid;
	}
	
	
	// 表格id  
	uint32_t GetItemId()
	{
		return m_ItemId;
	}
	
	// 表格id  
	void SetItemId(uint32_t _ItemId)
	{
		m_ItemId = _ItemId;
	}
	
	
	// 堆叠数目  
	uint32_t GetNum()
	{
		return m_Num;
	}
	
	// 堆叠数目  
	void SetNum(uint32_t _Num)
	{
		m_Num = _Num;
	}
	
	
	// 是否绑定  
	uint8_t GetIsBind()
	{
		return m_IsBind;
	}
	
	// 是否绑定  
	void SetIsBind(uint8_t _IsBind)
	{
		m_IsBind = _IsBind;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 唯一id	
		if (ret + (int32_t)sizeof(m_Guid) > size)
		{
			return -ret;
		}
		*(uint64_t*)destbuffer = m_Guid;
		ret += sizeof(m_Guid);
		destbuffer += sizeof(m_Guid);
		 
		 
		// 表格id	
		if (ret + (int32_t)sizeof(m_ItemId) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_ItemId;
		ret += sizeof(m_ItemId);
		destbuffer += sizeof(m_ItemId);
		 
		 
		// 堆叠数目	
		if (ret + (int32_t)sizeof(m_Num) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_Num;
		ret += sizeof(m_Num);
		destbuffer += sizeof(m_Num);
		 
		 
		// 是否绑定	
		if (ret + (int32_t)sizeof(m_IsBind) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_IsBind;
		ret += sizeof(m_IsBind);
		destbuffer += sizeof(m_IsBind);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 唯一id	
		if (ret + (int32_t)sizeof(m_Guid) > size)
		{
			return -ret;
		}
		m_Guid = *(uint64_t*)srcbuffer;
		ret += sizeof(m_Guid);
		srcbuffer += sizeof(m_Guid);
		 
		 
		// 表格id	
		if (ret + (int32_t)sizeof(m_ItemId) > size)
		{
			return -ret;
		}
		m_ItemId = *(uint32_t*)srcbuffer;
		ret += sizeof(m_ItemId);
		srcbuffer += sizeof(m_ItemId);
		 
		 
		// 堆叠数目	
		if (ret + (int32_t)sizeof(m_Num) > size)
		{
			return -ret;
		}
		m_Num = *(uint32_t*)srcbuffer;
		ret += sizeof(m_Num);
		srcbuffer += sizeof(m_Num);
		 
		 
		// 是否绑定	
		if (ret + (int32_t)sizeof(m_IsBind) > size)
		{
			return -ret;
		}
		m_IsBind = *(uint8_t*)srcbuffer;
		ret += sizeof(m_IsBind);
		srcbuffer += sizeof(m_IsBind);
		 
		 
		return ret;
	}
	
};  
       
// 道具信息  
enum E_ItemsInfo  
{
	
	 
	// 数目  
	E_ItemsInfo_Num,
	 
	// 道具信息  
	E_ItemsInfo_ItemInfo,
	  
	
};

// 道具信息  
struct ItemsInfo  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 数目  
	uint8_t m_Num;  
	  
	// 道具信息  
	ItemInfo m_ItemInfo[ITEM_LEN];  
	  
	
	
	
	// 数目  
	uint8_t GetNum()
	{
		return m_Num;
	}
	
	// 数目  
	void SetNum(uint8_t _Num)
	{
		m_Num = _Num;
	}
	
	
	// 道具信息  
	ItemInfo * GetItemInfo(int i)
	{
		assert(i >= 0 && i < ITEM_LEN);
		if (i >= 0 && i < ITEM_LEN)
		{
			return &m_ItemInfo[i];
		}
		return &m_ItemInfo[0];
	}
	
	// 道具信息  
	void SetItemInfo(int i, ItemInfo * pItemInfo)
	{
		assert(i >= 0 && i < ITEM_LEN);
		if (i >= 0 && i < ITEM_LEN && pItemInfo)
		{
			m_ItemInfo[i] = *pItemInfo;
		}
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 数目	
		if (ret + (int32_t)sizeof(m_Num) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_Num;
		ret += sizeof(m_Num);
		destbuffer += sizeof(m_Num);
		 
		 
		// 道具信息	
		for (int32_t i = 0; i < ITEM_LEN && i < m_Num; i++)
		{
			int32_t retItemInfo = m_ItemInfo[i].Marshal(destbuffer, size - ret);
			if (retItemInfo <= 0)
			{
				return -ret + retItemInfo;
			}
			ret += retItemInfo;
			destbuffer += retItemInfo;
		}
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 数目	
		if (ret + (int32_t)sizeof(m_Num) > size)
		{
			return -ret;
		}
		m_Num = *(uint8_t*)srcbuffer;
		ret += sizeof(m_Num);
		srcbuffer += sizeof(m_Num);
		 
		 
		// 道具信息	
		for (int32_t i = 0; i < ITEM_LEN && i < m_Num; i++)
		{
			int32_t retItemInfo = m_ItemInfo[i].Unmarshal(srcbuffer, size - ret);
			if (retItemInfo <= 0)
			{
				return -ret + retItemInfo;
			}
			ret += retItemInfo;
			srcbuffer += retItemInfo;
		}
		m_Num = PROTO_MIN(m_Num, ITEM_LEN);
		 
		 
		return ret;
	}
	
};  
       
// 玩家信息  
enum E_RoleInfo  
{
	
	 
	// 唯一id  
	E_RoleInfo_Guid,
	 
	// 用户名  
	E_RoleInfo_Acc,
	 
	// 角色名  
	E_RoleInfo_Name,
	 
	// 血量  
	E_RoleInfo_Hp,
	 
	// 魔量  
	E_RoleInfo_Mp,
	 
	// x  
	E_RoleInfo_X,
	 
	// y  
	E_RoleInfo_Y,
	 
	// 线路  
	E_RoleInfo_Line,
	 
	// 场景id  
	E_RoleInfo_SceneId,
	 
	// 场景guid  
	E_RoleInfo_SceneGuid,
	 
	// 好友信息  
	E_RoleInfo_FriendsInfo,
	 
	// 好友信息  
	E_RoleInfo_ItemsInfo,
	  
	
};

// 玩家信息  
struct RoleInfo  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 唯一id  
	uint64_t m_Guid;  
	  
	// 用户名  
	char m_Acc[ACC_LEN];  
	  
	// 角色名  
	char m_Name[ROLE_NAME_MAX_LEN];  
	  
	// 血量  
	uint32_t m_Hp;  
	  
	// 魔量  
	uint32_t m_Mp;  
	  
	// x  
	float m_X;  
	  
	// y  
	float m_Y;  
	  
	// 线路  
	uint32_t m_Line;  
	  
	// 场景id  
	uint32_t m_SceneId;  
	  
	// 场景guid  
	uint64_t m_SceneGuid;  
	  
	// 好友信息  
	FriendsInfo m_FriendsInfo;  
	  
	// 好友信息  
	ItemsInfo m_ItemsInfo;  
	  
	
	
	
	// 唯一id  
	uint64_t GetGuid()
	{
		return m_Guid;
	}
	
	// 唯一id  
	void SetGuid(uint64_t _Guid)
	{
		m_Guid = _Guid;
	}
	
	
	// 用户名  
	const char * GetAcc()
	{
		return m_Acc;
	}
	
	// 用户名  
	void SetAcc(const char * _Acc)
	{
		fstrcopy(m_Acc, _Acc);
	}
	
	
	// 角色名  
	const char * GetName()
	{
		return m_Name;
	}
	
	// 角色名  
	void SetName(const char * _Name)
	{
		fstrcopy(m_Name, _Name);
	}
	
	
	// 血量  
	uint32_t GetHp()
	{
		return m_Hp;
	}
	
	// 血量  
	void SetHp(uint32_t _Hp)
	{
		m_Hp = _Hp;
	}
	
	
	// 魔量  
	uint32_t GetMp()
	{
		return m_Mp;
	}
	
	// 魔量  
	void SetMp(uint32_t _Mp)
	{
		m_Mp = _Mp;
	}
	
	
	// x  
	float GetX()
	{
		return m_X;
	}
	
	// x  
	void SetX(float _X)
	{
		m_X = _X;
	}
	
	
	// y  
	float GetY()
	{
		return m_Y;
	}
	
	// y  
	void SetY(float _Y)
	{
		m_Y = _Y;
	}
	
	
	// 线路  
	uint32_t GetLine()
	{
		return m_Line;
	}
	
	// 线路  
	void SetLine(uint32_t _Line)
	{
		m_Line = _Line;
	}
	
	
	// 场景id  
	uint32_t GetSceneId()
	{
		return m_SceneId;
	}
	
	// 场景id  
	void SetSceneId(uint32_t _SceneId)
	{
		m_SceneId = _SceneId;
	}
	
	
	// 场景guid  
	uint64_t GetSceneGuid()
	{
		return m_SceneGuid;
	}
	
	// 场景guid  
	void SetSceneGuid(uint64_t _SceneGuid)
	{
		m_SceneGuid = _SceneGuid;
	}
	
	
	// 好友信息  
	FriendsInfo * GetFriendsInfo()
	{
		return &m_FriendsInfo;
	}
	
	// 好友信息  
	void SetFriendsInfo(FriendsInfo * pFriendsInfo)
	{
		if (pFriendsInfo)
		{
			m_FriendsInfo = *pFriendsInfo;
		}
	}
	
	
	// 好友信息  
	ItemsInfo * GetItemsInfo()
	{
		return &m_ItemsInfo;
	}
	
	// 好友信息  
	void SetItemsInfo(ItemsInfo * pItemsInfo)
	{
		if (pItemsInfo)
		{
			m_ItemsInfo = *pItemsInfo;
		}
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 唯一id	
		if (ret + (int32_t)sizeof(m_Guid) > size)
		{
			return -ret;
		}
		*(uint64_t*)destbuffer = m_Guid;
		ret += sizeof(m_Guid);
		destbuffer += sizeof(m_Guid);
		 
		 
		// 用户名	
		if (ret + (int32_t)sizeof(m_Acc) > size)
		{
			return -ret;
		}
		fstrcopy(destbuffer, m_Acc, sizeof(m_Acc));
		ret += sizeof(m_Acc);
		destbuffer += sizeof(m_Acc);
		 
		 
		// 角色名	
		if (ret + (int32_t)sizeof(m_Name) > size)
		{
			return -ret;
		}
		fstrcopy(destbuffer, m_Name, sizeof(m_Name));
		ret += sizeof(m_Name);
		destbuffer += sizeof(m_Name);
		 
		 
		// 血量	
		if (ret + (int32_t)sizeof(m_Hp) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_Hp;
		ret += sizeof(m_Hp);
		destbuffer += sizeof(m_Hp);
		 
		 
		// 魔量	
		if (ret + (int32_t)sizeof(m_Mp) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_Mp;
		ret += sizeof(m_Mp);
		destbuffer += sizeof(m_Mp);
		 
		 
		// x	
		if (ret + (int32_t)sizeof(m_X) > size)
		{
			return -ret;
		}
		*(float*)destbuffer = m_X;
		ret += sizeof(m_X);
		destbuffer += sizeof(m_X);
		 
		 
		// y	
		if (ret + (int32_t)sizeof(m_Y) > size)
		{
			return -ret;
		}
		*(float*)destbuffer = m_Y;
		ret += sizeof(m_Y);
		destbuffer += sizeof(m_Y);
		 
		 
		// 线路	
		if (ret + (int32_t)sizeof(m_Line) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_Line;
		ret += sizeof(m_Line);
		destbuffer += sizeof(m_Line);
		 
		 
		// 场景id	
		if (ret + (int32_t)sizeof(m_SceneId) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_SceneId;
		ret += sizeof(m_SceneId);
		destbuffer += sizeof(m_SceneId);
		 
		 
		// 场景guid	
		if (ret + (int32_t)sizeof(m_SceneGuid) > size)
		{
			return -ret;
		}
		*(uint64_t*)destbuffer = m_SceneGuid;
		ret += sizeof(m_SceneGuid);
		destbuffer += sizeof(m_SceneGuid);
		 
		 
		// 好友信息	
		int32_t retFriendsInfo = m_FriendsInfo.Marshal(destbuffer, size - ret);
		if (retFriendsInfo <= 0)
		{
			return -ret + retFriendsInfo;
		}
		ret += retFriendsInfo;
		destbuffer += retFriendsInfo;
		 
		 
		// 好友信息	
		int32_t retItemsInfo = m_ItemsInfo.Marshal(destbuffer, size - ret);
		if (retItemsInfo <= 0)
		{
			return -ret + retItemsInfo;
		}
		ret += retItemsInfo;
		destbuffer += retItemsInfo;
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 唯一id	
		if (ret + (int32_t)sizeof(m_Guid) > size)
		{
			return -ret;
		}
		m_Guid = *(uint64_t*)srcbuffer;
		ret += sizeof(m_Guid);
		srcbuffer += sizeof(m_Guid);
		 
		 
		// 用户名	
		if (ret + (int32_t)sizeof(m_Acc) > size)
		{
			return -ret;
		}
		fstrcopy(m_Acc, srcbuffer);
		ret += sizeof(m_Acc);
		srcbuffer += sizeof(m_Acc);
		 
		 
		// 角色名	
		if (ret + (int32_t)sizeof(m_Name) > size)
		{
			return -ret;
		}
		fstrcopy(m_Name, srcbuffer);
		ret += sizeof(m_Name);
		srcbuffer += sizeof(m_Name);
		 
		 
		// 血量	
		if (ret + (int32_t)sizeof(m_Hp) > size)
		{
			return -ret;
		}
		m_Hp = *(uint32_t*)srcbuffer;
		ret += sizeof(m_Hp);
		srcbuffer += sizeof(m_Hp);
		 
		 
		// 魔量	
		if (ret + (int32_t)sizeof(m_Mp) > size)
		{
			return -ret;
		}
		m_Mp = *(uint32_t*)srcbuffer;
		ret += sizeof(m_Mp);
		srcbuffer += sizeof(m_Mp);
		 
		 
		// x	
		if (ret + (int32_t)sizeof(m_X) > size)
		{
			return -ret;
		}
		m_X = *(float*)srcbuffer;
		ret += sizeof(m_X);
		srcbuffer += sizeof(m_X);
		 
		 
		// y	
		if (ret + (int32_t)sizeof(m_Y) > size)
		{
			return -ret;
		}
		m_Y = *(float*)srcbuffer;
		ret += sizeof(m_Y);
		srcbuffer += sizeof(m_Y);
		 
		 
		// 线路	
		if (ret + (int32_t)sizeof(m_Line) > size)
		{
			return -ret;
		}
		m_Line = *(uint32_t*)srcbuffer;
		ret += sizeof(m_Line);
		srcbuffer += sizeof(m_Line);
		 
		 
		// 场景id	
		if (ret + (int32_t)sizeof(m_SceneId) > size)
		{
			return -ret;
		}
		m_SceneId = *(uint32_t*)srcbuffer;
		ret += sizeof(m_SceneId);
		srcbuffer += sizeof(m_SceneId);
		 
		 
		// 场景guid	
		if (ret + (int32_t)sizeof(m_SceneGuid) > size)
		{
			return -ret;
		}
		m_SceneGuid = *(uint64_t*)srcbuffer;
		ret += sizeof(m_SceneGuid);
		srcbuffer += sizeof(m_SceneGuid);
		 
		 
		// 好友信息	
		int32_t retFriendsInfo = m_FriendsInfo.Unmarshal(srcbuffer, size - ret);
		if (retFriendsInfo <= 0)
		{
			return -ret + retFriendsInfo;
		}
		ret += retFriendsInfo;
		srcbuffer += retFriendsInfo;
		 
		 
		// 好友信息	
		int32_t retItemsInfo = m_ItemsInfo.Unmarshal(srcbuffer, size - ret);
		if (retItemsInfo <= 0)
		{
			return -ret + retItemsInfo;
		}
		ret += retItemsInfo;
		srcbuffer += retItemsInfo;
		 
		 
		return ret;
	}
	
};  
       
// 请求登录  
enum E_CSReqLogin  
{
	
	 
	// 用户名  
	E_CSReqLogin_Acc,
	 
	// 密码  
	E_CSReqLogin_Pwd,
	  
	
};

// 请求登录  
struct CSReqLogin  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 用户名  
	char m_Acc[ACC_LEN];  
	  
	// 密码  
	char m_Pwd[PWD_LEN];  
	  
	
	
	
	// 用户名  
	const char * GetAcc()
	{
		return m_Acc;
	}
	
	// 用户名  
	void SetAcc(const char * _Acc)
	{
		fstrcopy(m_Acc, _Acc);
	}
	
	
	// 密码  
	const char * GetPwd()
	{
		return m_Pwd;
	}
	
	// 密码  
	void SetPwd(const char * _Pwd)
	{
		fstrcopy(m_Pwd, _Pwd);
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 用户名	
		if (ret + (int32_t)sizeof(m_Acc) > size)
		{
			return -ret;
		}
		fstrcopy(destbuffer, m_Acc, sizeof(m_Acc));
		ret += sizeof(m_Acc);
		destbuffer += sizeof(m_Acc);
		 
		 
		// 密码	
		if (ret + (int32_t)sizeof(m_Pwd) > size)
		{
			return -ret;
		}
		fstrcopy(destbuffer, m_Pwd, sizeof(m_Pwd));
		ret += sizeof(m_Pwd);
		destbuffer += sizeof(m_Pwd);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 用户名	
		if (ret + (int32_t)sizeof(m_Acc) > size)
		{
			return -ret;
		}
		fstrcopy(m_Acc, srcbuffer);
		ret += sizeof(m_Acc);
		srcbuffer += sizeof(m_Acc);
		 
		 
		// 密码	
		if (ret + (int32_t)sizeof(m_Pwd) > size)
		{
			return -ret;
		}
		fstrcopy(m_Pwd, srcbuffer);
		ret += sizeof(m_Pwd);
		srcbuffer += sizeof(m_Pwd);
		 
		 
		return ret;
	}
	
};  
       
// 返回登录  
enum E_SCResLogin  
{
	
	 
	// 结果，0成功-1创建账号失败-2加载角色失败  
	E_SCResLogin_Ret,
	 
	// 账号信息  
	E_SCResLogin_AccInfo,
	 
	// 玩家数目  
	E_SCResLogin_RoleInfoNum,
	 
	// 玩家信息  
	E_SCResLogin_RoleInfo,
	  
	
};

// 返回登录  
struct SCResLogin  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 结果，0成功-1创建账号失败-2加载角色失败  
	uint8_t m_Ret;  
	  
	// 账号信息  
	AccInfo m_AccInfo;  
	  
	// 玩家数目  
	uint8_t m_RoleInfoNum;  
	  
	// 玩家信息  
	RoleInfo m_RoleInfo[ROLE_INFO_NUM];  
	  
	
	
	
	// 结果，0成功-1创建账号失败-2加载角色失败  
	uint8_t GetRet()
	{
		return m_Ret;
	}
	
	// 结果，0成功-1创建账号失败-2加载角色失败  
	void SetRet(uint8_t _Ret)
	{
		m_Ret = _Ret;
	}
	
	
	// 账号信息  
	AccInfo * GetAccInfo()
	{
		return &m_AccInfo;
	}
	
	// 账号信息  
	void SetAccInfo(AccInfo * pAccInfo)
	{
		if (pAccInfo)
		{
			m_AccInfo = *pAccInfo;
		}
	}
	
	
	// 玩家数目  
	uint8_t GetRoleInfoNum()
	{
		return m_RoleInfoNum;
	}
	
	// 玩家数目  
	void SetRoleInfoNum(uint8_t _RoleInfoNum)
	{
		m_RoleInfoNum = _RoleInfoNum;
	}
	
	
	// 玩家信息  
	RoleInfo * GetRoleInfo(int i)
	{
		assert(i >= 0 && i < ROLE_INFO_NUM);
		if (i >= 0 && i < ROLE_INFO_NUM)
		{
			return &m_RoleInfo[i];
		}
		return &m_RoleInfo[0];
	}
	
	// 玩家信息  
	void SetRoleInfo(int i, RoleInfo * pRoleInfo)
	{
		assert(i >= 0 && i < ROLE_INFO_NUM);
		if (i >= 0 && i < ROLE_INFO_NUM && pRoleInfo)
		{
			m_RoleInfo[i] = *pRoleInfo;
		}
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 结果，0成功-1创建账号失败-2加载角色失败	
		if (ret + (int32_t)sizeof(m_Ret) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_Ret;
		ret += sizeof(m_Ret);
		destbuffer += sizeof(m_Ret);
		 
		 
		// 账号信息	
		int32_t retAccInfo = m_AccInfo.Marshal(destbuffer, size - ret);
		if (retAccInfo <= 0)
		{
			return -ret + retAccInfo;
		}
		ret += retAccInfo;
		destbuffer += retAccInfo;
		 
		 
		// 玩家数目	
		if (ret + (int32_t)sizeof(m_RoleInfoNum) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_RoleInfoNum;
		ret += sizeof(m_RoleInfoNum);
		destbuffer += sizeof(m_RoleInfoNum);
		 
		 
		// 玩家信息	
		for (int32_t i = 0; i < ROLE_INFO_NUM && i < m_RoleInfoNum; i++)
		{
			int32_t retRoleInfo = m_RoleInfo[i].Marshal(destbuffer, size - ret);
			if (retRoleInfo <= 0)
			{
				return -ret + retRoleInfo;
			}
			ret += retRoleInfo;
			destbuffer += retRoleInfo;
		}
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 结果，0成功-1创建账号失败-2加载角色失败	
		if (ret + (int32_t)sizeof(m_Ret) > size)
		{
			return -ret;
		}
		m_Ret = *(uint8_t*)srcbuffer;
		ret += sizeof(m_Ret);
		srcbuffer += sizeof(m_Ret);
		 
		 
		// 账号信息	
		int32_t retAccInfo = m_AccInfo.Unmarshal(srcbuffer, size - ret);
		if (retAccInfo <= 0)
		{
			return -ret + retAccInfo;
		}
		ret += retAccInfo;
		srcbuffer += retAccInfo;
		 
		 
		// 玩家数目	
		if (ret + (int32_t)sizeof(m_RoleInfoNum) > size)
		{
			return -ret;
		}
		m_RoleInfoNum = *(uint8_t*)srcbuffer;
		ret += sizeof(m_RoleInfoNum);
		srcbuffer += sizeof(m_RoleInfoNum);
		 
		 
		// 玩家信息	
		for (int32_t i = 0; i < ROLE_INFO_NUM && i < m_RoleInfoNum; i++)
		{
			int32_t retRoleInfo = m_RoleInfo[i].Unmarshal(srcbuffer, size - ret);
			if (retRoleInfo <= 0)
			{
				return -ret + retRoleInfo;
			}
			ret += retRoleInfo;
			srcbuffer += retRoleInfo;
		}
		m_RoleInfoNum = PROTO_MIN(m_RoleInfoNum, ROLE_INFO_NUM);
		 
		 
		return ret;
	}
	
};  
       
// 请求创建角色  
enum E_CSReqCreateRole  
{
	
	 
	// 用户名  
	E_CSReqCreateRole_Acc,
	 
	// 角色名  
	E_CSReqCreateRole_Name,
	 
	// 职业  
	E_CSReqCreateRole_Occupation,
	  
	
};

// 请求创建角色  
struct CSReqCreateRole  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 用户名  
	char m_Acc[ACC_LEN];  
	  
	// 角色名  
	char m_Name[ROLE_NAME_MAX_LEN];  
	  
	// 职业  
	int8_t m_Occupation;  
	  
	
	
	
	// 用户名  
	const char * GetAcc()
	{
		return m_Acc;
	}
	
	// 用户名  
	void SetAcc(const char * _Acc)
	{
		fstrcopy(m_Acc, _Acc);
	}
	
	
	// 角色名  
	const char * GetName()
	{
		return m_Name;
	}
	
	// 角色名  
	void SetName(const char * _Name)
	{
		fstrcopy(m_Name, _Name);
	}
	
	
	// 职业  
	int8_t GetOccupation()
	{
		return m_Occupation;
	}
	
	// 职业  
	void SetOccupation(int8_t _Occupation)
	{
		m_Occupation = _Occupation;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 用户名	
		if (ret + (int32_t)sizeof(m_Acc) > size)
		{
			return -ret;
		}
		fstrcopy(destbuffer, m_Acc, sizeof(m_Acc));
		ret += sizeof(m_Acc);
		destbuffer += sizeof(m_Acc);
		 
		 
		// 角色名	
		if (ret + (int32_t)sizeof(m_Name) > size)
		{
			return -ret;
		}
		fstrcopy(destbuffer, m_Name, sizeof(m_Name));
		ret += sizeof(m_Name);
		destbuffer += sizeof(m_Name);
		 
		 
		// 职业	
		if (ret + (int32_t)sizeof(m_Occupation) > size)
		{
			return -ret;
		}
		*(int8_t*)destbuffer = m_Occupation;
		ret += sizeof(m_Occupation);
		destbuffer += sizeof(m_Occupation);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 用户名	
		if (ret + (int32_t)sizeof(m_Acc) > size)
		{
			return -ret;
		}
		fstrcopy(m_Acc, srcbuffer);
		ret += sizeof(m_Acc);
		srcbuffer += sizeof(m_Acc);
		 
		 
		// 角色名	
		if (ret + (int32_t)sizeof(m_Name) > size)
		{
			return -ret;
		}
		fstrcopy(m_Name, srcbuffer);
		ret += sizeof(m_Name);
		srcbuffer += sizeof(m_Name);
		 
		 
		// 职业	
		if (ret + (int32_t)sizeof(m_Occupation) > size)
		{
			return -ret;
		}
		m_Occupation = *(int8_t*)srcbuffer;
		ret += sizeof(m_Occupation);
		srcbuffer += sizeof(m_Occupation);
		 
		 
		return ret;
	}
	
};  
       
// 返回创建角色  
enum E_SCResCreateRole  
{
	
	 
	// 结果，0成功-1已经有角色了-2创建角色失败-3更新账号失败  
	E_SCResCreateRole_Ret,
	 
	// 玩家数目  
	E_SCResCreateRole_RoleInfoNum,
	 
	// 玩家信息  
	E_SCResCreateRole_RoleInfo,
	  
	
};

// 返回创建角色  
struct SCResCreateRole  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 结果，0成功-1已经有角色了-2创建角色失败-3更新账号失败  
	uint8_t m_Ret;  
	  
	// 玩家数目  
	uint8_t m_RoleInfoNum;  
	  
	// 玩家信息  
	RoleInfo m_RoleInfo[ROLE_INFO_NUM];  
	  
	
	
	
	// 结果，0成功-1已经有角色了-2创建角色失败-3更新账号失败  
	uint8_t GetRet()
	{
		return m_Ret;
	}
	
	// 结果，0成功-1已经有角色了-2创建角色失败-3更新账号失败  
	void SetRet(uint8_t _Ret)
	{
		m_Ret = _Ret;
	}
	
	
	// 玩家数目  
	uint8_t GetRoleInfoNum()
	{
		return m_RoleInfoNum;
	}
	
	// 玩家数目  
	void SetRoleInfoNum(uint8_t _RoleInfoNum)
	{
		m_RoleInfoNum = _RoleInfoNum;
	}
	
	
	// 玩家信息  
	RoleInfo * GetRoleInfo(int i)
	{
		assert(i >= 0 && i < ROLE_INFO_NUM);
		if (i >= 0 && i < ROLE_INFO_NUM)
		{
			return &m_RoleInfo[i];
		}
		return &m_RoleInfo[0];
	}
	
	// 玩家信息  
	void SetRoleInfo(int i, RoleInfo * pRoleInfo)
	{
		assert(i >= 0 && i < ROLE_INFO_NUM);
		if (i >= 0 && i < ROLE_INFO_NUM && pRoleInfo)
		{
			m_RoleInfo[i] = *pRoleInfo;
		}
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 结果，0成功-1已经有角色了-2创建角色失败-3更新账号失败	
		if (ret + (int32_t)sizeof(m_Ret) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_Ret;
		ret += sizeof(m_Ret);
		destbuffer += sizeof(m_Ret);
		 
		 
		// 玩家数目	
		if (ret + (int32_t)sizeof(m_RoleInfoNum) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_RoleInfoNum;
		ret += sizeof(m_RoleInfoNum);
		destbuffer += sizeof(m_RoleInfoNum);
		 
		 
		// 玩家信息	
		for (int32_t i = 0; i < ROLE_INFO_NUM && i < m_RoleInfoNum; i++)
		{
			int32_t retRoleInfo = m_RoleInfo[i].Marshal(destbuffer, size - ret);
			if (retRoleInfo <= 0)
			{
				return -ret + retRoleInfo;
			}
			ret += retRoleInfo;
			destbuffer += retRoleInfo;
		}
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 结果，0成功-1已经有角色了-2创建角色失败-3更新账号失败	
		if (ret + (int32_t)sizeof(m_Ret) > size)
		{
			return -ret;
		}
		m_Ret = *(uint8_t*)srcbuffer;
		ret += sizeof(m_Ret);
		srcbuffer += sizeof(m_Ret);
		 
		 
		// 玩家数目	
		if (ret + (int32_t)sizeof(m_RoleInfoNum) > size)
		{
			return -ret;
		}
		m_RoleInfoNum = *(uint8_t*)srcbuffer;
		ret += sizeof(m_RoleInfoNum);
		srcbuffer += sizeof(m_RoleInfoNum);
		 
		 
		// 玩家信息	
		for (int32_t i = 0; i < ROLE_INFO_NUM && i < m_RoleInfoNum; i++)
		{
			int32_t retRoleInfo = m_RoleInfo[i].Unmarshal(srcbuffer, size - ret);
			if (retRoleInfo <= 0)
			{
				return -ret + retRoleInfo;
			}
			ret += retRoleInfo;
			srcbuffer += retRoleInfo;
		}
		m_RoleInfoNum = PROTO_MIN(m_RoleInfoNum, ROLE_INFO_NUM);
		 
		 
		return ret;
	}
	
};  
       
// 请求登入游戏  
enum E_CSReqEnter  
{
	
	 
	// 占位  
	E_CSReqEnter_tmp,
	  
	
};

// 请求登入游戏  
struct CSReqEnter  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 占位  
	uint8_t m_tmp;  
	  
	
	
	
	// 占位  
	uint8_t Gettmp()
	{
		return m_tmp;
	}
	
	// 占位  
	void Settmp(uint8_t _tmp)
	{
		m_tmp = _tmp;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 占位	
		if (ret + (int32_t)sizeof(m_tmp) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_tmp;
		ret += sizeof(m_tmp);
		destbuffer += sizeof(m_tmp);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 占位	
		if (ret + (int32_t)sizeof(m_tmp) > size)
		{
			return -ret;
		}
		m_tmp = *(uint8_t*)srcbuffer;
		ret += sizeof(m_tmp);
		srcbuffer += sizeof(m_tmp);
		 
		 
		return ret;
	}
	
};  
       
// 返回登入游戏  
enum E_SCResEnter  
{
	
	 
	// 结果，0成功-1加载角色失败  
	E_SCResEnter_Ret,
	 
	// 玩家数目  
	E_SCResEnter_RoleInfoNum,
	 
	// 玩家信息  
	E_SCResEnter_RoleInfo,
	  
	
};

// 返回登入游戏  
struct SCResEnter  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 结果，0成功-1加载角色失败  
	uint8_t m_Ret;  
	  
	// 玩家数目  
	uint8_t m_RoleInfoNum;  
	  
	// 玩家信息  
	RoleInfo m_RoleInfo[ROLE_INFO_NUM];  
	  
	
	
	
	// 结果，0成功-1加载角色失败  
	uint8_t GetRet()
	{
		return m_Ret;
	}
	
	// 结果，0成功-1加载角色失败  
	void SetRet(uint8_t _Ret)
	{
		m_Ret = _Ret;
	}
	
	
	// 玩家数目  
	uint8_t GetRoleInfoNum()
	{
		return m_RoleInfoNum;
	}
	
	// 玩家数目  
	void SetRoleInfoNum(uint8_t _RoleInfoNum)
	{
		m_RoleInfoNum = _RoleInfoNum;
	}
	
	
	// 玩家信息  
	RoleInfo * GetRoleInfo(int i)
	{
		assert(i >= 0 && i < ROLE_INFO_NUM);
		if (i >= 0 && i < ROLE_INFO_NUM)
		{
			return &m_RoleInfo[i];
		}
		return &m_RoleInfo[0];
	}
	
	// 玩家信息  
	void SetRoleInfo(int i, RoleInfo * pRoleInfo)
	{
		assert(i >= 0 && i < ROLE_INFO_NUM);
		if (i >= 0 && i < ROLE_INFO_NUM && pRoleInfo)
		{
			m_RoleInfo[i] = *pRoleInfo;
		}
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 结果，0成功-1加载角色失败	
		if (ret + (int32_t)sizeof(m_Ret) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_Ret;
		ret += sizeof(m_Ret);
		destbuffer += sizeof(m_Ret);
		 
		 
		// 玩家数目	
		if (ret + (int32_t)sizeof(m_RoleInfoNum) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_RoleInfoNum;
		ret += sizeof(m_RoleInfoNum);
		destbuffer += sizeof(m_RoleInfoNum);
		 
		 
		// 玩家信息	
		for (int32_t i = 0; i < ROLE_INFO_NUM && i < m_RoleInfoNum; i++)
		{
			int32_t retRoleInfo = m_RoleInfo[i].Marshal(destbuffer, size - ret);
			if (retRoleInfo <= 0)
			{
				return -ret + retRoleInfo;
			}
			ret += retRoleInfo;
			destbuffer += retRoleInfo;
		}
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 结果，0成功-1加载角色失败	
		if (ret + (int32_t)sizeof(m_Ret) > size)
		{
			return -ret;
		}
		m_Ret = *(uint8_t*)srcbuffer;
		ret += sizeof(m_Ret);
		srcbuffer += sizeof(m_Ret);
		 
		 
		// 玩家数目	
		if (ret + (int32_t)sizeof(m_RoleInfoNum) > size)
		{
			return -ret;
		}
		m_RoleInfoNum = *(uint8_t*)srcbuffer;
		ret += sizeof(m_RoleInfoNum);
		srcbuffer += sizeof(m_RoleInfoNum);
		 
		 
		// 玩家信息	
		for (int32_t i = 0; i < ROLE_INFO_NUM && i < m_RoleInfoNum; i++)
		{
			int32_t retRoleInfo = m_RoleInfo[i].Unmarshal(srcbuffer, size - ret);
			if (retRoleInfo <= 0)
			{
				return -ret + retRoleInfo;
			}
			ret += retRoleInfo;
			srcbuffer += retRoleInfo;
		}
		m_RoleInfoNum = PROTO_MIN(m_RoleInfoNum, ROLE_INFO_NUM);
		 
		 
		return ret;
	}
	
};  
       
// 怪物信息  
enum E_MonsterInfo  
{
	
	 
	// 唯一id  
	E_MonsterInfo_Guid,
	 
	// 怪物id  
	E_MonsterInfo_Id,
	 
	// X  
	E_MonsterInfo_X,
	 
	// Y  
	E_MonsterInfo_Y,
	 
	// 线路  
	E_MonsterInfo_Line,
	 
	// 场景id  
	E_MonsterInfo_SceneId,
	 
	// 场景guid  
	E_MonsterInfo_SceneGuid,
	  
	
};

// 怪物信息  
struct MonsterInfo  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 唯一id  
	uint64_t m_Guid;  
	  
	// 怪物id  
	uint32_t m_Id;  
	  
	// X  
	uint32_t m_X;  
	  
	// Y  
	uint32_t m_Y;  
	  
	// 线路  
	uint32_t m_Line;  
	  
	// 场景id  
	uint32_t m_SceneId;  
	  
	// 场景guid  
	uint64_t m_SceneGuid;  
	  
	
	
	
	// 唯一id  
	uint64_t GetGuid()
	{
		return m_Guid;
	}
	
	// 唯一id  
	void SetGuid(uint64_t _Guid)
	{
		m_Guid = _Guid;
	}
	
	
	// 怪物id  
	uint32_t GetId()
	{
		return m_Id;
	}
	
	// 怪物id  
	void SetId(uint32_t _Id)
	{
		m_Id = _Id;
	}
	
	
	// X  
	uint32_t GetX()
	{
		return m_X;
	}
	
	// X  
	void SetX(uint32_t _X)
	{
		m_X = _X;
	}
	
	
	// Y  
	uint32_t GetY()
	{
		return m_Y;
	}
	
	// Y  
	void SetY(uint32_t _Y)
	{
		m_Y = _Y;
	}
	
	
	// 线路  
	uint32_t GetLine()
	{
		return m_Line;
	}
	
	// 线路  
	void SetLine(uint32_t _Line)
	{
		m_Line = _Line;
	}
	
	
	// 场景id  
	uint32_t GetSceneId()
	{
		return m_SceneId;
	}
	
	// 场景id  
	void SetSceneId(uint32_t _SceneId)
	{
		m_SceneId = _SceneId;
	}
	
	
	// 场景guid  
	uint64_t GetSceneGuid()
	{
		return m_SceneGuid;
	}
	
	// 场景guid  
	void SetSceneGuid(uint64_t _SceneGuid)
	{
		m_SceneGuid = _SceneGuid;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 唯一id	
		if (ret + (int32_t)sizeof(m_Guid) > size)
		{
			return -ret;
		}
		*(uint64_t*)destbuffer = m_Guid;
		ret += sizeof(m_Guid);
		destbuffer += sizeof(m_Guid);
		 
		 
		// 怪物id	
		if (ret + (int32_t)sizeof(m_Id) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_Id;
		ret += sizeof(m_Id);
		destbuffer += sizeof(m_Id);
		 
		 
		// X	
		if (ret + (int32_t)sizeof(m_X) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_X;
		ret += sizeof(m_X);
		destbuffer += sizeof(m_X);
		 
		 
		// Y	
		if (ret + (int32_t)sizeof(m_Y) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_Y;
		ret += sizeof(m_Y);
		destbuffer += sizeof(m_Y);
		 
		 
		// 线路	
		if (ret + (int32_t)sizeof(m_Line) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_Line;
		ret += sizeof(m_Line);
		destbuffer += sizeof(m_Line);
		 
		 
		// 场景id	
		if (ret + (int32_t)sizeof(m_SceneId) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_SceneId;
		ret += sizeof(m_SceneId);
		destbuffer += sizeof(m_SceneId);
		 
		 
		// 场景guid	
		if (ret + (int32_t)sizeof(m_SceneGuid) > size)
		{
			return -ret;
		}
		*(uint64_t*)destbuffer = m_SceneGuid;
		ret += sizeof(m_SceneGuid);
		destbuffer += sizeof(m_SceneGuid);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 唯一id	
		if (ret + (int32_t)sizeof(m_Guid) > size)
		{
			return -ret;
		}
		m_Guid = *(uint64_t*)srcbuffer;
		ret += sizeof(m_Guid);
		srcbuffer += sizeof(m_Guid);
		 
		 
		// 怪物id	
		if (ret + (int32_t)sizeof(m_Id) > size)
		{
			return -ret;
		}
		m_Id = *(uint32_t*)srcbuffer;
		ret += sizeof(m_Id);
		srcbuffer += sizeof(m_Id);
		 
		 
		// X	
		if (ret + (int32_t)sizeof(m_X) > size)
		{
			return -ret;
		}
		m_X = *(uint32_t*)srcbuffer;
		ret += sizeof(m_X);
		srcbuffer += sizeof(m_X);
		 
		 
		// Y	
		if (ret + (int32_t)sizeof(m_Y) > size)
		{
			return -ret;
		}
		m_Y = *(uint32_t*)srcbuffer;
		ret += sizeof(m_Y);
		srcbuffer += sizeof(m_Y);
		 
		 
		// 线路	
		if (ret + (int32_t)sizeof(m_Line) > size)
		{
			return -ret;
		}
		m_Line = *(uint32_t*)srcbuffer;
		ret += sizeof(m_Line);
		srcbuffer += sizeof(m_Line);
		 
		 
		// 场景id	
		if (ret + (int32_t)sizeof(m_SceneId) > size)
		{
			return -ret;
		}
		m_SceneId = *(uint32_t*)srcbuffer;
		ret += sizeof(m_SceneId);
		srcbuffer += sizeof(m_SceneId);
		 
		 
		// 场景guid	
		if (ret + (int32_t)sizeof(m_SceneGuid) > size)
		{
			return -ret;
		}
		m_SceneGuid = *(uint64_t*)srcbuffer;
		ret += sizeof(m_SceneGuid);
		srcbuffer += sizeof(m_SceneGuid);
		 
		 
		return ret;
	}
	
};  
       
// 移动信息  
enum E_MovePoint  
{
	
	 
	// X  
	E_MovePoint_X,
	 
	// Y  
	E_MovePoint_Y,
	  
	
};

// 移动信息  
struct MovePoint  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// X  
	uint32_t m_X;  
	  
	// Y  
	uint32_t m_Y;  
	  
	
	
	
	// X  
	uint32_t GetX()
	{
		return m_X;
	}
	
	// X  
	void SetX(uint32_t _X)
	{
		m_X = _X;
	}
	
	
	// Y  
	uint32_t GetY()
	{
		return m_Y;
	}
	
	// Y  
	void SetY(uint32_t _Y)
	{
		m_Y = _Y;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// X	
		if (ret + (int32_t)sizeof(m_X) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_X;
		ret += sizeof(m_X);
		destbuffer += sizeof(m_X);
		 
		 
		// Y	
		if (ret + (int32_t)sizeof(m_Y) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_Y;
		ret += sizeof(m_Y);
		destbuffer += sizeof(m_Y);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// X	
		if (ret + (int32_t)sizeof(m_X) > size)
		{
			return -ret;
		}
		m_X = *(uint32_t*)srcbuffer;
		ret += sizeof(m_X);
		srcbuffer += sizeof(m_X);
		 
		 
		// Y	
		if (ret + (int32_t)sizeof(m_Y) > size)
		{
			return -ret;
		}
		m_Y = *(uint32_t*)srcbuffer;
		ret += sizeof(m_Y);
		srcbuffer += sizeof(m_Y);
		 
		 
		return ret;
	}
	
};  
       
// 移动  
enum E_MoveData  
{
	
	 
	// 数目  
	E_MoveData_Num,
	 
	// 移动信息  
	E_MoveData_MovePoint,
	  
	
};

// 移动  
struct MoveData  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 数目  
	uint8_t m_Num;  
	  
	// 移动信息  
	MovePoint m_MovePoint[MOVE_DATA_NUM];  
	  
	
	
	
	// 数目  
	uint8_t GetNum()
	{
		return m_Num;
	}
	
	// 数目  
	void SetNum(uint8_t _Num)
	{
		m_Num = _Num;
	}
	
	
	// 移动信息  
	MovePoint * GetMovePoint(int i)
	{
		assert(i >= 0 && i < MOVE_DATA_NUM);
		if (i >= 0 && i < MOVE_DATA_NUM)
		{
			return &m_MovePoint[i];
		}
		return &m_MovePoint[0];
	}
	
	// 移动信息  
	void SetMovePoint(int i, MovePoint * pMovePoint)
	{
		assert(i >= 0 && i < MOVE_DATA_NUM);
		if (i >= 0 && i < MOVE_DATA_NUM && pMovePoint)
		{
			m_MovePoint[i] = *pMovePoint;
		}
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 数目	
		if (ret + (int32_t)sizeof(m_Num) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_Num;
		ret += sizeof(m_Num);
		destbuffer += sizeof(m_Num);
		 
		 
		// 移动信息	
		for (int32_t i = 0; i < MOVE_DATA_NUM && i < m_Num; i++)
		{
			int32_t retMovePoint = m_MovePoint[i].Marshal(destbuffer, size - ret);
			if (retMovePoint <= 0)
			{
				return -ret + retMovePoint;
			}
			ret += retMovePoint;
			destbuffer += retMovePoint;
		}
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 数目	
		if (ret + (int32_t)sizeof(m_Num) > size)
		{
			return -ret;
		}
		m_Num = *(uint8_t*)srcbuffer;
		ret += sizeof(m_Num);
		srcbuffer += sizeof(m_Num);
		 
		 
		// 移动信息	
		for (int32_t i = 0; i < MOVE_DATA_NUM && i < m_Num; i++)
		{
			int32_t retMovePoint = m_MovePoint[i].Unmarshal(srcbuffer, size - ret);
			if (retMovePoint <= 0)
			{
				return -ret + retMovePoint;
			}
			ret += retMovePoint;
			srcbuffer += retMovePoint;
		}
		m_Num = PROTO_MIN(m_Num, MOVE_DATA_NUM);
		 
		 
		return ret;
	}
	
};  
       
// 变更的数据 进入视野  
enum E_EventDataEnter  
{
	
	// Type
	E_EventDataEnter_Type,
	 
	// 玩家信息  
	E_EventDataEnter_RoleInfo,
	 
	// 怪物信息  
	E_EventDataEnter_MonsterInfo,
	  
	
};

// 变更的数据 进入视野  
struct EventDataEnter  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	// Type 
	int32_t m_Type;
	
	union
	{
		  
		// 玩家信息  
		RoleInfo m_RoleInfo;  
		  
		// 怪物信息  
		MonsterInfo m_MonsterInfo;  
		  
	};
	
	int32_t GetType()
	{
		return m_Type;
	}
	
	void SetType(int32_t type)
	{
		m_Type = type;
	}
	
	  
	// 玩家信息  
	RoleInfo * GetRoleInfo()
	{
		return &m_RoleInfo;
	}
	
	// 玩家信息  
	void SetRoleInfo(RoleInfo * pRoleInfo)
	{
		if (pRoleInfo)
		{
			m_RoleInfo = *pRoleInfo;
		}
	}
	  
	// 怪物信息  
	MonsterInfo * GetMonsterInfo()
	{
		return &m_MonsterInfo;
	}
	
	// 怪物信息  
	void SetMonsterInfo(MonsterInfo * pMonsterInfo)
	{
		if (pMonsterInfo)
		{
			m_MonsterInfo = *pMonsterInfo;
		}
	}
	  
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		
		// type
		if (ret + (int32_t)sizeof(m_Type) > size)
		{
			return -ret;
		}
		*(int32_t*)destbuffer = m_Type;
		ret += sizeof(m_Type);
		destbuffer += sizeof(m_Type);
		
		switch (m_Type)
		{
		  
		// 玩家信息  
		case EDET_Human:
			{
				int32_t retRoleInfo = m_RoleInfo.Marshal(destbuffer, size - ret);
				if (retRoleInfo <= 0)
				{
					return -ret + retRoleInfo;
				}
				ret += retRoleInfo;
				destbuffer += retRoleInfo;
			}
			break;
		  
		// 怪物信息  
		case EDET_Monster:
			{
				int32_t retMonsterInfo = m_MonsterInfo.Marshal(destbuffer, size - ret);
				if (retMonsterInfo <= 0)
				{
					return -ret + retMonsterInfo;
				}
				ret += retMonsterInfo;
				destbuffer += retMonsterInfo;
			}
			break;
		  
		default:
			return -ret;
		}
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		
		// type
		if (ret + (int32_t)sizeof(m_Type) > size)
		{
			return -ret;
		}
		m_Type = *(int32_t*)srcbuffer;
		ret += sizeof(m_Type);
		srcbuffer += sizeof(m_Type);
		
		switch (m_Type)
		{
		  
		// 玩家信息  
		case EDET_Human:
			{
				int32_t retRoleInfo = m_RoleInfo.Unmarshal(srcbuffer, size - ret);
				if (retRoleInfo <= 0)
				{
					return -ret + retRoleInfo;
				}
				ret += retRoleInfo;
				srcbuffer += retRoleInfo;
			}
			break;
		  
		// 怪物信息  
		case EDET_Monster:
			{
				int32_t retMonsterInfo = m_MonsterInfo.Unmarshal(srcbuffer, size - ret);
				if (retMonsterInfo <= 0)
				{
					return -ret + retMonsterInfo;
				}
				ret += retMonsterInfo;
				srcbuffer += retMonsterInfo;
			}
			break;
		  
		default:
			return -ret;
		}
		return ret;
	}
	
	
};  
       
// 变更的数据 离开视野  
enum E_EventDataLeave  
{
	
	 
	// 占位  
	E_EventDataLeave_tmp,
	  
	
};

// 变更的数据 离开视野  
struct EventDataLeave  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 占位  
	uint8_t m_tmp;  
	  
	
	
	
	// 占位  
	uint8_t Gettmp()
	{
		return m_tmp;
	}
	
	// 占位  
	void Settmp(uint8_t _tmp)
	{
		m_tmp = _tmp;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 占位	
		if (ret + (int32_t)sizeof(m_tmp) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_tmp;
		ret += sizeof(m_tmp);
		destbuffer += sizeof(m_tmp);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 占位	
		if (ret + (int32_t)sizeof(m_tmp) > size)
		{
			return -ret;
		}
		m_tmp = *(uint8_t*)srcbuffer;
		ret += sizeof(m_tmp);
		srcbuffer += sizeof(m_tmp);
		 
		 
		return ret;
	}
	
};  
       
// 变更的数据 移动  
enum E_EventDataMove  
{
	
	 
	// 移动信息  
	E_EventDataMove_MoveData,
	  
	
};

// 变更的数据 移动  
struct EventDataMove  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 移动信息  
	MoveData m_MoveData;  
	  
	
	
	
	// 移动信息  
	MoveData * GetMoveData()
	{
		return &m_MoveData;
	}
	
	// 移动信息  
	void SetMoveData(MoveData * pMoveData)
	{
		if (pMoveData)
		{
			m_MoveData = *pMoveData;
		}
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 移动信息	
		int32_t retMoveData = m_MoveData.Marshal(destbuffer, size - ret);
		if (retMoveData <= 0)
		{
			return -ret + retMoveData;
		}
		ret += retMoveData;
		destbuffer += retMoveData;
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 移动信息	
		int32_t retMoveData = m_MoveData.Unmarshal(srcbuffer, size - ret);
		if (retMoveData <= 0)
		{
			return -ret + retMoveData;
		}
		ret += retMoveData;
		srcbuffer += retMoveData;
		 
		 
		return ret;
	}
	
};  
       
// 变更的数据 属性变化  
enum E_EventDataProperty  
{
	
	 
	// 属性ID  
	E_EventDataProperty_PropertyID,
	 
	// 属性值  
	E_EventDataProperty_PropertyValue,
	  
	
};

// 变更的数据 属性变化  
struct EventDataProperty  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 属性ID  
	uint16_t m_PropertyID;  
	  
	// 属性值  
	uint32_t m_PropertyValue;  
	  
	
	
	
	// 属性ID  
	uint16_t GetPropertyID()
	{
		return m_PropertyID;
	}
	
	// 属性ID  
	void SetPropertyID(uint16_t _PropertyID)
	{
		m_PropertyID = _PropertyID;
	}
	
	
	// 属性值  
	uint32_t GetPropertyValue()
	{
		return m_PropertyValue;
	}
	
	// 属性值  
	void SetPropertyValue(uint32_t _PropertyValue)
	{
		m_PropertyValue = _PropertyValue;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 属性ID	
		if (ret + (int32_t)sizeof(m_PropertyID) > size)
		{
			return -ret;
		}
		*(uint16_t*)destbuffer = m_PropertyID;
		ret += sizeof(m_PropertyID);
		destbuffer += sizeof(m_PropertyID);
		 
		 
		// 属性值	
		if (ret + (int32_t)sizeof(m_PropertyValue) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_PropertyValue;
		ret += sizeof(m_PropertyValue);
		destbuffer += sizeof(m_PropertyValue);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 属性ID	
		if (ret + (int32_t)sizeof(m_PropertyID) > size)
		{
			return -ret;
		}
		m_PropertyID = *(uint16_t*)srcbuffer;
		ret += sizeof(m_PropertyID);
		srcbuffer += sizeof(m_PropertyID);
		 
		 
		// 属性值	
		if (ret + (int32_t)sizeof(m_PropertyValue) > size)
		{
			return -ret;
		}
		m_PropertyValue = *(uint32_t*)srcbuffer;
		ret += sizeof(m_PropertyValue);
		srcbuffer += sizeof(m_PropertyValue);
		 
		 
		return ret;
	}
	
};  
       
// 变更的数据 释放技能  
enum E_EventDataSkillCast  
{
	
	 
	// 技能ID  
	E_EventDataSkillCast_SkillID,
	  
	
};

// 变更的数据 释放技能  
struct EventDataSkillCast  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 技能ID  
	uint32_t m_SkillID;  
	  
	
	
	
	// 技能ID  
	uint32_t GetSkillID()
	{
		return m_SkillID;
	}
	
	// 技能ID  
	void SetSkillID(uint32_t _SkillID)
	{
		m_SkillID = _SkillID;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 技能ID	
		if (ret + (int32_t)sizeof(m_SkillID) > size)
		{
			return -ret;
		}
		*(uint32_t*)destbuffer = m_SkillID;
		ret += sizeof(m_SkillID);
		destbuffer += sizeof(m_SkillID);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 技能ID	
		if (ret + (int32_t)sizeof(m_SkillID) > size)
		{
			return -ret;
		}
		m_SkillID = *(uint32_t*)srcbuffer;
		ret += sizeof(m_SkillID);
		srcbuffer += sizeof(m_SkillID);
		 
		 
		return ret;
	}
	
};  
       
// 变更的数据  
enum E_EventData  
{
	
	// Type
	E_EventData_Type,
	 
	// 进入视野  
	E_EventData_EventDataEnter,
	 
	// 离开视野  
	E_EventData_EventDataLeave,
	 
	// 移动  
	E_EventData_EventDataMove,
	 
	// 属性变化  
	E_EventData_EventDataProperty,
	 
	// 释放技能  
	E_EventData_EventDataSkillCast,
	  
	
};

// 变更的数据  
struct EventData  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	// Type 
	int32_t m_Type;
	
	union
	{
		  
		// 进入视野  
		EventDataEnter m_EventDataEnter;  
		  
		// 离开视野  
		EventDataLeave m_EventDataLeave;  
		  
		// 移动  
		EventDataMove m_EventDataMove;  
		  
		// 属性变化  
		EventDataProperty m_EventDataProperty;  
		  
		// 释放技能  
		EventDataSkillCast m_EventDataSkillCast;  
		  
	};
	
	int32_t GetType()
	{
		return m_Type;
	}
	
	void SetType(int32_t type)
	{
		m_Type = type;
	}
	
	  
	// 进入视野  
	EventDataEnter * GetEventDataEnter()
	{
		return &m_EventDataEnter;
	}
	
	// 进入视野  
	void SetEventDataEnter(EventDataEnter * pEventDataEnter)
	{
		if (pEventDataEnter)
		{
			m_EventDataEnter = *pEventDataEnter;
		}
	}
	  
	// 离开视野  
	EventDataLeave * GetEventDataLeave()
	{
		return &m_EventDataLeave;
	}
	
	// 离开视野  
	void SetEventDataLeave(EventDataLeave * pEventDataLeave)
	{
		if (pEventDataLeave)
		{
			m_EventDataLeave = *pEventDataLeave;
		}
	}
	  
	// 移动  
	EventDataMove * GetEventDataMove()
	{
		return &m_EventDataMove;
	}
	
	// 移动  
	void SetEventDataMove(EventDataMove * pEventDataMove)
	{
		if (pEventDataMove)
		{
			m_EventDataMove = *pEventDataMove;
		}
	}
	  
	// 属性变化  
	EventDataProperty * GetEventDataProperty()
	{
		return &m_EventDataProperty;
	}
	
	// 属性变化  
	void SetEventDataProperty(EventDataProperty * pEventDataProperty)
	{
		if (pEventDataProperty)
		{
			m_EventDataProperty = *pEventDataProperty;
		}
	}
	  
	// 释放技能  
	EventDataSkillCast * GetEventDataSkillCast()
	{
		return &m_EventDataSkillCast;
	}
	
	// 释放技能  
	void SetEventDataSkillCast(EventDataSkillCast * pEventDataSkillCast)
	{
		if (pEventDataSkillCast)
		{
			m_EventDataSkillCast = *pEventDataSkillCast;
		}
	}
	  
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		
		// type
		if (ret + (int32_t)sizeof(m_Type) > size)
		{
			return -ret;
		}
		*(int32_t*)destbuffer = m_Type;
		ret += sizeof(m_Type);
		destbuffer += sizeof(m_Type);
		
		switch (m_Type)
		{
		  
		// 进入视野  
		case EDT_Enter:
			{
				int32_t retEventDataEnter = m_EventDataEnter.Marshal(destbuffer, size - ret);
				if (retEventDataEnter <= 0)
				{
					return -ret + retEventDataEnter;
				}
				ret += retEventDataEnter;
				destbuffer += retEventDataEnter;
			}
			break;
		  
		// 离开视野  
		case EDT_Leave:
			{
				int32_t retEventDataLeave = m_EventDataLeave.Marshal(destbuffer, size - ret);
				if (retEventDataLeave <= 0)
				{
					return -ret + retEventDataLeave;
				}
				ret += retEventDataLeave;
				destbuffer += retEventDataLeave;
			}
			break;
		  
		// 移动  
		case EDT_Move:
			{
				int32_t retEventDataMove = m_EventDataMove.Marshal(destbuffer, size - ret);
				if (retEventDataMove <= 0)
				{
					return -ret + retEventDataMove;
				}
				ret += retEventDataMove;
				destbuffer += retEventDataMove;
			}
			break;
		  
		// 属性变化  
		case EDT_Property:
			{
				int32_t retEventDataProperty = m_EventDataProperty.Marshal(destbuffer, size - ret);
				if (retEventDataProperty <= 0)
				{
					return -ret + retEventDataProperty;
				}
				ret += retEventDataProperty;
				destbuffer += retEventDataProperty;
			}
			break;
		  
		// 释放技能  
		case EDT_SkillCast:
			{
				int32_t retEventDataSkillCast = m_EventDataSkillCast.Marshal(destbuffer, size - ret);
				if (retEventDataSkillCast <= 0)
				{
					return -ret + retEventDataSkillCast;
				}
				ret += retEventDataSkillCast;
				destbuffer += retEventDataSkillCast;
			}
			break;
		  
		default:
			return -ret;
		}
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		
		// type
		if (ret + (int32_t)sizeof(m_Type) > size)
		{
			return -ret;
		}
		m_Type = *(int32_t*)srcbuffer;
		ret += sizeof(m_Type);
		srcbuffer += sizeof(m_Type);
		
		switch (m_Type)
		{
		  
		// 进入视野  
		case EDT_Enter:
			{
				int32_t retEventDataEnter = m_EventDataEnter.Unmarshal(srcbuffer, size - ret);
				if (retEventDataEnter <= 0)
				{
					return -ret + retEventDataEnter;
				}
				ret += retEventDataEnter;
				srcbuffer += retEventDataEnter;
			}
			break;
		  
		// 离开视野  
		case EDT_Leave:
			{
				int32_t retEventDataLeave = m_EventDataLeave.Unmarshal(srcbuffer, size - ret);
				if (retEventDataLeave <= 0)
				{
					return -ret + retEventDataLeave;
				}
				ret += retEventDataLeave;
				srcbuffer += retEventDataLeave;
			}
			break;
		  
		// 移动  
		case EDT_Move:
			{
				int32_t retEventDataMove = m_EventDataMove.Unmarshal(srcbuffer, size - ret);
				if (retEventDataMove <= 0)
				{
					return -ret + retEventDataMove;
				}
				ret += retEventDataMove;
				srcbuffer += retEventDataMove;
			}
			break;
		  
		// 属性变化  
		case EDT_Property:
			{
				int32_t retEventDataProperty = m_EventDataProperty.Unmarshal(srcbuffer, size - ret);
				if (retEventDataProperty <= 0)
				{
					return -ret + retEventDataProperty;
				}
				ret += retEventDataProperty;
				srcbuffer += retEventDataProperty;
			}
			break;
		  
		// 释放技能  
		case EDT_SkillCast:
			{
				int32_t retEventDataSkillCast = m_EventDataSkillCast.Unmarshal(srcbuffer, size - ret);
				if (retEventDataSkillCast <= 0)
				{
					return -ret + retEventDataSkillCast;
				}
				ret += retEventDataSkillCast;
				srcbuffer += retEventDataSkillCast;
			}
			break;
		  
		default:
			return -ret;
		}
		return ret;
	}
	
	
};  
       
// 附近信息  
enum E_NearInfo  
{
	
	 
	// 唯一id  
	E_NearInfo_Guid,
	 
	// 数目  
	E_NearInfo_EventDataNum,
	 
	// 变更的数据  
	E_NearInfo_EventData,
	  
	
};

// 附近信息  
struct NearInfo  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 唯一id  
	uint64_t m_Guid;  
	  
	// 数目  
	uint8_t m_EventDataNum;  
	  
	// 变更的数据  
	EventData m_EventData[EVENT_DATA_NUM];  
	  
	
	
	
	// 唯一id  
	uint64_t GetGuid()
	{
		return m_Guid;
	}
	
	// 唯一id  
	void SetGuid(uint64_t _Guid)
	{
		m_Guid = _Guid;
	}
	
	
	// 数目  
	uint8_t GetEventDataNum()
	{
		return m_EventDataNum;
	}
	
	// 数目  
	void SetEventDataNum(uint8_t _EventDataNum)
	{
		m_EventDataNum = _EventDataNum;
	}
	
	
	// 变更的数据  
	EventData * GetEventData(int i)
	{
		assert(i >= 0 && i < EVENT_DATA_NUM);
		if (i >= 0 && i < EVENT_DATA_NUM)
		{
			return &m_EventData[i];
		}
		return &m_EventData[0];
	}
	
	// 变更的数据  
	void SetEventData(int i, EventData * pEventData)
	{
		assert(i >= 0 && i < EVENT_DATA_NUM);
		if (i >= 0 && i < EVENT_DATA_NUM && pEventData)
		{
			m_EventData[i] = *pEventData;
		}
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 唯一id	
		if (ret + (int32_t)sizeof(m_Guid) > size)
		{
			return -ret;
		}
		*(uint64_t*)destbuffer = m_Guid;
		ret += sizeof(m_Guid);
		destbuffer += sizeof(m_Guid);
		 
		 
		// 数目	
		if (ret + (int32_t)sizeof(m_EventDataNum) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_EventDataNum;
		ret += sizeof(m_EventDataNum);
		destbuffer += sizeof(m_EventDataNum);
		 
		 
		// 变更的数据	
		for (int32_t i = 0; i < EVENT_DATA_NUM && i < m_EventDataNum; i++)
		{
			int32_t retEventData = m_EventData[i].Marshal(destbuffer, size - ret);
			if (retEventData <= 0)
			{
				return -ret + retEventData;
			}
			ret += retEventData;
			destbuffer += retEventData;
		}
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 唯一id	
		if (ret + (int32_t)sizeof(m_Guid) > size)
		{
			return -ret;
		}
		m_Guid = *(uint64_t*)srcbuffer;
		ret += sizeof(m_Guid);
		srcbuffer += sizeof(m_Guid);
		 
		 
		// 数目	
		if (ret + (int32_t)sizeof(m_EventDataNum) > size)
		{
			return -ret;
		}
		m_EventDataNum = *(uint8_t*)srcbuffer;
		ret += sizeof(m_EventDataNum);
		srcbuffer += sizeof(m_EventDataNum);
		 
		 
		// 变更的数据	
		for (int32_t i = 0; i < EVENT_DATA_NUM && i < m_EventDataNum; i++)
		{
			int32_t retEventData = m_EventData[i].Unmarshal(srcbuffer, size - ret);
			if (retEventData <= 0)
			{
				return -ret + retEventData;
			}
			ret += retEventData;
			srcbuffer += retEventData;
		}
		m_EventDataNum = PROTO_MIN(m_EventDataNum, EVENT_DATA_NUM);
		 
		 
		return ret;
	}
	
};  
       
// 附近的信息  
enum E_SCNtyNear  
{
	
	 
	// 数目  
	E_SCNtyNear_NearInfoNum,
	 
	// 附近的信息  
	E_SCNtyNear_NearInfo,
	  
	
};

// 附近的信息  
struct SCNtyNear  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 数目  
	uint8_t m_NearInfoNum;  
	  
	// 附近的信息  
	NearInfo m_NearInfo[NEAR_INFO_NUM];  
	  
	
	
	
	// 数目  
	uint8_t GetNearInfoNum()
	{
		return m_NearInfoNum;
	}
	
	// 数目  
	void SetNearInfoNum(uint8_t _NearInfoNum)
	{
		m_NearInfoNum = _NearInfoNum;
	}
	
	
	// 附近的信息  
	NearInfo * GetNearInfo(int i)
	{
		assert(i >= 0 && i < NEAR_INFO_NUM);
		if (i >= 0 && i < NEAR_INFO_NUM)
		{
			return &m_NearInfo[i];
		}
		return &m_NearInfo[0];
	}
	
	// 附近的信息  
	void SetNearInfo(int i, NearInfo * pNearInfo)
	{
		assert(i >= 0 && i < NEAR_INFO_NUM);
		if (i >= 0 && i < NEAR_INFO_NUM && pNearInfo)
		{
			m_NearInfo[i] = *pNearInfo;
		}
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 数目	
		if (ret + (int32_t)sizeof(m_NearInfoNum) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_NearInfoNum;
		ret += sizeof(m_NearInfoNum);
		destbuffer += sizeof(m_NearInfoNum);
		 
		 
		// 附近的信息	
		for (int32_t i = 0; i < NEAR_INFO_NUM && i < m_NearInfoNum; i++)
		{
			int32_t retNearInfo = m_NearInfo[i].Marshal(destbuffer, size - ret);
			if (retNearInfo <= 0)
			{
				return -ret + retNearInfo;
			}
			ret += retNearInfo;
			destbuffer += retNearInfo;
		}
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 数目	
		if (ret + (int32_t)sizeof(m_NearInfoNum) > size)
		{
			return -ret;
		}
		m_NearInfoNum = *(uint8_t*)srcbuffer;
		ret += sizeof(m_NearInfoNum);
		srcbuffer += sizeof(m_NearInfoNum);
		 
		 
		// 附近的信息	
		for (int32_t i = 0; i < NEAR_INFO_NUM && i < m_NearInfoNum; i++)
		{
			int32_t retNearInfo = m_NearInfo[i].Unmarshal(srcbuffer, size - ret);
			if (retNearInfo <= 0)
			{
				return -ret + retNearInfo;
			}
			ret += retNearInfo;
			srcbuffer += retNearInfo;
		}
		m_NearInfoNum = PROTO_MIN(m_NearInfoNum, NEAR_INFO_NUM);
		 
		 
		return ret;
	}
	
};  
       
// 网络消息内容  
enum E_NetMsgPara  
{
	
	// Type
	E_NetMsgPara_Type,
	 
	// 请求登录  
	E_NetMsgPara_CSReqLogin,
	 
	// 返回登录  
	E_NetMsgPara_SCResLogin,
	 
	// 请求创建角色  
	E_NetMsgPara_CSReqCreateRole,
	 
	// 返回创建角色  
	E_NetMsgPara_SCResCreateRole,
	 
	// 请求登入游戏  
	E_NetMsgPara_CSReqEnter,
	 
	// 返回登入游戏  
	E_NetMsgPara_SCResEnter,
	 
	// 推送附近的信息  
	E_NetMsgPara_SCNtyNear,
	  
	
};

// 网络消息内容  
struct NetMsgPara  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	// Type 
	int32_t m_Type;
	
	union
	{
		  
		// 请求登录  
		CSReqLogin m_CSReqLogin;  
		  
		// 返回登录  
		SCResLogin m_SCResLogin;  
		  
		// 请求创建角色  
		CSReqCreateRole m_CSReqCreateRole;  
		  
		// 返回创建角色  
		SCResCreateRole m_SCResCreateRole;  
		  
		// 请求登入游戏  
		CSReqEnter m_CSReqEnter;  
		  
		// 返回登入游戏  
		SCResEnter m_SCResEnter;  
		  
		// 推送附近的信息  
		SCNtyNear m_SCNtyNear;  
		  
	};
	
	int32_t GetType()
	{
		return m_Type;
	}
	
	void SetType(int32_t type)
	{
		m_Type = type;
	}
	
	  
	// 请求登录  
	CSReqLogin * GetCSReqLogin()
	{
		return &m_CSReqLogin;
	}
	
	// 请求登录  
	void SetCSReqLogin(CSReqLogin * pCSReqLogin)
	{
		if (pCSReqLogin)
		{
			m_CSReqLogin = *pCSReqLogin;
		}
	}
	  
	// 返回登录  
	SCResLogin * GetSCResLogin()
	{
		return &m_SCResLogin;
	}
	
	// 返回登录  
	void SetSCResLogin(SCResLogin * pSCResLogin)
	{
		if (pSCResLogin)
		{
			m_SCResLogin = *pSCResLogin;
		}
	}
	  
	// 请求创建角色  
	CSReqCreateRole * GetCSReqCreateRole()
	{
		return &m_CSReqCreateRole;
	}
	
	// 请求创建角色  
	void SetCSReqCreateRole(CSReqCreateRole * pCSReqCreateRole)
	{
		if (pCSReqCreateRole)
		{
			m_CSReqCreateRole = *pCSReqCreateRole;
		}
	}
	  
	// 返回创建角色  
	SCResCreateRole * GetSCResCreateRole()
	{
		return &m_SCResCreateRole;
	}
	
	// 返回创建角色  
	void SetSCResCreateRole(SCResCreateRole * pSCResCreateRole)
	{
		if (pSCResCreateRole)
		{
			m_SCResCreateRole = *pSCResCreateRole;
		}
	}
	  
	// 请求登入游戏  
	CSReqEnter * GetCSReqEnter()
	{
		return &m_CSReqEnter;
	}
	
	// 请求登入游戏  
	void SetCSReqEnter(CSReqEnter * pCSReqEnter)
	{
		if (pCSReqEnter)
		{
			m_CSReqEnter = *pCSReqEnter;
		}
	}
	  
	// 返回登入游戏  
	SCResEnter * GetSCResEnter()
	{
		return &m_SCResEnter;
	}
	
	// 返回登入游戏  
	void SetSCResEnter(SCResEnter * pSCResEnter)
	{
		if (pSCResEnter)
		{
			m_SCResEnter = *pSCResEnter;
		}
	}
	  
	// 推送附近的信息  
	SCNtyNear * GetSCNtyNear()
	{
		return &m_SCNtyNear;
	}
	
	// 推送附近的信息  
	void SetSCNtyNear(SCNtyNear * pSCNtyNear)
	{
		if (pSCNtyNear)
		{
			m_SCNtyNear = *pSCNtyNear;
		}
	}
	  
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		
		// type
		if (ret + (int32_t)sizeof(m_Type) > size)
		{
			return -ret;
		}
		*(int32_t*)destbuffer = m_Type;
		ret += sizeof(m_Type);
		destbuffer += sizeof(m_Type);
		
		switch (m_Type)
		{
		  
		// 请求登录  
		case CS_REQ_LOGIN:
			{
				int32_t retCSReqLogin = m_CSReqLogin.Marshal(destbuffer, size - ret);
				if (retCSReqLogin <= 0)
				{
					return -ret + retCSReqLogin;
				}
				ret += retCSReqLogin;
				destbuffer += retCSReqLogin;
			}
			break;
		  
		// 返回登录  
		case SC_RES_LOGIN:
			{
				int32_t retSCResLogin = m_SCResLogin.Marshal(destbuffer, size - ret);
				if (retSCResLogin <= 0)
				{
					return -ret + retSCResLogin;
				}
				ret += retSCResLogin;
				destbuffer += retSCResLogin;
			}
			break;
		  
		// 请求创建角色  
		case CS_REQ_CREATE_ROLE:
			{
				int32_t retCSReqCreateRole = m_CSReqCreateRole.Marshal(destbuffer, size - ret);
				if (retCSReqCreateRole <= 0)
				{
					return -ret + retCSReqCreateRole;
				}
				ret += retCSReqCreateRole;
				destbuffer += retCSReqCreateRole;
			}
			break;
		  
		// 返回创建角色  
		case SC_RES_CREATE_ROLE:
			{
				int32_t retSCResCreateRole = m_SCResCreateRole.Marshal(destbuffer, size - ret);
				if (retSCResCreateRole <= 0)
				{
					return -ret + retSCResCreateRole;
				}
				ret += retSCResCreateRole;
				destbuffer += retSCResCreateRole;
			}
			break;
		  
		// 请求登入游戏  
		case CS_REQ_ENTER:
			{
				int32_t retCSReqEnter = m_CSReqEnter.Marshal(destbuffer, size - ret);
				if (retCSReqEnter <= 0)
				{
					return -ret + retCSReqEnter;
				}
				ret += retCSReqEnter;
				destbuffer += retCSReqEnter;
			}
			break;
		  
		// 返回登入游戏  
		case SC_RES_ENTER:
			{
				int32_t retSCResEnter = m_SCResEnter.Marshal(destbuffer, size - ret);
				if (retSCResEnter <= 0)
				{
					return -ret + retSCResEnter;
				}
				ret += retSCResEnter;
				destbuffer += retSCResEnter;
			}
			break;
		  
		// 推送附近的信息  
		case SC_NTY_NEAR:
			{
				int32_t retSCNtyNear = m_SCNtyNear.Marshal(destbuffer, size - ret);
				if (retSCNtyNear <= 0)
				{
					return -ret + retSCNtyNear;
				}
				ret += retSCNtyNear;
				destbuffer += retSCNtyNear;
			}
			break;
		  
		default:
			return -ret;
		}
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		
		// type
		if (ret + (int32_t)sizeof(m_Type) > size)
		{
			return -ret;
		}
		m_Type = *(int32_t*)srcbuffer;
		ret += sizeof(m_Type);
		srcbuffer += sizeof(m_Type);
		
		switch (m_Type)
		{
		  
		// 请求登录  
		case CS_REQ_LOGIN:
			{
				int32_t retCSReqLogin = m_CSReqLogin.Unmarshal(srcbuffer, size - ret);
				if (retCSReqLogin <= 0)
				{
					return -ret + retCSReqLogin;
				}
				ret += retCSReqLogin;
				srcbuffer += retCSReqLogin;
			}
			break;
		  
		// 返回登录  
		case SC_RES_LOGIN:
			{
				int32_t retSCResLogin = m_SCResLogin.Unmarshal(srcbuffer, size - ret);
				if (retSCResLogin <= 0)
				{
					return -ret + retSCResLogin;
				}
				ret += retSCResLogin;
				srcbuffer += retSCResLogin;
			}
			break;
		  
		// 请求创建角色  
		case CS_REQ_CREATE_ROLE:
			{
				int32_t retCSReqCreateRole = m_CSReqCreateRole.Unmarshal(srcbuffer, size - ret);
				if (retCSReqCreateRole <= 0)
				{
					return -ret + retCSReqCreateRole;
				}
				ret += retCSReqCreateRole;
				srcbuffer += retCSReqCreateRole;
			}
			break;
		  
		// 返回创建角色  
		case SC_RES_CREATE_ROLE:
			{
				int32_t retSCResCreateRole = m_SCResCreateRole.Unmarshal(srcbuffer, size - ret);
				if (retSCResCreateRole <= 0)
				{
					return -ret + retSCResCreateRole;
				}
				ret += retSCResCreateRole;
				srcbuffer += retSCResCreateRole;
			}
			break;
		  
		// 请求登入游戏  
		case CS_REQ_ENTER:
			{
				int32_t retCSReqEnter = m_CSReqEnter.Unmarshal(srcbuffer, size - ret);
				if (retCSReqEnter <= 0)
				{
					return -ret + retCSReqEnter;
				}
				ret += retCSReqEnter;
				srcbuffer += retCSReqEnter;
			}
			break;
		  
		// 返回登入游戏  
		case SC_RES_ENTER:
			{
				int32_t retSCResEnter = m_SCResEnter.Unmarshal(srcbuffer, size - ret);
				if (retSCResEnter <= 0)
				{
					return -ret + retSCResEnter;
				}
				ret += retSCResEnter;
				srcbuffer += retSCResEnter;
			}
			break;
		  
		// 推送附近的信息  
		case SC_NTY_NEAR:
			{
				int32_t retSCNtyNear = m_SCNtyNear.Unmarshal(srcbuffer, size - ret);
				if (retSCNtyNear <= 0)
				{
					return -ret + retSCNtyNear;
				}
				ret += retSCNtyNear;
				srcbuffer += retSCNtyNear;
			}
			break;
		  
		default:
			return -ret;
		}
		return ret;
	}
	
	
};  
       
// 外部网络头消息  
enum E_NetMsgHead  
{
	
	 
	// 序列号  
	E_NetMsgHead_Index,
	 
	// 种子  
	E_NetMsgHead_RandSeed,
	 
	// 校验值  
	E_NetMsgHead_CheckSum,
	  
	
};

// 外部网络头消息  
struct NetMsgHead  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 序列号  
	uint8_t m_Index;  
	  
	// 种子  
	uint8_t m_RandSeed;  
	  
	// 校验值  
	uint8_t m_CheckSum;  
	  
	
	
	
	// 序列号  
	uint8_t GetIndex()
	{
		return m_Index;
	}
	
	// 序列号  
	void SetIndex(uint8_t _Index)
	{
		m_Index = _Index;
	}
	
	
	// 种子  
	uint8_t GetRandSeed()
	{
		return m_RandSeed;
	}
	
	// 种子  
	void SetRandSeed(uint8_t _RandSeed)
	{
		m_RandSeed = _RandSeed;
	}
	
	
	// 校验值  
	uint8_t GetCheckSum()
	{
		return m_CheckSum;
	}
	
	// 校验值  
	void SetCheckSum(uint8_t _CheckSum)
	{
		m_CheckSum = _CheckSum;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 序列号	
		if (ret + (int32_t)sizeof(m_Index) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_Index;
		ret += sizeof(m_Index);
		destbuffer += sizeof(m_Index);
		 
		 
		// 种子	
		if (ret + (int32_t)sizeof(m_RandSeed) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_RandSeed;
		ret += sizeof(m_RandSeed);
		destbuffer += sizeof(m_RandSeed);
		 
		 
		// 校验值	
		if (ret + (int32_t)sizeof(m_CheckSum) > size)
		{
			return -ret;
		}
		*(uint8_t*)destbuffer = m_CheckSum;
		ret += sizeof(m_CheckSum);
		destbuffer += sizeof(m_CheckSum);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 序列号	
		if (ret + (int32_t)sizeof(m_Index) > size)
		{
			return -ret;
		}
		m_Index = *(uint8_t*)srcbuffer;
		ret += sizeof(m_Index);
		srcbuffer += sizeof(m_Index);
		 
		 
		// 种子	
		if (ret + (int32_t)sizeof(m_RandSeed) > size)
		{
			return -ret;
		}
		m_RandSeed = *(uint8_t*)srcbuffer;
		ret += sizeof(m_RandSeed);
		srcbuffer += sizeof(m_RandSeed);
		 
		 
		// 校验值	
		if (ret + (int32_t)sizeof(m_CheckSum) > size)
		{
			return -ret;
		}
		m_CheckSum = *(uint8_t*)srcbuffer;
		ret += sizeof(m_CheckSum);
		srcbuffer += sizeof(m_CheckSum);
		 
		 
		return ret;
	}
	
};  
       
// 外部网络消息  
enum E_NetMsg  
{
	
	 
	// 外部网络头消息  
	E_NetMsg_NetMsgHead,
	 
	// 外部网络消息内容  
	E_NetMsg_NetMsgPara,
	  
	
};

// 外部网络消息  
struct NetMsg  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 外部网络头消息  
	NetMsgHead m_NetMsgHead;  
	  
	// 外部网络消息内容  
	NetMsgPara m_NetMsgPara;  
	  
	
	
	
	// 外部网络头消息  
	NetMsgHead * GetNetMsgHead()
	{
		return &m_NetMsgHead;
	}
	
	// 外部网络头消息  
	void SetNetMsgHead(NetMsgHead * pNetMsgHead)
	{
		if (pNetMsgHead)
		{
			m_NetMsgHead = *pNetMsgHead;
		}
	}
	
	
	// 外部网络消息内容  
	NetMsgPara * GetNetMsgPara()
	{
		return &m_NetMsgPara;
	}
	
	// 外部网络消息内容  
	void SetNetMsgPara(NetMsgPara * pNetMsgPara)
	{
		if (pNetMsgPara)
		{
			m_NetMsgPara = *pNetMsgPara;
		}
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 外部网络头消息	
		int32_t retNetMsgHead = m_NetMsgHead.Marshal(destbuffer, size - ret);
		if (retNetMsgHead <= 0)
		{
			return -ret + retNetMsgHead;
		}
		ret += retNetMsgHead;
		destbuffer += retNetMsgHead;
		 
		 
		// 外部网络消息内容	
		int32_t retNetMsgPara = m_NetMsgPara.Marshal(destbuffer, size - ret);
		if (retNetMsgPara <= 0)
		{
			return -ret + retNetMsgPara;
		}
		ret += retNetMsgPara;
		destbuffer += retNetMsgPara;
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 外部网络头消息	
		int32_t retNetMsgHead = m_NetMsgHead.Unmarshal(srcbuffer, size - ret);
		if (retNetMsgHead <= 0)
		{
			return -ret + retNetMsgHead;
		}
		ret += retNetMsgHead;
		srcbuffer += retNetMsgHead;
		 
		 
		// 外部网络消息内容	
		int32_t retNetMsgPara = m_NetMsgPara.Unmarshal(srcbuffer, size - ret);
		if (retNetMsgPara <= 0)
		{
			return -ret + retNetMsgPara;
		}
		ret += retNetMsgPara;
		srcbuffer += retNetMsgPara;
		 
		 
		return ret;
	}
	
};  
  

static void bindscript_netmsg(fake * fk)
{
    
	// AccInfo 账号信息  
	
	{
		  
		// 用户名  
		fkreg(fk, "GetAcc", &AccInfo::GetAcc);
		fkreg(fk, "SetAcc", &AccInfo::SetAcc);
		  
		// 钻石  
		fkreg(fk, "Getdiamond", &AccInfo::Getdiamond);
		fkreg(fk, "Setdiamond", &AccInfo::Setdiamond);
		  
		// 角色  
		fkreg(fk, "GetRoleGuid", &AccInfo::GetRoleGuid);
		fkreg(fk, "SetRoleGuid", &AccInfo::SetRoleGuid);
		  
	}
	
    
	// FriendInfo 好友信息  
	
	{
		  
		// 唯一id  
		fkreg(fk, "GetGuid", &FriendInfo::GetGuid);
		fkreg(fk, "SetGuid", &FriendInfo::SetGuid);
		  
		// 好感度  
		fkreg(fk, "GetNice", &FriendInfo::GetNice);
		fkreg(fk, "SetNice", &FriendInfo::SetNice);
		  
	}
	
    
	// FriendsInfo 好友信息  
	
	{
		  
		// 数目  
		fkreg(fk, "GetNum", &FriendsInfo::GetNum);
		fkreg(fk, "SetNum", &FriendsInfo::SetNum);
		  
		// 好友信息  
		fkreg(fk, "GetFriendInfo", &FriendsInfo::GetFriendInfo);
		fkreg(fk, "SetFriendInfo", &FriendsInfo::SetFriendInfo);
		  
	}
	
    
	// ItemInfo 道具信息  
	
	{
		  
		// 唯一id  
		fkreg(fk, "GetGuid", &ItemInfo::GetGuid);
		fkreg(fk, "SetGuid", &ItemInfo::SetGuid);
		  
		// 表格id  
		fkreg(fk, "GetItemId", &ItemInfo::GetItemId);
		fkreg(fk, "SetItemId", &ItemInfo::SetItemId);
		  
		// 堆叠数目  
		fkreg(fk, "GetNum", &ItemInfo::GetNum);
		fkreg(fk, "SetNum", &ItemInfo::SetNum);
		  
		// 是否绑定  
		fkreg(fk, "GetIsBind", &ItemInfo::GetIsBind);
		fkreg(fk, "SetIsBind", &ItemInfo::SetIsBind);
		  
	}
	
    
	// ItemsInfo 道具信息  
	
	{
		  
		// 数目  
		fkreg(fk, "GetNum", &ItemsInfo::GetNum);
		fkreg(fk, "SetNum", &ItemsInfo::SetNum);
		  
		// 道具信息  
		fkreg(fk, "GetItemInfo", &ItemsInfo::GetItemInfo);
		fkreg(fk, "SetItemInfo", &ItemsInfo::SetItemInfo);
		  
	}
	
    
	// RoleInfo 玩家信息  
	
	{
		  
		// 唯一id  
		fkreg(fk, "GetGuid", &RoleInfo::GetGuid);
		fkreg(fk, "SetGuid", &RoleInfo::SetGuid);
		  
		// 用户名  
		fkreg(fk, "GetAcc", &RoleInfo::GetAcc);
		fkreg(fk, "SetAcc", &RoleInfo::SetAcc);
		  
		// 角色名  
		fkreg(fk, "GetName", &RoleInfo::GetName);
		fkreg(fk, "SetName", &RoleInfo::SetName);
		  
		// 血量  
		fkreg(fk, "GetHp", &RoleInfo::GetHp);
		fkreg(fk, "SetHp", &RoleInfo::SetHp);
		  
		// 魔量  
		fkreg(fk, "GetMp", &RoleInfo::GetMp);
		fkreg(fk, "SetMp", &RoleInfo::SetMp);
		  
		// x  
		fkreg(fk, "GetX", &RoleInfo::GetX);
		fkreg(fk, "SetX", &RoleInfo::SetX);
		  
		// y  
		fkreg(fk, "GetY", &RoleInfo::GetY);
		fkreg(fk, "SetY", &RoleInfo::SetY);
		  
		// 线路  
		fkreg(fk, "GetLine", &RoleInfo::GetLine);
		fkreg(fk, "SetLine", &RoleInfo::SetLine);
		  
		// 场景id  
		fkreg(fk, "GetSceneId", &RoleInfo::GetSceneId);
		fkreg(fk, "SetSceneId", &RoleInfo::SetSceneId);
		  
		// 场景guid  
		fkreg(fk, "GetSceneGuid", &RoleInfo::GetSceneGuid);
		fkreg(fk, "SetSceneGuid", &RoleInfo::SetSceneGuid);
		  
		// 好友信息  
		fkreg(fk, "GetFriendsInfo", &RoleInfo::GetFriendsInfo);
		fkreg(fk, "SetFriendsInfo", &RoleInfo::SetFriendsInfo);
		  
		// 好友信息  
		fkreg(fk, "GetItemsInfo", &RoleInfo::GetItemsInfo);
		fkreg(fk, "SetItemsInfo", &RoleInfo::SetItemsInfo);
		  
	}
	
    
	// CSReqLogin 请求登录  
	
	{
		  
		// 用户名  
		fkreg(fk, "GetAcc", &CSReqLogin::GetAcc);
		fkreg(fk, "SetAcc", &CSReqLogin::SetAcc);
		  
		// 密码  
		fkreg(fk, "GetPwd", &CSReqLogin::GetPwd);
		fkreg(fk, "SetPwd", &CSReqLogin::SetPwd);
		  
	}
	
    
	// SCResLogin 返回登录  
	
	{
		  
		// 结果，0成功-1创建账号失败-2加载角色失败  
		fkreg(fk, "GetRet", &SCResLogin::GetRet);
		fkreg(fk, "SetRet", &SCResLogin::SetRet);
		  
		// 账号信息  
		fkreg(fk, "GetAccInfo", &SCResLogin::GetAccInfo);
		fkreg(fk, "SetAccInfo", &SCResLogin::SetAccInfo);
		  
		// 玩家数目  
		fkreg(fk, "GetRoleInfoNum", &SCResLogin::GetRoleInfoNum);
		fkreg(fk, "SetRoleInfoNum", &SCResLogin::SetRoleInfoNum);
		  
		// 玩家信息  
		fkreg(fk, "GetRoleInfo", &SCResLogin::GetRoleInfo);
		fkreg(fk, "SetRoleInfo", &SCResLogin::SetRoleInfo);
		  
	}
	
    
	// CSReqCreateRole 请求创建角色  
	
	{
		  
		// 用户名  
		fkreg(fk, "GetAcc", &CSReqCreateRole::GetAcc);
		fkreg(fk, "SetAcc", &CSReqCreateRole::SetAcc);
		  
		// 角色名  
		fkreg(fk, "GetName", &CSReqCreateRole::GetName);
		fkreg(fk, "SetName", &CSReqCreateRole::SetName);
		  
		// 职业  
		fkreg(fk, "GetOccupation", &CSReqCreateRole::GetOccupation);
		fkreg(fk, "SetOccupation", &CSReqCreateRole::SetOccupation);
		  
	}
	
    
	// SCResCreateRole 返回创建角色  
	
	{
		  
		// 结果，0成功-1已经有角色了-2创建角色失败-3更新账号失败  
		fkreg(fk, "GetRet", &SCResCreateRole::GetRet);
		fkreg(fk, "SetRet", &SCResCreateRole::SetRet);
		  
		// 玩家数目  
		fkreg(fk, "GetRoleInfoNum", &SCResCreateRole::GetRoleInfoNum);
		fkreg(fk, "SetRoleInfoNum", &SCResCreateRole::SetRoleInfoNum);
		  
		// 玩家信息  
		fkreg(fk, "GetRoleInfo", &SCResCreateRole::GetRoleInfo);
		fkreg(fk, "SetRoleInfo", &SCResCreateRole::SetRoleInfo);
		  
	}
	
    
	// CSReqEnter 请求登入游戏  
	
	{
		  
		// 占位  
		fkreg(fk, "Gettmp", &CSReqEnter::Gettmp);
		fkreg(fk, "Settmp", &CSReqEnter::Settmp);
		  
	}
	
    
	// SCResEnter 返回登入游戏  
	
	{
		  
		// 结果，0成功-1加载角色失败  
		fkreg(fk, "GetRet", &SCResEnter::GetRet);
		fkreg(fk, "SetRet", &SCResEnter::SetRet);
		  
		// 玩家数目  
		fkreg(fk, "GetRoleInfoNum", &SCResEnter::GetRoleInfoNum);
		fkreg(fk, "SetRoleInfoNum", &SCResEnter::SetRoleInfoNum);
		  
		// 玩家信息  
		fkreg(fk, "GetRoleInfo", &SCResEnter::GetRoleInfo);
		fkreg(fk, "SetRoleInfo", &SCResEnter::SetRoleInfo);
		  
	}
	
    
	// MonsterInfo 怪物信息  
	
	{
		  
		// 唯一id  
		fkreg(fk, "GetGuid", &MonsterInfo::GetGuid);
		fkreg(fk, "SetGuid", &MonsterInfo::SetGuid);
		  
		// 怪物id  
		fkreg(fk, "GetId", &MonsterInfo::GetId);
		fkreg(fk, "SetId", &MonsterInfo::SetId);
		  
		// X  
		fkreg(fk, "GetX", &MonsterInfo::GetX);
		fkreg(fk, "SetX", &MonsterInfo::SetX);
		  
		// Y  
		fkreg(fk, "GetY", &MonsterInfo::GetY);
		fkreg(fk, "SetY", &MonsterInfo::SetY);
		  
		// 线路  
		fkreg(fk, "GetLine", &MonsterInfo::GetLine);
		fkreg(fk, "SetLine", &MonsterInfo::SetLine);
		  
		// 场景id  
		fkreg(fk, "GetSceneId", &MonsterInfo::GetSceneId);
		fkreg(fk, "SetSceneId", &MonsterInfo::SetSceneId);
		  
		// 场景guid  
		fkreg(fk, "GetSceneGuid", &MonsterInfo::GetSceneGuid);
		fkreg(fk, "SetSceneGuid", &MonsterInfo::SetSceneGuid);
		  
	}
	
    
	// MovePoint 移动信息  
	
	{
		  
		// X  
		fkreg(fk, "GetX", &MovePoint::GetX);
		fkreg(fk, "SetX", &MovePoint::SetX);
		  
		// Y  
		fkreg(fk, "GetY", &MovePoint::GetY);
		fkreg(fk, "SetY", &MovePoint::SetY);
		  
	}
	
    
	// MoveData 移动  
	
	{
		  
		// 数目  
		fkreg(fk, "GetNum", &MoveData::GetNum);
		fkreg(fk, "SetNum", &MoveData::SetNum);
		  
		// 移动信息  
		fkreg(fk, "GetMovePoint", &MoveData::GetMovePoint);
		fkreg(fk, "SetMovePoint", &MoveData::SetMovePoint);
		  
	}
	
    
	// EventDataEnter 变更的数据 进入视野  
	
	{
		fkreg(fk, "GetType", &EventDataEnter::GetType);
		fkreg(fk, "SetType", &EventDataEnter::SetType);
		
		  
		// 玩家信息  
		fkreg(fk, "GetRoleInfo", &EventDataEnter::GetRoleInfo);
		fkreg(fk, "SetRoleInfo", &EventDataEnter::SetRoleInfo);
		  
		// 怪物信息  
		fkreg(fk, "GetMonsterInfo", &EventDataEnter::GetMonsterInfo);
		fkreg(fk, "SetMonsterInfo", &EventDataEnter::SetMonsterInfo);
		  
	}
	
    
	// EventDataLeave 变更的数据 离开视野  
	
	{
		  
		// 占位  
		fkreg(fk, "Gettmp", &EventDataLeave::Gettmp);
		fkreg(fk, "Settmp", &EventDataLeave::Settmp);
		  
	}
	
    
	// EventDataMove 变更的数据 移动  
	
	{
		  
		// 移动信息  
		fkreg(fk, "GetMoveData", &EventDataMove::GetMoveData);
		fkreg(fk, "SetMoveData", &EventDataMove::SetMoveData);
		  
	}
	
    
	// EventDataProperty 变更的数据 属性变化  
	
	{
		  
		// 属性ID  
		fkreg(fk, "GetPropertyID", &EventDataProperty::GetPropertyID);
		fkreg(fk, "SetPropertyID", &EventDataProperty::SetPropertyID);
		  
		// 属性值  
		fkreg(fk, "GetPropertyValue", &EventDataProperty::GetPropertyValue);
		fkreg(fk, "SetPropertyValue", &EventDataProperty::SetPropertyValue);
		  
	}
	
    
	// EventDataSkillCast 变更的数据 释放技能  
	
	{
		  
		// 技能ID  
		fkreg(fk, "GetSkillID", &EventDataSkillCast::GetSkillID);
		fkreg(fk, "SetSkillID", &EventDataSkillCast::SetSkillID);
		  
	}
	
    
	// EventData 变更的数据  
	
	{
		fkreg(fk, "GetType", &EventData::GetType);
		fkreg(fk, "SetType", &EventData::SetType);
		
		  
		// 进入视野  
		fkreg(fk, "GetEventDataEnter", &EventData::GetEventDataEnter);
		fkreg(fk, "SetEventDataEnter", &EventData::SetEventDataEnter);
		  
		// 离开视野  
		fkreg(fk, "GetEventDataLeave", &EventData::GetEventDataLeave);
		fkreg(fk, "SetEventDataLeave", &EventData::SetEventDataLeave);
		  
		// 移动  
		fkreg(fk, "GetEventDataMove", &EventData::GetEventDataMove);
		fkreg(fk, "SetEventDataMove", &EventData::SetEventDataMove);
		  
		// 属性变化  
		fkreg(fk, "GetEventDataProperty", &EventData::GetEventDataProperty);
		fkreg(fk, "SetEventDataProperty", &EventData::SetEventDataProperty);
		  
		// 释放技能  
		fkreg(fk, "GetEventDataSkillCast", &EventData::GetEventDataSkillCast);
		fkreg(fk, "SetEventDataSkillCast", &EventData::SetEventDataSkillCast);
		  
	}
	
    
	// NearInfo 附近信息  
	
	{
		  
		// 唯一id  
		fkreg(fk, "GetGuid", &NearInfo::GetGuid);
		fkreg(fk, "SetGuid", &NearInfo::SetGuid);
		  
		// 数目  
		fkreg(fk, "GetEventDataNum", &NearInfo::GetEventDataNum);
		fkreg(fk, "SetEventDataNum", &NearInfo::SetEventDataNum);
		  
		// 变更的数据  
		fkreg(fk, "GetEventData", &NearInfo::GetEventData);
		fkreg(fk, "SetEventData", &NearInfo::SetEventData);
		  
	}
	
    
	// SCNtyNear 附近的信息  
	
	{
		  
		// 数目  
		fkreg(fk, "GetNearInfoNum", &SCNtyNear::GetNearInfoNum);
		fkreg(fk, "SetNearInfoNum", &SCNtyNear::SetNearInfoNum);
		  
		// 附近的信息  
		fkreg(fk, "GetNearInfo", &SCNtyNear::GetNearInfo);
		fkreg(fk, "SetNearInfo", &SCNtyNear::SetNearInfo);
		  
	}
	
    
	// NetMsgPara 网络消息内容  
	
	{
		fkreg(fk, "GetType", &NetMsgPara::GetType);
		fkreg(fk, "SetType", &NetMsgPara::SetType);
		
		  
		// 请求登录  
		fkreg(fk, "GetCSReqLogin", &NetMsgPara::GetCSReqLogin);
		fkreg(fk, "SetCSReqLogin", &NetMsgPara::SetCSReqLogin);
		  
		// 返回登录  
		fkreg(fk, "GetSCResLogin", &NetMsgPara::GetSCResLogin);
		fkreg(fk, "SetSCResLogin", &NetMsgPara::SetSCResLogin);
		  
		// 请求创建角色  
		fkreg(fk, "GetCSReqCreateRole", &NetMsgPara::GetCSReqCreateRole);
		fkreg(fk, "SetCSReqCreateRole", &NetMsgPara::SetCSReqCreateRole);
		  
		// 返回创建角色  
		fkreg(fk, "GetSCResCreateRole", &NetMsgPara::GetSCResCreateRole);
		fkreg(fk, "SetSCResCreateRole", &NetMsgPara::SetSCResCreateRole);
		  
		// 请求登入游戏  
		fkreg(fk, "GetCSReqEnter", &NetMsgPara::GetCSReqEnter);
		fkreg(fk, "SetCSReqEnter", &NetMsgPara::SetCSReqEnter);
		  
		// 返回登入游戏  
		fkreg(fk, "GetSCResEnter", &NetMsgPara::GetSCResEnter);
		fkreg(fk, "SetSCResEnter", &NetMsgPara::SetSCResEnter);
		  
		// 推送附近的信息  
		fkreg(fk, "GetSCNtyNear", &NetMsgPara::GetSCNtyNear);
		fkreg(fk, "SetSCNtyNear", &NetMsgPara::SetSCNtyNear);
		  
	}
	
    
	// NetMsgHead 外部网络头消息  
	
	{
		  
		// 序列号  
		fkreg(fk, "GetIndex", &NetMsgHead::GetIndex);
		fkreg(fk, "SetIndex", &NetMsgHead::SetIndex);
		  
		// 种子  
		fkreg(fk, "GetRandSeed", &NetMsgHead::GetRandSeed);
		fkreg(fk, "SetRandSeed", &NetMsgHead::SetRandSeed);
		  
		// 校验值  
		fkreg(fk, "GetCheckSum", &NetMsgHead::GetCheckSum);
		fkreg(fk, "SetCheckSum", &NetMsgHead::SetCheckSum);
		  
	}
	
    
	// NetMsg 外部网络消息  
	
	{
		  
		// 外部网络头消息  
		fkreg(fk, "GetNetMsgHead", &NetMsg::GetNetMsgHead);
		fkreg(fk, "SetNetMsgHead", &NetMsg::SetNetMsgHead);
		  
		// 外部网络消息内容  
		fkreg(fk, "GetNetMsgPara", &NetMsg::GetNetMsgPara);
		fkreg(fk, "SetNetMsgPara", &NetMsg::SetNetMsgPara);
		  
	}
	
  
}

}
