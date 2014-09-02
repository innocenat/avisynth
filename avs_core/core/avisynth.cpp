// Avisynth v2.6.  Copyright 2002-2009 Ben Rudiak-Gould et al.
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

#include <avisynth.h>
#include "../core/internal.h"
#include "./parser/script.h"
#include <avs/minmax.h>
#include <avs/alignment.h>
#include "strings.h"
#include <avs/cpuid.h>
#include <unordered_set>
#include "bitblt.h"
#include "PluginManager.h"
#include "MappedList.h"
#include <vector>

#include <avs/win.h>
#include <objbase.h>

#include <string>
#include <cstdarg>
#include <cassert>
#include "MTGuard.h"
#include "cache.h"

#ifdef _MSC_VER
  #define strnicmp(a,b,c) _strnicmp(a,b,c)
#elif !defined(_RPT1)
  #define _RPT1(x,y,z) ((void)0)
#endif

#ifndef YieldProcessor // low power spin idle
  #define YieldProcessor() __nop(void)
#endif

extern const AVSFunction Audio_filters[], Combine_filters[], Convert_filters[],
                   Convolution_filters[], Edit_filters[], Field_filters[],
                   Focus_filters[], Fps_filters[], Histogram_filters[],
                   Layer_filters[], Levels_filters[], Misc_filters[],
                   Plugin_functions[], Resample_filters[], Resize_filters[],
                   Script_functions[], Source_filters[], Text_filters[],
                   Transform_filters[], Merge_filters[], Color_filters[],
                   Debug_filters[], Turn_filters[],
                   Conditional_filters[], Conditional_funtions_filters[],
                   Cache_filters[], Greyscale_filters[],
                   Swap_filters[], Overlay_filters[];


const AVSFunction* builtin_functions[] = {
                   Audio_filters, Combine_filters, Convert_filters,
                   Convolution_filters, Edit_filters, Field_filters,
                   Focus_filters, Fps_filters, Histogram_filters,
                   Layer_filters, Levels_filters, Misc_filters,
                   Resample_filters, Resize_filters,
                   Script_functions, Source_filters, Text_filters,
                   Transform_filters, Merge_filters, Color_filters,
                   Debug_filters, Turn_filters,
                   Conditional_filters, Conditional_funtions_filters,
                   Plugin_functions, Cache_filters,
                   Overlay_filters, Greyscale_filters, Swap_filters};

// Global statistics counters
struct {
  unsigned int CleanUps;
  unsigned int Losses;
  unsigned int PlanA1;
  unsigned int PlanA2;
  unsigned int PlanB;
  unsigned int PlanC;
  unsigned int PlanD;
  char tag[36];
} g_Mem_stats = {0, 0, 0, 0, 0, 0, 0, "CleanUps, Losses, Plan[A1,A2,B,C,D]"};

const _PixelClip PixelClip;


// Helper function to count set bits in the processor mask.
static DWORD CountSetBits(ULONG_PTR bitMask)
{
  DWORD LSHIFT = sizeof(ULONG_PTR)*8 - 1;
  DWORD bitSetCount = 0;
  ULONG_PTR bitTest = (ULONG_PTR)1 << LSHIFT;    
  DWORD i;

  for (i = 0; i <= LSHIFT; ++i)
  {
    bitSetCount += ((bitMask & bitTest)?1:0);
    bitTest/=2;
  }

  return bitSetCount;
}

typedef BOOL (WINAPI *LPFN_GLPI)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);
static size_t GetNumPhysicalCPUs()
{
  LPFN_GLPI glpi;
  BOOL done = FALSE;
  PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = NULL;
  PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = NULL;
  DWORD returnLength = 0;
  DWORD logicalProcessorCount = 0;
  DWORD numaNodeCount = 0;
  DWORD processorCoreCount = 0;
  DWORD processorL1CacheCount = 0;
  DWORD processorL2CacheCount = 0;
  DWORD processorL3CacheCount = 0;
  DWORD processorPackageCount = 0;
  DWORD byteOffset = 0;
  PCACHE_DESCRIPTOR Cache;

  glpi = (LPFN_GLPI) GetProcAddress(
    GetModuleHandle(TEXT("kernel32")),
    "GetLogicalProcessorInformation");
  if (NULL == glpi) 
  {
//    _tprintf(TEXT("\nGetLogicalProcessorInformation is not supported.\n"));
    return (0);
  }

  while (!done)
  {
    BOOL rc = glpi(buffer, &returnLength);

    if (FALSE == rc) 
    {
      if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) 
      {
        if (buffer) 
          free(buffer);

        buffer = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(
          returnLength);

        if (NULL == buffer) 
        {
//          _tprintf(TEXT("\nError: Allocation failure\n"));
          return (0);
        }
      } 
      else 
      {
//        _tprintf(TEXT("\nError %d\n"), GetLastError());
        return (0);
      }
    } 
    else
    {
      done = TRUE;
    }
  }

  ptr = buffer;

  while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnLength) 
  {
    switch (ptr->Relationship) 
    {
    case RelationNumaNode:
      // Non-NUMA systems report a single record of this type.
      numaNodeCount++;
      break;

    case RelationProcessorCore:
      processorCoreCount++;

      // A hyperthreaded core supplies more than one logical processor.
      logicalProcessorCount += CountSetBits(ptr->ProcessorMask);
      break;

    case RelationCache:
      // Cache data is in ptr->Cache, one CACHE_DESCRIPTOR structure for each cache. 
      Cache = &ptr->Cache;
      if (Cache->Level == 1)
      {
        processorL1CacheCount++;
      }
      else if (Cache->Level == 2)
      {
        processorL2CacheCount++;
      }
      else if (Cache->Level == 3)
      {
        processorL3CacheCount++;
      }
      break;

    case RelationProcessorPackage:
      // Logical processors share a physical package.
      processorPackageCount++;
      break;

    default:
//      _tprintf(TEXT("\nError: Unsupported LOGICAL_PROCESSOR_RELATIONSHIP value.\n"));
      return (0);
    }
    byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    ptr++;
  }

  /*
  _tprintf(TEXT("\nGetLogicalProcessorInformation results:\n"));
  _tprintf(TEXT("Number of NUMA nodes: %d\n"), 
    numaNodeCount);
  _tprintf(TEXT("Number of physical processor packages: %d\n"), 
    processorPackageCount);
  _tprintf(TEXT("Number of processor cores: %d\n"), 
    processorCoreCount);
  _tprintf(TEXT("Number of logical processors: %d\n"), 
    logicalProcessorCount);
  _tprintf(TEXT("Number of processor L1/L2/L3 caches: %d/%d/%d\n"), 
    processorL1CacheCount,
    processorL2CacheCount,
    processorL3CacheCount);
  */

  free(buffer);

  return processorCoreCount;
}




void* VideoFrame::operator new(size_t size) {
  return ::operator new(size);
}


VideoFrame::VideoFrame(VideoFrameBuffer* _vfb, int _offset, int _pitch, int _row_size, int _height)
  : refcount(0), vfb(_vfb), offset(_offset), pitch(_pitch), row_size(_row_size), height(_height),
    offsetU(_offset),offsetV(_offset),pitchUV(0), row_sizeUV(0), heightUV(0)  // PitchUV=0 so this doesn't take up additional space
{
  InterlockedIncrement(&vfb->refcount);
}

VideoFrame::VideoFrame(VideoFrameBuffer* _vfb, int _offset, int _pitch, int _row_size, int _height,
                       int _offsetU, int _offsetV, int _pitchUV, int _row_sizeUV, int _heightUV)
  : refcount(0), vfb(_vfb), offset(_offset), pitch(_pitch), row_size(_row_size), height(_height),
    offsetU(_offsetU),offsetV(_offsetV),pitchUV(_pitchUV), row_sizeUV(_row_sizeUV), heightUV(_heightUV)
{
  InterlockedIncrement(&vfb->refcount);
}

