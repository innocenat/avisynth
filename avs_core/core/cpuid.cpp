// Avisynth v1.0 beta.  Copyright 2000 Ben Rudiak-Gould.
// http://www.math.berkeley.edu/~benrg/avisynth.html

//	VirtualDub - Video processing and capture application
//	Copyright (C) 1998-2000 Avery Lee
//
//	This program is free software; you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation; either version 2 of the License, or
//	(at your option) any later version.
//
//	This program is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License
//	along with this program; if not, write to the Free Software
//	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

#include <avs/cpuid.h>
#include <avs/config.h>

// CPUID instruction
#if defined(AVS_MSVC) || defined(AVS_ICL)
#include <intrin.h>
#elif defined(AVS_ATT_ASSEMBLY)

// Those functions are written to MSVC's internal function
// as GCC's cpuid.h doesn't have __cpuidx which is required
// for getting data in higher leaves
// (e.g. AVX2 support which require eax=7 ecx=0 to cpuid to check)

inline void __cpuid(int* cpuinfo, unsigned function_id) {
  asm volatile (
    "cpuid"
    : "=a" (cpuinfo[0]), "=b" (cpuinfo[1]), "=c" (cpuinfo[2]), "=d" (cpuinfo[3])
    : "a" (function_id)
    :
  );
}

inline void __cpuidx(int* cpuinfo, unsigned function_id, unsigned subfunction_id) {
  asm volatile (
    "cpuid"
    : "=a" (cpuinfo[0]), "=b" (cpuinfo[1]), "=c" (cpuinfo[2]), "=d" (cpuinfo[3])
    : "a" (function_id), "c" (subfunction_id)
    :
  );
}

#else
#error  Error: cannot get CPUID.
#endif

// xgetbv instruction
#if defined(AVS_ICL) || (defined(AVS_MSVC) && (_MSC_FULL_VER >= 160040219))   // We require VC++2010 SP1 at least
// Do nothing
#elif defined(AVS_ATT_ASSEMBLY)
static inline unsigned long long _xgetbv(unsigned int index){
  unsigned int eax, edx;
  asm volatile (
    "xgetbv"
    : "=a" (eax), "=d"(edx)
    : "c" (index)
    :
  );
  return ((unsigned long long) edx << 32) | eax;
}
#else
#   defined _xgetbv(x) 0
#endif

#ifndef _XCR_XFEATURE_ENABLED_MASK
#   define _XCR_XFEATURE_ENABLED_MASK 0
#endif

#define IS_BIT_SET(bitfield, bit) ((bitfield) & (1<<(bit)) ? true : false)

static int CPUCheckForExtensions()
{
  int result = 0;
  int cpuinfo[4];

  __cpuid(cpuinfo, 1);
  if (IS_BIT_SET(cpuinfo[3], 0))
    result |= CPUF_FPU;
  if (IS_BIT_SET(cpuinfo[3], 23))
    result |= CPUF_MMX;
  if (IS_BIT_SET(cpuinfo[3], 25))
    result |= CPUF_SSE | CPUF_INTEGER_SSE;
  if (IS_BIT_SET(cpuinfo[3], 26))
    result |= CPUF_SSE2;
  if (IS_BIT_SET(cpuinfo[2], 0))
    result |= CPUF_SSE3;
  if (IS_BIT_SET(cpuinfo[2], 9))
    result |= CPUF_SSSE3;
  if (IS_BIT_SET(cpuinfo[2], 19))
    result |= CPUF_SSE4_1;
  if (IS_BIT_SET(cpuinfo[2], 20))
    result |= CPUF_SSE4_2;

  // AVX
  bool xgetbv_supported = IS_BIT_SET(cpuinfo[2], 27);
  bool avx_supported = IS_BIT_SET(cpuinfo[2], 28);
  if (xgetbv_supported && avx_supported)
  {
    if ((_xgetbv(_XCR_XFEATURE_ENABLED_MASK) & 0x6ull) == 0x6ull)
      result |= CPUF_AVX;   
  }

  // 3DNow!, 3DNow!, and ISSE
  __cpuid(cpuinfo, 0x80000000);   
  if (cpuinfo[0] >= 0x80000001) 
  {
    __cpuid(cpuinfo, 0x80000001);   
   
    if (IS_BIT_SET(cpuinfo[3], 31))
      result |= CPUF_3DNOW;   
   
    if (IS_BIT_SET(cpuinfo[3], 30))
      result |= CPUF_3DNOW_EXT;   
   
    if (IS_BIT_SET(cpuinfo[3], 22))
      result |= CPUF_INTEGER_SSE;   
  }

  return result;
}

int GetCPUFlags() {
  static int lCPUExtensionsAvailable = CPUCheckForExtensions();
  return lCPUExtensionsAvailable;
}
