#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef DERPNET_API
#	ifdef DERPNET_STATIC
#		define DERPNET_API static inline
#	else
#		define DERPNET_API extern
#	endif
#endif

//
// interface
//

typedef struct {
	uint8_t Bytes[32];
} DerpKey;

DERPNET_API void DerpNet_CreateNewKey(DerpKey* UserSecret);
DERPNET_API void DerpNet_GetPublicKey(const DerpKey* UserSecret, DerpKey* UserPublic);

typedef struct {
	uintptr_t Socket;
	void* SocketEvent;
	void* CredHandle[2];
	void* CtxHandle[2];
	uint8_t UserPrivateKey[32];
	uint8_t LastPublicKey[32];
	uint8_t LastSharedKey[32];
	size_t BufferSize;
	size_t BufferReceived;
	size_t LastFrameSize;
	size_t TotalReceived;
	size_t TotalSent;
	uint8_t Buffer[1 << 16];
} DerpNet;

// use DERP server hostname from https://login.tailscale.com/derpmap/default
DERPNET_API bool DerpNet_Open(DerpNet* Net, const char* DerpServer, const DerpKey* UserSecret);
DERPNET_API void DerpNet_Close(DerpNet* Net);

// returns 1 when received data from other user, pointer is valid till next call
// returns -1 if disconnected from server
// returns 0 if no new info is available to read
// if Wait=true, then never returns 0 - always waits for one incoming message
DERPNET_API int DerpNet_Recv(DerpNet* Net, DerpKey* ReceivedUserPublicKey, uint8_t** ReceivedData, uint32_t* ReceivedSize, bool Wait);

// returns false if disconnected
DERPNET_API bool DerpNet_Send(DerpNet* Net, const DerpKey* TargetUserPublicKey, const void* Data, size_t DataSize);

// use this if you're an expert!
DERPNET_API bool DerpNet_SendEx(DerpNet* Net, const DerpKey* TargetUserPublicKey, const uint8_t SharedKey[32], const uint8_t Nonce[24], const void* Data, size_t DataSize);

//
// implementation
//

#if defined(DERPNET_STATIC) || defined(DERPNET_IMPLEMENTATION)

#include <stdio.h>
#include <string.h>

#define SECURITY_WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <security.h>
#include <schannel.h>
#include <bcrypt.h>
#pragma comment (lib, "bcrypt")
#pragma comment (lib, "ws2_32")
#pragma comment (lib, "secur32")

//
// helpers
//

#if defined(__SIZEOF_INT128__)
	typedef unsigned __int128 uint128;
#	define mul64x64_128(out,a,b) out = (uint128)a * b
#	define shr128_pair(out,hi,lo,shift) out = (uint64_t)((((uint128)hi << 64) | lo) >> (shift))
#	define shl128_pair(out,hi,lo,shift) out = (uint64_t)(((((uint128)hi << 64) | lo) << (shift)) >> 64);
#	define shr128(out,in,shift) out = (uint64_t)(in >> (shift))
#	define shl128(out,in,shift) out = (uint64_t)((in << shift) >> 64)
#	define add128(a,b) a += b
#	define add128_64(a,b) a += (uint64_t)b
#	define lo128(a) ((uint64_t)a)
#	define hi128(a) ((uint64_t)(a >> 64))
#elif defined(_MSC_VER)
	typedef struct { uint64_t lo, hi; } uint128;
#	include <intrin.h>
#	define mul64x64_128(out,a,b) out.lo = _umul128(a,b,&out.hi)
#	define shr128_pair(out,hi,lo,shift) out = __shiftright128(lo, hi, shift)
#	define shl128_pair(out,hi,lo,shift) out = __shiftleft128(lo, hi, shift)
#	define shr128(out,in,shift) shr128_pair(out, in.hi, in.lo, shift)
#	define shl128(out,in,shift) shl128_pair(out, in.hi, in.lo, shift)
#	define add128(a,b) do { uint64_t p = a.lo; a.lo += b.lo; a.hi += b.hi + (a.lo < p); } while (0)
#	define add128_64(a,b) do { uint64_t p = a.lo; a.lo += b; a.hi += (a.lo < p); } while (0)
#	define lo128(a) (a.lo)
#	define hi128(a) (a.hi)
#else
#	error unsupported compiler/target
#endif

#if defined(__clang__)
#	define rol32(x, n) __builtin_rotateleft32(x, n)
#elif defined(_MSC_VER)
#	define rol32(x, n) _rotl(x, n)
#else
#	define rol32(x, n) ( ((x) << (n)) | ((x) >> (32-(n))) )
#endif

#if !defined(NDEBUG)
#	define DERPNET_ASSERT(cond) do { if (!(cond)) __debugbreak(); } while (0)
#	define DERPNET_LOG(...) do {                                  \
	char LogBuffer[256];                                          \
	snprintf(LogBuffer, sizeof(LogBuffer), "DERP: " __VA_ARGS__); \
	OutputDebugStringA(LogBuffer);                                \
	OutputDebugStringA("\n");                                     \
} while (0)
#else
#	define DERPNET_ASSERT(cond) do { (void)(cond); } while (0)
#	define DERPNET_LOG(...) do { (void)sizeof(__VA_ARGS__); } while (0)
#endif

static inline uint32_t Get32LE(const uint8_t* Buffer)
{
	return (Buffer[3] << 24) + (Buffer[2] << 16) + (Buffer[1] << 8) + Buffer[0];
}

static inline uint32_t Get32BE(const uint8_t* Buffer)
{
	return (Buffer[0] << 24) + (Buffer[1] << 16) + (Buffer[2] << 8) + Buffer[3];
}

static inline uint64_t Get64LE(const uint8_t* Buffer)
{
	return ((uint64_t)Buffer[7] << 56)
		 + ((uint64_t)Buffer[6] << 48)
		 + ((uint64_t)Buffer[5] << 40)
		 + ((uint64_t)Buffer[4] << 32)
		 + ((uint64_t)Buffer[3] << 24)
		 + ((uint64_t)Buffer[2] << 16)
		 + ((uint64_t)Buffer[1] << 8)
		 + ((uint64_t)Buffer[0]);
}

static inline void Set32LE(uint8_t* Buffer, uint32_t Value)
{
	Buffer[0] = Value;
	Buffer[1] = Value >> 8;
	Buffer[2] = Value >> 16;
	Buffer[3] = Value >> 24;
}

static inline void Set32BE(uint8_t* Buffer, uint32_t Value)
{
	Buffer[3] = Value;
	Buffer[2] = Value >> 8;
	Buffer[1] = Value >> 16;
	Buffer[0] = Value >> 24;
}

static inline void Set64LE(uint8_t* Buffer, uint64_t Value)
{
	Buffer[0] = (uint8_t)(Value);
	Buffer[1] = (uint8_t)(Value >> 8);
	Buffer[2] = (uint8_t)(Value >> 16);
	Buffer[3] = (uint8_t)(Value >> 24);
	Buffer[4] = (uint8_t)(Value >> 32);
	Buffer[5] = (uint8_t)(Value >> 40);
	Buffer[6] = (uint8_t)(Value >> 48);
	Buffer[7] = (uint8_t)(Value >> 56);
}

