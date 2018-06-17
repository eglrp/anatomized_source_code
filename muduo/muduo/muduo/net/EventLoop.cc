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
__thread EventLoop* t_loopInThisThread = 0;   //�ֲ߳̾��洢�ĵ�ǰ�߳�EventLoop����ָ�룬���Ṳ��

const int kPollTimeMs = 10000;

//������������eventfd
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
  : looping_(false),  //��ʾ��δѭ��
    quit_(false),
    eventHandling_(false),
    callingPendingFunctors_(false),
    iteration_(0),
    threadId_(CurrentThread::tid()),   //��ֵ��ʵid
    poller_(Poller::newDefaultPoller(this)),   //������һ��ʵ�ʵ�poller����
    timerQueue_(new TimerQueue(this)),
    wakeupFd_(createEventfd()),
    wakeupChannel_(new Channel(this, wakeupFd_)),
    currentActiveChannel_(NULL)
{
  LOG_DEBUG << "EventLoop created " << this << " in thread " << threadId_;
  if (t_loopInThisThread)  //ÿ���߳����һ��EventLoop��������Ѿ����ڣ�ʹ��LOG_FATAL��ֹabort����
  {
    LOG_FATAL << "Another EventLoop " << t_loopInThisThread
              << " exists in this thread " << threadId_;
  }
  else
  {
    t_loopInThisThread = this; //this�����ֲ߳̾�����ָ�룬ƾ��������Ա�֤per thread a EventLoop
  }

  //�趨wakeupChannel�Ļص���������EventLoop��handleRead��������������dopendingFunctors
  wakeupChannel_->setReadCallback(
      boost::bind(&EventLoop::handleRead, this));
  // we are always reading the wakeupfd
  wakeupChannel_->enableReading(); //ͬ����Ϊ��wakeupfd_����������dopendingFunctors()
}

EventLoop::~EventLoop()
{
  LOG_DEBUG << "EventLoop " << this << " of thread " << threadId_
            << " destructs in thread " << CurrentThread::tid();
  wakeupChannel_->disableAll();
  wakeupChannel_->remove();
  ::close(wakeupFd_);
  t_loopInThisThread = NULL;   //����
}

//�¼�ѭ�������ܿ��̵߳���
//ֻ���ڴ����ö�����߳��е���
void EventLoop::loop()
{
  assert(!looping_);
  assertInLoopThread();  //���Դ��ڴ����ö�����߳���
  looping_ = true;
  quit_ = false;  // FIXME: what if someone calls quit() before loop() ?
  LOG_TRACE << "EventLoop " << this << " start looping";

  while (!quit_)
  {
    activeChannels_.clear();  //��������
    pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);  //����poll���ػ��ͨ�����п����ǻ��ѷ��ص�
    ++iteration_; 
    if (Logger::logLevel() <= Logger::TRACE)
    {
      printActiveChannels();  //��־�Ǽǣ���־��ӡ
    }
    // TODO sort channel by priority
    eventHandling_ = true;  //true
    for (ChannelList::iterator it = activeChannels_.begin();
        it != activeChannels_.end(); ++it)  //����ͨ�������д���
    {
      currentActiveChannel_ = *it;
      currentActiveChannel_->handleEvent(pollReturnTime_);
    }
    currentActiveChannel_ = NULL;   //�������˸���
    eventHandling_ = false;  //false
 //I/O�߳���ƱȽ���ͨ������������Ҳ�ܹ����м������񣬷���I/O���Ǻܷ�æ��ʱ�����I/O�߳̾�һֱ��������״̬��
 //������Ҫ����Ҳ��ִ��һЩ�������� 
    doPendingFunctors();   //�����û��ص�����
  }

  LOG_TRACE << "EventLoop " << this << " stop looping";
  looping_ = false;
}

//�ú������Կ��̵߳���
void EventLoop::quit()
{
  quit_ = true;
  // There is a chance that loop() just executes while(!quit_) and exits,
  // then EventLoop destructs, then we are accessing an invalid object.
  // Can be fixed using mutex_ in both places.
  if (!isInLoopThread())  //������ǵ�ǰI/O�̵߳��õģ�������Ҫ���Ѹ�I/O�߳�
  {
    wakeup();   //���ѣ�������������poll��
  }
}

//����˼�壬��I/O�߳��е���ĳ���������ú������Կ��̵߳���
void EventLoop::runInLoop(const Functor& cb)
{
  //�������I/O�߳��е��ã���ͬ������cb�ص�����
  if (isInLoopThread())
  {
    cb();
  }
  else
  {
  	//�����������߳��е��ã����첽��cb��ӵ�������е��У��Ա���EventLoop��ʵ��Ӧ��I/O�߳�ִ������ص�����
    queueInLoop(cb);
  }
}

