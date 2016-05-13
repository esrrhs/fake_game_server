#pragma once

#include "yang.h"
#include "NetMsg.h"

namespace Fproto
{




// 好友信息
template <typename T, int tag>
struct check_FriendInfo_tag
{
};


// 唯一id
template <typename T>
struct check_FriendInfo_tag<T, 1>
{
};

// 好感度
template <typename T>
struct check_FriendInfo_tag<T, 2>
{
};



// 好友信息
template <typename T, int tag>
struct check_FriendsInfo_tag
{
};


// 好友信息
template <typename T>
struct check_FriendsInfo_tag<T, 1>
{
};



// 道具信息
template <typename T, int tag>
struct check_ItemInfo_tag
{
};


// 唯一id
template <typename T>
struct check_ItemInfo_tag<T, 1>
{
};

// 表格id
template <typename T>
struct check_ItemInfo_tag<T, 2>
{
};

// 堆叠数目
template <typename T>
struct check_ItemInfo_tag<T, 3>
{
};

// 是否绑定
template <typename T>
struct check_ItemInfo_tag<T, 4>
{
};



// 道具信息
template <typename T, int tag>
struct check_ItemsInfo_tag
{
};


// 道具信息
template <typename T>
struct check_ItemsInfo_tag<T, 1>
{
};




// 好友信息
static int32_t MarshalTag(FriendInfo * pStruct, char * buffer, int32_t size)  
{	
	int ret = 0;
	
	 
	// 1 唯一id	
	MARSHAL_NORMAL(1, pStruct->m_Guid);
	 
	// 2 好感度	
	MARSHAL_NORMAL(2, pStruct->m_Nice);
	
	
	return ret;
}

// 好友信息
static int32_t UnmarshalTag(FriendInfo * pStruct, const char * buffer, int32_t size)  
{
	  
	
	bool ok = false;
	
	while (size > 0)
	{
		if (!(EXPECT_TAG(1) ||
			EXPECT_TAG(2) ||
			 0))
		{
			int type = 0;
			UNMARSHAL_TAG(type);
			SKIP_DATA(type);
			continue;
		}
		
		
		// 1 唯一id	
		UNMARSHAL_NORMAL(1, pStruct->m_Guid);
		
		// 2 好感度	
		UNMARSHAL_NORMAL(2, pStruct->m_Nice);
		
	}
	
	return 0;
	
}


// 好友信息
static int32_t MarshalTag(FriendsInfo * pStruct, char * buffer, int32_t size)  
{	
	int ret = 0;
	
	 
	// 1 好友信息	
	for (int32_t i = 0; i < (int32_t)pStruct->m_Num && i < FRIEND_LEN; i++)
	{
		MARSHAL_STRUCT(1, pStruct->m_FriendInfo[i]);
	}
	
	
	return ret;
}

// 好友信息
static int32_t UnmarshalTag(FriendsInfo * pStruct, const char * buffer, int32_t size)  
{
	
	// 1 好友信息 Len
	pStruct->m_Num = 0;
	  
	
	bool ok = false;
	
	while (size > 0)
	{
		if (!(EXPECT_TAG(1) ||
			 0))
		{
			int type = 0;
			UNMARSHAL_TAG(type);
			SKIP_DATA(type);
			continue;
		}
		
		
		// 1 好友信息	
		while (EXPECT_TAG(1))
		{
			if ((int32_t)pStruct->m_Num < FRIEND_LEN)
			{
				UNMARSHAL_STRUCT(1, pStruct->m_FriendInfo[pStruct->m_Num]);
				if (ok)
				{
					pStruct->m_Num++;
				}
				else
				{
					break;
				}
			}
			else
			{
				int type = 0;
				UNMARSHAL_TAG(type);
				SKIP_DATA(type);
			}
		}
		
	}
	
	return 0;
	
}


// 道具信息
static int32_t MarshalTag(ItemInfo * pStruct, char * buffer, int32_t size)  
{	
	int ret = 0;
	
	 
	// 1 唯一id	
	MARSHAL_NORMAL(1, pStruct->m_Guid);
	 
	// 2 表格id	
	MARSHAL_NORMAL(2, pStruct->m_ItemId);
	 
	// 3 堆叠数目	
	MARSHAL_NORMAL(3, pStruct->m_Num);
	 
	// 4 是否绑定	
	MARSHAL_NORMAL(4, pStruct->m_IsBind);
	
	
	return ret;
}

// 道具信息
static int32_t UnmarshalTag(ItemInfo * pStruct, const char * buffer, int32_t size)  
{
	  
	
	bool ok = false;
	
	while (size > 0)
	{
		if (!(EXPECT_TAG(1) ||
			EXPECT_TAG(2) ||
			EXPECT_TAG(3) ||
			EXPECT_TAG(4) ||
			 0))
		{
			int type = 0;
			UNMARSHAL_TAG(type);
			SKIP_DATA(type);
			continue;
		}
		
		
		// 1 唯一id	
		UNMARSHAL_NORMAL(1, pStruct->m_Guid);
		
		// 2 表格id	
		UNMARSHAL_NORMAL(2, pStruct->m_ItemId);
		
		// 3 堆叠数目	
		UNMARSHAL_NORMAL(3, pStruct->m_Num);
		
		// 4 是否绑定	
		UNMARSHAL_NORMAL(4, pStruct->m_IsBind);
		
	}
	
	return 0;
	
}


// 道具信息
static int32_t MarshalTag(ItemsInfo * pStruct, char * buffer, int32_t size)  
{	
	int ret = 0;
	
	 
	// 1 道具信息	
	for (int32_t i = 0; i < (int32_t)pStruct->m_Num && i < ITEM_LEN; i++)
	{
		MARSHAL_STRUCT(1, pStruct->m_ItemInfo[i]);
	}
	
	
	return ret;
}

// 道具信息
static int32_t UnmarshalTag(ItemsInfo * pStruct, const char * buffer, int32_t size)  
{
	
	// 1 道具信息 Len
	pStruct->m_Num = 0;
	  
	
	bool ok = false;
	
	while (size > 0)
	{
		if (!(EXPECT_TAG(1) ||
			 0))
		{
			int type = 0;
			UNMARSHAL_TAG(type);
			SKIP_DATA(type);
			continue;
		}
		
		
		// 1 道具信息	
		while (EXPECT_TAG(1))
		{
			if ((int32_t)pStruct->m_Num < ITEM_LEN)
			{
				UNMARSHAL_STRUCT(1, pStruct->m_ItemInfo[pStruct->m_Num]);
				if (ok)
				{
					pStruct->m_Num++;
				}
				else
				{
					break;
				}
			}
			else
			{
				int type = 0;
				UNMARSHAL_TAG(type);
				SKIP_DATA(type);
			}
		}
		
	}
	
	return 0;
	
}


}
