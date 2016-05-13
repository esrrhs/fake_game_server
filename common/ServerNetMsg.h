#pragma once

#include "libcommon.h"
#include "fakescript.h"

#define PROTO_MIN(a,b) Min<int32_t>((int32_t)a, (int32_t)b)

namespace Fproto
{

    
// 消息id  
enum ServerMsgID
{
	 
	// 消息id开始  
	GD_MSG_START = 10000,
	 
	// 请求登录  
	GD_REQ_LOGIN = 10001,
	 
	// 返回登录  
	DG_RES_LOGIN = 10002,
	 
	// 请求创建角色  
	GD_REQ_CREATE_ROLE = 10003,
	 
	// 返回创建角色  
	DG_RES_CREATE_ROLE = 10004,
	 
	// 请求登入游戏  
	GD_REQ_ENTER = 10005,
	 
	// 返回登入游戏  
	DG_RES_ENTER = 10006,
	 
	// 消息id开始  
	GG_MSG_START = 20000,
	 
	// 请求gate更新guid  
	GG_REQ_UPDATE_GUID = 20001,
	 
	// 返回gate更新guid  
	GG_RES_UPDATE_GUID = 20002,
	  
};
  

    
// 消息id  
static const char * GetServerMsgIDName(int i)
{
	switch (i)
	{
	 
	// 消息id开始  
	case 10000:
		return "GD_MSG_START";
	 
	// 请求登录  
	case 10001:
		return "GD_REQ_LOGIN";
	 
	// 返回登录  
	case 10002:
		return "DG_RES_LOGIN";
	 
	// 请求创建角色  
	case 10003:
		return "GD_REQ_CREATE_ROLE";
	 
	// 返回创建角色  
	case 10004:
		return "DG_RES_CREATE_ROLE";
	 
	// 请求登入游戏  
	case 10005:
		return "GD_REQ_ENTER";
	 
	// 返回登入游戏  
	case 10006:
		return "DG_RES_ENTER";
	 
	// 消息id开始  
	case 20000:
		return "GG_MSG_START";
	 
	// 请求gate更新guid  
	case 20001:
		return "GG_REQ_UPDATE_GUID";
	 
	// 返回gate更新guid  
	case 20002:
		return "GG_RES_UPDATE_GUID";
	  
	default:
		return "";
	}
}

static const char * GetServerMsgIDFuncName(int i)
{
	switch (i)
	{
	 
	// 消息id开始  
	case 10000:
		return "on_GD_MSG_START";
	 
	// 请求登录  
	case 10001:
		return "on_GD_REQ_LOGIN";
	 
	// 返回登录  
	case 10002:
		return "on_DG_RES_LOGIN";
	 
	// 请求创建角色  
	case 10003:
		return "on_GD_REQ_CREATE_ROLE";
	 
	// 返回创建角色  
	case 10004:
		return "on_DG_RES_CREATE_ROLE";
	 
	// 请求登入游戏  
	case 10005:
		return "on_GD_REQ_ENTER";
	 
	// 返回登入游戏  
	case 10006:
		return "on_DG_RES_ENTER";
	 
	// 消息id开始  
	case 20000:
		return "on_GG_MSG_START";
	 
	// 请求gate更新guid  
	case 20001:
		return "on_GG_REQ_UPDATE_GUID";
	 
	// 返回gate更新guid  
	case 20002:
		return "on_GG_RES_UPDATE_GUID";
	  
	default:
		return "";
	}
}  
  



		
		
	


	
		
       
// 请求gate更新guid  
enum E_GGReqUpdateGuid  
{
	
	 
	// 旧guid  
	E_GGReqUpdateGuid_OldGuid,
	 
	// 新guid  
	E_GGReqUpdateGuid_NewGuid,
	  
	
};

// 请求gate更新guid  
struct GGReqUpdateGuid  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 旧guid  
	uint64_t m_OldGuid;  
	  
	// 新guid  
	uint64_t m_NewGuid;  
	  
	
	
	
	// 旧guid  
	uint64_t GetOldGuid()
	{
		return m_OldGuid;
	}
	
	// 旧guid  
	void SetOldGuid(uint64_t _OldGuid)
	{
		m_OldGuid = _OldGuid;
	}
	
	
	// 新guid  
	uint64_t GetNewGuid()
	{
		return m_NewGuid;
	}
	
