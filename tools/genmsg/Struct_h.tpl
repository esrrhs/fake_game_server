#pragma once

#include "libcommon.h"
#include "fakescript.h"

#define PROTO_MIN(a,b) Min<int32_t>((int32_t)a, (int32_t)b)

namespace Fproto
{

{{range .Enums}}    
// {{.Comment}}  
enum {{.Name}}
{
	{{range .Members}} 
	// {{.Comment}}  
	{{.Name}} = {{.Type}},
	{{end}}  
};
{{end}}  

{{range .Enums}}{{if or (eq "MsgID" .Name) (eq "ServerMsgID" .Name)}}    
// {{.Comment}}  
static const char * Get{{.Name}}Name(int i)
{
	switch (i)
	{
	{{range .Members}} 
	// {{.Comment}}  
	case {{.Type}}:
		return "{{.Name}}";
	{{end}}  
	default:
		return "";
	}
}

static const char * Get{{.Name}}FuncName(int i)
{
	switch (i)
	{
	{{range .Members}} 
	// {{.Comment}}  
	case {{.Type}}:
		return "on_{{.Name}}";
	{{end}}  
	default:
		return "";
	}
}{{end}}  
{{end}}  

{{define "MarshalCheck"}}if (ret + (int32_t)sizeof(m_{{.Name}}) > size)
		{
			return -ret;
		}{{end}}
{{define "MarshalAdd"}}ret += sizeof(m_{{.Name}});
		destbuffer += sizeof(m_{{.Name}});{{end}}
		
{{define "MarshalNormal"}}{{template "MarshalCheck" .}}
		*({{.Type}}*)destbuffer = m_{{.Name}};
		{{template "MarshalAdd" .}}{{end}}		
	
{{define "UnmarshalCheck"}}if (ret + (int32_t)sizeof(m_{{.Name}}) > size)
		{
			return -ret;
		}{{end}}
{{define "UnmarshalAdd"}}ret += sizeof(m_{{.Name}});
		srcbuffer += sizeof(m_{{.Name}});{{end}}
{{define "UnmarshalNormal"}}{{template "UnmarshalCheck" .}}
		m_{{.Name}} = *({{.Type}}*)srcbuffer;
		{{template "UnmarshalAdd" .}}{{end}}	
		
{{range .Structs}}       
// {{.Comment}}  
enum E_{{.Name}}{{$StructName := .Name}}  
{
	{{if eq .Type "union"}}
	// Type
	E_{{$StructName}}_Type,
	{{range .Members}} 
	// {{.Comment}}  
	E_{{$StructName}}_{{.Name}},
	{{end}}  
	{{else}}
	{{range .Members}} 
	// {{.Comment}}  
	E_{{$StructName}}_{{.Name}},
	{{end}}  
	{{end}}
};

// {{.Comment}}  
struct {{.Name}}  
{  
	void Clear()
	{
		memset(this, 0, sizeof(*this));
	}

	{{if eq .Type "union"}}
	// Type 
	int32_t m_Type;
	
	union
	{
		{{range .Members}}  
		// {{.Comment}}  
		{{.Type}} m_{{.Name}}{{if .Length}}[{{.Length}}]{{end}};  
		{{end}}  
	};
	
	int32_t GetType()
	{
		return m_Type;
	}
	
	void SetType(int32_t type)
	{
		m_Type = type;
	}
	
	{{range .Members}}  
	// {{.Comment}}  
	{{.Type}} * Get{{.Name}}()
	{
		return &m_{{.Name}};
	}
	