//��������ӵ����е���
void EventLoop::queueInLoop(const Functor& cb)
{
  {
  MutexLockGuard lock(mutex_);
  pendingFunctors_.push_back(cb);   //��ӵ�������е���
  }
  //�����ǰ����queueInLoop���ò���I/O�̣߳���ô���Ѹ�I/O�̣߳��Ա�I/O�̼߳�ʱ����
  //���ߵ��õ��߳��ǵ�ǰI/O�̣߳����Ҵ�ʱ����pendingfunctor����Ҫ����
  //ֻ�е�ǰI/O�̵߳��¼��ص��е���queueInLoop�Ų���Ҫ����
  if (!isInLoopThread() || callingPendingFunctors_)  
  {
    wakeup();
  }
}

//��ʱ���Ϊtime��ʱ��ִ�У�0.0��ʾһ���Բ��ظ�
TimerId EventLoop::runAt(const Timestamp& time, const TimerCallback& cb)
{
  return timerQueue_->addTimer(cb, time, 0.0);  
}

//�ӳ�delayʱ��ִ�еĶ�ʱ��
TimerId EventLoop::runAfter(double delay, const TimerCallback& cb)
{
  Timestamp time(addTime(Timestamp::now(), delay));  //�ϳ�һ��ʱ���
  return runAt(time, cb);
}

//����ԵĶ�ʱ������ʼ�����ظ���ʱ�������interval��Ҫ����0
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

//ֱ�ӵ���timerQueue��cancle
void EventLoop::cancel(TimerId timerId)
{
  return timerQueue_->cancel(timerId);  
}

void EventLoop::updateChannel(Channel* channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();  //������EventLoop�̵߳���
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

void EventLoop::abortNotInLoopThread()  //ͨ��LOG_FATAL��ֹ����
{
  LOG_FATAL << "EventLoop::abortNotInLoopThread - EventLoop " << this
            << " was created in threadId_ = " << threadId_
            << ", current thread id = " <<  CurrentThread::tid();
}

//����EventLoop
void EventLoop::wakeup()    
{
  uint64_t one = 1;                                                           
  ssize_t n = sockets::write(wakeupFd_, &one, sizeof one);  //���д�����ݽ�ȥ�ͻ�����
  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::wakeup() writes " << n << " bytes instead of 8";
  }
}

//ʵ������wakeFd_�Ķ��ص�����
void EventLoop::handleRead()
{
  uint64_t one = 1;
  ssize_t n = sockets::read(wakeupFd_, &one, sizeof one);
  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::handleRead() reads " << n << " bytes instead of 8";
  }
}

// 1. ���Ǽ򵥵����ٽ��������ε���functor�����ǰѻص��б�swap��functors�У���һ�����С��
//�ٽ����ĳ��ȣ���ζ�Ų������������̵߳�queueInLoop()����һ����Ҳ����������(��ΪFunctor�����ٴε���quueInLoop)
// 2. ����doPendingFunctors()���õ�Functor�����ٴε���queueInLoop(cb)������queueInLoop()�ͱ���wakeup(),����������cb���ܾͲ��ܼ�ʱ������
// 3. muduoû�з���ִ��doPendingFunctors()ֱ��pendingFunctorsΪ�գ���������ģ�����I/O�߳̿���������ѭ�����޷�����I/O�¼�
void EventLoop::doPendingFunctors()
{
  std::vector<Functor> functors;
  callingPendingFunctors_ = true;

  //ע��������ٽ���������ʹ����һ��ջ�ϱ���functors��pendingFunctors����
  {
  MutexLockGuard lock(mutex_);
  functors.swap(pendingFunctors_); //�����Ϳ�vector����
  }

	//�˴������߳̾Ϳ�����pendingFunctors�������

  //���ûص�����
  //��һ���ֲ����ٽ�������
  for (size_t i = 0; i < functors.size(); ++i)
  {
    functors[i]();
  }
  callingPendingFunctors_ = false;
}

//��ӡ��Ծͨ����
void EventLoop::printActiveChannels() const
{
  for (ChannelList::const_iterator it = activeChannels_.begin();
      it != activeChannels_.end(); ++it)
  {
    const Channel* ch = *it;
    LOG_TRACE << "{" << ch->reventsToString() << "} ";
  }
}