// Hack note :- Use of SubFrame will require an "InterlockedDecrement(&retval->refcount);" after
// assignement to a PVideoFrame, the same as for a "New VideoFrame" to keep the refcount consistant.

VideoFrame* VideoFrame::Subframe(int rel_offset, int new_pitch, int new_row_size, int new_height) const {
  return new VideoFrame(vfb, offset+rel_offset, new_pitch, new_row_size, new_height);
}


VideoFrame* VideoFrame::Subframe(int rel_offset, int new_pitch, int new_row_size, int new_height,
                                 int rel_offsetU, int rel_offsetV, int new_pitchUV) const {
  // Maintain plane size relationship
  const int new_row_sizeUV = !row_size ? 0 : MulDiv(new_row_size, row_sizeUV, row_size);
  const int new_heightUV   = !height   ? 0 : MulDiv(new_height,   heightUV,   height);

  return new VideoFrame(vfb, offset+rel_offset, new_pitch, new_row_size, new_height,
                        rel_offsetU+offsetU, rel_offsetV+offsetV, new_pitchUV, new_row_sizeUV, new_heightUV);
}


VideoFrameBuffer::VideoFrameBuffer() : refcount(1), data(NULL), data_size(0), sequence_number(0) {}


VideoFrameBuffer::VideoFrameBuffer(int size) :
  refcount(0),
#ifdef _DEBUG
  data(new BYTE[size+16]),
#else
  data(new BYTE[size]),
#endif
  data_size(size),
  sequence_number(0) 
{
  InterlockedIncrement(&sequence_number);

#ifdef _DEBUG
  int *pInt=(int *)(data+size);
  pInt[0] = 0xDEADBEEF;
  pInt[1] = 0xDEADBEEF;
  pInt[2] = 0xDEADBEEF;
  pInt[3] = 0xDEADBEEF;

  static const BYTE filler[] = { 0x0A, 0x11, 0x0C, 0xA7, 0xED };
  BYTE* pByte = data;
  BYTE* q = pByte + data_size/5*5;
  for (; pByte<q; pByte+=5)
  {
    pByte[0]=filler[0];
    pByte[1]=filler[1];
    pByte[2]=filler[2];
    pByte[3]=filler[3];
    pByte[4]=filler[4];
  }
#endif
}

VideoFrameBuffer::~VideoFrameBuffer() {
//  _ASSERTE(refcount == 0);
  InterlockedIncrement(&sequence_number); // HACK : Notify any children with a pointer, this buffer has changed!!!
  if (data) delete[] data;
  ((BYTE*)data) = 0; // and mark it invalid!!
  ((int)data_size) = 0;   // and don't forget to set the size to 0 as well!
}


// This doles out storage space for strings.  No space is ever freed
// until the class instance is destroyed (which happens when a script
// file is closed).
class StringDump {
  enum { BLOCK_SIZE = 32768 };
  char* current_block;
  size_t block_pos, block_size;

public:
  StringDump() : current_block(0), block_pos(BLOCK_SIZE), block_size(BLOCK_SIZE) {}
  ~StringDump();
  char* SaveString(const char* s, int len = -1);
};

StringDump::~StringDump() {
  _RPT0(0,"StringDump: DeAllocating all stringblocks.\r\n");
  char* p = current_block;
  while (p) {
    char* next = *(char**)p;
    delete[] p;
    p = next;
  }
}

char* StringDump::SaveString(const char* s, int len) {
  if (len == -1)
    len = lstrlen(s);

  if (block_pos+len+1 > block_size) {
    char* new_block = new char[block_size = max(block_size, len+1+sizeof(char*))];
    _RPT0(0,"StringDump: Allocating new stringblock.\r\n");
    *(char**)new_block = current_block;   // beginning of block holds pointer to previous block
    current_block = new_block;
    block_pos = sizeof(char*);
  }
  char* result = current_block+block_pos;
  memcpy(result, s, len);
  result[len] = 0;
  block_pos += AlignNumber(len+1, (int)sizeof(char*)); // Keep word-aligned
  return result;
}


class AtExiter {
  struct AtExitRec {
    const IScriptEnvironment::ShutdownFunc func;
    void* const user_data;
    AtExitRec* const next;
    AtExitRec(IScriptEnvironment::ShutdownFunc _func, void* _user_data, AtExitRec* _next)
      : func(_func), user_data(_user_data), next(_next) {}
  };
  AtExitRec* atexit_list;

public:
  AtExiter() {
    atexit_list = 0;
  }

  void Add(IScriptEnvironment::ShutdownFunc f, void* d) {
    atexit_list = new AtExitRec(f, d, atexit_list);
  }

  void Execute(IScriptEnvironment* env) {
    while (atexit_list) {
      AtExitRec* next = atexit_list->next;
      atexit_list->func(atexit_list->user_data, env);
      delete atexit_list;
      atexit_list = next;
    }
  }
};

#include <string>
#include <unordered_map>
class MTMapState
{
private:
  typedef std::unordered_map<std::string, MtMode> MTModeMapType;

  static std::string NormalizeFilterName(const std::string& filter)
  {
    // lowercase
    std::string ret = filter;
    for (size_t i = 0; i < ret.size(); ++i)
      ret[i] = tolower(ret[i]);

    // trim trailing spaces
    size_t endpos = ret.find_last_not_of(" \t");
    if( std::string::npos != endpos )
        ret = ret.substr( 0, endpos+1 );

    // trim leading spaces
    size_t startpos = ret.find_first_not_of(" \t");
    if( std::string::npos != startpos )
        ret = ret.substr( startpos );

    return ret;
  }

public:
  static const char* DEFAULT_MODE;

  MtMode DefaultMode;
  MTModeMapType PerFilterMap;
  MTModeMapType ForcedMap;

  MTMapState() 
    : DefaultMode(MT_SERIALIZED)
  {}

  void SetMode(const char* filter, MtMode mode, bool force)
  {
    if ( ((int)mode <= (int)MT_INVALID)
      || ((int)mode >= (int)MT_MODE_COUNT) )
    {
      throw AvisynthError("Invalid MT mode specified.");
    }

    if (filter == DEFAULT_MODE)
    {
      DefaultMode = mode;
      return;
    }

    std::string f = NormalizeFilterName(filter);
    if (!force)
      PerFilterMap[f] = mode;
    else
      ForcedMap[f] = mode;
  }

  MtMode GetMode(const char* filter, bool* is_forced) const
  {
    *is_forced = false;

    if (filter == DEFAULT_MODE)
      return DefaultMode;

    std::string f = NormalizeFilterName(filter);
    MTModeMapType::const_iterator it = ForcedMap.find(f);
    if (it != ForcedMap.end())
    {
      *is_forced = true;
      return it->second;
    }

    it = PerFilterMap.find(f);
    if (it != PerFilterMap.end())
      return it->second;

    return DefaultMode;
  }

};
const char* MTMapState::DEFAULT_MODE = NULL;