	// 新guid  
	void SetNewGuid(uint64_t _NewGuid)
	{
		m_NewGuid = _NewGuid;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 旧guid	
		if (ret + (int32_t)sizeof(m_OldGuid) > size)
		{
			return -ret;
		}
		*(uint64_t*)destbuffer = m_OldGuid;
		ret += sizeof(m_OldGuid);
		destbuffer += sizeof(m_OldGuid);
		 
		 
		// 新guid	
		if (ret + (int32_t)sizeof(m_NewGuid) > size)
		{
			return -ret;
		}
		*(uint64_t*)destbuffer = m_NewGuid;
		ret += sizeof(m_NewGuid);
		destbuffer += sizeof(m_NewGuid);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 旧guid	
		if (ret + (int32_t)sizeof(m_OldGuid) > size)
		{
			return -ret;
		}
		m_OldGuid = *(uint64_t*)srcbuffer;
		ret += sizeof(m_OldGuid);
		srcbuffer += sizeof(m_OldGuid);
		 
		 
		// 新guid	
		if (ret + (int32_t)sizeof(m_NewGuid) > size)
		{
			return -ret;
		}
		m_NewGuid = *(uint64_t*)srcbuffer;
		ret += sizeof(m_NewGuid);
		srcbuffer += sizeof(m_NewGuid);
		 
		 
		return ret;
	}
	
};  
       
// 返回gate更新guid  
enum E_GGResUpdateGuid  
{
	
	 
	// 旧guid  
	E_GGResUpdateGuid_OldGuid,
	 
	// 新guid  
	E_GGResUpdateGuid_NewGuid,
	  
	
};

// 返回gate更新guid  
struct GGResUpdateGuid  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 旧guid  
	uint64_t m_OldGuid;  
	  
	// 新guid  
	uint64_t m_NewGuid;  
	  
	
	
	
	// 旧guid  
	uint64_t GetOldGuid()
	{
		return m_OldGuid;
	}
	
	// 旧guid  
	void SetOldGuid(uint64_t _OldGuid)
	{
		m_OldGuid = _OldGuid;
	}
	
	
	// 新guid  
	uint64_t GetNewGuid()
	{
		return m_NewGuid;
	}
	
	// 新guid  
	void SetNewGuid(uint64_t _NewGuid)
	{
		m_NewGuid = _NewGuid;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 旧guid	
		if (ret + (int32_t)sizeof(m_OldGuid) > size)
		{
			return -ret;
		}
		*(uint64_t*)destbuffer = m_OldGuid;
		ret += sizeof(m_OldGuid);
		destbuffer += sizeof(m_OldGuid);
		 
		 
		// 新guid	
		if (ret + (int32_t)sizeof(m_NewGuid) > size)
		{
			return -ret;
		}
		*(uint64_t*)destbuffer = m_NewGuid;
		ret += sizeof(m_NewGuid);
		destbuffer += sizeof(m_NewGuid);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 旧guid	
		if (ret + (int32_t)sizeof(m_OldGuid) > size)
		{
			return -ret;
		}
		m_OldGuid = *(uint64_t*)srcbuffer;
		ret += sizeof(m_OldGuid);
		srcbuffer += sizeof(m_OldGuid);
		 
		 
		// 新guid	
		if (ret + (int32_t)sizeof(m_NewGuid) > size)
		{
			return -ret;
		}
		m_NewGuid = *(uint64_t*)srcbuffer;
		ret += sizeof(m_NewGuid);
		srcbuffer += sizeof(m_NewGuid);
		 
		 
		return ret;
	}
	
};  
       
// 网络消息内容  
enum E_ServerNetMsgPara  
{
	
	// Type
	E_ServerNetMsgPara_Type,
	 
	// 请求登录  
	E_ServerNetMsgPara_CSReqLogin,
	 
	// 返回登录  
	E_ServerNetMsgPara_SCResLogin,
	 
	// 请求创建角色  
	E_ServerNetMsgPara_CSReqCreateRole,
	 
	// 返回创建角色  
	E_ServerNetMsgPara_SCResCreateRole,
	 
	// 请求登入游戏  
	E_ServerNetMsgPara_CSReqEnter,
	 
	// 返回登入游戏  
	E_ServerNetMsgPara_SCResEnter,
	 
