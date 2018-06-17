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
  // XXX pollfds_ shouldn't change    //´«ÈëÊı×éÊ×µØÖ·£¬´óĞ¡£¬³¬Ê±
  int numEvents = ::poll(&*pollfds_.begin(), pollfds_.size(), timeoutMs);
  int savedErrno = errno;
  Timestamp now(Timestamp::now());  //Ê±¼ä´Á
  if (numEvents > 0)  //ËµÃ÷ÓĞÊÂ¼ş·¢Éú
  {
    LOG_TRACE << numEvents << " events happended";
    fillActiveChannels(numEvents, activeChannels);  //·ÅÈë»îÔ¾ÊÂ¼şÍ¨µÀÖĞ
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

//Ïò»îÔ¾ÊÂ¼şÍ¨µÀÊı×é·ÅÈë»îÔ¾ÊÂ¼ş
void PollPoller::fillActiveChannels(int numEvents,
                                    ChannelList* activeChannels) const
{
  for (PollFdList::const_iterator pfd = pollfds_.begin();
      pfd != pollfds_.end() && numEvents > 0; ++pfd)  
  {
    if (pfd->revents > 0)  //>=ËµÃ÷²úÉúÁËÊÂ¼ş
    {
      --numEvents;  //´¦ÀíÒ»¸ö¼õ¼õ
      //map<int, channel*>  ÎÄ¼şÃèÊö·ûºÍÍ¨µÀÖ¸ÕëµÄmap
      ChannelMap::const_iterator ch = channels_.find(pfd->fd);//µ÷ÓÃmap.findº¯Êı
      assert(ch != channels_.end()); //¶ÏÑÔÕÒµ½ÊÂ¼ş
      Channel* channel = ch->second;   //»ñÈ¡ÊÂ¼ş
      assert(channel->fd() == pfd->fd);  //¶ÏÑÔÏàµÈ
      channel->set_revents(pfd->revents);   //ÉèÖÃÒª·µ»ØµÄÊÂ¼şÀàĞÍ
      // pfd->revents = 0;
      activeChannels->push_back(channel);  //¼ÓÈë»îÔ¾ÊÂ¼şÊı×é
    }
  }
}

//ÓÃÓÚ×¢²á»òÕß¸üĞÂÍ¨µÀ
void PollPoller::updateChannel(Channel* channel)
{
  Poller::assertInLoopThread();  //¶ÏÑÔÖ»ÄÜÔÚI/OÏß³ÌÖĞµ÷ÓÃ
  LOG_TRACE << "fd = " << channel->fd() << " events = " << channel->events();
  if (channel->index() < 0)  //Èç¹ûĞ¡ÓÚ0£¬ËµÃ÷»¹Ã»ÓĞ×¢²á£
  {
    // a new one, add to pollfds_
    assert(channels_.find(channel->fd()) == channels_.end());//¶ÏÑÔ²éÕÒ²»µ½
    struct pollfd pfd;
    pfd.fd = channel->fd();
    pfd.events = static_cast<short>(channel->events());
    pfd.revents = 0;  
    pollfds_.push_back(pfd);  //ºÏ³Épollfd
    int idx = static_cast<int>(pollfds_.size())-1;  //idx·ÅÔÚÄ©Î²£¬ÒòÎªÊÇ×îĞÂµÄ£¬Õâ¸öidxÊÇpollfd_µÄÏÂ±ê
    channel->set_index(idx);  //idxÊÇchannelµÄ³ÉÔ±±äÁ¿
    channels_[pfd.fd] = channel; //¼ÓÈëmapÖĞ
  }
  else   //Èç¹û´óÓÚ0£¬ËµÃ÷ÊÇ¸üĞÂÒ»¸öÒÑ´æÔÚµÄÍ¨µÀ
  {
    // update existing one
    assert(channels_.find(channel->fd()) != channels_.end());
    assert(channels_[channel->fd()] == channel);
    int idx = channel->index();   //È¡³öchannelµÄÏÂ±ê
    assert(0 <= idx && idx < static_cast<int>(pollfds_.size()));
    struct pollfd& pfd = pollfds_[idx];  //»ñÈ¡pollfd£¬ÒÔÒıÓÃ·½Ê½Ìá¸ßĞ§ÂÊ
    assert(pfd.fd == channel->fd() || pfd.fd == -channel->fd()-1);  //-channel->fd()-1ÊÇÒòÎªÆ¥ÅäÏÂÃæÔİÊ±²»¹Ø×¢×´Ì¬µÄÉèÖÃ
    pfd.events = static_cast<short>(channel->events());   //¸üĞÂevents
    pfd.revents = 0;
    //½«Ò»¸öÍ¨µÀÔİÊ±¸ü¸ÄÎª²»¹Ø×¢ÊÂ¼ş£¬µ«²»´ÓPollerÖĞÒÆ³ıÍ¨µÀ£¬¿ÉÄÜÏÂÒ»´Î»¹»áÓÃ
    if (channel->isNoneEvent())  
    {
      // ignore this pollfd
      //ÔİÊ±ºöÂÔ¸ÃÎÄ¼şÃèÊö·ûµÄÊ±¼ä
      //ÕâÀïpfd.fd¿ÉÒÔÖ±½ÓÉèÖÃÎª-1
      pfd.fd = -channel->fd()-1;   //ÕâÑù×ÓÉèÖÃÊÇÎªÁËremoveChannelÓÅ»¯
    }
  }
}

//Õâ¸öÊÇÕæÕıÒÆ³ıpollfdµÄ³ÌĞò
void PollPoller::removeChannel(Channel* channel)
{
  Poller::assertInLoopThread();
  LOG_TRACE << "fd = " << channel->fd();
  assert(channels_.find(channel->fd()) != channels_.end());//É¾³ı±ØĞëÄÜÕÒµ½
  assert(channels_[channel->fd()] == channel);  //Ò»¶¨¶ÔÓ¦
  assert(channel->isNoneEvent());  //Ò»¶¨ÒÑ¾­Ã»ÓĞÊÂ¼şÉÏµÄ¹Ø×¢ÁË£¬ËùÒÔÔÚµ÷ÓÃremoveChannelÖ®Ç°±ØĞë
  int idx = channel->index();  //È¡³öÔÚpollfds_Êı×éÏÂ±êÖĞµÄÎ»ÖÃ
  assert(0 <= idx && idx < static_cast<int>(pollfds_.size()));
  const struct pollfd& pfd = pollfds_[idx]; (void)pfd;  //µÃµ½pollfd
  assert(pfd.fd == -channel->fd()-1 && pfd.events == channel->events());
  size_t n = channels_.erase(channel->fd()); //Ê¹ÓÃ¼üÖµÒÆ³ı´ÓmapÖĞÒÆ³ı
  assert(n == 1); (void)n;
  if (implicit_cast<size_t>(idx) == pollfds_.size()-1)  //Èç¹ûÊÇ×îºóÒ»¸ö
  {
    pollfds_.pop_back();  //Ö±½Ópop_back()
  }
  else  //²»ÊÇvector×îºóÒ»¸ö£¬Õâ¸öÎÊÌâ¾ÍÀ´ÁË£¡vectorÒÆ³ıÇ°ÃæÔªËØ¿ÉÄÜ»áÉæ¼°µ½°ÉºóÃæÔªËØÅ²¶¯£¬ËùÒÔ²»¿ÉÈ¡¡£
  {       //ÕâÀï²ÉÓÃµÄÊÇ½»»»µÄ·½·¨£¬ÀàËÆÓÚÍ¼Ëã·¨ÖĞÉ¾³ı¶¥µã£¬ÎÒÃÇ½«ÒªÒÆ³ıµÄÔªËØºÍ×îºóÒ»¸öÔªËØ½»»»¼´¿É¡£
  			//ÕâÀïËã·¨Ê±¼ä¸´ÔÓ¶ÈÊÇO(1)
    int channelAtEnd = pollfds_.back().fd;   //µÃµ½×îºóÒ»¸öÔªËØ
    iter_swap(pollfds_.begin()+idx, pollfds_.end()-1);  //½»»»
    if (channelAtEnd < 0)  //Èç¹û×îºóÒ»¸ö<0
    {
      channelAtEnd = -channelAtEnd-1;  //°ÑËü»¹Ô­³öÀ´£¬µÃµ½ÕæÊµµÄfd£¬Õâ¾ÍÊÇ²»Ê¹ÓÃ-1µÄÔ­Òò£¬ÎÒÃÇ²»¹Ø×¢ËüÁË
      															//°ÑËü¸ÄÎª-fd-1£¬È»ºóÉ¾³ıµÄÊ±ºò¿ÉÒÔÔÙ»¹Ô­»ØÀ´¡£Èç¹û¸ÄÎª-1¾Í²»¿ÉÒÔÁË¡£
    }
    channels_[channelAtEnd]->set_index(idx); //¶Ô¸ÃÕæÊµµÄfd¸üĞÂÏÂ±ê
    pollfds_.pop_back();  //µ¯³öÄ©Î²ÔªËØ
  }
}

