/* ***** BEGIN LICENSE BLOCK *****  
 * Source last modified: $Id: assembly.h,v 1.2 2005/05/11 16:00:58 albertofloyd Exp $ 
 *   
 * Portions Copyright (c) 1995-2005 RealNetworks, Inc. All Rights Reserved.  
 *       
 * The contents of this file, and the files included with this file, 
 * are subject to the current version of the RealNetworks Public 
 * Source License (the "RPSL") available at 
 * http://www.helixcommunity.org/content/rpsl unless you have licensed 
 * the file under the current version of the RealNetworks Community 
 * Source License (the "RCSL") available at 
 * http://www.helixcommunity.org/content/rcsl, in which case the RCSL 
 * will apply. You may also obtain the license terms directly from 
 * RealNetworks.  You may not use this file except in compliance with 
 * the RPSL or, if you have a valid RCSL with RealNetworks applicable 
 * to this file, the RCSL.  Please see the applicable RPSL or RCSL for 
 * the rights, obligations and limitations governing use of the 
 * contents of the file. 
 *   
 * This file is part of the Helix DNA Technology. RealNetworks is the 
 * developer of the Original Code and owns the copyrights in the 
 * portions it created. 
 *   
 * This file, and the files included with this file, is distributed 
 * and made available on an 'AS IS' basis, WITHOUT WARRANTY OF ANY 
 * KIND, EITHER EXPRESS OR IMPLIED, AND REALNETWORKS HEREBY DISCLAIMS 
 * ALL SUCH WARRANTIES, INCLUDING WITHOUT LIMITATION, ANY WARRANTIES 
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, QUIET 
 * ENJOYMENT OR NON-INFRINGEMENT. 
 *  
 * Technology Compatibility Kit Test Suite(s) Location:  
 *    http://www.helixcommunity.org/content/tck  
 *  
 * Contributor(s):  
 *   
 * ***** END LICENSE BLOCK ***** */  

/**************************************************************************************
 * Fixed-point HE-AAC decoder
 * Jon Recker (jrecker@real.com)
 * February 2005
 *
 * assembly.h - inline assembly language functions and prototypes
 *
 * MULSHIFT32(x, y) 		signed multiply of two 32-bit integers (x and y), 
 *                            returns top 32-bits of 64-bit result
 * CLIPTOSHORT(x)			convert 32-bit integer to 16-bit short, 
 *                            clipping to [-32768, 32767]
 * FASTABS(x)               branchless absolute value of signed integer x
 * CLZ(x)                   count leading zeros on signed integer x
 * MADD64(sum64, x, y)		64-bit multiply accumulate: sum64 += (x*y)
 **************************************************************************************/

#ifndef _ASSEMBLY_H
#define _ASSEMBLY_H

/* toolchain:           MSFT Visual C++
 * target architecture: x86
 */
#if (defined (_WIN32) && !defined (_WIN32_WCE)) || (defined (__WINS__) && defined (_SYMBIAN)) || (defined (WINCE_EMULATOR)) || (defined (_OPENWAVE_SIMULATOR))

#pragma warning( disable : 4035 )	/* complains about inline asm not returning a value */

static __inline int MULSHIFT32(int x, int y)	
{
    __asm {
		mov		eax, x
	    imul	y
	    mov		eax, edx
	    }
}

static __inline short CLIPTOSHORT(int x)
{
	int sign;

	/* clip to [-32768, 32767] */
	sign = x >> 31;
	if (sign != (x >> 15))
		x = sign ^ ((1 << 15) - 1);

	return (short)x;
}

static __inline int FASTABS(int x) 
{
	int sign;

	sign = x >> (sizeof(int) * 8 - 1);
	x ^= sign;
	x -= sign;

	return x;
}

static __inline int CLZ(int x)
{
	int numZeros;

	if (!x)
		return 32;

	/* count leading zeros with binary search */
	numZeros = 1;
	if (!((unsigned int)x >> 16))	{ numZeros += 16; x <<= 16; }
	if (!((unsigned int)x >> 24))	{ numZeros +=  8; x <<=  8; }
	if (!((unsigned int)x >> 28))	{ numZeros +=  4; x <<=  4; }
	if (!((unsigned int)x >> 30))	{ numZeros +=  2; x <<=  2; }

	numZeros -= ((unsigned int)x >> 31);

	return numZeros;
}

#ifdef __CW32__
typedef long long Word64;
#else
typedef __int64 Word64;
#endif

typedef union _U64 {
	Word64 w64;
	struct {
		/* x86 = little endian */
		unsigned int lo32; 
		signed int   hi32;
	} r;
} U64;

/* returns 64-bit value in [edx:eax] */
static __inline Word64 MADD64(Word64 sum64, int x, int y)
{
	U64 u;
	u.w64 = sum64;

	sum64 += (Word64)x * (Word64)y;
	return sum64;

/* asm version
 * 
 *	__asm mov	eax, x
 *	__asm imul	y
 *	__asm add   eax, u.r.lo32
 *	__asm adc   edx, u.r.hi32
 */
}

