#ifndef TYPES_H
#define TYPES_H

typedef unsigned char			u8;	/* unsigned  8-bit */
typedef unsigned short			u16;	/* unsigned 16-bit */
typedef unsigned int			u32;	/* unsigned 32-bit */
typedef unsigned long long		u64;	/* unsigned 64-bit */

typedef signed char			s8;	/* signed  8-bit */
typedef short				s16;	/* signed 16-bit */
typedef int				s32;	/* signed 32-bit */
typedef long long			s64;	/* signed 64-bit */

typedef volatile unsigned char		vu8;	/* unsigned  8-bit */
typedef volatile unsigned short		vu16;	/* unsigned 16-bit */
typedef volatile unsigned int		vu32;	/* unsigned 32-bit */
typedef volatile unsigned long long	vu64;	/* unsigned 64-bit */

typedef volatile signed char	vs8;	/* signed  8-bit */
typedef volatile short			vs16;	/* signed 16-bit */
typedef volatile int			vs32;	/* signed 32-bit */
typedef volatile long long		vs64;	/* signed 64-bit */

typedef float				f32;	/* single prec floating point */
typedef double				f64;	/* double prec floating point */

// Our64: N64 memory (RDRAM, DMEM, IMEM, the ROM header) is held as host-endian
// 32-bit words. On a little-endian host the N64 byte at address a sits at host
// byte a ^ 3, and the halfword at a sits at host byte a ^ 2 (halfword index
// i ^ 1). On a big-endian host each sits at its own address.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define GLN64_BIG_ENDIAN 1
#define BYTE_ADDR_XOR 0
#define HALF_ADDR_XOR 0
#define HALF_IDX_XOR 0
#else
#define BYTE_ADDR_XOR 3
#define HALF_ADDR_XOR 2
#define HALF_IDX_XOR 1
#endif

// Our64: structs laid over N64 memory list the fields of each 32-bit word in
// little-endian host order (the word's last N64 byte first). N64_WORD keeps
// that order on a little-endian host and reverses it on a big-endian one:
//   N64_WORD(s16 y, s16 x)  is  s16 y; s16 x;  or  s16 x; s16 y;
#define N64_WORD_N(_1, _2, _3, _4, N, ...) N
#define N64_WORD(...) N64_WORD_N(__VA_ARGS__, N64_WORD4, N64_WORD3, N64_WORD2, N64_WORD1)(__VA_ARGS__)
#define N64_WORD1(a) a;
#ifdef GLN64_BIG_ENDIAN
#define N64_WORD2(a, b) b; a;
#define N64_WORD3(a, b, c) c; b; a;
#define N64_WORD4(a, b, c, d) d; c; b; a;
#else
#define N64_WORD2(a, b) a; b;
#define N64_WORD3(a, b, c) a; b; c;
#define N64_WORD4(a, b, c, d) a; b; c; d;
#endif

#ifndef TRUE
#define TRUE    1
#endif

#ifndef FALSE
#define FALSE   0
#endif

#ifndef NULL
#define NULL    0
#endif

#ifndef PLUGIN_PATH_SIZE
#define PLUGIN_PATH_SIZE 260
#endif

template <typename T>
class ValueKeeper
{
public:
	ValueKeeper(T& _obj, T _newVal)
		: m_obj(_obj)
		, m_val(_obj)
	{
		m_obj = _newVal;
	}

	~ValueKeeper()
	{
		m_obj = m_val;
	}

private:
	T & m_obj;
	T m_val;
};

#endif // TYPES_H
