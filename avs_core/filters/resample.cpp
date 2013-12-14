// Avisynth v2.5.  Copyright 2002 Ben Rudiak-Gould et al.
// http://www.avisynth.org

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
// Linking Avisynth statically or dynamically with other modules is making a
// combined work based on Avisynth.  Thus, the terms and conditions of the GNU
// General Public License cover the whole combination.
//
// As a special exception, the copyright holders of Avisynth give you
// permission to link Avisynth with independent modules that communicate with
// Avisynth solely through the interfaces defined in avisynth.h, regardless of the license
// terms of these independent modules, and to copy and distribute the
// resulting combined work under terms of your choice, provided that
// every copy of the combined work is accompanied by a complete copy of
// the source code of Avisynth (the version of Avisynth used to produce the
// combined work), being distributed under the terms of the GNU General
// Public License plus this exception.  An independent module is a module
// which is not derived from or based on Avisynth, such as 3rd-party filters,
// import and export plugins, or graphical user interfaces.

#include "resample.h"
#include <malloc.h>
#include <avs/config.h>
#include "../core/internal.h"

#include "transform.h"
#include "turn.h"
#include <avs/alignment.h>
#include "../convert/convert_planar.h"
#include "../convert/convert_yuy2.h"


// Intrinsics for SSE4.1, SSSE3, SSE3, SSE2, ISSE and MMX
#include <smmintrin.h>

__forceinline static void allocate_frame_buffer(BYTE*& ptr, int& pitch, int w, int h)
{
  pitch = AlignNumber(w, 64); // alignment and pitch etc.
  ptr = (BYTE*) _aligned_malloc(pitch*h, 64);
}

/***************************************
 ********* Templated SSE Loader ********
 ***************************************/

typedef __m128i (SSELoader)(const __m128i*);

__forceinline __m128i simd_load_aligned(const __m128i* adr)
{
  return _mm_load_si128(adr);
}

__forceinline __m128i simd_load_unaligned(const __m128i* adr)
{
  return _mm_loadu_si128(adr);
}

__forceinline __m128i simd_load_unaligned_sse3(const __m128i* adr)
{
  return _mm_lddqu_si128(adr);
}

__forceinline __m128i simd_load_streaming(const __m128i* adr)
{
  return _mm_stream_load_si128(const_cast<__m128i*>(adr));
}

/***************************************
 ***** Vertical Resizer Assembly *******
 ***************************************/

static void resize_v_planar_pointresize(BYTE* dst, const BYTE* src, int dst_pitch, int src_pitch, ResamplingProgram* program, int width, int target_height, const int* pitch_table, const void* storage)
{
  for (int y = 0; y < target_height; y++) {
    int offset = program->pixel_offset[y];
    const BYTE* src_ptr = src + pitch_table[offset];

    memcpy(dst, src_ptr, width);

    dst += dst_pitch;
  }
}

template<int f_size>
static void resize_v_c_planar(BYTE* dst, const BYTE* src, int dst_pitch, int src_pitch, ResamplingProgram* program, int width, int target_height, const int* pitch_table, const void* storage)
{
  if (f_size != program->filter_size) {
    // Internal error?
  }

  int filter_size = f_size;
  if (filter_size == 0) {
    filter_size = program->filter_size;
  }

  short* current_coeff = program->pixel_coefficient;

  for (int y = 0; y < target_height; y++) {
    int offset = program->pixel_offset[y];
    const BYTE* src_ptr = src + pitch_table[offset];

    for (int x = 0; x < width; x++) {
      int result = 0;
      for (int i = 0; i < filter_size; i++) {
        result += (src_ptr+pitch_table[i])[x] * current_coeff[i];
      }
      result = ((result+8192)/16384);
      result = result > 255 ? 255 : result < 0 ? 0 : result;
      dst[x] = (BYTE) result;
    }

    dst += dst_pitch;
    current_coeff += filter_size;
  }
}

