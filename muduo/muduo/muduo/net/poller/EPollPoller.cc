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
    epollfd_(::epoll_create1(EPOLL_CLOEXEC)),   //����epollfd��ʹ�ô�1�İ汾
    events_(kInitEventListSize)    //vector������ʱ��ʼ��kInitEventListSize����С�ռ�
{
  if (epollfd_ < 0)   //�ڹ��캯�����жϣ�<0��abort()
  {
    LOG_SYSFATAL << "EPollPoller::EPollPoller";
  }
}

EPollPoller::~EPollPoller()
{
  ::close(epollfd_);   //�ر�
}

Timestamp EPollPoller::poll(int timeoutMs, ChannelList* activeChannels)
{
  LOG_TRACE << "fd total count " << channels_.size();
  int numEvents = ::epoll_wait(epollfd_,
                               &*events_.begin(),
                               static_cast<int>(events_.size()),    //ʹ��epoll_wait()���ȴ��¼�����,���ط������¼���Ŀ
                               timeoutMs);  //epoll
  int savedErrno = errno;    //�����
  Timestamp now(Timestamp::now());  //�õ�ʱ���
  if (numEvents > 0)
  {
    LOG_TRACE << numEvents << " events happended";
    fillActiveChannels(numEvents, activeChannels);  //����fillActiveChannels������numEventsҲ���Ƿ������¼���Ŀ
    if (implicit_cast<size_t>(numEvents) == events_.size())  //������ص��¼���Ŀ���ڵ�ǰ�¼������С���ͷ���2���ռ�
    {																//!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
      events_.resize(events_.size()*2);     //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    }														       //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
  }
  else if (numEvents == 0)  //����=0
  {
    LOG_TRACE << "nothing happended";
  }
  else
  {
    // error happens, log uncommon ones
    if (savedErrno != EINTR)   //�������EINTR�źţ��ͰѴ���ű����������������뵽��־��
    {
      errno = savedErrno;
      LOG_SYSERR << "EPollPoller::poll()";
    }
  }
  return now;  //����ʱ���
}

//�ѷ��ص�����ô����¼���ӵ�activeChannels
/*protected:
  typedef std::map<int, Channel*> ChannelMap;
  ChannelMap channels_;
*/
void EPollPoller::fillActiveChannels(int numEvents,
                                     ChannelList* activeChannels) const
{
  assert(implicit_cast<size_t>(numEvents) <= events_.size());//ȷ�����Ĵ�СС��events_�Ĵ�С����Ϊevents_��Ԥ�����¼�vector
                                                                                       
/*typedef std::vector<struct epoll_event> EventList;   //modify by WY.Huang

  int epollfd_;
  EventList events_;
*/
  
  for (int i = 0; i < numEvents; ++i)   //������������numEvents���¼���epollģʽ���ص�events_�����ж����Ѿ��������¼������б���select��poll
  {
    Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
/*
����epollģʽepoll_event�¼������ݽṹ������data�������Ա���fd��Ҳ���Ա���һ��void*���͵�ָ�롣
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
    int fd = channel->fd();  //debugʱ��һ�¼��
    ChannelMap::const_iterator it = channels_.find(fd);
    assert(it != channels_.end());
    assert(it->second == channel);
#endif
    channel->set_revents(events_[i].events);   //���ѷ������¼�����channel,д��ͨ������
    activeChannels->push_back(channel);    //����push_back��activeChannels
  }
}

//�����������������Ϊchannel->enablereading()�����ã��ٵ���channel->update()����event_loop->updateChannel()����->epoll��poll��updateChannel������
void EPollPoller::updateChannel(Channel* channel)   //����ͨ��
{
  Poller::assertInLoopThread();
  const int index = channel->index();   //channel�Ǳ��������������channel��index����ʼ״̬index��-1
  LOG_TRACE << "fd = " << channel->fd()
    << " events = " << channel->events() << " index = " << index;
  if (index == kNew || index == kDeleted)  //index����poll�����±꣬��epoll��������״̬����������������
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
    assert(index == kAdded);  //ȷ��Ŀǰ������
   //
   //poll�ǰ��ļ���������Ϊ�෴����1��������ʹ��EPOLL_CTL_DEL���ں��¼�����ɾ��
   //
    if (channel->isNoneEvent())  //���ʲôҲû��ע����ֱ�Ӹɵ�
    {
      update(EPOLL_CTL_DEL, channel);
      channel->set_index(kDeleted);     //ɾ��֮����Ϊdeleted����ʾ�Ѿ�ɾ����ֻ�Ǵ��ں��¼�����ɾ������channels_���ͨ�������в�û��ɾ��
    }
    else
    {
      update(EPOLL_CTL_MOD, channel);   //�й�ע���Ǿ�ֻ�Ǹ��¡����³�ʲô����channel�л������
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

//epoll_ctl�����ķ�װ
void EPollPoller::update(int operation, Channel* channel)
{
  struct epoll_event event;
  bzero(&event, sizeof event);
  event.events = channel->events();
  event.data.ptr = channel;   //����data.ptr
  int fd = channel->fd();
  LOG_TRACE << "epoll_ctl op = " << operationToString(operation)
    << " fd = " << fd << " event = { " << channel->eventsToString() << " }";
  if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)  //ʹ��epoll_ctl
  {
    if (operation == EPOLL_CTL_DEL)  //���deleteʧ�ܣ����˳�
    {
      LOG_SYSERR << "epoll_ctl op =" << operationToString(operation) << " fd =" << fd;
    }
    else
    {
      LOG_SYSFATAL << "epoll_ctl op =" << operationToString(operation) << " fd =" << fd;
    }
  }
}

const char* EPollPoller::operationToString(int op)  //�����õĺ���
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
