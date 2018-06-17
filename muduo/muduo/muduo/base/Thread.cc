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
  __thread int t_cachedTid = 0;      //用来缓存的id
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
  return static_cast<pid_t>(::syscall(SYS_gettid));     //系统调用获取
}

void afterFork()        //fork之后打扫战场
{
  muduo::CurrentThread::t_cachedTid = 0;
  muduo::CurrentThread::t_threadName = "main";
  CurrentThread::tid();
  // no need to call pthread_atfork(NULL, NULL, &afterFork);
}

class ThreadNameInitializer   //线程名初始化
{
 public:
  ThreadNameInitializer()
  {
    muduo::CurrentThread::t_threadName = "main";
    CurrentThread::tid();
    pthread_atfork(NULL, NULL, &afterFork);
  }
};

//全部变量类，这个对象构造先于main函数，当我们的程序引入这个库时，这个全局函数直接构造，我们程序的main()函数还没有执行。
ThreadNameInitializer init;

struct ThreadData    //线程数据类，观察者模式
{
  typedef muduo::Thread::ThreadFunc ThreadFunc;   
  ThreadFunc func_;
  string name_;
  boost::weak_ptr<pid_t> wkTid_;     

  ThreadData(const ThreadFunc& func,
             const string& name,
             const boost::shared_ptr<pid_t>& tid)    //保存有Thread类的shared_ptr
    : func_(func),
      name_(name),
      wkTid_(tid)
  { }

  void runInThread()    //线程运行
  {
    pid_t tid = muduo::CurrentThread::tid();  //得到线程tid

    boost::shared_ptr<pid_t> ptid = wkTid_.lock();  //尝试将weak_ptr提升为shared_ptr，thread_safe
    if (ptid)     //如果成功
    {
      *ptid = tid;    
      ptid.reset();   //成功了之后把通过智能指针修改父类线程id，该临时shared_ptr销毁掉
    }

    muduo::CurrentThread::t_threadName = name_.empty() ? "muduoThread" : name_.c_str();
    ::prctl(PR_SET_NAME, muduo::CurrentThread::t_threadName);
    try
    {
      func_();    //运行线程运行函数
      muduo::CurrentThread::t_threadName = "finished";   //运行玩的threadname
    }
    catch (const Exception& ex)   //Exception异常
    {
      muduo::CurrentThread::t_threadName = "crashed";
      fprintf(stderr, "exception caught in Thread %s\n", name_.c_str());
      fprintf(stderr, "reason: %s\n", ex.what());
      fprintf(stderr, "stack trace: %s\n", ex.stackTrace());   //打印函数调用栈
      abort();
    }   
    catch (const std::exception& ex)      //标准异常
    {
      muduo::CurrentThread::t_threadName = "crashed";
      fprintf(stderr, "exception caught in Thread %s\n", name_.c_str());
      fprintf(stderr, "reason: %s\n", ex.what());
      abort();
    }
    catch (...)   //其他
    {
      muduo::CurrentThread::t_threadName = "crashed";
      fprintf(stderr, "unknown exception caught in Thread %s\n", name_.c_str());
      throw; // rethrow                 //再次抛出
    }
  }
};

void* startThread(void* obj)   //线程启动
{
  ThreadData* data = static_cast<ThreadData*>(obj);
  data->runInThread();
  delete data;
  return NULL;
}

}
}

using namespace muduo;

void CurrentThread::cacheTid()   //在这里第一次会缓存tid，并不会每次都systemcall，提高了效率
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

void CurrentThread::sleepUsec(int64_t usec)    //休眠
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

#ifdef __GXX_EXPERIMENTAL_CXX0X__     //C++11标准
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
//pthread_join()和pthread_detach()都是防止现成资源泄露的途径，join()会阻塞等待。
Thread::~Thread()    //这个析构函数是线程安全的。析构时确认thread没有join，才会执行析构。即线程的析构不会等待线程结束
{                                    //如果thread对象的生命周期长于线程，那么可以通过join等待线程结束。否则thread对象析构时会自动detach线程，防止资源泄露
  if (started_ && !joined_)   //如果没有join，就detach，如果用过了，就不用了。
  {
    pthread_detach(pthreadId_);
  }
}

void Thread::setDefaultName()   //相当于给没有名字的线程起个名字 "Thread %d"
{
  int num = numCreated_.incrementAndGet();  //原子操作
  if (name_.empty())
  {
    char buf[32];
    snprintf(buf, sizeof buf, "Thread%d", num);
    name_ = buf;
  }
}

void Thread::start()   //线程启动
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

int Thread::join()    //等待线程
{
  assert(started_);
  assert(!joined_);
  joined_ = true;
  return pthread_join(pthreadId_, NULL);
}