static inline void DerpNet__GetRandom(void* Buffer, size_t BufferSize)
{
	int Status = BCryptGenRandom(NULL, (PUCHAR)Buffer, (ULONG)BufferSize, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	DERPNET_ASSERT(Status == 0);
}

//
// curve25519, based on public domain code from https://github.com/floodyberry/curve25519-donna
//

typedef uint64_t bignum25519[5];

static const uint64_t reduce_mask_51 = (1ULL << 51) - 1;

static void curve25519_copy(bignum25519 out, const bignum25519 in)
{
	out[0] = in[0];
	out[1] = in[1];
	out[2] = in[2];
	out[3] = in[3];
	out[4] = in[4];
}

static void curve25519_add(bignum25519 out, const bignum25519 a, const bignum25519 b)
{
	out[0] = a[0] + b[0];
	out[1] = a[1] + b[1];
	out[2] = a[2] + b[2];
	out[3] = a[3] + b[3];
	out[4] = a[4] + b[4];
}

static void curve25519_sub(bignum25519 out, const bignum25519 a, const bignum25519 b)
{
	const uint64_t two54m152 = (1ULL << 54) - 152;
	const uint64_t two54m8 = (1ULL << 54) - 8;

	out[0] = a[0] + two54m152 - b[0];
	out[1] = a[1] + two54m8 - b[1];
	out[2] = a[2] + two54m8 - b[2];
	out[3] = a[3] + two54m8 - b[3];
	out[4] = a[4] + two54m8 - b[4];
}

static void curve25519_scalar_product(bignum25519 out, const bignum25519 in, const uint64_t scalar)
{
	uint128 a;
	uint64_t c;

	mul64x64_128(a, in[0], scalar);                  out[0] = lo128(a) & reduce_mask_51; shr128(c, a, 51);
	mul64x64_128(a, in[1], scalar); add128_64(a, c); out[1] = lo128(a) & reduce_mask_51; shr128(c, a, 51);
	mul64x64_128(a, in[2], scalar); add128_64(a, c); out[2] = lo128(a) & reduce_mask_51; shr128(c, a, 51);
	mul64x64_128(a, in[3], scalar); add128_64(a, c); out[3] = lo128(a) & reduce_mask_51; shr128(c, a, 51);
	mul64x64_128(a, in[4], scalar); add128_64(a, c); out[4] = lo128(a) & reduce_mask_51; shr128(c, a, 51);
	                                                 out[0] += c * 19;
}

static void curve25519_mul(bignum25519 out, const bignum25519 a, const bignum25519 b)
{
	uint128 mul;

	uint128 t[5];
	uint64_t r0,r1,r2,r3,r4,s0,s1,s2,s3,s4,c;

	r0 = b[0];
	r1 = b[1];
	r2 = b[2];
	r3 = b[3];
	r4 = b[4];

	s0 = a[0];
	s1 = a[1];
	s2 = a[2];
	s3 = a[3];
	s4 = a[4];

	mul64x64_128(t[0], r0, s0);
	mul64x64_128(t[1], r0, s1); mul64x64_128(mul, r1, s0); add128(t[1], mul);
	mul64x64_128(t[2], r0, s2); mul64x64_128(mul, r2, s0); add128(t[2], mul); mul64x64_128(mul, r1, s1); add128(t[2], mul);
	mul64x64_128(t[3], r0, s3); mul64x64_128(mul, r3, s0); add128(t[3], mul); mul64x64_128(mul, r1, s2); add128(t[3], mul); mul64x64_128(mul, r2, s1); add128(t[3], mul);
	mul64x64_128(t[4], r0, s4); mul64x64_128(mul, r4, s0); add128(t[4], mul); mul64x64_128(mul, r3, s1); add128(t[4], mul); mul64x64_128(mul, r1, s3); add128(t[4], mul); mul64x64_128(mul, r2, s2); add128(t[4], mul);

	r1 *= 19;
	r2 *= 19;
	r3 *= 19;
	r4 *= 19;

	mul64x64_128(mul, r4, s1); add128(t[0], mul); mul64x64_128(mul, r1, s4); add128(t[0], mul); mul64x64_128(mul, r2, s3); add128(t[0], mul); mul64x64_128(mul, r3, s2); add128(t[0], mul);
	mul64x64_128(mul, r4, s2); add128(t[1], mul); mul64x64_128(mul, r2, s4); add128(t[1], mul); mul64x64_128(mul, r3, s3); add128(t[1], mul);
	mul64x64_128(mul, r4, s3); add128(t[2], mul); mul64x64_128(mul, r3, s4); add128(t[2], mul);
	mul64x64_128(mul, r4, s4); add128(t[3], mul);

	                     r0 = lo128(t[0]) & reduce_mask_51; shr128(c, t[0], 51);
	add128_64(t[1], c);  r1 = lo128(t[1]) & reduce_mask_51; shr128(c, t[1], 51);
	add128_64(t[2], c);  r2 = lo128(t[2]) & reduce_mask_51; shr128(c, t[2], 51);
	add128_64(t[3], c);  r3 = lo128(t[3]) & reduce_mask_51; shr128(c, t[3], 51);
	add128_64(t[4], c);  r4 = lo128(t[4]) & reduce_mask_51; shr128(c, t[4], 51);
	r0 +=   c * 19; c = r0 >> 51; r0 = r0 & reduce_mask_51;
	r1 +=   c;

	out[0] = r0;
	out[1] = r1;
	out[2] = r2;
	out[3] = r3;
	out[4] = r4;
}

static void curve25519_square_times(bignum25519 out, const bignum25519 in, uint64_t count)
{
	uint128 mul;

	uint128 t[5];
	uint64_t r0,r1,r2,r3,r4,c;
	uint64_t d0,d1,d2,d4,d419;

	r0 = in[0];
	r1 = in[1];
	r2 = in[2];
	r3 = in[3];
	r4 = in[4];

	do
	{
		d0 = r0 * 2;
		d1 = r1 * 2;
		d2 = r2 * 2 * 19;
		d419 = r4 * 19;
		d4 = d419 * 2;

		mul64x64_128(t[0], r0, r0); mul64x64_128(mul, d4, r1); add128(t[0], mul); mul64x64_128(mul, d2,      r3); add128(t[0], mul);
		mul64x64_128(t[1], d0, r1); mul64x64_128(mul, d4, r2); add128(t[1], mul); mul64x64_128(mul, r3, r3 * 19); add128(t[1], mul);
		mul64x64_128(t[2], d0, r2); mul64x64_128(mul, r1, r1); add128(t[2], mul); mul64x64_128(mul, d4,      r3); add128(t[2], mul);
		mul64x64_128(t[3], d0, r3); mul64x64_128(mul, d1, r2); add128(t[3], mul); mul64x64_128(mul, r4,    d419); add128(t[3], mul);
		mul64x64_128(t[4], d0, r4); mul64x64_128(mul, d1, r3); add128(t[4], mul); mul64x64_128(mul, r2,      r2); add128(t[4], mul);

		                     r0 = lo128(t[0]) & reduce_mask_51; shr128(c, t[0], 51);
		add128_64(t[1], c);  r1 = lo128(t[1]) & reduce_mask_51; shr128(c, t[1], 51);
		add128_64(t[2], c);  r2 = lo128(t[2]) & reduce_mask_51; shr128(c, t[2], 51);
		add128_64(t[3], c);  r3 = lo128(t[3]) & reduce_mask_51; shr128(c, t[3], 51);
		add128_64(t[4], c);  r4 = lo128(t[4]) & reduce_mask_51; shr128(c, t[4], 51);
		r0 +=   c * 19; c = r0 >> 51; r0 = r0 & reduce_mask_51;
		r1 +=   c;
	}
	while(--count);

	out[0] = r0;
	out[1] = r1;
	out[2] = r2;
	out[3] = r3;
	out[4] = r4;
}

static void curve25519_square(bignum25519 out, const bignum25519 in)
{
	uint128 mul;

	uint128 t[5];
	uint64_t r0,r1,r2,r3,r4,c;
	uint64_t d0,d1,d2,d4,d419;

	r0 = in[0];
	r1 = in[1];
	r2 = in[2];
	r3 = in[3];
	r4 = in[4];

	d0 = r0 * 2;
	d1 = r1 * 2;
	d2 = r2 * 2 * 19;
	d419 = r4 * 19;
	d4 = d419 * 2;

	mul64x64_128(t[0], r0, r0); mul64x64_128(mul, d4, r1); add128(t[0], mul); mul64x64_128(mul, d2,      r3); add128(t[0], mul);
	mul64x64_128(t[1], d0, r1); mul64x64_128(mul, d4, r2); add128(t[1], mul); mul64x64_128(mul, r3, r3 * 19); add128(t[1], mul);
	mul64x64_128(t[2], d0, r2); mul64x64_128(mul, r1, r1); add128(t[2], mul); mul64x64_128(mul, d4,      r3); add128(t[2], mul);
	mul64x64_128(t[3], d0, r3); mul64x64_128(mul, d1, r2); add128(t[3], mul); mul64x64_128(mul, r4,    d419); add128(t[3], mul);
	mul64x64_128(t[4], d0, r4); mul64x64_128(mul, d1, r3); add128(t[4], mul); mul64x64_128(mul, r2,      r2); add128(t[4], mul);

	                     r0 = lo128(t[0]) & reduce_mask_51; shr128(c, t[0], 51);
	add128_64(t[1], c);  r1 = lo128(t[1]) & reduce_mask_51; shr128(c, t[1], 51);
	add128_64(t[2], c);  r2 = lo128(t[2]) & reduce_mask_51; shr128(c, t[2], 51);
	add128_64(t[3], c);  r3 = lo128(t[3]) & reduce_mask_51; shr128(c, t[3], 51);
	add128_64(t[4], c);  r4 = lo128(t[4]) & reduce_mask_51; shr128(c, t[4], 51);
	r0 +=   c * 19; c = r0 >> 51; r0 = r0 & reduce_mask_51;
	r1 +=   c;

	out[0] = r0;
	out[1] = r1;
	out[2] = r2;
	out[3] = r3;
	out[4] = r4;
}

static void curve25519_expand(bignum25519 out, const uint8_t* in)
{
	uint64_t x0 = Get64LE(&in[ 0]);
	uint64_t x1 = Get64LE(&in[ 8]);
	uint64_t x2 = Get64LE(&in[16]);
	uint64_t x3 = Get64LE(&in[24]);

	out[0] = x0 & reduce_mask_51; x0 = (x0 >> 51) | (x1 << 13);
	out[1] = x0 & reduce_mask_51; x1 = (x1 >> 38) | (x2 << 26);
	out[2] = x1 & reduce_mask_51; x2 = (x2 >> 25) | (x3 << 39);
	out[3] = x2 & reduce_mask_51; x3 = (x3 >> 12);
	out[4] = x3 & reduce_mask_51; /* ignore the top bit */
}

static void curve25519_contract(uint8_t* out, const bignum25519 input)
{
	uint64_t t[5];
	uint64_t f;

	t[0] = input[0];
	t[1] = input[1];
	t[2] = input[2];
	t[3] = input[3];
	t[4] = input[4];

	#define curve25519_contract_carry() \
		t[1] += t[0] >> 51; t[0] &= reduce_mask_51; \
		t[2] += t[1] >> 51; t[1] &= reduce_mask_51; \
		t[3] += t[2] >> 51; t[2] &= reduce_mask_51; \
		t[4] += t[3] >> 51; t[3] &= reduce_mask_51;

	#define curve25519_contract_carry_full() curve25519_contract_carry() \
		t[0] += 19 * (t[4] >> 51); t[4] &= reduce_mask_51;

	#define curve25519_contract_carry_final() curve25519_contract_carry() \
		t[4] &= reduce_mask_51;

	curve25519_contract_carry_full()
	curve25519_contract_carry_full()

	/* now t is between 0 and 2^255-1, properly carried. */
	/* case 1: between 0 and 2^255-20. case 2: between 2^255-19 and 2^255-1. */
	t[0] += 19;
	curve25519_contract_carry_full()

	/* now between 19 and 2^255-1 in both cases, and offset by 19. */
	t[0] += 0x8000000000000 - 19;
	t[1] += 0x8000000000000 - 1;
	t[2] += 0x8000000000000 - 1;
	t[3] += 0x8000000000000 - 1;
	t[4] += 0x8000000000000 - 1;

	/* now between 2^255 and 2^256-20, and offset by 2^255. */
	curve25519_contract_carry_final()

	#define write51full(n,shift) \
		f = ((t[n] >> shift) | (t[n+1] << (51 - shift))); \
		for (int i = 0; i < 8; i++, f >>= 8) *out++ = (uint8_t)f;
	#define write51(n) write51full(n,13*n)

	write51(0)
	write51(1)
	write51(2)
	write51(3)

	#undef curve25519_contract_carry
	#undef curve25519_contract_carry_full
	#undef curve25519_contract_carry_final
	#undef write51full
	#undef write51
}

static void curve25519_swap_conditional(bignum25519 x, bignum25519 qpx, uint64_t iswap)
{
	const uint64_t swap = (uint64_t)(-(int64_t)iswap);
	uint64_t x0,x1,x2,x3,x4;

	x0 = swap & (x[0] ^ qpx[0]); x[0] ^= x0; qpx[0] ^= x0;
	x1 = swap & (x[1] ^ qpx[1]); x[1] ^= x1; qpx[1] ^= x1;
	x2 = swap & (x[2] ^ qpx[2]); x[2] ^= x2; qpx[2] ^= x2;
	x3 = swap & (x[3] ^ qpx[3]); x[3] ^= x3; qpx[3] ^= x3;
	x4 = swap & (x[4] ^ qpx[4]); x[4] ^= x4; qpx[4] ^= x4;

}

static void curve25519_pow_two5mtwo0_two250mtwo0(bignum25519 b)
{
	bignum25519 t0, c;

	/* 2^5   - 2^0   */ /* b */
	/* 2^10  - 2^5   */ curve25519_square_times(t0, b, 5);
	/* 2^10  - 2^0   */ curve25519_mul(b, t0, b);
	/* 2^20  - 2^10  */ curve25519_square_times(t0, b, 10);
	/* 2^20  - 2^0   */ curve25519_mul(c, t0, b);
	/* 2^40  - 2^20  */ curve25519_square_times(t0, c, 20);
	/* 2^40  - 2^0   */ curve25519_mul(t0, t0, c);
	/* 2^50  - 2^10  */ curve25519_square_times(t0, t0, 10);
	/* 2^50  - 2^0   */ curve25519_mul(b, t0, b);
	/* 2^100 - 2^50  */ curve25519_square_times(t0, b, 50);
	/* 2^100 - 2^0   */ curve25519_mul(c, t0, b);
	/* 2^200 - 2^100 */ curve25519_square_times(t0, c, 100);
	/* 2^200 - 2^0   */ curve25519_mul(t0, t0, c);
	/* 2^250 - 2^50  */ curve25519_square_times(t0, t0, 50);
	/* 2^250 - 2^0   */ curve25519_mul(b, t0, b);
}

static void curve25519_recip(bignum25519 out, const bignum25519 z)
{
	bignum25519 a, t0, b;

	/* 2 */              curve25519_square(a, z);  /* a = 2 */
	/* 8 */              curve25519_square_times(t0, a, 2);
	/* 9 */              curve25519_mul(b, t0, z); /* b = 9 */
	/* 11 */             curve25519_mul(a, b, a);  /* a = 11 */
	/* 22 */             curve25519_square(t0, a);
	/* 2^5 - 2^0 = 31 */ curve25519_mul(b, t0, b);
	/* 2^250 - 2^0 */    curve25519_pow_two5mtwo0_two250mtwo0(b);
	/* 2^255 - 2^5 */    curve25519_square_times(b, b, 5);
	/* 2^255 - 21 */     curve25519_mul(out, b, a);
}

static void curve25519_scalarmult(uint8_t* mypublic, const uint8_t* n, const uint8_t* basepoint)
{
	// curve25519-donna-64bit.h
	// curve25519-donna-common.h
	bignum25519 nqpqx = { 1 }, nqpqz = { 0 }, nqz = { 1 }, nqx;
	bignum25519 q, qx, qpqx, qqx, zzz, zmone;

	curve25519_expand(q, basepoint);
	curve25519_copy(nqx, q);

	/* bit 255 is always 0, and bit 254 is always 1, so skip bit 255 and 
	   start pre-swapped on bit 254 */
	size_t lastbit = 1;

	/* we are doing bits 254..3 in the loop, but are swapping in bits 253..2 */
	for (int i = 253; i >= 2; i--)
	{
		curve25519_add(qx, nqx, nqz);
		curve25519_sub(nqz, nqx, nqz);
		curve25519_add(qpqx, nqpqx, nqpqz);
		curve25519_sub(nqpqz, nqpqx, nqpqz);
		curve25519_mul(nqpqx, qpqx, nqz);
		curve25519_mul(nqpqz, qx, nqpqz);
		curve25519_add(qqx, nqpqx, nqpqz);
		curve25519_sub(nqpqz, nqpqx, nqpqz);
		curve25519_square(nqpqz, nqpqz);
		curve25519_square(nqpqx, qqx);
		curve25519_mul(nqpqz, nqpqz, q);
		curve25519_square(qx, qx);
		curve25519_square(nqz, nqz);
		curve25519_mul(nqx, qx, nqz);
		curve25519_sub(nqz, qx, nqz);
		curve25519_scalar_product(zzz, nqz, 121665);
		curve25519_add(zzz, zzz, qx);
		curve25519_mul(nqz, nqz, zzz);

		size_t bit = (n[i/8] >> (i & 7)) & 1;
		curve25519_swap_conditional(nqx, nqpqx, bit ^ lastbit);
		curve25519_swap_conditional(nqz, nqpqz, bit ^ lastbit);
		lastbit = bit;
	}

	/* the final 3 bits are always zero, so we only need to double */
	for (int i = 0; i < 3; i++)
	{
		curve25519_add(qx, nqx, nqz);
		curve25519_sub(nqz, nqx, nqz);
		curve25519_square(qx, qx);
		curve25519_square(nqz, nqz);
		curve25519_mul(nqx, qx, nqz);
		curve25519_sub(nqz, qx, nqz);
		curve25519_scalar_product(zzz, nqz, 121665);
		curve25519_add(zzz, zzz, qx);
		curve25519_mul(nqz, nqz, zzz);
	}

	curve25519_recip(zmone, nqz);
	curve25519_mul(nqz, nqx, zmone);
	curve25519_contract(mypublic, nqz);
}

//
// poly1305, based on public domain code from https://github.com/floodyberry/poly1305-donna
//

#define poly1305_block_size 16

typedef struct {
	uint64_t r[3];
	uint64_t h[3];
	uint64_t pad[2];
	size_t leftover;
	unsigned char buffer[poly1305_block_size];
	unsigned char final;
} poly1305_state_internal_t;

static void poly1305_init(poly1305_state_internal_t* st, const uint8_t key[32])
{
	/* r &= 0xffffffc0ffffffc0ffffffc0fffffff */
	uint64_t t0 = Get64LE(&key[0]);
	uint64_t t1 = Get64LE(&key[8]);
	st->r[0] = ( t0                    ) & 0xffc0fffffff;
	st->r[1] = ((t0 >> 44) | (t1 << 20)) & 0xfffffc0ffff;
	st->r[2] = ((t1 >> 24)             ) & 0x00ffffffc0f;

	/* h = 0 */
	st->h[0] = 0;
	st->h[1] = 0;
	st->h[2] = 0;

	/* save pad for later */
	st->pad[0] = Get64LE(&key[16]);
	st->pad[1] = Get64LE(&key[24]);

	st->leftover = 0;
	st->final = 0;
}

static void poly1305_blocks(poly1305_state_internal_t* st, const uint8_t* m, size_t bytes)
{
	const uint64_t hibit = (st->final) ? 0 : (1ULL << 40); /* 1 << 128 */
	uint64_t r0,r1,r2;
	uint64_t s1,s2;
	uint64_t h0,h1,h2;
	uint64_t c;
	uint128 d0,d1,d2,d;

	r0 = st->r[0];
	r1 = st->r[1];
	r2 = st->r[2];

	h0 = st->h[0];
	h1 = st->h[1];
	h2 = st->h[2];

	s1 = r1 * (5 << 2);
	s2 = r2 * (5 << 2);

	while (bytes >= poly1305_block_size)
	{
		/* h += m[i] */
		uint64_t t0 = Get64LE(&m[0]);
		uint64_t t1 = Get64LE(&m[8]);

		h0 += (( t0                    ) & 0xfffffffffff);
		h1 += (((t0 >> 44) | (t1 << 20)) & 0xfffffffffff);
		h2 += (((t1 >> 24)             ) & 0x3ffffffffff) | hibit;

		/* h *= r */
		mul64x64_128(d0, h0, r0); mul64x64_128(d, h1, s2); add128(d0, d); mul64x64_128(d, h2, s1); add128(d0, d);
		mul64x64_128(d1, h0, r1); mul64x64_128(d, h1, r0); add128(d1, d); mul64x64_128(d, h2, s2); add128(d1, d);
		mul64x64_128(d2, h0, r2); mul64x64_128(d, h1, r1); add128(d2, d); mul64x64_128(d, h2, r0); add128(d2, d);

		/* (partial) h %= p */
		                  shr128(c, d0, 44); h0 = lo128(d0) & 0xfffffffffff;
		add128_64(d1, c); shr128(c, d1, 44); h1 = lo128(d1) & 0xfffffffffff;
		add128_64(d2, c); shr128(c, d2, 42); h2 = lo128(d2) & 0x3ffffffffff;
		h0  += c * 5; c = (h0 >> 44);  h0 =    h0  & 0xfffffffffff;
		h1  += c;

		m += poly1305_block_size;
		bytes -= poly1305_block_size;
	}

	st->h[0] = h0;
	st->h[1] = h1;
	st->h[2] = h2;
}

void poly1305_finish(poly1305_state_internal_t* st, uint8_t mac[16])
{
	uint64_t h0,h1,h2,c;
	uint64_t g0,g1,g2;
	uint64_t t0,t1;

	/* process the remaining block */
	if (st->leftover)
	{
		size_t i = st->leftover;
		st->buffer[i] = 1;
		for (i = i + 1; i < poly1305_block_size; i++)
			st->buffer[i] = 0;
		st->final = 1;
		poly1305_blocks(st, st->buffer, poly1305_block_size);
	}

	/* fully carry h */
	h0 = st->h[0];
	h1 = st->h[1];
	h2 = st->h[2];

	             c = (h1 >> 44); h1 &= 0xfffffffffff;
	h2 += c;     c = (h2 >> 42); h2 &= 0x3ffffffffff;
	h0 += c * 5; c = (h0 >> 44); h0 &= 0xfffffffffff;
	h1 += c;     c = (h1 >> 44); h1 &= 0xfffffffffff;
	h2 += c;     c = (h2 >> 42); h2 &= 0x3ffffffffff;
	h0 += c * 5; c = (h0 >> 44); h0 &= 0xfffffffffff;
	h1 += c;

	/* compute h + -p */
	g0 = h0 + 5; c = (g0 >> 44); g0 &= 0xfffffffffff;
	g1 = h1 + c; c = (g1 >> 44); g1 &= 0xfffffffffff;
	g2 = h2 + c - (1ULL << 42);

	/* select h if h < p, or h + -p if h >= p */
	c = (g2 >> ((sizeof(uint64_t) * 8) - 1)) - 1;
	g0 &= c;
	g1 &= c;
	g2 &= c;
	c = ~c;
	h0 = (h0 & c) | g0;
	h1 = (h1 & c) | g1;
	h2 = (h2 & c) | g2;

	/* h = (h + pad) */
	t0 = st->pad[0];
	t1 = st->pad[1];

	h0 += (( t0                    ) & 0xfffffffffff)    ; c = (h0 >> 44); h0 &= 0xfffffffffff;
	h1 += (((t0 >> 44) | (t1 << 20)) & 0xfffffffffff) + c; c = (h1 >> 44); h1 &= 0xfffffffffff;
	h2 += (((t1 >> 24)             ) & 0x3ffffffffff) + c;                 h2 &= 0x3ffffffffff;

	/* mac = h % (2^128) */
	h0 = ((h0      ) | (h1 << 44));
	h1 = ((h1 >> 20) | (h2 << 24));

	Set64LE(&mac[0], h0);
	Set64LE(&mac[8], h1);
}

static void poly1305_update(poly1305_state_internal_t* st, const uint8_t* m, size_t bytes)
{
	/* handle leftover */
	if (st->leftover)
	{
		size_t want = (poly1305_block_size - st->leftover);
		if (want > bytes)
			want = bytes;
		memcpy(st->buffer + st->leftover, m, want);
		bytes -= want;
		m += want;
		st->leftover += want;
		if (st->leftover < poly1305_block_size)
			return;
		poly1305_blocks(st, st->buffer, poly1305_block_size);
		st->leftover = 0;
	}

	/* process full blocks */
	if (bytes >= poly1305_block_size)
	{
		size_t want = (bytes & ~(poly1305_block_size - 1));
		poly1305_blocks(st, m, want);
		m += want;
		bytes -= want;
	}

	/* store leftover */
	if (bytes)
	{
		memcpy(st->buffer + st->leftover, m, bytes);
		st->leftover += bytes;
	}
}

static void poly1305_auth(uint8_t mac[16], const uint8_t* m, size_t bytes, const uint8_t key[32])
{
	poly1305_state_internal_t st;
	poly1305_init(&st, key);
	poly1305_update(&st, m, bytes);
	poly1305_finish(&st, mac);
}

static int poly1305_verify(const uint8_t mac1[16], const uint8_t mac2[16])
{
	uint64_t a1, a2, b1, b2;
	memcpy(&a1, mac1 + 0, sizeof(a1));
	memcpy(&a2, mac1 + 8, sizeof(a2));
	memcpy(&b1, mac2 + 0, sizeof(b1));
	memcpy(&b2, mac2 + 8, sizeof(b2));
	return ((a1 ^ b1) | (a2 ^ b2)) == 0;
}

//
// salsa20, based on info from https://en.wikipedia.org/wiki/Salsa20#Structure
//

static void salsa20_rounds(uint32_t x[16])
{
	for (int i = 0; i < 20; i += 2)
	{
		uint32_t t;

#define Q(a,b,c,d) \
		t = x[a] + x[d]; x[b] ^= rol32(t,  7); \
		t = x[b] + x[a]; x[c] ^= rol32(t,  9); \
		t = x[c] + x[b]; x[d] ^= rol32(t, 13); \
		t = x[d] + x[c]; x[a] ^= rol32(t, 18)

		Q( 0,  4,  8, 12);
		Q( 5,  9, 13,  1);
		Q(10, 14,  2,  6);
		Q(15,  3,  7, 11);

		Q( 0,  1,  2,  3);
		Q( 5,  6,  7,  4);
		Q(10, 11,  8,  9);
		Q(15, 12, 13, 14);

#undef Q

	}
}

static const char salsa20_constant[] = "expand 32-byte k";

static void hsalsa20(uint8_t Output[32], const uint8_t Input[16], const uint8_t Key[32])
{
	uint32_t x[16];
	x[ 0] = Get32LE((uint8_t*)&salsa20_constant[0]);
	x[ 1] = Get32LE(&Key[ 0]);
	x[ 2] = Get32LE(&Key[ 4]);
	x[ 3] = Get32LE(&Key[ 8]);
	x[ 4] = Get32LE(&Key[12]);
	x[ 5] = Get32LE((uint8_t*)&salsa20_constant[4]);
	x[ 6] = Get32LE(&Input[ 0]);
	x[ 7] = Get32LE(&Input[ 4]);
	x[ 8] = Get32LE(&Input[ 8]);
	x[ 9] = Get32LE(&Input[12]);
	x[10] = Get32LE((uint8_t*)&salsa20_constant[8]);
	x[11] = Get32LE(&Key[16]);
	x[12] = Get32LE(&Key[20]);
	x[13] = Get32LE(&Key[24]);
	x[14] = Get32LE(&Key[28]);
	x[15] = Get32LE((uint8_t*)&salsa20_constant[12]);

	salsa20_rounds(x);

	Set32LE(&Output[ 0], x[ 0]);
	Set32LE(&Output[ 4], x[ 5]);
	Set32LE(&Output[ 8], x[10]);
	Set32LE(&Output[12], x[15]);
	Set32LE(&Output[16], x[ 6]);
	Set32LE(&Output[20], x[ 7]);
	Set32LE(&Output[24], x[ 8]);
	Set32LE(&Output[28], x[ 9]);
}

static void salsa20(uint8_t Output[64], const uint8_t Input[16], const uint8_t Key[32])
{
	uint32_t x[16];
	x[ 0] = Get32LE((uint8_t*)&salsa20_constant[0]);
	x[ 1] = Get32LE(&Key[ 0]);
	x[ 2] = Get32LE(&Key[ 4]);
	x[ 3] = Get32LE(&Key[ 8]);
	x[ 4] = Get32LE(&Key[12]);
	x[ 5] = Get32LE((uint8_t*)&salsa20_constant[4]);
	x[ 6] = Get32LE(&Input[ 0]);
	x[ 7] = Get32LE(&Input[ 4]);
	x[ 8] = Get32LE(&Input[ 8]);
	x[ 9] = Get32LE(&Input[12]);
	x[10] = Get32LE((uint8_t*)&salsa20_constant[8]);
	x[11] = Get32LE(&Key[16]);
	x[12] = Get32LE(&Key[20]);
	x[13] = Get32LE(&Key[24]);
	x[14] = Get32LE(&Key[28]);
	x[15] = Get32LE((uint8_t*)&salsa20_constant[12]);

	uint32_t j[16];
	memcpy(j, x, sizeof(j));

	salsa20_rounds(x);

	for (int i = 0; i < 16; i++)
	{
		Set32LE(&Output[i * 4], x[i] + j[i]);
	}
}

static void salsa20_xor(uint8_t* Output, const uint8_t* Input, size_t InputSize, const uint8_t Key[32], const uint8_t Nonce[8], uint64_t Counter)
{
	uint8_t TempInput[16];
	uint8_t Block[64];

	memcpy(TempInput, Nonce, 8);

	while (InputSize >= 64)
	{
		memcpy(TempInput + 8, &Counter, sizeof(Counter));
		salsa20(Block, TempInput, Key);

		for (size_t i = 0; i < 64; ++i)
		{
			Output[i] = Input[i] ^ Block[i];
		}

		Counter += 1;

		Output += 64;
		Input += 64;
		InputSize -= 64;
	}

	if (InputSize)
	{
		memcpy(TempInput + 8, &Counter, sizeof(Counter));
		salsa20(Block, TempInput, Key);

		for (size_t i = 0; i < InputSize; ++i)
		{
			Output[i] = Input[i] ^ Block[i];
		}
	}
}

//
// nacl box seal/unseal
//

static void DerpNet__GetSharedKey(uint8_t SharedKey[32], const uint8_t PrivateKey[32], const uint8_t PublicKey[32])
{
	uint8_t SharedSecret[32];
	curve25519_scalarmult(SharedSecret, PrivateKey, PublicKey);

	uint8_t ZeroInput[16] = { 0 };
	hsalsa20(SharedKey, ZeroInput, SharedSecret);
}

static void DerpNet__BoxSealEx(uint8_t Nonce[24], uint8_t Auth[16], uint8_t* Output, const uint8_t* Input, size_t InputSize, const uint8_t SharedKey[32])
{
	// xsalsa20 key construction
	uint8_t SubKey[32];
	hsalsa20(SubKey, Nonce, SharedKey);

	uint8_t FirstBlock[64] = { 0 };
	salsa20_xor(FirstBlock, FirstBlock, sizeof(FirstBlock), SubKey, Nonce + 16, 0);

	size_t FirstSize = InputSize > 32 ? 32 : InputSize;
	for (size_t i = 0; i < FirstSize; i++)
	{
		Output[i] = Input[i] ^ FirstBlock[32 + i];
	}

	salsa20_xor(Output + FirstSize, Input + FirstSize, InputSize - FirstSize, SubKey, Nonce + 16, 1);

	poly1305_auth(Auth, Output, InputSize, FirstBlock);
}

static void DerpNet__BoxSeal(uint8_t Nonce[24], uint8_t Auth[16], uint8_t* Output, const uint8_t* Input, size_t InputSize, const uint8_t PrivateKey[32], const uint8_t PublicKey[32])
{
	uint8_t SharedKey[32];
	DerpNet__GetSharedKey(SharedKey, PrivateKey, PublicKey);

	DerpNet__GetRandom(Nonce, 24);
	DerpNet__BoxSealEx(Nonce, Auth, Output, Input, InputSize, SharedKey);
}

static bool DerpNet__BoxUnsealEx(uint8_t* Output, const uint8_t* Input, size_t InputSize, const uint8_t Auth[16], const uint8_t Nonce[24], const uint8_t SharedKey[32])
{
	// xsalsa20 key construction
	uint8_t SubKey[32];
	hsalsa20(SubKey, Nonce, SharedKey);

	uint8_t FirstBlock[64] = { 0 };
	salsa20_xor(FirstBlock, FirstBlock, sizeof(FirstBlock), SubKey, Nonce + 16, 0);

	uint8_t ExpectedAuth[16];
	poly1305_auth(ExpectedAuth, Input, InputSize, FirstBlock);

	if (poly1305_verify(Auth, ExpectedAuth) == 0)
	{
		return false;
	}

	size_t FirstSize = InputSize > 32 ? 32 : InputSize;
	for (size_t i = 0; i < FirstSize; i++)
	{
		Output[i] = Input[i] ^ FirstBlock[32 + i];
	}

	salsa20_xor(Output + FirstSize, Input + FirstSize, InputSize - FirstSize, SubKey, Nonce + 16, 1);
	return true;
}

static bool DerpNet__BoxUnseal(uint8_t* Output, const uint8_t* Input, size_t InputSize, const uint8_t Auth[16], const uint8_t Nonce[24], const uint8_t PrivateKey[32], const uint8_t PublicKey[32])
{
	uint8_t SharedKey[32];
	DerpNet__GetSharedKey(SharedKey, PrivateKey, PublicKey);

	return DerpNet__BoxUnsealEx(Output, Input, InputSize, Auth, Nonce, SharedKey);
}

void DerpNet_CreateNewKey(DerpKey* UserSecret)
{
	DerpNet__GetRandom(UserSecret->Bytes, sizeof(UserSecret->Bytes));

	UserSecret->Bytes[ 0] &= 0xf8;
	UserSecret->Bytes[31] &= 0x7f;
	UserSecret->Bytes[31] |= 0x40;
}

void DerpNet_GetPublicKey(const DerpKey* UserSecret, DerpKey* UserPublic)
{
	static const uint8_t Base[32] = { 9 };
	curve25519_scalarmult(UserPublic->Bytes, UserSecret->Bytes, Base);
}

static bool DerpNet__TlsHandshake(DerpNet* Net, const char* Hostname, CredHandle* CredentialHandle, CtxtHandle* ContextHandle)
{
	SCHANNEL_CRED Cred = { 0 };
	Cred.dwVersion = SCHANNEL_CRED_VERSION;
	Cred.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
	Cred.grbitEnabledProtocols = SP_PROT_TLS1_2;

	SECURITY_STATUS SecStatus = AcquireCredentialsHandleA(NULL, UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &Cred, NULL, NULL, CredentialHandle, NULL);
	DERPNET_ASSERT(SecStatus == SEC_E_OK);

	CtxtHandle* Context = NULL;

	for (;;)
	{
		SecBuffer InBuffers[2] = { 0 };
		InBuffers[0].BufferType = SECBUFFER_TOKEN;
		InBuffers[0].pvBuffer = Net->Buffer;
		InBuffers[0].cbBuffer = (unsigned)Net->BufferReceived;
		InBuffers[1].BufferType = SECBUFFER_EMPTY;

		SecBuffer OutBuffers[1] = { 0 };
		OutBuffers[0].BufferType = SECBUFFER_TOKEN;

		SecBufferDesc InDesc = { SECBUFFER_VERSION, ARRAYSIZE(InBuffers), InBuffers };
		SecBufferDesc OutDesc = { SECBUFFER_VERSION, ARRAYSIZE(OutBuffers), OutBuffers };

		DWORD Flags = ISC_REQ_USE_SUPPLIED_CREDS | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_STREAM;
		SecStatus = InitializeSecurityContextA(
			CredentialHandle,
			Context,
			Context ? NULL : (SEC_CHAR*)Hostname,
			Flags,
			0,
			0,
			Context ? &InDesc : NULL,
			0,
			Context ? NULL : ContextHandle,
			&OutDesc,
			&Flags,
			NULL);
		Context = ContextHandle;

		if (InBuffers[1].BufferType == SECBUFFER_EXTRA)
		{
			memmove(Net->Buffer, Net->Buffer + (Net->BufferReceived - InBuffers[1].cbBuffer), InBuffers[1].cbBuffer);
			Net->BufferReceived = InBuffers[1].cbBuffer;
		}
		else
		{
			Net->BufferReceived = 0;
		}

		if (SecStatus == SEC_E_OK)
		{
			return true;
		}
		else if (SecStatus == SEC_I_INCOMPLETE_CREDENTIALS)
		{
			DERPNET_LOG("server asks for client certificate, not expected here");
			return false;
		}
		else if (SecStatus == SEC_I_CONTINUE_NEEDED)
		{
			char* OutBuffer = OutBuffers[0].pvBuffer;
			int OutSize = OutBuffers[0].cbBuffer;

			while (OutSize != 0)
			{
				int WriteSize = send(Net->Socket, OutBuffer, OutSize, 0);
				if (WriteSize <= 0)
				{
					DERPNET_LOG("failed to send data to server, remote server disconnected?");
					return false;
				}
				Net->TotalSent += WriteSize;

				OutSize -= WriteSize;
				OutBuffer += WriteSize;
			}
			FreeContextBuffer(OutBuffers[0].pvBuffer);
		}
		else if (SecStatus != SEC_E_INCOMPLETE_MESSAGE)
		{
			DERPNET_LOG("cannot verify server certificate, bad hostname, expired cert, or bad crypto handshake");
			return false;
		}

		if (Net->BufferReceived == sizeof(Net->BufferReceived))
		{
			DERPNET_LOG("server is sending too much data instead of proper handshake?");
			return false;
		}

		int ReadSize = recv(Net->Socket, (char*)Net->Buffer + Net->BufferReceived, (int)(sizeof(Net->Buffer) - Net->BufferReceived), 0);
		if (ReadSize <= 0)
		{
			DERPNET_LOG("failed to read data from server, remote server disconnected?");
			return false;
		}
		Net->TotalReceived += ReadSize;
		Net->BufferReceived += ReadSize;
	}
}

static bool DerpNet__TlsWrite(DerpNet* Net, const void* Data, size_t DataSize)
{
#if DERPNET_USE_PLAIN_HTTP
	while (DataSize != 0)
	{
		fd_set WriteSet;
		FD_ZERO(&WriteSet);
		FD_SET(Net->Socket, &WriteSet);

		int Select = select((int)(Net->Socket + 1), NULL, &WriteSet, NULL, NULL);
		if (Select < 0)
		{
			DERPNET_LOG("select failed");
			return false;
		}

		int WriteSize = send(Net->Socket, Data, (int)DataSize, 0);
		if (WriteSize <= 0)
		{
			DERPNET_LOG("failed to send data to server, remote server disconnected?");
			return false;
		}
		Net->TotalSent += WriteSize;

		Data = (char*)Data + WriteSize;
		DataSize -= WriteSize;
	}
	return true; 
#else
	CtxtHandle ContextHandle;
	memcpy(&ContextHandle, Net->CtxHandle, sizeof(ContextHandle));

	SecPkgContext_StreamSizes StreamSizes;
	SECURITY_STATUS SecStatus = QueryContextAttributes(&ContextHandle, SECPKG_ATTR_STREAM_SIZES, &StreamSizes);
	DERPNET_ASSERT(SecStatus == SEC_E_OK);

	while (DataSize != 0)
	{
		size_t DataSizeToUse = min(DataSize, StreamSizes.cbMaximumMessage);

		char WriteBuffer[16384 + 512];
		DERPNET_ASSERT(StreamSizes.cbHeader + StreamSizes.cbMaximumMessage + StreamSizes.cbTrailer <= sizeof(WriteBuffer));

		SecBuffer OutBuffers[3] = { 0 };
		OutBuffers[0].BufferType = SECBUFFER_STREAM_HEADER;
		OutBuffers[0].pvBuffer = WriteBuffer;
		OutBuffers[0].cbBuffer = StreamSizes.cbHeader;
		OutBuffers[1].BufferType = SECBUFFER_DATA;
		OutBuffers[1].pvBuffer = WriteBuffer + StreamSizes.cbHeader;
		OutBuffers[1].cbBuffer = (unsigned)DataSizeToUse;
		OutBuffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
		OutBuffers[2].pvBuffer = WriteBuffer + StreamSizes.cbHeader + DataSizeToUse;
		OutBuffers[2].cbBuffer = StreamSizes.cbTrailer;

		memcpy(OutBuffers[1].pvBuffer, Data, DataSizeToUse);

		SecBufferDesc OutDesc = { SECBUFFER_VERSION, ARRAYSIZE(OutBuffers), OutBuffers };
		SecStatus = EncryptMessage(&ContextHandle, 0, &OutDesc, 0);
		DERPNET_ASSERT(SecStatus == SEC_E_OK);

		int SizeToSend = OutBuffers[0].cbBuffer + OutBuffers[1].cbBuffer + OutBuffers[2].cbBuffer;
		int BytesSent = 0;
		while (BytesSent != SizeToSend)
		{
			fd_set WriteSet;
			FD_ZERO(&WriteSet);
			FD_SET(Net->Socket, &WriteSet);

			int Select = select((int)(Net->Socket + 1), NULL, &WriteSet, NULL, NULL);
			if (Select < 0)
			{
				DERPNET_LOG("select failed");
				return false;
			}

			int WriteSize = send(Net->Socket, WriteBuffer + BytesSent, SizeToSend - BytesSent, 0);
			if (WriteSize <= 0)
			{
				DERPNET_LOG("failed to send data to server, remote server disconnected?");
				return false;
			}
			Net->TotalSent += WriteSize;
			BytesSent += WriteSize;
		}

		Data = (char*)Data + DataSizeToUse;
		DataSize -= DataSizeToUse;
	}

	return true;
#endif
}

static bool DerpNet__TlsRead(DerpNet* Net, bool Wait)
{
#if DERPNET_USE_PLAIN_HTTP
	fd_set ReadSet;
	FD_ZERO(&ReadSet);
	FD_SET(Net->Socket, &ReadSet);

	struct timeval TimeVal = { 0, 0 };
	int Select = select((int)(Net->Socket + 1), &ReadSet, NULL, NULL, Wait ? NULL : &TimeVal);
	if (Select < 0)
	{
		return false;
	}
	if (Select == 0)
	{
		return true;
	}

	int ReadSize = recv(Net->Socket, (char*)Net->Buffer + Net->BufferReceived, (int)(sizeof(Net->Buffer) - Net->BufferReceived), 0);
	if (ReadSize <= 0)
	{
		DERPNET_LOG("failed to read data from server, remote server disconnected?");
		return false;
	}
	Net->TotalReceived += ReadSize;
	Net->BufferReceived += ReadSize;
	Net->BufferSize += ReadSize;

	DERPNET_LOG("read %d bytes from socket", ReadSize);
	WSAResetEvent(Net->SocketEvent);

	return true;

#else
	CtxtHandle ContextHandle;
	memcpy(&ContextHandle, Net->CtxHandle, sizeof(ContextHandle));

	SecPkgContext_StreamSizes StreamSizes;
	SECURITY_STATUS SecStatus = QueryContextAttributes(&ContextHandle, SECPKG_ATTR_STREAM_SIZES, &StreamSizes);
	DERPNET_ASSERT(SecStatus == SEC_E_OK);

	for (;;)
	{
		size_t EncryptedSize = Net->BufferReceived - Net->BufferSize;

		if (EncryptedSize != 0)
		{
			SecBuffer InBuffers[4] = { 0 };
			DERPNET_ASSERT(StreamSizes.cBuffers == ARRAYSIZE(InBuffers));

			InBuffers[0].BufferType = SECBUFFER_DATA;
			InBuffers[0].pvBuffer = Net->Buffer + Net->BufferSize;
			InBuffers[0].cbBuffer = (unsigned)EncryptedSize;
			InBuffers[1].BufferType = SECBUFFER_EMPTY;
			InBuffers[2].BufferType = SECBUFFER_EMPTY;
			InBuffers[3].BufferType = SECBUFFER_EMPTY;

			SecBufferDesc InDesc = { SECBUFFER_VERSION, ARRAYSIZE(InBuffers), InBuffers };

			SecStatus = DecryptMessage(&ContextHandle, &InDesc, 0, NULL);
			if (SecStatus == SEC_E_OK)
			{
				//
				// Before decryption, Buffer = [ppppppppppeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee.......]
				//                                        ^                                   ^
				//                                        |                                   |
				// Buffer + BufferSize -------------------+                                   |
				// Buffer + BufferReceived ---------------------------------------------------+
				// 
				// After decryption,  Buffer = [pppppppppphhhhhhhdddddddddtttttttteeeeeeeeeeee.......]
				//                              ^         ^      ^        ^       ^
				//                              |         |      |        |       |
				// Plaintext data in buffer ----+         |      |        |       |
				// Header from current TLS packet --------+      |        |       |
				// Newly decrypted data from current TLS packet -+        |       |
				// Trailer from current TLS packet -----------------------+       |
				// Any extra remaining ciphertext in buffer for next TLS packets -+
				//
				// After memmoves,    Buffer = [ppppppppppdddddddddeeeeeeeeeeee......................]
				//                                                 ^           ^
				//                                                 |           |
				// Buffer + BufferSize ----------------------------+           |
				// Buffer + BufferReceived ------------------------------------+
				//

				DERPNET_ASSERT(InBuffers[0].BufferType == SECBUFFER_STREAM_HEADER);
				DERPNET_ASSERT(InBuffers[1].BufferType == SECBUFFER_DATA);
				DERPNET_ASSERT(InBuffers[2].BufferType == SECBUFFER_STREAM_TRAILER);
				DERPNET_ASSERT(InBuffers[3].BufferType == SECBUFFER_EXTRA || InBuffers[3].BufferType == SECBUFFER_EMPTY);

				DERPNET_ASSERT(InBuffers[0].cbBuffer == StreamSizes.cbHeader);

				DERPNET_ASSERT(InBuffers[0].pvBuffer == Net->Buffer + Net->BufferSize);
				DERPNET_ASSERT(InBuffers[1].pvBuffer == Net->Buffer + Net->BufferSize + InBuffers[0].cbBuffer);
				DERPNET_ASSERT(InBuffers[2].pvBuffer == Net->Buffer + Net->BufferSize + InBuffers[0].cbBuffer + InBuffers[1].cbBuffer);

				if (InBuffers[3].BufferType == SECBUFFER_EXTRA)
				{
					DERPNET_ASSERT(InBuffers[3].pvBuffer == Net->Buffer + Net->BufferSize + InBuffers[0].cbBuffer + InBuffers[1].cbBuffer + StreamSizes.cbTrailer);
				}
				else if (InBuffers[3].BufferType == SECBUFFER_EMPTY)
				{
					DERPNET_ASSERT(Net->BufferSize + InBuffers[0].cbBuffer + InBuffers[1].cbBuffer + StreamSizes.cbTrailer == Net->BufferReceived);
				}

				memmove(Net->Buffer + Net->BufferSize, InBuffers[1].pvBuffer, InBuffers[1].cbBuffer);
				Net->BufferSize += InBuffers[1].cbBuffer;

				if (InBuffers[3].BufferType == SECBUFFER_EXTRA)
				{
					size_t ExtraSize = (Net->Buffer + Net->BufferReceived) - (uint8_t*)InBuffers[3].pvBuffer;
					memmove(Net->Buffer + Net->BufferSize, InBuffers[3].pvBuffer, ExtraSize);
				}
				Net->BufferReceived -= InBuffers[0].cbBuffer + StreamSizes.cbTrailer;
				
				DERPNET_LOG("TLS packet decrypted - encrypted=%lu, decrypted=%lu, BufferSize=%zu, BufferReceived=%zu", InBuffers[0].cbBuffer + InBuffers[1].cbBuffer + StreamSizes.cbTrailer, InBuffers[1].cbBuffer, Net->BufferSize, Net->BufferReceived);

				return true;
			}
			else if (SecStatus != SEC_E_INCOMPLETE_MESSAGE)
			{
				DERPNET_LOG("TLS protocol error when decrypting incoming data");
				return false;
			}
		}

		if (Net->BufferReceived == sizeof(Net->Buffer))
		{
			DERPNET_LOG("server is sending too much data instead of proper handshake?");
			return false;
		}

		DERPNET_LOG("reading more data from socket, BufferSize=%zu, BufferReceived=%zu", Net->BufferSize, Net->BufferReceived);

		fd_set ReadSet;
		FD_ZERO(&ReadSet);
		FD_SET(Net->Socket, &ReadSet);

		struct timeval TimeVal = { 0, 0 };
		int Select = select((int)(Net->Socket + 1), &ReadSet, NULL, NULL, Wait ? NULL : &TimeVal);
		if (Select < 0)
		{
			return false;
		}
		if (Select == 0)
		{
			return true;
		}

		int ReadSize = recv(Net->Socket, (char*)Net->Buffer + Net->BufferReceived, (int)(sizeof(Net->Buffer) - Net->BufferReceived), 0);
		if (ReadSize <= 0)
		{
			DERPNET_LOG("failed to read data from server, remote server disconnected?");
			return false;
		}
		Net->TotalReceived += ReadSize;
		Net->BufferReceived += ReadSize;

		DERPNET_LOG("read %d bytes from socket, BufferReceived=%zu", ReadSize, Net->BufferReceived);
		WSAResetEvent(Net->SocketEvent);
	}
#endif
}

static void DerpNet__TlsConsume(DerpNet* Net, size_t PlaintextSize)
{
	if (PlaintextSize == 0)
	{
		return;
	}

	DERPNET_ASSERT(Net->BufferSize <= Net->BufferReceived);
	DERPNET_ASSERT(PlaintextSize <= Net->BufferSize);

	memmove(Net->Buffer, Net->Buffer + PlaintextSize, Net->BufferReceived - PlaintextSize);
	Net->BufferSize -= PlaintextSize;
	Net->BufferReceived -= PlaintextSize;

	DERPNET_LOG("consumed %zu bytes from input buffer, BufferSize=%zu, BufferReceived=%zu", PlaintextSize, Net->BufferSize, Net->BufferReceived);
}

static int DerpNet__ReadFrame(DerpNet* Net, uint8_t* FrameType, uint32_t* FrameSize, bool Wait)
{
	const size_t FrameHeaderSize = 1 + 4;

	if (Wait)
	{
		for (;;)
		{
			if (Net->BufferSize < FrameHeaderSize)
			{
				if (!DerpNet__TlsRead(Net, Wait))
				{
					return -1;
				}
				continue;
			}

			*FrameType = Net->Buffer[0];
			*FrameSize = Get32BE(Net->Buffer + 1);

			if (Net->BufferSize < FrameHeaderSize + *FrameSize)
			{
				if (!DerpNet__TlsRead(Net, Wait))
				{
					return -1;
				}
				continue;
			}

			DerpNet__TlsConsume(Net, FrameHeaderSize);

			DERPNET_LOG("received frame type=%u, size=%u, BufferSize=%zu, BufferReceived=%zu", *FrameType, *FrameSize, Net->BufferSize, Net->BufferReceived);
			return 1;
		}
	}

	size_t LastBufferSize = Net->BufferSize;

	for (;;)
	{
		if (!DerpNet__TlsRead(Net, Wait))
		{
			return -1;
		}

		if (Net->BufferSize < FrameHeaderSize)
		{
			if (Net->BufferSize != LastBufferSize)
			{
				LastBufferSize = Net->BufferSize;
				continue;
			}
			return 0;
		}

		*FrameType = Net->Buffer[0];
		*FrameSize = Get32BE(Net->Buffer + 1);

		if (Net->BufferSize < FrameHeaderSize + *FrameSize)
		{
			if (Net->BufferSize != LastBufferSize)
			{
				LastBufferSize = Net->BufferSize;
				continue;
			}
			return 0;
		}

		DerpNet__TlsConsume(Net, FrameHeaderSize);

		DERPNET_LOG("received frame type=%u, size=%u, BufferSize=%zu, BufferReceived=%zu", *FrameType, *FrameSize, Net->BufferSize, Net->BufferReceived);
		return 1;
	}
}

bool DerpNet_Open(DerpNet* Net, const char* DerpServer, const DerpKey* UserSecret)
{
	CredHandle CredHandle;
	CtxtHandle CtxHandle;

	SecInvalidateHandle(&CredHandle);
	SecInvalidateHandle(&CtxHandle);

	struct addrinfo* AddrInfo = NULL;
	Net->Socket = INVALID_SOCKET;
	Net->SocketEvent = NULL;
	Net->BufferSize = Net->BufferReceived = 0;
	Net->TotalReceived = Net->TotalSent = 0;

	WSADATA SocketData;
	int SocketOk = WSAStartup(MAKEWORD(2, 2), &SocketData);
	DERPNET_ASSERT(SocketOk == 0);

	//
	// connect to DERP server
	//

	struct addrinfo AddrHints =
	{
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
	};

#if DERPNET_USE_PLAIN_HTTP
	const char* DerpServerPort = "80";
#else
	const char* DerpServerPort = "443";
#endif
	SocketOk = getaddrinfo(DerpServer, DerpServerPort, &AddrHints, &AddrInfo);
	if (SocketOk != 0)
	{
		DERPNET_LOG("cannot resolve '%s' hostname", DerpServer);
		goto error;
	}

	Net->Socket = socket(AddrInfo->ai_family, AddrInfo->ai_socktype, AddrInfo->ai_protocol);
	DERPNET_ASSERT(Net->Socket != INVALID_SOCKET);

	SocketOk = connect(Net->Socket, AddrInfo->ai_addr, (int)AddrInfo->ai_addrlen);
	if (SocketOk != 0)
	{
		DERPNET_LOG("cannot connect to '%s' server", DerpServer);
		goto error;
	}

#if !defined(NDEBUG)
	char Address[128];
	DWORD AddressLength = ARRAYSIZE(Address);
	WSAAddressToStringA(AddrInfo->ai_addr, (DWORD)AddrInfo->ai_addrlen, NULL, Address, &AddressLength);
	DERPNET_LOG("connected to '%s' -> '%s' server", DerpServer, Address);
#endif

	freeaddrinfo(AddrInfo);
	AddrInfo = NULL;

#if !DERPNET_USE_PLAIN_HTTP
	if (!DerpNet__TlsHandshake(Net, DerpServer, &CredHandle, &CtxHandle))
	{
		goto error;
	}

	DERPNET_ASSERT(sizeof(CredHandle) == sizeof(Net->CredHandle));
	DERPNET_ASSERT(sizeof(CtxHandle) == sizeof(Net->CtxHandle));
	memcpy(&Net->CredHandle, &CredHandle, sizeof(CredHandle));
	memcpy(&Net->CtxHandle, &CtxHandle, sizeof(CtxHandle));
#endif

	Net->SocketEvent = WSACreateEvent();
	DERPNET_ASSERT(Net->SocketEvent);

	WSAEventSelect(Net->Socket, Net->SocketEvent, FD_READ);

	//
	// send inital HTTP GET request, ask to switch to DERP protocol immediately
	//

	char HttpInit[256];
	int HttpInitLen = snprintf(HttpInit, sizeof(HttpInit),
		"GET /derp HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Connection: Upgrade\r\n"
		"Upgrade: DERP\r\n"
		"Derp-Fast-Start: 1\r\n"
		"\r\n",
		DerpServer);

	if (!DerpNet__TlsWrite(Net, HttpInit, HttpInitLen))
	{
		goto error;
	}

	//
	// now DERP protocol follows
	//

	uint8_t FrameType;
	uint32_t FrameSize;
	memset(Net->LastPublicKey, 0, sizeof(Net->LastPublicKey));

	//
	// receive ServerKey frame
	//

	uint8_t ServerPublicKey[32];

	{
		if (DerpNet__ReadFrame(Net, &FrameType, &FrameSize, true) < 0)
		{
			goto error;
		}

		DERPNET_ASSERT(FrameType == 1); // ServerKey
		DERPNET_ASSERT(FrameSize >= 8 + 32);

		static const uint8_t DerpMagic[8] = { 0x44, 0x45, 0x52, 0x50, 0xf0, 0x9f, 0x94, 0x91 };

		const uint8_t* Magic = Net->Buffer;
		DERPNET_ASSERT(memcmp(Magic, DerpMagic, sizeof(DerpMagic)) == 0);

		memcpy(ServerPublicKey, Net->Buffer + 8, sizeof(ServerPublicKey));

		DerpNet__TlsConsume(Net, FrameSize);
	}

	// calculate user public key to send to server

	DerpKey UserPublicKey;
	DerpNet_GetPublicKey(UserSecret, &UserPublicKey);

	//
	// send ClientInfo frame
	//

	{
		static const char ClientInfo[] = "{\"version\": 2}";

		uint8_t OutFrame[1 + 4 + 32 + 24 + 16 + sizeof(ClientInfo) - 1];

		OutFrame[0] = 2; // ClientInfo
		Set32BE(OutFrame + 1, sizeof(OutFrame) - (1 + 4));
		memcpy(OutFrame + 1 + 4, UserPublicKey.Bytes, sizeof(UserPublicKey.Bytes));
		DerpNet__BoxSeal(OutFrame + 1 + 4 + 32, OutFrame + 1 + 4 + 32 + 24, OutFrame + 1 + 4 + 32 + 24 + 16, (uint8_t*)ClientInfo, sizeof(ClientInfo) - 1, UserSecret->Bytes, ServerPublicKey);

		if (!DerpNet__TlsWrite(Net, OutFrame, sizeof(OutFrame)))
		{
			goto error;
		}
	}

	//
	// receive ServerInfo frame
	//

	{
		if (DerpNet__ReadFrame(Net, &FrameType, &FrameSize, true) < 0)
		{
			goto error;
		}

		DERPNET_ASSERT(FrameType == 3); // ServerInfo
		DERPNET_ASSERT(FrameSize >= 24 + 16);

		uint8_t* Nonce = Net->Buffer;
		uint8_t* Auth = Nonce + 24;
		uint8_t* Data = Auth + 16;

		size_t DataSize = FrameSize - (24 + 16);
		bool UnsealOk = DerpNet__BoxUnseal(Data, Data, DataSize, Auth, Nonce, UserSecret->Bytes, ServerPublicKey);
		if (!UnsealOk)
		{
			DERPNET_LOG("nacl box unseal for ServerInfo frame failed");
			goto error;
		}

		DerpNet__TlsConsume(Net, FrameSize);
	}

	//
	// ready!
	//

	memcpy(Net->UserPrivateKey, UserSecret->Bytes, sizeof(Net->UserPrivateKey));

	Net->LastFrameSize = 0;
	return true;

error:
#if !DERPNET_USE_PLAIN_HTTP
	if (SecIsValidHandle(&CtxHandle))
	{
		DeleteSecurityContext(&CtxHandle);
	}
	if (SecIsValidHandle(&CredHandle))
	{
		FreeCredentialsHandle(&CredHandle);
	}
#endif
	if (Net->SocketEvent)
	{
		WSACloseEvent(Net->SocketEvent);
	}
	if (Net->Socket != INVALID_SOCKET)
	{
		closesocket(Net->Socket);
	}
	if (AddrInfo)
	{
		freeaddrinfo(AddrInfo);
	}
	WSACleanup();

	return false;
}

void DerpNet_Close(DerpNet* Net)
{
#if !DERPNET_USE_PLAIN_HTTP
	DeleteSecurityContext((CtxtHandle*)Net->CtxHandle);
	FreeCredentialsHandle((CredHandle*)Net->CredHandle);
#endif
	WSACloseEvent(Net->SocketEvent);
	closesocket(Net->Socket);
	WSACleanup();
}

int DerpNet_Recv(DerpNet* Net, DerpKey* ReceivedUserPublicKey, uint8_t** ReceivedData, uint32_t* ReceivedSize, bool Wait)
{
	DerpNet__TlsConsume(Net, Net->LastFrameSize);
	Net->LastFrameSize = 0;

	for (;;)
	{
		uint8_t FrameType;
		uint32_t FrameSize;

		int GotFrame = DerpNet__ReadFrame(Net, &FrameType, &FrameSize, Wait);
		if (GotFrame < 0)
		{
			DERPNET_LOG("disconnecting in Recv");
			return -1;
		}

		if (GotFrame == 0)
		{
			return 0;
		}

		if (FrameType == 5) // RecvPacket
		{
			if (FrameSize >= 32 + 24 + 16)
			{
				uint8_t* PublicKey = Net->Buffer;
				uint8_t* Nonce = PublicKey + 32;
				uint8_t* Auth = Nonce + 24;
				uint8_t* Data = Auth + 16;
				uint32_t DataSize = FrameSize - (32 + 24 + 16);

				if (memcmp(PublicKey, Net->LastPublicKey, sizeof(Net->LastPublicKey)) != 0)
				{
					DerpNet__GetSharedKey(Net->LastSharedKey, Net->UserPrivateKey, PublicKey);
					memcpy(Net->LastPublicKey, PublicKey, sizeof(Net->LastPublicKey));
				}

				bool UnsealOk = DerpNet__BoxUnsealEx(Data, Data, DataSize, Auth, Nonce, Net->LastSharedKey);
				if (UnsealOk)
				{
					memcpy(ReceivedUserPublicKey->Bytes, PublicKey, sizeof(ReceivedUserPublicKey->Bytes));
					*ReceivedData = Data;
					*ReceivedSize = DataSize;

					Net->LastFrameSize = FrameSize;

					return 1;
				}
				else
				{
					DERPNET_LOG("failed to verify encrypted data");
				}
			}
			else
			{
				DERPNET_LOG("RecvPacket frame too short, expected at least %u bytes, got %u", 32 + 24 + 16, FrameSize);
			}
		}
		else
		{
			DERPNET_LOG("unknown frame, ignoring");
		}

		DerpNet__TlsConsume(Net, FrameSize);
	}

	return -1;
}

bool DerpNet_Send(DerpNet* Net, const DerpKey* TargetUserPublicKey, const void* Data, size_t DataSize)
{
	if (memcmp(TargetUserPublicKey->Bytes, Net->LastPublicKey, sizeof(Net->LastPublicKey)) != 0)
	{
		DerpNet__GetSharedKey(Net->LastSharedKey, Net->UserPrivateKey, TargetUserPublicKey->Bytes);
		memcpy(Net->LastPublicKey, TargetUserPublicKey->Bytes, sizeof(Net->LastPublicKey));
	}

	uint8_t Nonce[24];
	DerpNet__GetRandom(Nonce, sizeof(Nonce));

	return DerpNet_SendEx(Net, TargetUserPublicKey, Net->LastSharedKey, Nonce, Data, DataSize);
}

bool DerpNet_SendEx(DerpNet* Net, const DerpKey* TargetUserPublicKey, const uint8_t SharedKey[32], const uint8_t InNonce[24], const void* Data, size_t DataSize)
{
	uint8_t OutFrame[1 << 16];

	size_t OutFrameSize = 1 + 4 + 32 + 24 + 16 + DataSize;
	DERPNET_ASSERT(OutFrameSize <= sizeof(OutFrame));

	OutFrame[0] = 4; // SendPacket
	Set32BE(OutFrame + 1, (uint32_t)(OutFrameSize - (1 + 4)));

	uint8_t* PublicKey = OutFrame + 1 + 4;
	uint8_t* Nonce = PublicKey + 32;
	uint8_t* Auth = Nonce + 24;
	uint8_t* Output = Auth + 16;

	memcpy(PublicKey, TargetUserPublicKey->Bytes, sizeof(TargetUserPublicKey->Bytes));
	memcpy(Nonce, InNonce, 24);

	DerpNet__BoxSealEx(Nonce, Auth, Output, (uint8_t*)Data, DataSize, SharedKey);

	return DerpNet__TlsWrite(Net, OutFrame, OutFrameSize);
}

#endif // defined(DERP_STATIC) || defined(DERP_IMPLEMENTATION)