#include "vartable.h"
#include "ThreadPool.h"
#include <map>
#include <atomic>
#include "Prefetcher.h"
#include "BufferPool.h"
class ScriptEnvironment : public IScriptEnvironment2 {
public:
  ScriptEnvironment();
  void __stdcall CheckVersion(int version);
  int __stdcall GetCPUFlags();
  char* __stdcall SaveString(const char* s, int length = -1);
  char* __stdcall Sprintf(const char* fmt, ...);
  char* __stdcall VSprintf(const char* fmt, void* val);
  void __stdcall ThrowError(const char* fmt, ...);
  void __stdcall AddFunction(const char* name, const char* params, ApplyFunc apply, void* user_data=0);
  bool __stdcall FunctionExists(const char* name);
  AVSValue __stdcall Invoke(const char* name, const AVSValue args, const char* const* arg_names=0);
  AVSValue __stdcall GetVar(const char* name);
  bool __stdcall SetVar(const char* name, const AVSValue& val);
  bool __stdcall SetGlobalVar(const char* name, const AVSValue& val);
  void __stdcall PushContext(int level=0);
  void __stdcall PopContext();
  void __stdcall PopContextGlobal();
  PVideoFrame __stdcall NewVideoFrame(const VideoInfo& vi, int align);
  PVideoFrame NewVideoFrame(int row_size, int height, int align);
  PVideoFrame NewPlanarVideoFrame(int row_size, int height, int row_sizeUV, int heightUV, int align, bool U_first);
  bool __stdcall MakeWritable(PVideoFrame* pvf);
  void __stdcall BitBlt(BYTE* dstp, int dst_pitch, const BYTE* srcp, int src_pitch, int row_size, int height);
  void __stdcall AtExit(IScriptEnvironment::ShutdownFunc function, void* user_data);
  PVideoFrame __stdcall Subframe(PVideoFrame src, int rel_offset, int new_pitch, int new_row_size, int new_height);
  int __stdcall SetMemoryMax(int mem);
  int __stdcall SetWorkingDir(const char * newdir);
  __stdcall ~ScriptEnvironment();
  void* __stdcall ManageCache(int key, void* data);
  bool __stdcall PlanarChromaAlignment(IScriptEnvironment::PlanarChromaAlignmentMode key);
  PVideoFrame __stdcall SubframePlanar(PVideoFrame src, int rel_offset, int new_pitch, int new_row_size, int new_height, int rel_offsetU, int rel_offsetV, int new_pitchUV);
  void __stdcall DeleteScriptEnvironment();
  void _stdcall ApplyMessage(PVideoFrame* frame, const VideoInfo& vi, const char* message, int size, int textcolor, int halocolor, int bgcolor);
  const AVS_Linkage* const __stdcall GetAVSLinkage();

  /* IScriptEnvironment2 */
  virtual bool  __stdcall GetVar(const char* name, AVSValue *val) const;
  virtual bool __stdcall GetVar(const char* name, bool def) const;
  virtual int  __stdcall GetVar(const char* name, int def) const;
  virtual double  __stdcall GetVar(const char* name, double def) const;
  virtual const char*  __stdcall GetVar(const char* name, const char* def) const;
  virtual bool __stdcall LoadPlugin(const char* filePath, bool throwOnError, AVSValue *result);
  virtual void __stdcall AddAutoloadDir(const char* dirPath, bool toFront);
  virtual void __stdcall ClearAutoloadDirs();
  virtual void __stdcall AutoloadPlugins();
  virtual void __stdcall AddFunction(const char* name, const char* params, ApplyFunc apply, void* user_data, const char *exportVar);
  virtual bool __stdcall InternalFunctionExists(const char* name);
  virtual int __stdcall IncrImportDepth();
  virtual int __stdcall DecrImportDepth();
  virtual void __stdcall SetPrefetcher(Prefetcher *p);
  virtual void __stdcall AdjustMemoryConsumption(size_t amount, bool minus);
  virtual bool __stdcall Invoke(AVSValue *result, const char* name, const AVSValue& args, const char* const* arg_names=0);
  virtual void __stdcall SetFilterMTMode(const char* filter, MtMode mode, bool force);
  virtual MtMode __stdcall GetFilterMTMode(const char* filter, bool* is_forced) const;
  virtual void __stdcall ParallelJob(ThreadWorkerFuncPtr jobFunc, void* jobData, IJobCompletion* completion);
  virtual IJobCompletion* __stdcall NewCompletion(size_t capacity);
  virtual size_t  __stdcall GetProperty(AvsEnvProperty prop);
  virtual void* __stdcall Allocate(size_t nBytes, size_t alignment, AvsAllocType type);
  virtual void __stdcall Free(void* ptr);

private:

  // Tritical May 2005
  // Note order here!!
  // AtExiter has functions which
  // rely on StringDump elements.
  StringDump string_dump;
  std::mutex string_mutex;
  char * vsprintf_buf;
  size_t vsprintf_len;

  AtExiter at_exit;
  ThreadPool * thread_pool;

  PluginManager *plugin_manager;

  VarTable* global_var_table;
  VarTable* var_table;

  int ImportDepth;

  const AVSFunction* Lookup(const char* search_name, const AVSValue* args, size_t num_args,
                      bool &pstrict, size_t args_names_count, const char* const* arg_names);
  void EnsureMemoryLimit(size_t request);
  unsigned __int64 memory_max;
  std::atomic<unsigned __int64> memory_used;

  void ExportBuiltinFilters();

  IScriptEnvironment2* This() { return this; }
  bool PlanarChromaAlignmentState;

  HRESULT hrfromcoinit;
  DWORD coinitThreadId;

  bool closing;                 // Used to avoid deadlock, if vartable is being accessed while shutting down (Popcontext)

  typedef std::multimap<size_t, VideoFrame*> FrameRegistryType;
  typedef mapped_list<Cache*> CacheRegistryType;
  FrameRegistryType FrameRegistry;
  CacheRegistryType CacheRegistry;
  Cache* FrontCache;
  VideoFrame* GetNewFrame(size_t vfb_size);
  VideoFrame* AllocateFrame(size_t vfb_size);
  std::mutex memory_mutex;

  BufferPool BufferPool;

  MTMapState MTMap;
  typedef std::vector<MTGuard*> MTGuardRegistryType;
  MTGuardRegistryType MTGuardRegistry;
  Prefetcher *prefetcher;
};


static unsigned __int64 ConstrainMemoryRequest(unsigned __int64 requested)
{
  // Get system memory information
  MEMORYSTATUSEX memstatus;
  memstatus.dwLength = sizeof(memstatus);
  GlobalMemoryStatusEx(&memstatus);

  // mem_limit is the largest amount of memory that makes sense to use.
  // We don't want to use more than the virtual address space,
  // and we also don't want to start paging to disk.
  unsigned __int64 mem_limit = min(memstatus.ullTotalVirtual, memstatus.ullTotalPhys);

  unsigned __int64 mem_sysreserve = 0;
  if (memstatus.ullTotalPhys > memstatus.ullTotalVirtual)
  {
    // We are probably running on a 32bit OS system where the virtual space is capped to 
    // much less than what the system can use, so it is enough to reserve only a small amount.
    mem_sysreserve = 128*1024*1024ull;  
  }
  else
  {
    // We could probably use up all the RAM in our single application, 
    // so reserve more to leave some RAM for other apps and the OS too.
    mem_sysreserve = 1024*1024*1024ull;
  }

  // Cap memory_max to at most mem_sysreserve less than total, but at least to 64MB.
  return clamp(requested, 64*1024*1024ull, mem_limit - mem_sysreserve);
}

IJobCompletion* __stdcall ScriptEnvironment::NewCompletion(size_t capacity)
{
  return new JobCompletion(capacity);
}

