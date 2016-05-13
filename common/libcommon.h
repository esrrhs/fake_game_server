#pragma once

/////////////////////////////////////////////////////////////

// 帮助
#define SAFE_TEST_RET_VAL(EXPRESSION, TESTVALUE, RETURNVALUE) if((EXPRESSION) == (TESTVALUE)) return RETURNVALUE;
#define SAFE_TEST_RET(EXPRESSION, TESTVALUE) if((EXPRESSION) == (TESTVALUE)) return;
#define SAFE_TEST_CONTINUE(EXPRESSION, TESTVALUE) if((EXPRESSION) == (TESTVALUE)) continue;
#define SAFE_TEST_BREAK(EXPRESSION, TESTVALUE) if((EXPRESSION) == (TESTVALUE)) break;

#define SAFE_TEST_INDEX(EXPRESSION, TESTVALUE) if((EXPRESSION) < 0 || ((int)EXPRESSION) >= ((int)TESTVALUE)) return;
#define SAFE_TEST_INDEX_VAL(EXPRESSION, TESTVALUE, RETURNVALUE) if((EXPRESSION) < 0 || ((int)EXPRESSION) >= ((int)TESTVALUE)) return RETURNVALUE;

#define SAFE_DIFFER_TEST_RET_VAL(EXPRESSION, TESTVALUE, RETURNVALUE) if((EXPRESSION) != (TESTVALUE)) return RETURNVALUE;
#define SAFE_DIFFER_TEST_RET(EXPRESSION, TESTVALUE) if((EXPRESSION) != (TESTVALUE)) return;
#define SAFE_DIFFER_TEST_CONTINUE(EXPRESSION, TESTVALUE) if((EXPRESSION) != (TESTVALUE)) continue;
#define SAFE_DIFFER_TEST_BREAK(EXPRESSION, TESTVALUE) if((EXPRESSION) != (TESTVALUE)) break;

#define SAFE_DELETE(T, p) if (p) { delete p; p = 0; }
#define SAFE_FREE(p) if (p) { free(p); p = 0; }
#define SAFE_DELETE_ARRAY(T, p, num) if (p) { delete []p; p = 0; }

// 去掉警告
template <typename T>
void FUSE(T t) {}

// 断言
#define FASSERT(x) assert(x)

// 内联
#ifdef WIN32
	#define force_inline __forceinline
#else
	#define force_inline __inline__ __attribute__((always_inline))
#endif

// 格式化
#if defined(WIN32)
	#define tvsnprintf  _vsnprintf
	#define tstricmp    _stricmp
	#define tsnprintf   _snprintf
#else
	#define tvsnprintf  vsnprintf
	#define tstricmp    strcasecmp
	#define tsnprintf   snprintf
#endif

#define FPRINTF printf

#ifdef WIN32
#define PRINTF_CLOLOR_TYPE WORD
#else
#define PRINTF_CLOLOR_TYPE const char *
#endif

// socket
#ifdef WIN32
	#define GET_NET_ERROR WSAGetLastError()
	#define NET_BLOCK_ERROR WSAEWOULDBLOCK
	#define NET_BLOCK_ERROR_EX WSAEWOULDBLOCK
	#define FSOCKET_READ 0x1
	#define FSOCKET_WRITE 0x2
	#define FSOCKET_ERROR 0x4
#else
	#define GET_NET_ERROR errno
	#define NET_BLOCK_ERROR EWOULDBLOCK
	#define NET_BLOCK_ERROR_EX EAGAIN
	#define FSOCKET_READ EPOLLIN
	#define FSOCKET_WRITE EPOLLOUT
	#define FSOCKET_ERROR EPOLLERR
#endif

// printf
#ifdef WIN32
	#define F64d "I64d"
	#define F64u "I64u"
#else
	#define F64d "lld"
	#define F64u "llu"
#endif

// 数组长度
template <typename _CountofType, int _SizeOfArray>
char(*__fcountof_helper(_CountofType(&_Array)[_SizeOfArray]))[_SizeOfArray];
#define ARRAY_SIZE(_Array) (sizeof(*__fcountof_helper(_Array)) + 0)

// 线程局部变量
#ifdef WIN32
	#define THREAD_LOCAL_VALUE __declspec(thread)
#else
	#define THREAD_LOCAL_VALUE __thread
#endif

// log
#ifdef WIN32
	#define FLOG(ftype, format, ...) flog_system::ptr()->write(ftype, __FILE__, __LINE__, __FUNCTION__, format, __VA_ARGS__)
	#define LOG_DEBUG(format, ...) FLOG(FLOGT_DEBUG, format, __VA_ARGS__)
	#define LOG_ERROR(format, ...) FLOG(FLOGT_ERROR, format, __VA_ARGS__)
	#define LOG_SYS(format, ...) FLOG(FLOGT_SYSTEM, format, __VA_ARGS__)
#else
	#define FLOG(ftype, format, ...) flog_system::ptr()->write(ftype, __FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)
	#define LOG_DEBUG(format, ...) flog_system::ptr()->write(FLOGT_DEBUG, __FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)
	#define LOG_ERROR(format, ...) flog_system::ptr()->write(FLOGT_ERROR, __FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)
	#define LOG_SYS(format, ...) flog_system::ptr()->write(FLOGT_SYSTEM, __FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)
#endif

// 编译器TODO
#ifndef WIN32 
#define DO_PRAGMA(x) _Pragma (#x)
#define TODO(x) DO_PRAGMA(message ("TODO - " #x))
#else
#define TODO(x)
#endif

// hook
#ifdef WIN32
#define JMP_CODE_LEN 5
#else
#define JMP_CODE_LEN 12
#endif

// warn
#ifdef WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

// platform
#if defined(WIN32)
#ifdef _M_X64 
#define FPLATFORM64
#endif
#else
#ifdef __x86_64
#define FPLATFORM64
#endif
#endif

/////////////////////////////////////////////////////////////

#include <vector>
#include <map>
#include <list>
#include <set>
#include <fstream>
#include <string>
#include <iostream>
#include <algorithm>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <cassert>
#include <memory>
#include <stdarg.h>
#include <math.h>
#include <functional>
#include <string.h>
#include <time.h>
#include <stddef.h>
#include <sstream>

#ifdef WIN32
#include <windows.h>
#include <unordered_map>
#include <shlwapi.h>
#include <signal.h>
#else
#include <execinfo.h>
#include <signal.h>
#include <exception>
#include <setjmp.h>
#include <pthread.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/limits.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <malloc.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/syscall.h> 
#include <sys/reg.h>
#include <sys/user.h>
#endif

/////////////////////////////////////////////////////////////

// 数学
const double c_pi = 3.1415926535;
const double c_min_float = 1.0E-9;

// 日志
enum FLOG_TYPE
{
	FLOGT_DEBUG,
	FLOGT_INFO,
	FLOGT_ERROR,
	FLOGT_SYSTEM,
	FLOGT_MAX,
};

const int32_t c_LogBuffer = 102400;
#ifdef WIN32
const PRINTF_CLOLOR_TYPE c_log_print_color[FLOGT_MAX] = {
	0 | FOREGROUND_INTENSITY,
	FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
	FOREGROUND_RED | FOREGROUND_INTENSITY,
	FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY
};
#else
static PRINTF_CLOLOR_TYPE c_log_print_color[FLOGT_MAX] = {
	"\e[0m",
	"\e[34m\e[1m",//hight blue
	"\e[31m", //red
	"\e[32m", //green
};
#endif

/////////////////////////////////////////////////////////////

#if defined(WIN32)
typedef DWORD t_id;
#else
typedef pthread_t t_id;
#endif

typedef	int32_t socket_t;
#if defined(WIN32)
typedef int32_t socklen_t;
#else
#endif

#if defined(WIN32)
typedef DWORD tls_key;
#else
typedef pthread_key_t tls_key;
#endif

#if defined(WIN32)
typedef void* shm_handle;
typedef int shm_key;
#else
typedef int shm_handle;
typedef key_t shm_key;
#endif

union marshall_float 
{
	uint32_t i;
	float f;
};

union marshall_poiner
{
	uint32_t i;
	void * p;
};

/////////////////////////////////////////////////////////////

/*
 * Global static constants
 */

static const double ROUNDING_ERROR64   = 0.00000001;   // 1.0e-8
static const float ROUNDING_ERROR     = 0.000001f;    // 1.0e-6

static const float OMEGA              = 999999.f;

static const double PI64   = 3.1415926535897932384626433832795028841971693993751;
static const float PI     = 3.14159265359f;

const float DEG       = PI / 180.0f;
const float RAD       = 180.0f / PI;
const double DEG64     = PI64 / 180.0;
const double RAD64     = 180.0 / PI64;

const double SQRT2     = sqrt(2.0);
const float SQRT2F    = sqrtf(2.0f);

const double STDASPECT = 4.0 / 3.0;

/*-----------------------------------------------------------------------------
	Floating point constants.
-----------------------------------------------------------------------------*/

#define SMALL_NUMBER		(1.e-8f)
#define KINDA_SMALL_NUMBER	(1.e-4f)
#define BIG_NUMBER			(3.4e+38f)
#define EULERS_NUMBER       (2.71828182845904523536f)

// Copied from float.h
#define MAX_FLT 3.402823466e+38F

// Aux constants.
#define INV_PI			(0.31830988618f)
#define HALF_PI			(1.57079632679f)

// Magic numbers for numerical precision.
#define DELTA			(0.00001f)

/**
 * Generic implementation for most platforms
 */

/**
 * Converts a float to an integer with truncation towards zero.
 * @param F		Floating point value to convert
 * @return		Truncated integer.
 */
static  force_inline int32_t Trunc( float F )
{
	return (int32_t)F;
}

/**
 * Converts a float to an integer value with truncation towards zero.
 * @param F		Floating point value to convert
 * @return		Truncated integer value.
 */
static  force_inline float TruncFloat( float F )
{
	return (float)Trunc(F);
}

/**
 * Converts a float to a less or equal integer.
 * @param F		Floating point value to convert
 * @return		An integer less or equal to 'F'.
 */
static force_inline int32_t Floor( float F )
{
	return Trunc(floorf(F));
}

/**
 * Converts a double to a less or equal integer.
 * @param F		Floating point value to convert
 * @return		The nearest integer value to 'F'.
 */
static force_inline double FloorDouble( double F )
{
	return floor(F);
}

/**
 * Converts a float to the nearest integer. Rounds up when the fraction is .5
 * @param F		Floating point value to convert
 * @return		The nearest integer to 'F'.
 */
static force_inline int32_t Round( float F )
{
	return Floor(F+0.5f);
}

/**
 * Converts a float to a greater or equal integer.
 * @param F		Floating point value to convert
 * @return		An integer greater or equal to 'F'.
 */
static force_inline int32_t Ceil( float F )
{
	return Trunc(ceilf(F));
}
/**
 * Returns the fractional part of a float.
 * @param Value	Floating point value to convert
 * @return		A float between >=0 and < 1.
 */
static force_inline float Fractional( float Value ) 
{ 
	return Value - TruncFloat( Value );
}

static force_inline float Exp( float Value ) { return expf(Value); }
static force_inline float Loge( float Value ) {	return logf(Value); }
static force_inline float LogX( float Base, float Value ) { return Loge(Value) / Loge(Base); }
// 1.0 / Loge(2) = 1.4426950f
static force_inline float Log2( float Value ) { return Loge(Value) * 1.4426950f; }	

/** 
* Returns the floating-point remainder of X / Y
* Warning: Always returns remainder toward 0, not toward the smaller multiple of Y.
*			So for example Fmod(2.8f, 2) gives .8f as you would expect, however, Fmod(-2.8f, 2) gives -.8f, NOT 1.2f 
* Use Floor instead when snapping positions that can be negative to a grid
*/
static force_inline float Fmod( float X, float Y ) { return fmodf(X, Y); }
static force_inline float Sin( float Value ) { return sinf(Value); }
static force_inline float Asin( float Value ) { return asinf( (Value<-1.f) ? -1.f : ((Value<1.f) ? Value : 1.f) ); }
static force_inline float Cos( float Value ) { return cosf(Value); }
static force_inline float Acos( float Value ) { return acosf( (Value<-1.f) ? -1.f : ((Value<1.f) ? Value : 1.f) ); }
static force_inline float Tan( float Value ) { return tanf(Value); }
static force_inline float Atan( float Value ) { return atanf(Value); }
static force_inline float Atan2( float Y, float X ) { return atan2f(Y,X); }
static force_inline float Sqrt( float Value ) { return sqrtf(Value); }
static force_inline float Pow( float A, float B ) { return powf(A,B); }

/** Computes a fully accurate inverse square root */
static force_inline float InvSqrt( float F )
{
	return 1.0f / sqrtf( F );
}

/** Computes a faster but less accurate inverse square root */
static force_inline float InvSqrtEst( float F )
{
	return InvSqrt( F );
}

static force_inline bool IsNaN( float A ) 
{
	marshall_float tmp;
	tmp.f = A;
	return (tmp.i & 0x7FFFFFFF) > 0x7F800000;
}
static force_inline bool IsFinite( float A )
{
	marshall_float tmp;
	tmp.f = A;
	return (tmp.i & 0x7F800000) != 0x7F800000;
}
static force_inline bool IsNegativeFloat(const float& F1)
{
	marshall_float tmp;
	tmp.f = F1;
	return ( tmp.i >= (uint32_t)0x80000000 ); // Detects sign bit.
}

/** Returns a random integer between 0 and RAND_MAX, inclusive */
static force_inline int32_t Rand() { return rand(); }

/** Seeds global random number functions Rand() and FRand() */
static force_inline void RandInit(int32_t Seed) { srand( Seed ); }

/** Returns a random float between 0 and 1, inclusive. */
static force_inline float FRand() { return rand() / (float)RAND_MAX; }

/** Seeds future calls to SRand() */
static force_inline void SRandInit(int32_t & g_SRandSeed, int32_t Seed )
{
    g_SRandSeed = Seed; 
}

/** Returns a seeded random float in the range [0,1), using the seed from SRandInit(). */
static force_inline float SRand(int32_t & g_SRandSeed)
{ 
	g_SRandSeed = (g_SRandSeed * 196314165) + 907633515;
	union { float f; int32_t i; } Result;
	union { float f; int32_t i; } Temp;
	const float SRandTemp = 1.0f;
	Temp.f = SRandTemp;
	Result.i = (Temp.i & 0xff800000) | (g_SRandSeed & 0x007fffff);
	return Fractional( Result.f );
} 


/**
 * Computes the base 2 logarithm for an integer value that is greater than 0.
 * The result is rounded down to the nearest integer.
 *
 * @param Value		The value to compute the log of
 * @return			Log2 of Value. 0 if Value is 0.
 */	
static force_inline uint32_t FloorLog2(uint32_t Value) 
{
	uint32_t Bit = 32;
	for (; Bit > 0;)
	{
		Bit--;
		if (Value & (1<<Bit))
		{
			break;
		}
	}
	return Bit;
}

/**
 * Counts the number of leading zeros in the bit representation of the value
 *
 * @param Value the value to determine the number of leading zeros for
 *
 * @return the number of zeros before the first "on" bit
 */
static force_inline uint32_t CountLeadingZeros(uint32_t Value)
{
	if (Value == 0) return 32;
	return 31 - FloorLog2(Value);
}

/**
 * Returns smallest N such that (1<<N)>=Arg.
 * Note: CeilLogTwo(0)=0 because (1<<0)=1 >= 0.
 */
static force_inline uint32_t CeilLogTwo( uint32_t Arg )
{
	int32_t Bitmask = ((int32_t)(CountLeadingZeros(Arg) << 26)) >> 31;
	return (32 - CountLeadingZeros(Arg - 1)) & (~Bitmask);
}

/** @return Rounds the given number up to the next highest power of two. */
static force_inline uint32_t RoundUpToPowerOfTwo(uint32_t Arg)
{
	return 1 << CeilLogTwo(Arg);
}

/** Spreads bits to every other. */
static force_inline uint32_t MortonCode2( uint32_t x )
{
	x &= 0x0000ffff;
	x = (x ^ (x << 8)) & 0x00ff00ff;
	x = (x ^ (x << 4)) & 0x0f0f0f0f;
	x = (x ^ (x << 2)) & 0x33333333;
	x = (x ^ (x << 1)) & 0x55555555;
	return x;
}

/** Reverses MortonCode2. Compacts every other bit to the right. */
static force_inline uint32_t ReverseMortonCode2( uint32_t x )
{
	x &= 0x55555555;
	x = (x ^ (x >> 1)) & 0x33333333;
	x = (x ^ (x >> 2)) & 0x0f0f0f0f;
	x = (x ^ (x >> 4)) & 0x00ff00ff;
	x = (x ^ (x >> 8)) & 0x0000ffff;
	return x;
}

/** Spreads bits to every 3rd. */
static force_inline uint32_t MortonCode3( uint32_t x )
{
	x &= 0x000003ff;
	x = (x ^ (x << 16)) & 0xff0000ff;
	x = (x ^ (x <<  8)) & 0x0300f00f;
	x = (x ^ (x <<  4)) & 0x030c30c3;
	x = (x ^ (x <<  2)) & 0x09249249;
	return x;
}

/** Reverses MortonCode3. Compacts every 3rd bit to the right. */
static force_inline uint32_t ReverseMortonCode3( uint32_t x )
{
	x &= 0x09249249;
	x = (x ^ (x >>  2)) & 0x030c30c3;
	x = (x ^ (x >>  4)) & 0x0300f00f;
	x = (x ^ (x >>  8)) & 0xff0000ff;
	x = (x ^ (x >> 16)) & 0x000003ff;
	return x;
}

/**
 * Returns value based on comparand. The main purpose of this function is to avoid
 * branching based on floating point comparison which can be avoided via compiler
 * intrinsics.
 *
 * Please note that we don't define what happens in the case of NaNs as there might
 * be platform specific differences.
 *
 * @param	Comparand		Comparand the results are based on
 * @param	ValueGEZero		Return value if Comparand >= 0
 * @param	ValueLTZero		Return value if Comparand < 0
 *
 * @return	ValueGEZero if Comparand >= 0, ValueLTZero otherwise
 */
static  force_inline float FloatSelect( float Comparand, float ValueGEZero, float ValueLTZero )
{
	return Comparand >= 0.f ? ValueGEZero : ValueLTZero;
}

/**
 * Returns value based on comparand. The main purpose of this function is to avoid
 * branching based on floating point comparison which can be avoided via compiler
 * intrinsics.
 *
 * Please note that we don't define what happens in the case of NaNs as there might
 * be platform specific differences.
 *
 * @param	Comparand		Comparand the results are based on
 * @param	ValueGEZero		Return value if Comparand >= 0
 * @param	ValueLTZero		Return value if Comparand < 0
 *
 * @return	ValueGEZero if Comparand >= 0, ValueLTZero otherwise
 */
static  force_inline double FloatSelect( double Comparand, double ValueGEZero, double ValueLTZero )
{
	return Comparand >= 0.f ? ValueGEZero : ValueLTZero;
}

/** Computes absolute value in a generic way */
template< class T > 
static  force_inline T Abs( const T A )
{
	return (A>=(T)0) ? A : -A;
}

/** Returns 1, 0, or -1 depending on relation of T to 0 */
template< class T > 
static  force_inline T Sign( const T A )
{
	return (A > (T)0) ? (T)1 : ((A < (T)0) ? (T)-1 : (T)0);
}

/** Returns higher value in a generic way */
template< class T > 
static  force_inline T Max( const T A, const T B )
{
	return (A>=B) ? A : B;
}

/** Returns lower value in a generic way */
template< class T > 
static  force_inline T Min( const T A, const T B )
{
	return (A<=B) ? A : B;
}
    
//! Returns the largest value of the specified variables.
template <typename T> force_inline T Max(const T &A, const T &B, const T &C)
{
    return ( A >= B && A >= C ? A : ( B >= A && B >= C ? B : C ) );
}
//! Returns the smallest value of the specified variables.
template <typename T> force_inline T Min(const T &A, const T &B, const T &C)
{
    return ( A <= B && A <= C ? A : ( B <= A && B <= C ? B : C ) );
}

//! Increase the given value if the potentially new value is greater.
template <typename T> force_inline void Increase(T &Value, const T &PotNewValue)
{
    if (PotNewValue > Value)
        Value = PotNewValue;
}
//! Decrease the given value if the potentially new value is smaller.
template <typename T> force_inline void Decrease(T &Value, const T &PotNewValue)
{
    if (PotNewValue < Value)
        Value = PotNewValue;
}

//! Returns the input value if it is inside the range "Min" and "Max. Otherwise the clamped range.
template <typename T> force_inline T MinMax(const T &Value, const T &Min, const T &Max)
{
    if (Value > Max)
        return Max;
    else if (Value < Min)
        return Min;
    return Value;
}
//! Clamps the variable "Value" to the range "Min" and "Max".
template <typename T> force_inline void Clamp(T &Value, const T &Min, const T &Max)
{
    if (Value > Max)
        Value = Max;
    else if (Value < Min)
        Value = Min;
}

//! Exchanges (or rather swaps) the content of the specified variables A and B.
template <typename T> force_inline void Swap(T &A, T &B)
{
    const T Temp(A);
    A = B;
    B = Temp;
}

//! Returns the signed value. Resulting values can only be 1, -1 or 0.
template <typename T> force_inline T Sgn(const T &Value)
{
    return (Value > 0) ? T(1) : (Value < 0) ? T(-1) : T(0);
}

//! Returns the rounded value to the specified precision.
template <typename T> force_inline T Round(const T &Value, int32_t Precision)
{
    int32_t exp = static_cast<int32_t>(pow(10, Precision));
    return static_cast<T>(static_cast<int32_t>(Value*exp)) / exp;
}

//! Returns the square of the specified value (Value * Value).
template <typename T> force_inline T Pow2(const T &Value)
{
    return Value*Value;
}

//! Returns the sine of the specified value as degree.
template <typename T> force_inline T Sin(const T &Value)
{
    return static_cast<T>(sin(DEG64*Value));
}
//! Returns the cosine of the specified value as degree.
template <typename T> force_inline T Cos(const T &Value)
{
    return static_cast<T>(cos(DEG64*Value));
}
//! Returns the tangent of the specified value as degree.
template <typename T> force_inline T Tan(const T &Value)
{
    return static_cast<T>(tan(DEG64*Value));
}

//! Returns the arcus sine of the specified value as degree.
template <typename T> force_inline T ASin(const T &Value)
{
    return static_cast<T>(asin(Value)*RAD64);
}
//! Returns arcus cosine of the specified value as degree.
template <typename T> force_inline T ACos(const T &Value)
{
    return static_cast<T>(acos(Value)*RAD64);
}
//! Returns arcus tangent of the specified value as degree.
template <typename T> force_inline T ATan(const T &Value)
{
    return static_cast<T>(atan(Value)*RAD64);
}

//! Returns the logarithm with the specified base.
template <typename T> force_inline T Log(const T &Value, const T &Base = T(10))
{
    return log(Value) / log(Base);
}

/**
Returns a linear-interpolation ('lerp') between the two given points ('From' and 'To').
\param[out] Result Specifies the resulting output point.
\param[in] From Specifies the start point.
\param[in] To Specifies the end point.
\param[in] Factor Specifies the interpolation factor. This should be in the range [0.0 .. 1.0].
\tparam T Specifies the type of the points (e.g. float, dim::point2df, dim::vector3df etc.).
\tparam I Specifies the interpolation data type. This should be a float or a double.
\see dim::point2d
\see dim::vector3d
*/
template <typename T, typename I> force_inline void Lerp(T &Result, const T &From, const T &To, const I &Factor)
{
    Result = To;
    Result -= From;
    Result *= Factor;
    Result += From;
}

//! Overloaded function. For more information read the documentation of the first variant of this function.
template <typename T, typename I> force_inline T Lerp(const T &From, const T &To, const I &Factor)
{
    T Result;
    Lerp<T, I>(Result, From, To, Factor);
    return Result;
}

//! Parabolic interpolation. This is equivalent to "Lerp(From, To, Factor*Factor)".
template <typename T, typename I> force_inline T LerpParabolic(const T &From, const T &To, const I &Factor)
{
    return Lerp(From, To, Factor*Factor);
}

//! Sine interpolation. This is equivalent to "Lerp(From, To, Sin(Factor*90))".
template <typename T, typename I> force_inline T LerpSin(const T &From, const T &To, const I &Factor)
{
    return Lerp(From, To, Sin(Factor*I(90)));
}

//! Rounds the given value to the nearest power of two value (e.g. 34 to 32 and 120 to 128).
force_inline int32_t RoundPow2(int32_t Value)
{
    int32_t i;
    
    for (i = 1; i < Value; i <<= 1);
    
    if (i - Value <= Value - i/2)
        return i;
    
    return i/2;
}

//! Returns true if A and B are equal with the specified tolerance.
force_inline bool Equal(float A, float B, float Tolerance = ROUNDING_ERROR)
{
    return (A + Tolerance >= B) && (A - Tolerance <= B);
}
/**
Returns true if A and B are equal. The tolerance factor is only used to have the
same interface like the floating-point version of this function.
*/
force_inline bool Equal(int32_t A, int32_t B, int32_t Tolerance = 0)
{
    return A == B;
}

//! Returns the bit inside the specified integer at the specified position (right to left).
force_inline bool getBitR2L(uint32_t Integer, int32_t Pos)
{
    return ((Integer >> Pos) & 0x00000001) != 0;
}
//! Sets the bit inside the specified integer at the specified position (right to left).
force_inline void setBitR2L(uint32_t &Integer, int32_t Pos, bool Enable)
{
    if (Enable)
        Integer |= (0x00000001 << Pos);
    else
        Integer &= ((0xFFFFFFFE << Pos) + (0x7FFFFFFF << (Pos - 31)));
}

//! Returns the bit inside the specified integer at the specified position (left to right).
force_inline bool getBitL2R(uint32_t Integer, int32_t Pos)
{
    return ((Integer << Pos) & 0x80000000) != 0;
}
//! Sets the bit inside the specified integer at the specified position (left to right).
force_inline void setBitL2R(uint32_t &Integer, int32_t Pos, bool Enable)
{
    if (Enable)
        Integer |= (0x80000000 >> Pos);
    else
        Integer &= ((0x7FFFFFFF >> Pos) + (0xFFFFFFFE >> (Pos - 31)));
}

//! Returns the bit inside the specified integer at the specified position (right to left).
force_inline bool getBitR2L(uint8_t Integer, int32_t Pos)
{
    return ((Integer >> Pos) & 0x01) != 0;
}
//! Sets the bit inside the specified integer at the specified position (right to left).
force_inline void setBitR2L(uint8_t &Integer, int32_t Pos, bool Enable)
{
    if (Enable)
        Integer |= (0x01 << Pos);
    else
        Integer &= ((0xFE << Pos) + (0x7F << (Pos - 31)));
}

//! Returns the bit inside the specified integer at the specified position (left to right).
force_inline bool getBitL2R(uint8_t Integer, int32_t Pos)
{
    return ((Integer << Pos) & 0x80) != 0;
}
//! Sets the bit inside the specified integer at the specified position (left to right).
force_inline void setBitL2R(uint8_t &Integer, int32_t Pos, bool Enable)
{
    if (Enable)
        Integer |= (0x80 >> Pos);
    else
        Integer &= ((0x7F >> Pos) + (0xFE >> (Pos - 31)));
}

/**
Returns the 2D triangle area.
\note This is actually only used inside the dim::triangle3d::getBarycentricCoord function.
*/
template <typename T> force_inline T getTriangleArea2D(
    const T &x1, const T &y1, const T &x2, const T &y2, const T &x3, const T &y3)
{
    return (x1 - x2)*(y2 - y3) - (x2 - x3)*(y1 - y2);
}

//! Returns a bezier value used for bezier curves.
template <typename T> force_inline T getBezierValue(const float t, const T &Pos1, const T &Pos2, const T &Radial1, const T &Radial2)
{
    const float invt    = 1.0f - t;
    const float invt2   = invt*invt;
    const float invt3   = invt2*invt;
    const float t2      = t*t;
    const float t3      = t*t*t;
    
    return Pos1*invt3 + Radial1*3*t*invt2 + Radial2*3*t2*invt + Pos2*t3;
}

//! Returns a bernstein value used for bezier patch generation.
template <typename T> force_inline T getBernsteinValue(const float t, const T Points[4])
{
    const float invt = 1.0f - t;
    
    return
        Points[0] * pow(t, 3) +
        Points[1] * (T(3) * pow(t, 2) * invt) +
        Points[2] * (T(3) * pow(invt, 2) * t) +
        Points[3] * pow(invt, 3);
}

//! Returns a gaussian value used for gaussian blur.
template <typename T> force_inline T getGaussianValue(const T &X, const T &Mean, const T &StdDeviation)
{
    return (
        ( T(1) / sqrt( T(2) * static_cast<T>(PI) * StdDeviation * StdDeviation ) )
        * exp( ( -( ( X - Mean ) * ( X - Mean ) ) ) / ( T(2) * StdDeviation * StdDeviation ) )
    );
}

/**
Returns the halton sequence for the given input and base. This is used for quasi-random values.
For more details take a look at: http://orion.math.iastate.edu/reu/2001/voronoi/halton_sequence.html.
\param[in] Index Specifies the sequence index.
\param[in] Base Specifies the sequence base. This has to be a prime number! Usually beginning with 2, 3, etc.
\todo This is currently not used and has not been tested!
*/
template <typename T> force_inline T getHaltonSequence(int32_t Index, int32_t Base)
{
    T Result = T(0);
    
    T f = T(1) / Base;
    
    while (Index > 0)
    {
        Result += f * (Index % Base);
        Index /= Base;
        f /= Base;
    }
    
    return Result;
}

/**
Computes the efficient modular pow value.
\tparam T This type must support the following operators: %, %=, *= and >>=.
\param[in] Base Specifies the base value.
\param[in] Exp Specifies the exponent value.
\param[in] Modulus Specifies the modulus value.
\return 'Base' power of 'Exp' modulo 'Modulus'. This is equivalent to '(Base ^ Exp) % Modulus' but faster.
*/
template <typename T> force_inline T ModularPow(T Base, T Exp, const T &Modulus)
{
    T Result = T(1);
    
    while (Exp > T(0))
    {
        if (Exp % 2 == 1)
        {
            Result *= Base;
            Result %= Modulus;
        }
        Exp >>= 1;
        Base *= Base;
        Base %= Modulus;
    }
    
    return Result;
}

/** Helper function for rand implementations. Returns a random number in [0..A) */
static force_inline int32_t RandHelper(int32_t A)
{
	// RAND_MAX+1 give interval [0..A) with even distribution.
	return A>0 ? Trunc(Rand()/(float)((uint32_t)RAND_MAX+1) * A) : 0;
}

/** Helper function for rand implementations. Returns a random number >= Min and <= Max */
static force_inline int32_t RandRange(int32_t Min, int32_t Max)
{
	const int32_t Range = (Max - Min) + 1;
	return Min + RandHelper(Range);
}

/** Util to generate a random number in a range. */
static force_inline float FRandRange(float InMin, float InMax)
{
	return InMin + (InMax - InMin) * FRand();
}

/** Checks if value is within a range, exclusive on MaxValue) */
template< class U > 
static force_inline bool IsWithin(const U& TestValue, const U& MinValue, const U& MaxValue)
{
	return ((TestValue>=MinValue) && (TestValue < MaxValue));
}

/** Checks if value is within a range, inclusive on MaxValue) */
template< class U > 
static force_inline bool IsWithinInclusive(const U& TestValue, const U& MinValue, const U& MaxValue)
{
	return ((TestValue>=MinValue) && (TestValue <= MaxValue));
}

/**
 *	Checks whether a number is a power of two.
 *	@param Value	Number to check
 *	@return			true if Value is a power of two
 */
static force_inline bool IsPowerOfTwo( uint32_t Value )
{
	return ((Value & (Value - 1)) == 0);
}

/**
 *	Checks if two floating point numbers are nearly equal.
 *	@param A				First number to compare
 *	@param B				Second number to compare
 *	@param ErrorTolerance	Maximum allowed difference for considering them as 'nearly equal'
 *	@return					true if A and B are nearly equal
 */
static force_inline bool IsNearlyEqual(float A, float B, float ErrorTolerance = SMALL_NUMBER)
{
	return Abs<float>( A - B ) < ErrorTolerance;
}

/**
 *	Checks if two floating point numbers are nearly equal.
 *	@param A				First number to compare
 *	@param B				Second number to compare
 *	@param ErrorTolerance	Maximum allowed difference for considering them as 'nearly equal'
 *	@return					true if A and B are nearly equal
 */
static force_inline bool IsNearlyEqual(double A, double B, double ErrorTolerance = SMALL_NUMBER)
{
	return Abs<double>( A - B ) < ErrorTolerance;
}

/**
 *	Checks if a floating point number is nearly zero.
 *	@param Value			Number to compare
 *	@param ErrorTolerance	Maximum allowed difference for considering Value as 'nearly zero'
 *	@return					true if Value is nearly zero
 */
static force_inline bool IsNearlyZero(float Value, float ErrorTolerance = SMALL_NUMBER)
{
	return Abs<float>( Value ) < ErrorTolerance;
}

/**
 *	Checks if a floating point number is nearly zero.
 *	@param Value			Number to compare
 *	@param ErrorTolerance	Maximum allowed difference for considering Value as 'nearly zero'
 *	@return					true if Value is nearly zero
 */
static force_inline bool IsNearlyZero(double Value, double ErrorTolerance = SMALL_NUMBER)
{
	return Abs<double>( Value ) < ErrorTolerance;
}

/** Snaps a value to the nearest grid multiple */
static force_inline float GridSnap( float Location, float Grid )
{
	if( Grid==0.f )	return Location;
	else			
	{
		return (float)Floor((float)(Location + 0.5*Grid) / Grid)*Grid;
	}
}

/** Snaps a value to the nearest grid multiple */
static force_inline double GridSnap( double Location, double Grid )
{
	if( Grid==0.0 )	return Location;
	else			
	{
		return FloorDouble((Location + 0.5*Grid)/Grid)*Grid;
	}
}

/** Divides two integers and rounds up */
template <class T>
static force_inline T DivideAndRoundUp(T Dividend,T Divisor)
{
	return (Dividend + Divisor - 1) / Divisor;
}

/** 
 * Converts radians to degrees.
 * @param	RadVal			Value in radians.
 * @return					Value in degrees.
 */
template<class T>
static force_inline T RadiansToDegrees(T const& RadVal)
{
	return RadVal * (180.f / PI);
}

/** 
 * Converts degrees to radians.
 * @param	DegVal			Value in degrees.
 * @return					Value in radians.
 */
template<class T>
static force_inline T DegreesToRadians(T const& DegVal)
{
	return DegVal * (PI / 180.f);
}

/** Find the smallest angle between two headings (in radians) */
static force_inline float FindDeltaAngle(float A1, float A2)
{
	// Find the difference
	float Delta = A2 - A1;

	// If change is larger than PI
	if(Delta > PI)
	{
		// Flip to negative equivalent
		Delta = Delta - (PI * 2.0f);
	}
	else if(Delta < -PI)
	{
		// Otherwise, if change is smaller than -PI
		// Flip to positive equivalent
		Delta = Delta + (PI * 2.0f);
	}

	// Return delta in [-PI,PI] range
	return Delta;
}

/** Given a heading which may be outside the +/- PI range, 'unwind' it back into that range. */
static force_inline float UnwindRadians(float A)
{
	while(A > PI)
	{
		A -= ((float)PI * 2.0f);
	}

	while(A < -PI)
	{
		A += ((float)PI * 2.0f);
	}

	return A;
}

/** Utility to ensure angle is between +/- 180 degrees by unwinding. */
static force_inline float UnwindDegrees(float A)
{
	while(A > 180.f)
	{
		A -= 360.f;
	}

	while(A < -180.f)
	{
		A += 360.f;
	}

	return A;
}

/** Performs a linear interpolation between two values, Alpha ranges from 0-1. Handles full numeric range of T */
template< class T > 
static force_inline T LerpStable( const T& A, const T& B, double Alpha )
{
	return (T)((A * (1.0 - Alpha)) + (B * Alpha));
}

/** Performs a 2D linear interpolation between four values values, FracX, FracY ranges from 0-1 */
template< class T, class U > 
static force_inline T BiLerp(const T& P00,const T& P10,const T& P01,const T& P11, const U& FracX, const U& FracY)
{
	return Lerp(
		Lerp(P00,P10,FracX),
		Lerp(P01,P11,FracX),
		FracY
		);
}

/**
 * Performs a cubic interpolation
 *
 * @param  P - end points
 * @param  T - tangent directions at end points
 * @param  Alpha - distance along spline
 *
 * @return  Interpolated value
 */
template< class T, class U > 
static force_inline T CubicInterp( const T& P0, const T& T0, const T& P1, const T& T1, const U& A )
{
	const float A2 = A  * A;
	const float A3 = A2 * A;

	return (T)(((2*A3)-(3*A2)+1) * P0) + ((A3-(2*A2)+A) * T0) + ((A3-A2) * T1) + (((-2*A3)+(3*A2)) * P1);
}

/**
 * Performs a first derivative cubic interpolation
 *
 * @param  P - end points
 * @param  T - tangent directions at end points
 * @param  Alpha - distance along spline
 *
 * @return  Interpolated value
 */
template< class T, class U > 
static force_inline T CubicInterpDerivative( const T& P0, const T& T0, const T& P1, const T& T1, const U& A )
{
	T a = 6.f*P0 + 3.f*T0 + 3.f*T1 - 6.f*P1;
	T b = -6.f*P0 - 4.f*T0 - 2.f*T1 + 6.f*P1;
	T c = T0;

	const float A2 = A  * A;

	return (a * A2) + (b * A) + c;
}

/**
 * Performs a second derivative cubic interpolation
 *
 * @param  P - end points
 * @param  T - tangent directions at end points
 * @param  Alpha - distance along spline
 *
 * @return  Interpolated value
 */
template< class T, class U > 
static force_inline T CubicInterpSecondDerivative( const T& P0, const T& T0, const T& P1, const T& T1, const U& A )
{
	T a = 12.f*P0 + 6.f*T0 + 6.f*T1 - 12.f*P1;
	T b = -6.f*P0 - 4.f*T0 - 2.f*T1 + 6.f*P1;

	return (a * A) + b;
}

/** Interpolate between A and B, applying an ease in/out function.  Exp controls the degree of the curve. */
template< class T > 
static force_inline T InterpEaseInOut( const T& A, const T& B, float Alpha, float Exp )
{
	float const ModifiedAlpha = ( Alpha < 0.5f ) ?
		0.5f * Pow(2.f * Alpha, Exp) :
	1.f - 0.5f * Pow(2.f * (1.f - Alpha), Exp);

	return Lerp<T>(A, B, ModifiedAlpha);
}

/**
 * Simple function to create a pulsating scalar value
 *
 * @param  InCurrentTime  Current absolute time
 * @param  InPulsesPerSecond  How many full pulses per second?
 * @param  InPhase  Optional phase amount, between 0.0 and 1.0 (to synchronize pulses)
 *
 * @return  Pulsating value (0.0-1.0)
 */
static force_inline float MakePulsatingValue( const double InCurrentTime, const float InPulsesPerSecond, const float InPhase = 0.0f )
{
	return 0.5f + 0.5f * (float)sin( ( ( 0.25f + InPhase ) * PI * 2.0 ) + ( InCurrentTime * PI * 2.0 ) * InPulsesPerSecond );
}

/** 
 * Returns a smooth Hermite interpolation between 0 and 1 for the value X (where X ranges between A and B)
 * Clamped to 0 for X <= A and 1 for X >= B.
 *
 * @param A Minimum value of X
 * @param B Maximum value of X
 * @param X Parameter
 *
 * @return Smoothed value between 0 and 1
 */
static force_inline float SmoothStep(float A, float B, float X)
{
	if (X < A)
	{
		return 0.0f;
	}
	else if (X >= B)
	{
		return 1.0f;
	}
	const float InterpFraction = (X - A) / (B - A);
	return InterpFraction * InterpFraction * (3.0f - 2.0f * InterpFraction);
}

/**
 * Get a bit in memory created from bitflags (uint32_t Value:1), used for EngineShowFlags,
 * TestBitFieldFunctions() tests the implementation
 */
static force_inline bool ExtractBoolFromBitfield(uint8_t* Ptr, uint32_t Index)
{
	uint8_t* BytePtr = Ptr + Index / 8;
	uint8_t Mask = 1 << (Index & 0x7);

	return (*BytePtr & Mask) != 0;
}

/**
 * Set a bit in memory created from bitflags (uint32_t Value:1), used for EngineShowFlags,
 * TestBitFieldFunctions() tests the implementation
 */
static force_inline void SetBoolInBitField(uint8_t* Ptr, uint32_t Index, bool bSet)
{
	uint8_t* BytePtr = Ptr + Index / 8;
	uint8_t Mask = 1 << (Index & 0x7);

	if(bSet)
	{
		*BytePtr |= Mask;
	}
	else
	{
		*BytePtr &= ~Mask;
	}
}

//////////////////////////// 以上ue4 ////////////////////////////

template <typename T>
static force_inline int32_t fstricmp(const T * dst, const T * src)
{
	return tstricmp((const char *)dst, (const char *)src);
}

template <typename T>
static force_inline int32_t fvsnprintf(T * str, size_t size, const char * format, va_list ap)
{
	SAFE_TEST_RET_VAL(str, 0, 0);
	SAFE_TEST_RET_VAL(size, 0, 0);
	SAFE_TEST_RET_VAL((int32_t)size > 0, false, 0);
	int32_t ret = tvsnprintf((char *)str, size - 1, format, ap);
	str[size - 1] = 0;
	return ret;
}

template <typename T>
static int32_t fsnprintf(T * str, size_t size, const char *format, ...)
{
	SAFE_TEST_RET_VAL(str, 0, 0);
	SAFE_TEST_RET_VAL(size, 0, 0);
	SAFE_TEST_RET_VAL((int32_t)size > 0, false, 0);
	va_list ap;
	va_start(ap, format);
	int32_t ret = fvsnprintf((char*)str, size, format, ap);
	va_end(ap);
	return ret;
}

static force_inline void fstrcopy(char * des, const char * src, size_t dest_size)
{
	if (dest_size > 0)
	{
		strncpy((char*)des, (char*)src, dest_size - 1);
		des[dest_size - 1] = 0;
	}
}

static force_inline void fstrcopy(int8_t * des, const int8_t * src, size_t dest_size)
{
	if (dest_size > 0)
	{
		strncpy((char*)des, (char*)src, dest_size - 1);
		des[dest_size - 1] = 0;
	}
}

template <typename T, size_t N>  
force_inline void fstrcopy(T (&des)[N], const T * src)  
{ 
	fstrcopy((int8_t *)des, (const int8_t *)src, N);
}

template <typename T, size_t N>
force_inline void fstrcopy(T(&des)[N], const std::string & src)
{
	fstrcopy((int8_t *)des, (const int8_t *)src.c_str(), N);
}

//! Returns a character converted to lower case
static force_inline uint32_t locale_lower ( uint32_t x )
{
	// ansi
	return x >= 'A' && x <= 'Z' ? x + 0x20 : x;
}

//! Returns a character converted to upper case
static force_inline uint32_t locale_upper ( uint32_t x )
{
	// ansi
	return x >= 'a' && x <= 'z' ? x + ( 'A' - 'a' ) : x;
}

/////////////////////////////////////////////////////////////

template <typename T>
struct fcmp
{
    bool operator()(const T & t1, const T & t2) const
    {
    	return t1 == t2;
    }
};

template <typename T>
force_inline size_t fhash_value(const T & t)
{
	return size_t(t);
}

template <>
force_inline size_t fhash_value(const std::string & t)
{
    const char * buffer = t.c_str();
    size_t length = t.size();
	size_t result = 0;
	for (; length > 0; --length)
	{
        result = (result * 131) + *buffer++;
	}
	return result;    	
}

template <typename T>
struct fhash
{
    size_t operator()(const T & t) const
    {
    	return fhash_value(t);
    }
};

template<uint32_t Cur, uint32_t Next>
class fhashersize
{
public:
    static const uint32_t SIZE = 0;
};

template<uint32_t Cur>
class fhashersize<4294967291ul, Cur>
{
public:
    static const uint32_t SIZE = Cur < 4294967291ul ? 4294967291ul : Cur;
};

#define DEF_HASHER_SIZE(PRE, NEXT) \
template<uint32_t Cur>\
class fhashersize<PRE, Cur>\
{\
public:\
    static const uint32_t SIZE = Cur < PRE ? PRE : fhashersize<NEXT, Cur>::SIZE;\
};

DEF_HASHER_SIZE(3221225473ul, 4294967291ul)
DEF_HASHER_SIZE(1610612741ul, 3221225473ul)
DEF_HASHER_SIZE(805306457ul, 1610612741ul)
DEF_HASHER_SIZE(402653189ul, 805306457ul)
DEF_HASHER_SIZE(201326611ul, 402653189ul)
DEF_HASHER_SIZE(100663319ul, 201326611ul)
DEF_HASHER_SIZE(50331653ul, 100663319ul)
DEF_HASHER_SIZE(25165843ul, 50331653ul)
DEF_HASHER_SIZE(12582917ul, 25165843ul)
DEF_HASHER_SIZE(6291469ul, 12582917ul)
DEF_HASHER_SIZE(3145739ul, 6291469ul)
DEF_HASHER_SIZE(1572869ul, 3145739ul)
DEF_HASHER_SIZE(786433ul, 1572869ul)
DEF_HASHER_SIZE(393241ul, 786433ul)
DEF_HASHER_SIZE(196613ul, 393241ul)
DEF_HASHER_SIZE(98317ul, 196613ul)
DEF_HASHER_SIZE(49157ul, 98317ul)
DEF_HASHER_SIZE(24593ul, 49157ul)
DEF_HASHER_SIZE(12289ul, 24593ul)
DEF_HASHER_SIZE(6151ul, 12289ul)
DEF_HASHER_SIZE(3079ul, 6151ul)
DEF_HASHER_SIZE(1543ul, 3079ul)
DEF_HASHER_SIZE(769ul, 1543ul)
DEF_HASHER_SIZE(193ul, 769ul)
DEF_HASHER_SIZE(97ul, 193ul)
DEF_HASHER_SIZE(53ul, 97ul)
DEF_HASHER_SIZE(29ul, 53ul)
DEF_HASHER_SIZE(13ul, 29ul)
DEF_HASHER_SIZE(3ul, 13ul)

/////////////////////////////////////////////////////////////

template<typename T1, class T2>
class fpair
{
public:
    typedef T1 first_type;
    typedef T2 second_type;

    force_inline fpair() : first(T1()), second(T2())
	{	// construct from defaults
	}

    force_inline fpair(const T1 & _Val1, const T2 & _Val2) : first(_Val1), second(_Val2)
	{	// construct from specified values
	}

    template<class _Other1, class _Other2>
	force_inline fpair(fpair<_Other1, _Other2> & _Right) : first(_Right.first), second(_Right.second)
	{	// construct from compatible fpair
	}

    template<class _Other1, class _Other2>
	force_inline fpair(const fpair<_Other1, _Other2> & _Right) : first(_Right.first), second(_Right.second)
	{	// construct from compatible fpair
	}

    force_inline fpair<T1, T2> & operator=(const fpair<T1, T2> & _Right)
	{	// assign from copied fpair
	    this->first = _Right.first;
	    this->second = _Right.second;
	    return (*this);
	}

public:
	first_type first;
	second_type second;
};

// fpair TEMPLATE FUNCTIONS

template<class _Ty1, class _Ty2> 
force_inline bool operator==(const fpair<_Ty1, _Ty2>& _Left, const fpair<_Ty1, _Ty2>& _Right)
{	// test for fpair equality
    return (_Left.first == _Right.first && _Left.second == _Right.second);
}

template<class _Ty1, class _Ty2>
force_inline bool operator!=(const fpair<_Ty1, _Ty2>& _Left, const fpair<_Ty1, _Ty2>& _Right)
{	// test for fpair inequality
    return (!(_Left == _Right));
}

template<class _Ty1, class _Ty2>
force_inline bool operator<(const fpair<_Ty1, _Ty2>& _Left, const fpair<_Ty1, _Ty2>& _Right)
{	// test if _Left < _Right for pairs
    return (_Left.first < _Right.first || !(_Right.first < _Left.first) && _Left.second < _Right.second);
}

template<class _Ty1, class _Ty2> 
force_inline bool operator>(const fpair<_Ty1, _Ty2>& _Left, const fpair<_Ty1, _Ty2>& _Right)
{	// test if _Left > _Right for pairs
    return (_Right < _Left);
}

template<class _Ty1, class _Ty2> 
force_inline bool operator<=(const fpair<_Ty1, _Ty2>& _Left, const fpair<_Ty1, _Ty2>& _Right)
{	// test if _Left <= _Right for pairs
    return (!(_Right < _Left));
}

template<class _Ty1, class _Ty2> 
force_inline bool operator>=(const fpair<_Ty1, _Ty2>& _Left, const fpair<_Ty1, _Ty2>& _Right)
{	// test if _Left >= _Right for pairs
    return (!(_Left < _Right));
}

/////////////////////////////////////////////////////////////

template <typename Container>
class fiterator
{
public:
	typedef typename Container::Value T;
	force_inline fiterator(Container * _Container) : m_index(0), m_Container(_Container)
	{

	}
	force_inline fiterator(Container * _Container, int32_t _index) : m_index(_index), m_Container(_Container)
	{

	}
	force_inline ~fiterator()
	{

	}

	force_inline T & operator*() const
	{	
		// return designated value
		return m_Container->getbyidx(m_index);
	}

	force_inline T * operator->() const
	{	
		// return pointer to class object
		return &(m_Container->getbyidx(m_index));
	}

	force_inline fiterator & operator++()
	{	
		// preincrement
		m_index = m_Container->getnextidx(m_index);
		return (*this);
	}

	force_inline fiterator operator++(int)
	{	
		// postincrement
		fiterator _Tmp = *this;
		++*this;
		return (_Tmp);
	}

	force_inline fiterator & operator+=(int len)
	{
		for (int i = 0; i < len; i++)
		{
			m_index = m_Container->getnextidx(m_index);
		}
		return (*this);
	}

	force_inline fiterator & operator--()
	{	
		// predecrement
		m_index = m_Container->getpreidx(m_index);
		return (*this);
	}

	force_inline fiterator operator--(int)
	{	
		// postdecrement
		fiterator _Tmp = *this;
		--*this;
		return (_Tmp);
	}

	force_inline fiterator & operator-=(int len)
	{
		for (int i = 0; i < len; i++)
		{
			m_index = m_Container->getpreidx(m_index);
		}
		return (*this);
	}

	force_inline bool operator==(const fiterator & _Right) const
	{	
		// test for iterator equality
		return m_index == _Right.m_index;
	}

	force_inline bool operator!=(const fiterator & _Right) const
	{	
		// test for iterator inequality
		return (!(*this == _Right));
	}

	force_inline fiterator & operator=(const fiterator & _Right)
	{	
		m_index = _Right.m_index;
		m_Container = _Right.m_Container;
		return (*this);
	}

	force_inline int32_t index() const
	{
		return m_index;
	}

private:
	int32_t m_index;
	Container * m_Container;
};

/////////////////////////////////////////////////////////////

//! Sinks an element into the heap.
template<class T>
force_inline void heapsink(T*array, int32_t element, int32_t max)
{
	while ((element<<1) < max) // there is a left child
	{
		int32_t j = (element<<1);

		if (j+1 < max && array[j] < array[j+1])
			j = j+1; // take right child

		if (array[element] < array[j])
		{
			T t = array[j]; // swap elements
			array[j] = array[element];
			array[element] = t;
			element = j;
		}
		else
			return;
	}
}


//! Sorts an array with size 'size' using heapsort.
template<class T>
force_inline void heapsort(T* array_, int32_t size)
{
	// for heapsink we pretent this is not c++, where
	// arrays start with index 0. So we decrease the array pointer,
	// the maximum always +2 and the element always +1

	T* virtualArray = array_ - 1;
	int32_t virtualSize = size + 2;
	int32_t i;

	// build heap

	for (i=((size-1)/2); i>=0; --i)
		heapsink(virtualArray, i+1, virtualSize-1);

	// sort array, leave out the last element (0)
	for (i=size-1; i>0; --i)
	{
		T t = array_[0];
		array_[0] = array_[i];
		array_[i] = t;
		heapsink(virtualArray, 1, i + 1);
	}
}

/////////////////////////////////////////////////////////////

template <typename T, uint32_t N>
class fstack
{
public:
	typedef fstack<T, N> MyType;
	typedef T Value;
	typedef fiterator<MyType> iterator;
	friend class fiterator<MyType>;
public:
	force_inline fstack() : m_tmpdata(T())
	{
		clear();
	}

	force_inline ~fstack()
	{

	}

	force_inline void clear()
	{
		m_used = 0;
#ifdef _DEBUG
		memset(m_data, 0xFF, sizeof(m_data));
#endif
	}

	force_inline bool pop(T & t)
	{
		if (empty())
		{
			return false;
		}
		t = m_data[m_used - 1];
		m_used--;
		return true;
	}

	force_inline bool push(const T & t)
	{
		if (full())
		{
			return false;
		}

		m_data[m_used] = t;
		m_used++;

		return true;
	}

	force_inline uint32_t size() const
	{
		return m_used;
	}

	force_inline bool empty() const
	{
		return m_used == 0;
	}

	force_inline bool full() const
	{
		return m_used == N;
	}
	
	force_inline T& operator [](uint32_t index)
	{
		if (index>=N)
		{
			FASSERT(index>=N);
			return m_tmpdata;
		}

		return m_data[index];
	}

	force_inline const T& operator [](uint32_t index) const
	{
		if (index>=N)
		{
			FASSERT(index>=N);
			return m_tmpdata;
		}

		return m_data[index];
	}

    force_inline iterator begin()
    {
        return iterator(this, 0);
    }

    force_inline iterator end()
    {
        return iterator(this, m_used);
    }

private:
	force_inline T& getbyidx(uint32_t index)
	{
		if (index>=N)
		{
			FASSERT(index>=N);
			return m_tmpdata;
		}

		return m_data[index];
	}

	force_inline const T& getbyidx(uint32_t index) const
	{
		if (index>=N)
		{
			FASSERT(index>=N);
			return m_tmpdata;
		}

		return m_data[index];
	}

	force_inline int32_t getnextidx(int32_t idx)
	{
		if (idx >= 0 && idx + 1 < (int32_t)m_used)
		{
			return idx + 1;
		}
		return m_used;
	}

	force_inline int32_t getpreidx(int32_t idx)
	{
		if (idx - 1 >= 0 && idx < (int32_t)m_used)
		{
			return idx - 1;
		}
		return m_used;
	}

private:
	T m_data[N];
	uint32_t m_used;
	T m_tmpdata;
};

/////////////////////////////////////////////////////////////

template <class T, uint32_t N>
class fpool
{
public:
	static const int32_t INVALID_IDX = -1;
	typedef fpool<T, N> MyType;
	typedef T Value;
	typedef fiterator<MyType> iterator;
	friend class fiterator<MyType>;
public:
	force_inline fpool() : m_tmpdata(T())
	{
		clear();
	}
	force_inline ~fpool()
	{

	}

	force_inline void clear()
	{
		m_free.clear();
		for (int32_t i = 0; i < (int32_t)N; i++)
		{
			m_free.push(i);
			m_data[i].preindex = INVALID_IDX;
			m_data[i].nextindex= INVALID_IDX;
			m_data[i].isdirty = true;
#ifdef _DEBUG
			memset(&m_data[i].data, 0xFF, sizeof(m_data[i].data));
#endif
		}
		m_used = INVALID_IDX;
	}

	force_inline int32_t allocindex()
	{
		if (m_free.empty())
		{
			return INVALID_IDX;
		}

		uint32_t index;
		if (!m_free.pop(index))
		{
			return INVALID_IDX;
		}

		m_data[index].isdirty = false;
		if (m_used != INVALID_IDX)
		{
			m_data[m_used].preindex = index;
		}

		m_data[index].nextindex = m_used;
		m_data[index].preindex = INVALID_IDX;
		m_used = index;

		return index;
	}

	force_inline T * alloc()
	{
		int32_t index = allocindex();
		if (index == INVALID_IDX)
		{
			return NULL;
		}

		return &m_data[index].data;
	}

	force_inline bool deallocindex(int32_t index)
	{
		SAFE_TEST_INDEX_VAL(index, N, false);
		if (m_data[index].isdirty)
		{
			return false;
		}
		m_data[index].isdirty = true;
		m_free.push(index);

		int32_t nextidx = m_data[index].nextindex;
		int32_t preindex = m_data[index].preindex;

		m_data[index].nextindex = INVALID_IDX;
		m_data[index].preindex = INVALID_IDX;

		if (nextidx != INVALID_IDX)
		{
			m_data[nextidx].preindex = preindex;
		}

		if (preindex != INVALID_IDX)
		{
			m_data[preindex].nextindex = nextidx;
		}

		if (index == m_used)
		{
			m_used = nextidx;
		}

		return true;
	}

	force_inline bool dealloc(T & t)
	{
		const size_t offset = (size_t)&m_data[0].data - (size_t)&m_data[0];
		int32_t index = ((size_t)&t - offset - ((size_t)m_data))/sizeof(m_data[0]);
		return deallocindex(index);
	}

	force_inline T& operator [](uint32_t index)
	{
		if (index>=N)
		{
			FASSERT(index>=N);
			return m_tmpdata;
		}

		return m_data[index].data;
	}

	force_inline const T& operator [](uint32_t index) const
	{
		if (index>=N)
		{
			FASSERT(index>=N);
			return m_tmpdata;
		}

		return m_data[index].data;
	}

	// 使用的大小
	force_inline uint32_t size() const
	{
		return N - m_free.size();
	}

	force_inline bool empty() const
	{
		return size() == 0;
	}

	force_inline bool full() const
	{
		return size() == N;
	}

    force_inline iterator begin()
    {
        return iterator(this, m_used);
    }

    force_inline iterator end()
    {
        return iterator(this, INVALID_IDX);
    }

public:
	force_inline T& getbyidx(uint32_t index)
	{
		if (index>=N)
		{
			FASSERT(index>=N);
			return m_tmpdata;
		}

		return m_data[index].data;
	}

	force_inline const T& getbyidx(uint32_t index) const
	{
		if (index>=N)
		{
			FASSERT(index>=N);
			return m_tmpdata;
		}

		return m_data[index].data;
	}

	force_inline int32_t getnextidx(int32_t idx)
	{
		SAFE_TEST_RET_VAL(idx, INVALID_IDX, INVALID_IDX);
		SAFE_TEST_INDEX_VAL(idx, N, INVALID_IDX);
		return m_data[idx].nextindex;
	}

	force_inline int32_t getpreidx(int32_t idx)
	{
		SAFE_TEST_RET_VAL(idx, INVALID_IDX, INVALID_IDX);
		SAFE_TEST_INDEX_VAL(idx, N, INVALID_IDX);
		return m_data[idx].preindex;
	}

private:
	struct Node
	{
		Node() : preindex(INVALID_IDX), nextindex(INVALID_IDX), isdirty(true), data(T())
		{
		}
		~Node()
		{
		}
		int32_t preindex;
		int32_t nextindex;
		bool isdirty;	// true表示不能使用
		T data;
	};
private:
	Node m_data[N];
	typedef fstack<uint32_t, N> indexstack;
	indexstack m_free;
	int32_t m_used;
	T m_tmpdata;
};

/////////////////////////////////////////////////////////////

template <typename T, uint32_t N>
class farray
{
public:
	typedef farray<T, N> MyType;
	typedef T Value;
	typedef fiterator<MyType> iterator;
	friend class fiterator<MyType>;
public:
	force_inline farray() : m_tmpdata(T())
	{
		clear();
	}

	force_inline ~farray()
	{

	}

	force_inline T& operator [](uint32_t index)
	{
		if (index>=N)
		{
			FASSERT(index>=N);
			return m_tmpdata;
		}

		return m_pool[m_indexarray[index]].data;
	}

	force_inline const T& operator [](uint32_t index) const
	{
		if (index>=N)
		{
			FASSERT(index>=N);
			return m_tmpdata;
		}

		return m_pool[m_indexarray[index]].data;
	}

	force_inline void clear()
	{
		m_pool.clear();
		memset(m_indexarray, 0xFF, sizeof(m_indexarray));
	}

	force_inline bool push_back(const T & t)
	{
		if (full())
		{
			return false;
		}

		uint32_t index = size();

		uint32_t newidx = m_pool.allocindex();
		SAFE_TEST_RET_VAL((int32_t)newidx, INVALID_IDX, false);

		Node & node = m_pool[newidx];
		node.data = t;

		m_indexarray[index] = newidx;

		return true;
	}

	force_inline bool pop_back(T & t)
	{
		if (empty())
		{
			return false;
		}

		uint32_t index = size() - 1;
		uint32_t idx = m_indexarray[index];

		Node & node = m_pool[idx];
		t = node.data;

		m_pool.deallocindex(idx);

		return true;
	}

	force_inline bool back(T & t) const
	{
		if (empty())
		{
			return false;
		}

		uint32_t index = size() - 1;
		uint32_t idx = m_indexarray[index];

		const Node & node = m_pool[idx];
		t = node.data;

		return true;
	}

	force_inline bool push_front(const T & t)
	{
		if (full())
		{
			return false;
		}

		int32_t s = size();

		uint32_t newidx = m_pool.allocindex();
		SAFE_TEST_RET_VAL((int32_t)newidx, INVALID_IDX, false);

		Node & node = m_pool[newidx];
		node.data = t;

		if (s > 0)
		{
			memmove(m_indexarray + 1, m_indexarray, s * sizeof(int32_t));
		}
		m_indexarray[0] = newidx;

		return true;
	}

	force_inline bool pop_front(T & t)
	{
		if (empty())
		{
			return false;
		}

		int32_t s = size();

		uint32_t idx = m_indexarray[0];

		Node & node = m_pool[idx];
		t = node.data;

		m_pool.deallocindex(idx);

		m_indexarray[0] = INVALID_IDX;
		if (s > 1)
		{
			memmove(m_indexarray, m_indexarray + 1, (s - 1) * sizeof(int32_t));
		}

		return true;
	}

	force_inline bool front(T & t) const
	{
		if (empty())
		{
			return false;
		}

		uint32_t idx = m_indexarray[0];

		const Node & node = m_pool[idx];
		t = node.data;

		return true;
	}

	force_inline bool insert(int32_t index, const T & t)
	{
		if (full())
		{
			return false;
		}

		int32_t s = size();
		if (index < 0 || index > s)
		{
			return false;
		}

		uint32_t newidx = m_pool.allocindex();
		SAFE_TEST_RET_VAL((int32_t)newidx, INVALID_IDX, false);

		Node & node = m_pool[newidx];
		node.data = t;

		if (s - index > 0)
		{
			memmove(m_indexarray + index + 1, m_indexarray + index, (s - index)*sizeof(int32_t));
		}
		m_indexarray[index] = newidx;

		return true;
	}

	force_inline bool erase(int32_t index)
	{
		if (empty())
		{
			return false;
		}

		int32_t s = size();
		SAFE_TEST_INDEX_VAL(index, s, false);

		uint32_t idx = m_indexarray[index];

		m_pool.deallocindex(idx);

		m_indexarray[index] = INVALID_IDX;

		if (s - index > 1)
		{
			memmove(m_indexarray + index, m_indexarray + index + 1, (s - index - 1)*sizeof(int32_t));
		}

		return true;
	}

	force_inline uint32_t size() const
	{
		return m_pool.size();
	}

	force_inline bool empty() const
	{
		return m_pool.empty();
	}

	force_inline bool full() const
	{
		return m_pool.full();
	}

	force_inline iterator begin()
	{
		return iterator(this, 0);
	}

	force_inline iterator end()
	{
		return iterator(this, size());
	}

private:

	force_inline T& getbyidx(uint32_t index)
	{
		if (index>=N)
		{
			FASSERT(index>=N);
			return m_tmpdata;
		}

		return m_pool[m_indexarray[index]].data;
	}

	force_inline const T& getbyidx(uint32_t index) const
	{
		if (index>=N)
		{
			FASSERT(index>=N);
			return m_tmpdata;
		}

		return m_pool[m_indexarray[index]].data;
	}

	force_inline int32_t getnextidx(int32_t idx)
	{
		if (idx >= 0 && idx + 1 < (int32_t)size())
		{
			return idx + 1;
		}
		return size();
	}

	force_inline int32_t getpreidx(int32_t idx)
	{
		if (idx - 1 >= 0 && idx < (int32_t)size())
		{
			return idx - 1;
		}
		return size();
	}

private:
	struct Node
	{
		T data;
	};
	typedef fpool<Node, N> Pool;
	static const int32_t INVALID_IDX = Pool::INVALID_IDX;
private:
	Pool m_pool;
	int32_t m_indexarray[N];
	T m_tmpdata;
};

/////////////////////////////////////////////////////////////

template <typename T, uint32_t N, typename HashFunc = fhash< T >, typename CmpFunc = fcmp< T > >
class fhashset
{
public:
    static const uint32_t SIZE = fhashersize<3ul, N>::SIZE;
public:
	typedef fhashset<T, N, HashFunc, CmpFunc> MyType;
	typedef T Value;
	typedef fiterator<MyType> iterator;
	friend class fiterator<MyType>;
public:
	force_inline fhashset()
	{
		clear();
	}

	force_inline ~fhashset()
	{

	}

	force_inline T& operator [](const T & t)
	{
	    int32_t idx = real_find(t);
	    if (idx == INVALID_IDX)
	    {
	        idx = real_insert(t);
	    }
		return m_pool[idx].data;
	}

	force_inline const T& operator [](const T & t) const
	{
	    int32_t idx = real_find(t);
	    if (idx == INVALID_IDX)
	    {
	        idx = real_insert(t);
	    }
		return m_pool[idx].data;
	}

	force_inline void clear()
	{
		m_pool.clear();
		memset(m_indexarray, 0xFF, sizeof(m_indexarray));
	}

	force_inline iterator insert(const T & t)
	{
		return iterator(this, real_insert(t));
	}
	
	force_inline iterator find(const T & t)
	{
		return iterator(this, real_find(t));
	}

	force_inline iterator erase(const T & t)
	{
		return iterator(this, real_erase(t));
	}

	force_inline uint32_t size() const
	{
		return m_pool.size();
	}

	force_inline bool empty() const
	{
		return size() == 0;
	}

	force_inline bool full() const
	{
		return size() == N;
	}

	force_inline iterator begin()
	{
		return iterator(this, m_pool.begin().index());
	}

	force_inline iterator end()
	{
		return iterator(this, m_pool.end().index());
	}

public:

	force_inline T& getbyidx(uint32_t index)
	{
		return m_pool[index].data;
	}

	force_inline const T& getbyidx(uint32_t index) const
	{
		return m_pool[index].data;
	}

	force_inline int32_t getnextidx(int32_t idx)
	{
		SAFE_TEST_RET_VAL(idx, INVALID_IDX, INVALID_IDX);
		return m_pool.getnextidx(idx);
	}

	force_inline int32_t getpreidx(int32_t idx)
	{
		SAFE_TEST_RET_VAL(idx, INVALID_IDX, INVALID_IDX);
		return m_pool.getpreidx(idx);
	}

private:
	force_inline int32_t real_insert(const T & t)
	{
	    int32_t hashkey = m_HashFunc(t);
		int32_t hashidx = hashkey % SIZE;
		int32_t nodeidx = m_indexarray[hashidx];
		while (nodeidx != INVALID_IDX)
		{
			Node & node = m_pool[nodeidx];

			if (m_CmpFunc(node.data, t))
			{
				return INVALID_IDX;
			}

			nodeidx = node.nextindex;
		}

        int32_t newidx = m_pool.allocindex();
        if (newidx == INVALID_IDX)
        {
		    return INVALID_IDX;
        }

        int32_t oldidx = m_indexarray[hashidx];
        if (oldidx != INVALID_IDX)
        {
            Node & oldnode = m_pool[oldidx];
            oldnode.preindex = newidx;
        }
        
        Node & node = m_pool[newidx];
        node.preindex = INVALID_IDX;
        node.nextindex = oldidx;
        node.data = t;
        m_indexarray[hashidx] = newidx;

		return newidx;
	}

	force_inline int32_t real_find(const T & t)
	{
		int32_t hashkey = m_HashFunc(t);
		int32_t hashidx = hashkey % SIZE;
		int32_t nodeidx = m_indexarray[hashidx];
		while (nodeidx != INVALID_IDX)
		{
			Node & node = m_pool[nodeidx];

			if (m_CmpFunc(node.data, t))
			{
				return nodeidx;
			}

			nodeidx = node.nextindex;
		}

		return INVALID_IDX;
	}

	force_inline int32_t real_erase(const T & t)
	{
		int32_t findidx = INVALID_IDX;

		int32_t hashkey = m_HashFunc(t);
		int32_t hashidx = hashkey % SIZE;
		int32_t nodeidx = m_indexarray[hashidx];
		while (nodeidx != INVALID_IDX)
		{
			Node & node = m_pool[nodeidx];

			if (m_CmpFunc(node.data, t))
			{
				findidx = nodeidx;
				break;
			}

			nodeidx = node.nextindex;
		}

		SAFE_TEST_RET_VAL(findidx, INVALID_IDX, INVALID_IDX);

		int32_t iteridx = m_pool.getnextidx(findidx);

		Node & node = m_pool[findidx];

		int32_t nextidx = node.nextindex;
		int32_t preindex = node.preindex;

		node.nextindex = INVALID_IDX;
		node.preindex = INVALID_IDX;

		m_pool.deallocindex(findidx);

		if (nextidx != INVALID_IDX)
		{
			m_pool[nextidx].preindex = preindex;
		}

		if (preindex != INVALID_IDX)
		{
			m_pool[preindex].nextindex = nextidx;
		}

		if (findidx == m_indexarray[hashidx])
		{
			m_indexarray[hashidx] = nextidx;
		}

		return iteridx;
	}

private:
	static const int32_t INVALID_IDX = fpool<T, SIZE>::INVALID_IDX;
	struct Node
	{
		Node() : preindex(INVALID_IDX), nextindex(INVALID_IDX), data(T())
		{
		}
		~Node()
		{
		}
		int32_t preindex;
		int32_t nextindex;
		T data;
	};
	typedef fpool<Node, SIZE> Pool;
private:
	Pool m_pool;
	int32_t m_indexarray[SIZE];
	HashFunc m_HashFunc;
	CmpFunc m_CmpFunc;
};

/////////////////////////////////////////////////////////////

template <typename K, typename V, uint32_t N, typename HashFunc = fhash< K >, typename CmpFunc = fcmp< K > >
class fhashmap
{
public:
	struct Node
	{
		Node()
		{
		}
		Node(const K & _k) : first(_k), second(V())
		{
		}
		Node(const K & _k, const V & _v) : first(_k), second(_v)
		{
		}
		~Node()
		{
		}
		K first;
		V second;
	};
public:
	typedef fhashmap<K, V, N, HashFunc, CmpFunc> MyType;
	typedef Node Value;
	typedef fiterator<MyType> iterator;
	friend class fiterator<MyType>;
public:
	force_inline fhashmap()
	{
		clear();
	}

	force_inline ~fhashmap()
	{

	}

	force_inline V& operator [](const K & k)
	{
		return m_set[Node(k)].second;
	}

	force_inline const V& operator [](const K & k) const
	{
		return m_set[Node(k)].second;
	}

	force_inline void clear()
	{
		m_set.clear();
	}

	force_inline iterator insert(const K & k, const V & v)
	{
		return iterator(this, m_set.insert(Node(k, v)).index());
	}
	
	force_inline iterator find(const K & k)
	{
		return iterator(this, m_set.find(Node(k)).index());
	}

	force_inline iterator erase(const K & k)
	{
		return iterator(this, m_set.erase(Node(k)).index());
	}

	force_inline uint32_t size() const
	{
		return m_set.size();
	}

	force_inline bool empty() const
	{
		return m_set.empty();
	}

	force_inline bool full() const
	{
		return m_set.full();
	}

	force_inline iterator begin()
	{
		return iterator(this, m_set.begin().index());
	}

	force_inline iterator end()
	{
		return iterator(this, m_set.end().index());
	}

private:

	force_inline Node& getbyidx(uint32_t index)
	{
		return m_set.getbyidx(index);
	}

	force_inline const Node& getbyidx(uint32_t index) const
	{
		return m_set.getbyidx(index);
	}

	force_inline int32_t getnextidx(int32_t idx)
	{
		return m_set.getnextidx(idx);
	}

	force_inline int32_t getpreidx(int32_t idx)
	{
		return m_set.getpreidx(idx);
	}

private:
    struct NodeCmp
    {
        force_inline bool operator()(const Node & n1, const Node & n2) const
        {
        	return m_CmpFunc(n1.first, n2.first);
        }
	    CmpFunc m_CmpFunc;
    };
    struct NodeHash
    {
        force_inline size_t operator()(const Node & node) const
        {
        	return m_HashFunc(node.first);
        }
	    HashFunc m_HashFunc;
    };
	typedef fhashset<Node, N, NodeHash, NodeCmp> Hashset;
private:
	Hashset m_set;
};

/////////////////////////////////////////////////////////////

template <typename T, uint32_t N>
class flist
{
public:
	typedef flist<T, N> MyType;
	typedef T Value;
	typedef fiterator<MyType> iterator;
	friend class fiterator<MyType>;
public:
	force_inline flist() : m_tmpdata(T())
	{
		clear();
	}

	force_inline ~flist()
	{

	}

	force_inline void clear()
	{
		m_pool.clear();
		m_head = INVALID_IDX;
		m_tail = INVALID_IDX;
	}

	force_inline bool push_back(const T & t)
	{
		return insert_after(m_tail, t);
	}

	force_inline bool pop_back(T & t)
	{
		if (empty())
		{
			return false;
		}

		Node & node = m_pool[m_tail];
		t = node.data;

		return erase(m_tail);
	}

	force_inline bool back(T & t) const
	{
		if (empty())
		{
			return false;
		}

		const Node & node = m_pool[m_tail];
		t = node.data;

		return true;
	}

	force_inline T & back()
	{
		if (empty())
		{
			return m_tmpdata;
		}

		Node & node = m_pool[m_tail];
		return node.data;
	}

	force_inline bool push_front(const T & t)
	{
		return insert_before(m_head, t);
	}

	force_inline bool pop_front(T & t)
	{
		if (empty())
		{
			return false;
		}

		Node & node = m_pool[m_head];
		t = node.data;

		return erase(m_head);
	}

	force_inline bool front(T & t) const
	{
		if (empty())
		{
			return false;
		}

		const Node & node = m_pool[m_head];
		t = node.data;

		return true;
	}

	force_inline T & front()
	{
		if (empty())
		{
			return m_tmpdata;
		}

		Node & node = m_pool[m_head];
		return node.data;
	}

	force_inline bool erase(iterator it)
	{
		return erase(it.index());
	}

	force_inline uint32_t size() const
	{
		return m_pool.size();
	}

	force_inline bool empty() const
	{
		return m_pool.empty();
	}

	force_inline bool full() const
	{
		return m_pool.full();
	}

	force_inline iterator begin()
	{
		return iterator(this, m_head);
	}

	force_inline iterator end()
	{
		return iterator(this, INVALID_IDX);
	}

private:
	force_inline bool erase(int32_t idx)
	{
		if (empty())
		{
			return INVALID_IDX;
		}

		SAFE_TEST_INDEX_VAL(idx, N, false);
		Node & node = m_pool[idx];

		int32_t preindex = node.preindex;
		int32_t nextindex = node.nextindex;

		node.preindex = INVALID_IDX;
		node.nextindex = INVALID_IDX;

		m_pool.deallocindex(idx);

		if (preindex != INVALID_IDX)
		{
			Node & prenode = m_pool[preindex];
			prenode.nextindex = nextindex;
		}

		if (nextindex != INVALID_IDX)
		{
			Node & nextnode = m_pool[nextindex];
			nextnode.preindex = preindex;
		}

		if (m_head == idx)
		{
			m_head = nextindex;
		}

		if (m_tail == idx)
		{
			m_tail = preindex;
		}

		return true;
	}

	force_inline bool insert_before(int32_t idx, const T & t)
	{
		if (full())
		{
			return false;
		}

		int32_t preindex = INVALID_IDX;
		if (idx != INVALID_IDX)
		{
			Node & node = m_pool[idx];
			preindex = node.preindex;
		}

		uint32_t newidx = m_pool.allocindex();
		SAFE_TEST_RET_VAL((int32_t)newidx, INVALID_IDX, false);

		Node & newnode = m_pool[newidx];
		newnode.preindex = preindex;
		newnode.nextindex = idx;
		newnode.data = t;

		if (preindex != INVALID_IDX)
		{
			Node & prenode = m_pool[preindex];
			prenode.nextindex = newidx;
		}
		
		if (idx != INVALID_IDX)
		{
			Node & nextnode = m_pool[idx];
			nextnode.preindex = newidx;
		}
		
		if (m_head == idx)
		{
			m_head = newidx;
		}

		if (m_head == INVALID_IDX)
		{
			m_head = newidx;
		}

		if (m_tail == INVALID_IDX)
		{
			m_tail = newidx;
		}

		return true;
	}

	force_inline bool insert_after(int32_t idx, const T & t)
	{
		if (full())
		{
			return false;
		}

		int32_t nextindex = INVALID_IDX;
		if (idx != INVALID_IDX)
		{
			Node & node = m_pool[idx];
			nextindex = node.nextindex;
		}

		uint32_t newidx = m_pool.allocindex();
		SAFE_TEST_RET_VAL((int32_t)newidx, INVALID_IDX, false);

		Node & newnode = m_pool[newidx];
		newnode.preindex = idx;
		newnode.nextindex = nextindex;
		newnode.data = t;

		if (nextindex != INVALID_IDX)
		{
			Node & nextnode = m_pool[nextindex];
			nextnode.preindex = newidx;
		}

		if (idx != INVALID_IDX)
		{
			Node & nextnode = m_pool[idx];
			nextnode.nextindex = newidx;
		}

		if (m_tail == idx)
		{
			m_tail = newidx;
		}

		if (m_head == INVALID_IDX)
		{
			m_head = newidx;
		}

		if (m_tail == INVALID_IDX)
		{
			m_tail = newidx;
		}

		return true;
	}
private:
	force_inline T& getbyidx(uint32_t index)
	{
		return m_pool[index].data;
	}

	force_inline const T& getbyidx(uint32_t index) const
	{
		return m_pool[index].data;
	}

	force_inline int32_t getnextidx(int32_t idx)
	{
		return m_pool[idx].nextindex;
	}

	force_inline int32_t getpreidx(int32_t idx)
	{
		return m_pool[idx].preindex;
	}

private:
	static const int32_t INVALID_IDX = fpool<T, N>::INVALID_IDX;
	struct Node
	{
		Node() : preindex(INVALID_IDX), nextindex(INVALID_IDX)
		{
		}
		~Node()
		{
		}
		int32_t preindex;
		int32_t nextindex;
		T data;
	};
	typedef fpool<Node, N> Pool;
private:
	Pool m_pool;
	int32_t m_head;
	int32_t m_tail;
	T m_tmpdata;
};

/////////////////////////////////////////////////////////////

template <typename T> class vector3d;

template <typename T> class point2d
{
    
    public:
        
        force_inline point2d() :
            X(0),
            Y(0)
        {
        }
        force_inline point2d(T Size) :
            X(Size),
            Y(Size)
        {
        }
        force_inline point2d(T PointX, T PointY) :
            X(PointX),
            Y(PointY)
        {
        }
        force_inline point2d(const point2d<T> &other) :
            X(other.X),
            Y(other.Y)
        {
        }
        force_inline point2d(const vector3d<T> &other);
        force_inline ~point2d()
        {
        }
        
        /* === Operators - comparisions === */
        
        force_inline bool operator == (const point2d<T> &other) const
        {
            return X == other.X && Y == other.Y;
        }
        force_inline bool operator != (const point2d<T> &other) const
        {
            return X != other.X || Y != other.Y;
        }
        
        force_inline bool operator > (const point2d<T> &other) const
        {
            return (X == other.X) ? Y > other.Y : X > other.X;
        }
        force_inline bool operator < (const point2d<T> &other) const
        {
            return (X == other.X) ? Y < other.Y : X < other.X;
        }
        
        force_inline bool operator >= (const point2d<T> &other) const
        {
            return (X == other.X) ? Y >= other.Y : X >= other.X;
        }
        force_inline bool operator <= (const point2d<T> &other) const
        {
            return (X == other.X) ? Y <= other.Y : X <= other.X;
        }
        
        /* === Operators - addition, subtraction, division, multiplication === */
        
        force_inline point2d<T> operator + (const point2d<T> &other) const
        {
            return point2d<T>(X + other.X, Y + other.Y);
        }
        force_inline point2d<T>& operator += (const point2d<T> &other)
        {
            X += other.X; Y += other.Y; return *this;
        }
        
        force_inline point2d<T> operator - (const point2d<T> &other) const
        {
            return point2d<T>(X - other.X, Y - other.Y);
        }
        force_inline point2d<T>& operator -= (const point2d<T> &other)
        {
            X -= other.X; Y -= other.Y; return *this;
        }
        
        force_inline point2d<T> operator / (const point2d<T> &other) const
        {
            return point2d<T>(X / other.X, Y / other.Y);
        }
        force_inline point2d<T>& operator /= (const point2d<T> &other)
        {
            X /= other.X; Y /= other.Y; return *this;
        }
        
        force_inline point2d<T> operator * (const point2d<T> &other) const
        {
            return point2d<T>(X * other.X, Y * other.Y);
        }
        force_inline point2d<T>& operator *= (const point2d<T> &other)
        {
            X *= other.X; Y *= other.Y; return *this;
        }
        
        force_inline point2d<T> operator / (T Size) const
        {
            return point2d<T>(X / Size, Y / Size);
        }
        force_inline point2d<T>& operator /= (T Size)
        {
            X /= Size; Y /= Size; return *this;
        }
        
        force_inline point2d<T> operator * (T Size) const
        {
            return point2d<T>(X * Size, Y * Size);
        }
        force_inline point2d<T>& operator *= (T Size)
        {
            X *= Size; Y *= Size; return *this;
        }
        
        force_inline point2d<T> operator - () const
        {
            return point2d<T>(-X, -Y);
        }
        
        /* === Additional operators === */
        
        force_inline const T operator [] (int32_t i) const
        {
            switch (i)
            {
                case 0: return X;
                case 1: return Y;
            }
            return 0;
        }
        
        force_inline T& operator [] (int32_t i)
        {
            return *(&X + i);
        }
        
        /* === Static functions === */
        
        static force_inline void make3DProjection(T &NewX, T &NewY, int32_t ScreenWidth, int32_t ScreenHeight)
        {
            NewX = static_cast<T>( static_cast<float>(  NewX - ScreenWidth /2) / (ScreenWidth /2) );
            NewY = static_cast<T>( static_cast<float>(- NewY + ScreenHeight/2) / (ScreenHeight/2) );
        }
        
        /* === Extra functions === */
        
        force_inline T dot(const point2d<T> &other) const // Dot/ Scalar product
        {
            return X*other.X + Y*other.Y;
        }
        
        force_inline T getLength() const
        {
            return sqrt(X*X + Y*Y);
        }
        force_inline T getLengthSq() const
        {
            return X*X + Y*Y;
        }
        
        force_inline void make3DProjection(int32_t ScreenWidth, int32_t ScreenHeight) // (for 2D graphics)
        {
            X = static_cast<T>( static_cast<float>( X - ScreenWidth /2) / (ScreenWidth /2) );
            Y = static_cast<T>( static_cast<float>(-Y + ScreenHeight/2) / (ScreenHeight/2) );
        }
        
        force_inline void make3DProjectionOrigin(int32_t ScreenWidth, int32_t ScreenHeight)
        {
            X = static_cast<T>( static_cast<float>(X) / (ScreenWidth /2) );
            Y = static_cast<T>( static_cast<float>(Y) / (ScreenHeight/2) );
        }
        
        force_inline void make3DFrustum(float Width, float Height)
        {
            const float aspect = Width / Height;
            const float stdasp = 4.0f / 3.0f;
            
            X = static_cast<T>( float( X - Width /2) / (Width/2) * aspect / stdasp );
            Y = static_cast<T>( float(-Y + Height/2) / (Width/2) * aspect / stdasp );
        }
        
        force_inline point2d<T>& setAbs()
        {
            X = X > 0 ? X : -X;
            Y = Y > 0 ? Y : -Y;
            return *this;
        }
        force_inline point2d<T> getAbs() const
        {
            return point2d<T>(
                X > 0 ? X : -X,
                Y > 0 ? Y : -Y
            );
        }
        
        force_inline point2d<T>& normalize()
        {
            T n = X*X + Y*Y;
            
            if (n == 1 || n == 0)
                return *this;
            
            n = static_cast<T>(1.0 / sqrt(n));
            
            X *= n;
            Y *= n;
            
            return *this;
        }
        
        force_inline void set(T NewX, T NewY)
        {
            X = NewX; Y = NewY;
        }
        force_inline void get(T &NewX, T &NewY) const
        {
            NewX = X; NewY = Y;
        }
        
        force_inline void setLength(T Length)
        {
            normalize();
            *this *= Length;
        }
        
        force_inline point2d<T>& getCircleCollision(float ThisRadius, point2d<T> &OtherPoint, float OtherRadius)
        {
            float Distance = sqrt( (OtherPoint.X - X)*(OtherPoint.X - X) + (OtherPoint.Y - Y)*(OtherPoint.Y - Y) );
            float Degree = asin( (OtherPoint.X - X) / Distance )*180.0f/M_PI;
            
            if (Y < OtherPoint.Y)
                Degree = 180 - Degree;
            
            if (Distance < ThisRadius + OtherRadius) {
                OtherPoint.X = X + sin(Degree*M_PI/180) * (ThisRadius + OtherRadius);
                OtherPoint.Y = Y + cos(Degree*M_PI/180) * (ThisRadius + OtherRadius);
            }
            
            return OtherPoint;
        }
        
        force_inline bool isPointInsideCircle(const point2d<T> &Center, const float Radius) const
        {
            return (X - Center.X)*(X - Center.X) + (Y - Center.Y)*(Y - Center.Y) < Radius*Radius;
        }

        force_inline T getMin() const
        {
            return (X <= Y) ? X : Y;
        }
        force_inline T getMax() const
        {
            return (X >= Y) ? X : Y;
        }
        
        template <typename B> force_inline point2d<B> cast() const
        {
            return point2d<B>(static_cast<B>(X), static_cast<B>(Y));
        }
        
        /* Members */
        
        T X, Y;
        
};

typedef point2d<int32_t> point2di;
typedef point2d<float> point2df;

// ================================================================================

//! Returns the distance between the two given 2D points.
template <typename T> force_inline T getDistance(const point2d<T> &PosA, const point2d<T> &PosB)
{
    return Sqrt(Pow2(PosB.X - PosA.X) + Pow2(PosB.Y - PosA.Y));
}

/////////////////////////////////////////////////////////////

template <typename T> class size2d
{
    
    public:
        
        force_inline size2d() :
            Width   (0),
            Height  (0)
        {
        }
        force_inline size2d(T Size) :
            Width   (Size),
            Height  (Size)
        {
        }
        force_inline size2d(T InitWidth, T InitHeight) :
            Width   (InitWidth  ),
            Height  (InitHeight )
        {
        }
        force_inline size2d(const size2d<T> &Other) :
            Width   (Other.Width    ),
            Height  (Other.Height   )
        {
        }
        force_inline ~size2d()
        {
        }
        
        /* === Operators - comparisions === */
        
        force_inline bool operator == (const size2d<T> &Other) const
        {
            return Width == Other.Width && Height == Other.Height;
        }
        force_inline bool operator != (const size2d<T> &Other) const
        {
            return Width != Other.Width || Height != Other.Height;
        }
        
        //! Returns true if this width and height are greater to the Other.
        force_inline bool operator > (const size2d<T> &Other) const
        {
            return getArea() > Other.getArea();
        }
        //! Returns true if this width and height are smaller to the Other.
        force_inline bool operator < (const size2d<T> &Other) const
        {
            return getArea() < Other.getArea();
        }
        
        //! Returns true if this width and height are greater or equal to the Other.
        force_inline bool operator >= (const size2d<T> &Other) const
        {
            return getArea() >= Other.getArea();
        }
        //! Returns true if this width and height are small or equal to the Other.
        force_inline bool operator <= (const size2d<T> &Other) const
        {
            return getArea() <= Other.getArea();
        }
        
        /* === Operators - addition, subtraction, division, multiplication === */
        
        force_inline size2d<T> operator + (const size2d<T> &Other) const
        {
            return size2d<T>(Width + Other.Width, Height + Other.Height);
        }
        force_inline size2d<T>& operator += (const size2d<T> &Other)
        {
            Width += Other.Width; Height += Other.Height; return *this;
        }
        
        force_inline size2d<T> operator - (const size2d<T> &Other) const
        {
            return size2d<T>(Width - Other.Width, Height - Other.Height);
        }
        force_inline size2d<T>& operator -= (const size2d<T> &Other)
        {
            Width -= Other.Width; Height -= Other.Height; return *this;
        }
        
        force_inline size2d<T> operator / (const size2d<T> &Other) const
        {
            return size2d<T>(Width / Other.Width, Height / Other.Height);
        }
        force_inline size2d<T>& operator /= (const size2d<T> &Other)
        {
            Width /= Other.Width; Height /= Other.Height; return *this;
        }
        
        force_inline size2d<T> operator * (const size2d<T> &Other) const
        {
            return size2d<T>(Width * Other.Width, Height * Other.Height);
        }
        force_inline size2d<T>& operator *= (const size2d<T> &Other)
        {
            Width *= Other.Width; Height *= Other.Height; return *this;
        }
        
        force_inline size2d<T> operator - () const
        {
            return size2d<T>(-Width, -Height);
        }
        
        /* Extra functions */
        
        force_inline T getArea() const
        {
            return Width * Height;
        }
        
        //! Clamps this size to the specified maximum size and returns the new one. The aspect ratio remains the same.
        force_inline size2d<T> getClampedSize(const size2d<T> &MaxSize) const
        {
            if (Width < MaxSize.Width && Height < MaxSize.Height)
                return *this;
            
            double Scale = 1.0;
            
            if (Width - MaxSize.Width > Height - MaxSize.Height)
                Scale = static_cast<double>(MaxSize.Width) / Width;
            else
                Scale = static_cast<double>(MaxSize.Height) / Height;
            
            return size2d<T>(
                Min(static_cast<T>(Scale * Width), MaxSize.Width),
                Min(static_cast<T>(Scale * Height), MaxSize.Width)
            );
        }
        
        template <typename B> force_inline size2d<B> cast() const
        {
            return size2d<B>(static_cast<B>(Width), static_cast<B>(Height));
        }
        
        /* Members */
        
        T Width, Height;
        
};

typedef size2d<int32_t> size2di;
typedef size2d<float> size2df;

/////////////////////////////////////////////////////////////


template <typename T> class rect2d
{
    
    public:
        
        force_inline rect2d() :
            Left    (0),
            Right   (0),
            Top     (0),
            Bottom  (0)
        {
        }
        force_inline rect2d(T X, T Y) :
            Left    (X),
            Right   (X),
            Top     (Y),
            Bottom  (Y)
        {
        }
        force_inline rect2d(T NewLeft, T NewTop, T NewRight, T NewBottom) :
            Left    (NewLeft    ),
            Right   (NewRight   ),
            Top     (NewTop     ),
            Bottom  (NewBottom  )
        {
        }
        force_inline rect2d(const rect2d<T> &Other) :
            Left    (Other.Left     ),
            Right   (Other.Right    ),
            Top     (Other.Top      ),
            Bottom  (Other.Bottom   )
        {
        }
        force_inline ~rect2d()
        {
        }
        
        /* === Operators === */
        
        force_inline bool operator == (const rect2d<T> &other) const
        {
            return Left == other.Left && Top == other.Top && Right == other.Right && Bottom == other.Bottom;
        }
        force_inline bool operator != (const rect2d<T> &other) const
        {
            return Left != other.Left && Top != other.Top && Right != other.Right && Bottom != other.Bottom;
        }
        
        force_inline rect2d<T> operator + (const rect2d<T> &other) const
        {
            return rect2d<T>(Left + other.Left, Top + other.Top, Right + other.Right, Bottom + other.Bottom);
        }
        force_inline rect2d<T>& operator += (const rect2d<T> &other)
        {
            Left += other.Left; Top += other.Top; Right += other.Right; Bottom += other.Bottom; return *this;
        }
        
        force_inline rect2d<T> operator - (const rect2d<T> &other) const
        {
            return rect2d<T>(Left - other.Left, Top - other.Top, Right - other.Right, Bottom - other.Bottom);
        }
        force_inline rect2d<T>& operator -= (const rect2d<T> &other)
        {
            Left -= other.Left; Top -= other.Top; Right -= other.Right; Bottom -= other.Bottom; return *this;
        }
        
        force_inline rect2d<T> operator / (const rect2d<T> &other) const
        {
            return rect2d<T>(Left / other.Left, Top / other.Top, Right / other.Right, Bottom / other.Bottom);
        }
        force_inline rect2d<T>& operator /= (const rect2d<T> &other)
        {
            Left /= other.Left; Top /= other.Top; Right /= other.Right; Bottom /= other.Bottom; return *this;
        }
        
        force_inline rect2d<T> operator * (const rect2d<T> &other) const
        {
            return rect2d<T>(Left * other.Left, Top * other.Top, Right * other.Right, Bottom * other.Bottom);
        }
        force_inline rect2d<T>& operator *= (const rect2d<T> &other)
        {
            Left *= other.Left; Top *= other.Top; Right *= other.Right; Bottom *= other.Bottom; return *this;
        }
        
        force_inline rect2d<T> operator - () const
        {
            return rect2d<T>(-Left, -Top, -Right, -Bottom);
        }
        
        /* === Functions === */
        
        //! Sets the rectangle's size.
        force_inline void setSize(const size2d<T> &Size)
        {
            Right   = Left  + Size.Width;
            Bottom  = Top   + Size.Height;
        }
        //! Returns the rectangle's size.
        force_inline size2d<T> getSize() const
        {
            return size2d<T>(Right - Left, Bottom - Top);
        }
        
        //! Sets the rectangle's center point.
        force_inline void setCenter(const point2d<T> &Center)
        {
            *this += rect2d<T>(Center - getCenter());
        }
        //! Returns the rectangle's center point.
        force_inline point2d<T> getCenter() const
        {
            return point2d<T>((Right + Left)/2, (Bottom + Top)/2);
        }
        
        //! Sets the left-top point.
        force_inline void setLTPoint(const point2d<T> &Point)
        {
            Left    = Point.X;
            Top     = Point.Y;
        }
        //! Returns the left-top point.
        force_inline point2d<T> getLTPoint() const
        {
            return point2d<T>(Left, Top);
        }
        
        //! Sets the right-bottom point.
        force_inline void setRBPoint(const point2d<T> &Point)
        {
            Right   = Point.X;
            Bottom  = Point.Y;
        }
        //! Returns the right-bottom point.
        force_inline point2d<T> getRBPoint() const
        {
            return point2d<T>(Right, Bottom);
        }
        
        //! Returns the rectangle's width (Right - Left).
        force_inline T getWidth() const
        {
            return Right - Left;
        }
        //! Returns the rectangle's height (Bottom - Top).
        force_inline T getHeight() const
        {
            return Bottom - Top;
        }
        //! Returns true if all four components are 0.
        force_inline bool empty() const
        {
            return Left == Right == Top == Bottom == 0;
        }
        //! Returns true if this is a valid rectangle.
        force_inline bool valid() const
        {
            return getWidth() >= 0 && getHeight() >= 0;
        }
        
        force_inline rect2d<T>& repair()
        {
            if (Left > Right)
                Swap(Left, Right);
            if (Top > Bottom)
                Swap(Top, Bottom);
            return *this;
        }
        
        force_inline bool overlap(const point2d<T> &Point) const
        {
            return (Point.X >= Left && Point.X < Right && Point.Y >= Top && Point.Y < Bottom);
        }
        force_inline bool overlap(const rect2d<T> &other) const
        {
            return (Bottom > other.Top && Top < other.Bottom && Right > other.Left && Left < other.Right);
        }
        
        template <typename B> force_inline rect2d<B> cast() const
        {
            return rect2d<B>(static_cast<B>(Left), static_cast<B>(Top), static_cast<B>(Right), static_cast<B>(Bottom));
        }
        
        /* === Members === */
        
        T Left, Right, Top, Bottom;
        
};

typedef rect2d<int32_t> rect2di;
typedef rect2d<float> rect2df;

/////////////////////////////////////////////////////////////

template <typename T> class vector4d;
template <typename T> class point2d;
template <typename T> class size2d;


//! Axis direction types.
enum EAxisTypes
{
    AXIS_X_POSITIVE = 0,
    AXIS_X_NEGATIVE,
    AXIS_Y_POSITIVE,
    AXIS_Y_NEGATIVE,
    AXIS_Z_POSITIVE,
    AXIS_Z_NEGATIVE,
};


/**
Vector 3D class which has the three components X, Y and Z. This is the main class
used for 3D scene directions, positions, scaling etc.
*/
template <typename T> class vector3d
{
    
    public:
        
        force_inline vector3d() :
            X(0),
            Y(0),
            Z(0)
        {
        }
        force_inline vector3d(const T &Size) :
            X(Size),
            Y(Size),
            Z(Size)
        {
        }
        force_inline vector3d(const T &VecX, const T &VecY, const T &VecZ) :
            X(VecX),
            Y(VecY),
            Z(VecZ)
        {
        }
        force_inline vector3d(const T &VecX, const T &VecY, const T &VecZ, const T &VecW) :
            X(VecX*VecW),
            Y(VecY*VecW),
            Z(VecZ*VecW)
        {
        }
        force_inline vector3d(const vector3d<T> &Other) :
            X(Other.X),
            Y(Other.Y),
            Z(Other.Z)
        {
        }
        force_inline vector3d(const vector4d<T> &Other);
        force_inline vector3d(const point2d<T> &Other);
        force_inline vector3d(const size2d<T> &Other);
        force_inline ~vector3d()
        {
        }
        
        /* === Operators - comparisions === */
        
        force_inline bool operator == (const vector3d<T> &other) const
        {
            return Equal(X, other.X) && Equal(Y, other.Y) && Equal(Z, other.Z);
        }
        force_inline bool operator != (const vector3d<T> &other) const
        {
            return !Equal(X, other.X) || !Equal(Y, other.Y) || !Equal(Z, other.Z);
        }
        
        force_inline bool operator <= (const vector3d<T> &other) const
        {
            return
                ( X < other.X || Equal(X, other.X) ) ||
                ( Equal(X, other.X) && ( Y < other.Y || Equal(Y, other.Y) ) ) ||
                ( Equal(X, other.X) && Equal(Y, other.Y) && ( Z < other.Z || Equal(Z, other.Z) ) );
        }
        force_inline bool operator >= (const vector3d<T> &other) const
        {
            return
                ( X > other.X || Equal(X, other.X) ) ||
                ( Equal(X, other.X) && (Y > other.Y || Equal(Y, other.Y) ) ) ||
                ( Equal(X, other.X) && Equal(Y, other.Y) && ( Z > other.Z || Equal(Z, other.Z) ) );
        }
        
        force_inline bool operator < (const vector3d<T> &other) const
        {
            return
                ( X < other.X && !Equal(X, other.X) ) ||
                ( Equal(X, other.X) && Y < other.Y && !Equal(Y, other.Y) ) ||
                ( Equal(X, other.X) && Equal(Y, other.Y) && Z < other.Z && !Equal(Z, other.Z) );
        }
        force_inline bool operator > (const vector3d<T> &other) const
        {
            return
                ( X > other.X && !Equal(X, other.X) ) ||
                ( Equal(X, other.X) && Y > other.Y && !Equal(Y, other.Y) ) ||
                ( Equal(X, other.X) && Equal(Y, other.Y) && Z > other.Z && !Equal(Z, other.Z) );
        }
        
        /* === Operators - addition, subtraction, division, multiplication === */
        
        //! Pre-increment operator.
        force_inline vector3d<T>& operator ++ ()
        {
            ++X; ++Y; ++Z; return *this;
        }
        //! Post-increment operator.
        force_inline vector3d<T>& operator ++ (int)
        {
            const vector3d<T> Tmp(*this);
            ++*this;
            return Tmp;
        }
        
        //! Pre-decrement operator.
        force_inline vector3d<T>& operator -- ()
        {
            --X; --Y; --Z; return *this;
        }
        //! Post-decrement operator.
        force_inline vector3d<T>& operator -- (int)
        {
            const vector3d<T> Tmp(*this);
            --*this;
            return Tmp;
        }
        
        force_inline vector3d<T> operator + (const vector3d<T> &other) const
        {
            return vector3d<T>(X + other.X, Y + other.Y, Z + other.Z);
        }
        force_inline vector3d<T>& operator += (const vector3d<T> &other)
        {
            X += other.X; Y += other.Y; Z += other.Z; return *this;
        }
        
        force_inline vector3d<T> operator - (const vector3d<T> &other) const
        {
            return vector3d<T>(X - other.X, Y - other.Y, Z - other.Z);
        }
        force_inline vector3d<T>& operator -= (const vector3d<T> &other)
        {
            X -= other.X; Y -= other.Y; Z -= other.Z; return *this;
        }
        
        force_inline vector3d<T> operator / (const vector3d<T> &other) const
        {
            return vector3d<T>(X / other.X, Y / other.Y, Z / other.Z);
        }
        force_inline vector3d<T>& operator /= (const vector3d<T> &other)
        {
            X /= other.X; Y /= other.Y; Z /= other.Z; return *this;
        }
        
        force_inline vector3d<T> operator * (const vector3d<T> &other) const
        {
            return vector3d<T>(X * other.X, Y * other.Y, Z * other.Z);
        }
        force_inline vector3d<T>& operator *= (const vector3d<T> &other)
        {
            X *= other.X; Y *= other.Y; Z *= other.Z; return *this;
        }
        
        force_inline vector3d<T> operator * (const T &Size) const
        {
            return vector3d<T>(X * Size, Y * Size, Z * Size);
        }
        force_inline vector3d<T>& operator *= (const T &Size)
        {
            X *= Size; Y *= Size; Z *= Size; return *this;
        }
        
        force_inline vector3d<T> operator / (const T &Size) const
        {
            return *this * (T(1) / Size);
        }
        force_inline vector3d<T>& operator /= (const T &Size)
        {
            return *this *= (T(1) / Size);
        }
        
        force_inline vector3d<T> operator - () const
        {
            return vector3d<T>(-X, -Y, -Z);
        }
        
        /* === Additional operators === */
        
        force_inline const T operator [] (uint32_t i) const
        {
            return i < 3 ? *(&X + i) : static_cast<T>(0.0);
        }
        
        force_inline T& operator [] (uint32_t i)
        {
            return *(&X + i);
        }
        
        /* === Extra functions === */
        
        //! Returns the dot (or rather scalar) product between this and the given vector.
        force_inline T dot(const vector3d<T> &other) const
        {
            return X*other.X + Y*other.Y + Z*other.Z;
        }
        
        //! Returns the cross (or rather vector) product between this and the given vector.
        force_inline vector3d<T> cross(const vector3d<T> &other) const
        {
            return vector3d<T>(
                Y*other.Z - other.Y*Z,
                Z*other.X - other.Z*X,
                X*other.Y - other.X*Y
            );
        }
        
        //! Returns the vector's length.
        force_inline T getLength() const
        {
            return sqrt(X*X + Y*Y + Z*Z);
        }
        
        //! Returns the square of the vector's length (Can be used for faster comparision between two distances).
        force_inline T getLengthSq() const
        {
            return X*X + Y*Y + Z*Z;
        }
        
        //! Returns the angle (in degrees) between this and the given vector.
        force_inline T getAngle(const vector3d<T> &other) const
        {
            return static_cast<T>(ACos( dot(other) / (getLength()*other.getLength()) ));
        }
        
        force_inline vector3d<T>& setInvert()
        {
            X = -X; Y = -Y; Z = -Z; return *this;
        }
        force_inline vector3d<T> getInvert() const
        {
            return vector3d<T>(-X, -Y, -Z);
        }
        
        force_inline vector3d<T>& setRound(int32_t Precision)
        {
            Precision = static_cast<int32_t>(pow(10, Precision));
            X = static_cast<T>(static_cast<int32_t>(X*Precision)) / Precision;
            Y = static_cast<T>(static_cast<int32_t>(Y*Precision)) / Precision;
            Z = static_cast<T>(static_cast<int32_t>(Z*Precision)) / Precision;
            return *this;
        }
        force_inline vector3d<T> getRound(int32_t Precision) const
        {
            Precision = static_cast<int32_t>(pow(10, Precision));
            return vector3d<T>(
                static_cast<T>(static_cast<int32_t>(X*Precision)) / Precision,
                static_cast<T>(static_cast<int32_t>(Y*Precision)) / Precision,
                static_cast<T>(static_cast<int32_t>(Z*Precision)) / Precision
            );
        }
        
        force_inline bool equal(const vector3d<T> &other, float Tolerance = ROUNDING_ERROR) const
        {
            return
                (X + Tolerance > other.X) && (X - Tolerance < other.X) &&
                (Y + Tolerance > other.Y) && (Y - Tolerance < other.Y) &&
                (Z + Tolerance > other.Z) && (Z - Tolerance < other.Z);
        }
        force_inline bool empty() const
        {
            return equal(0);
        }
        
        force_inline void make2DProjection(int32_t ScreenWidth, int32_t ScreenHeight)
        {
            X =   X * static_cast<float>(ScreenWidth /2) + ScreenWidth /2;
            Y = - Y * static_cast<float>(ScreenHeight/2) + ScreenHeight/2;
            Z = T(0);
        }
        force_inline void make2DProjection(float FOV, int32_t ScreenWidth, int32_t ScreenHeight)
        {
            X =   X / Z * FOV + ScreenWidth /2;
            Y = - Y / Z * FOV + ScreenHeight/2;
        }
        
        force_inline vector3d<T>& setAbs()
        {
            X = X > 0 ? X : -X;
            Y = Y > 0 ? Y : -Y;
            Z = Z > 0 ? Z : -Z;
            return *this;
        }
        force_inline vector3d<T> getAbs() const
        {
            return vector3d<T>(
                X > 0 ? X : -X,
                Y > 0 ? Y : -Y,
                Z > 0 ? Z : -Z
            );
        }
        
        force_inline vector3d<T>& normalize()
        {
            T n = X*X + Y*Y + Z*Z;
            
            if (n == T(0) || n == T(1))
                return *this;
            
            n = T(1) / sqrt(n);
            
            X *= n;
            Y *= n;
            Z *= n;
            
            return *this;
        }
        force_inline vector3d<T>& sign()
        {
            if (X > 0) X = 1; else if (X < 0) X = -1;
            if (Y > 0) Y = 1; else if (Y < 0) Y = -1;
            if (Z > 0) Z = 1; else if (Z < 0) Z = -1;
            return *this;
        }
        
        force_inline vector3d<T>& setLength(const T &Length)
        {
            normalize();
            *this *= Length;
            return *this;
        }
        
        force_inline T getDistanceFromSq(const vector3d<T> &other) const
        {
            return vector3d<T>(X - other.X, Y - other.Y, Z - other.Z).getLengthSq();
        }
        
        force_inline bool isBetweenPoints(const vector3d<T> &Begin, const vector3d<T> &End) const
        {
            const T Temp = (End - Begin).getLengthSq();
            return getDistanceFromSq(Begin) <= Temp && getDistanceFromSq(End) <= Temp;
        }
        
        force_inline bool isPointInsideSphere(const vector3d<T> &Center, const float Radius) const
        {
            return Pow2(X - Center.X) + Pow2(Y - Center.Y) + Pow2(Z - Center.Z) < Pow2(Radius);
        }
        
        force_inline vector3d<T> getInterpolatedQuadratic(const vector3d<T> &v2, const vector3d<T> &v3, const T d) const
        {
            const T inv = static_cast<T>(1.0) - d;
            const T mul0 = inv * inv;
            const T mul1 = static_cast<T>(2.0) * d * inv;
            const T mul2 = d * d;
            
            return vector3d<T>(
                X * mul0 + v2.X * mul1 + v3.X * mul2,
                Y * mul0 + v2.Y * mul1 + v3.Y * mul2,
                Z * mul0 + v2.Z * mul1 + v3.Z * mul2
            );
        }
        
        force_inline vector3d<T> getRotatedAxis(const T &Angle, vector3d<T> Axis) const
        {
            if (Angle == T(0))
                return *this;
            
            Axis.normalize();
            
            vector3d<T> rotMatrixRow1, rotMatrixRow2, rotMatrixRow3;
            
            T sinAngle      = sin(Angle*DEG);
            T cosAngle      = cos(Angle*DEG);
            T cosAngleInv   = 1.0f - cosAngle;
            
            rotMatrixRow1.X = Axis.X*Axis.X + cosAngle*(1.0f - Axis.X*Axis.X);
            rotMatrixRow1.Y = Axis.X*Axis.Y*cosAngleInv - sinAngle*Axis.Z;
            rotMatrixRow1.Z = Axis.X*Axis.Z*cosAngleInv + sinAngle*Axis.Y;
            
            rotMatrixRow2.X = Axis.X*Axis.Y*cosAngleInv + sinAngle*Axis.Z;
            rotMatrixRow2.Y = Axis.Y*Axis.Y + cosAngle*(1.0f - Axis.Y*Axis.Y);
            rotMatrixRow2.Z = Axis.Y*Axis.Z*cosAngleInv - sinAngle*Axis.X;
            
            rotMatrixRow3.X = Axis.X*Axis.Z*cosAngleInv - sinAngle*Axis.Y;
            rotMatrixRow3.Y = Axis.Y*Axis.Z*cosAngleInv + sinAngle*Axis.X;
            rotMatrixRow3.Z = Axis.Z*Axis.Z + cosAngle*(1.0f - Axis.Z*Axis.Z);
            
            return vector3d<T>(
                dot(rotMatrixRow1),
                dot(rotMatrixRow2),
                dot(rotMatrixRow3)
            );
        }
        
        //! Returns the direction type of the dominant axis.
        force_inline EAxisTypes getAxisType() const
        {
            const vector3d<T> AbsDir(getAbs());
            
            if (AbsDir.X >= AbsDir.Y && AbsDir.X >= AbsDir.Z)
                return (X > 0 ? AXIS_X_POSITIVE : AXIS_X_NEGATIVE);
            else if (AbsDir.Y >= AbsDir.X && AbsDir.Y >= AbsDir.Z)
                return (Y > 0 ? AXIS_Y_POSITIVE : AXIS_Y_NEGATIVE);
            
            return (Z > 0 ? AXIS_Z_POSITIVE : AXIS_Z_NEGATIVE);
        }
        
        //! Returns a normal vector to this vector.
        force_inline vector3d<T> getNormal() const
        {
            if (X > Y && X > Z)
                return vector3d<T>(Y, -X, 0);
            else if (Y > X && Y > Z)
                return vector3d<T>(0, Z, -Y);
            return vector3d<T>(-Z, 0, X);
        }
        
        //! Returns the smalest vector component.
        force_inline T getMin() const
        {
            if (X <= Y && X <= Z) return X;
            if (Y <= X && Y <= Z) return Y;
            return Z;
        }
        //! Returns the greatest vector component.
        force_inline T getMax() const
        {
            if (X >= Y && X >= Z) return X;
            if (Y >= X && Y >= Z) return Y;
            return Z;
        }
        
        //! Returns the volume of the bounding box clamped by this vector (X*Y*Z).
        force_inline T getVolume() const
        {
            return X*Y*Z;
        }
        
        /**
        Just returns this vector. This is only required that this call can be used for several templates.
        Write your own vertex class for example and add this function so that it can be used
        for polygon clipping as well.
        Some templates expect a class with this function (e.g. 'CollisionLibrary::clipPolygon').
        \see CollisionLibrary::clipPolygon
        */
        force_inline vector3d<T> getCoord() const
        {
            return *this;
        }
        
        template <typename B> force_inline vector3d<B> cast() const
        {
            return vector3d<B>(static_cast<B>(X), static_cast<B>(Y), static_cast<B>(Z));
        }
        
        static force_inline bool isPointOnSameSide(
            const vector3d<T> &P1, const vector3d<T> &P2, const vector3d<T> &A, const vector3d<T> &B)
        {
            vector3d<T> Difference(B - A);
            vector3d<T> P3 = Difference.cross(P1 - A);
            vector3d<T> P4 = Difference.cross(P2 - A);
            return P3.dot(P4) >= T(0);
        }
        
        /* === Members === */
        
        T X, Y, Z;
        
};

typedef vector3d<int32_t> vector3di;
typedef vector3d<float> vector3df;


/*
 * vector4d class
 */

//! \todo Write this in an own class and don't inherit from "vector3d".
template <typename T> class vector4d : public vector3d<T>
{
    
    public:
        
        force_inline vector4d() : vector3d<T>(), W(1)
        {
        }
        force_inline vector4d(T Size) : vector3d<T>(Size), W(1)
        {
        }
        force_inline vector4d(T VecX, T VecY, T VecZ, T VecW = static_cast<T>(1)) : vector3d<T>(VecX, VecY, VecZ), W(VecW)
        {
        }
        force_inline vector4d(const vector3d<T> &other, T VecW = static_cast<T>(1)) : vector3d<T>(other.X, other.Y, other.Z), W(VecW)
        {
        }
        force_inline vector4d(const vector4d<T> &other) : vector3d<T>(other.X, other.Y, other.Z), W(other.W)
        {
        }
        force_inline ~vector4d()
        {
        }
        
        /* Members */
        
        T W;
        
};

template <typename T> force_inline vector3d<T>::vector3d(const vector4d<T> &Other) :
    X(Other.X),
    Y(Other.Y),
    Z(Other.Z)
{
}

typedef vector4d<int32_t> vector4di;
typedef vector4d<float> vector4df;

// ================================================================================

//! Returns the distance between the two given 3D points.
template <typename T> force_inline T getDistance(const vector3d<T> &PosA, const vector3d<T> &PosB)
{
    return Sqrt(Pow2(PosB.X - PosA.X) + Pow2(PosB.Y - PosA.Y) + Pow2(PosB.Z - PosA.Z));
}

/////////////////////////////////////////////////////////////

//! Relations between closest point on line to point.
enum ELinePointRelations
{
    LINE_RELATION_START,      //!< The closest point on line to point is the line start vector.
    LINE_RELATION_END,        //!< The closest point on line to point is the line end vector.
    LINE_RELATION_BETWEEN,    //!< The closest point on line to point is between the line start- and end vectors.
};


//! 3D line class for intersection tests or 3D drawing information.
template <typename T, template <typename> class Vec> class linekd
{
        
        typedef Vec<T> VecT;
        typedef linekd<T, Vec> L;
        
    public:
        
        force_inline linekd()
        {
        }
        force_inline linekd(const VecT &RayStart, const VecT &RayEnd) :
            Start   (RayStart   ),
            End     (RayEnd     )
        {
        }
        force_inline linekd(const L &Other) :
            Start   (Other.Start),
            End     (Other.End  )
        {
        }
        virtual ~linekd()
        {
        }
        
        /* === Operators === */
        
        force_inline bool operator == (const L &Other)
        {
            return Start == Other.Start && End == Other.End;
        }
        force_inline bool operator != (const L &Other)
        {
            return Start != Other.Start && End != Other.End;
        }
        
        force_inline L operator + (const L &Other) const
        {
            return L(Start + Other.Start, End + Other.End);
        }
        force_inline L& operator += (const L &Other)
        {
            Start += Other.Start; End += Other.End; return *this;
        }
        
        force_inline L operator - (const L &Other) const
        {
            return L(Start - Other.Start, End - Other.End);
        }
        force_inline L& operator -= (const L &Other)
        {
            Start -= Other.Start; End -= Other.End; return *this;
        }
        
        force_inline L operator / (const L &Other) const
        {
            return L(Start / Other.Start, End / Other.End);
        }
        force_inline L& operator /= (const L &Other)
        {
            Start /= Other.Start; End /= Other.End; return *this;
        }
        
        force_inline L operator * (const L &Other) const
        {
            return L(Start * Other.Start, End * Other.End);
        }
        force_inline L& operator *= (const L &Other)
        {
            Start *= Other.Start; End *= Other.End; return *this;
        }
        
        force_inline L operator - () const
        {
            return L(-Start, -End);
        }
        
        /* === Extra functions === */
        
        //! Returns the line's center ((Start + End) / 2).
        force_inline VecT getCenter() const
        {
            return (Start + End) / 2;
        }
        
        //! Returns the line's direction (End - Start).
        force_inline VecT getDirection() const
        {
            return End - Start;
        }
        
        //! Returns this line as vice-versa variant.
        force_inline L getViceVersa() const
        {
            return L(End, Start);
        }
        
        //! Returns true if the two lines (understood as axis-aligned-bounding-boxes) are intersecting.
        force_inline bool checkBoxBoxIntersection(const L &Line) const
        {
            return Start <= Line.End && End >= Line.Start;
        }
        
        //! Returns true if the specified point lies between the line's start and end point.
        force_inline bool isPointInside(const VecT &Point) const
        {
            return Point.isBetweenPoints(Start, End);
        }
        
        //! Returns the closest point on the line between the specfied point and the line.
        force_inline ELinePointRelations getClosestPointStraight(const VecT &Point, VecT &ClosestPoint) const
        {
            VecT Pos(Point - Start);
            VecT Dir(End - Start);
            
            T Len(Dir.getLength());
            Dir *= (T(1) / Len);
            T Factor(Dir.dot(Pos));
            
            Dir *= Factor;
            ClosestPoint = Start + Dir;
            
            if (Factor < T(0))
                return LINE_RELATION_START;
            if (Factor > Len)
                return LINE_RELATION_END;
            
            return LINE_RELATION_BETWEEN;
        }
        
        //! Returns the closest point on the line between the specfied point and the line.
        force_inline VecT getClosestPointStraight(const VecT &Point) const
        {
            VecT ClosestPoint;
            getClosestPointStraight(Point, ClosestPoint);
            return ClosestPoint;
        }
        
        //! Returns the closest point on the line between the specfied point and the line.
        force_inline ELinePointRelations getClosestPoint(const VecT &Point, VecT &ClosestPoint) const
        {
            VecT Pos(Point - Start);
            VecT Dir(End - Start);
            
            T Len(Dir.getLength());
            Dir *= (T(1) / Len);
            T Factor(Dir.dot(Pos));
            
            if (Factor < T(0))
            {
                ClosestPoint = Start;
                return LINE_RELATION_START;
            }
            if (Factor > Len)
            {
                ClosestPoint = End;
                return LINE_RELATION_END;
            }
            
            Dir *= Factor;
            
            ClosestPoint = Start + Dir;
            return LINE_RELATION_BETWEEN;
        }
        
        //! Returns the closest point on the line between the specfied point and the line.
        force_inline VecT getClosestPoint(const VecT &Point) const
        {
            VecT ClosestPoint;
            getClosestPoint(Point, ClosestPoint);
            return ClosestPoint;
        }
        
        //! Returns the distance between the line and the specified point.
        force_inline T getPointDistance(const VecT &Point) const
        {
            return (getClosestPoint(Point) - Point).getLength();
        }
        
        //! Returns the squared distance between the line and the specified point.
        force_inline T getPointDistanceSq(const VecT &Point) const
        {
            return (getClosestPoint(Point) - Point).getLengthSq();
        }
        
        /* Members */
        
        VecT Start, End;
        
};

template <typename T> class line3d : public linekd<T, vector3d>
{
    
    public:
        
        force_inline line3d() :
            linekd<T, vector3d>()
        {
        }
        force_inline line3d(const vector3d<T> &Start, const vector3d<T> &End) :
            linekd<T, vector3d>(Start, End)
        {
        }
        force_inline line3d(const linekd<T, vector3d> &Other) :
            linekd<T, vector3d>(Other)
        {
        }
        force_inline ~line3d()
        {
        }
        
};

template <typename T> class line2d : public linekd<T, point2d>
{
    
    public:
        
        force_inline line2d() :
            linekd<T, point2d>()
        {
        }
        force_inline line2d(const point2d<T> &Start, const point2d<T> &End) :
            linekd<T, point2d>(Start, End)
        {
        }
        force_inline line2d(const linekd<T, point2d> &Other) :
            linekd<T, point2d>(Other)
        {
        }
        force_inline ~line2d()
        {
        }
        
};

typedef linekd<int32_t, vector3d> line3di;
typedef linekd<float, vector3d> line3df;

typedef linekd<int32_t, point2d> line2di;
typedef linekd<float, point2d> line2df;

/////////////////////////////////////////////////////////////

template < typename T, class C = vector3d<T> > class triangle3d
{
    
    public:
        
        force_inline triangle3d()
        {
        }
        force_inline triangle3d(const C &NewPointA, const C &NewPointB, const C &NewPointC) :
            PointA(NewPointA),
            PointB(NewPointB),
            PointC(NewPointC)
        {
        }
        force_inline triangle3d(const triangle3d<T> &Other) :
            PointA(Other.PointA),
            PointB(Other.PointB),
            PointC(Other.PointC)
        {
        }
        force_inline triangle3d(const triangle3d<T, C*> &Other) :
            PointA(*Other.PointA),
            PointB(*Other.PointB),
            PointC(*Other.PointC)
        {
        }
        force_inline ~triangle3d()
        {
        }
        
        /* === Operators === */
        
        force_inline triangle3d<T>& operator = (const triangle3d<T, C*> &other)
        {
            PointA = *other.PointA; PointB = *other.PointB; PointC = *other.PointC;
            return *this;
        }
        
        force_inline triangle3d<T> operator + (const vector3d<T> &Vector) const
        {
            return triangle3d<T>(PointA + Vector, PointB + Vector, PointC + Vector);
        }
        force_inline triangle3d<T>& operator += (const vector3d<T> &Vector)
        {
            PointA += Vector; PointB += Vector; PointC += Vector; return *this;
        }
        
        force_inline triangle3d<T> operator - (const vector3d<T> &Vector) const
        {
            return triangle3d<T>(PointA - Vector, PointB - Vector, PointC - Vector);
        }
        force_inline triangle3d<T>& operator -= (const vector3d<T> &Vector)
        {
            PointA -= Vector; PointB -= Vector; PointC -= Vector; return *this;
        }
        
        force_inline triangle3d<T> operator * (const vector3d<T> &Vector) const
        {
            return triangle3d<T>(PointA*Vector, PointB*Vector, PointC*Vector);
        }
        force_inline triangle3d<T>& operator *= (const vector3d<T> &Vector)
        {
            PointA *= Vector; PointB *= Vector; PointC *= Vector; return *this;
        }
        
        force_inline triangle3d<T> operator / (const vector3d<T> &Vector) const
        {
            return triangle3d<T>(PointA/Vector, PointB/Vector, PointC/Vector);
        }
        force_inline triangle3d<T>& operator /= (const vector3d<T> &Vector)
        {
            PointA /= Vector; PointB /= Vector; PointC /= Vector; return *this;
        }
        
        force_inline triangle3d<T> operator - () const
        {
            return triangle3d<T>(-PointA, -PointB, -PointC);
        }
        
        /* === Additional operators === */
        
        force_inline const vector3d<T> operator [] (int32_t i) const
        {
            switch (i)
            {
                case 0: return PointA;
                case 1: return PointB;
                case 2: return PointC;
            }
            return vector3d<T>();
        }
        
        force_inline vector3d<T>& operator [] (int32_t i)
        {
            return *(&PointA + i);
        }
        
        /* === Extra functions === */
        
        //! Returns the unnormalized normal vector.
        force_inline vector3d<T> getNormalSq() const
        {
            return (PointB - PointA).cross(PointC - PointA);
        }
        //! Returns the normal vector.
        force_inline vector3d<T> getNormal() const
        {
            return getNormalSq().normalize();
        }
        
        //! Returns the triangle's center point.
        force_inline vector3d<T> getCenter() const
        {
            return (PointA + PointB + PointC) / 3;
        }
        
        //! Returns the triangle's area.
        force_inline T getArea() const
        {
            return getNormalSq().getLength() / 2;
        }
        
        force_inline bool isPointInside(const vector3d<T> &Vector) const
        {
            return
                vector3d<T>::isPointOnSameSide(Vector, PointA, PointB, PointC) &&
                vector3d<T>::isPointOnSameSide(Vector, PointB, PointA, PointC) &&
                vector3d<T>::isPointOnSameSide(Vector, PointC, PointA, PointB);
        }
        
        force_inline bool isFrontSide() const
        {
            return (
                (PointB.X - PointA.X)*(PointC.Y - PointA.Y) - (PointB.Y - PointA.Y)*(PointC.X - PointA.X)
            ) >= (T)0;
        }
        
        /**
        Returns the point on the triangles given by the barycentric coordinate.
        \param[in] Coord Specifies the barycentric coordinate. This vector must be
        normalized if the resulting point is used to be inside the triangle.
        \return Coordinate vector in cartesian coordinates lying onto the triangle.
        */
        force_inline C getBarycentricPoint(const vector3d<T> &Coord) const
        {
            return PointA*Coord.X + PointB*Coord.Y + PointC*Coord.Z;
        }
        
        force_inline triangle3d<T> getSwaped() const
        {
            return triangle3d<T>(PointC, PointB, PointA);
        }
        force_inline triangle3d<T>& swap()
        {
            Swap(PointA, PointC);
            return *this;
        }
        
        force_inline line3d<T> getBox() const
        {
            line3d<T> TriangleBox;
            
            TriangleBox.Start.X = Min(PointA.X, PointB.X, PointC.X);
            TriangleBox.Start.Y = Min(PointA.Y, PointB.Y, PointC.Y);
            TriangleBox.Start.Z = Min(PointA.Z, PointB.Z, PointC.Z);
            
            TriangleBox.End.X = Max(PointA.X, PointB.X, PointC.X);
            TriangleBox.End.Y = Max(PointA.Y, PointB.Y, PointC.Y);
            TriangleBox.End.Z = Max(PointA.Z, PointB.Z, PointC.Z);
            
            return TriangleBox;
        }
        
        force_inline bool equal(const triangle3d<T> &other, float Precision = ROUNDING_ERROR) const
        {
            return
                PointA.equal(other.PointA, Precision) &&
                PointB.equal(other.PointB, Precision) &&
                PointC.equal(other.PointC, Precision);
        }
        force_inline bool empty() const
        {
            return PointA.empty() && PointB.empty() && PointC.empty();
        }
        
        force_inline bool isFaceVisible() const
        {
            return (PointB.X - PointA.X)*(PointC.Y - PointA.Y) - (PointB.Y - PointA.Y)*(PointC.X - PointA.X) >= 0.0f;
        }
        
        template <typename T2, class C2> force_inline triangle3d<T2, C2> cast() const
        {
            triangle3d<T2, C2> Tri;
            
            Tri.PointA = PointA.cast<T2>();
            Tri.PointB = PointB.cast<T2>();
            Tri.PointC = PointC.cast<T2>();
            
            return Tri;
        }
        
        /* === Members === */
        
        C PointA, PointB, PointC;
        
};

typedef triangle3d<int32_t> triangle3di;
typedef triangle3d<float> triangle3df;

typedef triangle3d<int32_t, vector3di*> ptriangle3di;
typedef triangle3d<float, vector3df*> ptriangle3df;

/////////////////////////////////////////////////////////////

//! Quadrangle 3D class which is basically only used for collision detection.
template <typename T> class quadrangle3d
{
    
    public:
        
        force_inline quadrangle3d()
        {
        }
        force_inline quadrangle3d(const vector3d<T> &A, const vector3d<T> &B, const vector3d<T> &C, const vector3d<T> &D) :
            PointA(A),
            PointB(B),
            PointC(C),
            PointD(D)
        {
        }
        force_inline quadrangle3d(const quadrangle3d<T> &Other) :
            PointA(Other.PointA),
            PointB(Other.PointB),
            PointC(Other.PointC),
            PointD(Other.PointD)
        {
        }
        force_inline ~quadrangle3d()
        {
        }
        
        /* === Operators === */
        
        force_inline void operator = (const quadrangle3d<T> &Other)
        {
            PointA = Other.PointA;
            PointB = Other.PointB;
            PointC = Other.PointC;
            PointD = Other.PointD;
        }
        
        /* === Additional operators === */
        
        force_inline const vector3d<T> operator [] (int32_t i) const
        {
            switch (i)
            {
                case 0: return PointA;
                case 1: return PointB;
                case 2: return PointC;
                case 3: return PointD;
            }
            return vector3d<T>();
        }
        
        force_inline vector3d<T>& operator [] (int32_t i)
        {
            return *(&PointA + i);
        }
        
        /* === Extra functions === */
        
        force_inline vector3d<T> getNormal() const
        {
            return vector3d<T>( (PointB - PointA).cross(PointC - PointA) ).normalize();
        }
        
        force_inline vector3d<T> getCenter() const
        {
            return (PointA + PointB + PointC + PointD) / 4;
        }
        
        force_inline T getArea() const
        {
            return vector3d<T>( (PointB - PointA).cross(PointC - PointA) ).getLength();
        }
        
        //! \todo This has not been tested yet!
        force_inline bool isPointInside(const vector3d<T> &Vector) const
        {
            return
                ( vector3d<T>::isPointOnSameSide(Vector, PointA, PointB, PointC) &&
                  vector3d<T>::isPointOnSameSide(Vector, PointB, PointA, PointC) &&
                  vector3d<T>::isPointOnSameSide(Vector, PointC, PointA, PointB) ) ||
                ( vector3d<T>::isPointOnSameSide(Vector, PointA, PointC, PointD) &&
                  vector3d<T>::isPointOnSameSide(Vector, PointC, PointA, PointD) &&
                  vector3d<T>::isPointOnSameSide(Vector, PointD, PointA, PointC) );
        }
        
        force_inline quadrangle3d<T> getSwaped() const
        {
            return quadrangle3d<T>(PointA, PointD, PointC, PointB);
        }
        force_inline quadrangle3d<T>& swap()
        {
            Swap(PointB, PointD);
            return *this;
        }
        
        force_inline bool equal(const quadrangle3d<T> &other, float Precision = ROUNDING_ERROR) const
        {
            return
                PointA.equal(other.PointA, Precision) &&
                PointB.equal(other.PointB, Precision) &&
                PointC.equal(other.PointC, Precision) &&
                PointD.equal(other.PointC, Precision);
        }
        force_inline bool empty() const
        {
            return PointA.empty() && PointB.empty() && PointC.empty() && PointD.empty();
        }
        
        template <typename B> force_inline quadrangle3d<B> cast() const
        {
            return triangle3d<B>(
                PointA.cast<B>(), PointB.cast<B>(), PointC.cast<B>(), PointD.cast<B>()
            );
        }
        
        /* Members */
        
        vector3d<T> PointA, PointB, PointC, PointD;
        
};

typedef quadrangle3d<int32_t> quadrangle3di;
typedef quadrangle3d<float> quadrangle3df;

/////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////

template <typename T> class plane3d;


#define DefineConstOperator(op)                                 \
    force_inline aabbox3d<T> operator op (const aabbox3d<T> &other) const    \
    {                                                           \
        return aabbox3d<T>(Min op other.Min, Max op other.Max); \
    }

#define DefineOperator(op)                              \
    force_inline aabbox3d<T>& operator op (const aabbox3d<T> &other) \
    {                                                   \
        Min op other.Min;                               \
        Max op other.Max;                               \
        return *this;                                   \
    }


//! Axis aligned bounding box (AABB) class for bounding volume tests.
template <typename T> class aabbox3d
{

public:

	force_inline aabbox3d()
	{
	}
	force_inline aabbox3d(const vector3d<T> &MinEdge, const vector3d<T> &MaxEdge) :
		Min(MinEdge),
		Max(MaxEdge)
	{
	}
	force_inline aabbox3d(const line3d<T> &Line) :
		Min(Line.Start),
		Max(Line.End)
	{
	}
	force_inline aabbox3d(const aabbox3d<T> &Other) :
		Min(Other.Min),
		Max(Other.Max)
	{
	}
	force_inline ~aabbox3d()
	{
	}

	/* === Operators === */

	DefineConstOperator(*)
		DefineConstOperator(/ )
		DefineConstOperator(+)
		DefineConstOperator(-)

		DefineOperator(*= )
		DefineOperator(/= )
		DefineOperator(+= )
		DefineOperator(-= )

		/* === Extra functions === */

		//! Returns the center of the bounding box.
		force_inline vector3d<T> getCenter() const
	{
		return (Min + Max) / 2;
	}

	//! Returns the size of the bounding box.
	force_inline vector3d<T> getSize() const
	{
		return Max - Min;
	}

	//! Returns the volume of this bounding box.
	force_inline T getVolume() const
	{
		return getSize().getVolume();
	}

	//! Returns true if the two AABBs intersects.
	force_inline bool checkBoxBoxIntersection(const aabbox3d<T> &other) const
	{
		return Min <= other.Max && Max >= other.Min;
	}

	//! Returns true if the given AABB is completely inside this box.
	force_inline bool isBoxInside(const aabbox3d<T> &other) const
	{
		return Min >= other.Min && Max <= other.Max;
	}

	//! Returns true if the specified point is inside the box.
	force_inline bool isPointInside(const vector3d<T> &Point) const
	{
		return (
			Point.X >= Min.X && Point.Y >= Min.Y && Point.Z >= Min.Z &&
			Point.X <= Max.X && Point.Y <= Max.Y && Point.Z <= Max.Z
			);
	}

	//! Returns true if minimum and maximum are equal.
	force_inline bool empty(float Tolerance = ROUNDING_ERROR) const
	{
		return Min.equal(Max, Tolerance);
	}

	//! Returns true if this is a valid box.
	force_inline bool valid() const
	{
		return
			Max.X >= Min.X &&
			Max.Y >= Min.Y &&
			Max.Z >= Min.Z;
	}

	//! Repairs the bounding box if any component of "Min" is greater then the corresponding component of "Max".
	force_inline aabbox3d<T>& repair()
	{
		if (Min.X > Max.X) Swap(Min.X, Max.X);
		if (Min.Y > Max.Y) Swap(Min.Y, Max.Y);
		if (Min.Z > Max.Z) Swap(Min.Z, Max.Z);
		return *this;
	}

	//! Inserts a point to the bounding box. This can result in that the box becomes larger.
	force_inline void insertPoint(const vector3d<T> &Point)
	{
		if (Point.X > Max.X) Max.X = Point.X;
		if (Point.Y > Max.Y) Max.Y = Point.Y;
		if (Point.Z > Max.Z) Max.Z = Point.Z;

		if (Point.X < Min.X) Min.X = Point.X;
		if (Point.Y < Min.Y) Min.Y = Point.Y;
		if (Point.Z < Min.Z) Min.Z = Point.Z;
	}

	//! Inserts the min- and max points of the given box to this bounding box. This can result in that the box becomes larger.
	force_inline void insertBox(const aabbox3d<T> &Other)
	{
		insertPoint(Other.Min);
		insertPoint(Other.Max);
	}

	//! Returns the bounding box's volume.
	force_inline T getBoxVolume() const
	{
		return
			(Max.X - Min.X).getAbs() *
			(Max.Y - Min.Y).getAbs() *
			(Max.Z - Min.Z).getAbs();
	}

	//! Returns a copy of this box multiplied by the specified size.
	force_inline aabbox3d<T> getScaled(const vector3d<T> &Size) const
	{
		return aabbox3d<T>(Min * Size, Max * Size);
	}

	/**
	Returns the maximal absolute vector of min and max components.
	\code
	aabbox3df(vector3df(-4, -2, -3), vector3df(1, 5, 6)).getMaxRadius() == vector3df(4, 5, 6);
	\endcode
	*/
	force_inline vector3d<T> getMaxRadius() const
	{
		const vector3d<T> AbsMin(Min.getAbs());
		const vector3d<T> AbsMax(Max.getAbs());
		return vector3d<T>(
			Max(AbsMin.X, AbsMax.X),
			Max(AbsMin.Y, AbsMax.Y),
			Max(AbsMin.Z, AbsMax.Z)
			);
	}

	force_inline plane3d<T> getLeftPlane() const;
	force_inline plane3d<T> getRightPlane() const;
	force_inline plane3d<T> getTopPlane() const;
	force_inline plane3d<T> getBottomPlane() const;
	force_inline plane3d<T> getFrontPlane() const;
	force_inline plane3d<T> getBackPlane() const;

	plane3d<T> getPlane(uint32_t Index) const
	{
		switch (Index)
		{
		case 0: return getLeftPlane();
		case 1: return getRightPlane();
		case 2: return getTopPlane();
		case 3: return getBottomPlane();
		case 4: return getFrontPlane();
		case 5: return getBackPlane();
		}
		return plane3d<T>();
	}

	force_inline quadrangle3d<T> getLeftQuad() const
	{
		return quadrangle3d<T>(
			vector3d<T>(Min.X, Min.Y, Max.Z),
			vector3d<T>(Min.X, Max.Y, Max.Z),
			vector3d<T>(Min.X, Max.Y, Min.Z),
			vector3d<T>(Min.X, Min.Y, Min.Z)
			);
	}
	force_inline quadrangle3d<T> getRightQuad() const
	{
		return quadrangle3d<T>(
			vector3d<T>(Max.X, Min.Y, Min.Z),
			vector3d<T>(Max.X, Max.Y, Min.Z),
			vector3d<T>(Max.X, Max.Y, Max.Z),
			vector3d<T>(Max.X, Min.Y, Max.Z)
			);
	}
	force_inline quadrangle3d<T> getTopQuad() const
	{
		return quadrangle3d<T>(
			vector3d<T>(Min.X, Max.Y, Min.Z),
			vector3d<T>(Min.X, Max.Y, Max.Z),
			vector3d<T>(Max.X, Max.Y, Max.Z),
			vector3d<T>(Max.X, Max.Y, Min.Z)
			);
	}
	force_inline quadrangle3d<T> getBottomQuad() const
	{
		return quadrangle3d<T>(
			vector3d<T>(Min.X, Min.Y, Max.Z),
			vector3d<T>(Min.X, Min.Y, Min.Z),
			vector3d<T>(Max.X, Min.Y, Min.Z),
			vector3d<T>(Max.X, Min.Y, Max.Z)
			);
	}
	force_inline quadrangle3d<T> getFrontQuad() const
	{
		return quadrangle3d<T>(
			vector3d<T>(Min.X, Min.Y, Min.Z),
			vector3d<T>(Min.X, Max.Y, Min.Z),
			vector3d<T>(Max.X, Max.Y, Min.Z),
			vector3d<T>(Max.X, Min.Y, Min.Z)
			);
	}
	force_inline quadrangle3d<T> getBackQuad() const
	{
		return quadrangle3d<T>(
			vector3d<T>(Max.X, Min.Y, Max.Z),
			vector3d<T>(Max.X, Max.Y, Max.Z),
			vector3d<T>(Min.X, Max.Y, Max.Z),
			vector3d<T>(Min.X, Min.Y, Max.Z)
			);
	}

	/**
	Returns a corner of this box.
	\param Index: Specifies the corners index. This must be a value in the range [0 .. 7].
	\return 3D vector containing the corner coordinate.
	*/
	force_inline vector3d<T> getCorner(uint32_t Index) const
	{
		switch (Index)
		{
		case 0: return vector3d<T>(Min.X, Min.Y, Min.Z);
		case 1: return vector3d<T>(Max.X, Min.Y, Min.Z);
		case 2: return vector3d<T>(Min.X, Max.Y, Min.Z);
		case 3: return vector3d<T>(Max.X, Max.Y, Min.Z);
		case 4: return vector3d<T>(Min.X, Min.Y, Max.Z);
		case 5: return vector3d<T>(Max.X, Min.Y, Max.Z);
		case 6: return vector3d<T>(Min.X, Max.Y, Max.Z);
		case 7: return vector3d<T>(Max.X, Max.Y, Max.Z);
		}
		return T(0);
	}

	force_inline line3d<T> getEdge(uint32_t Index) const
	{
		switch (Index)
		{
		case  0: return line3d<T>(vector3d<T>(Min.X, Min.Y, Min.Z), vector3d<T>(Max.X, Min.Y, Min.Z));
		case  1: return line3d<T>(vector3d<T>(Min.X, Max.Y, Min.Z), vector3d<T>(Max.X, Max.Y, Min.Z));
		case  2: return line3d<T>(vector3d<T>(Min.X, Max.Y, Max.Z), vector3d<T>(Max.X, Max.Y, Max.Z));
		case  3: return line3d<T>(vector3d<T>(Min.X, Min.Y, Max.Z), vector3d<T>(Max.X, Min.Y, Max.Z));

		case  4: return line3d<T>(vector3d<T>(Min.X, Min.Y, Min.Z), vector3d<T>(Min.X, Max.Y, Min.Z));
		case  5: return line3d<T>(vector3d<T>(Max.X, Min.Y, Min.Z), vector3d<T>(Max.X, Max.Y, Min.Z));
		case  6: return line3d<T>(vector3d<T>(Max.X, Min.Y, Max.Z), vector3d<T>(Max.X, Max.Y, Max.Z));
		case  7: return line3d<T>(vector3d<T>(Min.X, Min.Y, Max.Z), vector3d<T>(Min.X, Max.Y, Max.Z));

		case  8: return line3d<T>(vector3d<T>(Min.X, Min.Y, Min.Z), vector3d<T>(Min.X, Min.Y, Max.Z));
		case  9: return line3d<T>(vector3d<T>(Max.X, Min.Y, Min.Z), vector3d<T>(Max.X, Min.Y, Max.Z));
		case 10: return line3d<T>(vector3d<T>(Max.X, Max.Y, Min.Z), vector3d<T>(Max.X, Max.Y, Max.Z));
		case 11: return line3d<T>(vector3d<T>(Min.X, Max.Y, Min.Z), vector3d<T>(Min.X, Max.Y, Max.Z));
		}
		return line3d<T>();
	}

	//! Returns the closest point on the plane.
	force_inline vector3d<T> getClosestPoint(const plane3d<T> &Plane) const;

	/* === Members === */

	vector3d<T> Min; //!< Left-lower-front corner of the bounding box.
	vector3d<T> Max; //!< Right-upper-back corner of the bounding box.

					 /* === Macros === */

	static const aabbox3d<T> OMEGA;     //!< Largest invalid bounding box (999999 to -999999).
	static const aabbox3d<T> IDENTITY;  //!< Identity bounding box (-1 to 1).
	static const aabbox3d<T> CUBE;      //!< Default cube with a size of 1 (-0.5 to 0.5).

};

template <typename T> const aabbox3d<T> aabbox3d<T>::OMEGA(OMEGA, -OMEGA);
template <typename T> const aabbox3d<T> aabbox3d<T>::IDENTITY(T(-1), T(1));
template <typename T> const aabbox3d<T> aabbox3d<T>::CUBE(T(-0.5), T(0.5));

typedef aabbox3d<int32_t> aabbox3di;
typedef aabbox3d<float> aabbox3df;


#undef DefineConstOperator
#undef DefineOperator


/////////////////////////////////////////////////////////////

template <typename T> class obbox3d
{

public:

	force_inline obbox3d()
	{
	}
	force_inline obbox3d(const vector3d<T> &Min, const vector3d<T> &Max) :
		Center((Min + Max) / 2),
		Axis(
			vector3d<T>(1, 0, 0),
			vector3d<T>(0, 1, 0),
			vector3d<T>(0, 0, 1)
			),
		HalfSize((Max - Min) / 2)
	{
	}
	force_inline obbox3d(const vector3d<T> &BoxCenter, const vector3d<T> &AxisX, const vector3d<T> &AxisY, const vector3d<T> &AxisZ) :
		Center(BoxCenter),
		Axis(AxisX, AxisY, AxisZ)
	{
		updateHalfSize();
	}
	force_inline obbox3d(const obbox3d<T> &Other) :
		Center(Other.Center),
		Axis(Other.Axis),
		HalfSize(Other.HalfSize)
	{
	}
	force_inline ~obbox3d()
	{
	}

	/* === Extra functions === */

	force_inline void updateHalfSize()
	{
		/* Store half size */
		HalfSize.X = Axis.X.getLength();
		HalfSize.Y = Axis.Y.getLength();
		HalfSize.Z = Axis.Z.getLength();

		/* Normalize axes */
		Axis.X *= (T(1) / HalfSize.X);
		Axis.Y *= (T(1) / HalfSize.Y);
		Axis.Z *= (T(1) / HalfSize.Z);
	}

	force_inline T getVolume() const
	{
		return (HalfSize * vector3d<T>(2)).getVolume();
	}

	/**
	Checks if the specified point is inside the box.
	\param[in] InvPoint Specifies the inverse point which must be transformed by the inverse matrix of this box.
	\return True if the specified inverse point is inside the box.
	*/
	force_inline bool isInversePointInside(const vector3d<T> &InvPoint) const
	{
		return
			Abs(InvPoint.X) < T(1) &&
			Abs(InvPoint.Y) < T(1) &&
			Abs(InvPoint.Z) < T(1);
	}

	//! Returns true if the specified point is inside the box.
	force_inline bool isPointInside(const vector3d<T> &Point) const;

	//! Returns true if the specifies box is inside this box.
	force_inline bool isBoxInside(const obbox3d<T> &Other) const;

	/* Members */

	vector3d<T> Center;
	vector3d< vector3d<T> > Axis;
	vector3d<T> HalfSize; // (HalfSize.X is the length of Axis.X vector)

};

typedef obbox3d<int32_t> obbox3di;
typedef obbox3d<float> obbox3df;


/////////////////////////////////////////////////////////////


//! Relations between plane and AABB.
enum EPlaneAABBRelations
{
    PLANE_RELATION_FRONT,
    PLANE_RELATION_BACK,
    PLANE_RELATION_CLIPPED,
};

//! Relations between plane and point.
enum EPlanePointRelations
{
    POINT_INFRONTOF_PLANE,
    POINT_BEHIND_PLANE,
    POINT_ON_PLANE,
};

template <typename T> class aabbbox3d;

template <typename T> class plane3d
{
    
    public:
        
        force_inline plane3d() :
            Distance(0)
        {
        }
        force_inline plane3d(const vector3d<T> &PlaneNormal, const T Dist) :
            Normal  (PlaneNormal),
            Distance(Dist       )
        {
        }
        force_inline plane3d(const vector3d<T> &PointA, const vector3d<T> &PointB, const vector3d<T> &PointC) :
            Distance(0)
        {
            computePlane(PointA, PointB, PointC);
        }
        force_inline plane3d(const triangle3d<T> &Triangle) :
            Distance(0)
        {
            computePlane(Triangle.PointA, Triangle.PointB, Triangle.PointC);
        }
        force_inline plane3d(const quadrangle3d<T> &Quadrangle) :
            Distance(0)
        {
            computePlane(Quadrangle.PointA, Quadrangle.PointB, Quadrangle.PointC);
        }
        force_inline plane3d(const plane3d<T> &Other) :
            Normal  (Other.Normal   ),
            Distance(Other.Distance )
        {
        }
        force_inline ~plane3d()
        {
        }
        
        /* === Extra functions === */
        
        force_inline void computePlane(
            const vector3d<T> &PointA, const vector3d<T> &PointB, const vector3d<T> &PointC)
        {
            // Normal := || (PointB - PointA) x (PointC - PointA) ||
            Normal = PointB;
            Normal -= PointA;
            Normal = Normal.cross(PointC - PointA);
            Normal.normalize();
            
            Distance = Normal.dot(PointA);
        }
        
        //! \todo Try to generalize this with the 'CollisionLibrary::getLinePlaneIntersection' function.
        force_inline bool checkLineIntersection(
            const vector3d<T> &LineStart, const vector3d<T> &LineEnd, vector3d<T> &Intersection) const
        {
            vector3d<T> Direction(LineEnd);
            Direction -= LineStart;
            
            const T t = (Distance - Normal.dot(LineStart)) / Normal.dot(Direction);
            
            if (t >= T(0) && t <= T(1))
            {
                // Intersection := LineStart + Direction * t
                Intersection = Direction;
                Intersection *= t;
                Intersection += LineStart;
                return true;
            }
            
            return false;
        }
        
        force_inline bool checkPlaneIntersection(
            const plane3d<T> &Other, vector3d<T> &Intersection, vector3d<T> &Direction) const
        {
            /* Compute the direction of the intersection line */
            Direction = Normal.cross(Other.Normal);
            
            /*
             * If d is 0 (zero), the planes are parallel (ans separated)
             * or coincident, so they are not considered intersecting
             */
            const T Denom = Direction.dot(Direction);
            
            if (Denom < ROUNDING_ERROR)
                return false;
            
            /* Compute point on intersection line */
            // Intersection := ( (Other.Normale * Distance - Normal * Other.Distance) x (Direction) ) / Denom
            Intersection = Other.Normal;
            Intersection *= Distance;
            Intersection -= (Normal * Other.Distace);
            Intersection = Intersection.cross(Direction) / Denom;
            
            return true;
        }
        
        force_inline bool checkMultiplePlaneIntersection(
            const plane3d<T> &Plane1, const plane3d<T> &Plane2, vector3d<T> &Intersection) const
        {
            vector3d<T> u = Plane1.Normal.cross(Plane2.Normal);
            const T Denom = Normal.dot(u);
            
            /* Check if the planes intersect in a point */
            if (Abs(Denom) < ROUNDING_ERROR)
                return false;
            
            /* Compute intersection point */
            Intersection = (u * Distance + Normal.cross(Plane1.Normal * Plane2.Distance - Plane2.Normal * Plane1.Distance)) / Denom;
            
            return true;
        }

		force_inline T getAABBoxDistance(const aabbox3d<T> &Box) const
		{
			/* These two lines not necessary with a (center, extents) AABB representation */
			vector3d<T> c(Box.getCenter()); // Compute AABB center
			vector3d<T> e(Box.Max - c);     // Compute positive extents

											/* Compute the projection interval readius of b onto L(t) = b.c + t * p.n */
			const T r = e.X * Abs(Normal.X) + e.Y * Abs(Normal.Y) + e.Z * Abs(Normal.Z);

			/* Compute distance of box center from plane */
			const T s = Normal.dot(c) - Distance;

			/* Intersection occurs when distance s falls within [-r, +r] interval */
			return Abs(s) - r;
		}

        force_inline T getOBBoxDistance(const obbox3d<T> &Box) const
        {
            /* Compute the projection interval radius of box */
            const T r = (
                Box.HalfSize.X * Abs(Normal.dot(Box.Axis.X)) +
                Box.HalfSize.Y * Abs(Normal.dot(Box.Axis.Y)) +
                Box.HalfSize.Z * Abs(Normal.dot(Box.Axis.Z))
            );
            
            /* Compute distance of box center from plane */
            const T s = Normal.dot(Box.Center) - Distance;
            
            /* Intersection occurs when distance s falls within [-r, +r] interval */
            return Abs(s) - r;
        }
        
        force_inline bool checkAABBoxIntersection(const aabbox3d<T> &Box) const
        {
            return getAABBoxDistance(Box) <= T(0);
        }
        
        force_inline bool checkOBBoxIntersection(const obbox3d<T> &Box) const
        {
            return getOBBoxDistance(Box) <= T(0);
        }
        
        force_inline EPlaneAABBRelations getAABBoxRelation(const aabbox3d<T> &Box) const
        {
            vector3d<T> NearPoint(Box.Max);
            vector3d<T> FarPoint(Box.Min);
            
            if (Normal.X > T(0))
            {
                NearPoint.X = Box.Min.X;
                FarPoint.X  = Box.Max.X;
            }
            if (Normal.Y > T(0))
            {
                NearPoint.Y = Box.Min.Y;
                FarPoint.Y  = Box.Max.Y;
            }
            if (Normal.Z > T(0))
            {
                NearPoint.Z = Box.Min.Z;
                FarPoint.Z  = Box.Max.Z;
            }
            
            if (isPointFrontSide(NearPoint))
                return PLANE_RELATION_FRONT;
            if (isPointFrontSide(FarPoint))
                return PLANE_RELATION_CLIPPED;
            
            return PLANE_RELATION_BACK;
        }
        
        force_inline EPlanePointRelations getPointRelation(const vector3d<T> &Point) const
        {
            const T Dist = Normal.dot(Point) - Distance;
            
            if (Dist > ROUNDING_ERROR)
                return POINT_INFRONTOF_PLANE;
            if (Dist < -ROUNDING_ERROR)
                return POINT_BEHIND_PLANE;
            return POINT_ON_PLANE;
        }
        
        force_inline T getPointDistance(const vector3d<T> &Point) const
        {
            return (Normal.dot(Point) - Distance) / Normal.dot(Normal);
        }
        
        //! Returns the closest point onto the plane from the plane to the specified point.
        force_inline vector3d<T> getClosestPoint(const vector3d<T> &Point) const
        {
            return Point - Normal * getPointDistance(Point);
        }
        
        /**
        Returns the closest point onto the plane from the plane to the specified point.
        This function is a little bit faster than the "getClosestPoint" function but the plane's normal must be normalized.
        \see getClosestPoint
        */
        force_inline vector3d<T> getClosestPointNormalized(const vector3d<T> &Point) const
        {
            return Point - Normal * ( Normal.dot(Point) - Distance );
        }
        
        //! Returns a point which lies onto the plane.
        force_inline vector3d<T> getMemberPoint() const
        {
            return Normal * Distance;
        }
        
        //! Returns true if the specified point lies on the front plane's side.
        force_inline bool isPointFrontSide(const vector3d<T> &Point) const
        {
            return getPointDistance(Point) >= 0;
        }
        
        force_inline bool equal(const plane3d<T> &Other, float Precision = ROUNDING_ERROR) const
        {
            return
                Normal.equal(Other.Normal, Precision) &&
                (Distance + Precision > Other.Distance) &&
                (Distance - Precision < Other.Distance);
        }
        
        force_inline plane3d<T>& swap()
        {
            Normal = -Normal;
            Distance = -Distance;
            return *this;
        }
        
        force_inline plane3d<T>& normalize()
        {
            const T Len = 1.0f / Normal.getLength();
            
            Normal *= Len;
            Distance *= Len;
            
            return *this;
        }
        
        template <typename B> force_inline vector3d<B> cast() const
        {
            return plane3d<B>(Normal.cast<B>(), static_cast<B>(Distance));
        }
        
        /* Members */
        
        vector3d<T> Normal;
        T Distance;
        
};

typedef plane3d<int32_t> plane3di;
typedef plane3d<float> plane3df;

/////////////////////////////////////////////////////////////
#pragma once

template <typename T> class matrix2
{
    
    public:
        
        /* === Constructors === */
        
        force_inline matrix2()
        {
            reset();
        }
        force_inline matrix2(const T (&other)[4])
        {
            *this = other;
        }
        force_inline matrix2(const matrix2<T> &other)
        {
            *this = other;
        }
        force_inline matrix2(
            T m1n1, T m2n1,
            T m1n2, T m2n2)
        {
            M[0] = m1n1; M[2] = m2n1;
            M[1] = m1n2; M[3] = m2n2;
        }
        force_inline ~matrix2()
        {
        }
        
        /* === Operators === */
        
        force_inline const T operator () (uint32_t row, uint32_t col) const
        {
            return row < 2 && col < 2 ? M[(row << 1) + col] : (T)0;
        }
        force_inline T& operator () (uint32_t row, uint32_t col)
        {
            return M[(row << 1) + col];
        }
        
        force_inline const T operator [] (uint32_t i) const
        {
            return i < 4 ? M[i] : (T)0;
        }
        force_inline T& operator [] (uint32_t i)
        {
            return M[i];
        }
        
        force_inline bool operator == (const matrix2<T> &other)
        {
            for (int32_t i = 0; i < 4; ++i)
            {
                if (M[i] != other.M[i])
                    return false;
            }
            return true;
        }
        force_inline bool operator != (const matrix2<T> &other)
        {
            for (int32_t i = 0; i < 4; ++i)
            {
                if (M[i] != other.M[i])
                    return true;
            }
            return false;
        }
        
        force_inline matrix2<T>& operator = (const T (&other)[4])
        {
            M[0] = other[0]; M[2] = other[2];
            M[1] = other[1]; M[3] = other[3];
            return *this;
        }
        force_inline matrix2<T>& operator = (const matrix2<T> &other)
        {
            M[0] = other[0]; M[2] = other[2];
            M[1] = other[1]; M[3] = other[3];
            return *this;
        }
        force_inline matrix2<T>& operator = (T Scalar)
        {
            memset(M, Scalar, sizeof(M));
            return *this;
        }
        
        force_inline matrix2<T> operator + (const matrix2<T> &mltMatrix) const
        {
            matrix2<T> other;
            
            other[0] = M[0] + mltMatrix[0]; other[2] = M[2] + mltMatrix[2];
            other[1] = M[1] + mltMatrix[1]; other[3] = M[3] + mltMatrix[3];
            
            return other;
        }
        
        force_inline matrix2<T>& operator += (const matrix2<T> &mltMatrix)
        {
            M[0] += mltMatrix[0]; M[2] += mltMatrix[2];
            M[1] += mltMatrix[1]; M[3] += mltMatrix[3];
            return *this;
        }
        
        force_inline matrix2<T> operator - (const matrix2<T> &mltMatrix) const
        {
            matrix2<T> other;
            other[0] = M[0] - mltMatrix[0]; other[2] = M[2] - mltMatrix[2];
            other[1] = M[1] - mltMatrix[1]; other[3] = M[3] - mltMatrix[3];
            return other;
        }
        force_inline matrix2<T>& operator -= (const matrix2<T> &mltMatrix)
        {
            M[0] -= mltMatrix[0]; M[2] -= mltMatrix[2];
            M[1] -= mltMatrix[1]; M[3] -= mltMatrix[3];
            return *this;
        }
        
        force_inline matrix2<T> operator * (const matrix2<T> &mltMatrix) const
        {
            matrix2<T> m3;
            const T* m1 = M;
            const T* m2 = mltMatrix.M;
            
            m3[0] = m1[0]*m2[0] + m1[2]*m2[1];
            m3[1] = m1[1]*m2[0] + m1[3]*m2[1];
            
            m3[2] = m1[0]*m2[2] + m1[2]*m2[3];
            m3[3] = m1[1]*m2[2] + m1[3]*m2[3];
            
            return m3;
        }
        
        force_inline matrix2<T> operator * (T Scalar) const
        {
            matrix2<T> other;
            
            other[0] = M[0]*Scalar; other[2] = M[2]*Scalar;
            other[1] = M[1]*Scalar; other[3] = M[3]*Scalar;
            
            return other;
        }
        
        force_inline matrix2<T>& operator *= (const matrix2<T> &mltMatrix)
        {
            T m1[4];
            memcpy(m1, M, sizeof(M));
            const T* m2 = mltMatrix.M;
            
            M[0] = m1[0]*m2[0] + m1[2]*m2[1];
            M[1] = m1[1]*m2[0] + m1[3]*m2[1];
            
            M[2] = m1[0]*m2[2] + m1[2]*m2[3];
            M[3] = m1[1]*m2[2] + m1[3]*m2[3];
            
            return *this;
        }
        
        force_inline matrix2<T>& operator *= (T Scalar)
        {
            M[0] *= Scalar; M[2] *= Scalar;
            M[1] *= Scalar; M[3] *= Scalar;
            return *this;
        }
        
        force_inline point2d<T> operator * (const point2d<T> &Vector) const
        {
            return point2d<T>(
                Vector.X*M[0] + Vector.Y*M[2],
                Vector.X*M[1] + Vector.Y*M[3]
            );
        }
        
        force_inline void clear()
        {
            memset(M, 0, sizeof(M));
        }
        
        /*
        ( 1  0 )
        ( 0  1 )
        */
        force_inline matrix2<T>& reset() // Loads identity
        {
            M[0] = 1; M[2] = 0;
            M[1] = 0; M[3] = 1;
            return *this;
        }
        
        force_inline void multiplySingleMatrix(const T (&Other)[2]) // Multiplies this matrix with a ( 1 x 2 ) matrix
        {
            T Result[2];
            
            Result[0] = M[0]*Other[0] + M[2]*Other[1];
            Result[1] = M[1]*Other[0] + M[3]*Other[1];
            
            Other[0] = Result[0];
            Other[1] = Result[1];
        }
        
        force_inline bool getInverse(matrix2<T> &InverseMat) const
        {
            T d = M[0]*M[3] - M[2]*M[1];
            
            if (d == T(0))
                return false;
            
            d = T(1) / d;
            
            InverseMat[0] = d * (  M[3] );
            InverseMat[1] = d * ( -M[1] );
            InverseMat[2] = d * ( -M[2] );
            InverseMat[3] = d * (  M[0] );
            
            return true;
        }
        
        force_inline bool setInverse()
        {
            matrix2<T> Matrix;
            
            if (getInverse(Matrix))
            {
                *this = Matrix;
                return true;
            }
            
            return false;
        }
        
        force_inline matrix2<T> getInverse() const
        {
            matrix2<T> Mat;
            getInverse(Mat);
            return Mat;
        }
        
        /*
        ( x  0 )
        ( 0  y )
        */
        force_inline matrix2<T>& scale(const point2d<T> &Vector)
        {
            matrix2<T> other;
            
            other[0] = Vector.X;
            other[3] = Vector.Y;
            
            return *this *= other;
        }
        
        /*
        (  cos  sin )
        ( -sin  cos )
        */
        force_inline matrix2<T>& rotate(const T Angle)
        {
            matrix2<T> other;
            other.setRotation(Angle);
            return *this *= other;
        }
        
        force_inline void setRotation(T Rotation, bool UseDegrees = true)
        {
            if (UseDegrees)
                Rotation = Rotation * static_cast<T>(M_PI) / T(180);
            
            const T s = sin(Rotation);
            const T c = cos(Rotation);
            
            M[0] =  c; M[2] = s;
            M[1] = -s; M[3] = c;
        }
        
        /* === Row & columns === */
        
        force_inline point2d<T> getRow(int32_t Position) const
        {
            switch (Position) {
                case 0:
                    return point2d<T>(M[0], M[2]);
                case 1:
                    return point2d<T>(M[1], M[3]);
            }
            return point2d<T>();
        }
        
        force_inline point2d<T> getColumn(int32_t Position) const
        {
            switch (Position) {
                case 0:
                    return point2d<T>(M[0], M[1]);
                case 1:
                    return point2d<T>(M[2], M[3]);
            }
            return point2d<T>();
        }
        
        force_inline void setScale(const point2d<T> &Scale)
        {
            M[0] = Scale.X; M[2] = Scale.Y;
        }
        force_inline point2d<T> getScale() const
        {
            if (Equal(M[1], 0.0f) && Equal(M[3], 0.0f))
                return point2d<T>(M[0], M[3]);
            
            return point2d<T>(
                sqrtf(M[0]*M[0] + M[1]*M[1]),
                sqrtf(M[2]*M[2] + M[3]*M[3])
            );
        }
        
        force_inline T getRotation() const
        {
            return 0.0f; // !TODO!
        }
        
		template <typename Matrix>
        force_inline Matrix getTransposed() const
        {
            Matrix Mat;
            getTransposed(Mat);
            return Mat;
        }
        
		template <typename Matrix>
        force_inline void getTransposed(Matrix &other) const
        {
            other[0] = M[0]; other[2] = M[1];
            other[1] = M[2]; other[3] = M[3];
        }
        
        force_inline point2d<T> interpolate(const point2d<T> &other, float seek) const
        {
            point2d<T> Mat;
            
            for (int32_t i = 0; i < 4; ++i)
                Mat.M[i] = M[i] + (other.M[i] - M[i]) * seek;
            
            return Mat;
        }
        
        force_inline bool isIdentity() const
        {
            return
                Equal(M[0], 1.0f) && Equal(M[1], 0.0f) &&
                Equal(M[2], 1.0f) && Equal(M[3], 0.0f);
        }
        
        force_inline const T* getArray() const
        {
            return M;
        }
        force_inline T* getArray()
        {
            return M;
        }
        
        template <typename B> force_inline matrix2<B> cast() const
        {
            B other[4];
            
            for (int32_t i = 0; i < 4; ++i)
                other[i] = static_cast<B>(M[i]);
            
            return matrix2<B>(other);
        }
        
    private:
        
        /* === Member === */
        
        /*
         * The matrix memory
         * (all 2x2 matrices allocate 4 elements)
         */
        T M[4];
        
};

typedef matrix2<int32_t> matrix2i;
typedef matrix2<float> matrix2f;
typedef matrix2<double> matrix2d;

/////////////////////////////////////////////////////////////

template <typename T> class matrix3
{
    
    public:
        
        force_inline matrix3()
        {
            reset();
        }
        force_inline matrix3(const T (&Other)[9])
        {
            *this = Other;
        }
        force_inline matrix3(const matrix3<T> &Other)
        {
            *this = Other;
        }
        force_inline matrix3(
            T m1n1, T m2n1, T m3n1,
            T m1n2, T m2n2, T m3n2,
            T m1n3, T m2n3, T m3n3)
        {
            M[0] = m1n1; M[3] = m2n1; M[6] = m3n1;
            M[1] = m1n2; M[4] = m2n2; M[7] = m3n2;
            M[2] = m1n3; M[5] = m2n3; M[8] = m3n3;
        }
        force_inline ~matrix3()
        {
        }
        
        /* === Operators === */
        
        force_inline const T& operator () (int32_t row, int32_t col) const
        {
            return M[ (row * 3) + col ];
        }
        force_inline T& operator () (int32_t row, int32_t col)
        {
            return M[ (row * 3) + col ];
        }
        
        force_inline const T operator [] (uint32_t i) const
        {
            return i < 9 ? M[i] : (T)0;
        }
        force_inline T& operator [] (uint32_t i)
        {
            return M[i];
        }
        
        force_inline bool operator == (const matrix3<T> &other)
        {
            for (int32_t i = 0; i < 9; ++i)
            {
                if (M[i] != other.M[i])
                    return false;
            }
            return true;
        }
        force_inline bool operator != (const matrix3<T> &other)
        {
            for (int32_t i = 0; i < 9; ++i)
            {
                if (M[i] != other.M[i])
                    return true;
            }
            return false;
        }
        
        force_inline matrix3<T>& operator = (const T (&other)[9])
        {
            M[0] = other[0]; M[3] = other[3]; M[6] = other[6];
            M[1] = other[1]; M[4] = other[4]; M[7] = other[7];
            M[2] = other[2]; M[5] = other[5]; M[8] = other[8];
            return *this;
        }
        force_inline matrix3<T>& operator = (const matrix3<T> &other)
        {
            M[0] = other[0]; M[3] = other[3]; M[6] = other[6];
            M[1] = other[1]; M[4] = other[4]; M[7] = other[7];
            M[2] = other[2]; M[5] = other[5]; M[8] = other[8];
            return *this;
        }
        force_inline matrix3<T>& operator = (T Scalar)
        {
            memset(M, Scalar, sizeof(M));
            return *this;
        }
        
        force_inline matrix3<T> operator + (const matrix3<T> &mltMatrix) const
        {
            matrix3<T> other;
            
            other[0] = M[0] + mltMatrix[0]; other[3] = M[3] + mltMatrix[3]; other[6] = M[6] + mltMatrix[6];
            other[1] = M[1] + mltMatrix[1]; other[4] = M[4] + mltMatrix[4]; other[7] = M[7] + mltMatrix[7];
            other[2] = M[2] + mltMatrix[2]; other[5] = M[5] + mltMatrix[5]; other[8] = M[8] + mltMatrix[8];
            
            return other;
        }
        
        force_inline matrix3<T>& operator += (const matrix3<T> &mltMatrix)
        {
            M[0] += mltMatrix[0]; M[3] += mltMatrix[3]; M[6] += mltMatrix[6];
            M[1] += mltMatrix[1]; M[4] += mltMatrix[4]; M[7] += mltMatrix[7];
            M[2] += mltMatrix[2]; M[5] += mltMatrix[5]; M[8] += mltMatrix[8];
            return *this;
        }
        
        force_inline matrix3<T> operator - (const matrix3<T> &mltMatrix) const
        {
            matrix3<T> other;
            other[0] = M[0] - mltMatrix[0]; other[3] = M[3] - mltMatrix[3]; other[6] = M[6] - mltMatrix[6];
            other[1] = M[1] - mltMatrix[1]; other[4] = M[4] - mltMatrix[4]; other[7] = M[7] - mltMatrix[7];
            other[2] = M[2] - mltMatrix[2]; other[5] = M[5] - mltMatrix[5]; other[8] = M[8] - mltMatrix[8];
            return other;
        }
        force_inline matrix3<T>& operator -= (const matrix3<T> &mltMatrix)
        {
            M[0] -= mltMatrix[0]; M[3] -= mltMatrix[3]; M[6] -= mltMatrix[6];
            M[1] -= mltMatrix[1]; M[4] -= mltMatrix[4]; M[7] -= mltMatrix[7];
            M[2] -= mltMatrix[2]; M[5] -= mltMatrix[5]; M[8] -= mltMatrix[8];
            return *this;
        }
        
        force_inline matrix3<T> operator * (const matrix3<T> &mltMatrix) const
        {
            matrix3<T> m3;
            const T* m1 = M;
            const T* m2 = mltMatrix.M;
            
            m3[0] = m1[0]*m2[0] + m1[3]*m2[1] + m1[6]*m2[2];
            m3[1] = m1[1]*m2[0] + m1[4]*m2[1] + m1[7]*m2[2];
            m3[2] = m1[2]*m2[0] + m1[5]*m2[1] + m1[8]*m2[2];
            
            m3[3] = m1[0]*m2[3] + m1[3]*m2[4] + m1[6]*m2[5];
            m3[4] = m1[1]*m2[3] + m1[4]*m2[4] + m1[7]*m2[5];
            m3[5] = m1[2]*m2[3] + m1[5]*m2[4] + m1[8]*m2[5];
            
            m3[6] = m1[0]*m2[6] + m1[3]*m2[7] + m1[6]*m2[8];
            m3[7] = m1[1]*m2[6] + m1[4]*m2[7] + m1[7]*m2[8];
            m3[8] = m1[2]*m2[6] + m1[5]*m2[7] + m1[8]*m2[8];
            
            return m3;
        }
        
        force_inline matrix3<T> operator * (const T &Scalar) const
        {
            matrix3<T> other;
            
            other[0] = M[0]*Scalar; other[3] = M[3]*Scalar; other[6] = M[6]*Scalar;
            other[1] = M[1]*Scalar; other[4] = M[4]*Scalar; other[7] = M[7]*Scalar;
            other[2] = M[2]*Scalar; other[5] = M[5]*Scalar; other[8] = M[8]*Scalar;
            
            return other;
        }
        
        force_inline matrix3<T>& operator *= (const matrix3<T> &mltMatrix)
        {
            T m1[16];
            memcpy(m1, M, sizeof(M));
            const T* m2 = mltMatrix.M;
            
            M[0] = m1[0]*m2[0] + m1[3]*m2[1] + m1[6]*m2[2];
            M[1] = m1[1]*m2[0] + m1[4]*m2[1] + m1[7]*m2[2];
            M[2] = m1[2]*m2[0] + m1[5]*m2[1] + m1[8]*m2[2];
            
            M[3] = m1[0]*m2[3] + m1[3]*m2[4] + m1[6]*m2[5];
            M[4] = m1[1]*m2[3] + m1[4]*m2[4] + m1[7]*m2[5];
            M[5] = m1[2]*m2[3] + m1[5]*m2[4] + m1[8]*m2[5];
            
            M[6] = m1[0]*m2[6] + m1[3]*m2[7] + m1[6]*m2[8];
            M[7] = m1[1]*m2[6] + m1[4]*m2[7] + m1[7]*m2[8];
            M[8] = m1[2]*m2[6] + m1[5]*m2[7] + m1[8]*m2[8];
            
            return *this;
        }
        
        force_inline matrix3<T>& operator *= (T Scalar)
        {
            M[0] *= Scalar; M[3] *= Scalar; M[6] *= Scalar;
            M[1] *= Scalar; M[4] *= Scalar; M[7] *= Scalar;
            M[2] *= Scalar; M[5] *= Scalar; M[8] *= Scalar;
            return *this;
        }
        
        /*
        (  1-1  2-1  3-1  )     ( x )
        |  1-2  2-2  3-2  |  *  | y |
        (  1-3  2-3  3-3  )     ( z )
        */
        force_inline vector3d<T> operator * (const vector3d<T> &Vector) const
        {
            return vector3d<T>(
                Vector.X*M[0] + Vector.Y*M[3] + Vector.Z*M[6],
                Vector.X*M[1] + Vector.Y*M[4] + Vector.Z*M[7],
                Vector.X*M[2] + Vector.Y*M[5] + Vector.Z*M[8]
            );
        }
        
        force_inline point2d<T> operator * (const point2d<T> &Vector) const
        {
            return point2d<T>(
                Vector.X*M[0] + Vector.Y*M[3],
                Vector.X*M[1] + Vector.Y*M[4]
            );
        }
        
        force_inline triangle3d<T> operator * (const triangle3d<T> &Triangle) const
        {
            return triangle3d<T>(
                *this * Triangle.PointA,
                *this * Triangle.PointB,
                *this * Triangle.PointC
            );
        }
        
        force_inline triangle3d<T> operator * (const triangle3d<T, vector3d<T>*> &Triangle) const
        {
            return triangle3d<T>(
                *this * (*Triangle.PointA),
                *this * (*Triangle.PointB),
                *this * (*Triangle.PointC)
            );
        }
        
        force_inline plane3d<T> operator * (const plane3d<T> &Plane) const
        {
            plane3d<T> NewPlane(Plane);
            
            vector3d<T> Member( *this * ( - NewPlane.Normal * NewPlane.Distance ) );
            
            vector3d<T> Origin;
            NewPlane.Normal = *this * NewPlane.Normal;
            Origin = *this * Origin;
            
            NewPlane.Normal -= Origin;
            NewPlane.Distance = - Member.dot(NewPlane.Normal);
            
            return NewPlane;
        }
        
        /* === Transformation functions === */
        
        force_inline vector3d<T> vecRotate(const vector3d<T> &Vector) const
        {
            return vector3d<T>(
                Vector.X*M[0] + Vector.Y*M[3] + Vector.Z*M[6],
                Vector.X*M[1] + Vector.Y*M[4] + Vector.Z*M[7],
                Vector.X*M[2] + Vector.Y*M[5] + Vector.Z*M[8]
            );
        }
        
        force_inline vector3d<T> vecRotateInverse(const vector3d<T> &Vector) const
        {
            return vector3d<T>(
                Vector.X*M[0] + Vector.Y*M[1] + Vector.Z*M[2],
                Vector.X*M[3] + Vector.Y*M[4] + Vector.Z*M[5],
                Vector.X*M[6] + Vector.Y*M[7] + Vector.Z*M[8]
            );
        }
        
        force_inline void clear()
        {
            memset(M, 0, sizeof(M));
        }
        
        /*
        (  1  0  0  )
        |  0  1  0  |
        (  0  0  1  )
        */
        force_inline matrix3<T>& reset() // Loads identity
        {
            M[0] = 1; M[3] = 0; M[6] = 0;
            M[1] = 0; M[4] = 1; M[7] = 0;
            M[2] = 0; M[5] = 0; M[8] = 1;
            return *this;
        }
        
        force_inline void multiplySingleMatrix(const T (&other)[3]) // Multiplies this matrix with a ( 1 x 3 ) matrix
        {
            T Result[3];
            
            Result[0] = M[0]*other[0] + M[3]*other[1] + M[6]*other[2];
            Result[1] = M[1]*other[0] + M[4]*other[1] + M[7]*other[2];
            Result[2] = M[2]*other[0] + M[5]*other[1] + M[8]*other[2];
            
            other[0] = Result[0]; other[1] = Result[1]; other[2] = Result[2];
        }
        
        force_inline void matrixLookAt(const vector3d<T> &Position, const vector3d<T> &LookAt, const vector3d<T> &upVector)
        {
            vector3d<T> ZAxis = LookAt - Position;
            ZAxis.normalize();
            
            vector3d<T> XAxis = upVector.cross(ZAxis);
            XAxis.normalize();
            
            vector3d<T> YAxis = ZAxis.cross(XAxis);
            
            M[0] = XAxis.X; M[3] = XAxis.Y; M[6] = XAxis.Z;
            M[1] = YAxis.X; M[4] = YAxis.Y; M[7] = YAxis.Z;
            M[2] = ZAxis.X; M[5] = ZAxis.Y; M[8] = ZAxis.Z;
        }
        
        force_inline bool getInverse(matrix3<T> &InverseMat) const
        {
            const matrix3<T> &m = *this;
            
            T d = ( m(0, 0) * m(1, 1) * m(2, 2) ) + ( m(0, 1) * m(1, 2) * m(2, 0) ) + ( m(0, 2) * m(1, 0) * m(2, 1) ) -
                    ( m(2, 0) * m(1, 1) * m(0, 2) ) - ( m(2, 0) * m(1, 2) * m(0, 0) ) - ( m(2, 2) * m(1, 0) * m(0, 1) );
            
            if (d == T(0))
                return false;
            
            d = T(1) / d;
            
            InverseMat(0, 0) = d * ( m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1) );
            InverseMat(0, 1) = d * ( m(0, 2) * m(2, 1) - m(0, 1) * m(2, 2) );
            InverseMat(0, 2) = d * ( m(0, 1) * m(1, 2) - m(0, 2) * m(1, 1) );
            InverseMat(1, 0) = d * ( m(1, 2) * m(2, 0) - m(1, 0) * m(2, 2) );
            InverseMat(1, 1) = d * ( m(0, 0) * m(2, 2) - m(0, 2) * m(2, 0) );
            InverseMat(1, 2) = d * ( m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0) );
            InverseMat(2, 0) = d * ( m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0) );
            InverseMat(2, 1) = d * ( m(0, 1) * m(2, 0) - m(0, 0) * m(2, 1) );
            InverseMat(2, 2) = d * ( m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0) );
            
            return true;
        }
        
        force_inline bool setInverse()
        {
            matrix3<T> Matrix;
            
            if (getInverse(Matrix))
            {
                *this = Matrix;
                return true;
            }
            
            return false;
        }
        
        force_inline matrix3<T> getInverse() const
        {
            matrix3<T> Mat;
            getInverse(Mat);
            return Mat;
        }
        
        /*
        (  x  0  0  )
        |  0  y  0  |
        (  0  0  z  )
        */
        force_inline matrix3<T>& scale(const vector3d<T> &Vector)
        {
            matrix3<T> other;
            
            other[0] = Vector.X;
            other[4] = Vector.Y;
            other[8] = Vector.Z;
            
            return *this *= other;
        }
        
        /*
        ( xx(1-c)+c   xy(1-c)-zs  xz(1-c)+ys )
        | yx(1-c)+zs  yy(1-c)+c   yz(1-c)-xs |
        ( xz(1-c)-ys  yz(1-c)+xs  zz(1-c)+c  )
        */
        force_inline matrix3<T>& rotate(const T &Angle, vector3d<T> Rotation)
        {
            matrix3<T> other;
            
            /* Normalize the vector */
            Rotation.normalize();
            
            /* Parameters normalization */
            T x   = Rotation.X;
            T y   = Rotation.Y;
            T z   = Rotation.Z;
            T c   = Cos(Angle);
            T s   = Cos(Angle);
            T cc  = T(1) - c;
            
            /* Rotation */
            other[0] = x*x*cc+c;   other[3] = x*y*cc-z*s; other[6] = x*z*cc+y*s;
            other[1] = y*x*cc+z*s; other[4] = y*y*cc+c;   other[7] = y*z*cc-x*s;
            other[2] = x*z*cc-y*s; other[5] = y*z*cc+x*s; other[8] = z*z*cc+c;
            
            return *this *= other;
        }
        
        force_inline matrix3<T>& rotateX(const T &Angle)
        {
            matrix3<T> other;
            T c = Cos(Angle);
            T s = Sin(Angle);
            
            /* Rotation */
            other[0] = 1; other[4] = 0; other[ 8] =  0;
            other[1] = 0; other[5] = c; other[ 9] = -s;
            other[2] = 0; other[6] = s; other[10] =  c;
            
            return *this *= other;
        }
        
        force_inline matrix3<T>& rotateY(const T &Angle)
        {
            matrix3<T> other;
            T c = Cos(Angle);
            T s = Sin(Angle);
            
            /* Rotation */
            other[0] =  c; other[3] = 0; other[6] = s;
            other[1] =  0; other[4] = 1; other[7] = 0;
            other[2] = -s; other[5] = 0; other[8] = c;
            
            return *this *= other;
        }
        
        force_inline matrix3<T>& rotateZ(const T &Angle)
        {
            matrix3<T> other;
            T c = Cos(Angle);
            T s = Sin(Angle);
            
            /* Rotation */
            other[0] = c; other[3] = -s; other[6] = 0;
            other[1] = s; other[4] =  c; other[7] = 0;
            other[2] = 0; other[5] =  0; other[8] = 1;
            
            return *this *= other;
        }
        
        force_inline void rotateYXZ(const vector3df &Rotation)
        {
            rotateY(Rotation.Y);
            rotateX(Rotation.X);
            rotateZ(Rotation.Z);
        }
        
        force_inline void rotateZXY(const vector3df &Rotation)
        {
            rotateZ(Rotation.Z);
            rotateX(Rotation.X);
            rotateY(Rotation.Y);
        }
        
        force_inline void setRotation(vector3df Rotation, bool UseDegrees = true)
        {
            if (UseDegrees)
                Rotation = Rotation * M_PI / 180.0;
            
            double cx = cos(Rotation.X);
            double sx = sin(Rotation.X);
            double cy = cos(Rotation.Y);
            double sy = sin(Rotation.Y);
            double cz = cos(Rotation.Z);
            double sz = sin(Rotation.Z);
            
            double sxy = sx*sy;
            double cxy = cx*sy;
            
            M[0] = (T)(cy*cz);
            M[1] = (T)(cy*sz);
            M[2] = (T)(-sy);
            
            M[3] = (T)(sxy*cz - cx*sz);
            M[4] = (T)(sxy*sz + cx*cz);
            M[5] = (T)(sx*cy);
            
            M[6] = (T)(cxy*cz + sx*sz);
            M[7] = (T)(cxy*sz - sx*cz);
            M[8] = (T)(cx*cy);
        }
        
        force_inline void setInverseRotation(vector3df Rotation, bool UseDegrees = true)
        {
            if (UseDegrees)
                Rotation = Rotation * M_PI / 180.0;
            
            double cx = cos(Rotation.X);
            double sx = sin(Rotation.X);
            double cy = cos(Rotation.Y);
            double sy = sin(Rotation.Y);
            double cz = cos(Rotation.Z);
            double sz = sin(Rotation.Z);
            
            double sxy = sx*sy;
            double cxy = cx*sy;
            
            M[0] = (T)(cy*cz);
            M[3] = (T)(cy*sz);
            M[6] = (T)(-sy);
            
            M[1] = (T)(sxy*cz - cx*sz);
            M[4] = (T)(sxy*sz + cx*cz);
            M[7] = (T)(sx*cy);
            
            M[2] = (T)(cxy*cz + sx*sz);
            M[5] = (T)(cxy*sz - sx*cz);
            M[8] = (T)(cx*cy);
        }
        
        force_inline void setTextureRotation(const T &Degree)
        {
            T c = Cos(Degree);
            T s = Sin(Degree);
            
            M[0] = c;
            M[1] = s;
            M[2] = T( -0.5 * ( c + s) + 0.5 );
            
            M[3] = -s;
            M[4] = c;
            M[5] = T( -0.5 * (-s + c) + 0.5 );
        }
        
        /* === Row & columns === */
        
        force_inline vector3d<T> getRow(int32_t Position) const
        {
            switch (Position) {
                case 0:
                    return vector3d<T>(M[0], M[3], M[6]);
                case 1:
                    return vector3d<T>(M[1], M[4], M[7]);
                case 2:
                    return vector3d<T>(M[2], M[5], M[8]);
            }
            return vector3d<T>();
        }
        
        force_inline vector3d<T> getColumn(int32_t Position) const
        {
            switch (Position) {
                case 0:
                    return vector3d<T>(M[0], M[1], M[2]);
                case 1:
                    return vector3d<T>(M[3], M[4], M[5]);
                case 2:
                    return vector3d<T>(M[6], M[7], M[8]);
            }
            return vector3d<T>();
        }
        
        force_inline void setScale(const vector3d<T> &Scale)
        {
            M[0] = Scale.X; M[4] = Scale.Y; M[8] = Scale.Z;
        }
        force_inline vector3d<T> getScale() const
        {
            return vector3d<T>(M[0], M[5], M[10]);
        }
        
        force_inline vector3d<T> getRotation() const
        {
            const matrix3<T> &Mat = *this;
            
            T Y = -(T)asin(Mat(0, 2));
            T C = (T)cos(Y);
            T rotx, roty, X, Z;
            
            Y *= (T)RAD64;
            
            if (fabs(C) > ROUNDING_ERROR)
            {
                C       = (T)(1.0 / C);
                rotx    = Mat(2, 2) * C;
                roty    = Mat(1, 2) * C;
                X       = (T)(atan2(roty, rotx) * RAD64);
                rotx    = Mat(0, 0) * C;
                roty    = Mat(0, 1) * C;
                Z       = (T)(atan2(roty, rotx) * RAD64);
            }
            else
            {
                X       = 0;
                rotx    =  Mat(1, 1);
                roty    = -Mat(1, 0);
                Z       = atan2(roty, rotx) * RAD64;
            }
            
            if (X < 0) X += 360;
            if (Y < 0) Y += 360;
            if (Z < 0) Z += 360;
            
            return vector3d<T>(X, Y, Z);
        }
        
        force_inline matrix3<T> getTransposed() const
        {
            matrix3<T> Mat;
            getTransposed(Mat);
            return Mat;
        }
        
        force_inline void getTransposed(matrix3<T> &other) const
        {
            other[0] = M[0]; other[3] = M[1]; other[6] = M[2];
            other[1] = M[3]; other[4] = M[4]; other[7] = M[5];
            other[2] = M[6]; other[5] = M[7]; other[8] = M[8];
        }
        
        force_inline matrix3<T> interpolate(const matrix3<T> &other, float seek) const
        {
            matrix3<T> Mat;
            
            for (int32_t i = 0; i < 9; ++i)
                Mat.M[i] = M[i] + (other.M[i] - M[i]) * seek;
            
            return Mat;
        }
        
        force_inline bool isIdentity() const
        {
            for (int32_t i = 0, j; i < 3; ++i)
            {
                for (j = 0; j < 3; ++j)
                {
                    if ( ( i != j && !Equal((*this)(i, j), 0.0f) ) ||
                         ( i == j && !Equal((*this)(i, j), 1.0f) ) )
                    {
                        return false;
                    }
                }
            }
            
            return true;
        }
        
        force_inline const T* getArray() const
        {
            return M;
        }
        
        force_inline T* getArray()
        {
            return M;
        }
        
        template <typename B> force_inline matrix3<B> cast() const
        {
            B other[9];
            
            for (int32_t i = 0; i < 9; ++i)
                other[i] = static_cast<B>(M[i]);
            
            return matrix3<B>(other);
        }
        
    private:
        
        /* === Members === */
        
        /*
         * The matrix memory
         * (all 3x3 matrices allocate 9 elements)
         */
        T M[9];
        
};

typedef matrix3<float> matrix3f;
typedef matrix3<double> matrix3d;

/////////////////////////////////////////////////////////////

template <typename T> class matrix3;
template <typename T> class matrix2;

enum EMatrixTypes
{
    MATRIX_PROJECTION = 0,  //!< Projection space.
    MATRIX_VIEW,            //!< View/ camera/ eye space.
    MATRIX_WORLD,           //!< World/ object space
    MATRIX_TEXTURE,         //!< Texture space.
    MATRIX_COLOR,           //!< Color space. Actually unused.
    
    MATRIX_COUNT            //!< Count of matrix types. Don't use this as a type!
};

//! Matrix coordinate systems.
enum EMatrixCoordinateSystmes
{
    MATRIX_LEFTHANDED,  //!< Left-handed coordinate system. Positive Z values point into the screen.
    MATRIX_RIGHTHANDED  //!< Right-handed coordinate system. Positive Z values point out of the screen.
};

/**
This is the 4x4 matrix class. The engine uses left-handed coordinate systems and the matrix data is stored in the following form:
\code
//      Vectors:
//   x   y   z   w
// / 0   4   8  12 \
// | 1   5   9  13 |
// | 2   6  10  14 |
// \ 3   7  11  15 /
\endcode
A matrix has by default its identity where the member 0, 5, 10 and 15 have a value of 1.0 and all the others 0.0.
*/
template <typename T> class matrix4
{
    
    public:
        
        force_inline matrix4()
        {
            reset();
        }
        force_inline matrix4(const T (&Other)[16])
        {
            *this = Other;
        }
        force_inline matrix4(const matrix4<T> &Other)
        {
            *this = Other;
        }
        force_inline matrix4(const matrix3<T> &Other);
        force_inline matrix4(const obbox3d<T> &Box)
        {
            M[0] = Box.Axis.X.X * Box.HalfSize.X;
            M[1] = Box.Axis.X.Y * Box.HalfSize.X;
            M[2] = Box.Axis.X.Z * Box.HalfSize.X;
            M[3] = 0;
            
            M[4] = Box.Axis.Y.X * Box.HalfSize.Y;
            M[5] = Box.Axis.Y.Y * Box.HalfSize.Y;
            M[6] = Box.Axis.Y.Z * Box.HalfSize.Y;
            M[7] = 0;
            
            M[ 8] = Box.Axis.Z.X * Box.HalfSize.Z;
            M[ 9] = Box.Axis.Z.Y * Box.HalfSize.Z;
            M[10] = Box.Axis.Z.Z * Box.HalfSize.Z;
            M[11] = 0;
            
            M[12] = Box.Center.X;
            M[13] = Box.Center.Y;
            M[14] = Box.Center.Z;
            M[15] = 1;
        }
        force_inline matrix4(
            T m1n1, T m2n1, T m3n1, T m4n1,
            T m1n2, T m2n2, T m3n2, T m4n2,
            T m1n3, T m2n3, T m3n3, T m4n3,
            T m1n4, T m2n4, T m3n4, T m4n4)
        {
            M[0] = m1n1; M[4] = m2n1; M[ 8] = m3n1; M[12] = m4n1;
            M[1] = m1n2; M[5] = m2n2; M[ 9] = m3n2; M[13] = m4n2;
            M[2] = m1n3; M[6] = m2n3; M[10] = m3n3; M[14] = m4n3;
            M[3] = m1n4; M[7] = m2n4; M[11] = m3n4; M[15] = m4n4;
        }
        force_inline matrix4(
            const vector4d<T> &XDirection,
            const vector4d<T> &YDirection,
            const vector4d<T> &ZDirection,
            const vector4d<T> &Position)
        {
            M[0] = XDirection.X; M[4] = YDirection.X; M[ 8] = ZDirection.X; M[12] = Position.X;
            M[1] = XDirection.Y; M[5] = YDirection.Y; M[ 9] = ZDirection.Y; M[13] = Position.Y;
            M[2] = XDirection.Z; M[6] = YDirection.Z; M[10] = ZDirection.Z; M[14] = Position.Z;
            M[3] = XDirection.W; M[7] = YDirection.W; M[11] = ZDirection.W; M[15] = Position.W;
        }
        force_inline ~matrix4()
        {
        }
        
        /* === Operators === */
        
        /**
        \param row: Row of the wanted component in the range [0 .. 3].
        \param col: Column of the wanted component in the range [0 .. 3].
        \return the matrix component value at the specified location
        */
        force_inline const T operator () (uint32_t row, uint32_t col) const
        {
            return row < 4 && col < 4 ? M[(row << 2) + col] : (T)0;
        }
        force_inline T& operator () (uint32_t row, uint32_t col)
        {
            return M[(row << 2) + col];
        }
        
        //! Returns the matrix component value at the specified offset (or rather index) 'i'.
        force_inline const T operator [] (uint32_t i) const
        {
            return i < 16 ? M[i] : (T)0;
        }
        force_inline T& operator [] (uint32_t i)
        {
            return M[i];
        }
        
        force_inline bool operator == (const matrix4<T> &Other)
        {
            for (int32_t i = 0; i < 16; ++i)
            {
                if (M[i] != Other.M[i])
                    return false;
            }
            return true;
        }
        force_inline bool operator != (const matrix4<T> &Other)
        {
            for (int32_t i = 0; i < 16; ++i)
            {
                if (M[i] != Other.M[i])
                    return true;
            }
            return false;
        }
        
        force_inline matrix4<T>& operator = (const T (&Other)[16])
        {
            M[0] = Other[0]; M[4] = Other[4]; M[ 8] = Other[ 8]; M[12] = Other[12];
            M[1] = Other[1]; M[5] = Other[5]; M[ 9] = Other[ 9]; M[13] = Other[13];
            M[2] = Other[2]; M[6] = Other[6]; M[10] = Other[10]; M[14] = Other[14];
            M[3] = Other[3]; M[7] = Other[7]; M[11] = Other[11]; M[15] = Other[15];
            return *this;
        }
        force_inline matrix4<T>& operator = (const matrix4<T> &Other)
        {
            M[0] = Other[0]; M[4] = Other[4]; M[ 8] = Other[ 8]; M[12] = Other[12];
            M[1] = Other[1]; M[5] = Other[5]; M[ 9] = Other[ 9]; M[13] = Other[13];
            M[2] = Other[2]; M[6] = Other[6]; M[10] = Other[10]; M[14] = Other[14];
            M[3] = Other[3]; M[7] = Other[7]; M[11] = Other[11]; M[15] = Other[15];
            return *this;
        }
        force_inline matrix4<T>& operator = (T Scalar)
        {
            memset(M, Scalar, sizeof(M));
            return *this;
        }
        
        force_inline matrix4<T> operator + (const matrix4<T> &mltMatrix) const
        {
            matrix4<T> Other;
            
            Other[0] = M[0] + mltMatrix[0]; Other[4] = M[4] + mltMatrix[4]; Other[ 8] = M[ 8] + mltMatrix[ 8]; Other[12] = M[12] + mltMatrix[12];
            Other[1] = M[1] + mltMatrix[1]; Other[5] = M[5] + mltMatrix[5]; Other[ 9] = M[ 9] + mltMatrix[ 9]; Other[13] = M[13] + mltMatrix[13];
            Other[2] = M[2] + mltMatrix[2]; Other[6] = M[6] + mltMatrix[6]; Other[10] = M[10] + mltMatrix[10]; Other[14] = M[14] + mltMatrix[14];
            Other[3] = M[3] + mltMatrix[3]; Other[7] = M[7] + mltMatrix[7]; Other[11] = M[11] + mltMatrix[11]; Other[15] = M[15] + mltMatrix[15];
            
            return Other;
        }
        
        force_inline matrix4<T>& operator += (const matrix4<T> &mltMatrix)
        {
            M[0] += mltMatrix[0]; M[4] += mltMatrix[4]; M[ 8] += mltMatrix[ 8]; M[12] += mltMatrix[12];
            M[1] += mltMatrix[1]; M[5] += mltMatrix[5]; M[ 9] += mltMatrix[ 9]; M[13] += mltMatrix[13];
            M[2] += mltMatrix[2]; M[6] += mltMatrix[6]; M[10] += mltMatrix[10]; M[14] += mltMatrix[14];
            M[3] += mltMatrix[3]; M[7] += mltMatrix[7]; M[11] += mltMatrix[11]; M[15] += mltMatrix[15];
            return *this;
        }
        
        force_inline matrix4<T> operator - (const matrix4<T> &mltMatrix) const
        {
            matrix4<T> Other;
            Other[0] = M[0] - mltMatrix[0]; Other[4] = M[4] - mltMatrix[4]; Other[ 8] = M[ 8] - mltMatrix[ 8]; Other[12] = M[12] - mltMatrix[12];
            Other[1] = M[1] - mltMatrix[1]; Other[5] = M[5] - mltMatrix[5]; Other[ 9] = M[ 9] - mltMatrix[ 9]; Other[13] = M[13] - mltMatrix[13];
            Other[2] = M[2] - mltMatrix[2]; Other[6] = M[6] - mltMatrix[6]; Other[10] = M[10] - mltMatrix[10]; Other[14] = M[14] - mltMatrix[14];
            Other[3] = M[3] - mltMatrix[3]; Other[7] = M[7] - mltMatrix[7]; Other[11] = M[11] - mltMatrix[11]; Other[15] = M[15] - mltMatrix[15];
            return Other;
        }
        force_inline matrix4<T>& operator -= (const matrix4<T> &mltMatrix)
        {
            M[0] -= mltMatrix[0]; M[4] -= mltMatrix[4]; M[ 8] -= mltMatrix[ 8]; M[12] -= mltMatrix[12];
            M[1] -= mltMatrix[1]; M[5] -= mltMatrix[5]; M[ 9] -= mltMatrix[ 9]; M[13] -= mltMatrix[13];
            M[2] -= mltMatrix[2]; M[6] -= mltMatrix[6]; M[10] -= mltMatrix[10]; M[14] -= mltMatrix[14];
            M[3] -= mltMatrix[3]; M[7] -= mltMatrix[7]; M[11] -= mltMatrix[11]; M[15] -= mltMatrix[15];
            return *this;
        }
        
        force_inline matrix4<T> operator * (const matrix4<T> &mltMatrix) const
        {
            matrix4<T> m3;
            const T* m1 = M;
            const T* m2 = mltMatrix.M;
            
            m3[ 0] = m1[0]*m2[ 0] + m1[4]*m2[ 1] + m1[ 8]*m2[ 2] + m1[12]*m2[ 3];
            m3[ 1] = m1[1]*m2[ 0] + m1[5]*m2[ 1] + m1[ 9]*m2[ 2] + m1[13]*m2[ 3];
            m3[ 2] = m1[2]*m2[ 0] + m1[6]*m2[ 1] + m1[10]*m2[ 2] + m1[14]*m2[ 3];
            m3[ 3] = m1[3]*m2[ 0] + m1[7]*m2[ 1] + m1[11]*m2[ 2] + m1[15]*m2[ 3];
            
            m3[ 4] = m1[0]*m2[ 4] + m1[4]*m2[ 5] + m1[ 8]*m2[ 6] + m1[12]*m2[ 7];
            m3[ 5] = m1[1]*m2[ 4] + m1[5]*m2[ 5] + m1[ 9]*m2[ 6] + m1[13]*m2[ 7];
            m3[ 6] = m1[2]*m2[ 4] + m1[6]*m2[ 5] + m1[10]*m2[ 6] + m1[14]*m2[ 7];
            m3[ 7] = m1[3]*m2[ 4] + m1[7]*m2[ 5] + m1[11]*m2[ 6] + m1[15]*m2[ 7];
            
            m3[ 8] = m1[0]*m2[ 8] + m1[4]*m2[ 9] + m1[ 8]*m2[10] + m1[12]*m2[11];
            m3[ 9] = m1[1]*m2[ 8] + m1[5]*m2[ 9] + m1[ 9]*m2[10] + m1[13]*m2[11];
            m3[10] = m1[2]*m2[ 8] + m1[6]*m2[ 9] + m1[10]*m2[10] + m1[14]*m2[11];
            m3[11] = m1[3]*m2[ 8] + m1[7]*m2[ 9] + m1[11]*m2[10] + m1[15]*m2[11];
            
            m3[12] = m1[0]*m2[12] + m1[4]*m2[13] + m1[ 8]*m2[14] + m1[12]*m2[15];
            m3[13] = m1[1]*m2[12] + m1[5]*m2[13] + m1[ 9]*m2[14] + m1[13]*m2[15];
            m3[14] = m1[2]*m2[12] + m1[6]*m2[13] + m1[10]*m2[14] + m1[14]*m2[15];
            m3[15] = m1[3]*m2[12] + m1[7]*m2[13] + m1[11]*m2[14] + m1[15]*m2[15];
            
            return m3;
        }
        
        force_inline matrix4<T> operator * (T Scalar) const
        {
            matrix4<T> Other;
            
            Other[0] = M[0]*Scalar; Other[4] = M[4]*Scalar; Other[ 8] = M[ 8]*Scalar; Other[12] = M[12]*Scalar;
            Other[1] = M[1]*Scalar; Other[5] = M[5]*Scalar; Other[ 9] = M[ 9]*Scalar; Other[13] = M[13]*Scalar;
            Other[2] = M[2]*Scalar; Other[6] = M[6]*Scalar; Other[10] = M[10]*Scalar; Other[14] = M[14]*Scalar;
            Other[3] = M[3]*Scalar; Other[7] = M[7]*Scalar; Other[11] = M[11]*Scalar; Other[15] = M[15]*Scalar;
            
            return Other;
        }
        
        force_inline matrix4<T>& operator *= (const matrix4<T> &mltMatrix)
        {
            T m1[16];
            memcpy(m1, M, sizeof(M));
            const T* m2 = mltMatrix.M;
            
            M[ 0] = m1[0]*m2[ 0] + m1[4]*m2[ 1] + m1[ 8]*m2[ 2] + m1[12]*m2[ 3];
            M[ 1] = m1[1]*m2[ 0] + m1[5]*m2[ 1] + m1[ 9]*m2[ 2] + m1[13]*m2[ 3];
            M[ 2] = m1[2]*m2[ 0] + m1[6]*m2[ 1] + m1[10]*m2[ 2] + m1[14]*m2[ 3];
            M[ 3] = m1[3]*m2[ 0] + m1[7]*m2[ 1] + m1[11]*m2[ 2] + m1[15]*m2[ 3];
            
            M[ 4] = m1[0]*m2[ 4] + m1[4]*m2[ 5] + m1[ 8]*m2[ 6] + m1[12]*m2[ 7];
            M[ 5] = m1[1]*m2[ 4] + m1[5]*m2[ 5] + m1[ 9]*m2[ 6] + m1[13]*m2[ 7];
            M[ 6] = m1[2]*m2[ 4] + m1[6]*m2[ 5] + m1[10]*m2[ 6] + m1[14]*m2[ 7];
            M[ 7] = m1[3]*m2[ 4] + m1[7]*m2[ 5] + m1[11]*m2[ 6] + m1[15]*m2[ 7];
            
            M[ 8] = m1[0]*m2[ 8] + m1[4]*m2[ 9] + m1[ 8]*m2[10] + m1[12]*m2[11];
            M[ 9] = m1[1]*m2[ 8] + m1[5]*m2[ 9] + m1[ 9]*m2[10] + m1[13]*m2[11];
            M[10] = m1[2]*m2[ 8] + m1[6]*m2[ 9] + m1[10]*m2[10] + m1[14]*m2[11];
            M[11] = m1[3]*m2[ 8] + m1[7]*m2[ 9] + m1[11]*m2[10] + m1[15]*m2[11];
            
            M[12] = m1[0]*m2[12] + m1[4]*m2[13] + m1[ 8]*m2[14] + m1[12]*m2[15];
            M[13] = m1[1]*m2[12] + m1[5]*m2[13] + m1[ 9]*m2[14] + m1[13]*m2[15];
            M[14] = m1[2]*m2[12] + m1[6]*m2[13] + m1[10]*m2[14] + m1[14]*m2[15];
            M[15] = m1[3]*m2[12] + m1[7]*m2[13] + m1[11]*m2[14] + m1[15]*m2[15];
            
            return *this;
        }
        
        force_inline matrix4<T>& operator *= (T Scalar)
        {
            M[0] *= Scalar; M[4] *= Scalar; M[ 8] *= Scalar; M[12] *= Scalar;
            M[1] *= Scalar; M[5] *= Scalar; M[ 9] *= Scalar; M[13] *= Scalar;
            M[2] *= Scalar; M[6] *= Scalar; M[10] *= Scalar; M[14] *= Scalar;
            M[3] *= Scalar; M[7] *= Scalar; M[11] *= Scalar; M[15] *= Scalar;
            return *this;
        }
        
        force_inline vector3d<T> operator * (const vector3d<T> &Vector) const
        {
            return vector3d<T>(
                Vector.X*M[0] + Vector.Y*M[4] + Vector.Z*M[ 8] + M[12],
                Vector.X*M[1] + Vector.Y*M[5] + Vector.Z*M[ 9] + M[13],
                Vector.X*M[2] + Vector.Y*M[6] + Vector.Z*M[10] + M[14]
            );
        }
        
        force_inline point2d<T> operator * (const point2d<T> &Vector) const
        {
            return point2d<T>(
                Vector.X*M[0] + Vector.Y*M[4] + M[12],
                Vector.X*M[1] + Vector.Y*M[5] + M[13]
            );
        }
        
        force_inline triangle3d<T> operator * (const triangle3d<T> &Triangle) const
        {
            return triangle3d<T>(
                *this * Triangle.PointA,
                *this * Triangle.PointB,
                *this * Triangle.PointC
            );
        }
        
        force_inline triangle3d<T> operator * (const triangle3d<T, vector3d<T>*> &Triangle) const
        {
            return triangle3d<T>(
                *this * (*Triangle.PointA),
                *this * (*Triangle.PointB),
                *this * (*Triangle.PointC)
            );
        }
        
        force_inline plane3d<T> operator * (const plane3d<T> &Plane) const
        {
            const vector3d<T> Member(*this * Plane.getMemberPoint());
            const matrix4<T> TransposedInverse(getInverse().getTransposed());
            const vector3d<T> Normal(TransposedInverse * Plane.Normal);
            
            return plane3d<T>(Normal, Normal.dot(Member));
        }
        
        force_inline obbox3d<T> operator * (const obbox3d<T> &Box) const
        {
            return obbox3d<T>(
                *this * Box.Center,
                vecRotate(Box.Axis.X), vecRotate(Box.Axis.Y), vecRotate(Box.Axis.Z)
            );
        }
        
        force_inline line3d<T> operator * (const line3d<T> &Line) const
        {
            return line3d<T>(*this * Line.Start, *this * Line.End);
        }
        
        /* === Transformation functions === */
        
        //! Returns the rotated specified vector. i.e. the vector will be multiplied with the 2x2 matrix.
        force_inline point2d<T> vecRotate(const point2d<T> &Vector) const
        {
            return point2d<T>(
                Vector.X*M[0] + Vector.Y*M[4],
                Vector.X*M[1] + Vector.Y*M[5]
            );
        }
        
        //! Returns the rotated specified vector. i.e. the vector will be multiplied with the inversed 2x2 matrix.
        force_inline point2d<T> vecRotateInverse(const point2d<T> &Vector) const
        {
            return point2d<T>(
                Vector.X*M[0] + Vector.Y*M[1],
                Vector.X*M[4] + Vector.Y*M[5]
            );
        }
        
        //! Returns the rotated specified vector. i.e. the vector will be multiplied with the 3x3 matrix.
        force_inline vector3d<T> vecRotate(const vector3d<T> &Vector) const
        {
            return vector3d<T>(
                Vector.X*M[0] + Vector.Y*M[4] + Vector.Z*M[ 8],
                Vector.X*M[1] + Vector.Y*M[5] + Vector.Z*M[ 9],
                Vector.X*M[2] + Vector.Y*M[6] + Vector.Z*M[10]
            );
        }
        
        //! Returns the inverse rotated specified vector. i.e. the vector will be multiplied with the inversed 3x3 matrix.
        force_inline vector3d<T> vecRotateInverse(const vector3d<T> &Vector) const
        {
            return vector3d<T>(
                Vector.X*M[0] + Vector.Y*M[1] + Vector.Z*M[ 2],
                Vector.X*M[4] + Vector.Y*M[5] + Vector.Z*M[ 6],
                Vector.X*M[8] + Vector.Y*M[9] + Vector.Z*M[10]
            );
        }
        
        //! Clear the matrix. i.e. each component is set to 0.
        force_inline void clear()
        {
            memset(M, 0, sizeof(M));
        }
        
        /**
        Loads the matrix's identity.
        \code
        (  1  0  0  0  )
        |  0  1  0  0  |
        |  0  0  1  0  |
        (  0  0  0  1  )
        \endcode
        */
        force_inline matrix4<T>& reset()
        {
            M[0] = 1; M[4] = 0; M[ 8] = 0; M[12] = 0;
            M[1] = 0; M[5] = 1; M[ 9] = 0; M[13] = 0;
            M[2] = 0; M[6] = 0; M[10] = 1; M[14] = 0;
            M[3] = 0; M[7] = 0; M[11] = 0; M[15] = 1;
            return *this;
        }
        
        /**
        Loads the matrix's identity and sets an initial position (xyz) and scaling (whp).
        \code
        (  w  0  0  x  )
        |  0  h  0  y  |
        |  0  0  d  z  |
        (  0  0  0  1  )
        \endcode
        */
        force_inline matrix4<T>& reset(const vector3d<T> &InitPosition, const vector3d<T> &InitScale = T(1))
        {
            M[0] = InitScale.X; M[4] = 0;           M[ 8] = 0;           M[12] = InitPosition.X;
            M[1] = 0;           M[5] = InitScale.Y; M[ 9] = 0;           M[13] = InitPosition.Y;
            M[2] = 0;           M[6] = 0;           M[10] = InitScale.Z; M[14] = InitPosition.Z;
            M[3] = 0;           M[7] = 0;           M[11] = 0;           M[15] = 1;
            return *this;
        }
        
        //! Multiplies this matrix with a 1x4 matrix
        force_inline void multiplySingleMatrix(const T (&Other)[4])
        {
            T Result[4];
            
            Result[0] = M[0]*Other[0] + M[4]*Other[1] + M[ 8]*Other[2] + M[12]*Other[3];
            Result[1] = M[1]*Other[0] + M[5]*Other[1] + M[ 9]*Other[2] + M[13]*Other[3];
            Result[2] = M[2]*Other[0] + M[6]*Other[1] + M[10]*Other[2] + M[14]*Other[3];
            Result[3] = M[3]*Other[0] + M[7]*Other[1] + M[11]*Other[2] + M[15]*Other[3];
            
            Other[0] = Result[0]; Other[1] = Result[1]; Other[2] = Result[2]; Other[3] = Result[3];
        }
        
        force_inline void matrixLookAt(const vector3d<T> &Position, const vector3d<T> &LookAt, const vector3d<T> &upVector)
        {
            vector3d<T> ZAxis = LookAt - Position;
            ZAxis.normalize();
            
            vector3d<T> XAxis = upVector.cross(ZAxis);
            XAxis.normalize();
            
            vector3d<T> YAxis = ZAxis.cross(XAxis);
            
            M[0] = XAxis.X; M[4] = XAxis.Y; M[ 8] = XAxis.Z; M[12] = -XAxis.dot(Position);
            M[1] = YAxis.X; M[5] = YAxis.Y; M[ 9] = YAxis.Z; M[13] = -YAxis.dot(Position);
            M[2] = ZAxis.X; M[6] = ZAxis.Y; M[10] = ZAxis.Z; M[14] = -ZAxis.dot(Position);
            M[3] = 0;       M[7] = 0;       M[11] = 0;       M[15] = 1;
        }
        
        force_inline bool getInverse(matrix4<T> &InverseMat) const
        {
            const matrix4<T> &m = *this;
            
            T d = (
                (m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0)) * (m(2, 2) * m(3, 3) - m(2, 3) * m(3, 2)) -
                (m(0, 0) * m(1, 2) - m(0, 2) * m(1, 0)) * (m(2, 1) * m(3, 3) - m(2, 3) * m(3, 1)) +
                (m(0, 0) * m(1, 3) - m(0, 3) * m(1, 0)) * (m(2, 1) * m(3, 2) - m(2, 2) * m(3, 1)) +
                (m(0, 1) * m(1, 2) - m(0, 2) * m(1, 1)) * (m(2, 0) * m(3, 3) - m(2, 3) * m(3, 0)) -
                (m(0, 1) * m(1, 3) - m(0, 3) * m(1, 1)) * (m(2, 0) * m(3, 2) - m(2, 2) * m(3, 0)) +
                (m(0, 2) * m(1, 3) - m(0, 3) * m(1, 2)) * (m(2, 0) * m(3, 1) - m(2, 1) * m(3, 0))
            );
            
            if (d == T(0))
                return false;
            
            d = T(1) / d;
            
            InverseMat(0, 0) = d * ( m(1, 1) * (m(2, 2) * m(3, 3) - m(2, 3) * m(3, 2)) + m(1, 2) * (m(2, 3) * m(3, 1) - m(2, 1) * m(3, 3)) + m(1, 3) * (m(2, 1) * m(3, 2) - m(2, 2) * m(3, 1)) );
            InverseMat(0, 1) = d * ( m(2, 1) * (m(0, 2) * m(3, 3) - m(0, 3) * m(3, 2)) + m(2, 2) * (m(0, 3) * m(3, 1) - m(0, 1) * m(3, 3)) + m(2, 3) * (m(0, 1) * m(3, 2) - m(0, 2) * m(3, 1)) );
            InverseMat(0, 2) = d * ( m(3, 1) * (m(0, 2) * m(1, 3) - m(0, 3) * m(1, 2)) + m(3, 2) * (m(0, 3) * m(1, 1) - m(0, 1) * m(1, 3)) + m(3, 3) * (m(0, 1) * m(1, 2) - m(0, 2) * m(1, 1)) );
            InverseMat(0, 3) = d * ( m(0, 1) * (m(1, 3) * m(2, 2) - m(1, 2) * m(2, 3)) + m(0, 2) * (m(1, 1) * m(2, 3) - m(1, 3) * m(2, 1)) + m(0, 3) * (m(1, 2) * m(2, 1) - m(1, 1) * m(2, 2)) );
            InverseMat(1, 0) = d * ( m(1, 2) * (m(2, 0) * m(3, 3) - m(2, 3) * m(3, 0)) + m(1, 3) * (m(2, 2) * m(3, 0) - m(2, 0) * m(3, 2)) + m(1, 0) * (m(2, 3) * m(3, 2) - m(2, 2) * m(3, 3)) );
            InverseMat(1, 1) = d * ( m(2, 2) * (m(0, 0) * m(3, 3) - m(0, 3) * m(3, 0)) + m(2, 3) * (m(0, 2) * m(3, 0) - m(0, 0) * m(3, 2)) + m(2, 0) * (m(0, 3) * m(3, 2) - m(0, 2) * m(3, 3)) );
            InverseMat(1, 2) = d * ( m(3, 2) * (m(0, 0) * m(1, 3) - m(0, 3) * m(1, 0)) + m(3, 3) * (m(0, 2) * m(1, 0) - m(0, 0) * m(1, 2)) + m(3, 0) * (m(0, 3) * m(1, 2) - m(0, 2) * m(1, 3)) );
            InverseMat(1, 3) = d * ( m(0, 2) * (m(1, 3) * m(2, 0) - m(1, 0) * m(2, 3)) + m(0, 3) * (m(1, 0) * m(2, 2) - m(1, 2) * m(2, 0)) + m(0, 0) * (m(1, 2) * m(2, 3) - m(1, 3) * m(2, 2)) );
            InverseMat(2, 0) = d * ( m(1, 3) * (m(2, 0) * m(3, 1) - m(2, 1) * m(3, 0)) + m(1, 0) * (m(2, 1) * m(3, 3) - m(2, 3) * m(3, 1)) + m(1, 1) * (m(2, 3) * m(3, 0) - m(2, 0) * m(3, 3)) );
            InverseMat(2, 1) = d * ( m(2, 3) * (m(0, 0) * m(3, 1) - m(0, 1) * m(3, 0)) + m(2, 0) * (m(0, 1) * m(3, 3) - m(0, 3) * m(3, 1)) + m(2, 1) * (m(0, 3) * m(3, 0) - m(0, 0) * m(3, 3)) );
            InverseMat(2, 2) = d * ( m(3, 3) * (m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0)) + m(3, 0) * (m(0, 1) * m(1, 3) - m(0, 3) * m(1, 1)) + m(3, 1) * (m(0, 3) * m(1, 0) - m(0, 0) * m(1, 3)) );
            InverseMat(2, 3) = d * ( m(0, 3) * (m(1, 1) * m(2, 0) - m(1, 0) * m(2, 1)) + m(0, 0) * (m(1, 3) * m(2, 1) - m(1, 1) * m(2, 3)) + m(0, 1) * (m(1, 0) * m(2, 3) - m(1, 3) * m(2, 0)) );
            InverseMat(3, 0) = d * ( m(1, 0) * (m(2, 2) * m(3, 1) - m(2, 1) * m(3, 2)) + m(1, 1) * (m(2, 0) * m(3, 2) - m(2, 2) * m(3, 0)) + m(1, 2) * (m(2, 1) * m(3, 0) - m(2, 0) * m(3, 1)) );
            InverseMat(3, 1) = d * ( m(2, 0) * (m(0, 2) * m(3, 1) - m(0, 1) * m(3, 2)) + m(2, 1) * (m(0, 0) * m(3, 2) - m(0, 2) * m(3, 0)) + m(2, 2) * (m(0, 1) * m(3, 0) - m(0, 0) * m(3, 1)) );
            InverseMat(3, 2) = d * ( m(3, 0) * (m(0, 2) * m(1, 1) - m(0, 1) * m(1, 2)) + m(3, 1) * (m(0, 0) * m(1, 2) - m(0, 2) * m(1, 0)) + m(3, 2) * (m(0, 1) * m(1, 0) - m(0, 0) * m(1, 1)) );
            InverseMat(3, 3) = d * ( m(0, 0) * (m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1)) + m(0, 1) * (m(1, 2) * m(2, 0) - m(1, 0) * m(2, 2)) + m(0, 2) * (m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0)) );
            
            return true;
        }
        
        force_inline bool setInverse()
        {
            matrix4<T> Matrix;
            
            if (getInverse(Matrix))
            {
                *this = Matrix;
                return true;
            }
            
            return false;
        }
        
        force_inline matrix4<T> getInverse() const
        {
            matrix4<T> Mat;
            getInverse(Mat);
            return Mat;
        }
        
        /*
        (  1  0  0  x  )
        |  0  1  0  y  |
        |  0  0  1  z  |
        (  0  0  0  1  )
        */
        force_inline matrix4<T>& translate(const vector3d<T> &Vector)
        {
            matrix4<T> Other;
            
            Other[12] = Vector.X;
            Other[13] = Vector.Y;
            Other[14] = Vector.Z;
            
            return *this *= Other;
        }
        
        /*
        (  x  0  0  0  )
        |  0  y  0  0  |
        |  0  0  z  0  |
        (  0  0  0  1  )
        */
        force_inline matrix4<T>& scale(const vector3d<T> &Vector)
        {
            matrix4<T> Other;
            
            Other[ 0] = Vector.X;
            Other[ 5] = Vector.Y;
            Other[10] = Vector.Z;
            
            return *this *= Other;
        }
        
        /*
        ( xx(1-c)+c   xy(1-c)-zs  xz(1-c)+ys  0 )
        | yx(1-c)+zs  yy(1-c)+c   yz(1-c)-xs  0 |
        | xz(1-c)-ys  yz(1-c)+xs  zz(1-c)+c   0 |
        (     0           0           0       1 )
        */
        force_inline matrix4<T>& rotate(const T Angle, vector3d<T> Rotation)
        {
            matrix4<T> Other;
            
            /* Normalize the vector */
            Rotation.normalize();
            
            /* Setup rotation degrees */
            T x     = Rotation.X;
            T y     = Rotation.Y;
            T z     = Rotation.Z;
            T c     = Cos(Angle);
            T s     = Sin(Angle);
            T cc    = static_cast<T>(1) - c;
            
            /* Rotation */
            Other[0] = x*x*cc + c;   Other[4] = x*y*cc - z*s; Other[ 8] = x*z*cc + y*s; Other[12] = 0;
            Other[1] = y*x*cc + z*s; Other[5] = y*y*cc + c;   Other[ 9] = y*z*cc - x*s; Other[13] = 0;
            Other[2] = x*z*cc - y*s; Other[6] = y*z*cc + x*s; Other[10] = z*z*cc + c;   Other[14] = 0;
            Other[3] = 0;            Other[7] = 0;            Other[11] = 0;            Other[15] = 1;
            
            return *this *= Other;
        }
        
        force_inline matrix4<T>& rotateX(const T Angle)
        {
            matrix4<T> Other;
            
            const T c = Cos(Angle);
            const T s = Sin(Angle);
            
            /* Rotation */
            Other[0] = 1; Other[4] = 0; Other[ 8] =  0; Other[12] = 0;
            Other[1] = 0; Other[5] = c; Other[ 9] = -s; Other[13] = 0;
            Other[2] = 0; Other[6] = s; Other[10] =  c; Other[14] = 0;
            Other[3] = 0; Other[7] = 0; Other[11] =  0; Other[15] = 1;
            
            return *this *= Other;
        }
        
        force_inline matrix4<T>& rotateY(const T Angle)
        {
            matrix4<T> Other;
            
            const T c = Cos(Angle);
            const T s = Sin(Angle);
            
            /* Rotation */
            Other[0] =  c; Other[4] = 0; Other[ 8] = s; Other[12] = 0;
            Other[1] =  0; Other[5] = 1; Other[ 9] = 0; Other[13] = 0;
            Other[2] = -s; Other[6] = 0; Other[10] = c; Other[14] = 0;
            Other[3] =  0; Other[7] = 0; Other[11] = 0; Other[15] = 1;
            
            return *this *= Other;
        }
        
        force_inline matrix4<T>& rotateZ(const T Angle)
        {
            matrix4<T> Other;
            
            const T c = Cos(Angle);
            const T s = Sin(Angle);
            
            /* Rotation */
            Other[0] = c; Other[4] = -s; Other[ 8] = 0; Other[12] = 0;
            Other[1] = s; Other[5] =  c; Other[ 9] = 0; Other[13] = 0;
            Other[2] = 0; Other[6] =  0; Other[10] = 1; Other[14] = 0;
            Other[3] = 0; Other[7] =  0; Other[11] = 0; Other[15] = 1;
            
            return *this *= Other;
        }
        
        force_inline void rotateYXZ(const vector3df &Rotation)
        {
            rotateY(Rotation.Y);
            rotateX(Rotation.X);
            rotateZ(Rotation.Z);
        }
        
        force_inline void rotateZXY(const vector3df &Rotation)
        {
            rotateZ(Rotation.Z);
            rotateX(Rotation.X);
            rotateY(Rotation.Y);
        }
        
        force_inline void setRotation(vector3d<T> Rotation, bool UseDegrees = true)
        {
            if (UseDegrees)
                Rotation = Rotation * DEG;
            
            /* Setup rotation degrees */
            const T cx = cos(Rotation.X);
            const T sx = sin(Rotation.X);
            const T cy = cos(Rotation.Y);
            const T sy = sin(Rotation.Y);
            const T cz = cos(Rotation.Z);
            const T sz = sin(Rotation.Z);
            
            const T sxy = sx*sy;
            const T cxy = cx*sy;
            
            /* Rotation */
            M[ 0] = cy*cz;
            M[ 1] = cy*sz;
            M[ 2] = -sy;
            
            M[ 4] = sxy*cz - cx*sz;
            M[ 5] = sxy*sz + cx*cz;
            M[ 6] = sx*cy;
            
            M[ 8] = cxy*cz + sx*sz;
            M[ 9] = cxy*sz - sx*cz;
            M[10] = cx*cy;
        }
        
        force_inline void setInverseRotation(vector3df Rotation, bool UseDegrees = true)
        {
            if (UseDegrees)
                Rotation = Rotation * static_cast<T>(M_PI / 180.0);
            
            /* Setup rotation degrees */
            const T cx = cos(Rotation.X);
            const T sx = sin(Rotation.X);
            const T cy = cos(Rotation.Y);
            const T sy = sin(Rotation.Y);
            const T cz = cos(Rotation.Z);
            const T sz = sin(Rotation.Z);
            
            const T sxy = sx*sy;
            const T cxy = cx*sy;
            
            /* Rotation */
            M[ 0] = cy*cz;
            M[ 4] = cy*sz;
            M[ 8] = -sy;
            
            M[ 1] = sxy*cz - cx*sz;
            M[ 5] = sxy*sz + cx*cz;
            M[ 9] = sx*cy;
            
            M[ 2] = cxy*cz + sx*sz;
            M[ 6] = cxy*sz - sx*cz;
            M[10] = cx*cy;
        }
        
        force_inline void setTextureRotation(const T &Degree)
        {
            /* Setup rotation degrees */
            const T c = Cos(Degree);
            const T s = Sin(Degree);
            
            M[0] = c;
            M[1] = s;
            M[2] = static_cast<T>(-0.5 * ( c + s) + 0.5);
            
            M[4] = -s;
            M[5] = c;
            M[6] = static_cast<T>(-0.5 * (-s + c) + 0.5);
        }
        
        /* === Projection functions === */
        
        // makePerspectiveLH / RH
        // makeOrthoLH / RH
        
        force_inline void setPerspectiveLH(T FieldOfView, T Aspect, T Near, T Far)
        {
            const T h = static_cast<T>(1.0 / tan((FieldOfView * DEG64) / T(2)));
            const T w = h / Aspect;
            
            const T dif = Far - Near;
            
            M[ 0] = w;
            M[ 1] = 0;
            M[ 2] = 0;
            M[ 3] = 0;
            
            M[ 4] = 0;
            M[ 5] = h;
            M[ 6] = 0;
            M[ 7] = 0;
            
            M[ 8] = 0;
            M[ 9] = 0;
            M[10] = Far/dif;                // DirectX version
            //M[10] = (Far + Near)/dif;     // OpenGL version
            M[11] = 1;
            
            M[12] = 0;
            M[13] = 0;
            M[14] = (-Near*Far)/dif;        // DirectX version
            //M[14] = (-Near*Far*2)/dif;    // OpenGL version
            M[15] = 0;
        }
        
        force_inline void setPerspectiveRH(T FieldOfView, T Aspect, T Near, T Far)
        {
            const T h = static_cast<T>(1.0 / tan((FieldOfView * DEG64) / T(2)));
            const T w = h / Aspect;
            
            const T dif = Near - Far;
            
            M[ 0] = w;
            M[ 1] = 0;
            M[ 2] = 0;
            M[ 3] = 0;
            
            M[ 4] = 0;
            M[ 5] = h;
            M[ 6] = 0;
            M[ 7] = 0;
            
            M[ 8] = 0;
            M[ 9] = 0;
            M[10] = Far/dif;            // DirectX version
            //M[10] = (Far + Near)/dif; // OpenGL version
            M[11] = -1;
            
            M[12] = 0;
            M[13] = 0;
            M[14] = Near*Far/dif;       // DirectX version
            //M[14] = (Near*Far*2)/dif; // OpenGL version
            M[15] = 0;
        }
        
        force_inline void setOrthoLH(T Left, T Right, T Top, T Bottom, T Near, T Far)
        {
            M[ 0] = T(2)/(Right - Left);
            M[ 1] = 0;
            M[ 2] = 0;
            M[ 3] = 0;
            
            M[ 4] = 0;
            M[ 5] = T(2)/(Bottom - Top);
            M[ 6] = 0;
            M[ 7] = 0;
            
            M[ 8] = 0;
            M[ 9] = 0;
            M[10] = T(1)/(Far - Near);
            M[11] = 0;
            
            M[12] = 0;
            M[13] = 0;
            M[14] = -Near/(Far - Near);             // DirectX version
            //M[14] = -(Far + Near)/(Far - Near);   // OpenGL version
            M[15] = 1;
        }
        
        force_inline void setOrthoRH(T Left, T Right, T Top, T Bottom, T Near, T Far)
        {
            M[ 0] = T(2)/(Right - Left);
            M[ 1] = 0;
            M[ 2] = 0;
            M[ 3] = 0;
            
            M[ 4] = 0;
            M[ 5] = T(2)/(Bottom - Top);
            M[ 6] = 0;
            M[ 7] = 0;
            
            M[ 8] = 0;
            M[ 9] = 0;
            M[10] = T(1)/(Near - Far);
            M[11] = 0;
            
            M[12] = 0;
            M[13] = 0;
            M[14] = Near/(Near - Far);              // DirectX version
            //M[14] = (Far + Near)/(Near - Far);    // OpenGL version
            M[15] = 1;
        }
        
        force_inline void make2Dimensional(int32_t Width, int32_t Height, int32_t ScreenWidth, int32_t ScreenHeight)
        {
            reset();
            scale(
                vector3d<T>(static_cast<T>(1.0 / (Width/2)), static_cast<T>(1.0 / (Height/2)), 1)
            );
            translate(
                vector3d<T>(static_cast<T>(-ScreenWidth/2), static_cast<T>(-ScreenHeight/2), 0)
            );
        }
        
        force_inline void makeViewport(const rect2di &Viewport, const float DepthScale = 1.0f)
        {
            const T Width   = static_cast<T>( ( Viewport.Right - Viewport.Left - 0.75 ) / 2 );
            const T Height  = static_cast<T>( ( Viewport.Bottom - Viewport.Top - 0.75 ) / 2 );
            
            const T PosX    = static_cast<T>( -0.5f + ( Viewport.Left + Viewport.Right ) / 2 );
            const T PosY    = static_cast<T>( -0.5f + ( Viewport.Top + Viewport.Bottom ) / 2 );
            
            M[ 0] = Width;
            M[ 1] = 0;
            M[ 2] = 0;
            M[ 3] = 0;
            
            M[ 4] = 0;
            M[ 5] = -Height;
            M[ 6] = 0;
            M[ 7] = 0;
            
            M[ 8] = 0;
            M[ 9] = 0;
            M[10] = DepthScale;
            M[11] = 0;
            
            M[12] = PosX;
            M[13] = PosY;
            M[14] = 0;
            M[15] = 0;
        }
        
        /* === Row & columns === */
        
        force_inline vector4d<T> getRow(int32_t Position) const
        {
            switch (Position)
            {
                case 0:
                    return vector4d<T>(M[0], M[4], M[ 8], M[12]);
                case 1:
                    return vector4d<T>(M[1], M[5], M[ 9], M[13]);
                case 2:
                    return vector4d<T>(M[2], M[6], M[10], M[14]);
                case 3:
                    return vector4d<T>(M[3], M[7], M[11], M[15]);
            }
            return vector4d<T>();
        }
        
        force_inline void setRow(int32_t Position, const vector4d<T> &Vec)
        {
            switch (Position)
            {
                case 0:
                    M[0] = Vec.X, M[4] = Vec.Y, M[ 8] = Vec.Z, M[12] = Vec.W;
                    break;
                case 1:
                    M[1] = Vec.X, M[5] = Vec.Y, M[ 9] = Vec.Z, M[13] = Vec.W;
                    break;
                case 2:
                    M[2] = Vec.X, M[6] = Vec.Y, M[10] = Vec.Z, M[14] = Vec.W;
                    break;
                case 3:
                    M[3] = Vec.X, M[7] = Vec.Y, M[11] = Vec.Z, M[15] = Vec.W;
                    break;
            }
        }
        
        force_inline vector4d<T> getColumn(int32_t Position) const
        {
            switch (Position)
            {
                case 0:
                    return vector4d<T>(M[ 0], M[ 1], M[ 2], M[ 3]);
                case 1:
                    return vector4d<T>(M[ 4], M[ 5], M[ 6], M[ 7]);
                case 2:
                    return vector4d<T>(M[ 8], M[ 9], M[10], M[11]);
                case 3:
                    return vector4d<T>(M[12], M[13], M[14], M[15]);
            }
            return vector4d<T>();
        }
        
        force_inline void setColumn(int32_t Position, const vector4d<T> &Vec)
        {
            switch (Position)
            {
                case 0:
                    M[ 0] = Vec.X, M[ 1] = Vec.Y, M[ 2] = Vec.Z, M[ 3] = Vec.W;
                    break;
                case 1:
                    M[ 4] = Vec.X, M[ 5] = Vec.Y, M[ 6] = Vec.Z, M[ 7] = Vec.W;
                    break;
                case 2:
                    M[ 8] = Vec.X, M[ 9] = Vec.Y, M[10] = Vec.Z, M[11] = Vec.W;
                    break;
                case 3:
                    M[12] = Vec.X, M[13] = Vec.Y, M[14] = Vec.Z, M[15] = Vec.W;
                    break;
            }
        }
        
        force_inline void setPosition(const vector3d<T> &Position)
        {
            M[12] = Position.X; M[13] = Position.Y; M[14] = Position.Z;
        }
        force_inline vector3d<T> getPosition() const
        {
            return vector3d<T>(M[12], M[13], M[14]);
        }
        
        // Sets the matrix scaling vector.
        force_inline void setScale(const vector3d<T> &Scale)
        {
            vector3d<T> XAxis(M[0], M[1], M[ 2]);
            vector3d<T> YAxis(M[4], M[5], M[ 6]);
            vector3d<T> ZAxis(M[8], M[9], M[10]);
            
            XAxis.setLength(Scale.X);
            YAxis.setLength(Scale.Y);
            ZAxis.setLength(Scale.Z);
            
            M[0] = XAxis.X, M[1] = XAxis.Y, M[ 2] = XAxis.Z;
            M[4] = YAxis.X, M[5] = YAxis.Y, M[ 6] = YAxis.Z;
            M[8] = ZAxis.X, M[9] = ZAxis.Y, M[10] = ZAxis.Z;
        }
        //! Returns the matrix scaling vector.
        force_inline vector3d<T> getScale() const
        {
            if (Equal(M[1], 0.0f) && Equal(M[2], 0.0f) &&
                Equal(M[4], 0.0f) && Equal(M[6], 0.0f) &&
                Equal(M[8], 0.0f) && Equal(M[9], 0.0f))
            {
                return vector3d<T>(M[0], M[5], M[10]);
            }
            
            return vector3d<T>(
                sqrtf(M[0]*M[0] + M[1]*M[1] + M[ 2]*M[ 2]),
                sqrtf(M[4]*M[4] + M[5]*M[5] + M[ 6]*M[ 6]),
                sqrtf(M[8]*M[8] + M[9]*M[9] + M[10]*M[10])
            );
        }
        
        force_inline vector3d<T> getRotation() const
        {
            const matrix4<T> &Mat       = *this;
            const vector3d<T> Scale     = getScale();
            const vector3d<T> InvScale  = vector3d<T>(T(1)/Scale.X, T(1)/Scale.Y, T(1)/Scale.Z);
            
            T X, Y, Z, rotx, roty, C;
            
            Y = -asin(Mat[2]*InvScale.X);
            C = cos(Y);
            Y *= static_cast<T>(RAD64);
            
            if (!Equal(C, T(0)))
            {
                C       = T(1) / C;
                rotx    = Mat[10] * C * InvScale.Z;
                roty    = Mat[6] * C * InvScale.Y;
                X       = atan2(roty, rotx) * static_cast<T>(RAD64);
                rotx    = Mat[0] * C * InvScale.X;
                roty    = Mat[1] * C * InvScale.X;
                Z       = atan2(roty, rotx) * static_cast<T>(RAD64);
            }
            else
            {
                X       = T(0);
                rotx    = Mat[5] * InvScale.Y;
                roty    = -Mat[4] * InvScale.Y;
                Z       = atan2(roty, rotx) * static_cast<T>(RAD64);
            }
            
            if (X < 0) X += 360;
            if (Y < 0) Y += 360;
            if (Z < 0) Z += 360;
            
            return vector3d<T>(X, Y, Z);
        }
        
        force_inline matrix4<T> getRotationMatrix() const
        {
            matrix4<T> Matrix(
                M[0], M[4], M[ 8], 0,
                M[1], M[5], M[ 9], 0,
                M[2], M[6], M[10], 0,
                0, 0, 0, 1
            );
            
            const vector3d<T> Scale(getScale());
            Matrix.scale(vector3d<T>(1) / Scale);
            
            return Matrix;
        }
        
        force_inline matrix4<T> getPositionMatrix() const
        {
            return matrix4<T>(
                1, 0, 0, M[12],
                0, 1, 0, M[13],
                0, 0, 1, M[14],
                0, 0, 0, 1
            );
        }
        
        force_inline matrix4<T> getPositionScaleMatrix() const
        {
            const vector3d<T> Scale(getScale());
            return matrix4<T>(
                Scale.X, 0, 0, M[12],
                0, Scale.Y, 0, M[13],
                0, 0, Scale.Z, M[14],
                0, 0, 0, 1
            );
        }
        
        force_inline matrix4<T> getPositionRotationMatrix() const
        {
            const vector3d<T> Scale(getScale());
            
            matrix4<T> Matrix(*this);
            Matrix.scale(vector3d<T>(1) / Scale);
            
            return Matrix;
        }
        
        force_inline matrix4<T> getTransposed() const
        {
            matrix4<T> Mat;
            getTransposed(Mat);
            return Mat;
        }
        
        force_inline void getTransposed(matrix4<T> &Other) const
        {
            Other[0] = M[ 0]; Other[4] = M[ 1]; Other[ 8] = M[ 2]; Other[12] = M[ 3];
            Other[1] = M[ 4]; Other[5] = M[ 5]; Other[ 9] = M[ 6]; Other[13] = M[ 7];
            Other[2] = M[ 8]; Other[6] = M[ 9]; Other[10] = M[10]; Other[14] = M[11];
            Other[3] = M[12]; Other[7] = M[13]; Other[11] = M[14]; Other[15] = M[15];
        }
        
        force_inline matrix4<T> getTextureMatrix() const
        {
            matrix4<T> Mat;
            
            Mat[ 0] = M[ 0]; Mat[ 1] = M[ 1]; Mat[ 2] = M[ 3];
            Mat[ 4] = M[ 4]; Mat[ 5] = M[ 5]; Mat[ 6] = M[ 7];
            Mat[ 8] = M[12]; Mat[ 9] = M[13]; Mat[10] = M[15];
            
            return Mat;
        }
        
        force_inline matrix4<T> interpolate(const matrix4<T> &Other, float seek) const
        {
            matrix4<T> Mat;
            
            for (int32_t i = 0; i < 16; ++i)
                Mat.M[i] = M[i] + (Other.M[i] - M[i]) * seek;
            
            return Mat;
        }
        
        force_inline bool isIdentity() const
        {
            for (int32_t i = 0, j; i < 4; ++i)
            {
                for (j = 0; j < 4; ++j)
                {
                    if ( ( i != j && !Equal((*this)(i, j), 0.0f) ) ||
                         ( i == j && !Equal((*this)(i, j), 1.0f) ) )
                    {
                        return false;
                    }
                }
            }
            
            return true;
        }
        
        force_inline bool equal(const matrix4<T> &Other) const
        {
            for (int32_t i = 0; i < 16; ++i)
            {
                if (Equal(M[i], Other.M[i]))
                    return false;
            }
            return true;
        }
        
        force_inline const T* getArray() const
        {
            return M;
        }
        force_inline T* getArray()
        {
            return M;
        }
        
        force_inline matrix3<T> get3x3() const
        {
            return matrix3<T>(
                M[0], M[4], M[ 8],
                M[1], M[5], M[ 9],
                M[2], M[6], M[10]
            );
        }
        force_inline matrix2<T> get2x2() const
        {
            return matrix2<T>(
                M[0], M[4],
                M[1], M[5]
            );
        }
        
        template <typename B> force_inline matrix4<B> cast() const
        {
            matrix4<B> Other;
            
            for (uint32_t i = 0; i < 16; ++i)
                Other[i] = static_cast<B>(M[i]);
            
            return Other;
        }
        
        /* === Static functions === */
        
        static force_inline vector3d<T> getProjection(
            const vector3d<T> ObjectPosition, const rect2di &Viewport,
            const matrix4<T> &ProjectionMatrix, const matrix4<T> &ModelviewMatrix)
        {
            T v[4] = { ObjectPosition.X, ObjectPosition.Y, ObjectPosition.Z, 1 };
            
            matrix4<T>(ProjectionMatrix * ModelviewMatrix).multiplySingleMatrix(v);
            
            return vector3d<T>(
                Viewport.Left + Viewport.Right * (v[0] + 1) / 2,
                Viewport.Top + Viewport.Bottom * (v[1] + 1) / 2,
                (v[2] + 1) / 2
            );
        }
        
        /* === Member === */
        
        //! The matrix memory buffer.
        T M[16];
        
        /* === Macros === */
        
        static const matrix4<T> IDENTITY; //!< Identity 4x4 matrix.
        
};

typedef matrix4<float> matrix4f;
typedef matrix4<double> matrix4d;

template <typename T> const matrix4<T> matrix4<T>::IDENTITY;


/*
 * Templates
 */

template <typename T> force_inline matrix4<T> getRotationMatrix(const matrix4<T> &Mat)
{
    return matrix4<T>(
        Mat[0], Mat[4], Mat[ 8], 0,
        Mat[1], Mat[5], Mat[ 9], 0,
        Mat[2], Mat[6], Mat[10], 0,
        0, 0, 0, 1
    );
}
template <typename T> force_inline matrix4<T> getScaleMatrix(matrix4<T> Mat)
{
    Mat.setPosition(0);
    
    vector3df Rot(Mat.getRotation());
    Mat.matrixRotateYXZ(-Rot);
    
    return Mat;
}

template <typename T> force_inline matrix4<T> getRotationMatrix(const vector3d<T> &Rotation)
{
    matrix4<T> Mat;
    Mat.setRotation(Rotation);
    return Mat;
}

template <typename T> force_inline matrix4<T> getPositionMatrix(const vector3d<T> &Position)
{
    matrix4<T> Mat;
    Mat.setPosition(Position);
    return Mat;
}

template <typename T> force_inline matrix4<T> getScaleMatrix(const vector3d<T> &Scale)
{
    matrix4<T> Mat;
    Mat.setScale(Scale);
    return Mat;
}

template <typename T> force_inline matrix4<T> getDirectionMatrix(const vector3d<T> From, const vector3d<T> To)
{
    /* Temporary variables */
    T w = To.X - From.X;
    T h = To.Y - From.Y;
    T dx = getDistance(From, To);
    T dy = getDistance(point2df(From.X, From.Z), point2df(To.X, To.Z));
    T rx = 0;
    T ry = 0;
    
    /* Compute rotation */
    if (!Equal(From.Y, To.Y))
        rx = ASin(h/dx);
    
    if (!Equal(From.X, To.X))
        ry = -ASin(w/dy);
    
    if (From.Z < To.Z)
        ry = T(180) - ry;
    
    /* Process rotation */
    matrix4<T> Mat;
    Mat.translate(From);
    Mat.rotateY(ry);
    Mat.rotateX(rx);
    
    return Mat;
}


/////////////////////////////////////////////////////////////

template <typename T> class quaternion4
{
    
    public:
        
        force_inline quaternion4() :
            X(0),
            Y(0),
            Z(0),
            W(1)
        {
        }
        force_inline quaternion4(const T &x, const T &y, const T &z) :
            X(0),
            Y(0),
            Z(0),
            W(1)
        {
            set(x, y, z);
        }
        force_inline quaternion4(const T &x, const T &y, const T &z, const T &w) :
            X(x),
            Y(y),
            Z(z),
            W(w)
        {
        }
        force_inline quaternion4(const vector3d<T> &Vec) :
            X(0),
            Y(0),
            Z(0),
            W(1)
        {
            set(Vec);
        }
        force_inline quaternion4(const vector4d<T> &Vec) :
            X(Vec.X),
            Y(Vec.Y),
            Z(Vec.Z),
            W(Vec.W)
        {
        }
        force_inline quaternion4(const quaternion4<T> &Other) :
            X(Other.X),
            Y(Other.Y),
            Z(Other.Z),
            W(Other.W)
        {
        }
        force_inline quaternion4(const matrix4<T> &Matrix) :
            X(0),
            Y(0),
            Z(0),
            W(1)
        {
            setMatrix(Matrix);
        }
        force_inline ~quaternion4()
        {
        }
        
        /* === Operators - copying === */
        
        force_inline quaternion4<T>& operator = (const quaternion4<T> &Other)
        {
            set(Other.X, Other.Y, Other.Z, Other.W);
            return *this;
        }
        
        /* === Operators - comparisions === */
        
        force_inline bool operator == (const quaternion4<T> &Other) const
        {
            return X == Other.X && Y == Other.Y && Z == Other.Z && W == Other.W;
        }
        force_inline bool operator != (const quaternion4<T> &Other) const
        {
            return X != Other.X || Y != Other.Y || Z != Other.Z || W != Other.W;
        }
        
        force_inline bool operator < (const quaternion4<T> &Other) const
        {
            return (X == Other.X) ? ( (Y == Other.Y) ? ( (Z == Other.Z) ? W < Other.W : Z < Other.Z ) : Y < Other.Y ) : X < Other.X;
        }
        force_inline bool operator > (const quaternion4<T> &Other) const
        {
            return (X == Other.X) ? ( (Y == Other.Y) ? ( (Z == Other.Z) ? W > Other.W : Z > Other.Z ) : Y > Other.Y ) : X > Other.X;
        }
        
        force_inline bool operator <= (const quaternion4<T> &Other) const
        {
            return (X == Other.X) ? ( (Y == Other.Y) ? ( (Z == Other.Z) ? W <= Other.W : Z <= Other.Z ) : Y <= Other.Y ) : X <= Other.X;
        }
        force_inline bool operator >= (const quaternion4<T> &Other) const
        {
            return (X == Other.X) ? ( (Y == Other.Y) ? ( (Z == Other.Z) ? W >= Other.W : Z >= Other.Z ) : Y >= Other.Y ) : X >= Other.X;
        }
        
        /* === Operators - addition, subtraction, division, multiplication === */
        
        force_inline quaternion4<T> operator + (const quaternion4<T> &Other) const
        {
            return quaternion4<T>(X + Other.X, Y + Other.Y, Z + Other.Z, W + Other.W);
        }
        force_inline quaternion4<T>& operator += (const quaternion4<T> &Other)
        {
            X += Other.X; Y += Other.Y; Z += Other.Z; W += Other.W; return *this;
        }
        
        force_inline quaternion4<T> operator - (const quaternion4<T> &Other) const
        {
            return quaternion4<T>(X - Other.X, Y - Other.Y, Z - Other.Z, W - Other.W);
        }
        force_inline quaternion4<T>& operator -= (const quaternion4<T> &Other)
        {
            X -= Other.X; Y -= Other.Y; Z -= Other.Z; W -= Other.W; return *this;
        }
        
        force_inline quaternion4<T> operator / (const quaternion4<T> &Other) const
        {
            return quaternion4<T>(X / Other.X, Y / Other.Y, Z / Other.Z, W / Other.W);
        }
        force_inline quaternion4<T>& operator /= (const quaternion4<T> &Other)
        {
            X /= Other.X; Y /= Other.Y; Z /= Other.Z; W /= Other.W; return *this;
        }
        
        force_inline quaternion4<T> operator * (const quaternion4<T> &Other) const
        {
            quaternion4<T> tmp;
            
            tmp.W = (Other.W * W) - (Other.X * X) - (Other.Y * Y) - (Other.Z * Z);
            tmp.X = (Other.W * X) + (Other.X * W) + (Other.Y * Z) - (Other.Z * Y);
            tmp.Y = (Other.W * Y) + (Other.Y * W) + (Other.Z * X) - (Other.X * Z);
            tmp.Z = (Other.W * Z) + (Other.Z * W) + (Other.X * Y) - (Other.Y * X);
            
            return tmp;
        }
        force_inline quaternion4<T>& operator *= (const quaternion4<T> &Other)
        {
            *this = *this * Other; return *this;
        }
        
        force_inline vector3d<T> operator * (const vector3d<T> &Vector) const
        {
            vector3d<T> uv, uuv;
            vector3d<T> qvec(X, Y, Z);
            
            uv = qvec.cross(Vector);
            uuv = qvec.cross(uv);
            uv *= (T(2) * W);
            uuv *= T(2);
            
            /* Result := Vector + uv + uuv */
            uv += uuv;
            uv += Vector;
            return uv;
        }
        
        force_inline quaternion4 operator / (const T &Size) const
        {
            return quaternion4(X / Size, Y / Size, Z / Size, W / Size);
        }
        force_inline quaternion4& operator /= (const T &Size)
        {
            X /= Size; Y /= Size; Z /= Size; W /= Size; return *this;
        }
        
        force_inline quaternion4 operator * (const T &Size) const
        {
            return quaternion4(X * Size, Y * Size, Z * Size, W * Size);
        }
        force_inline quaternion4& operator *= (const T &Size)
        {
            X *= Size; Y *= Size; Z *= Size; W *= Size; return *this;
        }
        
        /* === Additional operators === */
        
        force_inline const T operator [] (uint32_t i) const
        {
            return i < 4 ? *(&X + i) : T(0);
        }
        
        force_inline T& operator [] (uint32_t i)
        {
            return *(&X + i);
        }
        
        /* === Extra functions === */
        
        force_inline T dot(const quaternion4<T> &Other) const
        {
            return X*Other.X + Y*Other.Y + Z*Other.Z + W*Other.W;
        }
        
        force_inline quaternion4<T>& normalize()
        {
            T n = X*X + Y*Y + Z*Z + W*W;
            
            if (n == T(1) || n == T(0))
                return *this;
            
            n = T(1) / sqrt(n);
            
            X *= n;
            Y *= n;
            Z *= n;
            W *= n;
            
            return *this;
        } 
        
        force_inline quaternion4& setInverse()
        {
            X = -X; Y = -Y; Z = -Z; return *this;
        }
        force_inline quaternion4 getInverse() const
        {
            return quaternion4(-X, -Y, -Z, W);
        }
        
        force_inline void set(const T &NewX, const T &NewY, const T &NewZ, const T &NewW)
        {
            X = NewX;
            Y = NewY;
            Z = NewZ;
            W = NewW;
        }
        
        force_inline void set(const T &NewX, const T &NewY, const T &NewZ)
        {
            const T cp = cos(NewX/2);
            const T cr = cos(NewZ/2);
            const T cy = cos(NewY/2);
            
            const T sp = sin(NewX/2);
            const T sr = sin(NewZ/2);
            const T sy = sin(NewY/2);
            
            const T cpcy = cp * cy;
            const T spsy = sp * sy;
            const T cpsy = cp * sy;
            const T spcy = sp * cy;
            
            X = sr * cpcy - cr * spsy;
            Y = cr * spcy + sr * cpsy;
            Z = cr * cpsy - sr * spcy;
            W = cr * cpcy + sr * spsy;
            
            normalize();
        }
        
        force_inline void set(const vector3d<T> &Vector)
        {
            set(Vector.X, Vector.Y, Vector.Z);
        }
        force_inline void set(const vector4d<T> &Vector)
        {
            set(Vector.X, Vector.Y, Vector.Z, Vector.W);
        }
        
        force_inline void getMatrix(matrix4<T> &Mat) const
        {
            Mat[ 0] = 1.0f - T(2)*Y*Y - T(2)*Z*Z;
            Mat[ 1] =        T(2)*X*Y + T(2)*Z*W;
            Mat[ 2] =        T(2)*X*Z - T(2)*Y*W;
            Mat[ 3] =        0.0f;
            
            Mat[ 4] =        T(2)*X*Y - T(2)*Z*W;
            Mat[ 5] = 1.0f - T(2)*X*X - T(2)*Z*Z;
            Mat[ 6] =        T(2)*Z*Y + T(2)*X*W;
            Mat[ 7] =        0.0f;
            
            Mat[ 8] =        T(2)*X*Z + T(2)*Y*W;
            Mat[ 9] =        T(2)*Z*Y - T(2)*X*W;
            Mat[10] = 1.0f - T(2)*X*X - T(2)*Y*Y;
            Mat[11] =        T(0);
            
            Mat[12] = T(0);
            Mat[13] = T(0);
            Mat[14] = T(0);
            Mat[15] = 1.0f;
        }
        
        force_inline matrix4<T> getMatrix() const
        {
            matrix4<T> Mat;
            getMatrix(Mat);
            return Mat;
        }
        
        force_inline void getMatrixTransposed(matrix4<T> &Mat) const
        {
            Mat[ 0] = T(1) - T(2)*Y*Y - T(2)*Z*Z;
            Mat[ 4] =        T(2)*X*Y + T(2)*Z*W;
            Mat[ 8] =        T(2)*X*Z - T(2)*Y*W;
            Mat[12] =        T(0);
            
            Mat[ 1] =        T(2)*X*Y - T(2)*Z*W;
            Mat[ 5] = T(1) - T(2)*X*X - T(2)*Z*Z;
            Mat[ 9] =        T(2)*Z*Y + T(2)*X*W;
            Mat[13] =        T(0);
            
            Mat[ 2] =        T(2)*X*Z + T(2)*Y*W;
            Mat[ 6] =        T(2)*Z*Y - T(2)*X*W;
            Mat[10] = T(1) - T(2)*X*X - T(2)*Y*Y;
            Mat[14] =        T(0);
            
            Mat[ 3] = T(0);
            Mat[ 7] = T(0);
            Mat[11] = T(0);
            Mat[15] = T(1);
        }
        
        force_inline matrix4<T> getMatrixTransposed() const
        {
            matrix4<T> Mat;
            getMatrixTransposed(Mat);
            return Mat;
        }
        
        force_inline void setMatrix(const matrix4<T> &Mat)
        {
            T trace = Mat(0, 0) + Mat(1, 1) + Mat(2, 2) + 1.0f;
            
            if (trace > T(0))
            {
                const T s = T(2) * sqrt(trace);
                X = (Mat(2, 1) - Mat(1, 2)) / s;
                Y = (Mat(0, 2) - Mat(2, 0)) / s;
                Z = (Mat(1, 0) - Mat(0, 1)) / s;
                W = T(0.25) * s;
            }
            else
            {
                if (Mat(0, 0) > Mat(1, 1) && Mat(0, 0) > Mat(2, 2))
                {
                    const T s = T(2) * sqrtf(1.0f + Mat(0, 0) - Mat(1, 1) - Mat(2, 2));
                    X = T(0.25) * s;
                    Y = (Mat(0, 1) + Mat(1, 0) ) / s;
                    Z = (Mat(2, 0) + Mat(0, 2) ) / s;
                    W = (Mat(2, 1) - Mat(1, 2) ) / s;
                }
                else if (Mat(1, 1) > Mat(2, 2))
                {
                    const T s = T(2) * sqrtf(1.0f + Mat(1, 1) - Mat(0, 0) - Mat(2, 2));
                    X = (Mat(0, 1) + Mat(1, 0) ) / s;
                    Y = T(0.25) * s;
                    Z = (Mat(1, 2) + Mat(2, 1) ) / s;
                    W = (Mat(0, 2) - Mat(2, 0) ) / s;
                }
                else
                {
                    const T s = T(2) * sqrtf(1.0f + Mat(2, 2) - Mat(0, 0) - Mat(1, 1));
                    X = (Mat(0, 2) + Mat(2, 0) ) / s;
                    Y = (Mat(1, 2) + Mat(2, 1) ) / s;
                    Z = T(0.25) * s;
                    W = (Mat(1, 0) - Mat(0, 1) ) / s;
                }
            }
            
            normalize();
        }
        
        force_inline quaternion4<T>& setAngleAxis(const T &Angle, const vector3d<T> &Axis)
        {
            const T HalfAngle   = T(0.5) * Angle;
            const T Sine        = sin(HalfAngle);
            
            X = Sine * Axis.X;
            Y = Sine * Axis.Y;
            Z = Sine * Axis.Z;
            W = cos(HalfAngle);
            
            return *this;
        }
        
        force_inline void getAngleAxis(T &Angle, vector3d<T> &Axis) const
        {
            const T Scale = sqrt(X*X + Y*Y + Z*Z);
            
            if ( ( Scale > T(-1.0e-6) && Scale < T(1.0e-6) ) || W > T(1) || W < T(-1) )
            {
                Axis.X  = T(0);
                Axis.Y  = 1.0f;
                Axis.Z  = T(0);
                Angle   = T(0);
            }
            else
            {
                const T InvScale = T(1) / Scale;
                Axis.X  = X * InvScale;
                Axis.Y  = Y * InvScale;
                Axis.Z  = Z * InvScale;
                Angle   = T(2) * acos(W);
            }
        }
        
        force_inline void getEuler(vector3d<T> &Euler) const
        {
            const T sqX = X*X;
            const T sqY = Y*Y;
            const T sqZ = Z*Z;
            const T sqW = W*W;
            
            T tmp = T(-2) * (X*Z - Y*W);
            
            Clamp(tmp, T(-1), T(1));
            
            Euler.X = atan2(T(2) * (Y*Z + X*W), -sqX - sqY + sqZ + sqW);
            Euler.Y = asin(tmp);
            Euler.Z = atan2(T(2) * (X*Y + Z*W), sqX - sqY - sqZ + sqW);
        }
        
        /*
         * Slerp: "spherical linear interpolation"
         * Smoothly (spherically, shortest path on a quaternion sphere) 
         * interpolates between two UNIT quaternion positions
         * slerp(p, q, t) = ( p*sin((1 - t)*omega) + q*sin(t*omega) ) / sin(omega)
         */
        force_inline void slerp(const quaternion4<T> &to, const T &t)
        {
            /* Temporary variables */
            T to1[4];
            T omega, cosom, sinom;
            T scale0, scale1;
            
            /* Calculate cosine */
            cosom = X*to.X + Y*to.Y + Z*to.Z + W*to.W;
            
            /* Adjust signs (if necessary) */
            if (cosom < T(0))
            {
                cosom = -cosom;
                to1[0] = -to.X;
                to1[1] = -to.Y;
                to1[2] = -to.Z;
                to1[3] = -to.W;
            }
            else
            {
                to1[0] = to.X;
                to1[1] = to.Y;
                to1[2] = to.Z;
                to1[3] = to.W;
            }
            
            /* Calculate coefficients */
            if ( ( T(1) - cosom ) > T(1e-10) )
            {
                /* Standard case (slerp) */
                omega = acos(cosom);
                sinom = sin(omega);
                scale0 = sin((T(1) - t) * omega) / sinom;
                scale1 = sin(t * omega) / sinom;
            }
            else
            {
                /*
                 * "from" and "to" quaternions are very close
                 *  ... so we can do a linear interpolation
                 */
                scale0 = T(1) - t;
                scale1 = t;
            }
            
            /* Calculate final values */
            X = scale0*X + scale1*to1[0];
            Y = scale0*Y + scale1*to1[1];
            Z = scale0*Z + scale1*to1[2];
            W = scale0*W + scale1*to1[3];
        }
        
        force_inline void slerp(const quaternion4<T> &from, const quaternion4<T> &to, const T &t)
        {
            /* Temporary variables */
            T to1[4];
            T omega, cosom, sinom;
            T scale0, scale1;
            
            /* Calculate cosine */
            cosom = from.X*to.X + from.Y*to.Y + from.Z*to.Z + from.W*to.W;
            
            /* Adjust signs (if necessary) */
            if (cosom < T(0))
            {
                cosom = -cosom;
                to1[0] = -to.X;
                to1[1] = -to.Y;
                to1[2] = -to.Z;
                to1[3] = -to.W;
            }
            else
            {
                to1[0] = to.X;
                to1[1] = to.Y;
                to1[2] = to.Z;
                to1[3] = to.W;
            }
            
            /* Calculate coefficients */
            if ((T(1) - cosom) > T(1e-10)) 
            {
                /* Standard case (slerp) */
                omega = acos(cosom);
                sinom = sin(omega);
                scale0 = sin((T(1) - t) * omega) / sinom;
                scale1 = sin(t * omega) / sinom;
            }
            else
            {        
                /*
                 * "from" and "to" quaternions are very close 
                 *  ... so we can do a linear interpolation
                 */
                scale0 = T(1) - t;
                scale1 = t;
            }

            /* Calculate final values */
            X = scale0*from.X + scale1*to1[0];
            Y = scale0*from.Y + scale1*to1[1];
            Z = scale0*from.Z + scale1*to1[2];
            W = scale0*from.W + scale1*to1[3];
        }
        
        force_inline void reset() // Load identity
        {
            X = Y = Z = T(0);
            W = T(1);
        }
        
        /* === Members === */
        
        T X, Y, Z, W;
        
};


typedef quaternion4<float> quaternion;    //<! for backwards compatibility.
typedef quaternion4<float> quaternion4f;
typedef quaternion4<double> quaternion4d;

/////////////////////////////////////////////////////////////

/* Extended defintions */ 

template <typename T> force_inline vector3d<T>::vector3d(const point2d<T> &Other) :
    X(Other.X   ),
    Y(Other.Y   ),
    Z(0         )
{
}

template <typename T> force_inline vector3d<T>::vector3d(const size2d<T> &Other) :
    X(Other.Width   ),
    Y(Other.Height  ),
    Z(1             )
{
}

template <typename T> force_inline point2d<T>::point2d(const vector3d<T> &Other) :
    X(Other.X),
    Y(Other.Y)
{
}

template <typename T> force_inline matrix4<T>::matrix4(const matrix3<T> &Other)
{
    M[0] = Other[0]; M[4] = Other[3]; M[ 8] = Other[6]; M[12] = 0;
    M[1] = Other[1]; M[5] = Other[4]; M[ 9] = Other[7]; M[13] = 0;
    M[2] = Other[2]; M[6] = Other[5]; M[10] = Other[8]; M[14] = 0;
    M[3] = 0;        M[7] = 0;        M[11] = 0;        M[15] = 1;
}

template <typename T> force_inline plane3d<T> aabbox3d<T>::getLeftPlane() const
{
    return plane3d<T>(vector3d<T>(-1, 0, 0), -Min.X);
}
template <typename T> force_inline plane3d<T> aabbox3d<T>::getRightPlane() const
{
    return plane3d<T>(vector3d<T>(1, 0, 0), Max.X);
}
template <typename T> force_inline plane3d<T> aabbox3d<T>::getTopPlane() const
{
    return plane3d<T>(vector3d<T>(0, 1, 0), Max.Y);
}
template <typename T> force_inline plane3d<T> aabbox3d<T>::getBottomPlane() const
{
    return plane3d<T>(vector3d<T>(0, -1, 0), -Min.Y);
}
template <typename T> force_inline plane3d<T> aabbox3d<T>::getFrontPlane() const
{
    return plane3d<T>(vector3d<T>(0, 0, -1), -Min.Z);
}
template <typename T> force_inline plane3d<T> aabbox3d<T>::getBackPlane() const
{
    return plane3d<T>(vector3d<T>(0, 0, 1), Max.Z);
}

template <typename T> force_inline bool obbox3d<T>::isPointInside(const vector3d<T> &Point) const
{
    return isInversePointInside(matrix4<T>(*this).getInverse() * Point);
}

template <typename T> force_inline bool obbox3d<T>::isBoxInside(const obbox3d<T> &Other) const
{
    const matrix4<T> OtherMat(Other);
    const matrix4<T> ThisMat(matrix4<T>(*this).getInverse());
    
    return
        isInversePointInside(ThisMat * (OtherMat * vector3d<T>( 1,  1,  1))) &&
        isInversePointInside(ThisMat * (OtherMat * vector3d<T>( 1,  1, -1))) &&
        isInversePointInside(ThisMat * (OtherMat * vector3d<T>( 1, -1,  1))) &&
        isInversePointInside(ThisMat * (OtherMat * vector3d<T>( 1, -1, -1))) &&
        isInversePointInside(ThisMat * (OtherMat * vector3d<T>(-1,  1,  1))) &&
        isInversePointInside(ThisMat * (OtherMat * vector3d<T>(-1,  1, -1))) &&
        isInversePointInside(ThisMat * (OtherMat * vector3d<T>(-1, -1,  1))) &&
        isInversePointInside(ThisMat * (OtherMat * vector3d<T>(-1, -1, -1)));
}

template <typename T> force_inline vector3d<T> aabbox3d<T>::getClosestPoint(const plane3d<T> &Plane) const
{
    vector3d<T> Result, Corner;
    
    T Distance = OMEGA;
    T Temp;
    
    for (int32_t i = 0; i < 8; ++i)
    {
        Corner = getCorner(i);
        
        Temp = Plane.getPointDistance(Corner);
        
        if (Temp < Distance)
        {
            Result = Corner;
            Distance = Temp;
        }
    }
    
    return Result;
}

/////////////////////////////////////////////////////////////

static force_inline std::string fitoa(int32_t number)
{
	char buff[100];
	tsnprintf(buff, sizeof(buff) - 1, "%d", number);
	return buff;
}

static force_inline std::string fitoa16(int32_t number)
{
	std::string ret;

	// temporary buffer for 16 numbers
	char tmpbuf[16]={0};
	uint32_t idx = 15;

	// special case '0'

	if (!number)
	{
		tmpbuf[14] = '0';
		ret = &tmpbuf[14];
		return ret;
	}

	// add numbers
	const uint8_t chars[]="0123456789ABCDEF";
	while(number && idx)
	{
		--idx;
		tmpbuf[idx] = chars[(number % 16)];
		number /= 16;
	}

	ret = &tmpbuf[idx];
	return ret;
}

static force_inline int32_t fatoi(const std::string & str)
{
	return atoi((const char *)str.c_str());
}

static force_inline int64_t fatol(const std::string & str)
{
	return atoll((const char *)str.c_str());
}

static force_inline double fatof(const std::string & str)
{
	return atof((const char *)str.c_str());
}

static force_inline int32_t fatoi(const char * str)
{
	return atoi(str);
}

static force_inline int64_t fatol(const char * str)
{
	return atoll(str);
}

static force_inline double fatof(const char * str)
{
	return atof(str);
}

static force_inline int32_t fatoi16(const std::string & str)
{
    int32_t ret = 0;   
    for (int32_t i = 0; i < (int32_t)str.size(); i++)   
    {		
        uint8_t c = str[i];	
        if (c >= '0' && c <= '9')	
        {		
            ret = ret * 16 + (c - '0');	
        }	
        else if (c >= 'A' && c <= 'F')		
        {		
            ret = ret * 16 + (c - 'A') + 10;	
        }  
        else if (c >= 'a' && c <= 'f')		
        {		
            ret = ret * 16 + (c - 'a') + 10;	
        }  
    }
    return ret;
}

/////////////////////////////////////////////////////////////

/* Constants for MD5_Transform routine. */
#define MD5_S11 7
#define MD5_S12 12
#define MD5_S13 17
#define MD5_S14 22
#define MD5_S21 5
#define MD5_S22 9
#define MD5_S23 14
#define MD5_S24 20
#define MD5_S31 4
#define MD5_S32 11
#define MD5_S33 16
#define MD5_S34 23
#define MD5_S41 6
#define MD5_S42 10
#define MD5_S43 15
#define MD5_S44 21

/* MD5_F, MD5_G, MD5_H and MD5_I are basic MD5 functions. */
#define MD5_F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | (~z)))

/* MD5_ROTATE_LEFT rotates x left n bits. */
#define MD5_ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32-(n))))

/* MD5_FF, MD5_GG, MD5_HH, and MD5_II transformations for rounds 1, 2, 3, and 4.
   Rotation is separate from addition to prevent recomputation. */
#define MD5_FF(a, b, c, d, x, s, ac) { \
 (a) += MD5_F ((b), (c), (d)) + (x) + (uint32_t)(ac); \
 (a) = MD5_ROTATE_LEFT ((a), (s)); \
 (a) += (b); \
  }
#define MD5_GG(a, b, c, d, x, s, ac) { \
 (a) += MD5_G ((b), (c), (d)) + (x) + (uint32_t)(ac); \
 (a) = MD5_ROTATE_LEFT ((a), (s)); \
 (a) += (b); \
  }
#define MD5_HH(a, b, c, d, x, s, ac) { \
 (a) += MD5_H ((b), (c), (d)) + (x) + (uint32_t)(ac); \
 (a) = MD5_ROTATE_LEFT ((a), (s)); \
 (a) += (b); \
  }
#define MD5_II(a, b, c, d, x, s, ac) { \
 (a) += MD5_I ((b), (c), (d)) + (x) + (uint32_t)(ac); \
 (a) = MD5_ROTATE_LEFT ((a), (s)); \
 (a) += (b); \
  }

/* MD5 initialization. Begins an MD5 operation, writing a new context. */
#define MD5_init()\
{\
    context_count[0] = context_count[1] = 0;\
    /* Load magic initialization constants. */\
    context_state[0] = 0x67452301;\
    context_state[1] = 0xefcdab89;\
    context_state[2] = 0x98badcfe;\
    context_state[3] = 0x10325476;\
}

/* MD5 block update operation. Continues an MD5 message-digest operation, 
   processing another message block, and updating the context. */
#define MD5_update(input, inputLen)\
{\
    uint32_t i, index, partLen;\
	\
    /* Compute number of bytes mod 64 */\
    index = (uint32_t) ((context_count[0] >> 3) & 0x3F);\
	\
    /* Update number of bits */\
    if ((context_count[0] += ((uint32_t) inputLen << 3)) < ((uint32_t) inputLen << 3))\
        context_count[1]++;\
    context_count[1] += ((uint32_t) inputLen >> 29);\
	\
    partLen = 64 - index;\
	\
    /* Transform as many times as possible. */\
    if (inputLen >= partLen) {\
        memcpy((void *) &context_buffer[index], (void *) input, partLen);\
        MD5_Transform(context_state, context_buffer);\
		\
        for (i = partLen; i + 63 < inputLen; i += 64)\
            MD5_Transform(context_state, (&input[i]));\
			\
        index = 0;\
    }\
    else\
        i = 0;\
		\
    /* Buffer remaining input */\
    memcpy((void *) &context_buffer[index], (void *) &input[i], inputLen - i);\
}

/* MD5 finalization. Ends an MD5 message-digest operation, writing the message digest and zeroizing the context. */
#define MD5_fini(digest)\
{\
    uint8_t bits[8];\
    uint32_t index, padLen;\
	\
    /* Save number of bits */\
    MD5_Encode(bits, context_count, 8);\
	\
    /* Pad out to 56 mod 64. */\
    index = (uint32_t) ((context_count[0] >> 3) & 0x3f);\
    padLen = (index < 56) ? (56 - index) : (120 - index);\
    MD5_update(PADDING, padLen);\
	\
    /* Append length (before padding) */\
    MD5_update(bits, 8);\
	\
    /* Store state in digest */\
    MD5_Encode(digest, context_state, 16);\
}

/* Encodes input (uint32_t) into output (uint8_t). Assumes len is a multiple of 4. */
#define MD5_Encode(output, input, len)\
{\
	uint32_t i, j;\
	\
	for (i = 0, j = 0; j < len; i++, j += 4) {\
		output[j] = (uint8_t) (input[i] & 0xff);\
		output[j + 1] = (uint8_t) ((input[i] >> 8) & 0xff);\
		output[j + 2] = (uint8_t) ((input[i] >> 16) & 0xff);\
		output[j + 3] = (uint8_t) ((input[i] >> 24) & 0xff);\
	}\
}

/* Decodes input (uint8_t) into output (uint32_t). Assumes len is a multiple of 4.*/
#define MD5_Decode(output, input, len)\
{\
	uint32_t i, j;\
	\
	for (i = 0, j = 0; j < len; i++, j += 4) {\
		output[i] = ((uint32_t) input[j]) | (((uint32_t) input[j + 1]) << 8) |\
			(((uint32_t) input[j + 2]) << 16) | (((uint32_t) input[j + 3]) << 24);\
	}\
}

/* MD5 basic transformation. Transforms state based on block. */
#define MD5_Transform(state, block)\
{\
	uint32_t a = state[0], \
		b = state[1], \
		c = state[2], \
		d = state[3], \
		x[16];\
		\
	MD5_Decode(x, block, 64);\
	\
	/* Round 1 */\
	MD5_FF(a, b, c, d, x[0], MD5_S11, 0xd76aa478);    /* 1 */\
	MD5_FF(d, a, b, c, x[1], MD5_S12, 0xe8c7b756);    /* 2 */\
	MD5_FF(c, d, a, b, x[2], MD5_S13, 0x242070db);    /* 3 */\
	MD5_FF(b, c, d, a, x[3], MD5_S14, 0xc1bdceee);    /* 4 */\
	MD5_FF(a, b, c, d, x[4], MD5_S11, 0xf57c0faf);    /* 5 */\
	MD5_FF(d, a, b, c, x[5], MD5_S12, 0x4787c62a);    /* 6 */\
	MD5_FF(c, d, a, b, x[6], MD5_S13, 0xa8304613);    /* 7 */\
	MD5_FF(b, c, d, a, x[7], MD5_S14, 0xfd469501);    /* 8 */\
	MD5_FF(a, b, c, d, x[8], MD5_S11, 0x698098d8);    /* 9 */\
	MD5_FF(d, a, b, c, x[9], MD5_S12, 0x8b44f7af);    /* 10 */\
	MD5_FF(c, d, a, b, x[10], MD5_S13, 0xffff5bb1);    /* 11 */\
	MD5_FF(b, c, d, a, x[11], MD5_S14, 0x895cd7be);    /* 12 */\
	MD5_FF(a, b, c, d, x[12], MD5_S11, 0x6b901122);    /* 13 */\
	MD5_FF(d, a, b, c, x[13], MD5_S12, 0xfd987193);    /* 14 */\
	MD5_FF(c, d, a, b, x[14], MD5_S13, 0xa679438e);    /* 15 */\
	MD5_FF(b, c, d, a, x[15], MD5_S14, 0x49b40821);    /* 16 */\
	\
	/* Round 2 */\
	MD5_GG(a, b, c, d, x[1], MD5_S21, 0xf61e2562);    /* 17 */\
	MD5_GG(d, a, b, c, x[6], MD5_S22, 0xc040b340);    /* 18 */\
	MD5_GG(c, d, a, b, x[11], MD5_S23, 0x265e5a51);    /* 19 */\
	MD5_GG(b, c, d, a, x[0], MD5_S24, 0xe9b6c7aa);    /* 20 */\
	MD5_GG(a, b, c, d, x[5], MD5_S21, 0xd62f105d);    /* 21 */\
	MD5_GG(d, a, b, c, x[10], MD5_S22, 0x2441453);    /* 22 */\
	MD5_GG(c, d, a, b, x[15], MD5_S23, 0xd8a1e681);    /* 23 */\
	MD5_GG(b, c, d, a, x[4], MD5_S24, 0xe7d3fbc8);    /* 24 */\
	MD5_GG(a, b, c, d, x[9], MD5_S21, 0x21e1cde6);    /* 25 */\
	MD5_GG(d, a, b, c, x[14], MD5_S22, 0xc33707d6);    /* 26 */\
	MD5_GG(c, d, a, b, x[3], MD5_S23, 0xf4d50d87);    /* 27 */\
	MD5_GG(b, c, d, a, x[8], MD5_S24, 0x455a14ed);    /* 28 */\
	MD5_GG(a, b, c, d, x[13], MD5_S21, 0xa9e3e905);    /* 29 */\
	MD5_GG(d, a, b, c, x[2], MD5_S22, 0xfcefa3f8);    /* 30 */\
	MD5_GG(c, d, a, b, x[7], MD5_S23, 0x676f02d9);    /* 31 */\
	MD5_GG(b, c, d, a, x[12], MD5_S24, 0x8d2a4c8a);    /* 32 */\
	\
	/* Round 3 */\
	MD5_HH(a, b, c, d, x[5], MD5_S31, 0xfffa3942);    /* 33 */\
	MD5_HH(d, a, b, c, x[8], MD5_S32, 0x8771f681);    /* 34 */\
	MD5_HH(c, d, a, b, x[11], MD5_S33, 0x6d9d6122);    /* 35 */\
	MD5_HH(b, c, d, a, x[14], MD5_S34, 0xfde5380c);    /* 36 */\
	MD5_HH(a, b, c, d, x[1], MD5_S31, 0xa4beea44);    /* 37 */\
	MD5_HH(d, a, b, c, x[4], MD5_S32, 0x4bdecfa9);    /* 38 */\
	MD5_HH(c, d, a, b, x[7], MD5_S33, 0xf6bb4b60);    /* 39 */\
	MD5_HH(b, c, d, a, x[10], MD5_S34, 0xbebfbc70);    /* 40 */\
	MD5_HH(a, b, c, d, x[13], MD5_S31, 0x289b7ec6);    /* 41 */\
	MD5_HH(d, a, b, c, x[0], MD5_S32, 0xeaa127fa);    /* 42 */\
	MD5_HH(c, d, a, b, x[3], MD5_S33, 0xd4ef3085);    /* 43 */\
	MD5_HH(b, c, d, a, x[6], MD5_S34, 0x4881d05);    /* 44 */\
	MD5_HH(a, b, c, d, x[9], MD5_S31, 0xd9d4d039);    /* 45 */\
	MD5_HH(d, a, b, c, x[12], MD5_S32, 0xe6db99e5);    /* 46 */\
	MD5_HH(c, d, a, b, x[15], MD5_S33, 0x1fa27cf8);    /* 47 */\
	MD5_HH(b, c, d, a, x[2], MD5_S34, 0xc4ac5665);    /* 48 */\
	\
	/* Round 4 */\
	MD5_II(a, b, c, d, x[0], MD5_S41, 0xf4292244);    /* 49 */\
	MD5_II(d, a, b, c, x[7], MD5_S42, 0x432aff97);    /* 50 */\
	MD5_II(c, d, a, b, x[14], MD5_S43, 0xab9423a7);    /* 51 */\
	MD5_II(b, c, d, a, x[5], MD5_S44, 0xfc93a039);    /* 52 */\
	MD5_II(a, b, c, d, x[12], MD5_S41, 0x655b59c3);    /* 53 */\
	MD5_II(d, a, b, c, x[3], MD5_S42, 0x8f0ccc92);    /* 54 */\
	MD5_II(c, d, a, b, x[10], MD5_S43, 0xffeff47d);    /* 55 */\
	MD5_II(b, c, d, a, x[1], MD5_S44, 0x85845dd1);    /* 56 */\
	MD5_II(a, b, c, d, x[8], MD5_S41, 0x6fa87e4f);    /* 57 */\
	MD5_II(d, a, b, c, x[15], MD5_S42, 0xfe2ce6e0);    /* 58 */\
	MD5_II(c, d, a, b, x[6], MD5_S43, 0xa3014314);    /* 59 */\
	MD5_II(b, c, d, a, x[13], MD5_S44, 0x4e0811a1);    /* 60 */\
	MD5_II(a, b, c, d, x[4], MD5_S41, 0xf7537e82);    /* 61 */\
	MD5_II(d, a, b, c, x[11], MD5_S42, 0xbd3af235);    /* 62 */\
	MD5_II(c, d, a, b, x[2], MD5_S43, 0x2ad7d2bb);    /* 63 */\
	MD5_II(b, c, d, a, x[9], MD5_S44, 0xeb86d391);    /* 64 */\
	\
	state[0] += a;\
	state[1] += b;\
	state[2] += c;\
	state[3] += d;\
\
	/* Zeroize sensitive information. */\
	memset((void *) x, 0, sizeof (x));\
}

static force_inline std::string fmd5(const char * str, size_t len)
{
    int32_t i;

	uint8_t PADDING[64] = {
		0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};

	uint32_t context_state[4];       /* state (ABCD) */
	uint32_t context_count[2];       /* number of bits, modulo 2^64 (lsb first) */
	uint8_t context_buffer[64];    /* input buffer */

    uint8_t hash[16];
            
    MD5_init();
    MD5_update(str, len);
    MD5_fini(hash);
        
	std::string ret;
	for ( i = 0 ; i < 16 ; i++ )
	{
		std::string rettmp = fitoa16((uint8_t)hash[i]);

		if (rettmp.size() == 1)
		{
			ret += "0";
			ret += rettmp;
		}
		else
		{
			ret += rettmp;
		}
	}

    return ret;
}

/////////////////////////////////////////////////////////////

static const uint32_t crc32tab[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba,
    0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
    0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
    0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de,
    0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,
    0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
    0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
    0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940,
    0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116,
    0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
    0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
    0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a,
    0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818,
    0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
    0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
    0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c,
    0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2,
    0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
    0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
    0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086,
    0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4,
    0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
    0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
    0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8,
    0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe,
    0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
    0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
    0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252,
    0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60,
    0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
    0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
    0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04,
    0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a,
    0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
    0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
    0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e,
    0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c,
    0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
    0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0,
    0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6,
    0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
    0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
};

/*
* CRC-32 implementation compatible with libmemcached library. Unfortunately
* this implementation does not return CRC-32 as per spec.
*/
static force_inline uint32_t fcrc32(const char * buff, uint32_t len)
{
    uint64_t x;
    uint32_t crc = 0xFFFFFFFF;

    for (x = 0; x < len; x++) {
        crc = (crc >> 8) ^ crc32tab[(crc ^ (uint64_t)buff[x]) & 0xff];
    }

    return ((~crc) >> 16) & 0x7fff;
}

static force_inline uint32_t fcrc32a(const uint8_t * buff, uint32_t len)
{
    const uint8_t *p = buff;
    uint32_t crc;

    crc = ~0U;
    while (len--) {
        crc = crc32tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ ~0U;
}

/////////////////////////////////////////////////////////////


#define SHA1_BLOCK_SIZE  64
#define SHA1_DIGEST_SIZE 20
#define SHA2_GOOD         0
#define SHA2_BAD          1

#define sha1_rotl32(x,n) (((x) << n) | ((x) >> (32 - n)))

#if (FPLATFORM_BYTE_ORDER == F_BIG_ENDIAN)
#define sha1_swap_b32(x) (x)
#else
#define sha1_swap_b32(x) ((sha1_rotl32((x), 8) & 0x00ff00ff) | (sha1_rotl32((x), 24) & 0xff00ff00))
#endif

#define SHA1_MASK   (SHA1_BLOCK_SIZE - 1)

/* reverse byte order in 32-bit words   */

#define sha1_ch(x,y,z)       (((x) & (y)) ^ (~(x) & (z)))
#define sha1_parity(x,y,z)   ((x) ^ (y) ^ (z))
#define sha1_maj(x,y,z)      (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

/* A normal version as set out in the FIPS. This version uses   */
/* partial loop unrolling and is optimised for the Pentium 4    */

#define sha1_rnd(f,k)    \
    t = a; a = sha1_rotl32(a,5) + f(b,c,d) + e + k + w[i]; \
    e = d; d = c; c = sha1_rotl32(b, 30); b = t

#define sha1_compile()\
{   \
	uint32_t    w[80], i, a, b, c, d, e, t;\
	\
    /* note that words are compiled from the buffer into 32-bit */\
    /* words in big-endian order so an order reversal is needed */\
    /* here on little endian machines                           */\
    for(i = 0; i < SHA1_BLOCK_SIZE / 4; ++i)\
        w[i] = sha1_swap_b32(ctx_wbuf[i]);\
		\
    for(i = SHA1_BLOCK_SIZE / 4; i < 80; ++i)\
        w[i] = sha1_rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);\
		\
    a = ctx_hash[0];\
    b = ctx_hash[1];\
    c = ctx_hash[2];\
    d = ctx_hash[3];\
    e = ctx_hash[4];\
	\
    for(i = 0; i < 20; ++i)\
    {\
        sha1_rnd(sha1_ch, 0x5a827999);  \
    }\
		\
    for(i = 20; i < 40; ++i)\
    {\
        sha1_rnd(sha1_parity, 0x6ed9eba1);\
    }\
	\
    for(i = 40; i < 60; ++i)\
    {\
        sha1_rnd(sha1_maj, 0x8f1bbcdc);\
    }\
	\
    for(i = 60; i < 80; ++i)\
    {\
        sha1_rnd(sha1_parity, 0xca62c1d6);\
    }\
	\
    ctx_hash[0] += a; \
    ctx_hash[1] += b; \
    ctx_hash[2] += c; \
    ctx_hash[3] += d; \
    ctx_hash[4] += e;\
}

#define sha1_begin()\
{\
    ctx_count[0] = ctx_count[1] = 0;\
    ctx_hash[0] = 0x67452301;\
    ctx_hash[1] = 0xefcdab89;\
    ctx_hash[2] = 0x98badcfe;\
    ctx_hash[3] = 0x10325476;\
    ctx_hash[4] = 0xc3d2e1f0;\
}

/* SHA1 hash data in an array of bytes into hash buffer and call the        */
/* hash_compile function as required.                                       */

#define sha1_hash(data, len)\
{   \
	uint32_t pos = (uint32_t)(ctx_count[0] & SHA1_MASK), \
             space = SHA1_BLOCK_SIZE - pos;\
    const uint8_t *sp = data;\
	\
    if((ctx_count[0] += len) < len)\
        ++(ctx_count[1]);\
		\
    while(len >= space)     /* tranfer whole blocks while possible  */\
    {\
        memcpy(((uint8_t*)ctx_wbuf) + pos, sp, space);\
        sp += space; len -= space; space = SHA1_BLOCK_SIZE; pos = 0; \
        sha1_compile();\
    }\
	\
    memcpy(((uint8_t*)ctx_wbuf) + pos, sp, len);\
}

/* SHA1 final padding and digest calculation  */

#if (FPLATFORM_BYTE_ORDER == F_LITTLE_ENDIAN)
static const uint32_t  sha1_mask[4] = 
	{   0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff };
static const uint32_t  sha1_bits[4] = 
	{   0x00000080, 0x00008000, 0x00800000, 0x80000000 };
#elif (FPLATFORM_BYTE_ORDER == F_BIG_ENDIAN)
static const uint32_t  sha1_mask[4] = 
	{   0x00000000, 0xff000000, 0xffff0000, 0xffffff00 };
static const uint32_t  sha1_bits[4] = 
	{   0x80000000, 0x00800000, 0x00008000, 0x00000080 };
#else
#  error Please set undetermined byte order.
#endif

#define sha1_end(hval)\
{   \
	uint32_t    i = (uint32_t)(ctx_count[0] & SHA1_MASK);\
	\
    /* sha1_mask out the rest of any partial 32-bit word and then set    */\
    /* the next byte to 0x80. On big-endian machines any bytes in   */\
    /* the buffer will be at the top end of 32 bit words, on little */\
    /* endian machines they will be at the bottom. Hence the AND    */\
    /* and OR masks above are reversed for little endian systems    */\
	/* Note that we can always add the first padding byte at this	*/\
	/* because the buffer always contains at least one empty slot	*/ \
    ctx_wbuf[i >> 2] = (ctx_wbuf[i >> 2] & sha1_mask[i & 3]) | sha1_bits[i & 3];\
	\
    /* we need 9 or more empty positions, one for the padding byte  */\
    /* (above) and eight for the length count.  If there is not     */\
    /* enough space pad and empty the buffer                        */\
    if(i > SHA1_BLOCK_SIZE - 9)\
    {\
        if(i < 60) ctx_wbuf[15] = 0;\
        sha1_compile();\
        i = 0;\
    }\
    else    /* compute a word index for the empty buffer positions  */\
        i = (i >> 2) + 1;\
		\
    while(i < 14) /* and zero pad all but last two positions      */ \
        ctx_wbuf[i++] = 0;\
    \
    /* assemble the eight byte counter in in big-endian format		*/\
    ctx_wbuf[14] = sha1_swap_b32((ctx_count[1] << 3) | (ctx_count[0] >> 29));\
    ctx_wbuf[15] = sha1_swap_b32(ctx_count[0] << 3);\
	\
    sha1_compile();\
	\
    /* extract the hash value as bytes in case the hash buffer is   */\
    /* misaligned for 32-bit words                                  */\
    for(i = 0; i < SHA1_DIGEST_SIZE; ++i)\
        hval[i] = (uint8_t)(ctx_hash[i >> 2] >> 8 * (~i & 3));\
}

static force_inline std::string fsha1(const uint8_t * data, uint32_t len)
{   
	uint8_t hval[SHA1_DIGEST_SIZE]; 

	uint32_t ctx_count[2];
	uint32_t ctx_hash[5];
	uint32_t ctx_wbuf[16];

    sha1_begin(); 
	sha1_hash(data, len); 
	sha1_end(hval);

	std::string ret;
	for ( int32_t i = 0 ; i < SHA1_DIGEST_SIZE ; i++ )
	{
		std::string rettmp = fitoa16((uint8_t)hval[i]);

		if (rettmp.size() == 1)
		{
			ret += "0";
			ret += rettmp;
		}
		else
		{
			ret += rettmp;
		}
	}

	return ret;
}

/////////////////////////////////////////////////////////////

static const uint64_t des_SPtrans[8][64]={
{
    /* nibble 0 */
    0x00820200L, 0x00020000L, 0x80800000L, 0x80820200L,
    0x00800000L, 0x80020200L, 0x80020000L, 0x80800000L,
    0x80020200L, 0x00820200L, 0x00820000L, 0x80000200L,
    0x80800200L, 0x00800000L, 0x00000000L, 0x80020000L,
    0x00020000L, 0x80000000L, 0x00800200L, 0x00020200L,
    0x80820200L, 0x00820000L, 0x80000200L, 0x00800200L,
    0x80000000L, 0x00000200L, 0x00020200L, 0x80820000L,
    0x00000200L, 0x80800200L, 0x80820000L, 0x00000000L,
    0x00000000L, 0x80820200L, 0x00800200L, 0x80020000L,
    0x00820200L, 0x00020000L, 0x80000200L, 0x00800200L,
    0x80820000L, 0x00000200L, 0x00020200L, 0x80800000L,
    0x80020200L, 0x80000000L, 0x80800000L, 0x00820000L,
    0x80820200L, 0x00020200L, 0x00820000L, 0x80800200L,
    0x00800000L, 0x80000200L, 0x80020000L, 0x00000000L,
    0x00020000L, 0x00800000L, 0x80800200L, 0x00820200L,
    0x80000000L, 0x80820000L, 0x00000200L, 0x80020200L,
    },{
    /* nibble 1 */
    0x10042004L, 0x00000000L, 0x00042000L, 0x10040000L,
    0x10000004L, 0x00002004L, 0x10002000L, 0x00042000L,
    0x00002000L, 0x10040004L, 0x00000004L, 0x10002000L,
    0x00040004L, 0x10042000L, 0x10040000L, 0x00000004L,
    0x00040000L, 0x10002004L, 0x10040004L, 0x00002000L,
    0x00042004L, 0x10000000L, 0x00000000L, 0x00040004L,
    0x10002004L, 0x00042004L, 0x10042000L, 0x10000004L,
    0x10000000L, 0x00040000L, 0x00002004L, 0x10042004L,
    0x00040004L, 0x10042000L, 0x10002000L, 0x00042004L,
    0x10042004L, 0x00040004L, 0x10000004L, 0x00000000L,
    0x10000000L, 0x00002004L, 0x00040000L, 0x10040004L,
    0x00002000L, 0x10000000L, 0x00042004L, 0x10002004L,
    0x10042000L, 0x00002000L, 0x00000000L, 0x10000004L,
    0x00000004L, 0x10042004L, 0x00042000L, 0x10040000L,
    0x10040004L, 0x00040000L, 0x00002004L, 0x10002000L,
    0x10002004L, 0x00000004L, 0x10040000L, 0x00042000L,
    },{
    /* nibble 2 */
    0x41000000L, 0x01010040L, 0x00000040L, 0x41000040L,
    0x40010000L, 0x01000000L, 0x41000040L, 0x00010040L,
    0x01000040L, 0x00010000L, 0x01010000L, 0x40000000L,
    0x41010040L, 0x40000040L, 0x40000000L, 0x41010000L,
    0x00000000L, 0x40010000L, 0x01010040L, 0x00000040L,
    0x40000040L, 0x41010040L, 0x00010000L, 0x41000000L,
    0x41010000L, 0x01000040L, 0x40010040L, 0x01010000L,
    0x00010040L, 0x00000000L, 0x01000000L, 0x40010040L,
    0x01010040L, 0x00000040L, 0x40000000L, 0x00010000L,
    0x40000040L, 0x40010000L, 0x01010000L, 0x41000040L,
    0x00000000L, 0x01010040L, 0x00010040L, 0x41010000L,
    0x40010000L, 0x01000000L, 0x41010040L, 0x40000000L,
    0x40010040L, 0x41000000L, 0x01000000L, 0x41010040L,
    0x00010000L, 0x01000040L, 0x41000040L, 0x00010040L,
    0x01000040L, 0x00000000L, 0x41010000L, 0x40000040L,
    0x41000000L, 0x40010040L, 0x00000040L, 0x01010000L,
    },{
    /* nibble 3 */
    0x00100402L, 0x04000400L, 0x00000002L, 0x04100402L,
    0x00000000L, 0x04100000L, 0x04000402L, 0x00100002L,
    0x04100400L, 0x04000002L, 0x04000000L, 0x00000402L,
    0x04000002L, 0x00100402L, 0x00100000L, 0x04000000L,
    0x04100002L, 0x00100400L, 0x00000400L, 0x00000002L,
    0x00100400L, 0x04000402L, 0x04100000L, 0x00000400L,
    0x00000402L, 0x00000000L, 0x00100002L, 0x04100400L,
    0x04000400L, 0x04100002L, 0x04100402L, 0x00100000L,
    0x04100002L, 0x00000402L, 0x00100000L, 0x04000002L,
    0x00100400L, 0x04000400L, 0x00000002L, 0x04100000L,
    0x04000402L, 0x00000000L, 0x00000400L, 0x00100002L,
    0x00000000L, 0x04100002L, 0x04100400L, 0x00000400L,
    0x04000000L, 0x04100402L, 0x00100402L, 0x00100000L,
    0x04100402L, 0x00000002L, 0x04000400L, 0x00100402L,
    0x00100002L, 0x00100400L, 0x04100000L, 0x04000402L,
    0x00000402L, 0x04000000L, 0x04000002L, 0x04100400L,
    },{
    /* nibble 4 */
    0x02000000L, 0x00004000L, 0x00000100L, 0x02004108L,
    0x02004008L, 0x02000100L, 0x00004108L, 0x02004000L,
    0x00004000L, 0x00000008L, 0x02000008L, 0x00004100L,
    0x02000108L, 0x02004008L, 0x02004100L, 0x00000000L,
    0x00004100L, 0x02000000L, 0x00004008L, 0x00000108L,
    0x02000100L, 0x00004108L, 0x00000000L, 0x02000008L,
    0x00000008L, 0x02000108L, 0x02004108L, 0x00004008L,
    0x02004000L, 0x00000100L, 0x00000108L, 0x02004100L,
    0x02004100L, 0x02000108L, 0x00004008L, 0x02004000L,
    0x00004000L, 0x00000008L, 0x02000008L, 0x02000100L,
    0x02000000L, 0x00004100L, 0x02004108L, 0x00000000L,
    0x00004108L, 0x02000000L, 0x00000100L, 0x00004008L,
    0x02000108L, 0x00000100L, 0x00000000L, 0x02004108L,
    0x02004008L, 0x02004100L, 0x00000108L, 0x00004000L,
    0x00004100L, 0x02004008L, 0x02000100L, 0x00000108L,
    0x00000008L, 0x00004108L, 0x02004000L, 0x02000008L,
    },{
    /* nibble 5 */
    0x20000010L, 0x00080010L, 0x00000000L, 0x20080800L,
    0x00080010L, 0x00000800L, 0x20000810L, 0x00080000L,
    0x00000810L, 0x20080810L, 0x00080800L, 0x20000000L,
    0x20000800L, 0x20000010L, 0x20080000L, 0x00080810L,
    0x00080000L, 0x20000810L, 0x20080010L, 0x00000000L,
    0x00000800L, 0x00000010L, 0x20080800L, 0x20080010L,
    0x20080810L, 0x20080000L, 0x20000000L, 0x00000810L,
    0x00000010L, 0x00080800L, 0x00080810L, 0x20000800L,
    0x00000810L, 0x20000000L, 0x20000800L, 0x00080810L,
    0x20080800L, 0x00080010L, 0x00000000L, 0x20000800L,
    0x20000000L, 0x00000800L, 0x20080010L, 0x00080000L,
    0x00080010L, 0x20080810L, 0x00080800L, 0x00000010L,
    0x20080810L, 0x00080800L, 0x00080000L, 0x20000810L,
    0x20000010L, 0x20080000L, 0x00080810L, 0x00000000L,
    0x00000800L, 0x20000010L, 0x20000810L, 0x20080800L,
    0x20080000L, 0x00000810L, 0x00000010L, 0x20080010L,
    },{
    /* nibble 6 */
    0x00001000L, 0x00000080L, 0x00400080L, 0x00400001L,
    0x00401081L, 0x00001001L, 0x00001080L, 0x00000000L,
    0x00400000L, 0x00400081L, 0x00000081L, 0x00401000L,
    0x00000001L, 0x00401080L, 0x00401000L, 0x00000081L,
    0x00400081L, 0x00001000L, 0x00001001L, 0x00401081L,
    0x00000000L, 0x00400080L, 0x00400001L, 0x00001080L,
    0x00401001L, 0x00001081L, 0x00401080L, 0x00000001L,
    0x00001081L, 0x00401001L, 0x00000080L, 0x00400000L,
    0x00001081L, 0x00401000L, 0x00401001L, 0x00000081L,
    0x00001000L, 0x00000080L, 0x00400000L, 0x00401001L,
    0x00400081L, 0x00001081L, 0x00001080L, 0x00000000L,
    0x00000080L, 0x00400001L, 0x00000001L, 0x00400080L,
    0x00000000L, 0x00400081L, 0x00400080L, 0x00001080L,
    0x00000081L, 0x00001000L, 0x00401081L, 0x00400000L,
    0x00401080L, 0x00000001L, 0x00001001L, 0x00401081L,
    0x00400001L, 0x00401080L, 0x00401000L, 0x00001001L,
    },{
    /* nibble 7 */
    0x08200020L, 0x08208000L, 0x00008020L, 0x00000000L,
    0x08008000L, 0x00200020L, 0x08200000L, 0x08208020L,
    0x00000020L, 0x08000000L, 0x00208000L, 0x00008020L,
    0x00208020L, 0x08008020L, 0x08000020L, 0x08200000L,
    0x00008000L, 0x00208020L, 0x00200020L, 0x08008000L,
    0x08208020L, 0x08000020L, 0x00000000L, 0x00208000L,
    0x08000000L, 0x00200000L, 0x08008020L, 0x08200020L,
    0x00200000L, 0x00008000L, 0x08208000L, 0x00000020L,
    0x00200000L, 0x00008000L, 0x08000020L, 0x08208020L,
    0x00008020L, 0x08000000L, 0x00000000L, 0x00208000L,
    0x08200020L, 0x08008020L, 0x08008000L, 0x00200020L,
    0x08208000L, 0x00000020L, 0x00200020L, 0x08008000L,
    0x08208020L, 0x00200000L, 0x08200000L, 0x08000020L,
    0x00208000L, 0x00008020L, 0x08008020L, 0x08200000L,
    0x00000020L, 0x08208000L, 0x00208020L, 0x00000000L,
    0x08000000L, 0x08200020L, 0x00008000L, 0x00208020L,
    }
};

static const uint64_t des_skb[8][64]={
{
    /* for C bits (numbered as per FIPS 46) 1 2 3 4 5 6 */
    0x00000000L,0x00000010L,0x20000000L,0x20000010L,
    0x00010000L,0x00010010L,0x20010000L,0x20010010L,
    0x00000800L,0x00000810L,0x20000800L,0x20000810L,
    0x00010800L,0x00010810L,0x20010800L,0x20010810L,
    0x00000020L,0x00000030L,0x20000020L,0x20000030L,
    0x00010020L,0x00010030L,0x20010020L,0x20010030L,
    0x00000820L,0x00000830L,0x20000820L,0x20000830L,
    0x00010820L,0x00010830L,0x20010820L,0x20010830L,
    0x00080000L,0x00080010L,0x20080000L,0x20080010L,
    0x00090000L,0x00090010L,0x20090000L,0x20090010L,
    0x00080800L,0x00080810L,0x20080800L,0x20080810L,
    0x00090800L,0x00090810L,0x20090800L,0x20090810L,
    0x00080020L,0x00080030L,0x20080020L,0x20080030L,
    0x00090020L,0x00090030L,0x20090020L,0x20090030L,
    0x00080820L,0x00080830L,0x20080820L,0x20080830L,
    0x00090820L,0x00090830L,0x20090820L,0x20090830L,
    },{
    /* for C bits (numbered as per FIPS 46) 7 8 10 11 12 13 */
    0x00000000L,0x02000000L,0x00002000L,0x02002000L,
    0x00200000L,0x02200000L,0x00202000L,0x02202000L,
    0x00000004L,0x02000004L,0x00002004L,0x02002004L,
    0x00200004L,0x02200004L,0x00202004L,0x02202004L,
    0x00000400L,0x02000400L,0x00002400L,0x02002400L,
    0x00200400L,0x02200400L,0x00202400L,0x02202400L,
    0x00000404L,0x02000404L,0x00002404L,0x02002404L,
    0x00200404L,0x02200404L,0x00202404L,0x02202404L,
    0x10000000L,0x12000000L,0x10002000L,0x12002000L,
    0x10200000L,0x12200000L,0x10202000L,0x12202000L,
    0x10000004L,0x12000004L,0x10002004L,0x12002004L,
    0x10200004L,0x12200004L,0x10202004L,0x12202004L,
    0x10000400L,0x12000400L,0x10002400L,0x12002400L,
    0x10200400L,0x12200400L,0x10202400L,0x12202400L,
    0x10000404L,0x12000404L,0x10002404L,0x12002404L,
    0x10200404L,0x12200404L,0x10202404L,0x12202404L,
    },{
    /* for C bits (numbered as per FIPS 46) 14 15 16 17 19 20 */
    0x00000000L,0x00000001L,0x00040000L,0x00040001L,
    0x01000000L,0x01000001L,0x01040000L,0x01040001L,
    0x00000002L,0x00000003L,0x00040002L,0x00040003L,
    0x01000002L,0x01000003L,0x01040002L,0x01040003L,
    0x00000200L,0x00000201L,0x00040200L,0x00040201L,
    0x01000200L,0x01000201L,0x01040200L,0x01040201L,
    0x00000202L,0x00000203L,0x00040202L,0x00040203L,
    0x01000202L,0x01000203L,0x01040202L,0x01040203L,
    0x08000000L,0x08000001L,0x08040000L,0x08040001L,
    0x09000000L,0x09000001L,0x09040000L,0x09040001L,
    0x08000002L,0x08000003L,0x08040002L,0x08040003L,
    0x09000002L,0x09000003L,0x09040002L,0x09040003L,
    0x08000200L,0x08000201L,0x08040200L,0x08040201L,
    0x09000200L,0x09000201L,0x09040200L,0x09040201L,
    0x08000202L,0x08000203L,0x08040202L,0x08040203L,
    0x09000202L,0x09000203L,0x09040202L,0x09040203L,
    },{
    /* for C bits (numbered as per FIPS 46) 21 23 24 26 27 28 */
    0x00000000L,0x00100000L,0x00000100L,0x00100100L,
    0x00000008L,0x00100008L,0x00000108L,0x00100108L,
    0x00001000L,0x00101000L,0x00001100L,0x00101100L,
    0x00001008L,0x00101008L,0x00001108L,0x00101108L,
    0x04000000L,0x04100000L,0x04000100L,0x04100100L,
    0x04000008L,0x04100008L,0x04000108L,0x04100108L,
    0x04001000L,0x04101000L,0x04001100L,0x04101100L,
    0x04001008L,0x04101008L,0x04001108L,0x04101108L,
    0x00020000L,0x00120000L,0x00020100L,0x00120100L,
    0x00020008L,0x00120008L,0x00020108L,0x00120108L,
    0x00021000L,0x00121000L,0x00021100L,0x00121100L,
    0x00021008L,0x00121008L,0x00021108L,0x00121108L,
    0x04020000L,0x04120000L,0x04020100L,0x04120100L,
    0x04020008L,0x04120008L,0x04020108L,0x04120108L,
    0x04021000L,0x04121000L,0x04021100L,0x04121100L,
    0x04021008L,0x04121008L,0x04021108L,0x04121108L,
    },{
    /* for D bits (numbered as per FIPS 46) 1 2 3 4 5 6 */
    0x00000000L,0x10000000L,0x00010000L,0x10010000L,
    0x00000004L,0x10000004L,0x00010004L,0x10010004L,
    0x20000000L,0x30000000L,0x20010000L,0x30010000L,
    0x20000004L,0x30000004L,0x20010004L,0x30010004L,
    0x00100000L,0x10100000L,0x00110000L,0x10110000L,
    0x00100004L,0x10100004L,0x00110004L,0x10110004L,
    0x20100000L,0x30100000L,0x20110000L,0x30110000L,
    0x20100004L,0x30100004L,0x20110004L,0x30110004L,
    0x00001000L,0x10001000L,0x00011000L,0x10011000L,
    0x00001004L,0x10001004L,0x00011004L,0x10011004L,
    0x20001000L,0x30001000L,0x20011000L,0x30011000L,
    0x20001004L,0x30001004L,0x20011004L,0x30011004L,
    0x00101000L,0x10101000L,0x00111000L,0x10111000L,
    0x00101004L,0x10101004L,0x00111004L,0x10111004L,
    0x20101000L,0x30101000L,0x20111000L,0x30111000L,
    0x20101004L,0x30101004L,0x20111004L,0x30111004L,
    },{
    /* for D bits (numbered as per FIPS 46) 8 9 11 12 13 14 */
    0x00000000L,0x08000000L,0x00000008L,0x08000008L,
    0x00000400L,0x08000400L,0x00000408L,0x08000408L,
    0x00020000L,0x08020000L,0x00020008L,0x08020008L,
    0x00020400L,0x08020400L,0x00020408L,0x08020408L,
    0x00000001L,0x08000001L,0x00000009L,0x08000009L,
    0x00000401L,0x08000401L,0x00000409L,0x08000409L,
    0x00020001L,0x08020001L,0x00020009L,0x08020009L,
    0x00020401L,0x08020401L,0x00020409L,0x08020409L,
    0x02000000L,0x0A000000L,0x02000008L,0x0A000008L,
    0x02000400L,0x0A000400L,0x02000408L,0x0A000408L,
    0x02020000L,0x0A020000L,0x02020008L,0x0A020008L,
    0x02020400L,0x0A020400L,0x02020408L,0x0A020408L,
    0x02000001L,0x0A000001L,0x02000009L,0x0A000009L,
    0x02000401L,0x0A000401L,0x02000409L,0x0A000409L,
    0x02020001L,0x0A020001L,0x02020009L,0x0A020009L,
    0x02020401L,0x0A020401L,0x02020409L,0x0A020409L,
    },{
    /* for D bits (numbered as per FIPS 46) 16 17 18 19 20 21 */
    0x00000000L,0x00000100L,0x00080000L,0x00080100L,
    0x01000000L,0x01000100L,0x01080000L,0x01080100L,
    0x00000010L,0x00000110L,0x00080010L,0x00080110L,
    0x01000010L,0x01000110L,0x01080010L,0x01080110L,
    0x00200000L,0x00200100L,0x00280000L,0x00280100L,
    0x01200000L,0x01200100L,0x01280000L,0x01280100L,
    0x00200010L,0x00200110L,0x00280010L,0x00280110L,
    0x01200010L,0x01200110L,0x01280010L,0x01280110L,
    0x00000200L,0x00000300L,0x00080200L,0x00080300L,
    0x01000200L,0x01000300L,0x01080200L,0x01080300L,
    0x00000210L,0x00000310L,0x00080210L,0x00080310L,
    0x01000210L,0x01000310L,0x01080210L,0x01080310L,
    0x00200200L,0x00200300L,0x00280200L,0x00280300L,
    0x01200200L,0x01200300L,0x01280200L,0x01280300L,
    0x00200210L,0x00200310L,0x00280210L,0x00280310L,
    0x01200210L,0x01200310L,0x01280210L,0x01280310L,
    },{
    /* for D bits (numbered as per FIPS 46) 22 23 24 25 27 28 */
    0x00000000L,0x04000000L,0x00040000L,0x04040000L,
    0x00000002L,0x04000002L,0x00040002L,0x04040002L,
    0x00002000L,0x04002000L,0x00042000L,0x04042000L,
    0x00002002L,0x04002002L,0x00042002L,0x04042002L,
    0x00000020L,0x04000020L,0x00040020L,0x04040020L,
    0x00000022L,0x04000022L,0x00040022L,0x04040022L,
    0x00002020L,0x04002020L,0x00042020L,0x04042020L,
    0x00002022L,0x04002022L,0x00042022L,0x04042022L,
    0x00000800L,0x04000800L,0x00040800L,0x04040800L,
    0x00000802L,0x04000802L,0x00040802L,0x04040802L,
    0x00002800L,0x04002800L,0x00042800L,0x04042800L,
    0x00002802L,0x04002802L,0x00042802L,0x04042802L,
    0x00000820L,0x04000820L,0x00040820L,0x04040820L,
    0x00000822L,0x04000822L,0x00040822L,0x04040822L,
    0x00002820L,0x04002820L,0x00042820L,0x04042820L,
    0x00002822L,0x04002822L,0x00042822L,0x04042822L,
    }
};

#define DES_PERM_OP(a,b,t,n,m) ((t)=((((a)>>(n))^(b))&(m)),\
	(b)^=(t),\
	(a)^=((t)<<(n)))

#define DES_HPERM_OP(a,t,n,m) ((t)=((((a)<<(16-(n)))^(a))&(m)),\
	(a)=(a)^(t)^(t>>(16-(n))))

static const int32_t des_shifts2[16]={0,0,1,1,1,1,1,1,0,1,1,1,1,1,1,0};
#define des_c2l(c,l)	(l =((uint64_t)(*((c)++)))    , \
			 l|=((uint64_t)(*((c)++)))<< 8L, \
			 l|=((uint64_t)(*((c)++)))<<16L, \
			 l|=((uint64_t)(*((c)++)))<<24L)

#define des_c2ln(c,l1,l2,n)	{ \
			c+=n; \
			l1=l2=0; \
			switch (n) { \
			case 8: l2 =((uint64_t)(*(--(c))))<<24L; \
			case 7: l2|=((uint64_t)(*(--(c))))<<16L; \
			case 6: l2|=((uint64_t)(*(--(c))))<< 8L; \
			case 5: l2|=((uint64_t)(*(--(c))));     \
			case 4: l1 =((uint64_t)(*(--(c))))<<24L; \
			case 3: l1|=((uint64_t)(*(--(c))))<<16L; \
			case 2: l1|=((uint64_t)(*(--(c))))<< 8L; \
			case 1: l1|=((uint64_t)(*(--(c))));     \
				} \
			}

#define des_l2c(l,c)	(*((c)++)=(uint8_t)(((l)     )&0xff), \
			 *((c)++)=(uint8_t)(((l)>> 8L)&0xff), \
			 *((c)++)=(uint8_t)(((l)>>16L)&0xff), \
			 *((c)++)=(uint8_t)(((l)>>24L)&0xff))

#define des_n2l(c,l)	(l =((uint64_t)(*((c)++)))<<24L, \
			 l|=((uint64_t)(*((c)++)))<<16L, \
			 l|=((uint64_t)(*((c)++)))<< 8L, \
			 l|=((uint64_t)(*((c)++))))

#define des_l2n(l,c)	(*((c)++)=(uint8_t)(((l)>>24L)&0xff), \
			 *((c)++)=(uint8_t)(((l)>>16L)&0xff), \
			 *((c)++)=(uint8_t)(((l)>> 8L)&0xff), \
			 *((c)++)=(uint8_t)(((l)     )&0xff))

#define des_l2cn(l1,l2,c,n)	{ \
			c+=n; \
			switch (n) { \
			case 8: *(--(c))=(uint8_t)(((l2)>>24L)&0xff); \
			case 7: *(--(c))=(uint8_t)(((l2)>>16L)&0xff); \
			case 6: *(--(c))=(uint8_t)(((l2)>> 8L)&0xff); \
			case 5: *(--(c))=(uint8_t)(((l2)     )&0xff); \
			case 4: *(--(c))=(uint8_t)(((l1)>>24L)&0xff); \
			case 3: *(--(c))=(uint8_t)(((l1)>>16L)&0xff); \
			case 2: *(--(c))=(uint8_t)(((l1)>> 8L)&0xff); \
			case 1: *(--(c))=(uint8_t)(((l1)     )&0xff); \
				} \
			}

#define DES_ENCRYPT(L,R,S)	\
	U.l=R^s[S+1]; \
	T.s[0]=((U.s[0]>>4)|(U.s[1]<<12))&0x3f3f; \
	T.s[1]=((U.s[1]>>4)|(U.s[0]<<12))&0x3f3f; \
	U.l=(R^s[S  ])&0x3f3f3f3fL; \
	L^=	des_SPtrans[1][(T.c[0])]| \
		des_SPtrans[3][(T.c[1])]| \
		des_SPtrans[5][(T.c[2])]| \
		des_SPtrans[7][(T.c[3])]| \
		des_SPtrans[0][(U.c[0])]| \
		des_SPtrans[2][(U.c[1])]| \
		des_SPtrans[4][(U.c[2])]| \
		des_SPtrans[6][(U.c[3])];

#define DES_IP(l,r) \
	{ \
	register uint64_t tt; \
	DES_PERM_OP(r,l,tt, 4,0x0f0f0f0fL); \
	DES_PERM_OP(l,r,tt,16,0x0000ffffL); \
	DES_PERM_OP(r,l,tt, 2,0x33333333L); \
	DES_PERM_OP(l,r,tt, 8,0x00ff00ffL); \
	DES_PERM_OP(r,l,tt, 1,0x55555555L); \
	}

#define DES_FP(l,r) \
	{ \
	register uint64_t tt; \
	DES_PERM_OP(l,r,tt, 1,0x55555555L); \
	DES_PERM_OP(r,l,tt, 8,0x00ff00ffL); \
	DES_PERM_OP(l,r,tt, 2,0x33333333L); \
	DES_PERM_OP(r,l,tt,16,0x0000ffffL); \
	DES_PERM_OP(l,r,tt, 4,0x0f0f0f0fL); \
	}

typedef struct des_ks_struct
{
	union
	{
		uint64_t pad[2];
	} ks;
} des_key_schedule[16];

#define DES_BUFF_LEN 8

static force_inline int32_t des_set_key(uint8_t (&key)[DES_BUFF_LEN], des_key_schedule schedule)
{
	register uint64_t c,d,t,s;
	register uint8_t *in;
	register uint64_t *k;
	register int32_t i;

	k=(uint64_t *)schedule;
	in=(uint8_t *)key;

	des_c2l(in,c);
	des_c2l(in,d);

	DES_PERM_OP (d,c,t,4,0x0f0f0f0fL);
	DES_HPERM_OP(c,t,-2,0xcccc0000L);
	DES_HPERM_OP(d,t,-2,0xcccc0000L);
	DES_PERM_OP (d,c,t,1,0x55555555L);
	DES_PERM_OP (c,d,t,8,0x00ff00ffL);
	DES_PERM_OP (d,c,t,1,0x55555555L);
	d=	(((d&0x000000ffL)<<16L)| (d&0x0000ff00L)     |
		 ((d&0x00ff0000L)>>16L)|((c&0xf0000000L)>>4L));
	c&=0x0fffffffL;

	for (i=0; i<16; i++)
		{
		if (des_shifts2[i])
			{ c=((c>>2L)|(c<<26L)); d=((d>>2L)|(d<<26L)); }
		else
			{ c=((c>>1L)|(c<<27L)); d=((d>>1L)|(d<<27L)); }
		c&=0x0fffffffL;
		d&=0x0fffffffL;

		s=	des_skb[0][ (c    )&0x3f                ]|
			des_skb[1][((c>> 6)&0x03)|((c>> 7L)&0x3c)]|
			des_skb[2][((c>>13)&0x0f)|((c>>14L)&0x30)]|
			des_skb[3][((c>>20)&0x01)|((c>>21L)&0x06) |
						  ((c>>22L)&0x38)];
		t=	des_skb[4][ (d    )&0x3f                ]|
			des_skb[5][((d>> 7L)&0x03)|((d>> 8L)&0x3c)]|
			des_skb[6][ (d>>15L)&0x3f                ]|
			des_skb[7][((d>>21L)&0x0f)|((d>>22L)&0x30)];

		/* table contained 0213 4657 */
		*(k++)=((t<<16L)|(s&0x0000ffffL))&0xffffffffL;
		s=     ((s>>16L)|(t&0xffff0000L));
		
		s=(s<<4L)|(s>>28L);
		*(k++)=s&0xffffffffL;
		}
	return(0);
}

static force_inline void des_encrypt(uint64_t *data, des_key_schedule ks, int32_t encrypt)
{
	register uint64_t l,r,u;
	union fudge 
	{
		uint64_t  l;
		unsigned short s[2];
		uint8_t  c[4];
	} U,T;
	register int32_t i;
	register uint64_t *s;

	u=data[0];
	r=data[1];

	DES_IP(u,r);

	l=(r<<1)|(r>>31);
	r=(u<<1)|(u>>31);

	/* clear the top bits on machines with 8byte longs */
	l&=0xffffffffL;
	r&=0xffffffffL;

	s=(uint64_t *)ks;
	if (encrypt)
	{
		for (i=0; i<32; i+=4)
		{
			DES_ENCRYPT(l,r,i+0); /*  1 */
			DES_ENCRYPT(r,l,i+2); /*  2 */
		}
	}
	else
	{
		for (i=30; i>0; i-=4)
		{
			DES_ENCRYPT(l,r,i-0); /* 16 */
			DES_ENCRYPT(r,l,i-2); /* 15 */
		}
	}
	l=(l>>1)|(l<<31);
	r=(r>>1)|(r<<31);
	/* clear the top bits on machines with 8byte longs */
	l&=0xffffffffL;
	r&=0xffffffffL;

	DES_FP(r,l);
	data[0]=l;
	data[1]=r;
}

static force_inline void des_ecb_encrypt(uint8_t (&input)[DES_BUFF_LEN], uint8_t (&output)[DES_BUFF_LEN], des_key_schedule ks, int32_t encrypt)
{
	register uint64_t l;
	register uint8_t *in,*out;
	uint64_t ll[2];

	in=(uint8_t *)input;
	out=(uint8_t *)output;
	des_c2l(in,l); ll[0]=l;
	des_c2l(in,l); ll[1]=l;
	des_encrypt(ll,ks,encrypt);
	l=ll[0]; des_l2c(l,out);
	l=ll[1]; des_l2c(l,out);
	l=ll[0]=ll[1]=0;
}

static force_inline void fdes(const std::string & strkey, const char * s_text, size_t s_len, char (&d_text)[DES_BUFF_LEN])
{
    uint8_t key[DES_BUFF_LEN];
    memset(key, 0, sizeof(key));
    memcpy(key, strkey.c_str(), std::min<size_t>(strkey.size(), DES_BUFF_LEN));

    uint8_t stext[DES_BUFF_LEN];
    memset(stext, 0, sizeof(stext));
    memcpy(stext, s_text, std::min<size_t>(s_len, DES_BUFF_LEN));
    
	des_key_schedule sch;
	des_set_key(key, sch);
	des_ecb_encrypt(stext, (uint8_t (&)[DES_BUFF_LEN])d_text, &(sch[0]), 1);
}

static force_inline std::string fdes(const std::string & strkey, const std::string & s_text)
{
	char d_text[DES_BUFF_LEN] = {0};
    fdes(strkey, s_text.c_str(), s_text.size(), d_text);

	std::string ret;
	for (int32_t i = 0 ; i < DES_BUFF_LEN; i++ )
	{
		std::string rettmp = fitoa16((uint8_t)d_text[i]);

		if (rettmp.size() == 1)
		{
			ret += "0";
			ret += rettmp;
		}
		else
		{
			ret += rettmp;
		}
	}

    return ret;
}

static force_inline void fundes(const std::string & strkey, const char * s_text, size_t s_len, char (&d_text)[DES_BUFF_LEN])
{
    uint8_t key[DES_BUFF_LEN] = {0};
    memset(key, 0, sizeof(key));
    memcpy(key, strkey.c_str(), std::min<size_t>(strkey.size(), DES_BUFF_LEN));

    uint8_t stext[DES_BUFF_LEN] = {0};
    memset(stext, 0, sizeof(stext));
    memcpy(stext, s_text, std::min<size_t>(s_len, DES_BUFF_LEN));
    
	des_key_schedule sch;
	des_set_key(key, sch);
	des_ecb_encrypt(stext, (uint8_t (&)[DES_BUFF_LEN])d_text, &(sch[0]), 0);
}

static force_inline std::string fundes(const std::string & strkey, const std::string & s_text)
{
    char stext[DES_BUFF_LEN] = {0};
    memset(stext, 0, sizeof(stext));
	for (int32_t i = 0; i < DES_BUFF_LEN && ((2 * i + 2) <= (int)s_text.size()); i++)
    {
        std::string tmp = s_text.substr(2 * i, 2);
        stext[i] = fatoi16(tmp);
    }

	union
	{
		char d_buff[DES_BUFF_LEN + 1];
		char d_text[DES_BUFF_LEN];
	} dtext;
	memset(&dtext, 0, sizeof(dtext));
	fundes(strkey, stext, DES_BUFF_LEN, dtext.d_text);
    
	return std::string(dtext.d_text);
}

/////////////////////////////////////////////////////////////


/****************************** MACROS ******************************/
// The least significant byte of the word is rotated to the end.
#define AES_KE_ROTWORD(x) (((x) << 8) | ((x) >> 24))

/**************************** DATA TYPES ****************************/
#define AES_128_ROUNDS 10
#define AES_192_ROUNDS 12
#define AES_256_ROUNDS 14

/**************************** VARIABLES *****************************/
// This is the specified AES SBox. To look up a substitution value, put the first
// nibble in the first index (row) and the second nibble in the second index (column).
static const uint8_t aes_sbox[16][16] = {
	{ 0x63,0x7C,0x77,0x7B,0xF2,0x6B,0x6F,0xC5,0x30,0x01,0x67,0x2B,0xFE,0xD7,0xAB,0x76 },
	{ 0xCA,0x82,0xC9,0x7D,0xFA,0x59,0x47,0xF0,0xAD,0xD4,0xA2,0xAF,0x9C,0xA4,0x72,0xC0 },
	{ 0xB7,0xFD,0x93,0x26,0x36,0x3F,0xF7,0xCC,0x34,0xA5,0xE5,0xF1,0x71,0xD8,0x31,0x15 },
	{ 0x04,0xC7,0x23,0xC3,0x18,0x96,0x05,0x9A,0x07,0x12,0x80,0xE2,0xEB,0x27,0xB2,0x75 },
	{ 0x09,0x83,0x2C,0x1A,0x1B,0x6E,0x5A,0xA0,0x52,0x3B,0xD6,0xB3,0x29,0xE3,0x2F,0x84 },
	{ 0x53,0xD1,0x00,0xED,0x20,0xFC,0xB1,0x5B,0x6A,0xCB,0xBE,0x39,0x4A,0x4C,0x58,0xCF },
	{ 0xD0,0xEF,0xAA,0xFB,0x43,0x4D,0x33,0x85,0x45,0xF9,0x02,0x7F,0x50,0x3C,0x9F,0xA8 },
	{ 0x51,0xA3,0x40,0x8F,0x92,0x9D,0x38,0xF5,0xBC,0xB6,0xDA,0x21,0x10,0xFF,0xF3,0xD2 },
	{ 0xCD,0x0C,0x13,0xEC,0x5F,0x97,0x44,0x17,0xC4,0xA7,0x7E,0x3D,0x64,0x5D,0x19,0x73 },
	{ 0x60,0x81,0x4F,0xDC,0x22,0x2A,0x90,0x88,0x46,0xEE,0xB8,0x14,0xDE,0x5E,0x0B,0xDB },
	{ 0xE0,0x32,0x3A,0x0A,0x49,0x06,0x24,0x5C,0xC2,0xD3,0xAC,0x62,0x91,0x95,0xE4,0x79 },
	{ 0xE7,0xC8,0x37,0x6D,0x8D,0xD5,0x4E,0xA9,0x6C,0x56,0xF4,0xEA,0x65,0x7A,0xAE,0x08 },
	{ 0xBA,0x78,0x25,0x2E,0x1C,0xA6,0xB4,0xC6,0xE8,0xDD,0x74,0x1F,0x4B,0xBD,0x8B,0x8A },
	{ 0x70,0x3E,0xB5,0x66,0x48,0x03,0xF6,0x0E,0x61,0x35,0x57,0xB9,0x86,0xC1,0x1D,0x9E },
	{ 0xE1,0xF8,0x98,0x11,0x69,0xD9,0x8E,0x94,0x9B,0x1E,0x87,0xE9,0xCE,0x55,0x28,0xDF },
	{ 0x8C,0xA1,0x89,0x0D,0xBF,0xE6,0x42,0x68,0x41,0x99,0x2D,0x0F,0xB0,0x54,0xBB,0x16 }
};

static const uint8_t aes_invsbox[16][16] = {
	{ 0x52,0x09,0x6A,0xD5,0x30,0x36,0xA5,0x38,0xBF,0x40,0xA3,0x9E,0x81,0xF3,0xD7,0xFB },
	{ 0x7C,0xE3,0x39,0x82,0x9B,0x2F,0xFF,0x87,0x34,0x8E,0x43,0x44,0xC4,0xDE,0xE9,0xCB },
	{ 0x54,0x7B,0x94,0x32,0xA6,0xC2,0x23,0x3D,0xEE,0x4C,0x95,0x0B,0x42,0xFA,0xC3,0x4E },
	{ 0x08,0x2E,0xA1,0x66,0x28,0xD9,0x24,0xB2,0x76,0x5B,0xA2,0x49,0x6D,0x8B,0xD1,0x25 },
	{ 0x72,0xF8,0xF6,0x64,0x86,0x68,0x98,0x16,0xD4,0xA4,0x5C,0xCC,0x5D,0x65,0xB6,0x92 },
	{ 0x6C,0x70,0x48,0x50,0xFD,0xED,0xB9,0xDA,0x5E,0x15,0x46,0x57,0xA7,0x8D,0x9D,0x84 },
	{ 0x90,0xD8,0xAB,0x00,0x8C,0xBC,0xD3,0x0A,0xF7,0xE4,0x58,0x05,0xB8,0xB3,0x45,0x06 },
	{ 0xD0,0x2C,0x1E,0x8F,0xCA,0x3F,0x0F,0x02,0xC1,0xAF,0xBD,0x03,0x01,0x13,0x8A,0x6B },
	{ 0x3A,0x91,0x11,0x41,0x4F,0x67,0xDC,0xEA,0x97,0xF2,0xCF,0xCE,0xF0,0xB4,0xE6,0x73 },
	{ 0x96,0xAC,0x74,0x22,0xE7,0xAD,0x35,0x85,0xE2,0xF9,0x37,0xE8,0x1C,0x75,0xDF,0x6E },
	{ 0x47,0xF1,0x1A,0x71,0x1D,0x29,0xC5,0x89,0x6F,0xB7,0x62,0x0E,0xAA,0x18,0xBE,0x1B },
	{ 0xFC,0x56,0x3E,0x4B,0xC6,0xD2,0x79,0x20,0x9A,0xDB,0xC0,0xFE,0x78,0xCD,0x5A,0xF4 },
	{ 0x1F,0xDD,0xA8,0x33,0x88,0x07,0xC7,0x31,0xB1,0x12,0x10,0x59,0x27,0x80,0xEC,0x5F },
	{ 0x60,0x51,0x7F,0xA9,0x19,0xB5,0x4A,0x0D,0x2D,0xE5,0x7A,0x9F,0x93,0xC9,0x9C,0xEF },
	{ 0xA0,0xE0,0x3B,0x4D,0xAE,0x2A,0xF5,0xB0,0xC8,0xEB,0xBB,0x3C,0x83,0x53,0x99,0x61 },
	{ 0x17,0x2B,0x04,0x7E,0xBA,0x77,0xD6,0x26,0xE1,0x69,0x14,0x63,0x55,0x21,0x0C,0x7D }
};

// This table stores pre-calculated values for all possible GF(2^8) calculations.This
// table is only used by the (Inv)AES_MixColumns steps.
// USAGE: The second index (column) is the coefficient of multiplication. Only 7 different
// coefficients are used: 0x01, 0x02, 0x03, 0x09, 0x0b, 0x0d, 0x0e, but multiplication by
// 1 is negligible leaving only 6 coefficients. Each column of the table is devoted to one
// of these coefficients, in the ascending order of value, from values 0x00 to 0xFF.
static const uint8_t aes_gf_mul[256][6] = {
	{ 0x00,0x00,0x00,0x00,0x00,0x00 },{ 0x02,0x03,0x09,0x0b,0x0d,0x0e },
	{ 0x04,0x06,0x12,0x16,0x1a,0x1c },{ 0x06,0x05,0x1b,0x1d,0x17,0x12 },
	{ 0x08,0x0c,0x24,0x2c,0x34,0x38 },{ 0x0a,0x0f,0x2d,0x27,0x39,0x36 },
	{ 0x0c,0x0a,0x36,0x3a,0x2e,0x24 },{ 0x0e,0x09,0x3f,0x31,0x23,0x2a },
	{ 0x10,0x18,0x48,0x58,0x68,0x70 },{ 0x12,0x1b,0x41,0x53,0x65,0x7e },
	{ 0x14,0x1e,0x5a,0x4e,0x72,0x6c },{ 0x16,0x1d,0x53,0x45,0x7f,0x62 },
	{ 0x18,0x14,0x6c,0x74,0x5c,0x48 },{ 0x1a,0x17,0x65,0x7f,0x51,0x46 },
	{ 0x1c,0x12,0x7e,0x62,0x46,0x54 },{ 0x1e,0x11,0x77,0x69,0x4b,0x5a },
	{ 0x20,0x30,0x90,0xb0,0xd0,0xe0 },{ 0x22,0x33,0x99,0xbb,0xdd,0xee },
	{ 0x24,0x36,0x82,0xa6,0xca,0xfc },{ 0x26,0x35,0x8b,0xad,0xc7,0xf2 },
	{ 0x28,0x3c,0xb4,0x9c,0xe4,0xd8 },{ 0x2a,0x3f,0xbd,0x97,0xe9,0xd6 },
	{ 0x2c,0x3a,0xa6,0x8a,0xfe,0xc4 },{ 0x2e,0x39,0xaf,0x81,0xf3,0xca },
	{ 0x30,0x28,0xd8,0xe8,0xb8,0x90 },{ 0x32,0x2b,0xd1,0xe3,0xb5,0x9e },
	{ 0x34,0x2e,0xca,0xfe,0xa2,0x8c },{ 0x36,0x2d,0xc3,0xf5,0xaf,0x82 },
	{ 0x38,0x24,0xfc,0xc4,0x8c,0xa8 },{ 0x3a,0x27,0xf5,0xcf,0x81,0xa6 },
	{ 0x3c,0x22,0xee,0xd2,0x96,0xb4 },{ 0x3e,0x21,0xe7,0xd9,0x9b,0xba },
	{ 0x40,0x60,0x3b,0x7b,0xbb,0xdb },{ 0x42,0x63,0x32,0x70,0xb6,0xd5 },
	{ 0x44,0x66,0x29,0x6d,0xa1,0xc7 },{ 0x46,0x65,0x20,0x66,0xac,0xc9 },
	{ 0x48,0x6c,0x1f,0x57,0x8f,0xe3 },{ 0x4a,0x6f,0x16,0x5c,0x82,0xed },
	{ 0x4c,0x6a,0x0d,0x41,0x95,0xff },{ 0x4e,0x69,0x04,0x4a,0x98,0xf1 },
	{ 0x50,0x78,0x73,0x23,0xd3,0xab },{ 0x52,0x7b,0x7a,0x28,0xde,0xa5 },
	{ 0x54,0x7e,0x61,0x35,0xc9,0xb7 },{ 0x56,0x7d,0x68,0x3e,0xc4,0xb9 },
	{ 0x58,0x74,0x57,0x0f,0xe7,0x93 },{ 0x5a,0x77,0x5e,0x04,0xea,0x9d },
	{ 0x5c,0x72,0x45,0x19,0xfd,0x8f },{ 0x5e,0x71,0x4c,0x12,0xf0,0x81 },
	{ 0x60,0x50,0xab,0xcb,0x6b,0x3b },{ 0x62,0x53,0xa2,0xc0,0x66,0x35 },
	{ 0x64,0x56,0xb9,0xdd,0x71,0x27 },{ 0x66,0x55,0xb0,0xd6,0x7c,0x29 },
	{ 0x68,0x5c,0x8f,0xe7,0x5f,0x03 },{ 0x6a,0x5f,0x86,0xec,0x52,0x0d },
	{ 0x6c,0x5a,0x9d,0xf1,0x45,0x1f },{ 0x6e,0x59,0x94,0xfa,0x48,0x11 },
	{ 0x70,0x48,0xe3,0x93,0x03,0x4b },{ 0x72,0x4b,0xea,0x98,0x0e,0x45 },
	{ 0x74,0x4e,0xf1,0x85,0x19,0x57 },{ 0x76,0x4d,0xf8,0x8e,0x14,0x59 },
	{ 0x78,0x44,0xc7,0xbf,0x37,0x73 },{ 0x7a,0x47,0xce,0xb4,0x3a,0x7d },
	{ 0x7c,0x42,0xd5,0xa9,0x2d,0x6f },{ 0x7e,0x41,0xdc,0xa2,0x20,0x61 },
	{ 0x80,0xc0,0x76,0xf6,0x6d,0xad },{ 0x82,0xc3,0x7f,0xfd,0x60,0xa3 },
	{ 0x84,0xc6,0x64,0xe0,0x77,0xb1 },{ 0x86,0xc5,0x6d,0xeb,0x7a,0xbf },
	{ 0x88,0xcc,0x52,0xda,0x59,0x95 },{ 0x8a,0xcf,0x5b,0xd1,0x54,0x9b },
	{ 0x8c,0xca,0x40,0xcc,0x43,0x89 },{ 0x8e,0xc9,0x49,0xc7,0x4e,0x87 },
	{ 0x90,0xd8,0x3e,0xae,0x05,0xdd },{ 0x92,0xdb,0x37,0xa5,0x08,0xd3 },
	{ 0x94,0xde,0x2c,0xb8,0x1f,0xc1 },{ 0x96,0xdd,0x25,0xb3,0x12,0xcf },
	{ 0x98,0xd4,0x1a,0x82,0x31,0xe5 },{ 0x9a,0xd7,0x13,0x89,0x3c,0xeb },
	{ 0x9c,0xd2,0x08,0x94,0x2b,0xf9 },{ 0x9e,0xd1,0x01,0x9f,0x26,0xf7 },
	{ 0xa0,0xf0,0xe6,0x46,0xbd,0x4d },{ 0xa2,0xf3,0xef,0x4d,0xb0,0x43 },
	{ 0xa4,0xf6,0xf4,0x50,0xa7,0x51 },{ 0xa6,0xf5,0xfd,0x5b,0xaa,0x5f },
	{ 0xa8,0xfc,0xc2,0x6a,0x89,0x75 },{ 0xaa,0xff,0xcb,0x61,0x84,0x7b },
	{ 0xac,0xfa,0xd0,0x7c,0x93,0x69 },{ 0xae,0xf9,0xd9,0x77,0x9e,0x67 },
	{ 0xb0,0xe8,0xae,0x1e,0xd5,0x3d },{ 0xb2,0xeb,0xa7,0x15,0xd8,0x33 },
	{ 0xb4,0xee,0xbc,0x08,0xcf,0x21 },{ 0xb6,0xed,0xb5,0x03,0xc2,0x2f },
	{ 0xb8,0xe4,0x8a,0x32,0xe1,0x05 },{ 0xba,0xe7,0x83,0x39,0xec,0x0b },
	{ 0xbc,0xe2,0x98,0x24,0xfb,0x19 },{ 0xbe,0xe1,0x91,0x2f,0xf6,0x17 },
	{ 0xc0,0xa0,0x4d,0x8d,0xd6,0x76 },{ 0xc2,0xa3,0x44,0x86,0xdb,0x78 },
	{ 0xc4,0xa6,0x5f,0x9b,0xcc,0x6a },{ 0xc6,0xa5,0x56,0x90,0xc1,0x64 },
	{ 0xc8,0xac,0x69,0xa1,0xe2,0x4e },{ 0xca,0xaf,0x60,0xaa,0xef,0x40 },
	{ 0xcc,0xaa,0x7b,0xb7,0xf8,0x52 },{ 0xce,0xa9,0x72,0xbc,0xf5,0x5c },
	{ 0xd0,0xb8,0x05,0xd5,0xbe,0x06 },{ 0xd2,0xbb,0x0c,0xde,0xb3,0x08 },
	{ 0xd4,0xbe,0x17,0xc3,0xa4,0x1a },{ 0xd6,0xbd,0x1e,0xc8,0xa9,0x14 },
	{ 0xd8,0xb4,0x21,0xf9,0x8a,0x3e },{ 0xda,0xb7,0x28,0xf2,0x87,0x30 },
	{ 0xdc,0xb2,0x33,0xef,0x90,0x22 },{ 0xde,0xb1,0x3a,0xe4,0x9d,0x2c },
	{ 0xe0,0x90,0xdd,0x3d,0x06,0x96 },{ 0xe2,0x93,0xd4,0x36,0x0b,0x98 },
	{ 0xe4,0x96,0xcf,0x2b,0x1c,0x8a },{ 0xe6,0x95,0xc6,0x20,0x11,0x84 },
	{ 0xe8,0x9c,0xf9,0x11,0x32,0xae },{ 0xea,0x9f,0xf0,0x1a,0x3f,0xa0 },
	{ 0xec,0x9a,0xeb,0x07,0x28,0xb2 },{ 0xee,0x99,0xe2,0x0c,0x25,0xbc },
	{ 0xf0,0x88,0x95,0x65,0x6e,0xe6 },{ 0xf2,0x8b,0x9c,0x6e,0x63,0xe8 },
	{ 0xf4,0x8e,0x87,0x73,0x74,0xfa },{ 0xf6,0x8d,0x8e,0x78,0x79,0xf4 },
	{ 0xf8,0x84,0xb1,0x49,0x5a,0xde },{ 0xfa,0x87,0xb8,0x42,0x57,0xd0 },
	{ 0xfc,0x82,0xa3,0x5f,0x40,0xc2 },{ 0xfe,0x81,0xaa,0x54,0x4d,0xcc },
	{ 0x1b,0x9b,0xec,0xf7,0xda,0x41 },{ 0x19,0x98,0xe5,0xfc,0xd7,0x4f },
	{ 0x1f,0x9d,0xfe,0xe1,0xc0,0x5d },{ 0x1d,0x9e,0xf7,0xea,0xcd,0x53 },
	{ 0x13,0x97,0xc8,0xdb,0xee,0x79 },{ 0x11,0x94,0xc1,0xd0,0xe3,0x77 },
	{ 0x17,0x91,0xda,0xcd,0xf4,0x65 },{ 0x15,0x92,0xd3,0xc6,0xf9,0x6b },
	{ 0x0b,0x83,0xa4,0xaf,0xb2,0x31 },{ 0x09,0x80,0xad,0xa4,0xbf,0x3f },
	{ 0x0f,0x85,0xb6,0xb9,0xa8,0x2d },{ 0x0d,0x86,0xbf,0xb2,0xa5,0x23 },
	{ 0x03,0x8f,0x80,0x83,0x86,0x09 },{ 0x01,0x8c,0x89,0x88,0x8b,0x07 },
	{ 0x07,0x89,0x92,0x95,0x9c,0x15 },{ 0x05,0x8a,0x9b,0x9e,0x91,0x1b },
	{ 0x3b,0xab,0x7c,0x47,0x0a,0xa1 },{ 0x39,0xa8,0x75,0x4c,0x07,0xaf },
	{ 0x3f,0xad,0x6e,0x51,0x10,0xbd },{ 0x3d,0xae,0x67,0x5a,0x1d,0xb3 },
	{ 0x33,0xa7,0x58,0x6b,0x3e,0x99 },{ 0x31,0xa4,0x51,0x60,0x33,0x97 },
	{ 0x37,0xa1,0x4a,0x7d,0x24,0x85 },{ 0x35,0xa2,0x43,0x76,0x29,0x8b },
	{ 0x2b,0xb3,0x34,0x1f,0x62,0xd1 },{ 0x29,0xb0,0x3d,0x14,0x6f,0xdf },
	{ 0x2f,0xb5,0x26,0x09,0x78,0xcd },{ 0x2d,0xb6,0x2f,0x02,0x75,0xc3 },
	{ 0x23,0xbf,0x10,0x33,0x56,0xe9 },{ 0x21,0xbc,0x19,0x38,0x5b,0xe7 },
	{ 0x27,0xb9,0x02,0x25,0x4c,0xf5 },{ 0x25,0xba,0x0b,0x2e,0x41,0xfb },
	{ 0x5b,0xfb,0xd7,0x8c,0x61,0x9a },{ 0x59,0xf8,0xde,0x87,0x6c,0x94 },
	{ 0x5f,0xfd,0xc5,0x9a,0x7b,0x86 },{ 0x5d,0xfe,0xcc,0x91,0x76,0x88 },
	{ 0x53,0xf7,0xf3,0xa0,0x55,0xa2 },{ 0x51,0xf4,0xfa,0xab,0x58,0xac },
	{ 0x57,0xf1,0xe1,0xb6,0x4f,0xbe },{ 0x55,0xf2,0xe8,0xbd,0x42,0xb0 },
	{ 0x4b,0xe3,0x9f,0xd4,0x09,0xea },{ 0x49,0xe0,0x96,0xdf,0x04,0xe4 },
	{ 0x4f,0xe5,0x8d,0xc2,0x13,0xf6 },{ 0x4d,0xe6,0x84,0xc9,0x1e,0xf8 },
	{ 0x43,0xef,0xbb,0xf8,0x3d,0xd2 },{ 0x41,0xec,0xb2,0xf3,0x30,0xdc },
	{ 0x47,0xe9,0xa9,0xee,0x27,0xce },{ 0x45,0xea,0xa0,0xe5,0x2a,0xc0 },
	{ 0x7b,0xcb,0x47,0x3c,0xb1,0x7a },{ 0x79,0xc8,0x4e,0x37,0xbc,0x74 },
	{ 0x7f,0xcd,0x55,0x2a,0xab,0x66 },{ 0x7d,0xce,0x5c,0x21,0xa6,0x68 },
	{ 0x73,0xc7,0x63,0x10,0x85,0x42 },{ 0x71,0xc4,0x6a,0x1b,0x88,0x4c },
	{ 0x77,0xc1,0x71,0x06,0x9f,0x5e },{ 0x75,0xc2,0x78,0x0d,0x92,0x50 },
	{ 0x6b,0xd3,0x0f,0x64,0xd9,0x0a },{ 0x69,0xd0,0x06,0x6f,0xd4,0x04 },
	{ 0x6f,0xd5,0x1d,0x72,0xc3,0x16 },{ 0x6d,0xd6,0x14,0x79,0xce,0x18 },
	{ 0x63,0xdf,0x2b,0x48,0xed,0x32 },{ 0x61,0xdc,0x22,0x43,0xe0,0x3c },
	{ 0x67,0xd9,0x39,0x5e,0xf7,0x2e },{ 0x65,0xda,0x30,0x55,0xfa,0x20 },
	{ 0x9b,0x5b,0x9a,0x01,0xb7,0xec },{ 0x99,0x58,0x93,0x0a,0xba,0xe2 },
	{ 0x9f,0x5d,0x88,0x17,0xad,0xf0 },{ 0x9d,0x5e,0x81,0x1c,0xa0,0xfe },
	{ 0x93,0x57,0xbe,0x2d,0x83,0xd4 },{ 0x91,0x54,0xb7,0x26,0x8e,0xda },
	{ 0x97,0x51,0xac,0x3b,0x99,0xc8 },{ 0x95,0x52,0xa5,0x30,0x94,0xc6 },
	{ 0x8b,0x43,0xd2,0x59,0xdf,0x9c },{ 0x89,0x40,0xdb,0x52,0xd2,0x92 },
	{ 0x8f,0x45,0xc0,0x4f,0xc5,0x80 },{ 0x8d,0x46,0xc9,0x44,0xc8,0x8e },
	{ 0x83,0x4f,0xf6,0x75,0xeb,0xa4 },{ 0x81,0x4c,0xff,0x7e,0xe6,0xaa },
	{ 0x87,0x49,0xe4,0x63,0xf1,0xb8 },{ 0x85,0x4a,0xed,0x68,0xfc,0xb6 },
	{ 0xbb,0x6b,0x0a,0xb1,0x67,0x0c },{ 0xb9,0x68,0x03,0xba,0x6a,0x02 },
	{ 0xbf,0x6d,0x18,0xa7,0x7d,0x10 },{ 0xbd,0x6e,0x11,0xac,0x70,0x1e },
	{ 0xb3,0x67,0x2e,0x9d,0x53,0x34 },{ 0xb1,0x64,0x27,0x96,0x5e,0x3a },
	{ 0xb7,0x61,0x3c,0x8b,0x49,0x28 },{ 0xb5,0x62,0x35,0x80,0x44,0x26 },
	{ 0xab,0x73,0x42,0xe9,0x0f,0x7c },{ 0xa9,0x70,0x4b,0xe2,0x02,0x72 },
	{ 0xaf,0x75,0x50,0xff,0x15,0x60 },{ 0xad,0x76,0x59,0xf4,0x18,0x6e },
	{ 0xa3,0x7f,0x66,0xc5,0x3b,0x44 },{ 0xa1,0x7c,0x6f,0xce,0x36,0x4a },
	{ 0xa7,0x79,0x74,0xd3,0x21,0x58 },{ 0xa5,0x7a,0x7d,0xd8,0x2c,0x56 },
	{ 0xdb,0x3b,0xa1,0x7a,0x0c,0x37 },{ 0xd9,0x38,0xa8,0x71,0x01,0x39 },
	{ 0xdf,0x3d,0xb3,0x6c,0x16,0x2b },{ 0xdd,0x3e,0xba,0x67,0x1b,0x25 },
	{ 0xd3,0x37,0x85,0x56,0x38,0x0f },{ 0xd1,0x34,0x8c,0x5d,0x35,0x01 },
	{ 0xd7,0x31,0x97,0x40,0x22,0x13 },{ 0xd5,0x32,0x9e,0x4b,0x2f,0x1d },
	{ 0xcb,0x23,0xe9,0x22,0x64,0x47 },{ 0xc9,0x20,0xe0,0x29,0x69,0x49 },
	{ 0xcf,0x25,0xfb,0x34,0x7e,0x5b },{ 0xcd,0x26,0xf2,0x3f,0x73,0x55 },
	{ 0xc3,0x2f,0xcd,0x0e,0x50,0x7f },{ 0xc1,0x2c,0xc4,0x05,0x5d,0x71 },
	{ 0xc7,0x29,0xdf,0x18,0x4a,0x63 },{ 0xc5,0x2a,0xd6,0x13,0x47,0x6d },
	{ 0xfb,0x0b,0x31,0xca,0xdc,0xd7 },{ 0xf9,0x08,0x38,0xc1,0xd1,0xd9 },
	{ 0xff,0x0d,0x23,0xdc,0xc6,0xcb },{ 0xfd,0x0e,0x2a,0xd7,0xcb,0xc5 },
	{ 0xf3,0x07,0x15,0xe6,0xe8,0xef },{ 0xf1,0x04,0x1c,0xed,0xe5,0xe1 },
	{ 0xf7,0x01,0x07,0xf0,0xf2,0xf3 },{ 0xf5,0x02,0x0e,0xfb,0xff,0xfd },
	{ 0xeb,0x13,0x79,0x92,0xb4,0xa7 },{ 0xe9,0x10,0x70,0x99,0xb9,0xa9 },
	{ 0xef,0x15,0x6b,0x84,0xae,0xbb },{ 0xed,0x16,0x62,0x8f,0xa3,0xb5 },
	{ 0xe3,0x1f,0x5d,0xbe,0x80,0x9f },{ 0xe1,0x1c,0x54,0xb5,0x8d,0x91 },
	{ 0xe7,0x19,0x4f,0xa8,0x9a,0x83 },{ 0xe5,0x1a,0x46,0xa3,0x97,0x8d }
};

/*********************** FUNCTION DEFINITIONS ***********************/
// XORs the in and out buffers, storing the result in out. Length is in bytes.
static force_inline void aes_xor_buf(const uint8_t in[], uint8_t out[], size_t len)
{
	size_t idx;

	for (idx = 0; idx < len; idx++)
		out[idx] ^= in[idx];
}

/*******************
* AES
*******************/
/////////////////
// KEY EXPANSION
/////////////////

// Substitutes a word using the AES S-Box.
static force_inline uint32_t AES_SubWord(uint32_t word)
{
	uint32_t result;

	result = (int32_t)aes_sbox[(word >> 4) & 0x0000000F][word & 0x0000000F];
	result += (int32_t)aes_sbox[(word >> 12) & 0x0000000F][(word >> 8) & 0x0000000F] << 8;
	result += (int32_t)aes_sbox[(word >> 20) & 0x0000000F][(word >> 16) & 0x0000000F] << 16;
	result += (int32_t)aes_sbox[(word >> 28) & 0x0000000F][(word >> 24) & 0x0000000F] << 24;
	return(result);
}

// Performs the action of generating the keys that will be used in every round of
// encryption. "key" is the user-supplied input key, "w" is the output key schedule,
// "keysize" is the length in bits of "key", must be 128, 192, or 256.
static force_inline void aes_key_setup(const uint8_t key[], uint32_t w[], int32_t keysize)
{
	int32_t Nb = 4, Nr, Nk, idx;
	uint32_t temp, Rcon[] = { 0x01000000,0x02000000,0x04000000,0x08000000,0x10000000,0x20000000,
		0x40000000,0x80000000,0x1b000000,0x36000000,0x6c000000,0xd8000000,
		0xab000000,0x4d000000,0x9a000000 };

	switch (keysize) {
	case 128: Nr = 10; Nk = 4; break;
	case 192: Nr = 12; Nk = 6; break;
	case 256: Nr = 14; Nk = 8; break;
	default: return;
	}

	for (idx = 0; idx < Nk; ++idx) {
		w[idx] = ((key[4 * idx]) << 24) | ((key[4 * idx + 1]) << 16) |
			((key[4 * idx + 2]) << 8) | ((key[4 * idx + 3]));
	}

	for (idx = Nk; idx < Nb * (Nr + 1); ++idx) {
		temp = w[idx - 1];
		if ((idx % Nk) == 0)
			temp = AES_SubWord(AES_KE_ROTWORD(temp)) ^ Rcon[(idx - 1) / Nk];
		else if (Nk > 6 && (idx % Nk) == 4)
			temp = AES_SubWord(temp);
		w[idx] = w[idx - Nk] ^ temp;
	}
}

/////////////////
// ADD ROUND KEY
/////////////////

// Performs the AES_AddRoundKey step. Each round has its own pre-generated 16-byte key in the
// form of 4 integers (the "w" array). Each integer is XOR'd by one column of the state.
// Also performs the job of InvAddRoundKey(); since the function is a simple XOR process,
// it is its own inverse.
static force_inline void AES_AddRoundKey(uint8_t state[][4], const uint32_t w[])
{
	uint8_t subkey[4];

	// memcpy(subkey,&w[idx],4); // Not accurate for big endian machines
	// Subkey 1
	subkey[0] = w[0] >> 24;
	subkey[1] = w[0] >> 16;
	subkey[2] = w[0] >> 8;
	subkey[3] = w[0];
	state[0][0] ^= subkey[0];
	state[1][0] ^= subkey[1];
	state[2][0] ^= subkey[2];
	state[3][0] ^= subkey[3];
	// Subkey 2
	subkey[0] = w[1] >> 24;
	subkey[1] = w[1] >> 16;
	subkey[2] = w[1] >> 8;
	subkey[3] = w[1];
	state[0][1] ^= subkey[0];
	state[1][1] ^= subkey[1];
	state[2][1] ^= subkey[2];
	state[3][1] ^= subkey[3];
	// Subkey 3
	subkey[0] = w[2] >> 24;
	subkey[1] = w[2] >> 16;
	subkey[2] = w[2] >> 8;
	subkey[3] = w[2];
	state[0][2] ^= subkey[0];
	state[1][2] ^= subkey[1];
	state[2][2] ^= subkey[2];
	state[3][2] ^= subkey[3];
	// Subkey 4
	subkey[0] = w[3] >> 24;
	subkey[1] = w[3] >> 16;
	subkey[2] = w[3] >> 8;
	subkey[3] = w[3];
	state[0][3] ^= subkey[0];
	state[1][3] ^= subkey[1];
	state[2][3] ^= subkey[2];
	state[3][3] ^= subkey[3];
}

/////////////////
// (Inv)AES_SubBytes
/////////////////

// Performs the AES_SubBytes step. All bytes in the state are substituted with a
// pre-calculated value from a lookup table.
static force_inline void AES_SubBytes(uint8_t state[][4])
{
	state[0][0] = aes_sbox[state[0][0] >> 4][state[0][0] & 0x0F];
	state[0][1] = aes_sbox[state[0][1] >> 4][state[0][1] & 0x0F];
	state[0][2] = aes_sbox[state[0][2] >> 4][state[0][2] & 0x0F];
	state[0][3] = aes_sbox[state[0][3] >> 4][state[0][3] & 0x0F];
	state[1][0] = aes_sbox[state[1][0] >> 4][state[1][0] & 0x0F];
	state[1][1] = aes_sbox[state[1][1] >> 4][state[1][1] & 0x0F];
	state[1][2] = aes_sbox[state[1][2] >> 4][state[1][2] & 0x0F];
	state[1][3] = aes_sbox[state[1][3] >> 4][state[1][3] & 0x0F];
	state[2][0] = aes_sbox[state[2][0] >> 4][state[2][0] & 0x0F];
	state[2][1] = aes_sbox[state[2][1] >> 4][state[2][1] & 0x0F];
	state[2][2] = aes_sbox[state[2][2] >> 4][state[2][2] & 0x0F];
	state[2][3] = aes_sbox[state[2][3] >> 4][state[2][3] & 0x0F];
	state[3][0] = aes_sbox[state[3][0] >> 4][state[3][0] & 0x0F];
	state[3][1] = aes_sbox[state[3][1] >> 4][state[3][1] & 0x0F];
	state[3][2] = aes_sbox[state[3][2] >> 4][state[3][2] & 0x0F];
	state[3][3] = aes_sbox[state[3][3] >> 4][state[3][3] & 0x0F];
}

static force_inline void AES_InvSubBytes(uint8_t state[][4])
{
	state[0][0] = aes_invsbox[state[0][0] >> 4][state[0][0] & 0x0F];
	state[0][1] = aes_invsbox[state[0][1] >> 4][state[0][1] & 0x0F];
	state[0][2] = aes_invsbox[state[0][2] >> 4][state[0][2] & 0x0F];
	state[0][3] = aes_invsbox[state[0][3] >> 4][state[0][3] & 0x0F];
	state[1][0] = aes_invsbox[state[1][0] >> 4][state[1][0] & 0x0F];
	state[1][1] = aes_invsbox[state[1][1] >> 4][state[1][1] & 0x0F];
	state[1][2] = aes_invsbox[state[1][2] >> 4][state[1][2] & 0x0F];
	state[1][3] = aes_invsbox[state[1][3] >> 4][state[1][3] & 0x0F];
	state[2][0] = aes_invsbox[state[2][0] >> 4][state[2][0] & 0x0F];
	state[2][1] = aes_invsbox[state[2][1] >> 4][state[2][1] & 0x0F];
	state[2][2] = aes_invsbox[state[2][2] >> 4][state[2][2] & 0x0F];
	state[2][3] = aes_invsbox[state[2][3] >> 4][state[2][3] & 0x0F];
	state[3][0] = aes_invsbox[state[3][0] >> 4][state[3][0] & 0x0F];
	state[3][1] = aes_invsbox[state[3][1] >> 4][state[3][1] & 0x0F];
	state[3][2] = aes_invsbox[state[3][2] >> 4][state[3][2] & 0x0F];
	state[3][3] = aes_invsbox[state[3][3] >> 4][state[3][3] & 0x0F];
}

/////////////////
// (Inv)AES_ShiftRows
/////////////////

// Performs the AES_ShiftRows step. All rows are shifted cylindrically to the left.
static force_inline void AES_ShiftRows(uint8_t state[][4])
{
	int32_t t;

	// Shift left by 1
	t = state[1][0];
	state[1][0] = state[1][1];
	state[1][1] = state[1][2];
	state[1][2] = state[1][3];
	state[1][3] = t;
	// Shift left by 2
	t = state[2][0];
	state[2][0] = state[2][2];
	state[2][2] = t;
	t = state[2][1];
	state[2][1] = state[2][3];
	state[2][3] = t;
	// Shift left by 3
	t = state[3][0];
	state[3][0] = state[3][3];
	state[3][3] = state[3][2];
	state[3][2] = state[3][1];
	state[3][1] = t;
}

// All rows are shifted cylindrically to the right.
static force_inline void AES_InvShiftRows(uint8_t state[][4])
{
	int32_t t;

	// Shift right by 1
	t = state[1][3];
	state[1][3] = state[1][2];
	state[1][2] = state[1][1];
	state[1][1] = state[1][0];
	state[1][0] = t;
	// Shift right by 2
	t = state[2][3];
	state[2][3] = state[2][1];
	state[2][1] = t;
	t = state[2][2];
	state[2][2] = state[2][0];
	state[2][0] = t;
	// Shift right by 3
	t = state[3][3];
	state[3][3] = state[3][0];
	state[3][0] = state[3][1];
	state[3][1] = state[3][2];
	state[3][2] = t;
}

/////////////////
// (Inv)AES_MixColumns
/////////////////

// Performs the MixColums step. The state is multiplied by itself using matrix
// multiplication in a Galios Field 2^8. All multiplication is pre-computed in a table.
// Addition is equivilent to XOR. (Must always make a copy of the column as the original
// values will be destoyed.)
static force_inline void AES_MixColumns(uint8_t state[][4])
{
	uint8_t col[4];

	// Column 1
	col[0] = state[0][0];
	col[1] = state[1][0];
	col[2] = state[2][0];
	col[3] = state[3][0];
	state[0][0] = aes_gf_mul[col[0]][0];
	state[0][0] ^= aes_gf_mul[col[1]][1];
	state[0][0] ^= col[2];
	state[0][0] ^= col[3];
	state[1][0] = col[0];
	state[1][0] ^= aes_gf_mul[col[1]][0];
	state[1][0] ^= aes_gf_mul[col[2]][1];
	state[1][0] ^= col[3];
	state[2][0] = col[0];
	state[2][0] ^= col[1];
	state[2][0] ^= aes_gf_mul[col[2]][0];
	state[2][0] ^= aes_gf_mul[col[3]][1];
	state[3][0] = aes_gf_mul[col[0]][1];
	state[3][0] ^= col[1];
	state[3][0] ^= col[2];
	state[3][0] ^= aes_gf_mul[col[3]][0];
	// Column 2
	col[0] = state[0][1];
	col[1] = state[1][1];
	col[2] = state[2][1];
	col[3] = state[3][1];
	state[0][1] = aes_gf_mul[col[0]][0];
	state[0][1] ^= aes_gf_mul[col[1]][1];
	state[0][1] ^= col[2];
	state[0][1] ^= col[3];
	state[1][1] = col[0];
	state[1][1] ^= aes_gf_mul[col[1]][0];
	state[1][1] ^= aes_gf_mul[col[2]][1];
	state[1][1] ^= col[3];
	state[2][1] = col[0];
	state[2][1] ^= col[1];
	state[2][1] ^= aes_gf_mul[col[2]][0];
	state[2][1] ^= aes_gf_mul[col[3]][1];
	state[3][1] = aes_gf_mul[col[0]][1];
	state[3][1] ^= col[1];
	state[3][1] ^= col[2];
	state[3][1] ^= aes_gf_mul[col[3]][0];
	// Column 3
	col[0] = state[0][2];
	col[1] = state[1][2];
	col[2] = state[2][2];
	col[3] = state[3][2];
	state[0][2] = aes_gf_mul[col[0]][0];
	state[0][2] ^= aes_gf_mul[col[1]][1];
	state[0][2] ^= col[2];
	state[0][2] ^= col[3];
	state[1][2] = col[0];
	state[1][2] ^= aes_gf_mul[col[1]][0];
	state[1][2] ^= aes_gf_mul[col[2]][1];
	state[1][2] ^= col[3];
	state[2][2] = col[0];
	state[2][2] ^= col[1];
	state[2][2] ^= aes_gf_mul[col[2]][0];
	state[2][2] ^= aes_gf_mul[col[3]][1];
	state[3][2] = aes_gf_mul[col[0]][1];
	state[3][2] ^= col[1];
	state[3][2] ^= col[2];
	state[3][2] ^= aes_gf_mul[col[3]][0];
	// Column 4
	col[0] = state[0][3];
	col[1] = state[1][3];
	col[2] = state[2][3];
	col[3] = state[3][3];
	state[0][3] = aes_gf_mul[col[0]][0];
	state[0][3] ^= aes_gf_mul[col[1]][1];
	state[0][3] ^= col[2];
	state[0][3] ^= col[3];
	state[1][3] = col[0];
	state[1][3] ^= aes_gf_mul[col[1]][0];
	state[1][3] ^= aes_gf_mul[col[2]][1];
	state[1][3] ^= col[3];
	state[2][3] = col[0];
	state[2][3] ^= col[1];
	state[2][3] ^= aes_gf_mul[col[2]][0];
	state[2][3] ^= aes_gf_mul[col[3]][1];
	state[3][3] = aes_gf_mul[col[0]][1];
	state[3][3] ^= col[1];
	state[3][3] ^= col[2];
	state[3][3] ^= aes_gf_mul[col[3]][0];
}

static force_inline void AES_InvMixColumns(uint8_t state[][4])
{
	uint8_t col[4];

	// Column 1
	col[0] = state[0][0];
	col[1] = state[1][0];
	col[2] = state[2][0];
	col[3] = state[3][0];
	state[0][0] = aes_gf_mul[col[0]][5];
	state[0][0] ^= aes_gf_mul[col[1]][3];
	state[0][0] ^= aes_gf_mul[col[2]][4];
	state[0][0] ^= aes_gf_mul[col[3]][2];
	state[1][0] = aes_gf_mul[col[0]][2];
	state[1][0] ^= aes_gf_mul[col[1]][5];
	state[1][0] ^= aes_gf_mul[col[2]][3];
	state[1][0] ^= aes_gf_mul[col[3]][4];
	state[2][0] = aes_gf_mul[col[0]][4];
	state[2][0] ^= aes_gf_mul[col[1]][2];
	state[2][0] ^= aes_gf_mul[col[2]][5];
	state[2][0] ^= aes_gf_mul[col[3]][3];
	state[3][0] = aes_gf_mul[col[0]][3];
	state[3][0] ^= aes_gf_mul[col[1]][4];
	state[3][0] ^= aes_gf_mul[col[2]][2];
	state[3][0] ^= aes_gf_mul[col[3]][5];
	// Column 2
	col[0] = state[0][1];
	col[1] = state[1][1];
	col[2] = state[2][1];
	col[3] = state[3][1];
	state[0][1] = aes_gf_mul[col[0]][5];
	state[0][1] ^= aes_gf_mul[col[1]][3];
	state[0][1] ^= aes_gf_mul[col[2]][4];
	state[0][1] ^= aes_gf_mul[col[3]][2];
	state[1][1] = aes_gf_mul[col[0]][2];
	state[1][1] ^= aes_gf_mul[col[1]][5];
	state[1][1] ^= aes_gf_mul[col[2]][3];
	state[1][1] ^= aes_gf_mul[col[3]][4];
	state[2][1] = aes_gf_mul[col[0]][4];
	state[2][1] ^= aes_gf_mul[col[1]][2];
	state[2][1] ^= aes_gf_mul[col[2]][5];
	state[2][1] ^= aes_gf_mul[col[3]][3];
	state[3][1] = aes_gf_mul[col[0]][3];
	state[3][1] ^= aes_gf_mul[col[1]][4];
	state[3][1] ^= aes_gf_mul[col[2]][2];
	state[3][1] ^= aes_gf_mul[col[3]][5];
	// Column 3
	col[0] = state[0][2];
	col[1] = state[1][2];
	col[2] = state[2][2];
	col[3] = state[3][2];
	state[0][2] = aes_gf_mul[col[0]][5];
	state[0][2] ^= aes_gf_mul[col[1]][3];
	state[0][2] ^= aes_gf_mul[col[2]][4];
	state[0][2] ^= aes_gf_mul[col[3]][2];
	state[1][2] = aes_gf_mul[col[0]][2];
	state[1][2] ^= aes_gf_mul[col[1]][5];
	state[1][2] ^= aes_gf_mul[col[2]][3];
	state[1][2] ^= aes_gf_mul[col[3]][4];
	state[2][2] = aes_gf_mul[col[0]][4];
	state[2][2] ^= aes_gf_mul[col[1]][2];
	state[2][2] ^= aes_gf_mul[col[2]][5];
	state[2][2] ^= aes_gf_mul[col[3]][3];
	state[3][2] = aes_gf_mul[col[0]][3];
	state[3][2] ^= aes_gf_mul[col[1]][4];
	state[3][2] ^= aes_gf_mul[col[2]][2];
	state[3][2] ^= aes_gf_mul[col[3]][5];
	// Column 4
	col[0] = state[0][3];
	col[1] = state[1][3];
	col[2] = state[2][3];
	col[3] = state[3][3];
	state[0][3] = aes_gf_mul[col[0]][5];
	state[0][3] ^= aes_gf_mul[col[1]][3];
	state[0][3] ^= aes_gf_mul[col[2]][4];
	state[0][3] ^= aes_gf_mul[col[3]][2];
	state[1][3] = aes_gf_mul[col[0]][2];
	state[1][3] ^= aes_gf_mul[col[1]][5];
	state[1][3] ^= aes_gf_mul[col[2]][3];
	state[1][3] ^= aes_gf_mul[col[3]][4];
	state[2][3] = aes_gf_mul[col[0]][4];
	state[2][3] ^= aes_gf_mul[col[1]][2];
	state[2][3] ^= aes_gf_mul[col[2]][5];
	state[2][3] ^= aes_gf_mul[col[3]][3];
	state[3][3] = aes_gf_mul[col[0]][3];
	state[3][3] ^= aes_gf_mul[col[1]][4];
	state[3][3] ^= aes_gf_mul[col[2]][2];
	state[3][3] ^= aes_gf_mul[col[3]][5];
}

/////////////////
// (En/De)Crypt
/////////////////

static force_inline void aes_encrypt(const uint8_t in[], uint8_t out[], const uint32_t key[], int32_t keysize)
{
	uint8_t state[4][4];

	// Copy input array (should be 16 bytes long) to a matrix (sequential bytes are ordered
	// by row, not col) called "state" for processing.
	// *** Implementation note: The official AES documentation references the state by
	// column, then row. Accessing an element in C requires row then column. Thus, all state
	// references in AES must have the column and row indexes reversed for C implementation.
	state[0][0] = in[0];
	state[1][0] = in[1];
	state[2][0] = in[2];
	state[3][0] = in[3];
	state[0][1] = in[4];
	state[1][1] = in[5];
	state[2][1] = in[6];
	state[3][1] = in[7];
	state[0][2] = in[8];
	state[1][2] = in[9];
	state[2][2] = in[10];
	state[3][2] = in[11];
	state[0][3] = in[12];
	state[1][3] = in[13];
	state[2][3] = in[14];
	state[3][3] = in[15];

	// Perform the necessary number of rounds. The round key is added first.
	// The last round does not perform the AES_MixColumns step.
	AES_AddRoundKey(state, &key[0]);
	AES_SubBytes(state); AES_ShiftRows(state); AES_MixColumns(state); AES_AddRoundKey(state, &key[4]);
	AES_SubBytes(state); AES_ShiftRows(state); AES_MixColumns(state); AES_AddRoundKey(state, &key[8]);
	AES_SubBytes(state); AES_ShiftRows(state); AES_MixColumns(state); AES_AddRoundKey(state, &key[12]);
	AES_SubBytes(state); AES_ShiftRows(state); AES_MixColumns(state); AES_AddRoundKey(state, &key[16]);
	AES_SubBytes(state); AES_ShiftRows(state); AES_MixColumns(state); AES_AddRoundKey(state, &key[20]);
	AES_SubBytes(state); AES_ShiftRows(state); AES_MixColumns(state); AES_AddRoundKey(state, &key[24]);
	AES_SubBytes(state); AES_ShiftRows(state); AES_MixColumns(state); AES_AddRoundKey(state, &key[28]);
	AES_SubBytes(state); AES_ShiftRows(state); AES_MixColumns(state); AES_AddRoundKey(state, &key[32]);
	AES_SubBytes(state); AES_ShiftRows(state); AES_MixColumns(state); AES_AddRoundKey(state, &key[36]);
	if (keysize != 128) {
		AES_SubBytes(state); AES_ShiftRows(state); AES_MixColumns(state); AES_AddRoundKey(state, &key[40]);
		AES_SubBytes(state); AES_ShiftRows(state); AES_MixColumns(state); AES_AddRoundKey(state, &key[44]);
		if (keysize != 192) {
			AES_SubBytes(state); AES_ShiftRows(state); AES_MixColumns(state); AES_AddRoundKey(state, &key[48]);
			AES_SubBytes(state); AES_ShiftRows(state); AES_MixColumns(state); AES_AddRoundKey(state, &key[52]);
			AES_SubBytes(state); AES_ShiftRows(state); AES_AddRoundKey(state, &key[56]);
		}
		else {
			AES_SubBytes(state); AES_ShiftRows(state); AES_AddRoundKey(state, &key[48]);
		}
	}
	else {
		AES_SubBytes(state); AES_ShiftRows(state); AES_AddRoundKey(state, &key[40]);
	}

	// Copy the state to the output array.
	out[0] = state[0][0];
	out[1] = state[1][0];
	out[2] = state[2][0];
	out[3] = state[3][0];
	out[4] = state[0][1];
	out[5] = state[1][1];
	out[6] = state[2][1];
	out[7] = state[3][1];
	out[8] = state[0][2];
	out[9] = state[1][2];
	out[10] = state[2][2];
	out[11] = state[3][2];
	out[12] = state[0][3];
	out[13] = state[1][3];
	out[14] = state[2][3];
	out[15] = state[3][3];
}

static force_inline void aes_decrypt(const uint8_t in[], uint8_t out[], const uint32_t key[], int32_t keysize)
{
	uint8_t state[4][4];

	// Copy the input to the state.
	state[0][0] = in[0];
	state[1][0] = in[1];
	state[2][0] = in[2];
	state[3][0] = in[3];
	state[0][1] = in[4];
	state[1][1] = in[5];
	state[2][1] = in[6];
	state[3][1] = in[7];
	state[0][2] = in[8];
	state[1][2] = in[9];
	state[2][2] = in[10];
	state[3][2] = in[11];
	state[0][3] = in[12];
	state[1][3] = in[13];
	state[2][3] = in[14];
	state[3][3] = in[15];

	// Perform the necessary number of rounds. The round key is added first.
	// The last round does not perform the AES_MixColumns step.
	if (keysize > 128) {
		if (keysize > 192) {
			AES_AddRoundKey(state, &key[56]);
			AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[52]);AES_InvMixColumns(state);
			AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[48]);AES_InvMixColumns(state);
		}
		else {
			AES_AddRoundKey(state, &key[48]);
		}
		AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[44]);AES_InvMixColumns(state);
		AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[40]);AES_InvMixColumns(state);
	}
	else {
		AES_AddRoundKey(state, &key[40]);
	}
	AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[36]);AES_InvMixColumns(state);
	AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[32]);AES_InvMixColumns(state);
	AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[28]);AES_InvMixColumns(state);
	AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[24]);AES_InvMixColumns(state);
	AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[20]);AES_InvMixColumns(state);
	AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[16]);AES_InvMixColumns(state);
	AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[12]);AES_InvMixColumns(state);
	AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[8]);AES_InvMixColumns(state);
	AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[4]);AES_InvMixColumns(state);
	AES_InvShiftRows(state);AES_InvSubBytes(state);AES_AddRoundKey(state, &key[0]);

	// Copy the state to the output array.
	out[0] = state[0][0];
	out[1] = state[1][0];
	out[2] = state[2][0];
	out[3] = state[3][0];
	out[4] = state[0][1];
	out[5] = state[1][1];
	out[6] = state[2][1];
	out[7] = state[3][1];
	out[8] = state[0][2];
	out[9] = state[1][2];
	out[10] = state[2][2];
	out[11] = state[3][2];
	out[12] = state[0][3];
	out[13] = state[1][3];
	out[14] = state[2][3];
	out[15] = state[3][3];
}

#define AES_BUFF_LEN 16
#define AES_KEY_BUFF_LEN 16

static force_inline void faes(const std::string & strkey, const char * s_text, size_t s_len, uint8_t(&d_text)[AES_BUFF_LEN])
{
	uint8_t key[AES_KEY_BUFF_LEN];
	memset(key, 0, sizeof(key));
	memcpy(key, strkey.c_str(), Min<size_t>(strkey.size(), AES_KEY_BUFF_LEN));

	uint8_t stext[AES_BUFF_LEN];
	memset(stext, 0, sizeof(stext));
	memcpy(stext, s_text, Min<size_t>(s_len, AES_BUFF_LEN));

	uint32_t w[256];
	aes_key_setup(key, w, AES_KEY_BUFF_LEN * 8);
	aes_encrypt(stext, d_text, w, AES_KEY_BUFF_LEN * 8);
}

static force_inline std::string faes(const std::string & strkey, const std::string & s_text)
{
	uint8_t d_text[AES_BUFF_LEN];
	faes(strkey, s_text.c_str(), s_text.size(), d_text);

	std::string ret;
	for (int32_t i = 0; i < AES_BUFF_LEN; i++)
	{
		std::string rettmp = fitoa16((uint8_t)d_text[i]);

		if (rettmp.size() == 1)
		{
			ret += "0";
			ret += rettmp;
		}
		else
		{
			ret += rettmp;
		}
	}

	return ret;
}

static force_inline void funaes(const std::string & strkey, const uint8_t * s_text, size_t s_len, uint8_t(&d_text)[AES_BUFF_LEN])
{
	uint8_t key[AES_KEY_BUFF_LEN];
	memset(key, 0, sizeof(key));
	memcpy(key, strkey.c_str(), Min<size_t>(strkey.size(), AES_KEY_BUFF_LEN));

	uint8_t stext[AES_BUFF_LEN];
	memset(stext, 0, sizeof(stext));
	memcpy(stext, s_text, Min<size_t>(s_len, AES_BUFF_LEN));

	uint32_t w[256];
	aes_key_setup(key, w, AES_KEY_BUFF_LEN * 8);
	aes_decrypt(stext, d_text, w, AES_KEY_BUFF_LEN * 8);
}

static force_inline std::string funaes(const std::string & strkey, const std::string & s_text)
{
	uint8_t stext[AES_BUFF_LEN];
	memset(stext, 0, sizeof(stext));
	for (int32_t i = 0; i < AES_BUFF_LEN; i++)
	{
		std::string tmp = s_text.substr(2 * i, 2);
		stext[i] = fatoi16(tmp);
	}

	union
	{
		uint8_t d_buff[AES_BUFF_LEN + 1];
		uint8_t d_text[AES_BUFF_LEN];
	} dtext;
	memset(&dtext, 0, sizeof(dtext));
	funaes(strkey, stext, AES_BUFF_LEN, dtext.d_text);

	return std::string((const char*)dtext.d_text);
}

/////////////////////////////////////////////////////////////

#ifdef WIN32

// Test for a NaN (not a number) value - usually defined in math.h
static force_inline int32_t isnan(double x)
{
	return _isnan(x);
}

// Test for infinity - usually defined in math.h
static force_inline int32_t isinf(double x)
{
	return (_fpclass(x) & (_FPCLASS_PINF | _FPCLASS_NINF)) != 0;
}

// Test if x is less than y and both nominal - usually defined in math.h
static force_inline int32_t isless(double x, double y)
{
	return isnan(x) || isnan(y) ? 0 : x < y;
}

// Test if x is greater than y and both nominal - usually defined in math.h
static force_inline int32_t isgreater(double x, double y)
{
	return isnan(x) || isnan(y) ? 0 : x > y;
}

#endif

/////////////////////////////////////////////////////////////

static force_inline void fdebug_break()
{
#ifdef WIN32
	__debugbreak();
#else
	asm("int $3");
#endif
}

static force_inline void fshow_call_stack(std::string & ret)
{
#ifdef WIN32
	// win下太复杂，不提供
	ret.clear();
#else
	void * trace[256];
	memset(trace, 0, sizeof(trace));

	int32_t trace_size = backtrace(trace, ARRAY_SIZE(trace));
	char ** messages = backtrace_symbols( trace, trace_size );
	ret.clear();
	if (messages == 0)
	{	
		return;
	}

	for(int32_t i = 0; i < trace_size; ++i) 
	{
		ret += (std::string)messages[i] + "\n";
	}

	free(messages);
#endif
}

static force_inline void fshow_color_text(const char * text, PRINTF_CLOLOR_TYPE color)
{
#ifndef WIN32
	FPRINTF("%s%s\e[0m", color, text);
#else
	HANDLE hStd = ::GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStd == INVALID_HANDLE_VALUE)
	{
		return;
	}
	CONSOLE_SCREEN_BUFFER_INFO oldInfo;
	if (!GetConsoleScreenBufferInfo(hStd, &oldInfo))
	{
		return;
	}
	else
	{
		SetConsoleTextAttribute(hStd, color);
		FPRINTF("%s", text);
		SetConsoleTextAttribute(hStd, oldInfo.wAttributes);
	}
#endif
	return;
}

/////////////////////////////////////////////////////////////

template <class T>
class singleton
{
public:
	singleton(){}
	virtual ~singleton(){}
	static T * pData;
	static force_inline void	release();
	static force_inline T * ptr();
	static force_inline T & ref();
};

template <class T>
T* singleton<T>::pData = 0;

template <class T> force_inline
T* singleton<T>::ptr()
{
	if(!pData)
	{
		pData = new T();
	}
	return pData;
}

template <class T> force_inline
T& singleton<T>::ref()
{
	if(!pData)
	{
		pData = new T();
	}
	return *pData;
}

template <class T> force_inline
void singleton<T>::release()
{
	SAFE_DELETE(T, pData);
}

/////////////////////////////////////////////////////////////

/*
type:           [1]
iter:     begin(2)     end(8)
            |           |
data:   _ _ * * * * * * _ _ _ 
buffer: _ _ _ _ _ _ _ _ _ _ _ 
index:  0 1 2 3 4 5 6 7 8 9 10

type:           [2]
iter:      end(2)   begin(7)
            |         |
data:   * * _ _ _ _ _ * * * *  
buffer: _ _ _ _ _ _ _ _ _ _ _ 
index:  0 1 2 3 4 5 6 7 8 9 10


type:           [3]
iter:      begin(4),end(4)
                |
data:   _ _ _ _ _ _ _ _ _ _ _ 
buffer: _ _ _ _ _ _ _ _ _ _ _ 
index:  0 1 2 3 4 5 6 7 8 9 10

type:           [4]
iter:      begin(4),end(4)
|				 |
data:   * * * * * * * * * * *
buffer: _ _ _ _ _ _ _ _ _ _ _
index:  0 1 2 3 4 5 6 7 8 9 10

*/
template <typename T, size_t N>
class circle_buffer
{
public:
	circle_buffer() : m_datasize(0), m_begin(0), m_end(0),
		m_store_datasize(0), m_store_begin(0), m_store_end(0)
	{
	}
	~circle_buffer()
	{
	}
public:
	force_inline bool can_write(size_t size)
	{
		return m_datasize + size <= N;
	}
	force_inline void skip_write(size_t size)
	{
		m_datasize += size;
		m_end += size;
		if (m_end >= N)
		{
			m_end -= N;
		}
	}
	force_inline bool write(const T * p, size_t size)
	{
		if (!can_write(size))
		{
			return false;
		}

		real_write(p, size);

		skip_write(size);

		return true;
	}
	template <typename OT>
	force_inline bool write(const OT & ot)
	{
		store();
		if (!ot.serialize(*this))
		{
			restore();
			return false;
		}
		return true;
	}
	force_inline bool can_read(size_t size)
	{
		return m_datasize >= size;
	}
	force_inline void skip_read(size_t size)
	{
		m_datasize -= size;
		m_begin += size;
		if (m_begin >= N)
		{
			m_begin -= N;
		}
		if (!m_datasize)
		{
			m_begin = 0;
			m_end = 0;
		}
	}
	force_inline bool read(T * out, size_t size)
	{
		if (!can_read(size))
		{
			return false;
		}

		real_read(out, size);

		skip_read(size);

		return true;
	}
	template <typename OT>
	force_inline bool read(OT & ot)
	{
		store();
		if (!ot.deserialize(*this))
		{
			restore();
			return false;
		}
		return true;
	}
	force_inline void store()
	{
		m_store_datasize = m_datasize;
		m_store_begin = m_begin;
		m_store_end = m_end;
	}
	force_inline void restore()
	{
		m_datasize = m_store_datasize;
		m_begin = m_store_begin;
		m_end = m_store_end;
	}
	force_inline bool clear()
	{
		m_datasize = 0;
		m_begin = 0;
		m_end = 0;
#ifdef _DEBUG
		memset(m_buffer, 0xFF, sizeof(m_buffer));
#endif
		return true;
	}
	force_inline size_t size() const
	{
		return m_datasize;
	}
	force_inline size_t capacity() const
	{
		return N;
	}
	force_inline bool empty() const
	{
		return m_datasize == 0;
	}
	force_inline bool full() const
	{
		return m_datasize == N;
	}
	force_inline T * get_read_line_buffer()
	{
		return m_buffer + m_begin;
	}
	force_inline size_t get_read_line_size()
	{
		return Min<size_t>(m_datasize, N - m_begin);
	}
	force_inline T * get_write_line_buffer()
	{
		return m_buffer + m_end;
	}
	force_inline size_t get_write_line_size()
	{
		return Min<size_t>(N - m_datasize, N - m_end);
	}
private:
	force_inline void real_write(const T * p, size_t size)
	{
		if (m_end >= m_begin)	// [1][3]
		{
			// 能装下
			if (N - m_end >= size)
			{
				memcpy(m_buffer + m_end, p, size * sizeof(T));
			}
			else
			{
				memcpy(m_buffer + m_end, p, (N - m_end) * sizeof(T));
				memcpy(m_buffer, p + (N - m_end), (size - (N - m_end)) * sizeof(T));
			}
		}
		else	//[2]
		{
			memcpy(m_buffer + m_end, p, size * sizeof(T));
		}
	}
	force_inline void real_read(T * out, size_t size)
	{
		if (m_begin >= m_end)	// [2][4]
		{
			// 能读完
			if (N - m_begin >= size)
			{
				memcpy(out, m_buffer + m_begin, size * sizeof(T));
			}
			else
			{
				memcpy(out, m_buffer + m_begin, (N - m_begin) * sizeof(T));
				memcpy(out + (N - m_begin), m_buffer, (size - (N - m_begin)) * sizeof(T));
			}
		}
		else	// [1]
		{
			memcpy(out, m_buffer + m_begin, size * sizeof(T));
		}
	}
private:
	T m_buffer[N];
	size_t m_datasize;
	size_t m_begin;
	size_t m_end;
	size_t m_store_datasize;
	size_t m_store_begin;
	size_t m_store_end;
};

/////////////////////////////////////////////////////////////

template <typename T, typename F>
class slot
{
public:
	slot() : m_function(0), m_object(0)
	{
	}
	
	slot(F func, T * obj) : m_function(func), m_object(obj)
	{
	}

	~slot() 
	{
	}

	void ini(F func, T * obj)
	{
		m_function = func;
		m_object = obj;
	}

	bool operator()()
	{
		return (m_object->*m_function)();
	}

	template <typename P1>
	bool operator()(P1 param1) 
	{
		return (m_object->*m_function)(param1);
	}

	template <typename P1>
	bool operator()(P1 param1) const
	{
		return (m_object->*m_function)(param1);
	}

	template <typename P1, typename P2>
	bool operator()(P1 param1, P2 param2)
	{
		return (m_object->*m_function)(param1, param2);
	}

	template <typename P1, typename P2>
	bool operator()(P1 param1, P2 param2) const
	{
		return (m_object->*m_function)(param1, param2);
	}

	template <typename P1, typename P2, typename P3>
	bool operator()(P1 param1, P2 param2, P3 param3)
	{
		return (m_object->*m_function)(param1, param2, param3);
	}

	template <typename P1, typename P2, typename P3>
	bool operator()(P1 param1, P2 param2, P3 param3) const
	{
		return (m_object->*m_function)(param1, param2, param3);
	}
private:
	F m_function;
	T * m_object;
};

template <typename F>
class slot<void, F>
{
public:
	slot() 
	{
	}

	slot(F f) : m_function(f) {}

	~slot() 
	{
	}

	bool operator()()
	{
		return m_function();
	}

	template <typename P1>
	bool operator()(P1 param1) 
	{
		return m_function(param1);
	}

	template <typename P1>
	bool operator()(P1 param1) const
	{
		return m_function(param1);
	}

	template <typename P1, typename P2>
	bool operator()(P1 param1, P2 param2)
	{
		return m_function(param1, param2);
	}

	template <typename P1, typename P2>
	bool operator()(P1 param1, P2 param2) const
	{
		return m_function(param1, param2);
	}

	template <typename P1, typename P2, typename P3>
	bool operator()(P1 param1, P2 param2, P3 param3)
	{
		return m_function(param1, param2, param3);
	}

	template <typename P1, typename P2, typename P3>
	bool operator()(P1 param1, P2 param2, P3 param3) const
	{
		return m_function(param1, param2, param3);
	}
private:
	F m_function;
};

/////////////////////////////////////////////////////////////

static force_inline time_t get_s_tick()
{
	return ::time(0);
}

static force_inline uint32_t get_ms_tick()
{
#ifdef WIN32
	return ::GetTickCount();
#else
	struct timeval tv;
	if(::gettimeofday(&tv, 0) == 0)
	{
		uint64_t t = tv.tv_sec * 1000;
		t += tv.tv_usec / 1000;
		return t &  0xffffffff;
	}
	return 0;
#endif
}

static force_inline uint64_t get_ns_tick()
{
#ifdef WIN32
	uint32_t dwLow, dwHigh;
	__asm
	{
		rdtsc
		mov dwLow, eax;
		mov dwHigh, edx;
	}
	return ((uint64_t)dwHigh << 32) | (uint64_t)dwLow;
#else
	uint32_t dwLow, dwHigh;
	__asm__ __volatile__("rdtsc" : "=a" (dwLow), "=d" (dwHigh));
	return ((uint64_t)dwHigh << 32) | (uint64_t)dwLow;
#endif
}

/////////////////////////////////////////////////////////////

/*
#comment xxx#
[sec0]
#comment xxx#
key0 = value0;
#comment xxx#
key1 = value1;

#comment xxx#
[sec1]
#comment xxx#
key0 = value0;
#comment xxx#
key1 = value1;

#comment xxx#
[sec2]
#comment xxx#
key0 = value0;
#comment xxx#
key1 = value1; 

*/

template <uint32_t SecN, uint32_t ValueN>
class inifile
{
private:
    typedef fhashmap<std::string, std::string, ValueN> _valueMap;
    typedef fhashmap<std::string, _valueMap, SecN> _secMap;
public:
	inifile()
	{
	}
	~inifile()
	{
	}
public:
	force_inline bool load(const std::string & file)
	{
		clear();

		std::string buff;
		if (!open(file, buff))
		{
			return false;
		}

		if (!parse(buff))
		{
			return false;
		}
		
		return true;
	}
	force_inline bool get(const std::string & sec, const std::string & key, 
		int32_t & value)
	{
		std::string tmp;
		if (get(sec, key, tmp))
		{
			value = fatoi(tmp);
			return true;
		}
		return false;
	}
	force_inline bool get(const std::string & sec, const std::string & key, 
		int32_t & value, int32_t defaultvalue)
	{
		std::string tmp;
		if (get(sec, key, tmp))
		{
			value = fatoi(tmp);
			return true;
		}
		value = defaultvalue;
		return false;
	}
	force_inline bool get(const std::string & sec, const std::string & key, 
		std::string & value)
	{
		typename _secMap::iterator it = m_map.find(sec);
		if (it != m_map.end())
		{
			_valueMap & map = it->second;
			typename _valueMap::iterator itex = map.find(key);
			if (itex != map.end())
			{
				value = itex->second;
				return true;
			}
		}
		return false;
	}
	force_inline bool get(const std::string & sec, const std::string & key, 
		std::string & value, const std::string & defaultvalue)
	{
		typename _secMap::iterator it = m_map.find(sec);
		if (it != m_map.end())
		{
			_valueMap & map = it->second;
			typename _valueMap::iterator itex = map.find(key);
			if (itex != map.end())
			{
				value = itex->second;
				return true;
			}
		}
		value = defaultvalue;
		return false;
	}
	force_inline void clear()
	{
		m_map.clear();
	}
private:
	force_inline bool open(const std::string & file, std::string & buff)
	{
	    const char * filename = (const char *)file.c_str();
		FILE * fp = fopen(filename, "r");
		if (!fp)
		{
			return false;
		}

		int8_t readbuff[100];
		size_t len = 0;
		while (!feof(fp))
		{
			len = fread((void*)readbuff, 1, sizeof(readbuff) - 1, fp);
			readbuff[len] = 0;
			buff += (char *)readbuff;
		}

		trim_string(buff);

		return true;
	}
	force_inline bool parse(std::string & buff)
	{
		while (buff.size() > 0)
		{
			std::string sec;
			if (!get_sec(buff, sec))
			{
				break;
			}
			
			std::string value;
			if (!get_value(buff, value))
			{
				break;
			}

			_valueMap valueMap;
			if (!parse_value(value, valueMap))
			{
				break;
			}
			
			if (!sec.empty() && valueMap.size() > 0)
			{
				m_map[sec] = valueMap;
			}
		}

		return true;
	}
	force_inline bool get_sec(std::string & buff, std::string & sec)
	{
		int32_t pos = buff.find('[');
		SAFE_TEST_RET_VAL(pos, -1, false);

		buff = buff.substr(pos + 1);
		pos = buff.find(']');
		SAFE_TEST_RET_VAL(pos, -1, false);

		sec = buff.substr(0, pos);
		buff = buff.substr(pos + 1);

		return true;
	}
	force_inline bool get_value(std::string & buff, std::string & value)
	{
		int32_t pos = buff.find('[');
		if (pos != -1)
		{
			value = buff.substr(0, pos);
			buff = buff.substr(pos);
		}
		else
		{
			value = buff;
			buff = "";
		}

		return true;
	}
	force_inline bool parse_value(std::string & value, _valueMap & valueMap)
	{
		while (value.size() > 0)
		{
			std::string line_value;
			if (!get_line_value(value, line_value))
			{
				break;
			}

			if (!parse_line_value(line_value, valueMap))
			{
				break;
			}
			
		}
		return true;
	}
	force_inline bool get_line_value(std::string & value, std::string & line_value)
	{
		int32_t pos = value.find(';');
		SAFE_TEST_RET_VAL(pos, -1, false);

		line_value = value.substr(0, pos);
		value = value.substr(pos + 1);
		
		return true;
	}
	force_inline bool parse_line_value(std::string & line_value, _valueMap & valueMap)
	{
		int32_t pos = line_value.find('=');
		SAFE_TEST_RET_VAL(pos, -1, false);

		std::string key = line_value.substr(0, pos);
		std::string value = line_value.substr(pos + 1);

		if (!key.empty() && !value.empty())
		{
			valueMap[key] = value;
		}

		return true;
	}
	force_inline void trim_string(std::string & str)
	{
		bool iscomment = false;
		for (int i = 0; i < (int)str.size(); i++)
		{
			char & c = (char &)str[i];
			if (c == '#')
			{
				iscomment = !iscomment;
				c = ' ';
			}
			else if (iscomment)
			{
				c = ' ';
			}
		}

		str.erase(std::remove(str.begin(), str.end(), '\t'), str.end());
		str.erase(std::remove(str.begin(), str.end(), '\n'), str.end());
		str.erase(std::remove(str.begin(), str.end(), '\r'), str.end());
		str.erase(std::remove(str.begin(), str.end(), ' '), str.end());
	}
private:
	_secMap m_map;
};

/////////////////////////////////////////////////////////////

// N是敏感词个数，M是首字母相同最多的个数，比如ABC，AB，AD，则M=3
template<typename _c, uint32_t N, uint32_t M>
class ftrie
{
public:
	ftrie()
	{

	}
	~ftrie()
	{

	}
	
	force_inline void clear()
	{
		m_dicinfomap.clear();
		m_dicset.clear();
	}

	force_inline bool insert(const std::string & str)
	{
		if (str.empty())
		{
			return false;
		}

		int32_t size = str.size();

		_c c = str[0];
		_infomap & infomap = m_dicinfomap[c];
		Node & node = infomap[size];
		node.len = size;
		node.times++;

		m_dicset.insert(str);

		return true;
	}
	
	force_inline bool erase(const std::string & str)
	{
		if (str.empty())
		{
			return false;
		}

		int32_t size = str.size();

		_c c = str[0];
		typename _dicinfomap::iterator it = m_dicinfomap.find(c);
		if (it == m_dicinfomap.end())
		{
			return false;
		}

		_infomap & infomap = it->second;
		typename _infomap::iterator it1 = infomap.find(size);
		if (it1 == infomap.end())
		{
			return false;
		}

		Node & node = it1->second;
		node.times--;

		if (node.times <= 0)
		{
			infomap.erase(size);
		}

		if (infomap.empty())
		{
			m_dicinfomap.erase(c);
		}

		m_dicset.erase(str);

		return true;
	}
	
	// 是否包含word 返回匹配的字数
	force_inline size_t ishaveword(const std::string & str)
	{
		if (str.empty())
		{
			return 0;
		}

		for (int32_t i = 0; i < (int32_t)str.size(); i++)
		{
			std::string tmp = str.substr(i);
			size_t ret = _ishaveword(tmp);
			if (ret)
			{
				return ret;
			}
		}

		return 0;
	}

	// 过滤word 返回过滤的字数
	force_inline size_t filterword(std::string & str, const _c & replaceword)
	{
		if (str.empty())
		{
			return 0;
		}

		size_t ret = 0;
		for (int32_t i = 0; i < (int32_t)str.size(); i++)
		{
			std::string tmp = str.substr(i);
			size_t _ret = _filterword(tmp);
			if (_ret)
			{
				for (int32_t j = 0; j < (int32_t)_ret; j++)
				{
					str[i + j] = replaceword;
				}
				ret += _ret;
			}
		}

		return ret;
	}

private:
	force_inline size_t _ishaveword(const std::string & str)
	{
		if (str.empty())
		{
			return 0;
		}

		int32_t size = str.size();

		_c c = str[0];
		typename _dicinfomap::iterator it = m_dicinfomap.find(c);
		if (it == m_dicinfomap.end())
		{
			return 0;
		}

		_infomap & infomap = it->second;
		for (typename _infomap::iterator it = infomap.begin(); it != infomap.end(); it++)
		{
			Node & node = it->second;
			if (node.len <= size)
			{
				std::string tmp = str.substr(0, node.len);
				if (m_dicset.find(tmp) != m_dicset.end())
				{
					return node.len;
				}
			}
		}

		return 0;
	}

	force_inline size_t _filterword(const std::string & str)
	{
		if (str.empty())
		{
			return 0;
		}

		int32_t size = str.size();

		_c c = str[0];
		typename _dicinfomap::iterator it = m_dicinfomap.find(c);
		if (it == m_dicinfomap.end())
		{
			return 0;
		}

		int32_t ret = 0;
		_infomap & infomap = it->second;
		for (typename _infomap::iterator it = infomap.begin(); it != infomap.end(); it++)
		{
			Node & node = it->second;
			if (node.len <= size)
			{
				std::string tmp = str.substr(0, node.len);
				if (m_dicset.find(tmp) != m_dicset.end())
				{
					ret = Max(ret, node.len);
				}
			}
		}

		return ret;
	}

private:
	struct Node
	{
		Node() : len(0), times(0)
		{

		}
		Node(int32_t _len, int32_t _times) : len(_len), times(_times)
		{

		}
		int32_t len;
		int32_t times;
	};
private:
	typedef fhashmap<int32_t, Node, M> _infomap;
    typedef fhashmap<_c, _infomap, N> _dicinfomap;
	typedef fhashset<std::string, N> _dicset;
	_dicinfomap m_dicinfomap;
	_dicset m_dicset;
};

/////////////////////////////////////////////////////////////

#ifdef WIN32
static force_inline bool fhook_func(uint8_t * target_function, uint8_t * newfuc, uint8_t (&old_mem)[JMP_CODE_LEN])
{
	DWORD old_target_function_protect = 0;  
	BOOL succeeded = ::VirtualProtect(reinterpret_cast<void*>(target_function),  
		32,  
		PAGE_EXECUTE_READWRITE,  
		&old_target_function_protect);  

	if (!succeeded) {  
	    FASSERT(0);
		return false;  
	}  

	// save
	memcpy(old_mem, target_function, JMP_CODE_LEN);

	// hook
	*target_function = 0xE9;  
	int offset = (uint8_t *)newfuc - target_function - JMP_CODE_LEN;  
	memcpy(target_function + 1, &offset, JMP_CODE_LEN - 1);  

	succeeded = ::VirtualProtect(reinterpret_cast<void*>(target_function),  
		32,  
		old_target_function_protect,  
		&old_target_function_protect);  

	if (!succeeded) {  
	    FASSERT(0);
		return false;  
	}  

	return true;
}

static force_inline bool frestore_func(uint8_t * target_function, uint8_t (&old_mem)[JMP_CODE_LEN])
{
	DWORD old_target_function_protect = 0;  
	BOOL succeeded = ::VirtualProtect(reinterpret_cast<void*>(target_function),  
		32,  
		PAGE_EXECUTE_READWRITE,  
		&old_target_function_protect);  

	if (!succeeded) {  
		return false;  
	}  

	// save
	memcpy(target_function, old_mem, JMP_CODE_LEN);

	succeeded = ::VirtualProtect(reinterpret_cast<void*>(target_function),  
		32,  
		old_target_function_protect,  
		&old_target_function_protect);  

	if (!succeeded) {  
		return false;  
	}  

	return true;
}

#else

static force_inline bool fhook_func(uint8_t * target_function, uint8_t * newfuc, uint8_t (&old_mem)[JMP_CODE_LEN])
{
	uint8_t machine_code[JMP_CODE_LEN] = {
			//movq $0x0, %rax 后面8个字节的0为64位立即数
			0x48, 0xb8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
			//jmpq *%rax
			0xff, 0xe0
		};

	int pagesize = getpagesize();

	uint8_t *mem = (uint8_t *) target_function;
	void *p = (uint8_t*) (mem - ((uint64_t) mem % pagesize));
	if (mprotect(p, pagesize, PROT_READ | PROT_WRITE | PROT_EXEC))
	{
		perror("mprotect error");
	    FASSERT(0);
		return false;
	}

	// save
	memcpy(old_mem, mem, JMP_CODE_LEN);
	
	//改写立即数为mock的地址,写入函数入口处
	memcpy(machine_code + 2, &newfuc, sizeof(newfuc));
	memcpy(mem, machine_code, sizeof(machine_code));

	if (mprotect(p, pagesize, PROT_EXEC))
	{
		perror("mprotect error");
	    FASSERT(0);
		return false;
	}

	return true;
}

static force_inline bool frestore_func(uint8_t * target_function, uint8_t (&old_mem)[JMP_CODE_LEN])
{
	int pagesize = getpagesize();

	uint8_t *mem = (uint8_t *) target_function;
	void *p = (uint8_t*) (mem - ((uint64_t) mem % pagesize));
	if (mprotect(p, pagesize, PROT_READ | PROT_WRITE | PROT_EXEC))
	{
		perror("mprotect error");
	    FASSERT(0);
		return false;
	}

	memcpy(mem, old_mem, JMP_CODE_LEN);

	if (mprotect(p, pagesize, PROT_EXEC))
	{
		perror("mprotect error");
	    FASSERT(0);
		return false;
	}

	return true;
}

#endif

/////////////////////////////////////////////////////////////

/** @brief Add a value to a variable, atomically.
*
*  Implementation is heavily platform-dependent.
*  @param ptr Pointer to a 32-bit signed integer.
*  @param addend Value to add.
*/
static force_inline int32_t ffetch_and_add_32(volatile int32_t * ptr, int32_t addend)
{
#if defined(WIN32)
	return _InterlockedExchangeAdd(reinterpret_cast<volatile long*>(ptr), addend);
#else
	return __sync_fetch_and_add(ptr, addend);
#endif
}

/** @brief Add a value to a variable, atomically.
*
*  Implementation is heavily platform-dependent.
*  @param ptr Pointer to a 64-bit signed integer.
*  @param addend Value to add.
*/
static force_inline int64_t ffetch_and_add_64(volatile int64_t * ptr, int64_t addend)
{
#if defined(WIN32)
	return _InterlockedExchangeAdd64(ptr, addend);
#else
	return __sync_fetch_and_add(ptr, addend);
#endif
}

/** @brief Add a value to a variable, atomically.
*
*  Implementation is heavily platform-dependent.
*  @param ptr Pointer to a signed integer.
*  @param addend Value to add.
*/
template<typename T>
force_inline T ffetch_and_add(volatile T* ptr, T addend)
{
	if (sizeof(T) == sizeof(int32_t))
	{
		return (T)ffetch_and_add_32((volatile int32_t*) ptr, (int32_t)addend);
	}
	else if (sizeof(T) == sizeof(int64_t))
	{
		return (T)ffetch_and_add_64((volatile int64_t*) ptr, (int64_t)addend);
	}
	else
	{
		FASSERT(0);
		return 0;
	}
}

/** @brief Compare @c *ptr and @c comparand. If equal, let @c
* *ptr=replacement and return @c true, return @c false otherwise.
*
*  Implementation is heavily platform-dependent.
*  @param ptr Pointer to 32-bit signed integer.
*  @param comparand Compare value.
*  @param replacement Replacement value.
*/
static force_inline bool fcompare_and_swap_32(volatile int32_t * ptr, int32_t comparand, int32_t replacement)
{
#if defined(WIN32)
	return _InterlockedCompareExchange(reinterpret_cast<volatile long*>(ptr), 
		replacement, comparand) == comparand;
#else
	return __sync_bool_compare_and_swap(ptr, comparand, replacement);
#endif
}

/** @brief Compare @c *ptr and @c comparand. If equal, let @c
* *ptr=replacement and return @c true, return @c false otherwise.
*
*  Implementation is heavily platform-dependent.
*  @param ptr Pointer to 64-bit signed integer.
*  @param comparand Compare value.
*  @param replacement Replacement value.
*/
static force_inline bool fcompare_and_swap_64(volatile int64_t * ptr, int64_t comparand, int64_t replacement)
{
#if defined(WIN32)
	return _InterlockedCompareExchange64(ptr, replacement,
		comparand) == comparand;
#else
	return __sync_bool_compare_and_swap(ptr, comparand, replacement);
#endif
}

/** @brief Compare @c *ptr and @c comparand. If equal, let @c
* *ptr=replacement and return @c true, return @c false otherwise.
*
*  Implementation is heavily platform-dependent.
*  @param ptr Pointer to signed integer.
*  @param comparand Compare value.
*  @param replacement Replacement value. */
template<typename T>
force_inline bool fcompare_and_swap(volatile T* ptr, T comparand, T replacement)
{
	if (sizeof(T) == sizeof(int32_t))
	{
		return fcompare_and_swap_32((volatile int32_t*) ptr, (int32_t)comparand, (int32_t)replacement);
	}
	else if (sizeof(T) == sizeof(int64_t))
	{
		return fcompare_and_swap_64((volatile int64_t*) ptr, (int64_t)comparand, (int64_t)replacement);
	}
	else
	{
		FASSERT(0);
		return false;
	}
}

/** @brief Yield the control to another thread, without waiting for
  the end to the time slice. */
static force_inline void fyield()
{
#if defined(WIN32)
	Sleep(0);
#else
	sched_yield();
#endif
}

/////////////////////////////////////////////////////////////

#if defined(WIN32)
template<class _Ty>
struct fallocator_base
{	// base class for generic allocators
	typedef _Ty value_type;
};
template<class _Ty, class real_alloc>
class fallocator : public fallocator_base<_Ty>
{	// generic fallocator for objects of class _Ty
public:
	typedef fallocator_base<_Ty> _Mybase;
	typedef typename _Mybase::value_type value_type;

	typedef value_type *pointer;
	typedef const value_type *const_pointer;
	typedef void *void_pointer;
	typedef const void *const_void_pointer;

	typedef value_type& reference;
	typedef const value_type& const_reference;

	typedef size_t size_type;
	typedef ptrdiff_t difference_type;

	template<class _Other>
	struct rebind
	{	// convert an fallocator<_Ty> to an fallocator <_Other>
		typedef fallocator<_Other, typename real_alloc::template rebind<_Other>::other> other;
	};

	pointer address(reference _Val) const
	{	// return address of mutable _Val
		return (&_Val);
	}

	const_pointer address(const_reference _Val) const
	{	// return address of nonmutable _Val
		return (&_Val);
	}

	fallocator() 
	{	// construct default fallocator (do nothing)
	}

	fallocator(const fallocator<_Ty, real_alloc>&) 
	{	// construct by copying (do nothing)
	}

	template<class _Other, class RealAllocOther>
	fallocator(const fallocator<_Other, RealAllocOther>&) 
	{	// construct from a related fallocator (do nothing)
	}

	template<class _Other, class RealAllocOther>
	fallocator<_Ty, real_alloc>& operator=(const fallocator<_Other, RealAllocOther>&)
	{	// assign from a related fallocator (do nothing)
		return (*this);
	}

	void deallocate(pointer _Ptr, size_type)
	{	// deallocate object at _Ptr, ignore size
		real_alloc::ptr()->deallocate(_Ptr);
	}

	pointer allocate(size_type _Count)
	{	// allocate array of _Count elements
		return real_alloc::ptr()->allocate(_Count);
	}

	pointer allocate(size_type _Count, const void  *)
	{	// allocate array of _Count elements, ignore hint
		return (allocate(_Count));
	}

	void construct(pointer _Ptr, const _Ty& _Val)
	{	// construct object at _Ptr with value _Val
		std::_Construct(_Ptr, _Val);
	}

	void destroy(pointer _Ptr)
	{	// destroy object at _Ptr
		std::_Destroy(_Ptr);
	}

	size_t max_size() const
	{	// estimate maximum array size
		size_t _Count = (size_t)(-1) / sizeof (_Ty);
		return (0 < _Count ? _Count : 1);
	}
};
template<class _Ty,
class _Other, typename real_alloc> inline
	bool operator==(const fallocator<_Ty, real_alloc>&, const fallocator<_Other, real_alloc>&) _THROW0()
{	// test for allocator equality (always true)
	return (true);
}

template<class _Ty,
class _Other, typename real_alloc> inline
	bool operator!=(const fallocator<_Ty, real_alloc>&, const fallocator<_Other, real_alloc>&) _THROW0()
{	// test for allocator inequality (always false)
	return (false);
}

#else

template<typename _Tp, typename real_alloc>
class fallocator
{
public:
	typedef size_t     size_type;
	typedef ptrdiff_t  difference_type;
	typedef _Tp*       pointer;
	typedef const _Tp* const_pointer;
	typedef _Tp&       reference;
	typedef const _Tp& const_reference;
	typedef _Tp        value_type;

	template<typename _Tp1>
	struct rebind
	{ typedef fallocator<_Tp1, typename real_alloc::template rebind<_Tp1>::other> other; };

	fallocator() throw() { }

	fallocator(const fallocator&) throw() { }

	template<typename _Tp1, class RealAllocOther>
	fallocator(const fallocator<_Tp1, RealAllocOther>&) throw() { }

	~fallocator() throw() { }

	pointer
		address(reference __x) const { return &__x; }

	const_pointer
		address(const_reference __x) const { return &__x; }

	// NB: __n is permitted to be 0.  The C++ standard says nothing
	// about what the return value is when __n == 0.
	pointer
		allocate(size_type __n, const void* = 0)
	{
		return real_alloc::ptr()->allocate(__n);
	}

	// __p is not permitted to be a null pointer.
	void
		deallocate(pointer __p, size_type)
	{ real_alloc::ptr()->deallocate(__p); }

	size_type
		max_size() const throw() 
	{ return size_t(-1) / sizeof(_Tp); }

	// _GLIBCXX_RESOLVE_LIB_DEFECTS
	// 402. wrong new expression in [some_] allocator::construct
	void 
		construct(pointer __p, const _Tp& __val) 
	{ ::new(__p) _Tp(__val); }

	void 
		destroy(pointer __p) { __p->~_Tp(); }
};

template<typename _Tp, typename real_alloc>
inline bool
operator==(const fallocator<_Tp, real_alloc>&, const fallocator<_Tp, real_alloc>&)
{ return true; }

template<typename _Tp, typename real_alloc>
inline bool
operator!=(const fallocator<_Tp, real_alloc>&, const fallocator<_Tp, real_alloc>&)
{ return false; }

#endif

/////////////////////////////////////////////////////////////

class fclock : public singleton< fclock >
{
public:
	fclock() : m_last(0)
	{
		tick();
	}
	~fclock()
	{
	}
	force_inline time_t now()
	{
		return m_now;
	}
	force_inline tm nowtm()
	{
		return m_nowtm;
	}
	force_inline const int8_t * nowstr()
	{
		return (const int8_t *)m_nowstr.c_str();
	}
	force_inline const int8_t * nowdatestr()
	{
		return (const int8_t *)m_nowdatestr.c_str();
	}
	force_inline void heartbeat()
	{
		tick();
	}
	static force_inline uint32_t Cycles()
	{
#ifdef WIN32        
		LARGE_INTEGER Cycles;
		QueryPerformanceCounter(&Cycles);
		return (uint32_t)Cycles.QuadPart;
#else
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return static_cast<uint32_t>(static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL);
#endif
	}
private:
	force_inline void tick()
	{
		time(&m_now);
		if (m_last == m_now)
		{
		    return;
		}
		m_last = m_now;
		m_nowtm = *gmtime(&m_now);

		char buff[100] = {0};
		tsnprintf(buff, sizeof(buff) - 1, "%d-%d-%d %d:%d:%d", m_nowtm.tm_year + 1900, m_nowtm.tm_mon + 1,
			m_nowtm.tm_mday,
			m_nowtm.tm_hour,
			m_nowtm.tm_min,
			m_nowtm.tm_sec);
		m_nowstr = buff;

		tsnprintf(buff, sizeof(buff) - 1, "%d-%d-%d",
			m_nowtm.tm_year + 1900,
			m_nowtm.tm_mon + 1,
			m_nowtm.tm_mday);
		m_nowdatestr = buff;
	}
private:
	time_t m_now;
	time_t m_last;
	tm m_nowtm;
	std::string m_nowstr;
	std::string m_nowdatestr;
};

/////////////////////////////////////////////////////////////

template<typename T>
class normal_allocator : public singleton< normal_allocator<T> >
{
public:
	normal_allocator() {}
	template<class _Other>
	struct rebind
	{
		typedef normal_allocator<_Other> other;
	};
	force_inline T * allocate(size_t count)
	{
		return (T *)(malloc(count * sizeof(T)));
	}
	force_inline void deallocate(T * p)
	{
		free(p);
	}
};

/////////////////////////////////////////////////////////////

#if defined(WIN32)
class fthread_lock
{
	CRITICAL_SECTION m_lock ;
public :
	fthread_lock() { InitializeCriticalSection(&m_lock); }
	~fthread_lock() { DeleteCriticalSection(&m_lock); }
	force_inline void lock() { EnterCriticalSection(&m_lock); }
	force_inline void unlock() { LeaveCriticalSection(&m_lock); }
};
#else
class fthread_lock
{
	pthread_mutex_t m_mutex; 
public :
	fthread_lock() { pthread_mutex_init( &m_mutex , NULL); }
	~fthread_lock() { pthread_mutex_destroy( &m_mutex); }
	force_inline void lock() { pthread_mutex_lock(&m_mutex); }
	force_inline void unlock() { pthread_mutex_unlock(&m_mutex); }
};
#endif

/////////////////////////////////////////////////////////////

static force_inline bool ftls_alloc(tls_key & key)
{
#ifdef WIN32
	key = ::TlsAlloc();
	return key != TLS_OUT_OF_INDEXES;
#else
	return pthread_key_create(&key, 0) == 0;
#endif
}

static force_inline void ftls_free(tls_key key)
{
#ifdef WIN32
	::TlsFree(key);
#else
	pthread_key_delete(key);
#endif
}

static force_inline void ftls_set(tls_key key, const void * data)
{
#ifdef WIN32
	::TlsSetValue(key, (LPVOID)data);
#else
	pthread_setspecific(key, data);
#endif
}

static force_inline void * ftls_get(tls_key key)
{
#ifdef WIN32
	return ::TlsGetValue(key);
#else
	return pthread_getspecific(key);
#endif
}

/////////////////////////////////////////////////////////////

template <typename T>
class fauto_lock
{
public:
	fauto_lock(T & rLock) : m_lock(rLock)
	{
		m_lock.lock();
	}
	~fauto_lock()
	{
		m_lock.unlock();
	}
private:
	fauto_lock() {}
private:
	T & m_lock;
};

/////////////////////////////////////////////////////////////

static force_inline void fsleep(int32_t millionseconds)
{
#if defined(WIN32)
	Sleep(millionseconds);
#else
	usleep(millionseconds * 1000);
#endif
}

static force_inline t_id fget_current_thread_id()
{
#if defined(WIN32)
	return GetCurrentThreadId();
#else
	return pthread_self();
#endif
}

/////////////////////////////////////////////////////////////

class flog_system : public singleton< flog_system >
{
public:
	flog_system()
	{
		memset(m_buff, 0, sizeof(m_buff));
		memset(m_switch, 1, sizeof(m_switch));
		memset(m_printswitch, 1, sizeof(m_printswitch));
	}
	~flog_system()
	{
	}
	force_inline void setname(const std::string & name)
	{
		m_name = name;
	}
	force_inline void enable(FLOG_TYPE type)
	{
		SAFE_TEST_INDEX(type, FLOGT_MAX);
		m_switch[type] = 1;
	}
	force_inline void disable(FLOG_TYPE type)
	{
		SAFE_TEST_INDEX(type, FLOGT_MAX);
		m_switch[type] = 0;
	}
	force_inline void enableprint(FLOG_TYPE type)
	{
		SAFE_TEST_INDEX(type, FLOGT_MAX);
		m_printswitch[type] = 1;
	}
	force_inline void disableprint(FLOG_TYPE type)
	{
		SAFE_TEST_INDEX(type, FLOGT_MAX);
		m_printswitch[type] = 0;
	}
	void write(uint32_t type, const char * file, int pos, const char * funcname, const char * format, ...)
	{
		if (!m_switch[type])
		{
			return;
		}

#ifdef WIN32
		const char * filename = strrchr(file, '\\');
#else
		const char * filename = strrchr(file, '/');
#endif
		if (filename)
		{
			filename++;
		}
		else
		{
			filename = file;
		}

		int32_t ret = fsnprintf(m_buff, ARRAY_SIZE(m_buff),
			"%s %s:%d %s %s : ",
			gettypename(type), filename, pos, funcname, (const char*)fclock::ptr()->nowstr());

		va_list ap;
		va_start(ap, format);
		ret += fvsnprintf(m_buff + ret, ARRAY_SIZE(m_buff) - ret, format, ap);
		va_end(ap);

		ret += fsnprintf(m_buff + ret, ARRAY_SIZE(m_buff) - ret, "\n");

		m_buff[ARRAY_SIZE(m_buff) - 1] = 0;

		if (m_printswitch[type])
		{
			fshow_color_text((const char *)m_buff, c_log_print_color[type]);
		}

		char tmp[100] = { 0 };
		tsnprintf(tmp, sizeof(tmp) - 1, "%s_%s_%s.log", gettypename(type), (const char *)m_name.c_str(), (const char*)fclock::ptr()->nowdatestr());
		FILE * fp = fopen(tmp, "a");
		if (!fp)
		{
			return;
		}

		fwrite(m_buff, ret, 1, fp);
		fclose(fp);
	}

private:
	force_inline const char * gettypename(uint32_t type)
	{
		switch (type)
		{
		case FLOGT_DEBUG:
			return "debug";
		case FLOGT_SYSTEM:
			return "system";
		case FLOGT_ERROR:
			return "error";
		case FLOGT_INFO:
			return "info";
		default:
			return "unknow";
		}
	}
private:
	int8_t m_buff[c_LogBuffer];
	int8_t m_switch[FLOGT_MAX];
	int8_t m_printswitch[FLOGT_MAX];
	std::string m_name;
};

/////////////////////////////////////////////////////////////

class readonly_buffer : public singleton<readonly_buffer>
{
public:
	readonly_buffer() : pbuffer(0), totalsize(0), nowpos(0) {}
	void ini(int8_t * _pBuffer, size_t _totalsize)
	{
		pbuffer = _pBuffer;
		totalsize = _totalsize;
		nowpos = 0;
	}
	force_inline int8_t * allocate(size_t allocsize)
	{
		nowpos += allocsize;
		FASSERT(nowpos <= totalsize);
		return pbuffer + nowpos - allocsize;
	}
	force_inline void deallocate(int8_t * p)
	{
		FASSERT(0);
		LOG_ERROR("readonly buffer deallocate");
	}
	int8_t * pbuffer;
	size_t totalsize;
	size_t nowpos;
};

template<typename T>
class readonly_allocator : public singleton< readonly_allocator<T> >
{
public:
	readonly_allocator() {}
	template<class _Other>
	struct rebind
	{
		typedef readonly_allocator<_Other> other;
	};
	force_inline T * allocate(size_t count)
	{
		return (T *)(readonly_buffer::ptr()->allocate(count * sizeof(T)));
	}
	force_inline void deallocate(T * p)
	{
		FASSERT(0);
		LOG_ERROR("readonly allocator deallocate");
	}
};

/////////////////////////////////////////////////////////////

class fthread 
{
public:
	enum thread_status 
	{
		ts_ready,		// 当前线程处于准备状态
		ts_running,		// 处于运行状态
		ts_exiting,		// 线程正在退出
		ts_exit			// 已经退出 
	};
	fthread() : m_id(0), m_status(fthread::ts_ready)
#if defined(WIN32)
		,m_hthread(0)
#endif
	{
	}
	virtual ~fthread()
	{
	}
public:
	force_inline void start()
	{
		FASSERT(m_status == fthread::ts_ready);

#if defined(WIN32)
		m_hthread = ::CreateThread(NULL, 0, fthread::my_thread_process, this, 0, &m_id);
#else
		pthread_create(&m_id, NULL, thread::my_thread_process, this);
#endif
	}

	force_inline void exit(void * retval = NULL )
	{
#if defined(WIN32)
		::CloseHandle(m_hthread);
#else
		pthread_exit(retval);
#endif
	}

	virtual void run() {}
	force_inline thread_status get_status() { return m_status; }
	force_inline void set_status(thread_status status) { m_status = status; }
	force_inline t_id get_id () { return m_id; }
	force_inline void join()
	{
		while (m_status != fthread::ts_exit)
		{
			fsleep(1);
		}
	}

#if defined(WIN32)
	static DWORD WINAPI my_thread_process(void * derivedThread)
#else
	static void * my_thread_process(void * derivedThread)
#endif
	{
#ifndef WIN32
		pthread_detach(pthread_self());
#endif
		fthread * t = (fthread*)derivedThread;
		FASSERT(t);

		t->set_status(fthread::ts_running);

		t->run();

		t->set_status(fthread::ts_exit);

		t->exit(0);

		return 0;	// avoid compiler's warning
	}
private:
	// thread identifier variable
	t_id m_id;
	
	// thread status
	thread_status m_status;

#if defined(WIN32)
	HANDLE m_hthread ;
#endif
};

/////////////////////////////////////////////////////////////

/*

[head][key-value node]

head:[magic][node offset][md5][sha1][name string]

node:[key string][value blob]

*/

template <size_t N>
class fkeybuffer
{
public:
	force_inline fkeybuffer()
	{
		clear();
	}
	force_inline ~fkeybuffer()
	{
		clear();
	}

	force_inline void clear()
	{
		memset(m_databuffer, 0, sizeof(m_databuffer));
		m_dataiter = 0;
		m_datasize = 0;
	}

	force_inline void reset()
	{
		m_dataiter = 0;
	}

	force_inline const uint8_t * buff() const
	{
		return m_databuffer;
	}

	force_inline uint8_t * buff()
	{
		return m_databuffer;
	}

	force_inline void setsize(size_t size)
	{
		m_datasize = size;
	}

	force_inline size_t size() const
	{
		return m_datasize;
	}

	force_inline size_t maxsize() const
	{
		return N;
	}

	#pragma pack(1)
	struct Head
	{
		uint32_t size;
		uint32_t nodeoffset;
		/* name string */
	};
	struct Node
	{
		uint32_t size;
		uint32_t nextoffset;
		/* key string */
		/* value blob */
	};
	#pragma pack()

	force_inline bool begin(const std::string & name)
	{
		clear();

		Head head;
		SAFE_TEST_RET_VAL(write_head(head), false, false);
		SAFE_TEST_RET_VAL(write_string(name), false, false);
		Head * phead = gethead();
		phead->nodeoffset = m_dataiter;
		return true;
	}

	template <typename T>
	force_inline bool add(const std::string & name, const T & data)
	{
		return add(name, &data, sizeof(data));
	}
	template <size_t M>
	force_inline bool add(const std::string & name, const fkeybuffer<M> & data)
	{
		return add(name, data.buff(), data.size());
	}
	force_inline bool add(const std::string & name, const std::string & data)
	{
		return add(name, data.c_str(), data.size());
	}
	force_inline bool add(const std::string & name, const void * data, size_t size)
	{
		Node * pnode = getcur();
		size_t old = m_dataiter;
		Node node;
		SAFE_TEST_RET_VAL(write_node(node), false, false);
		SAFE_TEST_RET_VAL(write_string(name), false, false);
		SAFE_TEST_RET_VAL(write((uint32_t)size), false, false);
		SAFE_TEST_RET_VAL(write_buffer((const uint8_t *)data, size), false, false);
		pnode->nextoffset = m_dataiter;
		pnode->size = m_dataiter - old;
		return true;
	}

	force_inline bool end()
	{
		Head * phead = gethead();
		phead->size = m_dataiter;
		m_datasize = m_dataiter;
		reset();
		return true;
	}

	template <typename T>
	force_inline bool get(const std::string & name, T & data) const
	{
		return get(name, &data, sizeof(data));
	}
	template <size_t M>
	force_inline bool get(const std::string & name, fkeybuffer<M> & data) const
	{
		size_t retsize = 0;
		SAFE_TEST_RET_VAL(get(name, data.buff(), data.maxsize(), &retsize), false, false);
		data.setsize(retsize);
		return true;
	}
	force_inline bool get(const std::string & name, std::string & data) const
	{
		uint8_t strbuff[256];
		memset(strbuff, 0, sizeof(strbuff));
		SAFE_TEST_RET_VAL(get(name, strbuff, 256), false, false);
		data = strbuff;
		return true;
	}
	force_inline bool get(const std::string & name, void * data, size_t size, size_t * retsize = NULL) const
	{
		const Head * phead = gethead();
		m_dataiter = phead->nodeoffset;
		while (m_dataiter < m_datasize)
		{
			const Node * pnode = getcur();
			Node node;
			std::string nodename;
			uint32_t valuesize;
			SAFE_TEST_RET_VAL(read_node(node), false, false);
			SAFE_TEST_RET_VAL(read_string(nodename), false, false);

			if (nodename != name)
			{
				m_dataiter = pnode->nextoffset;
				continue;
			}

			SAFE_TEST_RET_VAL(read(valuesize), false, false);
			SAFE_TEST_RET_VAL(valuesize <= size, false, false);
			SAFE_TEST_RET_VAL(read_buffer((uint8_t *)data, valuesize), false, false);

			if (retsize)
			{
				*retsize = valuesize;
			}

			return true;
		}

		return false;
	}

private:
	template <typename T>
	force_inline bool write(const T & data)
	{
		return write_buffer((const uint8_t *)&data, sizeof(data));
	}
	template <typename T>
	force_inline bool read(T & data) const
	{
		return read_buffer((uint8_t *)&data, sizeof(data));
	}
	force_inline bool write_buffer(const uint8_t * data, size_t size)
	{
		if (m_dataiter + size > N)
		{
			return false;
		}
		memcpy(m_databuffer + m_dataiter, data, size);
		m_dataiter += size;
		return true;
	}
	force_inline bool read_buffer(uint8_t * data, size_t size) const
	{
		if (m_dataiter + size > m_datasize)
		{
			return false;
		}
		memcpy(data, m_databuffer + m_dataiter, size);
		m_dataiter += size;
		return true;
	}
	force_inline bool write_head(const Head & head)
	{
		return write(head);
	}
	force_inline bool write_node(const Node & node)
	{
		return write(node);
	}
	force_inline bool read_node(Node & node) const
	{
		return read(node);
	}
	force_inline bool write_string(const std::string & str)
	{
		uint16_t strsize = str.size();
		SAFE_TEST_RET_VAL(write(strsize), false, false);
		SAFE_TEST_RET_VAL(write_buffer(str.c_str(), strsize), false, false);
		return true;
	}
	force_inline bool read_string(std::string & str) const
	{
		uint16_t strsize = 0;
		SAFE_TEST_RET_VAL(read(strsize), false, false);
		SAFE_TEST_RET_VAL(strsize <= 256, false, false);
		uint8_t strbuff[256];
		memset(strbuff, 0, sizeof(strbuff));
		SAFE_TEST_RET_VAL(read_buffer(strbuff, strsize), false, false);
		str = strbuff;
		return true;
	}
	force_inline Head * gethead()
	{
		return &m_head;
	}
	force_inline const Head * gethead() const
	{
		return &m_head;
	}
	force_inline Node * getcur()
	{
		return (Node *)(m_databuffer + m_dataiter); 
	}
	force_inline const Node * getcur() const
	{
		return (const Node *)(m_databuffer + m_dataiter);
	}
private:
	union
	{
		uint8_t m_databuffer[N];
		Head m_head;
	};
	mutable size_t m_dataiter;
	size_t m_datasize;
};

/////////////////////////////////////////////////////////////


// 创建ShareMem 内存区
static force_inline shm_handle create_share_mem(shm_key key,size_t size)
{
#if defined(WIN32)
	char keybuf[64] = {0};
	fsnprintf(keybuf, sizeof(keybuf), "%d", key);
	return CreateFileMapping( (HANDLE)0xFFFFFFFFFFFFFFFF, NULL, PAGE_READWRITE, 0, size, keybuf);
	
#else
	return shmget(key, size, IPC_CREAT|IPC_EXCL|0666);

#endif

	return shm_handle(-1);
}

// 打开ShareMem 内存区
static force_inline shm_handle open_share_mem(shm_key key,size_t size)
{
#if defined(WIN32)
	char keybuf[64] = {0};
	fsnprintf(keybuf, sizeof(keybuf), "%d", key);
	return OpenFileMapping( FILE_MAP_ALL_ACCESS, TRUE, keybuf);

#else
	return shmget(key, size, 0);

#endif

	return shm_handle(-1);
}
	
// 映射ShareMem 内存区
static force_inline int8_t* map_share_mem(shm_handle handle)
{
#if defined(WIN32)
	return (int8_t *)MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0);

#else
	return (int8_t*)shmat(handle, 0, 0);

#endif

	return 0;
}
	
// 关闭映射 ShareMem 内存区
static force_inline void unmap_share_mem(int8_t* memoryptr)
{
#if defined(WIN32)
	 UnmapViewOfFile(memoryptr);

#else
	shmdt(memoryptr);

#endif
}
	
// 	关闭ShareMem
static force_inline void close_share_mem(shm_handle handle)
{
#if defined(WIN32)
	CloseHandle(handle);

#else
	shmctl(handle, IPC_RMID, 0); 

#endif
}

/////////////////////////////////////////////////////////////

class fcmdparser : public singleton<fcmdparser>
{
public:
	fcmdparser()
	{
	}
	~fcmdparser()
	{
	}
	force_inline int argc() { return m_argc; }
	force_inline char ** argv() { return m_argv; }
	force_inline bool parse(int argc, char *argv[])
	{
		m_argc = argc;
		m_argv = argv;
		for (int i = 0; i < argc; i++)
		{
			parse_arg(argv[i]);
		}
		return true;
	}
	force_inline bool get(const std::string & key, int32_t & value)
	{
		std::string tmp;
		if (get(key, tmp))
		{
			value = fatoi(tmp);
			return true;
		}
		return false;
	}
	force_inline bool get(const std::string & key, std::string & value)
	{
		_cmdMapIter it = m_map.find(key);
		if (it != m_map.end())
		{
			value = it->second;
			return true;
		}
		return false;
	}
private:
	force_inline bool parse_arg(std::string str)
	{
		int32_t pos = str.find_first_not_of("-");
		SAFE_TEST_RET_VAL(pos, -1, false);

		str = str.substr(pos);
		pos = str.find('=');
		SAFE_TEST_RET_VAL(pos, -1, false);

		std::string key = str.substr(0, pos);
		std::string value = str.substr(pos + 1);
		
		SAFE_TEST_RET_VAL(key.empty(), true, false);
		SAFE_TEST_RET_VAL(value.empty(), true, false);

		m_map[key] = value;

		return true;
	}
private:
    typedef std::map<std::string, std::string> _cmdMap;
    typedef _cmdMap::iterator _cmdMapIter;
	_cmdMap m_map;
	int m_argc;
	char ** m_argv;
};

/////////////////////////////////////////////////////////////

template <typename Base>
class ifactory
{
public:
	virtual Base * alloc() = 0;
	virtual bool dealloc(Base * pBase) = 0;
};

template <typename T, typename Base, typename Alloctor=fallocator<T, normal_allocator<T> > >	
class factory : public ifactory<Base>
{
public:
	virtual Base * alloc()
	{
		Base * pBase = m_alloctor.allocate(1);
		SAFE_TEST_RET_VAL(pBase, NULL, NULL);
		new (pBase) T ();
		return pBase;
	}
	virtual bool dealloc(Base * pBase)
	{
		SAFE_TEST_RET_VAL(pBase, NULL, NULL);
		((T*)pBase)->~T();
		m_alloctor.deallocate((T*)pBase, 1);
		return true;
	}
private:
	Alloctor m_alloctor;
};

template <typename Base, uint32_t N, typename Key=uint32_t>
class factorymng : public singleton< factorymng<Base, N, Key> >
{
public:
	typedef ifactory<Base> IFactory;
	typedef fhashmap<Key, IFactory*, N> FactoryMap;
public:
	force_inline bool regist(Key type, IFactory * pfactory)
	{
		m_map[type] = pfactory;
		return true;
	}
	force_inline Base * alloc(Key type)
	{
		IFactory * pfactory = m_map[type];
		SAFE_TEST_RET_VAL(pfactory, NULL, NULL);
		return pfactory->alloc();
	}
	force_inline bool dealloc(Base * pBase)
	{
		Key type = pBase->gettype();
		IFactory * pfactory = m_map[type];
		SAFE_TEST_RET_VAL(pfactory, NULL, false);
		return pfactory->dealloc(pBase);
	}
private:
	FactoryMap m_map;
};

/////////////////////////////////////////////////////////////

static force_inline bool fis_file_exist(const std::string & file)
{
#if defined(WIN32)
	return ::PathFileExists((const char *)file.c_str()) == TRUE;
#else
	return access((const char *)file.c_str(), F_OK) == 0;
#endif
}

static force_inline bool fdelete_file(const std::string & file)
{
#if defined(WIN32)
	return ::DeleteFile((const char *)file.c_str()) == TRUE;
#else
	return unlink((const char *)file.c_str()) == 0;
#endif
}

/////////////////////////////////////////////////////////////

/************************************************************************/
/* 

start_trace
while
	if wait_syscall_begin

		get_syscall_begin

		wait_syscall_end

		get_syscall_end
	end

	continue_trace
end
stop_trace
                                                                     */
/************************************************************************/

#ifndef WIN32

class fptrace
{
public:
	fptrace()
	{
	}
	~fptrace()
	{
	}

	force_inline void set_traced_process(pid_t traced_process)
	{
		m_traced_process = traced_process;
	}

	force_inline bool start_trace()
	{
		int32_t status = 0;
		SAFE_DIFFER_TEST_RET_VAL(ptrace(PTRACE_ATTACH, m_traced_process, NULL, NULL), 0, false);
		wait(&status);
		SAFE_DIFFER_TEST_RET_VAL(ptrace(PTRACE_SETOPTIONS, m_traced_process, NULL, PTRACE_O_TRACESYSGOOD), 0, false);
		SAFE_DIFFER_TEST_RET_VAL(ptrace(PTRACE_SYSCALL, m_traced_process, NULL, NULL), 0, false);
		return true;
	}

	force_inline bool wait_syscall_begin()
	{
		int32_t status = 0;

		wait(&status);

		if (WSTOPSIG(status) != (SIGTRAP | 0x80))
		{
			return false;
		}

		return true;
	}

	force_inline bool get_syscall_begin(uint64_t call, uint64_t p1, uint64_t p2, uint64_t p3)
	{
		struct user_regs_struct regs;
		// We are now entering a system call
		SAFE_DIFFER_TEST_RET_VAL(ptrace(PTRACE_GETREGS, m_traced_process, NULL, &regs), 0, false);
		call = regs.orig_rax;
		p1 = regs.rdi;
		p2 = regs.rsi;
		p3 = regs.rdx;
		return true;
	}

	force_inline bool wait_syscall_end()
	{
		int32_t status = 0;
		// Wait until we're exiting the system call
		SAFE_DIFFER_TEST_RET_VAL(ptrace(PTRACE_SYSCALL, m_traced_process, NULL, NULL), 0, false);
		wait(&status);

		return true;
	}

	force_inline bool get_syscall_end(uint64_t ret)
	{
		struct user_regs_struct regs;
		/* Syscall exit */
		SAFE_DIFFER_TEST_RET_VAL(ptrace(PTRACE_GETREGS, m_traced_process, NULL, &regs), 0, false);

		ret = regs.rax;

		return true;
	}

	force_inline bool continue_trace()
	{
		SAFE_DIFFER_TEST_RET_VAL(ptrace(PTRACE_SYSCALL, m_traced_process, NULL, NULL), 0, false);
		return true;
	}

	force_inline bool stop_trace()
	{
		SAFE_DIFFER_TEST_RET_VAL(ptrace(PTRACE_DETACH, m_traced_process, NULL, NULL), 0, false);
		return true;
	}

private:
	pid_t m_traced_process;
};

#endif

/////////////////////////////////////////////////////////////

static force_inline void finit_signal(void(*func)(int))
{
	signal(SIGFPE, SIG_IGN);

#ifdef _WIN32
	signal(SIGINT, func);
#else
	signal(SIGINT, SIG_IGN);
#endif
	signal(SIGTERM, SIG_IGN);

	signal(SIGILL, func);
	//signal(SIGSEGV, func);
	signal(SIGABRT, func);

#ifndef _WIN32
	signal(SIGBUS, func);
	signal(SIGUSR1, func);
	signal(SIGUSR2, func);
	signal(SIGHUP, func);

	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
#else
	signal(SIGBREAK, func);
#endif
}













































