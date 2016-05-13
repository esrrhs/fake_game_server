#pragma once

#include <stdint.h>
#include <string.h>
#include <cassert>

enum ProtoDataType
{
	PDT_int8,
	PDT_uint8,
	PDT_int16,
	PDT_uint16,
	PDT_int32,
	PDT_uint32,
	PDT_int64,
	PDT_uint64,
	PDT_float,
	PDT_double,
	PDT_String,
	PDT_Struct,
};

template <typename T>
static inline ProtoDataType __type_id(const T & t)
{
	return (ProtoDataType)-1;
}

template <>
inline ProtoDataType __type_id(const int8_t & t)
{
	return PDT_int8;
}

template <>
inline ProtoDataType __type_id(const uint8_t & t)
{
	return PDT_uint8;
}

template <>
inline ProtoDataType __type_id(const int16_t & t)
{
	return PDT_int16;
}

template <>
inline ProtoDataType __type_id(const uint16_t & t)
{
	return PDT_uint16;
}

template <>
inline ProtoDataType __type_id(const int32_t & t)
{
	return PDT_int32;
}

template <>
inline ProtoDataType __type_id(const uint32_t & t)
{
	return PDT_uint32;
}

template <>
inline ProtoDataType __type_id(const int64_t & t)
{
	return PDT_int64;
}

template <>
inline ProtoDataType __type_id(const uint64_t & t)
{
	return PDT_uint64;
}

template <>
inline ProtoDataType __type_id(const float & t)
{
	return PDT_float;
}

template <>
inline ProtoDataType __type_id(const double & t)
{
	return PDT_double;
}

template <typename T>
static inline void __marshal_value(char * buffer, const T & t)
{
}

template <>
inline void __marshal_value(char * buffer, const int8_t & t)
{
	*(int8_t*)buffer = t;
}

template <>
inline void __marshal_value(char * buffer, const uint8_t & t)
{
	*(uint8_t*)buffer = t;
}

template <>
inline void __marshal_value(char * buffer, const int16_t & t)
{
	*(int16_t*)buffer = t;
}

template <>
inline void __marshal_value(char * buffer, const uint16_t & t)
{
	*(uint16_t*)buffer = t;
}

template <>
inline void __marshal_value(char * buffer, const int32_t & t)
{
	*(int32_t*)buffer = t;
}

template <>
inline void __marshal_value(char * buffer, const uint32_t & t)
{
	*(uint32_t*)buffer = t;
}

template <>
inline void __marshal_value(char * buffer, const int64_t & t)
{
	*(int64_t*)buffer = t;
}

template <>
inline void __marshal_value(char * buffer, const uint64_t & t)
{
	*(uint64_t*)buffer = t;
}

template <>
inline void __marshal_value(char * buffer, const float & t)
{
	*(float*)buffer = t;
}

template <>
inline void __marshal_value(char * buffer, const double & t)
{
	*(double*)buffer = t;
}

#define MARSHAL_TAG(tag, type) \
	if (size < sizeof(uint16_t)) \
	{ \
		return -1; \
	} \
	*(uint8_t*)buffer = (uint8_t)tag; \
	*(uint8_t*)(buffer + 1) = (uint8_t)type; \
	buffer += sizeof(uint16_t); \
	ret += sizeof(uint16_t); \
	size -= sizeof(uint16_t);

#define MARSHAL_LEN(len) \
	if (size < sizeof(uint16_t)) \
	{ \
		return -1; \
	} \
	*(uint16_t*)buffer = (uint16_t)len; \
	buffer += sizeof(uint16_t); \
	ret += sizeof(uint16_t); \
	size -= sizeof(uint16_t);

#define MARSHAL_STRING(tag, str, maxlen) \
	{ \
		MARSHAL_TAG(tag, PDT_String); \
		(str)[(maxlen) - 1] = 0; \
		int32_t len = (int32_t)strlen(str); \
		MARSHAL_LEN(len); \
		if (size < len) \
		{ \
			return -1; \
		} \
		memcpy(buffer, str, len); \
		buffer += len; \
		ret += len; \
		size -= len; \
	}

#define MARSHAL_NORMAL(tag, data) \
	{ \
		int type = __type_id(data); \
		assert(type > 0); \
		MARSHAL_TAG(tag, type); \
		if (size < sizeof(data)) \
		{ \
			return -1; \
		} \
		__marshal_value(buffer, data); \
		buffer += sizeof(data); \
		ret += sizeof(data); \
		size -= sizeof(data); \
	}

#define MARSHAL_STRUCT(tag, s) \
	{ \
		MARSHAL_TAG(tag, PDT_Struct); \
		char * oldbuffer = buffer; \
		int32_t len = 0; \
		MARSHAL_LEN(len); \
		len = MarshalTag(&(s), buffer, size); \
		if (len < 0) \
		{ \
			return -1;\
		} \
		*(uint16_t*)oldbuffer = len; \
		buffer += len; \
		ret += len; \
		size -= len; \
	}

#define UNMARSHAL_TAG(type) \
	if (size < sizeof(uint16_t)) \
	{ \
		return -1; \
	} \
	type = *(uint8_t*)(buffer + 1); \
	buffer += sizeof(uint16_t); \
	size -= sizeof(uint16_t);