ScriptEnvironment::ScriptEnvironment()
  : at_exit(),
    plugin_manager(NULL),
    vsprintf_buf(NULL),
    vsprintf_len(0),
    hrfromcoinit(E_FAIL), coinitThreadId(0),
    closing(false),
    PlanarChromaAlignmentState(true),   // Change to "true" for 2.5.7
    ImportDepth(0),
    thread_pool(NULL),
    prefetcher(NULL),
    FrontCache(NULL),
    BufferPool(this)
{
  try {
    // Make sure COM is initialised
    hrfromcoinit = CoInitialize(NULL);

    // If it was already init'd then decrement
    // the use count and leave it alone!
    if(hrfromcoinit == S_FALSE) {
      hrfromcoinit=E_FAIL;
      CoUninitialize();
    }
    // Remember our threadId.
    coinitThreadId=GetCurrentThreadId();

    MEMORYSTATUSEX memstatus;
    memstatus.dwLength = sizeof(memstatus);
    GlobalMemoryStatusEx(&memstatus);
    memory_max = ConstrainMemoryRequest(memstatus.ullTotalPhys / 4);
    memory_max = min(memory_max, 1024*1024*1024ull);  // at start, cap memory usage to 1GB
    memory_used = 0ull;

    global_var_table = new VarTable(0, 0);
    var_table = new VarTable(0, global_var_table);
    global_var_table->Set("true", true);
    global_var_table->Set("false", false);
    global_var_table->Set("yes", true);
    global_var_table->Set("no", false);
    global_var_table->Set("last", AVSValue());

    global_var_table->Set("$ScriptName$", AVSValue());
    global_var_table->Set("$ScriptFile$", AVSValue());
    global_var_table->Set("$ScriptDir$",  AVSValue());

    global_var_table->Set("MT_NICE_FILTER",     (int)MT_NICE_FILTER);
    global_var_table->Set("MT_MULTI_INSTANCE",  (int)MT_MULTI_INSTANCE);
    global_var_table->Set("MT_SERIALIZED",      (int)MT_SERIALIZED);

    plugin_manager = new PluginManager(this);
    plugin_manager->AddAutoloadDir("USER_PLUS_PLUGINS", false);
    plugin_manager->AddAutoloadDir("MACHINE_PLUS_PLUGINS", false);
    plugin_manager->AddAutoloadDir("USER_CLASSIC_PLUGINS", false);
    plugin_manager->AddAutoloadDir("MACHINE_CLASSIC_PLUGINS", false);

    thread_pool = new ThreadPool(std::thread::hardware_concurrency());

    ExportBuiltinFilters();
  }
  catch (const AvisynthError &err) {
    if(SUCCEEDED(hrfromcoinit)) {
      hrfromcoinit=E_FAIL;
      CoUninitialize();
    }
    // Needs must, to not loose the text we
    // must leak a little memory.
    throw AvisynthError(_strdup(err.msg));
  }
}

ScriptEnvironment::~ScriptEnvironment() {

  closing = true;

  // Before we start to pull the world apart
  // give every one their last wish.
  at_exit.Execute(this);

  delete thread_pool;

  while (var_table)
    PopContext();

  while (global_var_table)
    PopContextGlobal();

  const FrameRegistryType::iterator end_it = FrameRegistry.end();
  for (
    FrameRegistryType::iterator it = FrameRegistry.begin();
    it != end_it;
    ++it)
  {
    VideoFrame *frame = it->second;
    assert(frame->refcount == 0);

    if (frame->vfb->refcount == 0)
      delete frame->vfb;

    delete frame;
  }

  delete plugin_manager;
  delete [] vsprintf_buf;

  // If we init'd COM and this is the right thread then release it
  // If it's the wrong threadId then tuff, nothing we can do.
  if(SUCCEEDED(hrfromcoinit) && (coinitThreadId == GetCurrentThreadId())) {
    hrfromcoinit=E_FAIL;
    CoUninitialize();
  }
}

void __stdcall ScriptEnvironment::SetPrefetcher(Prefetcher *p)
{
  if (prefetcher != NULL)
    throw AvisynthError("Only a single prefetcher is allowed per script.");

  // Make this the active prefetcher
  prefetcher = p;

  // Since this method basically enables MT operation,
  // upgrade all MTGuards to MT-mode.
  size_t nTotalThreads = 1 + p->NumPrefetchThreads();
  for (MTGuard* guard : MTGuardRegistry)
  {
    if (guard != NULL)
      guard->EnableMT(nTotalThreads);
  }
}

void __stdcall ScriptEnvironment::AdjustMemoryConsumption(size_t amount, bool minus)
{
  if (minus)
    memory_used -= amount;
  else
    memory_used += amount;
}

void __stdcall ScriptEnvironment::ParallelJob(ThreadWorkerFuncPtr jobFunc, void* jobData, IJobCompletion* completion)
{
  thread_pool->QueueJob(jobFunc, jobData, this, static_cast<JobCompletion*>(completion));
}

void __stdcall ScriptEnvironment::SetFilterMTMode(const char* filter, MtMode mode, bool force)
{

  if (streqi(filter, ""))
    filter = MTMapState::DEFAULT_MODE;

  MTMap.SetMode(filter, mode, force);
}

MtMode __stdcall ScriptEnvironment::GetFilterMTMode(const char* filter, bool* is_forced) const
{
  if (streqi(filter, ""))
    filter = MTMapState::DEFAULT_MODE;

  return MTMap.GetMode(filter, is_forced);
}

void* __stdcall ScriptEnvironment::Allocate(size_t nBytes, size_t alignment, AvsAllocType type)
{
  if ((type != AVS_NORMAL_ALLOC) && (type != AVS_POOLED_ALLOC))
    return NULL;
  return BufferPool.Allocate(nBytes, alignment, type == AVS_POOLED_ALLOC);
}

void __stdcall ScriptEnvironment::Free(void* ptr)
{
  BufferPool.Free(ptr);
}

/* This function adds information about builtin functions into global variables.
 * External utilities (like AvsPmod) can parse these variables and use them 
 * to learn about supported functions and their syntax.
 */
void ScriptEnvironment::ExportBuiltinFilters()
{
    std::string FunctionList;
    FunctionList.reserve(512);
    const size_t NumFunctionArrays = sizeof(builtin_functions)/sizeof(builtin_functions[0]);
    for (size_t i = 0; i < NumFunctionArrays; ++i)
    {
      for (const AVSFunction* f = builtin_functions[i]; f->name; ++f)
      {
        // This builds the $InternalFunctions$ variable, which is a list of space-delimited
        // function names. Utilities can learn the names of the builtin function from this.
        FunctionList.append(f->name);
        FunctionList.push_back(' ');

        // For each supported function, a global variable is added with <param_var_name> as the name,
        // and the list of parameters to that function as the value.
        std::string param_var_name;
        param_var_name.reserve(128);
        param_var_name.append("$Plugin!");
        param_var_name.append(f->name);
        param_var_name.append("!Param$");
        SetGlobalVar( SaveString(param_var_name.c_str(), param_var_name.length() + 1), AVSValue(f->param_types) );
      }
    }

    // Save $InternalFunctions$
    SetGlobalVar("$InternalFunctions$", AVSValue( SaveString(FunctionList.c_str(), FunctionList.length() + 1) ));
}

size_t  __stdcall ScriptEnvironment::GetProperty(AvsEnvProperty prop)
{
  switch(prop)
  {
  case AEP_FILTERCHAIN_THREADS:
    return (prefetcher != NULL) ? prefetcher->NumPrefetchThreads()+1 : 1;
  case AEP_PHYSICAL_CPUS:
    return GetNumPhysicalCPUs();
  case AEP_LOGICAL_CPUS:
    return std::thread::hardware_concurrency();
  case AEP_THREAD_ID:
    return 0;
  case AEP_THREADPOOL_THREADS:
    return thread_pool->NumThreads();
  case AEP_VERSION:
    return AVS_SEQREV;
  default:
    this->ThrowError("Invalid property request.");
    return std::numeric_limits<size_t>::max();
  }

  assert(0);
}

int __stdcall ScriptEnvironment::IncrImportDepth()
{
  ImportDepth++;
  return ImportDepth;
}
int __stdcall ScriptEnvironment::DecrImportDepth()
{
  ImportDepth--;
  return ImportDepth;
}