#ifdef X86_32
template<int f_size>
static void resize_v_mmx_planar(BYTE* dst, const BYTE* src, int dst_pitch, int src_pitch, ResamplingProgram* program, int width, int target_height, const int* pitch_table, const void* storage)
{
  if (f_size != program->filter_size) {
    // Internal error?
  }

  int filter_size = f_size;
  if (filter_size == 0) {
    filter_size = program->filter_size;
  }

  short* current_coeff = program->pixel_coefficient;

  int wMod8 = (width / 8) * 8;
  int sizeMod2 = (filter_size/2) * 2;
  bool notMod2 = sizeMod2 < filter_size;

  __m64 zero = _mm_setzero_si64();

  for (int y = 0; y < target_height; y++) {
    int offset = program->pixel_offset[y];
    const BYTE* src_ptr = src + pitch_table[offset];

    for (int x = 0; x < wMod8; x += 8) {
      __m64 result_1 = _mm_set1_pi32(8192); // Init. with rounder (16384/2 = 8192)
      __m64 result_2 = result_1;
      __m64 result_3 = result_1;
      __m64 result_4 = result_1;

      for (int i = 0; i < sizeMod2; i += 2) {
        __m64 src_p1 = *(reinterpret_cast<const __m64*>(src_ptr+pitch_table[i]+x));   // For detailed explanation please see SSE2 version.
        __m64 src_p2 = *(reinterpret_cast<const __m64*>(src_ptr+pitch_table[i+1]+x));

        __m64 src_l = _mm_unpacklo_pi8(src_p1, src_p2);                                   
        __m64 src_h = _mm_unpackhi_pi8(src_p1, src_p2);                                   

        __m64 src_1 = _mm_unpacklo_pi8(src_l, zero);                                      
        __m64 src_2 = _mm_unpackhi_pi8(src_l, zero);                                      
        __m64 src_3 = _mm_unpacklo_pi8(src_h, zero);                                      
        __m64 src_4 = _mm_unpackhi_pi8(src_h, zero);                                      

        __m64 coeff = _mm_cvtsi32_si64(*reinterpret_cast<const int*>(current_coeff+i));   
        coeff = _mm_unpacklo_pi32(coeff, coeff);                                               

        __m64 dst_1 = _mm_madd_pi16(src_1, coeff);                                        
        __m64 dst_2 = _mm_madd_pi16(src_2, coeff);                                        
        __m64 dst_3 = _mm_madd_pi16(src_3, coeff);
        __m64 dst_4 = _mm_madd_pi16(src_4, coeff);

        result_1 = _mm_add_pi32(result_1, dst_1);
        result_2 = _mm_add_pi32(result_2, dst_2);
        result_3 = _mm_add_pi32(result_3, dst_3);
        result_4 = _mm_add_pi32(result_4, dst_4);
      }

      if (notMod2) { // do last odd row
        __m64 src_p = *(reinterpret_cast<const __m64*>(src_ptr+pitch_table[sizeMod2]+x));

        __m64 src_l = _mm_unpacklo_pi8(src_p, zero);
        __m64 src_h = _mm_unpackhi_pi8(src_p, zero);

        __m64 coeff = _mm_set1_pi16(current_coeff[sizeMod2]);

        __m64 dst_ll = _mm_mullo_pi16(src_l, coeff);   // Multiply by coefficient
        __m64 dst_lh = _mm_mulhi_pi16(src_l, coeff);
        __m64 dst_hl = _mm_mullo_pi16(src_h, coeff);
        __m64 dst_hh = _mm_mulhi_pi16(src_h, coeff);

        __m64 dst_1 = _mm_unpacklo_pi16(dst_ll, dst_lh); // Unpack to 32-bit integer
        __m64 dst_2 = _mm_unpackhi_pi16(dst_ll, dst_lh);
        __m64 dst_3 = _mm_unpacklo_pi16(dst_hl, dst_hh);
        __m64 dst_4 = _mm_unpackhi_pi16(dst_hl, dst_hh);

        result_1 = _mm_add_pi32(result_1, dst_1);
        result_2 = _mm_add_pi32(result_2, dst_2);
        result_3 = _mm_add_pi32(result_3, dst_3);
        result_4 = _mm_add_pi32(result_4, dst_4);
      }

      // Divide by 16348 (FPRound)
      result_1  = _mm_srai_pi32(result_1, 14);
      result_2  = _mm_srai_pi32(result_2, 14);
      result_3  = _mm_srai_pi32(result_3, 14);
      result_4  = _mm_srai_pi32(result_4, 14);

      // Pack and store
      __m64 result_l = _mm_packs_pi32(result_1, result_2);
      __m64 result_h = _mm_packs_pi32(result_3, result_4);
      __m64 result   = _mm_packs_pu16(result_l, result_h);

      *(reinterpret_cast<__m64*>(dst+x)) = result;
    }

    // Leftover
    for (int x = wMod8; x < width; x++) {
      int result = 0;
      for (int i = 0; i < filter_size; i++) {
        result += (src_ptr+pitch_table[i])[x] * current_coeff[i];
      }
      result = ((result+8192)/16384);
      result = result > 255 ? 255 : result < 0 ? 0 : result;
      dst[x] = (BYTE) result;
    }

    dst += dst_pitch;
    current_coeff += filter_size;
  }

  _mm_empty();
}
#endif

