// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/base/Thread.h>
#include <muduo/base/CurrentThread.h>
#include <muduo/base/Exception.h>
#include <muduo/base/Logging.h>

#include <boost/static_assert.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/weak_ptr.hpp>

#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/unistd.h>

namespace muduo
{
namespace CurrentThread
{
  __thread int t_cachedTid = 0;      //���������id
  __thread char t_tidString[32];
  __thread int t_tidStringLength = 6;
  __thread const char* t_threadName = "unknown";
  const bool sameType = boost::is_same<int, pid_t>::value;
  BOOST_STATIC_ASSERT(sameType);
}

namespace detail
{

pid_t gettid()
{
  return static_cast<pid_t>(::syscall(SYS_gettid));     //ϵͳ���û�ȡ
}

void afterFork()        //fork֮���ɨս��
{
  muduo::CurrentThread::t_cachedTid = 0;
  muduo::CurrentThread::t_threadName = "main";
  CurrentThread::tid();
  // no need to call pthread_atfork(NULL, NULL, &afterFork);
}

class ThreadNameInitializer   //�߳�����ʼ��
{
 public:
  ThreadNameInitializer()
  {
    muduo::CurrentThread::t_threadName = "main";
    CurrentThread::tid();
    pthread_atfork(NULL, NULL, &afterFork);
  }
};

ThreadNameInitializer init;

struct ThreadData    //�߳������࣬�۲���ģʽ
{
  typedef muduo::Thread::ThreadFunc ThreadFunc;   
  ThreadFunc func_;
  string name_;
  boost::weak_ptr<pid_t> wkTid_;     

  ThreadData(const ThreadFunc& func,
             const string& name,
             const boost::shared_ptr<pid_t>& tid)    //������Thread���shared_ptr
    : func_(func),
      name_(name),
      wkTid_(tid)
  { }

  void runInThread()    //�߳�����
  {
    pid_t tid = muduo::CurrentThread::tid();  //�õ��߳�tid

    boost::shared_ptr<pid_t> ptid = wkTid_.lock();  //���Խ�weak_ptr����Ϊshared_ptr��thread_safe
    if (ptid)     //����ɹ�
    {
      *ptid = tid;    
      ptid.reset();   //�ɹ���֮���ͨ������ָ���޸ĸ����߳�id������ʱshared_ptr���ٵ�
    }

    muduo::CurrentThread::t_threadName = name_.empty() ? "muduoThread" : name_.c_str();
    ::prctl(PR_SET_NAME, muduo::CurrentThread::t_threadName);
    try
    {
      func_();    //�����߳����к���
      muduo::CurrentThread::t_threadName = "finished";   //�������threadname
    }
    catch (const Exception& ex)   //Exception�쳣
    {
      muduo::CurrentThread::t_threadName = "crashed";
      fprintf(stderr, "exception caught in Thread %s\n", name_.c_str());
      fprintf(stderr, "reason: %s\n", ex.what());
      fprintf(stderr, "stack trace: %s\n", ex.stackTrace());   //��ӡ��������ջ
      abort();
    }   
    catch (const std::exception& ex)      //��׼�쳣
    {
      muduo::CurrentThread::t_threadName = "crashed";
      fprintf(stderr, "exception caught in Thread %s\n", name_.c_str());
      fprintf(stderr, "reason: %s\n", ex.what());
      abort();
    }
    catch (...)   //����
    {
      muduo::CurrentThread::t_threadName = "crashed";
      fprintf(stderr, "unknown exception caught in Thread %s\n", name_.c_str());
      throw; // rethrow                 //�ٴ��׳�
    }
  }
};

void* startThread(void* obj)   //�߳�����
{
  ThreadData* data = static_cast<ThreadData*>(obj);
  data->runInThread();
  delete data;
  return NULL;
}

}
}

using namespace muduo;

void CurrentThread::cacheTid()   //�������һ�λỺ��tid��������ÿ�ζ�systemcall�������Ч��
{
  if (t_cachedTid == 0)
  {
    t_cachedTid = detail::gettid();
    t_tidStringLength = snprintf(t_tidString, sizeof t_tidString, "%5d ", t_cachedTid);
  }
}

bool CurrentThread::isMainThread()
{
  return tid() == ::getpid();
}

void CurrentThread::sleepUsec(int64_t usec)    //����
{
  struct timespec ts = { 0, 0 };
  ts.tv_sec = static_cast<time_t>(usec / Timestamp::kMicroSecondsPerSecond);
  ts.tv_nsec = static_cast<long>(usec % Timestamp::kMicroSecondsPerSecond * 1000);
  ::nanosleep(&ts, NULL);
}

AtomicInt32 Thread::numCreated_;

Thread::Thread(const ThreadFunc& func, const string& n)
  : started_(false),
    joined_(false),
    pthreadId_(0),
    tid_(new pid_t(0)),
    func_(func),
    name_(n)
{
  setDefaultName();
}

#ifdef __GXX_EXPERIMENTAL_CXX0X__     //C++11��׼
Thread::Thread(ThreadFunc&& func, const string& n)
  : started_(false),
    joined_(false),
    pthreadId_(0),
    tid_(new pid_t(0)),
    func_(std::move(func)),
    name_(n)
{
  setDefaultName();
}

#endif
//pthread_join()��pthread_detach()���Ƿ�ֹ�ֳ���Դй¶��;����join()�������ȴ���
Thread::~Thread()    //��������������̰߳�ȫ�ġ�����ʱȷ��threadû��join���Ż�ִ�����������̵߳���������ȴ��߳̽���
{                                    //���thread������������ڳ����̣߳���ô����ͨ��join�ȴ��߳̽���������thread��������ʱ���Զ�detach�̣߳���ֹ��Դй¶
  if (started_ && !joined_)   //���û��join����detach������ù��ˣ��Ͳ����ˡ�
  {
    pthread_detach(pthreadId_);
  }
}

void Thread::setDefaultName()   //�൱�ڸ�û�����ֵ��߳�������� "Thread %d"
{
  int num = numCreated_.incrementAndGet();  //ԭ�Ӳ���
  if (name_.empty())
  {
    char buf[32];
    snprintf(buf, sizeof buf, "Thread%d", num);
    name_ = buf;
  }
}

void Thread::start()   //�߳�����
{
  assert(!started_);
  started_ = true;
  // FIXME: move(func_)
  detail::ThreadData* data = new detail::ThreadData(func_, name_, tid_);
  if (pthread_create(&pthreadId_, NULL, &detail::startThread, data))
  {
    started_ = false;
    delete data; // or no delete?
    LOG_SYSFATAL << "Failed in pthread_create";
  }
}

int Thread::join()    //�ȴ��߳�
{
  assert(started_);
  assert(!joined_);
  joined_ = true;
  return pthread_join(pthreadId_, NULL);
}