bool __stdcall ScriptEnvironment::LoadPlugin(const char* filePath, bool throwOnError, AVSValue *result)
{
  // Autoload needed to ensure that manual LoadPlugin() calls always override autoloaded plugins.
  // For that, autoloading must happen before any LoadPlugin(), so we force an 
  // autoload operation before any LoadPlugin().
  this->AutoloadPlugins();
  return plugin_manager->LoadPlugin(filePath, throwOnError, result);
}

void __stdcall ScriptEnvironment::AddAutoloadDir(const char* dirPath, bool toFront)
{
  plugin_manager->AddAutoloadDir(dirPath, toFront);
}

void __stdcall ScriptEnvironment::ClearAutoloadDirs()
{
  plugin_manager->ClearAutoloadDirs();
}

void __stdcall ScriptEnvironment::AutoloadPlugins()
{
  plugin_manager->AutoloadPlugins();
}

int ScriptEnvironment::SetMemoryMax(int mem) {

  if (mem > 0)  /* If mem is zero, we should just return current setting */
    memory_max = ConstrainMemoryRequest(mem * 1048576ull);

  return (int)(memory_max/1048576ull);
}

int ScriptEnvironment::SetWorkingDir(const char * newdir) {
  return SetCurrentDirectory(newdir) ? 0 : 1;
}

void ScriptEnvironment::CheckVersion(int version) {
  if (version > AVISYNTH_INTERFACE_VERSION)
    ThrowError("Plugin was designed for a later version of Avisynth (%d)", version);
}

int ScriptEnvironment::GetCPUFlags() { return ::GetCPUFlags(); }

void ScriptEnvironment::AddFunction(const char* name, const char* params, ApplyFunc apply, void* user_data) {
  this->AddFunction(name, params, apply, user_data, NULL);
}

void ScriptEnvironment::AddFunction(const char* name, const char* params, ApplyFunc apply, void* user_data, const char *exportVar) {
  plugin_manager->AddFunction(name, params, apply, user_data, exportVar);
}

// Throws if unsuccessfull
AVSValue ScriptEnvironment::GetVar(const char* name) {
  if (closing) return AVSValue();  // We easily risk  being inside the critical section below, while deleting variables.
  
  AVSValue val;
  if (var_table->Get(name, &val))
    return val;
  else
    throw IScriptEnvironment::NotFound();
}

bool ScriptEnvironment::GetVar(const char* name, AVSValue *ret) const {
  if (closing) return false;  // We easily risk  being inside the critical section below, while deleting variables.
  
  return var_table->Get(name, ret);
}

bool ScriptEnvironment::GetVar(const char* name, bool def) const {
  if (closing) return def;  // We easily risk  being inside the critical section below, while deleting variables.
  
  AVSValue val;
  if (this->GetVar(name, &val))
    return val.AsBool(def);
  else
    return def;
}

int ScriptEnvironment::GetVar(const char* name, int def) const {
  if (closing) return def;  // We easily risk  being inside the critical section below, while deleting variables.
  
  AVSValue val;
  if (this->GetVar(name, &val))
    return val.AsInt(def);
  else
    return def;
}

double ScriptEnvironment::GetVar(const char* name, double def) const {
  if (closing) return def;  // We easily risk  being inside the critical section below, while deleting variables.
  
  AVSValue val;
  if (this->GetVar(name, &val))
    return val.AsDblDef(def);
  else
    return def;
}

const char* ScriptEnvironment::GetVar(const char* name, const char* def) const {
  if (closing) return def;  // We easily risk  being inside the critical section below, while deleting variables.
  
  AVSValue val;
  if (this->GetVar(name, &val))
    return val.AsString(def);
  else
    return def;
}

bool ScriptEnvironment::SetVar(const char* name, const AVSValue& val) {
  if (closing) return true;  // We easily risk  being inside the critical section below, while deleting variables.

  return var_table->Set(name, val);
}

bool ScriptEnvironment::SetGlobalVar(const char* name, const AVSValue& val) {
  if (closing) return true;  // We easily risk  being inside the critical section below, while deleting variables.

  return global_var_table->Set(name, val);
}

VideoFrame* ScriptEnvironment::AllocateFrame(size_t vfb_size)
{
  if (vfb_size > (size_t)std::numeric_limits<int>::max())
  {
    throw AvisynthError(this->Sprintf("Requested buffer size of %zu is too large", vfb_size));
  }

  EnsureMemoryLimit(vfb_size);

  VideoFrameBuffer* vfb = NULL;
  try
  {
    vfb = new VideoFrameBuffer(vfb_size);
  }
  catch(const std::bad_alloc&)
  {
    return NULL;
  }

  VideoFrame *newFrame = NULL;
  try
  {
    newFrame = new VideoFrame(vfb, 0, 0, 0, 0);
  }
  catch(const std::bad_alloc&)
  {
    delete vfb;
    return NULL;
  }

  memory_used+=vfb_size;

  FrameRegistry.insert(FrameRegistryType::value_type(vfb_size, newFrame));

  return newFrame;
}

VideoFrame* ScriptEnvironment::GetNewFrame(size_t vfb_size)
{
  std::unique_lock<std::mutex> env_lock(memory_mutex);

  /* -----------------------------------------------------------
   *   Try to return an unused but already allocated instance
   * -----------------------------------------------------------
   */
  for (
    FrameRegistryType::iterator it = FrameRegistry.lower_bound(vfb_size), end_it = FrameRegistry.end();
    it != end_it;
  ++it)
  {
    VideoFrame *frame = it->second;
    assert((size_t)frame->vfb->GetDataSize() >= vfb_size);

    if ( (frame->refcount == 0)        // Nobody is using the frame
      && (frame->vfb->refcount == 0))  // And only this frame is using its vfb
    {
      InterlockedIncrement(&(frame->vfb->refcount));
      return frame;
      break;
    }
  }


  /* -----------------------------------------------------------
   *   No unused instance was found, try to allocate a new one
   * -----------------------------------------------------------
   */
  VideoFrame* frame = AllocateFrame(vfb_size);
  if ( frame != NULL)
    return frame;


  /* -----------------------------------------------------------
   * Couldn't allocate, try to free up unused frames of any size
   * -----------------------------------------------------------
   */
  for (
      FrameRegistryType::iterator it = FrameRegistry.begin(), end_it = FrameRegistry.end();
      (it != end_it) && (size_t(it->second->vfb->data_size) < vfb_size);
      )
  {
    VideoFrame *frame = it->second;
    assert((size_t)frame->vfb->GetDataSize() < vfb_size);

    if (frame->refcount == 0)
    {
      if (frame->vfb->refcount == 0)
      {
        memory_used -= frame->vfb->GetDataSize();
        delete frame->vfb;
      }

      delete frame;
      FrameRegistry.erase(it++);
    }
    else
    {
      ++it;
    }
  }


  /* -----------------------------------------------------------
   *   Try to allocate again
   * -----------------------------------------------------------
   */
  frame = AllocateFrame(vfb_size);
  if ( frame != NULL)
    return frame;


  /* -----------------------------------------------------------
   *   Oh boy...
   * -----------------------------------------------------------
   */
  ThrowError("Could not allocate video frame. Out of memory.");
  return NULL;
}