template<SSELoader load, int f_size>
static void resize_v_sse2_planar(BYTE* dst, const BYTE* src, int dst_pitch, int src_pitch, ResamplingProgram* program, int width, int target_height, const int* pitch_table, const void* storage)
{
  if (f_size != program->filter_size) {
    // Internal error?
  }

  int filter_size = f_size;
  if (filter_size == 0) {
    filter_size = program->filter_size;
  }

  short* current_coeff = program->pixel_coefficient;
  
  int wMod16 = (width / 16) * 16;
  int sizeMod2 = (filter_size/2) * 2;
  bool notMod2 = sizeMod2 < filter_size;

  __m128i zero = _mm_setzero_si128();

  for (int y = 0; y < target_height; y++) {
    int offset = program->pixel_offset[y];
    const BYTE* src_ptr = src + pitch_table[offset];

    for (int x = 0; x < wMod16; x += 16) {
      __m128i result_1 = _mm_set1_epi32(8192); // Init. with rounder (16384/2 = 8192)
      __m128i result_2 = result_1;
      __m128i result_3 = result_1;
      __m128i result_4 = result_1;
      
      for (int i = 0; i < sizeMod2; i += 2) {
        __m128i src_p1 = load(reinterpret_cast<const __m128i*>(src_ptr+pitch_table[i]+x));   // p|o|n|m|l|k|j|i|h|g|f|e|d|c|b|a
        __m128i src_p2 = load(reinterpret_cast<const __m128i*>(src_ptr+pitch_table[i+1]+x)); // P|O|N|M|L|K|J|I|H|G|F|E|D|C|B|A
         
        __m128i src_l = _mm_unpacklo_epi8(src_p1, src_p2);                                   // Hh|Gg|Ff|Ee|Dd|Cc|Bb|Aa
        __m128i src_h = _mm_unpackhi_epi8(src_p1, src_p2);                                   // Pp|Oo|Nn|Mm|Ll|Kk|Jj|Ii

        __m128i src_1 = _mm_unpacklo_epi8(src_l, zero);                                      // .D|.d|.C|.c|.B|.b|.A|.a
        __m128i src_2 = _mm_unpackhi_epi8(src_l, zero);                                      // .H|.h|.G|.g|.F|.f|.E|.e
        __m128i src_3 = _mm_unpacklo_epi8(src_h, zero);                                      // etc.
        __m128i src_4 = _mm_unpackhi_epi8(src_h, zero);                                      // etc.

        __m128i coeff = _mm_cvtsi32_si128(*reinterpret_cast<const int*>(current_coeff+i));   // XX|XX|XX|XX|XX|XX|CO|co
        coeff = _mm_shuffle_epi32(coeff, 0);                                                 // CO|co|CO|co|CO|co|CO|co
        
        __m128i dst_1 = _mm_madd_epi16(src_1, coeff);                                         // CO*D+co*d | CO*C+co*c | CO*B+co*b | CO*A+co*a
        __m128i dst_2 = _mm_madd_epi16(src_2, coeff);                                         // etc.
        __m128i dst_3 = _mm_madd_epi16(src_3, coeff);
        __m128i dst_4 = _mm_madd_epi16(src_4, coeff);

        result_1 = _mm_add_epi32(result_1, dst_1);
        result_2 = _mm_add_epi32(result_2, dst_2);
        result_3 = _mm_add_epi32(result_3, dst_3);
        result_4 = _mm_add_epi32(result_4, dst_4);
      }
      
      if (notMod2) { // do last odd row
        __m128i src_p = load(reinterpret_cast<const __m128i*>(src_ptr+pitch_table[sizeMod2]+x));

        __m128i src_l = _mm_unpacklo_epi8(src_p, zero);
        __m128i src_h = _mm_unpackhi_epi8(src_p, zero);

        __m128i coeff = _mm_set1_epi16(current_coeff[sizeMod2]);

        __m128i dst_ll = _mm_mullo_epi16(src_l, coeff);   // Multiply by coefficient
        __m128i dst_lh = _mm_mulhi_epi16(src_l, coeff);
        __m128i dst_hl = _mm_mullo_epi16(src_h, coeff);
        __m128i dst_hh = _mm_mulhi_epi16(src_h, coeff);

        __m128i dst_1 = _mm_unpacklo_epi16(dst_ll, dst_lh); // Unpack to 32-bit integer
        __m128i dst_2 = _mm_unpackhi_epi16(dst_ll, dst_lh);
        __m128i dst_3 = _mm_unpacklo_epi16(dst_hl, dst_hh);
        __m128i dst_4 = _mm_unpackhi_epi16(dst_hl, dst_hh);

        result_1 = _mm_add_epi32(result_1, dst_1);
        result_2 = _mm_add_epi32(result_2, dst_2);
        result_3 = _mm_add_epi32(result_3, dst_3);
        result_4 = _mm_add_epi32(result_4, dst_4);
      }
      
      // Divide by 16348 (FPRound)
      result_1  = _mm_srai_epi32(result_1, 14);
      result_2  = _mm_srai_epi32(result_2, 14);
      result_3  = _mm_srai_epi32(result_3, 14);
      result_4  = _mm_srai_epi32(result_4, 14);

      // Pack and store
      __m128i result_l = _mm_packs_epi32(result_1, result_2);
      __m128i result_h = _mm_packs_epi32(result_3, result_4);
      __m128i result   = _mm_packus_epi16(result_l, result_h);

      _mm_store_si128(reinterpret_cast<__m128i*>(dst+x), result);
    }

    // Leftover
    for (int x = wMod16; x < width; x++) {
      int result = 0;
      for (int i = 0; i < filter_size; i++) {
        result += (src_ptr+pitch_table[i])[x] * current_coeff[i];
      }
      result = ((result+8192)/16384);
      result = result > 255 ? 255 : result < 0 ? 0 : result;
      dst[x] = (BYTE) result;
    }

    dst += dst_pitch;
    current_coeff += filter_size;
  }
}

template<SSELoader load, int f_size>
static void resize_v_ssse3_planar(BYTE* dst, const BYTE* src, int dst_pitch, int src_pitch, ResamplingProgram* program, int width, int target_height, const int* pitch_table, const void* storage)
{
  if (f_size != program->filter_size) {
     // Internal error?
  }

  int filter_size = f_size;
  if (filter_size == 0) {
    filter_size = program->filter_size;
  }

  short* current_coeff = program->pixel_coefficient;
  
  int wMod16 = (width / 16) * 16;
  
  __m128i zero = _mm_setzero_si128();
  __m128i coeff_unpacker = _mm_set_epi8(1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0);

  for (int y = 0; y < target_height; y++) {
    int offset = program->pixel_offset[y];
    const BYTE* src_ptr = src + pitch_table[offset];

    for (int x = 0; x < wMod16; x+=16) {
      __m128i result_l = _mm_set1_epi16(32); // Init. with rounder ((1 << 6)/2 = 32)
      __m128i result_h = result_l;

      const BYTE* src2_ptr = src_ptr+x;
      
#pragma unroll(f_size)
      for (int i = 0; i < filter_size; i++) {
        __m128i src_p = load(reinterpret_cast<const __m128i*>(src2_ptr));

        __m128i src_l = _mm_unpacklo_epi8(src_p, zero);
        __m128i src_h = _mm_unpackhi_epi8(src_p, zero);

        src_l = _mm_slli_epi16(src_l, 7);
        src_h = _mm_slli_epi16(src_h, 7);

        __m128i coeff = _mm_cvtsi32_si128(*reinterpret_cast<const int*>(current_coeff+i));
                coeff = _mm_shuffle_epi8(coeff, coeff_unpacker);

        __m128i dst_l = _mm_mulhrs_epi16(src_l, coeff);   // Multiply by coefficient (SSSE3)
        __m128i dst_h = _mm_mulhrs_epi16(src_h, coeff);

        result_l = _mm_add_epi16(result_l, dst_l);
        result_h = _mm_add_epi16(result_h, dst_h);

        src2_ptr += src_pitch;
      }

      // Divide by 64
      result_l  = _mm_srai_epi16(result_l, 6);
      result_h  = _mm_srai_epi16(result_h, 6);

      // Pack and store
      __m128i result   = _mm_packus_epi16(result_l, result_h);

      _mm_store_si128(reinterpret_cast<__m128i*>(dst+x), result);
    }

    // Leftover
    for (int x = wMod16; x < width; x++) {
      int result = 0;
      for (int i = 0; i < filter_size; i++) {
        result += (src_ptr+pitch_table[i])[x] * current_coeff[i];
      }
      result = ((result+8192)/16384);
      result = result > 255 ? 255 : result < 0 ? 0 : result;
      dst[x] = (BYTE) result;
    }

    dst += dst_pitch;
    current_coeff += filter_size;
  }
}

