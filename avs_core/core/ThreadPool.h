#ifndef _AVS_THREADPOOL_H
#define _AVS_THREADPOOL_H

#include <avisynth.h>
#include <boost/thread.hpp>
#include <vector>

typedef boost::unique_future<AVSValue> AVSFuture;
typedef boost::promise<AVSValue> AVSPromise;

class JobCompletion : public IJobCompletion
{
private:
  const size_t max_jobs;
  size_t nJobs;

public:
  typedef std::pair<AVSPromise, AVSFuture> PromFutPair;
  PromFutPair *pairs;

  JobCompletion(size_t _max_jobs) :
    max_jobs(_max_jobs),
    nJobs(0),
    pairs(NULL)
  {
    pairs = new PromFutPair[max_jobs];

    // Initialize for first use
    nJobs = max_jobs;
    Reset();
  }

  AVSPromise* Add()
  {
    if (nJobs == max_jobs)
      throw new AvisynthError("This object is already full.");

    AVSPromise* ret = &(pairs[nJobs].first);
    ++nJobs;
    return ret;
  }

  virtual __stdcall ~JobCompletion()
  {
    delete [] pairs;
  }

  void __stdcall Wait() // TODO auto-reset 
  {
    for (size_t i = 0; i < nJobs; ++i)
      pairs[i].second.wait();
  }
  size_t __stdcall Size() const
  {
    return nJobs;
  }
  size_t __stdcall Capacity() const
  {
    return max_jobs;
  }
  bool __stdcall Finished() const
  {
    bool ret = true;
    for (size_t i = 0; i < nJobs; ++i)
      ret = ret && pairs[i].second.is_ready();
    return ret;
  }
  AVSValue __stdcall Get(size_t i)
  {
    return pairs[i].second.get();
  }
  void __stdcall Reset()
  {
    for (size_t i = 0; i < nJobs; ++i)
    {
      pairs[i].first = boost::move(AVSPromise());
      pairs[i].second = boost::move(pairs[i].first.get_future());
    }
    nJobs = 0;
  }
  void __stdcall Destroy()
  {
    delete this;
  }
};

class ThreadPoolPimpl;
class ThreadPool
{
private:
  ThreadPoolPimpl * const _pimpl;

public:
  ThreadPool(size_t nThreads);
  ~ThreadPool();

  void QueueJob(ThreadWorkerFuncPtr clb, void* params, IScriptEnvironment2 *env, JobCompletion *tc);
  size_t NumThreads() const;
};

#endif  // _AVS_THREADPOOL_H