	// 推送附近的信息  
	E_ServerNetMsgPara_SCNtyNear,
	 
	// 请求登录  
	E_ServerNetMsgPara_GDReqLogin,
	 
	// 返回登录  
	E_ServerNetMsgPara_DGResLogin,
	 
	// 请求创建角色  
	E_ServerNetMsgPara_GDReqCreateRole,
	 
	// 返回创建角色  
	E_ServerNetMsgPara_DGResCreateRole,
	 
	// 请求登入游戏  
	E_ServerNetMsgPara_GDReqEnter,
	 
	// 返回登入游戏  
	E_ServerNetMsgPara_DGResEnter,
	 
	// 请求gate更新guid  
	E_ServerNetMsgPara_GGReqUpdateGuid,
	 
	// 返回gate更新guid  
	E_ServerNetMsgPara_GGResUpdateGuid,
	  
	
};

// 网络消息内容  
struct ServerNetMsgPara  
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
		  
		// 请求登录  
		CSReqLogin m_GDReqLogin;  
		  
		// 返回登录  
		SCResLogin m_DGResLogin;  
		  
		// 请求创建角色  
		CSReqCreateRole m_GDReqCreateRole;  
		  
		// 返回创建角色  
		SCResCreateRole m_DGResCreateRole;  
		  
		// 请求登入游戏  
		CSReqEnter m_GDReqEnter;  
		  
		// 返回登入游戏  
		SCResEnter m_DGResEnter;  
		  
		// 请求gate更新guid  
		GGReqUpdateGuid m_GGReqUpdateGuid;  
		  
		// 返回gate更新guid  
		GGResUpdateGuid m_GGResUpdateGuid;  
		  
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
	  
	// 请求登录  
	CSReqLogin * GetGDReqLogin()
	{
		return &m_GDReqLogin;
	}
	
	// 请求登录  
	void SetGDReqLogin(CSReqLogin * pCSReqLogin)
	{
		if (pCSReqLogin)
		{
			m_GDReqLogin = *pCSReqLogin;
		}
	}
	  
	// 返回登录  
	SCResLogin * GetDGResLogin()
	{
		return &m_DGResLogin;
	}
	
	// 返回登录  
	void SetDGResLogin(SCResLogin * pSCResLogin)
	{
		if (pSCResLogin)
		{
			m_DGResLogin = *pSCResLogin;
		}
	}
	  
	// 请求创建角色  
	CSReqCreateRole * GetGDReqCreateRole()
	{
		return &m_GDReqCreateRole;
	}
	
	// 请求创建角色  
	void SetGDReqCreateRole(CSReqCreateRole * pCSReqCreateRole)
	{
		if (pCSReqCreateRole)
		{
			m_GDReqCreateRole = *pCSReqCreateRole;
		}
	}
	  
	// 返回创建角色  
	SCResCreateRole * GetDGResCreateRole()
	{
		return &m_DGResCreateRole;
	}
	
	// 返回创建角色  
	void SetDGResCreateRole(SCResCreateRole * pSCResCreateRole)
	{
		if (pSCResCreateRole)
		{
			m_DGResCreateRole = *pSCResCreateRole;
		}
	}
	  
	// 请求登入游戏  
	CSReqEnter * GetGDReqEnter()
	{
		return &m_GDReqEnter;
	}
	
	// 请求登入游戏  
	void SetGDReqEnter(CSReqEnter * pCSReqEnter)
	{
		if (pCSReqEnter)
		{
			m_GDReqEnter = *pCSReqEnter;
		}
	}
	  
	// 返回登入游戏  
	SCResEnter * GetDGResEnter()
	{
		return &m_DGResEnter;
	}
	
	// 返回登入游戏  
	void SetDGResEnter(SCResEnter * pSCResEnter)
	{
		if (pSCResEnter)
		{
			m_DGResEnter = *pSCResEnter;
		}
	}
	  
	// 请求gate更新guid  
	GGReqUpdateGuid * GetGGReqUpdateGuid()
	{
		return &m_GGReqUpdateGuid;
	}
	
	// 请求gate更新guid  
	void SetGGReqUpdateGuid(GGReqUpdateGuid * pGGReqUpdateGuid)
	{
		if (pGGReqUpdateGuid)
		{
			m_GGReqUpdateGuid = *pGGReqUpdateGuid;
		}
	}
	  
	// 返回gate更新guid  
	GGResUpdateGuid * GetGGResUpdateGuid()
	{
		return &m_GGResUpdateGuid;
	}
	
	// 返回gate更新guid  
	void SetGGResUpdateGuid(GGResUpdateGuid * pGGResUpdateGuid)
	{
		if (pGGResUpdateGuid)
		{
			m_GGResUpdateGuid = *pGGResUpdateGuid;
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
		  
		// 请求登录  
		case GD_REQ_LOGIN:
			{
				int32_t retGDReqLogin = m_GDReqLogin.Marshal(destbuffer, size - ret);
				if (retGDReqLogin <= 0)
				{
					return -ret + retGDReqLogin;
				}
				ret += retGDReqLogin;
				destbuffer += retGDReqLogin;
			}
			break;
		  
		// 返回登录  
		case DG_RES_LOGIN:
			{
				int32_t retDGResLogin = m_DGResLogin.Marshal(destbuffer, size - ret);
				if (retDGResLogin <= 0)
				{
					return -ret + retDGResLogin;
				}
				ret += retDGResLogin;
				destbuffer += retDGResLogin;
			}
			break;
		  
		// 请求创建角色  
		case GD_REQ_CREATE_ROLE:
			{
				int32_t retGDReqCreateRole = m_GDReqCreateRole.Marshal(destbuffer, size - ret);
				if (retGDReqCreateRole <= 0)
				{
					return -ret + retGDReqCreateRole;
				}
				ret += retGDReqCreateRole;
				destbuffer += retGDReqCreateRole;
			}
			break;
		  
		// 返回创建角色  
		case DG_RES_CREATE_ROLE:
			{
				int32_t retDGResCreateRole = m_DGResCreateRole.Marshal(destbuffer, size - ret);
				if (retDGResCreateRole <= 0)
				{
					return -ret + retDGResCreateRole;
				}
				ret += retDGResCreateRole;
				destbuffer += retDGResCreateRole;
			}
			break;
		  
		// 请求登入游戏  
		case GD_REQ_ENTER:
			{
				int32_t retGDReqEnter = m_GDReqEnter.Marshal(destbuffer, size - ret);
				if (retGDReqEnter <= 0)
				{
					return -ret + retGDReqEnter;
				}
				ret += retGDReqEnter;
				destbuffer += retGDReqEnter;
			}
			break;
		  
		// 返回登入游戏  
		case DG_RES_ENTER:
			{
				int32_t retDGResEnter = m_DGResEnter.Marshal(destbuffer, size - ret);
				if (retDGResEnter <= 0)
				{
					return -ret + retDGResEnter;
				}
				ret += retDGResEnter;
				destbuffer += retDGResEnter;
			}
			break;
		  
		// 请求gate更新guid  
		case GG_REQ_UPDATE_GUID:
			{
				int32_t retGGReqUpdateGuid = m_GGReqUpdateGuid.Marshal(destbuffer, size - ret);
				if (retGGReqUpdateGuid <= 0)
				{
					return -ret + retGGReqUpdateGuid;
				}
				ret += retGGReqUpdateGuid;
				destbuffer += retGGReqUpdateGuid;
			}
			break;
		  
		// 返回gate更新guid  
		case GG_RES_UPDATE_GUID:
			{
				int32_t retGGResUpdateGuid = m_GGResUpdateGuid.Marshal(destbuffer, size - ret);
				if (retGGResUpdateGuid <= 0)
				{
					return -ret + retGGResUpdateGuid;
				}
				ret += retGGResUpdateGuid;
				destbuffer += retGGResUpdateGuid;
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
		  
		// 请求登录  
		case GD_REQ_LOGIN:
			{
				int32_t retGDReqLogin = m_GDReqLogin.Unmarshal(srcbuffer, size - ret);
				if (retGDReqLogin <= 0)
				{
					return -ret + retGDReqLogin;
				}
				ret += retGDReqLogin;
				srcbuffer += retGDReqLogin;
			}
			break;
		  
		// 返回登录  
		case DG_RES_LOGIN:
			{
				int32_t retDGResLogin = m_DGResLogin.Unmarshal(srcbuffer, size - ret);
				if (retDGResLogin <= 0)
				{
					return -ret + retDGResLogin;
				}
				ret += retDGResLogin;
				srcbuffer += retDGResLogin;
			}
			break;
		  
		// 请求创建角色  
		case GD_REQ_CREATE_ROLE:
			{
				int32_t retGDReqCreateRole = m_GDReqCreateRole.Unmarshal(srcbuffer, size - ret);
				if (retGDReqCreateRole <= 0)
				{
					return -ret + retGDReqCreateRole;
				}
				ret += retGDReqCreateRole;
				srcbuffer += retGDReqCreateRole;
			}
			break;
		  
		// 返回创建角色  
		case DG_RES_CREATE_ROLE:
			{
				int32_t retDGResCreateRole = m_DGResCreateRole.Unmarshal(srcbuffer, size - ret);
				if (retDGResCreateRole <= 0)
				{
					return -ret + retDGResCreateRole;
				}
				ret += retDGResCreateRole;
				srcbuffer += retDGResCreateRole;
			}
			break;
		  
		// 请求登入游戏  
		case GD_REQ_ENTER:
			{
				int32_t retGDReqEnter = m_GDReqEnter.Unmarshal(srcbuffer, size - ret);
				if (retGDReqEnter <= 0)
				{
					return -ret + retGDReqEnter;
				}
				ret += retGDReqEnter;
				srcbuffer += retGDReqEnter;
			}
			break;
		  
		// 返回登入游戏  
		case DG_RES_ENTER:
			{
				int32_t retDGResEnter = m_DGResEnter.Unmarshal(srcbuffer, size - ret);
				if (retDGResEnter <= 0)
				{
					return -ret + retDGResEnter;
				}
				ret += retDGResEnter;
				srcbuffer += retDGResEnter;
			}
			break;
		  
		// 请求gate更新guid  
		case GG_REQ_UPDATE_GUID:
			{
				int32_t retGGReqUpdateGuid = m_GGReqUpdateGuid.Unmarshal(srcbuffer, size - ret);
				if (retGGReqUpdateGuid <= 0)
				{
					return -ret + retGGReqUpdateGuid;
				}
				ret += retGGReqUpdateGuid;
				srcbuffer += retGGReqUpdateGuid;
			}
			break;
		  
		// 返回gate更新guid  
		case GG_RES_UPDATE_GUID:
			{
				int32_t retGGResUpdateGuid = m_GGResUpdateGuid.Unmarshal(srcbuffer, size - ret);
				if (retGGResUpdateGuid <= 0)
				{
					return -ret + retGGResUpdateGuid;
				}
				ret += retGGResUpdateGuid;
				srcbuffer += retGGResUpdateGuid;
			}
			break;
		  
		default:
			return -ret;
		}
		return ret;
	}
	
	
};  
       