__forceinline static void resize_v_create_pitch_table(int* table, int pitch, int height) {
  table[0] = 0;
  for (int i = 1; i < height; i++) {
    table[i] = table[i-1]+pitch;
  }
}


/********************************************************************
***** Declare index of new filters for Avisynth's filter engine *****
********************************************************************/

extern const AVSFunction Resample_filters[] = {
  { "PointResize", "cii[src_left]f[src_top]f[src_width]f[src_height]f", FilteredResize::Create_PointResize },
  { "BilinearResize", "cii[src_left]f[src_top]f[src_width]f[src_height]f", FilteredResize::Create_BilinearResize },
  { "BicubicResize", "cii[b]f[c]f[src_left]f[src_top]f[src_width]f[src_height]f", FilteredResize::Create_BicubicResize },
  { "LanczosResize", "cii[src_left]f[src_top]f[src_width]f[src_height]f[taps]i", FilteredResize::Create_LanczosResize},
  { "Lanczos4Resize", "cii[src_left]f[src_top]f[src_width]f[src_height]f", FilteredResize::Create_Lanczos4Resize},
  { "BlackmanResize", "cii[src_left]f[src_top]f[src_width]f[src_height]f[taps]i", FilteredResize::Create_BlackmanResize},
  { "Spline16Resize", "cii[src_left]f[src_top]f[src_width]f[src_height]f", FilteredResize::Create_Spline16Resize},
  { "Spline36Resize", "cii[src_left]f[src_top]f[src_width]f[src_height]f", FilteredResize::Create_Spline36Resize},
  { "Spline64Resize", "cii[src_left]f[src_top]f[src_width]f[src_height]f", FilteredResize::Create_Spline64Resize},
  { "GaussResize", "cii[src_left]f[src_top]f[src_width]f[src_height]f[p]f", FilteredResize::Create_GaussianResize},
  { "SincResize", "cii[src_left]f[src_top]f[src_width]f[src_height]f[taps]i", FilteredResize::Create_SincResize},
  /**
    * Resize(PClip clip, dst_width, dst_height [src_left, src_top, src_width, int src_height,] )
    *
    * src_left et al.   =  when these optional arguments are given, the filter acts just like
    *                      a Crop was performed with those parameters before resizing, only faster
   **/

  { 0 }
};


FilteredResizeH::FilteredResizeH( PClip _child, double subrange_left, double subrange_width,
                                  int target_width, ResamplingFunction* func, IScriptEnvironment* env )
  : GenericVideoFilter(_child),
  resampling_program_luma(0), resampling_program_chroma(0),
  src_pitch_table_luma(0), src_pitch_table_chromaU(0), src_pitch_table_chromaV(0),
  src_pitch_luma(-1), src_pitch_chromaU(-1), src_pitch_chromaV(-1),
  filter_storage_luma(0), filter_storage_chroma(0),
  temp_y_1(0), temp_y_2(0), temp_u_1(0), temp_u_2(0), temp_v_1(0), temp_v_2(0)
{
  src_width  = vi.width;
  src_height = vi.height;
  dst_width  = target_width;
  dst_height = vi.height;

  if (target_width <= 0) {
    env->ThrowError("Resize: Width must be greater than 0.");
  }

  if (vi.IsPlanar() && !vi.IsY8()) {
    const int mask = (1 << vi.GetPlaneWidthSubsampling(PLANAR_U)) - 1;

    if (target_width & mask)
      env->ThrowError("Resize: Planar destination height must be a multiple of %d.", mask+1);
  }

  // Create resampling program and pitch table
  resampling_program_luma  = func->GetResamplingProgram(vi.width, subrange_left, subrange_width, target_width, env);
  src_pitch_table_luma     = new int[vi.width];
  resampler_luma   = FilteredResizeV::GetResampler(env->GetCPUFlags(), true, filter_storage_luma, resampling_program_luma);

  // Allocate temporary byte buffer
  allocate_frame_buffer(temp_y_1, temp_y_1_pitch, vi.BytesFromPixels(src_height), src_width); // transposed
  allocate_frame_buffer(temp_y_2, temp_y_2_pitch, vi.BytesFromPixels(dst_height), dst_width); // transposed
  resize_v_create_pitch_table(src_pitch_table_luma, temp_y_1_pitch, src_width);

  if (vi.IsPlanar() && !vi.IsY8()) {
    const int shift = vi.GetPlaneWidthSubsampling(PLANAR_U);
    const int shift_h = vi.GetPlaneHeightSubsampling(PLANAR_U);
    const int div   = 1 << shift;

    const int src_chroma_width = src_width >> shift;
    const int dst_chroma_width = dst_width >> shift;
    const int src_chroma_height = src_height >> shift_h;
    const int dst_chroma_height = dst_height >> shift_h;

    resampling_program_chroma = func->GetResamplingProgram(
      vi.width       >> shift,
      subrange_left   / div,
      subrange_width  / div,
      target_width   >> shift,
      env);
    src_pitch_table_chromaU    = new int[vi.width >> shift];
    src_pitch_table_chromaV    = new int[vi.width >> shift];
    resampler_chroma = FilteredResizeV::GetResampler(env->GetCPUFlags(), true, filter_storage_chroma, resampling_program_chroma);
    
    // Allocate temporary byte buffer
    allocate_frame_buffer(temp_u_1, temp_u_1_pitch, src_chroma_height, src_chroma_width); // transposed
    allocate_frame_buffer(temp_u_2, temp_u_2_pitch, dst_chroma_height, dst_chroma_width); // transposed
    allocate_frame_buffer(temp_v_1, temp_v_1_pitch, src_chroma_height, src_chroma_width); // transposed
    allocate_frame_buffer(temp_v_2, temp_v_2_pitch, dst_chroma_height, dst_chroma_width); // transposed
    resize_v_create_pitch_table(src_pitch_table_chromaU, temp_u_1_pitch, src_chroma_width);
    resize_v_create_pitch_table(src_pitch_table_chromaV, temp_v_1_pitch, src_chroma_width);
  }

  // Initialize Turn function
  if (vi.IsRGB24()) {
    turn_left = turn_left_rgb24;
    turn_right = turn_right_rgb24;
  } else if (vi.IsRGB32()) {
    if (env->GetCPUFlags() & CPUF_SSE2) {
      turn_left = turn_left_rgb32_sse2;
      turn_right = turn_right_rgb32_sse2;
    } else {
      turn_left = turn_left_rgb32_c;
      turn_right = turn_right_rgb32_c;
    }
  } else {
    if (env->GetCPUFlags() & CPUF_SSE2) {
      turn_left = turn_left_plane_sse2;
      turn_right = turn_right_plane_sse2;
    } else {
      turn_left = turn_left_plane_c;
      turn_right = turn_right_plane_c;
    }
  }

  // Change target video info size
  vi.width = target_width;
}

