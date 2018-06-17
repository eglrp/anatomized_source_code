// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include <muduo/net/TimerQueue.h>

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/Timer.h>
#include <muduo/net/TimerId.h>

#include <boost/bind.hpp>

#include <sys/timerfd.h>

namespace muduo
{
namespace net
{
namespace detail
{

int createTimerfd()
{
  int timerfd = ::timerfd_create(CLOCK_MONOTONIC,
                                 TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerfd < 0)
  {
    LOG_SYSFATAL << "Failed in timerfd_create";
  }
  return timerfd;
}

//计算超时时刻与当前时间的时间差
struct timespec howMuchTimeFromNow(Timestamp when)
{
  int64_t microseconds = when.microSecondsSinceEpoch()
                         - Timestamp::now().microSecondsSinceEpoch();  //超时时刻微秒数-当前时间微秒数
  if (microseconds < 100)  //不能小于100，精确度不需要
  {
    microseconds = 100;
  }
  struct timespec ts;  //转换成这个结构体返回
  ts.tv_sec = static_cast<time_t>(
      microseconds / Timestamp::kMicroSecondsPerSecond);
  ts.tv_nsec = static_cast<long>(
      (microseconds % Timestamp::kMicroSecondsPerSecond) * 1000);
  return ts;
}

//从timerfd读取，避免定时器事件一直触发
void readTimerfd(int timerfd, Timestamp now)
{
  uint64_t howmany;
  ssize_t n = ::read(timerfd, &howmany, sizeof howmany);  //从timerfd读取4个字节，这样timerfd就不会一直触发了  
  LOG_TRACE << "TimerQueue::handleRead() " << howmany << " at " << now.toString();
  if (n != sizeof howmany)
  {
    LOG_ERROR << "TimerQueue::handleRead() reads " << n << " bytes instead of 8";
  }
}

//重置定时器超时时刻
void resetTimerfd(int timerfd, Timestamp expiration)  
{
  // wake up loop by timerfd_settime()
  struct itimerspec newValue;   
  struct itimerspec oldValue;
  bzero(&newValue, sizeof newValue);
  bzero(&oldValue, sizeof oldValue);
  newValue.it_value = howMuchTimeFromNow(expiration);   //将时间戳类转换成it_value的形式
  int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue);  //设置进去，到期之后会产生一个定时器事件
  if (ret)
  {
    LOG_SYSERR << "timerfd_settime()";
  }
}

}
}
}

using namespace muduo;
using namespace muduo::net;
using namespace muduo::net::detail;

TimerQueue::TimerQueue(EventLoop* loop)
  : loop_(loop),
    timerfd_(createTimerfd()),    //创建定时器，调用timerfd_create，返回timerfd
    timerfdChannel_(loop, timerfd_),
    timers_(),
    callingExpiredTimers_(false)
{
/*Channel timerfdChannel_;   //这是定时器事件的通道*/
	//设置定时器类型通道的读回调函数。
  timerfdChannel_.setReadCallback(
      boost::bind(&TimerQueue::handleRead, this));   
  // we are always reading the timerfd, we disarm it with timerfd_settime.
  timerfdChannel_.enableReading();  //注册，底层是一系列update，你懂的。
}

TimerQueue::~TimerQueue()
{
  timerfdChannel_.disableAll();
  timerfdChannel_.remove();
  ::close(timerfd_);
  // do not remove channel, since we're in EventLoop::dtor();
  for (TimerList::iterator it = timers_.begin();
      it != timers_.end(); ++it)
  {
    delete it->second;  //析构函数只释放一次，因为两个set保存的是一样的
  }
}

//增加一个定时器
TimerId TimerQueue::addTimer(const TimerCallback& cb,
                             Timestamp when,
                             double interval)
{
  Timer* timer = new Timer(cb, when, interval);   //构造一个定时器对象，interval>0就是重复定时器
  loop_->runInLoop(
      boost::bind(&TimerQueue::addTimerInLoop, this, timer));
  return TimerId(timer, timer->sequence());
}

#ifdef __GXX_EXPERIMENTAL_CXX0X__
TimerId TimerQueue::addTimer(TimerCallback&& cb,
                             Timestamp when,
                             double interval)
{
  Timer* timer = new Timer(std::move(cb), when, interval);
  loop_->runInLoop(
      boost::bind(&TimerQueue::addTimerInLoop, this, timer));
  return TimerId(timer, timer->sequence());
}
#endif

//执行线程退出的回调函数
void TimerQueue::cancel(TimerId timerId)
{
  loop_->runInLoop(
      boost::bind(&TimerQueue::cancelInLoop, this, timerId)); 
}

void TimerQueue::addTimerInLoop(Timer* timer)
{
  loop_->assertInLoopThread();
  //插入一个定时器游客能会使得最早到期的定时器发生改变，比如当前插入一个最早到期的，那就要重置定时器超时时刻
  bool earliestChanged = insert(timer);

  if (earliestChanged)  //如果改变了
  {
    resetTimerfd(timerfd_, timer->expiration());  //重置定时器超时时刻
  }
}