void ScriptEnvironment::EnsureMemoryLimit(size_t request)
{

  /* -----------------------------------------------------------
   *             Ensure SetMemoryMax limit is kept
   * -----------------------------------------------------------
   */

  // We reserve 15% for unaccounted stuff
  size_t memory_need = size_t((memory_used + request) / 0.85f);

  const CacheRegistryType::iterator end_cit = CacheRegistry.end();
  for (
        CacheRegistryType::iterator cit = CacheRegistry.begin();
        (memory_need > memory_max) && (cit != end_cit);
        ++cit
      )
  {
    // Oh darn. We'd need more memory than we are allowed to use.
    // Let's reduce the amount of caching.
    
    // We try to shrink least recently used caches first.

    Cache* cache = *cit;
    int cache_size = cache->SetCacheHints(CACHE_GET_SIZE, 0);
    if (cache_size != 0)
    {
      cache->SetCacheHints(CACHE_SET_MAX_CAPACITY, cache_size-1);

      /* -----------------------------------------------------------
       * Try to free up memory that we've just released from a cache
       * -----------------------------------------------------------
       */
      const FrameRegistryType::iterator end_fit = FrameRegistry.end();
      for (
          FrameRegistryType::iterator fit = FrameRegistry.begin();
          fit != end_fit;
          )
      {
        VideoFrame *frame = fit->second;

        if (frame->refcount == 0)
        {
          if (frame->vfb->refcount == 0)
          {
            memory_used -= frame->vfb->GetDataSize();
            delete frame->vfb;
          }

          delete frame;
          FrameRegistry.erase(fit++);
        }
        else
        {
          ++fit;
        }
      } // for fit
    } // if
  } // for cit
}

PVideoFrame ScriptEnvironment::NewPlanarVideoFrame(int row_size, int height, int row_sizeUV, int heightUV, int align, bool U_first)
{
  if (align < 0)
  {
    // Forced alignment
    align = -align;
  }
  else
  {
    align = max(align, FRAME_ALIGN);
  }

  int pitchUV;
  const int pitchY = AlignNumber(row_size, align);
  if (!PlanarChromaAlignmentState && (row_size == row_sizeUV*2) && (height == heightUV*2)) { // Meet old 2.5 series API expectations for YV12
    // Legacy alignment - pack Y as specified, pack UV half that
    pitchUV = (pitchY+1)>>1;  // UV plane, width = 1/2 byte per pixel - don't align UV planes seperately.
  }
  else {
    // Align planes separately
    pitchUV = AlignNumber(row_sizeUV, align);
  }

  int size = pitchY * height + 2 * pitchUV * heightUV;
  size = size + align -1;

  VideoFrame *res = GetNewFrame(size);

  int  offsetU, offsetV;
  const int offsetY = AlignPointer(res->vfb->GetWritePtr(), align) - res->vfb->GetWritePtr(); // first line offset for proper alignment
  if (U_first) {
    offsetU = offsetY + pitchY * height;
    offsetV = offsetY + pitchY * height + pitchUV * heightUV;
  } else {
    offsetV = offsetY + pitchY * height;
    offsetU = offsetY + pitchY * height + pitchUV * heightUV;
  }

  res->offset = offsetY;
  res->pitch = pitchY;
  res->row_size = row_size;
  res->height = height;
  res->offsetU = offsetU;
  res->offsetV = offsetV;
  res->pitchUV = pitchUV;
  res->row_sizeUV = row_sizeUV;
  res->heightUV = heightUV;

  return PVideoFrame(res);
}


PVideoFrame ScriptEnvironment::NewVideoFrame(int row_size, int height, int align)
{
  if (align < 0)
  {
    // Forced alignment
    align = -align;
  }
  else
  {
    align = max(align, FRAME_ALIGN);
  }

  const int pitch = AlignNumber(row_size, align);
  int size = pitch * height;
  size = size + align - 1;

  VideoFrame *res = GetNewFrame(size);

  const int offset = AlignPointer(res->vfb->GetWritePtr(), align) - res->vfb->GetWritePtr(); // first line offset for proper alignment

  res->offset = offset;
  res->pitch = pitch;
  res->row_size = row_size;
  res->height = height;
  res->offsetU = offset;
  res->offsetV = offset;
  res->pitchUV = 0;
  res->row_sizeUV = 0;
  res->heightUV = 0;

  return PVideoFrame(res);
}


PVideoFrame __stdcall ScriptEnvironment::NewVideoFrame(const VideoInfo& vi, int align) {
  // Check requested pixel_type:
  switch (vi.pixel_type) {
    case VideoInfo::CS_BGR24:
    case VideoInfo::CS_BGR32:
    case VideoInfo::CS_YUY2:
    case VideoInfo::CS_Y8:
    case VideoInfo::CS_YV12:
    case VideoInfo::CS_YV16:
    case VideoInfo::CS_YV24:
    case VideoInfo::CS_YV411:
    case VideoInfo::CS_I420:
      break;
    default:
      ThrowError("Filter Error: Filter attempted to create VideoFrame with invalid pixel_type.");
  }

  PVideoFrame retval;

  if (vi.IsPlanar() && !vi.IsY8()) { // Planar requires different math ;)
    const int xmod  = 1 << vi.GetPlaneWidthSubsampling(PLANAR_U);
    const int xmask = xmod - 1;
    if (vi.width & xmask)
      ThrowError("Filter Error: Attempted to request a planar frame that wasn't mod%d in width!", xmod);

    const int ymod  = 1 << vi.GetPlaneHeightSubsampling(PLANAR_U);
    const int ymask = ymod - 1;
    if (vi.height & ymask)
      ThrowError("Filter Error: Attempted to request a planar frame that wasn't mod%d in height!", ymod);

    const int heightUV = vi.height >> vi.GetPlaneHeightSubsampling(PLANAR_U);
    retval=NewPlanarVideoFrame(vi.RowSize(PLANAR_Y), vi.height, vi.RowSize(PLANAR_U), heightUV, align, !vi.IsVPlaneFirst());
  }
  else {
    if ((vi.width&1)&&(vi.IsYUY2()))
      ThrowError("Filter Error: Attempted to request an YUY2 frame that wasn't mod2 in width.");

    retval=NewVideoFrame(vi.RowSize(), vi.height, align);
  }

  return retval;
}

bool ScriptEnvironment::MakeWritable(PVideoFrame* pvf) {
  const PVideoFrame& vf = *pvf;

  // If the frame is already writable, do nothing.
  if (vf->IsWritable())
    return false;

  // Otherwise, allocate a new frame (using NewVideoFrame) and
  // copy the data into it.  Then modify the passed PVideoFrame
  // to point to the new buffer.
  const int row_size = vf->GetRowSize();
  const int height   = vf->GetHeight();
  PVideoFrame dst;

  if (vf->GetPitch(PLANAR_U)) {  // we have no videoinfo, so we assume that it is Planar if it has a U plane.
    const int row_sizeUV = vf->GetRowSize(PLANAR_U);
    const int heightUV   = vf->GetHeight(PLANAR_U);

    dst = NewPlanarVideoFrame(row_size, height, row_sizeUV, heightUV, FRAME_ALIGN, false);  // Always V first on internal images
  } else {
    dst = NewVideoFrame(row_size, height, FRAME_ALIGN);
  }

  BitBlt(dst->GetWritePtr(), dst->GetPitch(), vf->GetReadPtr(), vf->GetPitch(), row_size, height);
  // Blit More planes (pitch, rowsize and height should be 0, if none is present)
  BitBlt(dst->GetWritePtr(PLANAR_V), dst->GetPitch(PLANAR_V), vf->GetReadPtr(PLANAR_V),
         vf->GetPitch(PLANAR_V), vf->GetRowSize(PLANAR_V), vf->GetHeight(PLANAR_V));
  BitBlt(dst->GetWritePtr(PLANAR_U), dst->GetPitch(PLANAR_U), vf->GetReadPtr(PLANAR_U),
         vf->GetPitch(PLANAR_U), vf->GetRowSize(PLANAR_U), vf->GetHeight(PLANAR_U));

  *pvf = dst;
  return true;
}


void ScriptEnvironment::AtExit(IScriptEnvironment::ShutdownFunc function, void* user_data) {
  at_exit.Add(function, user_data);
}