PVideoFrame __stdcall FilteredResizeH::GetFrame(int n, IScriptEnvironment* env)
{
  PVideoFrame src = child->GetFrame(n, env);
  PVideoFrame dst = env->NewVideoFrame(vi);

  if (!vi.IsRGB()) {
    // Y Plane
    turn_right(src->GetReadPtr(), temp_y_1, src_width, src_height, src->GetPitch(), temp_y_1_pitch);
    resampler_luma(temp_y_2, temp_y_1, temp_y_2_pitch, temp_y_1_pitch, resampling_program_luma, src_height, dst_width, src_pitch_table_luma, filter_storage_luma);
    turn_left(temp_y_2, dst->GetWritePtr(), dst_height, dst_width, temp_y_2_pitch, dst->GetPitch());

    if (!vi.IsY8()) {
      const int shift = vi.GetPlaneWidthSubsampling(PLANAR_U);
      const int shift_h = vi.GetPlaneHeightSubsampling(PLANAR_U);

      const int src_chroma_width = src_width >> shift;
      const int dst_chroma_width = dst_width >> shift;
      const int src_chroma_height = src_height >> shift_h;
      const int dst_chroma_height = dst_height >> shift_h;

      // U Plane
      turn_right(src->GetReadPtr(PLANAR_U), temp_u_1, src_chroma_width, src_chroma_height, src->GetPitch(PLANAR_U), temp_u_1_pitch);
      resampler_luma(temp_u_2, temp_u_1, temp_u_2_pitch, temp_u_1_pitch, resampling_program_chroma, src_chroma_height, dst_chroma_width, src_pitch_table_chromaU, filter_storage_chroma);
      turn_left(temp_u_2, dst->GetWritePtr(PLANAR_U), dst_chroma_height, dst_chroma_width, temp_u_2_pitch, dst->GetPitch(PLANAR_U));

      // V Plane
      turn_right(src->GetReadPtr(PLANAR_V), temp_v_1, src_chroma_width, src_chroma_height, src->GetPitch(PLANAR_V), temp_v_1_pitch);
      resampler_luma(temp_v_2, temp_v_1, temp_v_2_pitch, temp_v_1_pitch, resampling_program_chroma, src_chroma_height, dst_chroma_width, src_pitch_table_chromaV, filter_storage_chroma);
      turn_left(temp_v_2, dst->GetWritePtr(PLANAR_V), dst_chroma_height, dst_chroma_width, temp_v_2_pitch, dst->GetPitch(PLANAR_V));
    }
  } else {
    // RGB
    turn_right(src->GetReadPtr(), temp_y_1, vi.BytesFromPixels(src_width), src_height, src->GetPitch(), temp_y_1_pitch);
    resampler_luma(temp_y_2, temp_y_1, temp_y_2_pitch, temp_y_1_pitch, resampling_program_luma, vi.BytesFromPixels(src_height), dst_width, src_pitch_table_luma, filter_storage_luma);
    turn_left(temp_y_2, dst->GetWritePtr(), vi.BytesFromPixels(dst_height), dst_width, temp_y_2_pitch, dst->GetPitch());
  }

  return dst;
}

FilteredResizeH::~FilteredResizeH(void)
{
  if (resampling_program_luma)   { delete resampling_program_luma; }
  if (resampling_program_chroma) { delete resampling_program_chroma; }
  if (src_pitch_table_luma)    { delete[] src_pitch_table_luma; }
  if (src_pitch_table_chromaU) { delete[] src_pitch_table_chromaU; }
  if (src_pitch_table_chromaV) { delete[] src_pitch_table_chromaV; }

  if (filter_storage_luma) { _aligned_free(filter_storage_luma); }
  if (filter_storage_chroma) { _aligned_free(filter_storage_chroma); }

  if (temp_y_1) { _aligned_free(temp_y_1); }
  if (temp_y_2) { _aligned_free(temp_y_2); }
  if (temp_u_1) { _aligned_free(temp_u_1); }
  if (temp_u_2) { _aligned_free(temp_u_2); }
  if (temp_v_1) { _aligned_free(temp_v_1); }
  if (temp_v_2) { _aligned_free(temp_v_2); }
}