// 网络头消息  
enum E_ServerNetMsgHead  
{
	
	 
	// 玩家GUID  
	E_ServerNetMsgHead_Guid,
	  
	
};

// 网络头消息  
struct ServerNetMsgHead  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 玩家GUID  
	uint64_t m_Guid;  
	  
	
	
	
	// 玩家GUID  
	uint64_t GetGuid()
	{
		return m_Guid;
	}
	
	// 玩家GUID  
	void SetGuid(uint64_t _Guid)
	{
		m_Guid = _Guid;
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 玩家GUID	
		if (ret + (int32_t)sizeof(m_Guid) > size)
		{
			return -ret;
		}
		*(uint64_t*)destbuffer = m_Guid;
		ret += sizeof(m_Guid);
		destbuffer += sizeof(m_Guid);
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 玩家GUID	
		if (ret + (int32_t)sizeof(m_Guid) > size)
		{
			return -ret;
		}
		m_Guid = *(uint64_t*)srcbuffer;
		ret += sizeof(m_Guid);
		srcbuffer += sizeof(m_Guid);
		 
		 
		return ret;
	}
	
};  
       
// 网络消息  
enum E_ServerNetMsg  
{
	
	 
	// 网络头消息  
	E_ServerNetMsg_ServerNetMsgHead,
	 
