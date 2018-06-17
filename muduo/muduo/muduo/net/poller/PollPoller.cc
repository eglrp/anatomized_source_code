// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/poller/PollPoller.h>

#include <muduo/base/Logging.h>
#include <muduo/base/Types.h>
#include <muduo/net/Channel.h>

#include <assert.h>
#include <errno.h>
#include <poll.h>

using namespace muduo;
using namespace muduo::net;

PollPoller::PollPoller(EventLoop* loop)
  : Poller(loop)
{
}

PollPoller::~PollPoller()
{
}

Timestamp PollPoller::poll(int timeoutMs, ChannelList* activeChannels)
{
  // XXX pollfds_ shouldn't change    //���������׵�ַ����С����ʱ
  int numEvents = ::poll(&*pollfds_.begin(), pollfds_.size(), timeoutMs);
  int savedErrno = errno;
  Timestamp now(Timestamp::now());  //ʱ���
  if (numEvents > 0)  //˵�����¼�����
  {
    LOG_TRACE << numEvents << " events happended";
    fillActiveChannels(numEvents, activeChannels);  //�����Ծ�¼�ͨ����
  }
  else if (numEvents == 0)
  {
    LOG_TRACE << " nothing happended";
  }
  else
  {
    if (savedErrno != EINTR)
    {
      errno = savedErrno;
      LOG_SYSERR << "PollPoller::poll()";
    }
  }
  return now;
}

//���Ծ�¼�ͨ����������Ծ�¼�
void PollPoller::fillActiveChannels(int numEvents,
                                    ChannelList* activeChannels) const
{
  for (PollFdList::const_iterator pfd = pollfds_.begin();
      pfd != pollfds_.end() && numEvents > 0; ++pfd)  
  {
    if (pfd->revents > 0)  //>=˵���������¼�
    {
      --numEvents;  //����һ������
      //map<int, channel*>  �ļ���������ͨ��ָ���map
      ChannelMap::const_iterator ch = channels_.find(pfd->fd);//����map.find����
      assert(ch != channels_.end()); //�����ҵ��¼�
      Channel* channel = ch->second;   //��ȡ�¼�
      assert(channel->fd() == pfd->fd);  //�������
      channel->set_revents(pfd->revents);   //����Ҫ���ص��¼�����
      // pfd->revents = 0;
      activeChannels->push_back(channel);  //�����Ծ�¼�����
    }
  }
}

//����ע����߸���ͨ��
void PollPoller::updateChannel(Channel* channel)
{
  Poller::assertInLoopThread();  //����ֻ����I/O�߳��е���
  LOG_TRACE << "fd = " << channel->fd() << " events = " << channel->events();
  if (channel->index() < 0)  //���С��0��˵����û��ע��
  {
    // a new one, add to pollfds_
    assert(channels_.find(channel->fd()) == channels_.end());//���Բ��Ҳ���
    struct pollfd pfd;
    pfd.fd = channel->fd();
    pfd.events = static_cast<short>(channel->events());
    pfd.revents = 0;  
    pollfds_.push_back(pfd);  //�ϳ�pollfd
    int idx = static_cast<int>(pollfds_.size())-1;  //idx����ĩβ����Ϊ�����µģ����idx��pollfd_���±�
    channel->set_index(idx);  //idx��channel�ĳ�Ա����
    channels_[pfd.fd] = channel; //����map��
  }
  else   //�������0��˵���Ǹ���һ���Ѵ��ڵ�ͨ��
  {
    // update existing one
    assert(channels_.find(channel->fd()) != channels_.end());
    assert(channels_[channel->fd()] == channel);
    int idx = channel->index();   //ȡ��channel���±�
    assert(0 <= idx && idx < static_cast<int>(pollfds_.size()));
    struct pollfd& pfd = pollfds_[idx];  //��ȡpollfd�������÷�ʽ���Ч��
    assert(pfd.fd == channel->fd() || pfd.fd == -channel->fd()-1);  //-channel->fd()-1����Ϊƥ��������ʱ����ע״̬������
    pfd.events = static_cast<short>(channel->events());   //����events
    pfd.revents = 0;
    //��һ��ͨ����ʱ����Ϊ����ע�¼���������Poller���Ƴ�ͨ����������һ�λ�����
    if (channel->isNoneEvent())  
    {
      // ignore this pollfd
      //��ʱ���Ը��ļ���������ʱ��
      //����pfd.fd����ֱ������Ϊ-1
      pfd.fd = -channel->fd()-1;   //������������Ϊ��removeChannel�Ż�
    }
  }
}

//����������Ƴ�pollfd�ĳ���
void PollPoller::removeChannel(Channel* channel)
{
  Poller::assertInLoopThread();
  LOG_TRACE << "fd = " << channel->fd();
  assert(channels_.find(channel->fd()) != channels_.end());//ɾ���������ҵ�
  assert(channels_[channel->fd()] == channel);  //һ����Ӧ
  assert(channel->isNoneEvent());  //һ���Ѿ�û���¼��ϵĹ�ע�ˣ������ڵ���removeChannel֮ǰ����
  int idx = channel->index();  //ȡ����pollfds_�����±��е�λ��
  assert(0 <= idx && idx < static_cast<int>(pollfds_.size()));
  const struct pollfd& pfd = pollfds_[idx]; (void)pfd;  //�õ�pollfd
  assert(pfd.fd == -channel->fd()-1 && pfd.events == channel->events());
  size_t n = channels_.erase(channel->fd()); //ʹ�ü�ֵ�Ƴ���map���Ƴ�
  assert(n == 1); (void)n;
  if (implicit_cast<size_t>(idx) == pollfds_.size()-1)  //��������һ��
  {
    pollfds_.pop_back();  //ֱ��pop_back()
  }
  else  //����vector���һ���������������ˣ�vector�Ƴ�ǰ��Ԫ�ؿ��ܻ��漰���ɺ���Ԫ��Ų�������Բ���ȡ��
  {       //������õ��ǽ����ķ�����������ͼ�㷨��ɾ�����㣬���ǽ�Ҫ�Ƴ���Ԫ�غ����һ��Ԫ�ؽ������ɡ�
  			//�����㷨ʱ�临�Ӷ���O(1)
    int channelAtEnd = pollfds_.back().fd;   //�õ����һ��Ԫ��
    iter_swap(pollfds_.begin()+idx, pollfds_.end()-1);  //����
    if (channelAtEnd < 0)  //������һ��<0
    {
      channelAtEnd = -channelAtEnd-1;  //������ԭ�������õ���ʵ��fd������ǲ�ʹ��-1��ԭ�����ǲ���ע����
      															//������Ϊ-fd-1��Ȼ��ɾ����ʱ������ٻ�ԭ�����������Ϊ-1�Ͳ������ˡ�
    }
    channels_[channelAtEnd]->set_index(idx); //�Ը���ʵ��fd�����±�
    pollfds_.pop_back();  //����ĩβԪ��
  }
}