void TimerQueue::cancelInLoop(TimerId timerId)
{
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size());
  ActiveTimer timer(timerId.timer_, timerId.sequence_);
  //查找该定时器
  ActiveTimerSet::iterator it = activeTimers_.find(timer);
  if (it != activeTimers_.end())
  {
  	//删除该定时器
    size_t n = timers_.erase(Entry(it->first->expiration(), it->first));
    assert(n == 1); (void)n;
    //如果用unique_ptr这里就不需要手工删除了
    delete it->first; // FIXME: no delete please
    activeTimers_.erase(it);
  }
  else if (callingExpiredTimers_)   //如果在定时器列表中没有找到，可能已经到期，且正在处理的定时器
  {
    //已经到期，并且正在调用回调函数的定时器
    cancelingTimers_.insert(timer);
  }
  assert(timers_.size() == activeTimers_.size());
}

//可读事件处理
void TimerQueue::handleRead()
{
  loop_->assertInLoopThread();  //断言I/O线程中调用
  Timestamp now(Timestamp::now());
  readTimerfd(timerfd_, now);   //清除该事件，避免一直触发，实际上是对timerfd做了read

  //获取该时刻之前所有的定时器列表，即超时定时器列表，因为实际上可能有多个定时器超时，存在定时器的时间设定是一样的这种情况
  std::vector<Entry> expired = getExpired(now);

  callingExpiredTimers_ = true;  //处于处理定时器状态中
  cancelingTimers_.clear();  
  // safe to callback outside critical section
  for (std::vector<Entry>::iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    it->second->run();   //调用所有的run()函数，底层调用Timer类的设置了的超时回调函数
  }
  callingExpiredTimers_ = false;

  reset(expired, now);   //如果移除的不是一次性定时器，那么重新启动它们
}

//返回当前所有超时的定时器列表
//返回值由于rvo优化，不会拷贝构造vector，直接返回它
std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now)
{
  assert(timers_.size() == activeTimers_.size());
  std::vector<Entry> expired;
  Entry sentry(now, reinterpret_cast<Timer*>(UINTPTR_MAX));//创建一个时间戳和定时器地址的集合
  //返回第一个未到期的Timer的迭代器
  //lower_bound返回第一个值>=sentry的
  //即*end>=sentry，从而end->first > now，而不是>=now，因为pair比较的第一个相等后会比较第二个，而sentry的第二个是UINTPTR_MAX最大
  //所以用lower_bound没有用upper_bound
  TimerList::iterator end = timers_.lower_bound(sentry);
  assert(end == timers_.end() || now < end->first);  //now < end->first
  std::copy(timers_.begin(), end, back_inserter(expired));  //将到期的定时器插入到expired中
  timers_.erase(timers_.begin(), end);  //删除已到期的所有定时器

  //从activeTimers_中也要移除到期的定时器
  for (std::vector<Entry>::iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    ActiveTimer timer(it->second, it->second->sequence());
    size_t n = activeTimers_.erase(timer);
    assert(n == 1); (void)n;
  }

  assert(timers_.size() == activeTimers_.size());
  return expired;
}

void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now)
{
  Timestamp nextExpire;

  for (std::vector<Entry>::const_iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    ActiveTimer timer(it->second, it->second->sequence());
    //如果是重复的定时器，并且是未取消定时器，则重启该定时器
    if (it->second->repeat()
        && cancelingTimers_.find(timer) == cancelingTimers_.end())
    {
      it->second->restart(now);  //restart()函数中会重新计算下一个超时时刻
      insert(it->second);
    }
    else
    {
    	//一次性定时器或者已被取消的定时器是不能重置的，因此删除该定时器
      // FIXME move to a free list
      delete it->second; // FIXME: no delete please  
    }
  }

  if (!timers_.empty()) 
  {
    //获取最早到期的超时时间
    nextExpire = timers_.begin()->second->expiration();
  }

  if (nextExpire.valid())
  {
  	//重新设定timerfd的超时时间
    resetTimerfd(timerfd_, nextExpire);
  }
}

bool TimerQueue::insert(Timer* timer)
{
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size());  //这两个存的是同样的定时器列表，成员函数中分析过了
  bool earliestChanged = false;
  //检测最早到期时间是否改变
  Timestamp when = timer->expiration();
  TimerList::iterator it = timers_.begin();   //第一个定时器，timers是set实现的，所以第一个就是最早，空的也算
  if (it == timers_.end() || when < it->first)
  {
    earliestChanged = true;  //如果插入定时器时间小于最早到期时间
  }
  //下面两个插入的set保存的是一样的，都是定时器，只不过对组的另一个辅助成员不一样
  {
    //利用RAII机制
    //插入到timers_中，result是临时对象，需要用它来保证插入成功
    std::pair<TimerList::iterator, bool> result
      = timers_.insert(Entry(when, timer));
    assert(result.second); (void)result;
  }
  {
    //插入到activeTimers中
    std::pair<ActiveTimerSet::iterator, bool> result
      = activeTimers_.insert(ActiveTimer(timer, timer->sequence()));
    assert(result.second); (void)result;
  }

  assert(timers_.size() == activeTimers_.size());
  return earliestChanged;  //返回是否最早到期时间改变
}

