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

//���㳬ʱʱ���뵱ǰʱ���ʱ���
struct timespec howMuchTimeFromNow(Timestamp when)
{
  int64_t microseconds = when.microSecondsSinceEpoch()
                         - Timestamp::now().microSecondsSinceEpoch();  //��ʱʱ��΢����-��ǰʱ��΢����
  if (microseconds < 100)  //����С��100����ȷ�Ȳ���Ҫ
  {
    microseconds = 100;
  }
  struct timespec ts;  //ת��������ṹ�巵��
  ts.tv_sec = static_cast<time_t>(
      microseconds / Timestamp::kMicroSecondsPerSecond);
  ts.tv_nsec = static_cast<long>(
      (microseconds % Timestamp::kMicroSecondsPerSecond) * 1000);
  return ts;
}

//��timerfd��ȡ�����ⶨʱ���¼�һֱ����
void readTimerfd(int timerfd, Timestamp now)
{
  uint64_t howmany;
  ssize_t n = ::read(timerfd, &howmany, sizeof howmany);  //��timerfd��ȡ4���ֽڣ�����timerfd�Ͳ���һֱ������  
  LOG_TRACE << "TimerQueue::handleRead() " << howmany << " at " << now.toString();
  if (n != sizeof howmany)
  {
    LOG_ERROR << "TimerQueue::handleRead() reads " << n << " bytes instead of 8";
  }
}

//���ö�ʱ����ʱʱ��
void resetTimerfd(int timerfd, Timestamp expiration)  
{
  // wake up loop by timerfd_settime()
  struct itimerspec newValue;   
  struct itimerspec oldValue;
  bzero(&newValue, sizeof newValue);
  bzero(&oldValue, sizeof oldValue);
  newValue.it_value = howMuchTimeFromNow(expiration);   //��ʱ�����ת����it_value����ʽ
  int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue);  //���ý�ȥ������֮������һ����ʱ���¼�
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
    timerfd_(createTimerfd()),    //������ʱ��������timerfd_create������timerfd
    timerfdChannel_(loop, timerfd_),
    timers_(),
    callingExpiredTimers_(false)
{
/*Channel timerfdChannel_;   //���Ƕ�ʱ���¼���ͨ��*/
	//���ö�ʱ������ͨ���Ķ��ص�������
  timerfdChannel_.setReadCallback(
      boost::bind(&TimerQueue::handleRead, this));   
  // we are always reading the timerfd, we disarm it with timerfd_settime.
  timerfdChannel_.enableReading();  //ע�ᣬ�ײ���һϵ��update���㶮�ġ�
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
    delete it->second;  //��������ֻ�ͷ�һ�Σ���Ϊ����set�������һ����
  }
}

//����һ����ʱ��
TimerId TimerQueue::addTimer(const TimerCallback& cb,
                             Timestamp when,
                             double interval)
{
  Timer* timer = new Timer(cb, when, interval);   //����һ����ʱ������interval>0�����ظ���ʱ��
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

//ִ���߳��˳��Ļص�����
void TimerQueue::cancel(TimerId timerId)
{
  loop_->runInLoop(
      boost::bind(&TimerQueue::cancelInLoop, this, timerId)); 
}

void TimerQueue::addTimerInLoop(Timer* timer)
{
  loop_->assertInLoopThread();
  //����һ����ʱ���ο��ܻ�ʹ�����絽�ڵĶ�ʱ�������ı䣬���統ǰ����һ�����絽�ڵģ��Ǿ�Ҫ���ö�ʱ����ʱʱ��
  bool earliestChanged = insert(timer);

  if (earliestChanged)  //����ı���
  {
    resetTimerfd(timerfd_, timer->expiration());  //���ö�ʱ����ʱʱ��
  }
}

void TimerQueue::cancelInLoop(TimerId timerId)
{
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size());
  ActiveTimer timer(timerId.timer_, timerId.sequence_);
  //���Ҹö�ʱ��
  ActiveTimerSet::iterator it = activeTimers_.find(timer);
  if (it != activeTimers_.end())
  {
  	//ɾ���ö�ʱ��
    size_t n = timers_.erase(Entry(it->first->expiration(), it->first));
    assert(n == 1); (void)n;
    //�����unique_ptr����Ͳ���Ҫ�ֹ�ɾ����
    delete it->first; // FIXME: no delete please
    activeTimers_.erase(it);
  }
  else if (callingExpiredTimers_)   //����ڶ�ʱ���б���û���ҵ��������Ѿ����ڣ������ڴ���Ķ�ʱ��
  {
    //�Ѿ����ڣ��������ڵ��ûص������Ķ�ʱ��
    cancelingTimers_.insert(timer);
  }
  assert(timers_.size() == activeTimers_.size());
}

