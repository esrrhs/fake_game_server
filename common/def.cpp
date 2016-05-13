#include "def.h"
#include "configloader.h"
#include "libcommon.h"
#include "lu.h"
#include "fakescript.h"
#include "NetMsg.h"
#include "ServerNetMsg.h"

CConfigLoader g_CConfigLoader;
bool g_refresh = false;
bool g_exit = false;
bool g_isActive = false;

uint64_t make_guid(uint32_t groupid, uint32_t srvid, uint64_t & conter)
{
	uint64_t base = 0;

	uint64_t nTemp = groupid;
	base += nTemp << GROUP_MOVE;

	nTemp = srvid;
	base += nTemp << SERVER_MOVE;

	uint64_t nRes = 0;
	nTemp = 0;

	nRes += base;

#ifdef WIN32
	nTemp = conter++;
#else
	nTemp = __sync_fetch_and_add(&conter, 1);
#endif
	nRes += nTemp << COUNTER_MOVE;

	int nTime = (int)time(NULL);
	nTime &= ~(7 << COUNTER_MOVE);

	nRes += nTime;

	return nRes;
}

void sSignalProcess(int nSig)
{
	std::cout << "signal: " << nSig << std::endl;
	switch (nSig)
	{
#ifndef _WIN32
	case SIGUSR2:
	{
		g_refresh = true;
	}
	break;
#else
	case SIGINT:
	{
		signal(nSig, sSignalProcess);
		g_refresh = true;
	}
	break;
#endif
#ifndef _WIN32
	case SIGUSR1:
	case SIGHUP:
#else
	case SIGBREAK:
#endif
	{
		g_exit = true;
	}
	break;
	case SIGILL:
		//case SIGSEGV:
	case SIGABRT:
#ifndef _WIN32
	case SIGBUS:
#else
#endif
	default:
	{
		signal(nSig, SIG_DFL);
		std::cout << "Srv Crashed Signal[" << nSig << "]!!!" << std::endl;
	}
	break;
	}
}

bool common_ini(const char * name, int argc, char *argv[])
{
#ifdef WIN32
	SetConsoleTitle(name);
#endif

	finit_signal(sSignalProcess);

	inilu();

	fcmdparser::ptr()->parse(argc, argv);
	flog_system::ptr()->setname(name);

	if (!g_CConfigLoader.LoadCfg("config.xml"))
	{
		LOG_ERROR("LoadCfg config.xml fail");
		return false;
	}

	return true;
}

void fk_error_log(fake * fk, int eno, const char * file, int lineno, const char * func, const char * str)
{
	std::string cs = fkgetcurcallstack(fk);
	std::string cr = fkgetcurroutine(fk);
	LOG_ERROR("fake error[%d] in file(%s:%d) func(%s) : %s\ncall stack :\n%s\ncur routine :\n%s", 
		eno, file, lineno, func, str, cs.c_str(), cr.c_str());
}

void fk_log_debug(const char * log)
{
	LOG_DEBUG("%s", log);
}

void fk_log_error(const char * log)
{
	LOG_ERROR("%s", log);
}

void fk_log_sys(const char * log)
{
	LOG_SYS("%s", log);
}

bool fake_ini(fake * & fk, const std::string & name, int argc, char *argv[])
{
	fakeconfig fakecfg;
	fk = newfake(&fakecfg);

	fkopenbaselib(fk);
	fkseterrorfunc(fk, fk_error_log);

	fkparse(fk, name.c_str());
	if (fkerror(fk))
	{
#ifdef WIN32
		assert(0);
#endif // WIN32
		return false;
	}

	Fproto::bindscript_netmsg(fk);
	Fproto::bindscript_servernetmsg(fk);

	fkreg<void>(fk, "log_debug", &fk_log_debug);
	fkreg<void>(fk, "log_error", &fk_log_error);
	fkreg<void>(fk, "log_sys", &fk_log_sys);

	return true;
}