	// 网络消息内容  
	E_ServerNetMsg_ServerNetMsgPara,
	  
	
};

// 网络消息  
struct ServerNetMsg  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	
	  
	// 网络头消息  
	ServerNetMsgHead m_ServerNetMsgHead;  
	  
	// 网络消息内容  
	ServerNetMsgPara m_ServerNetMsgPara;  
	  
	
	
	
	// 网络头消息  
	ServerNetMsgHead * GetServerNetMsgHead()
	{
		return &m_ServerNetMsgHead;
	}
	
	// 网络头消息  
	void SetServerNetMsgHead(ServerNetMsgHead * pServerNetMsgHead)
	{
		if (pServerNetMsgHead)
		{
			m_ServerNetMsgHead = *pServerNetMsgHead;
		}
	}
	
	
	// 网络消息内容  
	ServerNetMsgPara * GetServerNetMsgPara()
	{
		return &m_ServerNetMsgPara;
	}
	
	// 网络消息内容  
	void SetServerNetMsgPara(ServerNetMsgPara * pServerNetMsgPara)
	{
		if (pServerNetMsgPara)
		{
			m_ServerNetMsgPara = *pServerNetMsgPara;
		}
	}
	
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		 
		// 网络头消息	
		int32_t retServerNetMsgHead = m_ServerNetMsgHead.Marshal(destbuffer, size - ret);
		if (retServerNetMsgHead <= 0)
		{
			return -ret + retServerNetMsgHead;
		}
		ret += retServerNetMsgHead;
		destbuffer += retServerNetMsgHead;
		 
		 
		// 网络消息内容	
		int32_t retServerNetMsgPara = m_ServerNetMsgPara.Marshal(destbuffer, size - ret);
		if (retServerNetMsgPara <= 0)
		{
			return -ret + retServerNetMsgPara;
		}
		ret += retServerNetMsgPara;
		destbuffer += retServerNetMsgPara;
		 
		 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		 
		// 网络头消息	
		int32_t retServerNetMsgHead = m_ServerNetMsgHead.Unmarshal(srcbuffer, size - ret);
		if (retServerNetMsgHead <= 0)
		{
			return -ret + retServerNetMsgHead;
		}
		ret += retServerNetMsgHead;
		srcbuffer += retServerNetMsgHead;
		 
		 
		// 网络消息内容	
		int32_t retServerNetMsgPara = m_ServerNetMsgPara.Unmarshal(srcbuffer, size - ret);
		if (retServerNetMsgPara <= 0)
		{
			return -ret + retServerNetMsgPara;
		}
		ret += retServerNetMsgPara;
		srcbuffer += retServerNetMsgPara;
		 
		 
		return ret;
	}
	
};  
  

