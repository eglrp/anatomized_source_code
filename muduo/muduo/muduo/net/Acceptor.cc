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
    acceptSocket_(sockets::createNonblockingOrDie(listenAddr.family())), //���������׽���
    acceptChannel_(loop, acceptSocket_.fd()),  //��Channel��socketfd
    listenning_(false),  
    idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC))  //Ԥ��׼��һ�������ļ�������
{
  assert(idleFd_ >= 0);  //>=0
  acceptSocket_.setReuseAddr(true);
  acceptSocket_.setReusePort(reuseport);
  acceptSocket_.bindAddress(listenAddr);
  acceptChannel_.setReadCallback(                        
      boost::bind(&Acceptor::handleRead, this));   //����Channel��fd�Ķ��ص�����
}

Acceptor::~Acceptor()
{
  acceptChannel_.disableAll();   //��Ҫ�������¼���disable�������ܵ���remove����
  acceptChannel_.remove();
  ::close(idleFd_);    //�ر��ļ�������
}

//Acceptor����listen�п�ʼ��ע�ɶ��¼�
void Acceptor::listen()
{
  loop_->assertInLoopThread();
  listenning_ = true;
  acceptSocket_.listen();
  acceptChannel_.enableReading();   //��ע�ɶ��¼�
}

void Acceptor::handleRead()    //���ص�����
{
  loop_->assertInLoopThread();
  InetAddress peerAddr;
  //FIXME loop until no more
  int connfd = acceptSocket_.accept(&peerAddr);   //����������׽���
  if (connfd >= 0)
  {
    // string hostport = peerAddr.toIpPort();
    // LOG_TRACE << "Accepts of " << hostport;
    if (newConnectionCallback_)   //������������ٽֻص�����
    {
      newConnectionCallback_(connfd, peerAddr);   //��ô��ִ����
    }
    else
    {
      sockets::close(connfd);  //����͹ر�
    }
  }
  else   //���<0ʧ���ˣ
  {
    LOG_SYSERR << "in Acceptor::handleRead";
    // Read the section named "The special problem of
    // accept()ing when you can't" in libev's doc.
    // By Marc Lehmann, author of livev.
    if (errno == EMFILE)   //̫����ļ�������
    {
      ::close(idleFd_);   //�ȹرտ����ļ��������������ܹ����ա��������ڲ��õ�ƽ�����������ջ�һֱ������
      idleFd_ = ::accept(acceptSocket_.fd(), NULL, NULL);  //�Ǿ��ڳ�һ���ļ�������������accept
      ::close(idleFd_);  //accept֮���ٹر�
      idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);  //Ȼ���ٴ򿪳�Ĭ�Ϸ�ʽ
    }
  }
}

