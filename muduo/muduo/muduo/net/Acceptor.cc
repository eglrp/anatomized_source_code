// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/Acceptor.h>

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/SocketsOps.h>

#include <boost/bind.hpp>

#include <errno.h>
#include <fcntl.h>
//#include <sys/types.h>
//#include <sys/stat.h>

using namespace muduo;
using namespace muduo::net;

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport)
  : loop_(loop),
    acceptSocket_(sockets::createNonblockingOrDie(listenAddr.family())), //´´½¨¼àÌýÌ×½Ó×Ö
    acceptChannel_(loop, acceptSocket_.fd()),  //°ó¶¨ChannelºÍsocketfd
    listenning_(false),  
    idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC))  //Ô¤ÏÈ×¼±¸Ò»¸ö¿ÕÏÐÎÄ¼þÃèÊö·û
{
  assert(idleFd_ >= 0);  //>=0
  acceptSocket_.setReuseAddr(true);
  acceptSocket_.setReusePort(reuseport);
  acceptSocket_.bindAddress(listenAddr);
  acceptChannel_.setReadCallback(                        
      boost::bind(&Acceptor::handleRead, this));   //ÉèÖÃChannelµÄfdµÄ¶Á»Øµ÷º¯Êý
}

Acceptor::~Acceptor()
{
  acceptChannel_.disableAll();   //ÐèÒª°ÑËùÓÐÊÂ¼þ¶¼disableµô£¬²ÅÄÜµ÷ÓÃremoveº¯Êý
  acceptChannel_.remove();
  ::close(idleFd_);    //¹Ø±ÕÎÄ¼þÃèÊö·û
}

//AcceptorÊÇÔÚlistenÖÐ¿ªÊ¼¹Ø×¢¿É¶ÁÊÂ¼þ
void Acceptor::listen()
{
  loop_->assertInLoopThread();
  listenning_ = true;
  acceptSocket_.listen();
  acceptChannel_.enableReading();   //¹Ø×¢¿É¶ÁÊÂ¼þ
}

void Acceptor::handleRead()    //¶Á»Øµ÷º¯Êý
{
  loop_->assertInLoopThread();
  InetAddress peerAddr;
  //FIXME loop until no more
  int connfd = acceptSocket_.accept(&peerAddr);   //»ñµÃÒÑÁ¬½ÓÌ×½Ó×Ö
  if (connfd >= 0)
  {
    // string hostport = peerAddr.toIpPort();
    // LOG_TRACE << "Accepts of " << hostport;
    if (newConnectionCallback_)   //Èç¹ûÉèÖÃÁËÐÂÁÙ½Ö»Øµ÷º¯Êý
    {
      newConnectionCallback_(connfd, peerAddr);   //ÄÇÃ´¾ÍÖ´ÐÐËü
    }
    else
    {
      sockets::close(connfd);  //·ñÔò¾Í¹Ø±Õ
    }
  }
  else   //Èç¹û<0Ê§°ÜÁË£
  {
    LOG_SYSERR << "in Acceptor::handleRead";
    // Read the section named "The special problem of
    // accept()ing when you can't" in libev's doc.
    // By Marc Lehmann, author of livev.
    if (errno == EMFILE)   //Ì«¶àµÄÎÄ¼þÃèÊö·û
    {
      ::close(idleFd_);   //ÏÈ¹Ø±Õ¿ÕÏÐÎÄ¼þÃèÊö·û£¬ÈÃËüÄÜ¹»½ÓÊÕ¡£·ñÔòÓÉÓÚ²ÉÓÃµçÆ½´¥·¢£¬²»½ÓÊÕ»áÒ»Ö±´¥·¢¡£
      idleFd_ = ::accept(acceptSocket_.fd(), NULL, NULL);  //ÄÇ¾ÍÌÚ³öÒ»¸öÎÄ¼þÃèÊö·û£¬ÓÃÀ´accept
      ::close(idleFd_);  //acceptÖ®ºóÔÙ¹Ø±Õ
      idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);  //È»ºóÔÙ´ò¿ª³ÉÄ¬ÈÏ·½Ê½
    }
  }
}

