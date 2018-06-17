// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/poller/EPollPoller.h>

#include <muduo/base/Logging.h>
#include <muduo/net/Channel.h>

#include <boost/static_assert.hpp>

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <sys/epoll.h>

using namespace muduo;
using namespace muduo::net;

// On Linux, the constants of poll(2) and epoll(4)
// are expected to be the same.
BOOST_STATIC_ASSERT(EPOLLIN == POLLIN);
BOOST_STATIC_ASSERT(EPOLLPRI == POLLPRI);
BOOST_STATIC_ASSERT(EPOLLOUT == POLLOUT);
BOOST_STATIC_ASSERT(EPOLLRDHUP == POLLRDHUP);
BOOST_STATIC_ASSERT(EPOLLERR == POLLERR);
BOOST_STATIC_ASSERT(EPOLLHUP == POLLHUP);

namespace
{
const int kNew = -1;
const int kAdded = 1;
const int kDeleted = 2;
}

EPollPoller::EPollPoller(EventLoop* loop)
  : Poller(loop),
    epollfd_(::epoll_create1(EPOLL_CLOEXEC)),   //创建epollfd，使用带1的版本
    events_(kInitEventListSize)    //vector这样用时初始化kInitEventListSize个大小空间
{
  if (epollfd_ < 0)   //在构造函数中判断，<0就abort()
  {
    LOG_SYSFATAL << "EPollPoller::EPollPoller";
  }
}

EPollPoller::~EPollPoller()
{
  ::close(epollfd_);   //关闭
}