/***************************************
 ***** Filtered Resize - Vertical ******
 ***************************************/

FilteredResizeV::FilteredResizeV( PClip _child, double subrange_top, double subrange_height,
                                  int target_height, ResamplingFunction* func, IScriptEnvironment* env )
  : GenericVideoFilter(_child),
    resampling_program_luma(0), resampling_program_chroma(0),
    src_pitch_table_luma(0), src_pitch_table_chromaU(0), src_pitch_table_chromaV(0),
    src_pitch_luma(-1), src_pitch_chromaU(-1), src_pitch_chromaV(-1),
    filter_storage_luma_aligned(0), filter_storage_luma_unaligned(0),
    filter_storage_chroma_aligned(0), filter_storage_chroma_unaligned(0)
{
  if (target_height <= 0)
    env->ThrowError("Resize: Height must be greater than 0.");

  if (vi.IsPlanar() && !vi.IsY8()) {
    const int mask = (1 << vi.GetPlaneHeightSubsampling(PLANAR_U)) - 1;

    if (target_height & mask)
      env->ThrowError("Resize: Planar destination height must be a multiple of %d.", mask+1);
  }

  if (vi.IsRGB())
    subrange_top = vi.height - subrange_top - subrange_height; // why?

  // Create resampling program and pitch table
  resampling_program_luma  = func->GetResamplingProgram(vi.height, subrange_top, subrange_height, target_height, env);
  src_pitch_table_luma     = new int[vi.height];
  resampler_luma_aligned   = GetResampler(env->GetCPUFlags(), true , filter_storage_luma_aligned,   resampling_program_luma);
  resampler_luma_unaligned = GetResampler(env->GetCPUFlags(), false, filter_storage_luma_unaligned, resampling_program_luma);

  if (vi.IsPlanar() && !vi.IsY8()) {
    const int shift = vi.GetPlaneHeightSubsampling(PLANAR_U);
    const int div   = 1 << shift;

    resampling_program_chroma = func->GetResamplingProgram(
                                  vi.height      >> shift,
                                  subrange_top    / div,
                                  subrange_height / div,
                                  target_height  >> shift,
                                  env);
    src_pitch_table_chromaU    = new int[vi.height >> shift];
    src_pitch_table_chromaV    = new int[vi.height >> shift];
    resampler_chroma_aligned   = GetResampler(env->GetCPUFlags(), true , filter_storage_chroma_aligned,   resampling_program_chroma);
    resampler_chroma_unaligned = GetResampler(env->GetCPUFlags(), false, filter_storage_chroma_unaligned, resampling_program_chroma);
  }

  // Change target video info size
  vi.height = target_height;
}

PVideoFrame __stdcall FilteredResizeV::GetFrame(int n, IScriptEnvironment* env)
{
  PVideoFrame src = child->GetFrame(n, env);
  PVideoFrame dst = env->NewVideoFrame(vi);
  int src_pitch = src->GetPitch();
  int dst_pitch = dst->GetPitch();
  const BYTE* srcp = src->GetReadPtr();
        BYTE* dstp = dst->GetWritePtr();

  // Create pitch table
  if (src_pitch_luma != src->GetPitch()) {
    src_pitch_luma = src->GetPitch();
    resize_v_create_pitch_table(src_pitch_table_luma, src_pitch_luma, src->GetHeight());
  }

  if ((!vi.IsY8() && vi.IsPlanar()) && src_pitch_chromaU != src->GetPitch(PLANAR_U)) {
    src_pitch_chromaU = src->GetPitch(PLANAR_U);
    resize_v_create_pitch_table(src_pitch_table_chromaU, src_pitch_chromaU, src->GetHeight(PLANAR_U));
  }

  if ((!vi.IsY8() && vi.IsPlanar()) && src_pitch_chromaV != src->GetPitch(PLANAR_V)) {
    src_pitch_chromaV = src->GetPitch(PLANAR_V);
    resize_v_create_pitch_table(src_pitch_table_chromaV, src_pitch_chromaV, src->GetHeight(PLANAR_V));
  }

  // Do resizing
  if (IsPtrAligned(srcp, 16) && (src_pitch & 15) == 0)
    resampler_luma_aligned(dstp, srcp, dst_pitch, src_pitch, resampling_program_luma, vi.BytesFromPixels(vi.width), vi.height, src_pitch_table_luma, filter_storage_luma_aligned);
  else
    resampler_luma_unaligned(dstp, srcp, dst_pitch, src_pitch, resampling_program_luma, vi.BytesFromPixels(vi.width), vi.height, src_pitch_table_luma, filter_storage_luma_unaligned);
    
  if (!vi.IsY8() && vi.IsPlanar()) {
    int width = vi.width >> vi.GetPlaneWidthSubsampling(PLANAR_U);
    int height = vi.height >> vi.GetPlaneHeightSubsampling(PLANAR_U);

    // Plane U resizing
    src_pitch = src->GetPitch(PLANAR_U);
    dst_pitch = dst->GetPitch(PLANAR_U);
    srcp = src->GetReadPtr(PLANAR_U);
    dstp = dst->GetWritePtr(PLANAR_U);
      
    if (IsPtrAligned(srcp, 16) && (src_pitch & 15) == 0)
      resampler_chroma_aligned(dstp, srcp, dst_pitch, src_pitch, resampling_program_chroma, width, height, src_pitch_table_chromaU, filter_storage_chroma_unaligned);
    else
      resampler_chroma_unaligned(dstp, srcp, dst_pitch, src_pitch, resampling_program_chroma, width, height, src_pitch_table_chromaU, filter_storage_chroma_unaligned);

    // Plane V resizing
    src_pitch = src->GetPitch(PLANAR_V);
    dst_pitch = dst->GetPitch(PLANAR_V);
    srcp = src->GetReadPtr(PLANAR_V);
    dstp = dst->GetWritePtr(PLANAR_V);
  
    if (IsPtrAligned(srcp, 16) && (src_pitch & 15) == 0)
      resampler_chroma_aligned(dstp, srcp, dst_pitch, src_pitch, resampling_program_chroma, width, height, src_pitch_table_chromaV, filter_storage_chroma_unaligned);
    else
      resampler_chroma_unaligned(dstp, srcp, dst_pitch, src_pitch, resampling_program_chroma, width, height, src_pitch_table_chromaV, filter_storage_chroma_unaligned);
  }

  return dst;
}