/* toolchain:           MSFT Embedded Visual C++
 * target architecture: ARM v.4 and above (require 'M' type processor for 32x32->64 multiplier)
 */
#elif defined (_WIN32) && defined (_WIN32_WCE) && defined (ARM)

static __inline short CLIPTOSHORT(int x)
{
	int sign;

	/* clip to [-32768, 32767] */
	sign = x >> 31;
	if (sign != (x >> 15))
		x = sign ^ ((1 << 15) - 1);

	return (short)x;
}

static __inline int FASTABS(int x) 
{
	int sign;

	sign = x >> (sizeof(int) * 8 - 1);
	x ^= sign;
	x -= sign;

	return x;
}

static __inline int CLZ(int x)
{
	int numZeros;

	if (!x)
		return 32;

	/* count leading zeros with binary search (function should be 17 ARM instructions total) */
	numZeros = 1;
	if (!((unsigned int)x >> 16))	{ numZeros += 16; x <<= 16; }
	if (!((unsigned int)x >> 24))	{ numZeros +=  8; x <<=  8; }
	if (!((unsigned int)x >> 28))	{ numZeros +=  4; x <<=  4; }
	if (!((unsigned int)x >> 30))	{ numZeros +=  2; x <<=  2; }

	numZeros -= ((unsigned int)x >> 31);

	return numZeros;
}

/* implemented in asmfunc.s */
#ifdef __cplusplus
extern "C" {
#endif

typedef __int64 Word64;

typedef union _U64 {
	Word64 w64;
	struct {
		/* ARM WinCE = little endian */
		unsigned int lo32; 
		signed int   hi32;
	} r;
} U64;

/* manual name mangling for just this platform (must match labels in .s file) */
#define MULSHIFT32	raac_MULSHIFT32
#define MADD64		raac_MADD64

int MULSHIFT32(int x, int y);
Word64 MADD64(Word64 sum64, int x, int y);

#ifdef __cplusplus
}
#endif

/* toolchain:           ARM ADS or RealView
 * target architecture: ARM v.4 and above (requires 'M' type processor for 32x32->64 multiplier)
 */
#elif defined (__arm) && defined (__ARMCC_VERSION)

static __inline int MULSHIFT32(int x, int y)
{
    /* rules for smull RdLo, RdHi, Rm, Rs:
     *   RdHi != Rm 
     *   RdLo != Rm 
     *   RdHi != RdLo
     */
    int zlow;
    __asm {
    	smull zlow,y,x,y
   	}

    return y;
}

static __inline short CLIPTOSHORT(int x)
{
	int sign;

	/* clip to [-32768, 32767] */
	sign = x >> 31;
	if (sign != (x >> 15))
		x = sign ^ ((1 << 15) - 1);

	return (short)x;
}

static __inline int FASTABS(int x) 
{
	int sign;

	sign = x >> (sizeof(int) * 8 - 1);
	x ^= sign;
	x -= sign;

	return x;
}

static __inline int CLZ(int x)
{
	int numZeros;

	if (!x)
		return 32;

	/* count leading zeros with binary search (function should be 17 ARM instructions total) */
	numZeros = 1;
	if (!((unsigned int)x >> 16))	{ numZeros += 16; x <<= 16; }
	if (!((unsigned int)x >> 24))	{ numZeros +=  8; x <<=  8; }
	if (!((unsigned int)x >> 28))	{ numZeros +=  4; x <<=  4; }
	if (!((unsigned int)x >> 30))	{ numZeros +=  2; x <<=  2; }

	numZeros -= ((unsigned int)x >> 31);

	return numZeros;

/* ARM code would look like this, but do NOT use inline asm in ADS for this,
   because you can't safely use the status register flags intermixed with C code 
 
	__asm {
	    mov		numZeros, #1
		tst		x, 0xffff0000
		addeq	numZeros, numZeros, #16
		moveq	x, x, lsl #16
		tst		x, 0xff000000
		addeq	numZeros, numZeros, #8
		moveq	x, x, lsl #8
		tst		x, 0xf0000000
		addeq	numZeros, numZeros, #4
		moveq	x, x, lsl #4
		tst		x, 0xc0000000
		addeq	numZeros, numZeros, #2
		moveq	x, x, lsl #2
		sub		numZeros, numZeros, x, lsr #31
	}
*/
/* reference:
	numZeros = 0;
	while (!(x & 0x80000000)) {
		numZeros++;
		x <<= 1;
	} 
*/
}

typedef __int64 Word64;

typedef union _U64 {
	Word64 w64;
	struct {
		/* ARM ADS = little endian */
		unsigned int lo32; 
		signed int   hi32;
	} r;
} U64;

static __inline Word64 MADD64(Word64 sum64, int x, int y) 
{
	U64 u;
	u.w64 = sum64;
	
	__asm {
    	smlal u.r.lo32, u.r.hi32, x, y 
	}

	return u.w64;
}

/* toolchain:           ARM gcc
 * target architecture: ARM v.4 and above (requires 'M' type processor for 32x32->64 multiplier)
 */
