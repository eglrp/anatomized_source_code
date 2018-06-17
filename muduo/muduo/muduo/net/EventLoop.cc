// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/EventLoop.h>

#include <muduo/base/Logging.h>
#include <muduo/base/Mutex.h>
#include <muduo/net/Channel.h>
#include <muduo/net/Poller.h>
#include <muduo/net/SocketsOps.h>
#include <muduo/net/TimerQueue.h>

#include <boost/bind.hpp>

#include <signal.h>
#include <sys/eventfd.h>

using namespace muduo;
using namespace muduo::net;

namespace
{
__thread EventLoop* t_loopInThisThread = 0;   //线程局部存储的当前线程EventLoop对象指针，不会共享

const int kPollTimeMs = 10000;

//创建非阻塞的eventfd
int createEventfd()  
{
  int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (evtfd < 0)
  {
    LOG_SYSERR << "Failed in eventfd";
    abort();
  }
  return evtfd;
}

#pragma GCC diagnostic ignored "-Wold-style-cast"
class IgnoreSigPipe
{
 public:
  IgnoreSigPipe()
  {
    ::signal(SIGPIPE, SIG_IGN);
    // LOG_TRACE << "Ignore SIGPIPE";
  }
};
#pragma GCC diagnostic error "-Wold-style-cast"

IgnoreSigPipe initObj;
}

EventLoop* EventLoop::getEventLoopOfCurrentThread()
{
  return t_loopInThisThread;
}

EventLoop::EventLoop()
  : looping_(false),  //表示还未循环
    quit_(false),
    eventHandling_(false),
    callingPendingFunctors_(false),
    iteration_(0),
    threadId_(CurrentThread::tid()),   //赋值真实id
    poller_(Poller::newDefaultPoller(this)),   //构造了一个实际的poller对象
    timerQueue_(new TimerQueue(this)),
    wakeupFd_(createEventfd()),
    wakeupChannel_(new Channel(this, wakeupFd_)),
    currentActiveChannel_(NULL)
{
  LOG_DEBUG << "EventLoop created " << this << " in thread " << threadId_;
  if (t_loopInThisThread)  //每个线程最多一个EventLoop对象，如果已经存在，使用LOG_FATAL终止abort它。
  {
    LOG_FATAL << "Another EventLoop " << t_loopInThisThread
              << " exists in this thread " << threadId_;
  }
  else
  {
    t_loopInThisThread = this; //this赋给线程局部数据指针，凭借这个这以保证per thread a EventLoop
  }

  //设定wakeupChannel的回调函数，即EventLoop的handleRead函数，用来处理dopendingFunctors
  wakeupChannel_->setReadCallback(
      boost::bind(&EventLoop::handleRead, this));
  // we are always reading the wakeupfd
  wakeupChannel_->enableReading(); //同样是为了wakeupfd_，用来处理dopendingFunctors()
}

EventLoop::~EventLoop()
{
  LOG_DEBUG << "EventLoop " << this << " of thread " << threadId_
            << " destructs in thread " << CurrentThread::tid();
  wakeupChannel_->disableAll();
  wakeupChannel_->remove();
  ::close(wakeupFd_);
  t_loopInThisThread = NULL;   //赋空
}

//事件循环，不能跨线程调用
//只能在创建该对象的线程中调用
void EventLoop::loop()
{
  assert(!looping_);
  assertInLoopThread();  //断言处于创建该对象的线程中
  looping_ = true;
  quit_ = false;  // FIXME: what if someone calls quit() before loop() ?
  LOG_TRACE << "EventLoop " << this << " start looping";

  while (!quit_)
  {
    activeChannels_.clear();  //首先清零
    pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);  //调用poll返回活动的通道，有可能是唤醒返回的
    ++iteration_; 
    if (Logger::logLevel() <= Logger::TRACE)
    {
      printActiveChannels();  //日志登记，日志打印
    }
    // TODO sort channel by priority
    eventHandling_ = true;  //true
    for (ChannelList::iterator it = activeChannels_.begin();
        it != activeChannels_.end(); ++it)  //遍历通道来进行处理
    {
      currentActiveChannel_ = *it;
      currentActiveChannel_->handleEvent(pollReturnTime_);
    }
    currentActiveChannel_ = NULL;   //处理完了赋空
    eventHandling_ = false;  //false
 //I/O线程设计比较灵活，通过下面这个设计也能够进行计算任务，否则当I/O不是很繁忙的时候，这个I/O线程就一直处于阻塞状态。
 //我们需要让它也能执行一些计算任务 
    doPendingFunctors();   //处理用户回调任务
  }

  LOG_TRACE << "EventLoop " << this << " stop looping";
  looping_ = false;
}

//该函数可以跨线程调用
void EventLoop::quit()
{
  quit_ = true;
  // There is a chance that loop() just executes while(!quit_) and exits,
  // then EventLoop destructs, then we are accessing an invalid object.
  // Can be fixed using mutex_ in both places.
  if (!isInLoopThread())  //如果不是当前I/O线程调用的，我们需要唤醒该I/O线程
  {
    wakeup();   //唤醒，塔克能阻塞在poll中
  }
}

//顾名思义，在I/O线程中调用某个函数，该函数可以跨线程调用
void EventLoop::runInLoop(const Functor& cb)
{
  //如果是在I/O线程中调用，就同步调用cb回调函数
  if (isInLoopThread())
  {
    cb();
  }
  else
  {
  	//否则在其他线程中调用，就异步将cb添加到任务队列当中，以便让EventLoop真实对应的I/O线程执行这个回调函数
    queueInLoop(cb);
  }
}

