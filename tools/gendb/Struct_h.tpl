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

{{range .Structs}}  
// {{.Comment}} 
static std::string create_{{.Table}}()
{
	std::stringstream ret;
	ret << "\
		DROP TABLE IF EXISTS `{{.Table}}`;\n\
		CREATE TABLE `{{.Table}}` (\n\
		{{range .Members}}{{if eq .Type "char"}}`{{.Name}}` varchar(" << {{.Length}} << ") NOT NULL COMMENT '{{.Comment}}',\n\
		{{else if is_int_type .Type}}`{{.Name}}` int(11) NOT NULL DEFAULT '0' COMMENT '{{.Comment}}',\n\
		{{else if is_int64_type .Type}}`{{.Name}}` bigint(20) NOT NULL COMMENT '{{.Comment}}',\n\
		{{else if is_float_type .Type}}`{{.Name}}` double NOT NULL DEFAULT '0' COMMENT '{{.Comment}}',\n\
		{{else}}`{{.Name}}` blob NOT NULL COMMENT '{{.Comment}}',\n\
		{{end}}{{end}}`last_updatetime` datetime NOT NULL,\n\
		PRIMARY KEY (`{{.Key}}`)\
	) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='{{.Comment}}';\n";
	
	return ret.str();
}
{{end}} 

static std::string create_all_table()
{
	std::string ret;
	{{range .Structs}}  
	// {{.Comment}} 
	ret += create_{{.Table}}();
	{{end}}
	return ret;
} 

{{range .Structs}}  
// {{.Comment}} 
static bool select_{{.Table}}(dbthread * t, {{.Name}} * pdata)
{
	MYSQL *mysql = t->getmysql();
	{{.Name}} & data = *pdata;
	
	std::stringstream ss;
	ss << "select {{range .Members}}{{.Name}},{{end}}{{.Key}} from {{.Table}} where {{.Key}} = {{if .KeyIsString}}'" << escape_string(t, data.m_{{.Key}}) << "'";{{else}}" << data.m_{{.Key}};{{end}}
	
	int ret = mysql_real_query(mysql, ss.str().c_str(), ss.str().size());
	if (ret != 0)
	{
		LOG_ERROR("select_{{.Table}} error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}
	
	MYSQL_RES * res_ptr = mysql_store_result(mysql);
	if (!res_ptr)
	{
		LOG_ERROR("select_{{.Table}} mysql_store_result error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}

	int num = mysql_num_rows(res_ptr);
	if (num <= 0)
	{
		mysql_free_result(res_ptr);
		LOG_DEBUG("select_{{.Table}} empty %s", ss.str().c_str());
		return false;
	}

	MYSQL_ROW sqlrow = mysql_fetch_row(res_ptr);
	if (!sqlrow) 
	{
		mysql_free_result(res_ptr);
		LOG_ERROR("select_{{.Table}} mysql_fetch_row error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}
	
	unsigned long * lengths = mysql_fetch_lengths(res_ptr);
	if (!lengths) 
	{
		mysql_free_result(res_ptr);
		LOG_ERROR("select_{{.Table}} mysql_fetch_lengths error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}
	
	int i = 0;
	{{$TableName := .Table}}{{range .Members}}
	{{if eq .Type "char"}}
	// {{.Comment}}
	fstrcopy(data.m_{{.Name}}, sqlrow[i++]);
	{{else if is_int_type .Type}}
	// {{.Comment}}
	data.m_{{.Name}} = fatoi(sqlrow[i++]);
	{{else if is_int64_type .Type}}
	// {{.Comment}}
	data.m_{{.Name}} = fatol(sqlrow[i++]);
	{{else if is_float_type .Type}}
	// {{.Comment}}
	data.m_{{.Name}} = fatof(sqlrow[i++]);
	{{else}}
	// {{.Comment}}
	data.m_{{.Name}}.Clear();
	if (lengths[i] > 0)
	{
		int ret = UnmarshalTag(&data.m_{{.Name}}, sqlrow[i], lengths[i]);
		if (ret < 0)
		{
			mysql_free_result(res_ptr);
			LOG_ERROR("select_{{$TableName}} UnmarshalTag m_{{.Name}} Fail %d", ret);
			return false;
		}
	}
	i++;
	{{end}}{{end}}
	
	mysql_free_result(res_ptr);
	
	return true;
}
{{end}}  

{{range .Structs}}  
// {{.Comment}} 
static bool update_{{.Table}}(dbthread * t, {{.Name}} * pdata)
{
	MYSQL *mysql = t->getmysql();
	{{.Name}} & data = *pdata;
	
	{{$TableName := .Table}}{{range .Members}}{{if is_user_type .Type}}
	std::string str{{.Name}} = escape_blob(t, &data.m_{{.Name}});
	if (str{{.Name}} == "error")
	{
		LOG_ERROR("update_{{$TableName}} escape_blob m_{{.Name}} Fail");
		return false;
	}
	{{end}}{{end}}
	
	std::stringstream ss;
	ss << "insert into {{.Table}}({{range .Members}}{{.Name}},{{end}}last_updatetime) "
	"values({{range .Members}}{{if eq .Type "char"}}'" << data.m_{{.Name}} << "'{{else if is_int_type .Type}}" << data.m_{{.Name}} << "{{else if is_int64_type .Type}}" << data.m_{{.Name}} << "{{else if is_float_type .Type}}" << data.m_{{.Name}} << "{{else}}'" << str{{.Name}} << "'{{end}},{{end}}NOW()) "
	"on duplicate key update {{range .Members}}{{.Name}}={{if eq .Type "char"}}'" << data.m_{{.Name}} << "'{{else if is_int_type .Type}}" << data.m_{{.Name}} << "{{else if is_int64_type .Type}}" << data.m_{{.Name}} << "{{else if is_float_type .Type}}" << data.m_{{.Name}} << "{{else}}'" << str{{.Name}} << "'{{end}},{{end}}last_updatetime=NOW();";
	
	int ret = mysql_real_query(mysql, ss.str().c_str(), ss.str().size());
	if(ret < 0)
	{
		LOG_ERROR("update_{{.Table}} error: %s, %s", ss.str().c_str(), mysql_error(mysql));
		return false;
	}
	
	return true;
}
{{end}}  

struct TmpStruct
{
{{range .Structs}}  
	// {{.Comment}} 
	{{.Name}} m_{{.Name}};
{{end}}
};

{{range .Structs}}  
// {{.Comment}} 
static {{.Name}} * get_tmp_{{.Name}}(dbthread * t)
{
	t->get_tmp_struct()->m_{{.Name}}.Clear();
	return &(t->get_tmp_struct()->m_{{.Name}});
}
{{end}}

static void bindscript_dbnetmsg(fake * fk)
{
{{range .Structs}}  
	// {{.Comment}} 
	fkreg(fk, "create_{{.Table}}", create_{{.Table}});
	fkreg(fk, "select_{{.Table}}", select_{{.Table}});
	fkreg(fk, "update_{{.Table}}", update_{{.Table}});
	fkreg(fk, "get_tmp_{{.Name}}", get_tmp_{{.Name}});
{{end}}
}

}
