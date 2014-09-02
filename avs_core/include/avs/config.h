// Avisynth C Interface Version 0.20
// Copyright 2003 Kevin Atkinson

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA, or visit
// http://www.gnu.org/copyleft/gpl.html .
//
// As a special exception, I give you permission to link to the
// Avisynth C interface with independent modules that communicate with
// the Avisynth C interface solely through the interfaces defined in
// avisynth_c.h, regardless of the license terms of these independent
// modules, and to copy and distribute the resulting combined work
// under terms of your choice, provided that every copy of the
// combined work is accompanied by a complete copy of the source code
// of the Avisynth C interface and Avisynth itself (with the version
// used to produce the combined work), being distributed under the
// terms of the GNU General Public License plus this exception.  An
// independent module is a module which is not derived from or based
// on Avisynth C Interface, such as 3rd-party filters, import and
// export plugins, or graphical user interfaces.

#ifndef AVS_CONFIG_H
#define AVS_CONFIG_H

// Undefine this to get cdecl calling convention
#define AVSC_USE_STDCALL 1

// NOTE TO PLUGIN AUTHORS:
// Because FRAME_ALIGN can be substantially higher than the alignment 
// a plugin actually needs, plugins should not use FRAME_ALIGN to check for
// alignment. They should always request the exact alignment value they need.
// This is to make sure that plugins work over the widest range of AviSynth
// builds possible.
#define FRAME_ALIGN 32

// Platform Architecture Flag
#if   defined(_M_AMD64) || defined(__x86_64)
#   define X86_64
#elif defined(_M_IX86) || defined(__i386__)
#   define X86_32
#else
#   error Unsupported CPU architecture.
#endif

// Operating System Flag
#ifdef _WIN32
#   define AVS_WINDOWS
#elif  __linux__
#   define AVS_LINUX
#else
#   error Operating system unsupported.
#endif

// Compiler Flag
#if   defined(__INTEL_COMPILER)
#   define AVS_ICL
#elif defined(__clang__)
#   define AVS_CLANG
#elif defined(__GNUC__)
#   define AVS_GCC
#elif defined(_MSC_VER)
#   define AVS_MSVC
#endif

// Inline assembly
#if defined(AVS_ICL) || (defined(AVS_MSVC) && defined(X86_32))
#   define AVS_INTEL_ASSEMBLY
#elif defined(AVS_CLANG) || defined(AVS_GCC)
#   define AVS_ATT_ASSEMBLY
#endif

#ifdef AVS_GCC
#define __single_inheritance
#define _aligned_malloc(a,b) malloc(a)
#define _aligned_free        free
#define _strcmpi             strcmp
#define _stdcall __stdcall
#define _cdecl __cdecl
#define __forceinline  inline

#include <stdint.h>
typedef int64_t INT64;
#ifdef __int64
#undef __int64
#endif
typedef int64_t __int64;

// TODO: I am not even sure what these do
#define _control87(x,y) 0
#define _clear87() 0

#define _ASSERT(x)

#endif

#endif //AVS_CONFIG_H