Timestamp EPollPoller::poll(int timeoutMs, ChannelList* activeChannels)
{
  LOG_TRACE << "fd total count " << channels_.size();
  int numEvents = ::epoll_wait(epollfd_,
                               &*events_.begin(),
                               static_cast<int>(events_.size()),    //使用epoll_wait()，等待事件返回,返回发生的事件数目
                               timeoutMs);  //epoll
  int savedErrno = errno;    //错误号
  Timestamp now(Timestamp::now());  //得到时间戳
  if (numEvents > 0)
  {
    LOG_TRACE << numEvents << " events happended";
    fillActiveChannels(numEvents, activeChannels);  //调用fillActiveChannels，传入numEvents也就是发生的事件数目
    if (implicit_cast<size_t>(numEvents) == events_.size())  //如果返回的事件数目等于当前事件数组大小，就分配2倍空间
    {																//!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
      events_.resize(events_.size()*2);     //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    }														       //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
  }
  else if (numEvents == 0)  //返回=0
  {
    LOG_TRACE << "nothing happended";
  }
  else
  {
    // error happens, log uncommon ones
    if (savedErrno != EINTR)   //如果不是EINTR信号，就把错误号保存下来，并且输入到日志中
    {
      errno = savedErrno;
      LOG_SYSERR << "EPollPoller::poll()";
    }
  }
  return now;  //返回时间戳
}

//把返回到的这么多个事件添加到activeChannels
/*protected:
  typedef std::map<int, Channel*> ChannelMap;
  ChannelMap channels_;
*/
void EPollPoller::fillActiveChannels(int numEvents,
                                     ChannelList* activeChannels) const
{
  assert(implicit_cast<size_t>(numEvents) <= events_.size());//确定它的大小小于events_的大小，因为events_是预留的事件vector
                                                                                       
/*typedef std::vector<struct epoll_event> EventList;   //modify by WY.Huang

  int epollfd_;
  EventList events_;
*/
  
  for (int i = 0; i < numEvents; ++i)   //挨个处理发生的numEvents个事件，epoll模式返回的events_数组中都是已经发生额事件，这有别于select和poll
  {
    Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
/*
这是epoll模式epoll_event事件的数据结构，其中data不仅可以保存fd，也可以保存一个void*类型的指针。
typedef union epoll_data {
               void    *ptr;
               int      fd;
               uint32_t u32;
               uint64_t u64;
           } epoll_data_t;

           struct epoll_event {
               uint32_t     events;    // Epoll events 
               epoll_data_t data;      //User data variable 
           };
*/
#ifndef NDEBUG
    int fd = channel->fd();  //debug时做一下检测
    ChannelMap::const_iterator it = channels_.find(fd);
    assert(it != channels_.end());
    assert(it->second == channel);
#endif
    channel->set_revents(events_[i].events);   //把已发生的事件传给channel,写到通道当中
    activeChannels->push_back(channel);    //并且push_back进activeChannels
  }
}

//这个函数被调用是因为channel->enablereading()被调用，再调用channel->update()，再event_loop->updateChannel()，再->epoll或poll的updateChannel被调用
void EPollPoller::updateChannel(Channel* channel)   //更新通道
{
  Poller::assertInLoopThread();
  const int index = channel->index();   //channel是本函数参数，获得channel的index，初始状态index是-1
  LOG_TRACE << "fd = " << channel->fd()
    << " events = " << channel->events() << " index = " << index;
  if (index == kNew || index == kDeleted)  //index是在poll中是下标，在epoll中是三种状态，上面有三个常量
  {
    // a new one, add with EPOLL_CTL_ADD
    int fd = channel->fd();
    if (index == kNew)
    {
      assert(channels_.find(fd) == channels_.end());
      channels_[fd] = channel;
    }
    else // index == kDeleted
    {
      assert(channels_.find(fd) != channels_.end());
      assert(channels_[fd] == channel);
    }

    channel->set_index(kAdded);
    update(EPOLL_CTL_ADD, channel);
  }
  else
  {
    // update existing one with EPOLL_CTL_MOD/DEL
    int fd = channel->fd();
    (void)fd;
    assert(channels_.find(fd) != channels_.end());
    assert(channels_[fd] == channel);
    assert(index == kAdded);  //确保目前它存在
   //
   //poll是把文件描述符变为相反数减1，这里是使用EPOLL_CTL_DEL从内核事件表中删除
   //
    if (channel->isNoneEvent())  //如果什么也没关注，就直接干掉
    {
      update(EPOLL_CTL_DEL, channel);
      channel->set_index(kDeleted);     //删除之后设为deleted，表示已经删除，只是从内核事件表中删除，在channels_这个通道数组中并没有删除
    }
    else
    {
      update(EPOLL_CTL_MOD, channel);   //有关注，那就只是更新。更新成什么样子channel中会决定。
    }
  }
}

void EPollPoller::removeChannel(Channel* channel)
{
  Poller::assertInLoopThread();
  int fd = channel->fd();
  LOG_TRACE << "fd = " << fd;
  assert(channels_.find(fd) != channels_.end());
  assert(channels_[fd] == channel);
  assert(channel->isNoneEvent());
  int index = channel->index();
  assert(index == kAdded || index == kDeleted);
  size_t n = channels_.erase(fd);
  (void)n;
  assert(n == 1);

  if (index == kAdded)
  {
    update(EPOLL_CTL_DEL, channel);
  }
  channel->set_index(kNew);
}

//epoll_ctl函数的封装
void EPollPoller::update(int operation, Channel* channel)
{
  struct epoll_event event;
  bzero(&event, sizeof event);
  event.events = channel->events();
  event.data.ptr = channel;   //传入data.ptr
  int fd = channel->fd();
  LOG_TRACE << "epoll_ctl op = " << operationToString(operation)
    << " fd = " << fd << " event = { " << channel->eventsToString() << " }";
  if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)  //使用epoll_ctl
  {
    if (operation == EPOLL_CTL_DEL)  //如果delete失败，不退出
    {
      LOG_SYSERR << "epoll_ctl op =" << operationToString(operation) << " fd =" << fd;
    }
    else
    {
      LOG_SYSFATAL << "epoll_ctl op =" << operationToString(operation) << " fd =" << fd;
    }
  }
}

const char* EPollPoller::operationToString(int op)  //调试用的函数
{
  switch (op)
  {
    case EPOLL_CTL_ADD:
      return "ADD";
    case EPOLL_CTL_DEL:
      return "DEL";
    case EPOLL_CTL_MOD:
      return "MOD";
    default:
      assert(false && "ERROR op");
      return "Unknown Operation";
  }
}