	// {{.Comment}}  
	void Set{{.Name}}({{.Type}} * p{{.Type}})
	{
		if (p{{.Type}})
		{
			m_{{.Name}} = *p{{.Type}};
		}
	}
	{{end}}  
	
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
		{{range .Members}}  
		// {{.Comment}}  
		case {{.Ref}}:
			{
				int32_t ret{{.Name}} = m_{{.Name}}.Marshal(destbuffer, size - ret);
				if (ret{{.Name}} <= 0)
				{
					return -ret + ret{{.Name}};
				}
				ret += ret{{.Name}};
				destbuffer += ret{{.Name}};
			}
			break;
		{{end}}  
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
		{{range .Members}}  
		// {{.Comment}}  
		case {{.Ref}}:
			{
				int32_t ret{{.Name}} = m_{{.Name}}.Unmarshal(srcbuffer, size - ret);
				if (ret{{.Name}} <= 0)
				{
					return -ret + ret{{.Name}};
				}
				ret += ret{{.Name}};
				srcbuffer += ret{{.Name}};
			}
			break;
		{{end}}  
		default:
			return -ret;
		}
		return ret;
	}
	
	{{else}}
	{{range .Members}}  
	// {{.Comment}}  
	{{.Type}} m_{{.Name}}{{if .Length}}[{{.Length}}]{{end}};  
	{{end}}  
	
	{{range .Members}}
	{{if eq .Type "char"}}
	// {{.Comment}}  
	const char * Get{{.Name}}()
	{
		return m_{{.Name}};
	}
	
	// {{.Comment}}  
	void Set{{.Name}}(const char * _{{.Name}})
	{
		fstrcopy(m_{{.Name}}, _{{.Name}});
	}
	{{else if .Length}}{{if is_normal_type .Type}}
	// {{.Comment}}  
	{{.Type}} Get{{.Name}}(int i)
	{
		assert(i >= 0 && i < {{.Length}});
		if (i >= 0 && i < {{.Length}})
		{
			return m_{{.Name}}[i];
		}
		return m_{{.Name}}[0];
	}
	
	// {{.Comment}}  
	void Set{{.Name}}(int i, {{.Type}} _{{.Name}})
	{
		assert(i >= 0 && i < {{.Length}});
		if (i >= 0 && i < {{.Length}})
		{
			m_{{.Name}}[i] = _{{.Name}};
		}
	}
	{{else}}
	// {{.Comment}}  
	{{.Type}} * Get{{.Name}}(int i)
	{
		assert(i >= 0 && i < {{.Length}});
		if (i >= 0 && i < {{.Length}})
		{
			return &m_{{.Name}}[i];
		}
		return &m_{{.Name}}[0];
	}
	
	// {{.Comment}}  
	void Set{{.Name}}(int i, {{.Type}} * p{{.Type}})
	{
		assert(i >= 0 && i < {{.Length}});
		if (i >= 0 && i < {{.Length}} && p{{.Type}})
		{
			m_{{.Name}}[i] = *p{{.Type}};
		}
	}{{end}}
	{{else}}{{if is_normal_type .Type}}
	// {{.Comment}}  
	{{.Type}} Get{{.Name}}()
	{
		return m_{{.Name}};
	}
	
	// {{.Comment}}  
	void Set{{.Name}}({{.Type}} _{{.Name}})
	{
		m_{{.Name}} = _{{.Name}};
	}
	{{else}}
	// {{.Comment}}  
	{{.Type}} * Get{{.Name}}()
	{
		return &m_{{.Name}};
	}
	
	// {{.Comment}}  
	void Set{{.Name}}({{.Type}} * p{{.Type}})
	{
		if (p{{.Type}})
		{
			m_{{.Name}} = *p{{.Type}};
		}
	}
	{{end}}{{end}}{{end}}
	
	int32_t Marshal(char * destbuffer, int32_t size) const
	{
		int32_t ret = 0;
		{{range .Members}} 
		// {{.Comment}}	
		{{if eq .Type "char"}}{{template "MarshalCheck" .}}
		fstrcopy(destbuffer, m_{{.Name}}, sizeof(m_{{.Name}}));
		{{template "MarshalAdd" .}}
		{{else if .Length}}{{if is_normal_type .Type}}int32_t copy{{.Name}}Size = {{if .Ref}}sizeof({{.Type}}) * PROTO_MIN({{.Length}}, m_{{.Ref}}){{else}}sizeof(m_{{.Name}}){{end}};
		if (ret + copy{{.Name}}Size > size)
		{
			return -ret;
		}
		if (copy{{.Name}}Size > 0)
		{
			memcpy(destbuffer, m_{{.Name}}, copy{{.Name}}Size);
		}
		ret += copy{{.Name}}Size;
		destbuffer += copy{{.Name}}Size;{{else}}for (int32_t i = 0; i < {{.Length}}{{if .Ref}} && i < m_{{.Ref}}{{end}}; i++)
		{
			int32_t ret{{.Name}} = m_{{.Name}}[i].Marshal(destbuffer, size - ret);
			if (ret{{.Name}} <= 0)
			{
				return -ret + ret{{.Name}};
			}
			ret += ret{{.Name}};
			destbuffer += ret{{.Name}};
		}{{end}}
		{{else if is_normal_type .Type}}{{template "MarshalNormal" .}}
		{{else}}int32_t ret{{.Name}} = m_{{.Name}}.Marshal(destbuffer, size - ret);
		if (ret{{.Name}} <= 0)
		{
			return -ret + ret{{.Name}};
		}
		ret += ret{{.Name}};
		destbuffer += ret{{.Name}};
		{{end}} 
		{{end}} 
		return ret;
	}
	
	int32_t Unmarshal(const char * srcbuffer, int32_t size)
	{
		int32_t ret = 0;
		{{range .Members}} 
		// {{.Comment}}	
		{{if eq .Type "char"}}{{template "UnmarshalCheck" .}}
		fstrcopy(m_{{.Name}}, srcbuffer);
		{{template "UnmarshalAdd" .}}
		{{else if .Length}}{{if is_normal_type .Type}}int32_t copy{{.Name}}Size = {{if .Ref}}sizeof({{.Type}}) * PROTO_MIN({{.Length}}, m_{{.Ref}}){{else}}sizeof(m_{{.Name}}){{end}};
		if (ret + copy{{.Name}}Size > size)
		{
			return -ret;
		}
		if (copy{{.Name}}Size > 0)
		{
			memcpy(m_{{.Name}}, srcbuffer, copy{{.Name}}Size);
		}
		ret += copy{{.Name}}Size;
		srcbuffer += copy{{.Name}}Size;
		{{if .Ref}}m_{{.Ref}} = PROTO_MIN(m_{{.Ref}}, {{.Length}});{{end}}{{else}}for (int32_t i = 0; i < {{.Length}}{{if .Ref}} && i < m_{{.Ref}}{{end}}; i++)
		{
			int32_t ret{{.Name}} = m_{{.Name}}[i].Unmarshal(srcbuffer, size - ret);
			if (ret{{.Name}} <= 0)
			{
				return -ret + ret{{.Name}};
			}
			ret += ret{{.Name}};
			srcbuffer += ret{{.Name}};
		}
		{{if .Ref}}m_{{.Ref}} = PROTO_MIN(m_{{.Ref}}, {{.Length}});{{end}}{{end}}
		{{else if is_normal_type .Type}}{{template "UnmarshalNormal" .}}
		{{else}}int32_t ret{{.Name}} = m_{{.Name}}.Unmarshal(srcbuffer, size - ret);
		if (ret{{.Name}} <= 0)
		{
			return -ret + ret{{.Name}};
		}
		ret += ret{{.Name}};
		srcbuffer += ret{{.Name}};
		{{end}} 
		{{end}} 
		return ret;
	}
	{{end}}
};  
{{end}}  

static void bindscript_{{.Name}}(fake * fk)
{
{{range .Structs}}    
	// {{.Name}} {{.Comment}} {{$StructName := .Name}} 
	{{if eq .Type "union"}}
	{
		fkreg(fk, "GetType", &{{$StructName}}::GetType);
		fkreg(fk, "SetType", &{{$StructName}}::SetType);
		
		{{range .Members}}  
		// {{.Comment}}  
		fkreg(fk, "Get{{.Name}}", &{{$StructName}}::Get{{.Name}});
		fkreg(fk, "Set{{.Name}}", &{{$StructName}}::Set{{.Name}});
		{{end}}  
	}
	{{else}}
	{
		{{range .Members}}  
		// {{.Comment}}  
		fkreg(fk, "Get{{.Name}}", &{{$StructName}}::Get{{.Name}});
		fkreg(fk, "Set{{.Name}}", &{{$StructName}}::Set{{.Name}});
		{{end}}  
	}
	{{end}}
{{end}}  
}

}