static void bindscript_servernetmsg(fake * fk)
{
    
	// GGReqUpdateGuid 请求gate更新guid  
	
	{
		  
		// 旧guid  
		fkreg(fk, "GetOldGuid", &GGReqUpdateGuid::GetOldGuid);
		fkreg(fk, "SetOldGuid", &GGReqUpdateGuid::SetOldGuid);
		  
		// 新guid  
		fkreg(fk, "GetNewGuid", &GGReqUpdateGuid::GetNewGuid);
		fkreg(fk, "SetNewGuid", &GGReqUpdateGuid::SetNewGuid);
		  
	}
	
    
	// GGResUpdateGuid 返回gate更新guid  
	
	{
		  
		// 旧guid  
		fkreg(fk, "GetOldGuid", &GGResUpdateGuid::GetOldGuid);
		fkreg(fk, "SetOldGuid", &GGResUpdateGuid::SetOldGuid);
		  
		// 新guid  
		fkreg(fk, "GetNewGuid", &GGResUpdateGuid::GetNewGuid);
		fkreg(fk, "SetNewGuid", &GGResUpdateGuid::SetNewGuid);
		  
	}
	
    
	// ServerNetMsgPara 网络消息内容  
	
	{
		fkreg(fk, "GetType", &ServerNetMsgPara::GetType);
		fkreg(fk, "SetType", &ServerNetMsgPara::SetType);
		
		  
		// 请求登录  
		fkreg(fk, "GetCSReqLogin", &ServerNetMsgPara::GetCSReqLogin);
		fkreg(fk, "SetCSReqLogin", &ServerNetMsgPara::SetCSReqLogin);
		  
		// 返回登录  
		fkreg(fk, "GetSCResLogin", &ServerNetMsgPara::GetSCResLogin);
		fkreg(fk, "SetSCResLogin", &ServerNetMsgPara::SetSCResLogin);
		  
		// 请求创建角色  
		fkreg(fk, "GetCSReqCreateRole", &ServerNetMsgPara::GetCSReqCreateRole);
		fkreg(fk, "SetCSReqCreateRole", &ServerNetMsgPara::SetCSReqCreateRole);
		  
		// 返回创建角色  
		fkreg(fk, "GetSCResCreateRole", &ServerNetMsgPara::GetSCResCreateRole);
		fkreg(fk, "SetSCResCreateRole", &ServerNetMsgPara::SetSCResCreateRole);
		  
		// 请求登入游戏  
		fkreg(fk, "GetCSReqEnter", &ServerNetMsgPara::GetCSReqEnter);
		fkreg(fk, "SetCSReqEnter", &ServerNetMsgPara::SetCSReqEnter);
		  
		// 返回登入游戏  
		fkreg(fk, "GetSCResEnter", &ServerNetMsgPara::GetSCResEnter);
		fkreg(fk, "SetSCResEnter", &ServerNetMsgPara::SetSCResEnter);
		  
		// 推送附近的信息  
		fkreg(fk, "GetSCNtyNear", &ServerNetMsgPara::GetSCNtyNear);
		fkreg(fk, "SetSCNtyNear", &ServerNetMsgPara::SetSCNtyNear);
		  
		// 请求登录  
		fkreg(fk, "GetGDReqLogin", &ServerNetMsgPara::GetGDReqLogin);
		fkreg(fk, "SetGDReqLogin", &ServerNetMsgPara::SetGDReqLogin);
		  
		// 返回登录  
		fkreg(fk, "GetDGResLogin", &ServerNetMsgPara::GetDGResLogin);
		fkreg(fk, "SetDGResLogin", &ServerNetMsgPara::SetDGResLogin);
		  
		// 请求创建角色  
		fkreg(fk, "GetGDReqCreateRole", &ServerNetMsgPara::GetGDReqCreateRole);
		fkreg(fk, "SetGDReqCreateRole", &ServerNetMsgPara::SetGDReqCreateRole);
		  
		// 返回创建角色  
		fkreg(fk, "GetDGResCreateRole", &ServerNetMsgPara::GetDGResCreateRole);
		fkreg(fk, "SetDGResCreateRole", &ServerNetMsgPara::SetDGResCreateRole);
		  
		// 请求登入游戏  
		fkreg(fk, "GetGDReqEnter", &ServerNetMsgPara::GetGDReqEnter);
		fkreg(fk, "SetGDReqEnter", &ServerNetMsgPara::SetGDReqEnter);
		  
		// 返回登入游戏  
		fkreg(fk, "GetDGResEnter", &ServerNetMsgPara::GetDGResEnter);
		fkreg(fk, "SetDGResEnter", &ServerNetMsgPara::SetDGResEnter);
		  
		// 请求gate更新guid  
		fkreg(fk, "GetGGReqUpdateGuid", &ServerNetMsgPara::GetGGReqUpdateGuid);
		fkreg(fk, "SetGGReqUpdateGuid", &ServerNetMsgPara::SetGGReqUpdateGuid);
		  
		// 返回gate更新guid  
		fkreg(fk, "GetGGResUpdateGuid", &ServerNetMsgPara::GetGGResUpdateGuid);
		fkreg(fk, "SetGGResUpdateGuid", &ServerNetMsgPara::SetGGResUpdateGuid);
		  
	}
	
    
	// ServerNetMsgHead 网络头消息  
	
	{
		  
		// 玩家GUID  
		fkreg(fk, "GetGuid", &ServerNetMsgHead::GetGuid);
		fkreg(fk, "SetGuid", &ServerNetMsgHead::SetGuid);
		  
	}
	
    
	// ServerNetMsg 网络消息  
	
	{
		  
		// 网络头消息  
		fkreg(fk, "GetServerNetMsgHead", &ServerNetMsg::GetServerNetMsgHead);
		fkreg(fk, "SetServerNetMsgHead", &ServerNetMsg::SetServerNetMsgHead);
		  
		// 网络消息内容  
		fkreg(fk, "GetServerNetMsgPara", &ServerNetMsg::GetServerNetMsgPara);
		fkreg(fk, "SetServerNetMsgPara", &ServerNetMsg::SetServerNetMsgPara);
		  
	}
	
  
}

}