//�ɶ��¼�����
void TimerQueue::handleRead()
{
  loop_->assertInLoopThread();  //����I/O�߳��е���
  Timestamp now(Timestamp::now());
  readTimerfd(timerfd_, now);   //������¼�������һֱ������ʵ�����Ƕ�timerfd����read

  //��ȡ��ʱ��֮ǰ���еĶ�ʱ���б�����ʱ��ʱ���б���Ϊʵ���Ͽ����ж����ʱ����ʱ�����ڶ�ʱ����ʱ���趨��һ�����������
  std::vector<Entry> expired = getExpired(now);

  callingExpiredTimers_ = true;  //���ڴ���ʱ��״̬��
  cancelingTimers_.clear();  
  // safe to callback outside critical section
  for (std::vector<Entry>::iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    it->second->run();   //�������е�run()�������ײ����Timer��������˵ĳ�ʱ�ص�����
  }
  callingExpiredTimers_ = false;

  reset(expired, now);   //����Ƴ��Ĳ���һ���Զ�ʱ������ô������������
}

//���ص�ǰ���г�ʱ�Ķ�ʱ���б�
//����ֵ����rvo�Ż������´������vector��ֱ�ӷ�����
std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now)
{
  assert(timers_.size() == activeTimers_.size());
  std::vector<Entry> expired;
  Entry sentry(now, reinterpret_cast<Timer*>(UINTPTR_MAX));//����һ��ʱ����Ͷ�ʱ����ַ�ļ���
  //���ص�һ��δ���ڵ�Timer�ĵ�����
  //lower_bound���ص�һ��ֵ>=sentry��
  //��*end>=sentry���Ӷ�end->first > now��������>=now����Ϊpair�Ƚϵĵ�һ����Ⱥ��Ƚϵڶ�������sentry�ĵڶ�����UINTPTR_MAX���
  //������lower_boundû����upper_bound
  TimerList::iterator end = timers_.lower_bound(sentry);
  assert(end == timers_.end() || now < end->first);  //now < end->first
  std::copy(timers_.begin(), end, back_inserter(expired));  //�����ڵĶ�ʱ�����뵽expired��
  timers_.erase(timers_.begin(), end);  //ɾ���ѵ��ڵ����ж�ʱ��

  //��activeTimers_��ҲҪ�Ƴ����ڵĶ�ʱ��
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
    //������ظ��Ķ�ʱ����������δȡ����ʱ�����������ö�ʱ��
    if (it->second->repeat()
        && cancelingTimers_.find(timer) == cancelingTimers_.end())
    {
      it->second->restart(now);  //restart()�����л����¼�����һ����ʱʱ��
      insert(it->second);
    }
    else
    {
    	//һ���Զ�ʱ�������ѱ�ȡ���Ķ�ʱ���ǲ������õģ����ɾ���ö�ʱ��
      // FIXME move to a free list
      delete it->second; // FIXME: no delete please  
    }
  }

  if (!timers_.empty()) 
  {
    //��ȡ���絽�ڵĳ�ʱʱ��
    nextExpire = timers_.begin()->second->expiration();
  }

  if (nextExpire.valid())
  {
  	//�����趨timerfd�ĳ�ʱʱ��
    resetTimerfd(timerfd_, nextExpire);
  }
}

bool TimerQueue::insert(Timer* timer)
{
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size());  //�����������ͬ���Ķ�ʱ���б���Ա�����з�������
  bool earliestChanged = false;
  //������絽��ʱ���Ƿ�ı�
  Timestamp when = timer->expiration();
  TimerList::iterator it = timers_.begin();   //��һ����ʱ����timers��setʵ�ֵģ����Ե�һ���������磬�յ�Ҳ��
  if (it == timers_.end() || when < it->first)
  {
    earliestChanged = true;  //������붨ʱ��ʱ��С�����絽��ʱ��
  }
  //�������������set�������һ���ģ����Ƕ�ʱ����ֻ�����������һ��������Ա��һ��
  {
    //����RAII����
    //���뵽timers_�У�result����ʱ������Ҫ��������֤����ɹ�
    std::pair<TimerList::iterator, bool> result
      = timers_.insert(Entry(when, timer));
    assert(result.second); (void)result;
  }
  {
    //���뵽activeTimers��
    std::pair<ActiveTimerSet::iterator, bool> result
      = activeTimers_.insert(ActiveTimer(timer, timer->sequence()));
    assert(result.second); (void)result;
  }

  assert(timers_.size() == activeTimers_.size());
  return earliestChanged;  //�����Ƿ����絽��ʱ��ı�
}