#define UNMARSHAL_LEN(len) \
	if (size < sizeof(uint16_t)) \
	{ \
		return -1; \
	} \
	len = *(uint16_t*)buffer; \
	buffer += sizeof(uint16_t); \
	size -= sizeof(uint16_t);

#define EXPECT_TAG(tag) (size > 0 && *(uint8_t*)buffer == tag)

#define SKIP_DATA(type) \
	switch (type) \
	{ \
	case PDT_int8: \
	case PDT_uint8: \
		if (size < sizeof(int8_t)) \
		{ \
			return -1; \
		} \
		buffer += sizeof(int8_t); \
		size -= sizeof(int8_t); \
		break; \
	case PDT_int16: \
	case PDT_uint16: \
		if (size < sizeof(int16_t)) \
		{ \
			return -1; \
		} \
		buffer += sizeof(int16_t); \
		size -= sizeof(int16_t); \
		break; \
	case PDT_int32: \
	case PDT_uint32: \
		if (size < sizeof(int32_t)) \
		{ \
			return -1; \
		} \
		buffer += sizeof(int32_t); \
		size -= sizeof(int32_t); \
		break; \
	case PDT_int64: \
	case PDT_uint64: \
		if (size < sizeof(int64_t)) \
		{ \
			return -1; \
		} \
		buffer += sizeof(int64_t); \
		size -= sizeof(int64_t); \
		break; \
	case PDT_float: \
		if (size < sizeof(float)) \
		{ \
			return -1; \
		} \
		buffer += sizeof(float); \
		size -= sizeof(float); \
		break; \
	case PDT_double: \
		if (size < sizeof(double)) \
		{ \
			return -1; \
		} \
		buffer += sizeof(double); \
		size -= sizeof(double); \
		break; \
	case PDT_String: \
	case PDT_Struct: \
		{ \
			int len = 0; \
			UNMARSHAL_LEN(len); \
			if (size < len) \
			{ \
				return -1; \
			} \
			buffer += len; \
			size -= len; \
		} \
		break; \
	};

#define UNMARSHAL_STRING(tag, str, maxlen) \
	ok = false; \
	if (EXPECT_TAG(tag)) \
	{ \
		int type = 0; \
		UNMARSHAL_TAG(type); \
		if (type == PDT_String) \
		{ \
			int len = 0; \
			UNMARSHAL_LEN(len); \
			if (size < len) \
			{ \
				return -1; \
			} \
			strncpy(str, buffer, (len > (maxlen) ? (maxlen) : len)); \
			(str)[(maxlen) - 1] = 0; \
			buffer += len; \
			size -= len; \
			ok = true; \
		} \
		else \
		{ \
			SKIP_DATA(type); \
		} \
	}

#define UNMARSHAL_NORMAL(tag, data) \
	ok = false; \
	if (EXPECT_TAG(tag)) \
	{ \
		int type = 0; \
		UNMARSHAL_TAG(type); \
		switch (type) \
		{ \
		case PDT_int8: \
			if (size < sizeof(int8_t)) \
			{ \
				return -1; \
			} \
			data = *(int8_t*)buffer; \
			ok = true; \
			break; \
		case PDT_uint8: \
			if (size < sizeof(uint8_t)) \
			{ \
				return -1; \
			} \
			data = *(uint8_t*)buffer; \
			ok = true; \
			break; \
		case PDT_int16: \
			if (size < sizeof(int16_t)) \
			{ \
				return -1; \
			} \
			data = *(int16_t*)buffer; \
			ok = true; \
			break; \
		case PDT_uint16: \
			if (size < sizeof(uint16_t)) \
			{ \
				return -1; \
			} \
			data = *(uint16_t*)buffer; \
			ok = true; \
			break; \
		case PDT_int32: \
			if (size < sizeof(int32_t)) \
			{ \
				return -1; \
			} \
			data = *(int32_t*)buffer; \
			ok = true; \
			break; \
		case PDT_uint32: \
			if (size < sizeof(uint32_t)) \
			{ \
				return -1; \
			} \
			data = *(uint32_t*)buffer; \
			ok = true; \
			break; \
		case PDT_int64: \
			if (size < sizeof(int64_t)) \
			{ \
				return -1; \
			} \
			data = *(int64_t*)buffer; \
			ok = true; \
			break; \
		case PDT_uint64: \
			if (size < sizeof(uint64_t)) \
			{ \
				return -1; \
			} \
			data = *(uint64_t*)buffer; \
			ok = true; \
			break; \
		case PDT_float: \
			if (size < sizeof(float)) \
			{ \
				return -1; \
			} \
			data = *(float*)buffer; \
			ok = true; \
			break; \
		case PDT_double: \
			if (size < sizeof(double)) \
			{ \
				return -1; \
			} \
			data = *(double*)buffer; \
			ok = true; \
			break; \
		case PDT_String: \
		case PDT_Struct: \
			break; \
		}; \
		SKIP_DATA(type); \
	}

#define UNMARSHAL_STRUCT(tag, s) \
	ok = false; \
	if (EXPECT_TAG(tag)) \
	{ \
		int type = 0; \
		UNMARSHAL_TAG(type); \
		if (type == PDT_Struct) \
		{ \
			int len = 0; \
			UNMARSHAL_LEN(len); \
			if (size < len) \
			{ \
				return -1; \
			} \
			if (UnmarshalTag(&(s), buffer, len) == 0) \
			{ \
				ok = true; \
			} \
			buffer += len; \
			size -= len; \
		} \
	}