#elif defined(__GNUC__) && defined(__arm__)

static __inline__ int MULSHIFT32(int x, int y)
{
    int zlow;
    __asm__ volatile ("smull %0,%1,%2,%3" : "=&r" (zlow), "=r" (y) : "r" (x), "1" (y) : "cc");
    return y;
}

static __inline short CLIPTOSHORT(int x)
{
	int sign;

	/* clip to [-32768, 32767] */
	sign = x >> 31;
	if (sign != (x >> 15))
		x = sign ^ ((1 << 15) - 1);

	return (short)x;
}

static __inline int FASTABS(int x) 
{
	int sign;

	sign = x >> (sizeof(int) * 8 - 1);
	x ^= sign;
	x -= sign;

	return x;
}

static __inline int CLZ(int x)
{
	int numZeros;

	if (!x)
		return (sizeof(int) * 8);

	numZeros = 0;
	while (!(x & 0x80000000)) {
		numZeros++;
		x <<= 1;
	} 

	return numZeros;
}

typedef long long Word64;

typedef union _U64 {
	Word64 w64;
	struct {
		/* ARM ADS = little endian */
		unsigned int lo32;
		signed int   hi32;
	} r;
} U64;

static __inline Word64 MADD64(Word64 sum64, int x, int y)
{
	U64 u;
	u.w64 = sum64;
	
	__asm__ volatile ("smlal %0,%1,%2,%3" : "+&r" (u.r.lo32), "+&r" (u.r.hi32) : "r" (x), "r" (y) : "cc");
	
	return u.w64;
}

/* toolchain:           x86 gcc
 * target architecture: x86
 */
#elif defined(__GNUC__) && defined(__i386__)

typedef long long Word64;

static __inline__ int MULSHIFT32(int x, int y)
{
    int z;

    z = (Word64)x * (Word64)y >> 32;
    
	return z;
}

static __inline short CLIPTOSHORT(int x)
{
	int sign;

	/* clip to [-32768, 32767] */
	sign = x >> 31;
	if (sign != (x >> 15))
		x = sign ^ ((1 << 15) - 1);

	return (short)x;
}

static __inline int FASTABS(int x) 
{
	int sign;

	sign = x >> (sizeof(int) * 8 - 1);
	x ^= sign;
	x -= sign;

	return x;
}

static __inline int CLZ(int x)
{
	int numZeros;

	if (!x)
		return 32;

	/* count leading zeros with binary search (function should be 17 ARM instructions total) */
	numZeros = 1;
	if (!((unsigned int)x >> 16))	{ numZeros += 16; x <<= 16; }
	if (!((unsigned int)x >> 24))	{ numZeros +=  8; x <<=  8; }
	if (!((unsigned int)x >> 28))	{ numZeros +=  4; x <<=  4; }
	if (!((unsigned int)x >> 30))	{ numZeros +=  2; x <<=  2; }

	numZeros -= ((unsigned int)x >> 31);

	return numZeros;
}

typedef union _U64 {
	Word64 w64;
	struct {
		/* x86 = little endian */
		unsigned int lo32;
		signed int   hi32;
	} r;
} U64;

static __inline Word64 MADD64(Word64 sum64, int x, int y)
{
	sum64 += (Word64)x * (Word64)y;

	return sum64;
}
#elif defined (__GNUC__) && defined(__ALLEGREX__)

typedef long long Word64;

static __inline__ int MULSHIFT32(int x, int y)
{
    int z;

    z = (Word64)x * (Word64)y >> 32;
    
	return z;
}

static __inline short CLIPTOSHORT(int x)
{
	int sign;

	/* clip to [-32768, 32767] */
	sign = x >> 31;
	if (sign != (x >> 15))
		x = sign ^ ((1 << 15) - 1);

	return (short)x;
}

static __inline int FASTABS(int x) 
{
	int sign;

	sign = x >> (sizeof(int) * 8 - 1);
	x ^= sign;
	x -= sign;

	return x;
}

static __inline int CLZ(int x)
{
	int numZeros;

	if (!x)
		return 32;

	/* count leading zeros with binary search (function should be 17 ARM instructions total) */
	numZeros = 1;
	if (!((unsigned int)x >> 16))	{ numZeros += 16; x <<= 16; }
	if (!((unsigned int)x >> 24))	{ numZeros +=  8; x <<=  8; }
	if (!((unsigned int)x >> 28))	{ numZeros +=  4; x <<=  4; }
	if (!((unsigned int)x >> 30))	{ numZeros +=  2; x <<=  2; }

	numZeros -= ((unsigned int)x >> 31);

	return numZeros;
}

typedef union _U64 {
	Word64 w64;
	struct {
		/* x86 = little endian */
		unsigned int lo32;
		signed int   hi32;
	} r;
} U64;

static __inline Word64 MADD64(Word64 sum64, int x, int y)
{
	sum64 += (Word64)x * (Word64)y;

	return sum64;
}
#else

#error Unsupported platform in assembly.h

#endif	/* platforms */

#endif /* _ASSEMBLY_H */