void ScriptEnvironment::PushContext(int level) {
  var_table = new VarTable(var_table, global_var_table);
}

void ScriptEnvironment::PopContext() {
  var_table = var_table->Pop();
}

void ScriptEnvironment::PopContextGlobal() {
  global_var_table = global_var_table->Pop();
}


PVideoFrame __stdcall ScriptEnvironment::Subframe(PVideoFrame src, int rel_offset, int new_pitch, int new_row_size, int new_height) {
  VideoFrame* subframe = src->Subframe(rel_offset, new_pitch, new_row_size, new_height);
  
  // TODO: Figure out why uncommenting this line causes problems
  //FrameRegistry.insert(FrameRegistryType::value_type(src->GetFrameBuffer()->GetDataSize(), subframe));

  return subframe;
}

//tsp June 2005 new function compliments the above function
PVideoFrame __stdcall ScriptEnvironment::SubframePlanar(PVideoFrame src, int rel_offset, int new_pitch, int new_row_size,
                                                        int new_height, int rel_offsetU, int rel_offsetV, int new_pitchUV) {
  VideoFrame* subframe = src->Subframe(rel_offset, new_pitch, new_row_size, new_height, rel_offsetU, rel_offsetV, new_pitchUV);

  // TODO: Figure out why uncommenting this line causes problems
  //FrameRegistry.insert(FrameRegistryType::value_type(src->GetFrameBuffer()->GetDataSize(), subframe));

  return subframe;
}

void* ScriptEnvironment::ManageCache(int key, void* data) {
// An extensible interface for providing system or user access to the
// ScriptEnvironment class without extending the IScriptEnvironment
// definition.

  switch((MANAGE_CACHE_KEYS)key)
  {
  // Called by Cache instances upon creation
  case MC_RegisterCache:
  {
    Cache* cache = reinterpret_cast<Cache*>(data);
    if (FrontCache != NULL)
      CacheRegistry.push_back(FrontCache);     
    FrontCache = cache;
    break;
  }
  // Called by Cache instances upon destruction
  case MC_UnRegisterCache:
  {
    Cache* cache = reinterpret_cast<Cache*>(data);
    if (FrontCache == cache)
      FrontCache = NULL;
    else
      CacheRegistry.remove(cache);
    break;
  }
  // Called by Cache instances when they want to expand their limit
  case MC_NodAndExpandCache:
  {
    std::unique_lock<std::mutex> env_lock(memory_mutex);
    Cache* cache     = reinterpret_cast<Cache*>(data);

    // Nod
    if (cache != FrontCache)
    {
      CacheRegistry.move_to_back(cache);
    }

    // Given that we are within our memory limits,
    // try to expand the limit of those caches that
    // need it.
    // We try to expand most recently used caches first.

    int cache_cap    = cache->SetCacheHints(CACHE_GET_CAPACITY, 0);
    int cache_reqcap = cache->SetCacheHints(CACHE_GET_REQUESTED_CAP, 0);
    if (cache_reqcap <= cache_cap)
      return 0;

    if ((memory_used > memory_max) || (memory_max - memory_used < memory_max*0.1f))
    {
      // If we don't have enough free reserves, take away a cache slot from
      // a cache instance that hasn't been used since long.
      for (Cache* old_cache : CacheRegistry)
      {
        int osize = cache->SetCacheHints(CACHE_GET_SIZE, 0);
        if (osize != 0)
        {
          old_cache->SetCacheHints(CACHE_SET_MAX_CAPACITY, osize-1);
          break;
        }
      } // for cit
    }
      
    cache->SetCacheHints(CACHE_SET_MAX_CAPACITY, cache_cap+1);

    break;
  }
  // Called by Cache instances when they are accessed
  case MC_NodCache:
  {
    Cache* cache = reinterpret_cast<Cache*>(data);
    if (cache == FrontCache)
      return 0;

    std::unique_lock<std::mutex> env_lock(memory_mutex);
    CacheRegistry.move_to_back(cache);
    break;
  } // case
  case MC_RegisterMTGuard:
  {
    MTGuard* guard = reinterpret_cast<MTGuard*>(data);
    MTGuardRegistry.push_back(guard);
    break;
  }
  case MC_UnRegisterMTGuard:
  {
    MTGuard* guard = reinterpret_cast<MTGuard*>(data);
    for (auto& item : MTGuardRegistry)
    {
      if (item == guard)
      {
        item = NULL;
        break;
      }
    }
    break;
  }
  } // switch
  return 0;
}


bool ScriptEnvironment::PlanarChromaAlignment(IScriptEnvironment::PlanarChromaAlignmentMode key){
  bool oldPlanarChromaAlignmentState = PlanarChromaAlignmentState;

  switch (key)
  {
  case IScriptEnvironment::PlanarChromaAlignmentOff:
  {
    PlanarChromaAlignmentState = false;
    break;
  }
  case IScriptEnvironment::PlanarChromaAlignmentOn:
  {
    PlanarChromaAlignmentState = true;
    break;
  }
  default:
    break;
  }
  return oldPlanarChromaAlignmentState;
}

/* A helper for Invoke.
   Copy a nested array of 'src' into a flat array 'dst'.
   Returns the number of elements that have been written to 'dst'.
   If 'dst' is NULL, will still return the number of elements 
   that would have been written to 'dst', but will not actually write to 'dst'.
*/
static size_t Flatten(const AVSValue& src, AVSValue* dst, size_t index, const char* const* arg_names = NULL) {
  if (src.IsArray()) {
    const int array_size = src.ArraySize();
    for (int i=0; i<array_size; ++i) {
      if (!arg_names || arg_names[i] == 0)
        index = Flatten(src[i], dst, index);
    }
  } else {
    if (dst != NULL)
      dst[index] = src;
    ++index;
  }
  return index;
}

const AVSFunction* ScriptEnvironment::Lookup(const char* search_name, const AVSValue* args, size_t num_args,
                    bool &pstrict, size_t args_names_count, const char* const* arg_names)
{
  const AVSFunction *result = NULL;

  size_t oanc;
  do {
    for (int strict = 1; strict >= 0; --strict) {
      pstrict = strict&1;

      // first, look in loaded plugins
      result = plugin_manager->Lookup(search_name, args, num_args, pstrict, args_names_count, arg_names);
      if (result)
        return result;

      // then, look for a built-in function
      for (int i = 0; i < sizeof(builtin_functions)/sizeof(builtin_functions[0]); ++i)
        for (const AVSFunction* j = builtin_functions[i]; j->name; ++j)
          if (streqi(j->name, search_name) &&
              AVSFunction::TypeMatch(j->param_types, args, num_args, pstrict, this) &&
              AVSFunction::ArgNameMatch(j->param_types, args_names_count, arg_names))
            return j;
    }
    // Try again without arg name matching
    oanc = args_names_count;
    args_names_count = 0;
  } while (oanc);

  // If we got here it means the function has not been found.
  // If we haven't done so yet, load the plugins in the autoload folders
  // and try again.
  if (!plugin_manager->HasAutoloadExecuted())
  {
    plugin_manager->AutoloadPlugins();
    return Lookup(search_name, args, num_args, pstrict, args_names_count, arg_names);
  }

  return NULL;
}

AVSValue ScriptEnvironment::Invoke(const char* name, const AVSValue args, const char* const* arg_names)
{
  AVSValue result;
  if (!Invoke(&result, name, args, arg_names))
  {
    throw NotFound();
  }

  return result;
}