//将任务添加到队列当中
void EventLoop::queueInLoop(const Functor& cb)
{
  {
  MutexLockGuard lock(mutex_);
  pendingFunctors_.push_back(cb);   //添加到任务队列当中
  }
  //如果当前调用queueInLoop调用不是I/O线程，那么唤醒该I/O线程，以便I/O线程及时处理。
  //或者调用的线程是当前I/O线程，并且此时调用pendingfunctor，需要唤醒
  //只有当前I/O线程的事件回调中调用queueInLoop才不需要唤醒
  if (!isInLoopThread() || callingPendingFunctors_)  
  {
    wakeup();
  }
}

//在时间戳为time的时间执行，0.0表示一次性不重复
TimerId EventLoop::runAt(const Timestamp& time, const TimerCallback& cb)
{
  return timerQueue_->addTimer(cb, time, 0.0);  
}

//延迟delay时间执行的定时器
TimerId EventLoop::runAfter(double delay, const TimerCallback& cb)
{
  Timestamp time(addTime(Timestamp::now(), delay));  //合成一个时间戳
  return runAt(time, cb);
}

//间隔性的定时器，起始就是重复定时器，间隔interval需要大于0
TimerId EventLoop::runEvery(double interval, const TimerCallback& cb)
{
  Timestamp time(addTime(Timestamp::now(), interval));
  return timerQueue_->addTimer(cb, time, interval);
}

#ifdef __GXX_EXPERIMENTAL_CXX0X__
// FIXME: remove duplication
void EventLoop::runInLoop(Functor&& cb)
{
  if (isInLoopThread())
  {
    cb();
  }
  else
  {
    queueInLoop(std::move(cb));
  }
}

void EventLoop::queueInLoop(Functor&& cb)
{
  {
  MutexLockGuard lock(mutex_);
  pendingFunctors_.push_back(std::move(cb));  // emplace_back
  }

  if (!isInLoopThread() || callingPendingFunctors_)
  {
    wakeup();
  }
}

TimerId EventLoop::runAt(const Timestamp& time, TimerCallback&& cb)
{
  return timerQueue_->addTimer(std::move(cb), time, 0.0);
}

TimerId EventLoop::runAfter(double delay, TimerCallback&& cb)
{
  Timestamp time(addTime(Timestamp::now(), delay));
  return runAt(time, std::move(cb));
}

TimerId EventLoop::runEvery(double interval, TimerCallback&& cb)
{
  Timestamp time(addTime(Timestamp::now(), interval));
  return timerQueue_->addTimer(std::move(cb), time, interval);
}
#endif

//直接调用timerQueue的cancle
void EventLoop::cancel(TimerId timerId)
{
  return timerQueue_->cancel(timerId);  
}

void EventLoop::updateChannel(Channel* channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();  //断言在EventLoop线程当中
  poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  if (eventHandling_)
  {
    assert(currentActiveChannel_ == channel ||
        std::find(activeChannels_.begin(), activeChannels_.end(), channel) == activeChannels_.end());
  }
  poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel* channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  return poller_->hasChannel(channel);
}

void EventLoop::abortNotInLoopThread()  //通过LOG_FATAL终止程序
{
  LOG_FATAL << "EventLoop::abortNotInLoopThread - EventLoop " << this
            << " was created in threadId_ = " << threadId_
            << ", current thread id = " <<  CurrentThread::tid();
}

//唤醒EventLoop
void EventLoop::wakeup()    
{
  uint64_t one = 1;                                                           
  ssize_t n = sockets::write(wakeupFd_, &one, sizeof one);  //随便写点数据进去就唤醒了
  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::wakeup() writes " << n << " bytes instead of 8";
  }
}

//实际上是wakeFd_的读回调函数
void EventLoop::handleRead()
{
  uint64_t one = 1;
  ssize_t n = sockets::read(wakeupFd_, &one, sizeof one);
  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::handleRead() reads " << n << " bytes instead of 8";
  }
}

// 1. 不是简单的在临界区内依次调用functor，而是把回调列表swap到functors中，这一方面减小了
//临界区的长度，意味着不会阻塞其他线程的queueInLoop()，另一方面也避免了死锁(因为Functor可能再次调用quueInLoop)
// 2. 由于doPendingFunctors()调用的Functor可能再次调用queueInLoop(cb)，这是queueInLoop()就必须wakeup(),否则新增的cb可能就不能及时调用了
// 3. muduo没有反复执行doPendingFunctors()直到pendingFunctors为空，这是有意的，否则I/O线程可能陷入死循环，无法处理I/O事件
void EventLoop::doPendingFunctors()
{
  std::vector<Functor> functors;
  callingPendingFunctors_ = true;

  //注意这里的临界区，这里使用了一个栈上变量functors和pendingFunctors交换
  {
  MutexLockGuard lock(mutex_);
  functors.swap(pendingFunctors_); //把它和空vector交换
  }

	//此处其它线程就可以往pendingFunctors添加任务

  //调用回调任务
  //这一部分不用临界区保护
  for (size_t i = 0; i < functors.size(); ++i)
  {
    functors[i]();
  }
  callingPendingFunctors_ = false;
}

//打印活跃通道表
void EventLoop::printActiveChannels() const
{
  for (ChannelList::const_iterator it = activeChannels_.begin();
      it != activeChannels_.end(); ++it)
  {
    const Channel* ch = *it;
    LOG_TRACE << "{" << ch->reventsToString() << "} ";
  }
}

