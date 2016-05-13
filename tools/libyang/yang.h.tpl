#pragma once

#include "yang.h"
#include "NetMsg.h"

namespace Fproto
{

{{fake func is_normal_type(str) 
	if str == "int8_t" or
		str == "uint8_t" or
		str == "int16_t" or
		str == "uint16_t" or
		str == "int32_t" or
		str == "uint32_t" or
		str == "int64_t" or
		str == "uint64_t" or
		str == "float" or
		str == "double" then
		
		return true
	
	else
		return false
	end
end
}}
{{range .memlist}}
{{if : p := data["param"] if p["type"] != "union" then return true else return false end}}
{{with .param}}// {{.comment}}
template <typename T, int tag>
struct check_{{.name}}_tag{{end}}
{
};
{{do : p := data["param"] global["structname"]= p["name"] return "" }}
{{range .memlist}}{{with .param}}
// {{.comment}}
template <typename T>
struct check_{{do : return global["structname"]}}_tag<T, {{.tag}}>{{end}}
{
};
{{end}}
{{end}}{{end}}
{{range .memlist}}
{{if : p := data["param"] if p["type"] == "union" then return true else return false end}}
{{with .param}}// {{.comment}}
static int32_t MarshalTag({{.name}} * pStruct, char * buffer, int32_t size){{end}}  
{
	int32_t ret = 0;
	
	// type
	MARSHAL_NORMAL(1, pStruct->m_Type);
	
	switch (pStruct->m_Type)
	{
	{{range .memlist}}{{with .param}}  
	// {{.comment}}
	case {{.ref}}:
		{
			MARSHAL_STRUCT(2, pStruct->m_{{.name}});
		}
		break;
	{{end}}{{end}}    
	default:
		return -1;
	}
	return ret;
}

{{with .param}}// {{.comment}}
static int32_t UnmarshalTag({{.name}} * pStruct, const char * buffer, int32_t size){{end}}  
{	
	bool ok = false;
	
	// type
	UNMARSHAL_NORMAL(1, pStruct->m_Type);
	
	switch (pStruct->m_Type)
	{
	{{range .memlist}}{{with .param}}    
	// {{.comment}}  
	case {{.ref}}:
		{
			UNMARSHAL_STRUCT(2, pStruct->m_{{.name}});
		}
		break;
	{{end}}{{end}}    
	default:
		return -1;
	}
	return 0;
}
{{else}}
{{with .param}}// {{.comment}}
static int32_t MarshalTag({{.name}} * pStruct, char * buffer, int32_t size){{end}}  
{	
	int ret = 0;
	
	{{range .memlist}}{{with .param}} 
	// {{.tag}} {{.comment}}	
	{{if : if data["type"] == "char" then return true else return false end}}MARSHAL_STRING({{.tag}}, pStruct->m_{{.name}}, {{.length}});
	{{else}}{{if : if is data["length"] then return true else return false end}}for (int32_t i = 0; i < (int32_t)pStruct->m_{{.ref}} && i < {{.length}}; i++)
	{
		{{if : if is is_normal_type(data["type"]) then return true else return false end}}MARSHAL_NORMAL({{.tag}}, pStruct->m_{{.name}}[i]);{{else}}MARSHAL_STRUCT({{.tag}}, pStruct->m_{{.name}}[i]);{{end}}
	}
	{{else}}{{if : if is is_normal_type(data["type"]) then return true else return false end}}MARSHAL_NORMAL({{.tag}}, pStruct->m_{{.name}});{{else}}MARSHAL_STRUCT({{.tag}}, pStruct->m_{{.name}});{{end}}
	{{end}}{{end}}{{end}}{{end}}
	
	return ret;
}

{{with .param}}// {{.comment}}
static int32_t UnmarshalTag({{.name}} * pStruct, const char * buffer, int32_t size){{end}}  
{
	{{range .memlist}}{{with .param}}{{if : if is data["ref"] then return true else return false end}}
	// {{.tag}} {{.comment}} Len
	pStruct->m_{{.ref}} = 0;
	{{end}}{{end}}{{end}}  
	
	bool ok = false;
	
	while (size > 0)
	{
		if (!({{range .memlist}}{{with .param}}EXPECT_TAG({{.tag}}) ||
			{{end}}{{end}} 0))
		{
			int type = 0;
			UNMARSHAL_TAG(type);
			SKIP_DATA(type);
			continue;
		}
		
		{{range .memlist}}{{with .param}}
		// {{.tag}} {{.comment}}	
		{{if : if data["type"] == "char" then return true else return false end}}UNMARSHAL_STRING({{.tag}}, pStruct->m_{{.name}}, {{.length}});
		{{else}}{{if : if is data["length"] then return true else return false end}}while (EXPECT_TAG({{.tag}}))
		{
			if ((int32_t)pStruct->m_{{.ref}} < {{.length}})
			{
				{{if : if is is_normal_type(data["type"]) then return true else return false end}}UNMARSHAL_NORMAL({{.tag}}, pStruct->m_{{.name}}[pStruct->m_{{.ref}}]);{{else}}UNMARSHAL_STRUCT({{.tag}}, pStruct->m_{{.name}}[pStruct->m_{{.ref}}]);{{end}}
				if (ok)
				{
					pStruct->m_{{.ref}}++;
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
		{{else}}{{if : if is is_normal_type(data["type"]) then return true else return false end}}UNMARSHAL_NORMAL({{.tag}}, pStruct->m_{{.name}});{{else}}UNMARSHAL_STRUCT({{.tag}}, pStruct->m_{{.name}});{{end}}
		{{end}}{{end}}{{end}}{{end}}
	}
	
	return 0;
	{{end}}
}
{{end}}

}
