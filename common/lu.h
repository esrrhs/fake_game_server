#pragma once

#ifdef WIN32
#if _MSC_VER > 1500
#include <stdint.h>
#else
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
#undef INT8_MIN  
#undef INT16_MIN 
#undef INT32_MIN 
#undef INT64_MIN 
#undef INT8_MAX  
#undef INT16_MAX 
#undef INT32_MAX 
#undef INT64_MAX 
#undef UINT8_MAX 
#undef UINT16_MAX
#undef UINT32_MAX
#undef UINT64_MAX
#define INT8_MIN         (-127i8 - 1)
#define INT16_MIN        (-32767i16 - 1)
#define INT32_MIN        (-2147483647i32 - 1)
#define INT64_MIN        (-9223372036854775807i64 - 1)
#define INT8_MAX         127i8
#define INT16_MAX        32767i16
#define INT32_MAX        2147483647i32
#define INT64_MAX        9223372036854775807i64
#define UINT8_MAX        0xffui8
#define UINT16_MAX       0xffffui16
#define UINT32_MAX       0xffffffffui32
#define UINT64_MAX       0xffffffffffffffffui64
#endif
#endif
#include <stdlib.h>
#include <string.h>

#define LU_VERSION "1.0"
#define LU_VERSION_NUM 100
#define LU_AUTHOR "esrrhs@163.com"

// �����
enum eluerror
{
    elu_ok = 0,
};

typedef void * (*lumalloc)(size_t size);
typedef void (*lufree)(void *ptr);

#define LU_API extern "C"

// ����
struct lu;

enum lutype
{
	// tcp������
	lut_tcpserver,
	// tcp�ͻ���
	lut_tcpclient,
};

#define LU_IP_SIZE 24

// �û�����
union luuserdataparam
{
    void * _ptr;
    uint8_t _u8;
    int8_t _i8;
    uint16_t _u16;
    int16_t _i16;
    uint32_t u32;
    int32_t i32;
    uint64_t u64;
    int64_t i64;
    float _fl;
    double _dl;
};
#define LU_MAX_USER_DATA_PARAM 4
struct luuserdata
{
    luuserdataparam params[LU_MAX_USER_DATA_PARAM];
};

// �ص� ���ӳɹ���������������
typedef void (*cb_conn_open)(lu * l, int connid, luuserdata & userdata);
// �ص� ���ݵ���
typedef void(*cb_conn_recv_packet)(lu * l, int connid, const char * buff, size_t size, luuserdata & userdata);
// �ص� ���ӹر�
typedef void(*cb_conn_close)(lu * l, int connid, luuserdata & userdata, int reason);

struct luconfig
{
    luconfig() : lum(&malloc), luf(&free), type(lut_tcpserver),
		port(8888), maxconnnum(1000),
		backlog(128), linger(0), 
		iskeepalive(true), keepidle(60), keepinterval(5), keepcount(3),
		isnonblocking(true), isnodelay(true),
		sendbuff(1024*1024), recvbuff(1024*1024),
		socket_sendbuff(1024*256), socket_recvbuff(1024*256),
		waittimeout(1), cco(0), ccrp(0), ccc(0), 
		maxrecvpacket_perframe(10000), maxpacketlen(100*1024),
		isencrypt(true), iscompress(true)
    {
		strcpy(ip, "127.0.0.1");
	}
	// �ڴ����
    lumalloc lum;
	lufree luf;
	// ����
	lutype type;
	// ip�˿�
	char ip[LU_IP_SIZE];
	uint16_t port;
	// ���������
	int maxconnnum;
	// tcp����
	int backlog;
	int linger;
	// ����
	bool iskeepalive;
	int keepidle;
	int keepinterval;
	int keepcount;
	// ������
	bool isnonblocking;
	// ���ӳ�
	bool isnodelay;
	// ���ͽ��ջ�����
	int sendbuff;
	int recvbuff;
	// socket���ͽ��ջ�������С
	int socket_sendbuff;
	int socket_recvbuff;
	// select��ʱʱ��
	int waittimeout;
	// �ص�����
	cb_conn_open cco;
	cb_conn_recv_packet ccrp;
	cb_conn_close ccc;
	// ÿ֡������Ϣ��Ŀ
	int32_t maxrecvpacket_perframe;
	// ��Ϣ���size
	uint32_t maxpacketlen;
	// �Ƿ����
	bool isencrypt;
	// �Ƿ�ѹ��
	bool iscompress;
	// ����lu�ϵ��û�����
	luuserdata userdata;
};

// ��ʼ��
LU_API void inilu();

// �������
LU_API lu * newlu(luconfig * cfg = 0);
LU_API void dellu(lu * l);

// ����
LU_API void ticklu(lu * l);

// ����
LU_API lutype gettypelu(lu * l);

// ������Ϣ
enum luerrortype
{
    luet_ok,
    // ���ʹ���
    luet_typeerr,
    // ������Ч
    luet_conninvalid,
    // ��Ϣ̫��
    luet_msgtoobig,
    // ��������
    luet_sendbufffull,
    // ѹ��ʧ��
    luet_compressfail,
    // ����ʧ��
    luet_encryptfail,
};

// ������Ϣ
LU_API int sendlu(lu * l, const char * buffer, size_t size, int connid = -1);

// �û�����
LU_API luuserdata * getlu_userdata(lu * l);

// �����ϵ��û�����
LU_API luuserdata * getlu_conn_userdata(lu * l, int connid = -1);