ResamplerV FilteredResizeV::GetResampler(int CPU, bool aligned, void*& storage, ResamplingProgram* program)
{
  switch (program->filter_size) {
#define FILTER_SIZE(n) \
  case n: return GetResamplerInternal<n>(CPU, aligned, storage, program); break;

    FILTER_SIZE(1);
    FILTER_SIZE(2);
    FILTER_SIZE(3);
    FILTER_SIZE(4);
    FILTER_SIZE(5);
    FILTER_SIZE(6);
    FILTER_SIZE(7);
    FILTER_SIZE(8);
    FILTER_SIZE(9);
    FILTER_SIZE(10);
    FILTER_SIZE(11);
    FILTER_SIZE(12);
    FILTER_SIZE(13);
    FILTER_SIZE(14);
    FILTER_SIZE(15);
    FILTER_SIZE(16);

#undef FILTER_SIZE

  default:
    return GetResamplerInternal<0>(CPU, aligned, storage, program);
  }
}

template<int filter_size>
ResamplerV FilteredResizeV::GetResamplerInternal(int CPU, bool aligned, void*& storage, ResamplingProgram* program)
{
  if (program->filter_size == 1) {
    // Fast pointresize
    return resize_v_planar_pointresize;
  } else {
    // Other resizers
    if (CPU & CPUF_SSSE3) {
      if (aligned && CPU & CPUF_SSE4_1) {
        return resize_v_ssse3_planar<simd_load_streaming, filter_size>;
      } else if (aligned) { // SSSE3 aligned
        return resize_v_ssse3_planar<simd_load_aligned, filter_size>;
      } else if (CPU & CPUF_SSE3) { // SSE3 lddqu
        return resize_v_ssse3_planar<simd_load_unaligned_sse3, filter_size>;
      } else { // SSSE3 unaligned
        return resize_v_ssse3_planar<simd_load_unaligned, filter_size>;
      }
    } else if (CPU & CPUF_SSE2) {
      if (aligned && CPU & CPUF_SSE4_1) { // SSE4.1 movntdqa constantly provide ~2% performance increase in my testing
        return resize_v_sse2_planar<simd_load_streaming, filter_size>;
      } else if (aligned) { // SSE2 aligned
        return resize_v_sse2_planar<simd_load_aligned, filter_size>;
      } else if (CPU & CPUF_SSE3) { // SSE2 lddqu
        return resize_v_sse2_planar<simd_load_unaligned_sse3, filter_size>;
      } else { // SSE2 unaligned
        return resize_v_sse2_planar<simd_load_unaligned, filter_size>;
      }
#ifdef X86_32
    } else if (CPU & CPUF_MMX) {
      return resize_v_mmx_planar<filter_size>;
#endif
    } else { // C version
      return resize_v_c_planar<filter_size>;
    }
  }
}

FilteredResizeV::~FilteredResizeV(void)
{
  if (resampling_program_luma)   { delete resampling_program_luma; }
  if (resampling_program_chroma) { delete resampling_program_chroma; }
  if (src_pitch_table_luma)    { delete[] src_pitch_table_luma; }
  if (src_pitch_table_chromaU) { delete[] src_pitch_table_chromaU; }
  if (src_pitch_table_chromaV) { delete[] src_pitch_table_chromaV; }

  if (filter_storage_luma_aligned) { _aligned_free(filter_storage_luma_aligned); }
  if (filter_storage_luma_unaligned) { _aligned_free(filter_storage_luma_unaligned); }
  if (filter_storage_chroma_aligned) { _aligned_free(filter_storage_chroma_aligned); }
  if (filter_storage_chroma_unaligned) { _aligned_free(filter_storage_chroma_unaligned); }
}


/**********************************************
 *******   Resampling Factory Methods   *******
 **********************************************/

PClip FilteredResize::CreateResizeH(PClip clip, double subrange_left, double subrange_width, int target_width,
                    ResamplingFunction* func, IScriptEnvironment* env)
{
  const VideoInfo& vi = clip->GetVideoInfo();
  if (subrange_left == 0 && subrange_width == target_width && subrange_width == vi.width) {
    return clip;
  }

  if (subrange_left == int(subrange_left) && subrange_width == target_width
   && subrange_left >= 0 && subrange_left + subrange_width <= vi.width) {
    const int mask = (vi.IsYUV() && !vi.IsY8()) ? (1 << vi.GetPlaneWidthSubsampling(PLANAR_U)) - 1 : 0;

    if (((int(subrange_left) | int(subrange_width)) & mask) == 0)
      return new Crop(int(subrange_left), 0, int(subrange_width), vi.height, 0, clip, env);
  }

  // Convert interleaved yuv to planar yuv
  PClip result = clip;
  if (vi.IsYUY2()) {
    result = new ConvertYUY2ToYV16(result,  env);
  }
  result = new FilteredResizeH(result, subrange_left, subrange_width, target_width, func, env);
  if (vi.IsYUY2()) {
    result = new ConvertYV16ToYUY2(result,  env);
  }

  return result;
}