bool __stdcall ScriptEnvironment::Invoke(AVSValue *result, const char* name, const AVSValue& args, const char* const* arg_names)
{
  bool strict = false;
  const AVSFunction *f;

  const int args_names_count = (arg_names && args.IsArray()) ? args.ArraySize() : 0;

  // get how many args we will need to store
  size_t args2_count = Flatten(args, NULL, 0, arg_names);
  if (args2_count > ScriptParser::max_args)
    ThrowError("Too many arguments passed to function (max. is %d)", ScriptParser::max_args);

  // flatten unnamed args
  std::vector<AVSValue> args2(args2_count, AVSValue());
  Flatten(args, args2.data(), 0, arg_names);

  // find matching function
  f = this->Lookup(name, args2.data(), args2_count, strict, args_names_count, arg_names);
  if (!f)
    return false;

  // combine unnamed args into arrays
  size_t src_index=0, dst_index=0;
  const char* p = f->param_types;
  const size_t maxarg3 = max(args2_count, strlen(p)); // well it can't be any longer than this.

  std::vector<AVSValue> args3(maxarg3, AVSValue());

  while (*p) {
    if (*p == '[') {
      p = strchr(p+1, ']');
      if (!p) break;
      p++;
    } else if ((p[1] == '*') || (p[1] == '+')) {
      size_t start = src_index;
      while ((src_index < args2_count) && (AVSFunction::SingleTypeMatch(*p, args2[src_index], strict)))
        src_index++;
      size_t size = src_index - start;
      assert(args2_count >= size);

      // Even if the AVSValue below is an array of zero size, we can't skip adding it to args3,
      // because filters like BlankClip might still be expecting it.
      args3[dst_index++] = AVSValue(size > 0 ? args2.data()+start : NULL, size); // can't delete args2 early because of this

      p += 2;
    } else {
      if (src_index < args2_count)
        args3[dst_index] = args2[src_index];
      src_index++;
      dst_index++;
      p++;
    }
  }
  if (src_index < args2_count)
    ThrowError("Too many arguments to function %s", name);

  const int args3_count = (int)dst_index;

  // copy named args
  for (int i=0; i<args_names_count; ++i) {
    if (arg_names[i]) {
      size_t named_arg_index = 0;
      for (const char* p = f->param_types; *p; ++p) {
        if (*p == '*' || *p == '+') {
          continue;   // without incrementing named_arg_index
        } else if (*p == '[') {
          p += 1;
          const char* q = strchr(p, ']');
          if (!q) break;
          if (strlen(arg_names[i]) == size_t(q-p) && !strnicmp(arg_names[i], p, q-p)) {
            // we have a match
            if (args3[named_arg_index].Defined()) {
              ThrowError("Script error: the named argument \"%s\" was passed more than once to %s", arg_names[i], name);
            } else if (args[i].IsArray()) {
              ThrowError("Script error: can't pass an array as a named argument");
            } else if (args[i].Defined() && !AVSFunction::SingleTypeMatch(q[1], args[i], false)) {
              ThrowError("Script error: the named argument \"%s\" to %s had the wrong type", arg_names[i], name);
            } else {
              args3[named_arg_index] = args[i];
              goto success;
            }
          } else {
            p = q+1;
          }
        }
        named_arg_index++;
      }
      // failure
      ThrowError("Script error: %s does not have a named argument \"%s\"", name, arg_names[i]);
success:;
    }
  }
 
  // ... and we're finally ready to make the call
  args3.resize(args3_count);
  std::vector<AVSValue>(args3).swap(args3);

  if (f->IsScriptFunction())
  {
    AVSValue funcArgs(args3.data(), args3.size());
    *result = f->apply(funcArgs, f->user_data, this);
  }
  else
  {
    *result = Cache::Create(MTGuard::Create(f, &args2, &args3, this), NULL, this);
    // args2 and args3 are not valid after this point anymore
  }
  
  return true;
}


bool ScriptEnvironment::FunctionExists(const char* name)
{
  // Look among internal functions
  if (InternalFunctionExists(name))
    return true;

  // Look among plugin functions
  if (plugin_manager->FunctionExists(name))
    return true;

  // Uhh... maybe if we load the plugins we'll have the function
  if (!plugin_manager->HasAutoloadExecuted())
  {
    plugin_manager->AutoloadPlugins();
    return this->FunctionExists(name);
  }

  return false;
}

bool __stdcall ScriptEnvironment::InternalFunctionExists(const char* name)
{
  for (int i = 0; i < sizeof(builtin_functions)/sizeof(builtin_functions[0]); ++i)
    for (const AVSFunction* j = builtin_functions[i]; j->name; ++j)
      if (!lstrcmpi(j->name, name))
        return true;

  return false;
}

void ScriptEnvironment::BitBlt(BYTE* dstp, int dst_pitch, const BYTE* srcp, int src_pitch, int row_size, int height) {
  if (height<0)
    ThrowError("Filter Error: Attempting to blit an image with negative height.");
  if (row_size<0)
    ThrowError("Filter Error: Attempting to blit an image with negative row size.");
  ::BitBlt(dstp, dst_pitch, srcp, src_pitch, row_size, height);
}


char* ScriptEnvironment::SaveString(const char* s, int len) {
  std::lock_guard<std::mutex> lock(string_mutex);
  return string_dump.SaveString(s, len);
}


char* ScriptEnvironment::VSprintf(const char* fmt, void* val) {
  std::lock_guard<std::mutex> lock(string_mutex);

  char*& buf = vsprintf_buf;
  size_t& size = vsprintf_len;

  int count = _vsnprintf(buf, size, fmt, (va_list)val);
  while ((count < 0) || (count >= (int)size))
  {
    delete[] buf;
    size += 4096;
    buf = new(std::nothrow) char[size];
    if (!buf) return NULL;
    count = _vsnprintf(buf, size, fmt, (va_list)val);
  }
  return string_dump.SaveString(buf, count); // SaveString will add the NULL in len mode.
}

char* ScriptEnvironment::Sprintf(const char* fmt, ...) {
  va_list val;
  va_start(val, fmt);
  char* result = ScriptEnvironment::VSprintf(fmt, val);
  va_end(val);
  return result;
}


void ScriptEnvironment::ThrowError(const char* fmt, ...) {
  char buf[8192];
  va_list val;
  va_start(val, fmt);
  try {
    _vsnprintf(buf, sizeof(buf)-1, fmt, val);
    if (!this) throw this; // Force inclusion of try catch code!
  } catch (...) {
    strcpy(buf,"Exception while processing ScriptEnvironment::ThrowError().");
  }
  va_end(val);
  buf[sizeof(buf)-1] = '\0';
  throw AvisynthError(ScriptEnvironment::SaveString(buf));
}


extern void ApplyMessage(PVideoFrame* frame, const VideoInfo& vi,
  const char* message, int size, int textcolor, int halocolor, int bgcolor,
  IScriptEnvironment* env);


const AVS_Linkage* const __stdcall ScriptEnvironment::GetAVSLinkage() {
  extern const AVS_Linkage* const AVS_linkage; // In interface.cpp

  return AVS_linkage;
}


void _stdcall ScriptEnvironment::ApplyMessage(PVideoFrame* frame, const VideoInfo& vi, const char* message, int size, int textcolor, int halocolor, int bgcolor) {
  ::ApplyMessage(frame, vi, message, size, textcolor, halocolor, bgcolor, this);
}


void __stdcall ScriptEnvironment::DeleteScriptEnvironment() {
  // Provide a method to delete this ScriptEnvironment in
  // the same malloc context in which it was created below.
  delete this;
}


IScriptEnvironment* __stdcall CreateScriptEnvironment(int version) {
  return CreateScriptEnvironment2(version);
}

IScriptEnvironment2* __stdcall CreateScriptEnvironment2(int version) {
  if (version <= AVISYNTH_INTERFACE_VERSION)
    return new ScriptEnvironment();
  else
    return NULL;
}