PClip FilteredResize::CreateResizeV(PClip clip, double subrange_top, double subrange_height, int target_height,
                    ResamplingFunction* func, IScriptEnvironment* env)
{
  const VideoInfo& vi = clip->GetVideoInfo();
  if (subrange_top == 0 && subrange_height == target_height && subrange_height == vi.height) {
    return clip;
  }

  if (subrange_top == int(subrange_top) && subrange_height == target_height
   && subrange_top >= 0 && subrange_top + subrange_height <= vi.height) {
    const int mask = (vi.IsYUV() && !vi.IsY8()) ? (1 << vi.GetPlaneHeightSubsampling(PLANAR_U)) - 1 : 0;

    if (((int(subrange_top) | int(subrange_height)) & mask) == 0)
      return new Crop(0, int(subrange_top), vi.width, int(subrange_height), 0, clip, env);
  }
  return new FilteredResizeV(clip, subrange_top, subrange_height, target_height, func, env);
}


PClip FilteredResize::CreateResize(PClip clip, int target_width, int target_height, const AVSValue* args,
                   ResamplingFunction* f, IScriptEnvironment* env)
{
  const VideoInfo& vi = clip->GetVideoInfo();
  const double subrange_left = args[0].AsFloat(0), subrange_top = args[1].AsFloat(0);

  double subrange_width = args[2].AsDblDef(vi.width), subrange_height = args[3].AsDblDef(vi.height);
  // Crop style syntax
  if (subrange_width  <= 0.0) subrange_width  = vi.width  - subrange_left + subrange_width;
  if (subrange_height <= 0.0) subrange_height = vi.height - subrange_top  + subrange_height;

  PClip result;
  // ensure that the intermediate area is maximal
  const double area_FirstH = subrange_height * target_width;
  const double area_FirstV = subrange_width * target_height;
  if (area_FirstH < area_FirstV)
  {
      result = CreateResizeV(clip, subrange_top, subrange_height, target_height, f, env);
      result = CreateResizeH(result, subrange_left, subrange_width, target_width, f, env);
  }
  else
  {
      result = CreateResizeH(clip, subrange_left, subrange_width, target_width, f, env);
      result = CreateResizeV(result, subrange_top, subrange_height, target_height, f, env);
  }
  return result;
}

AVSValue __cdecl FilteredResize::Create_PointResize(AVSValue args, void*, IScriptEnvironment* env)
{
  return CreateResize( args[0].AsClip(), args[1].AsInt(), args[2].AsInt(), &args[3],
                       &PointFilter(), env );
}


AVSValue __cdecl FilteredResize::Create_BilinearResize(AVSValue args, void*, IScriptEnvironment* env)
{
  return CreateResize( args[0].AsClip(), args[1].AsInt(), args[2].AsInt(), &args[3],
                       &TriangleFilter(), env );
}


AVSValue __cdecl FilteredResize::Create_BicubicResize(AVSValue args, void*, IScriptEnvironment* env)
{
  return CreateResize( args[0].AsClip(), args[1].AsInt(), args[2].AsInt(), &args[5],
                       &MitchellNetravaliFilter(args[3].AsDblDef(1./3.), args[4].AsDblDef(1./3.)), env );
}

AVSValue __cdecl FilteredResize::Create_LanczosResize(AVSValue args, void*, IScriptEnvironment* env)
{
  return CreateResize( args[0].AsClip(), args[1].AsInt(), args[2].AsInt(), &args[3],
                       &LanczosFilter(args[7].AsInt(3)), env );
}

AVSValue __cdecl FilteredResize::Create_Lanczos4Resize(AVSValue args, void*, IScriptEnvironment* env)
{
  return CreateResize( args[0].AsClip(), args[1].AsInt(), args[2].AsInt(), &args[3],
                       &LanczosFilter(4), env );
}

AVSValue __cdecl FilteredResize::Create_BlackmanResize(AVSValue args, void*, IScriptEnvironment* env)
{
  return CreateResize( args[0].AsClip(), args[1].AsInt(), args[2].AsInt(), &args[3],
                       &BlackmanFilter(args[7].AsInt(4)), env );
}

AVSValue __cdecl FilteredResize::Create_Spline16Resize(AVSValue args, void*, IScriptEnvironment* env)
{
  return CreateResize( args[0].AsClip(), args[1].AsInt(), args[2].AsInt(), &args[3],
                       &Spline16Filter(), env );
}

AVSValue __cdecl FilteredResize::Create_Spline36Resize(AVSValue args, void*, IScriptEnvironment* env)
{
  return CreateResize( args[0].AsClip(), args[1].AsInt(), args[2].AsInt(), &args[3],
                       &Spline36Filter(), env );
}

AVSValue __cdecl FilteredResize::Create_Spline64Resize(AVSValue args, void*, IScriptEnvironment* env)
{
  return CreateResize( args[0].AsClip(), args[1].AsInt(), args[2].AsInt(), &args[3],
                       &Spline64Filter(), env );
}

AVSValue __cdecl FilteredResize::Create_GaussianResize(AVSValue args, void*, IScriptEnvironment* env)
{
  return CreateResize( args[0].AsClip(), args[1].AsInt(), args[2].AsInt(), &args[3],
                       &GaussianFilter(args[7].AsFloat(30.0f)), env );
}

AVSValue __cdecl FilteredResize::Create_SincResize(AVSValue args, void*, IScriptEnvironment* env)
{
  return CreateResize( args[0].AsClip(), args[1].AsInt(), args[2].AsInt(), &args[3],
                       &SincFilter(args[7].AsInt(4)), env );
}

