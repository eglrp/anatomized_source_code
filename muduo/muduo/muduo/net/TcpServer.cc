// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/TcpServer.h>

#include <muduo/base/Logging.h>
#include <muduo/net/Acceptor.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThreadPool.h>
#include <muduo/net/SocketsOps.h>

#include <boost/bind.hpp>

#include <stdio.h>  // snprintf

using namespace muduo;
using namespace muduo::net;

TcpServer::TcpServer(EventLoop* loop,
                     const InetAddress& listenAddr,
                     const string& nameArg,
                     Option option)
  : loop_(CHECK_NOTNULL(loop)),  //����Ϊ�գ����򴥷�FATAL
    ipPort_(listenAddr.toIpPort()),  //�˿ں�
    name_(nameArg),  //����
    acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)), //����Acceptor��ʹ��scoped_ptr����
    threadPool_(new EventLoopThreadPool(loop, name_)),
    connectionCallback_(defaultConnectionCallback),
    messageCallback_(defaultMessageCallback),
    nextConnId_(1)   //��һ�������ӱ��id
{
  acceptor_->setNewConnectionCallback(
      //Acceptor::handleRead�����л�ص���TcpServer::newConnection
      //_1��Ӧ��socket�ļ���������_2��Ӧ���ǶԵȷ��ĵ�ַ
      boost::bind(&TcpServer::newConnection, this, _1, _2));   //����һ�����ӻص�����
}

TcpServer::~TcpServer()
{
  loop_->assertInLoopThread();
  LOG_TRACE << "TcpServer::~TcpServer [" << name_ << "] destructing";

  for (ConnectionMap::iterator it(connections_.begin());
      it != connections_.end(); ++it)
  {
    TcpConnectionPtr conn = it->second;
    it->second.reset();
    conn->getLoop()->runInLoop(
      boost::bind(&TcpConnection::connectDestroyed, conn));
    conn.reset();
  }
}

void TcpServer::setThreadNum(int numThreads)
{
  assert(0 <= numThreads);
  threadPool_->setThreadNum(numThreads);  //����I/O�̸߳�����������main Reactor
}

//�ú��������ظ�����
//�ú������Կ��̵߳���
void TcpServer::start()
{
  if (started_.getAndSet(1) == 0)   //��getȻ��õ������0��Ȼ��ֵΪ1���Ժ�Ϊ1�Ͳ������if���  
  {
    threadPool_->start(threadInitCallback_);  //�����̳߳�

    assert(!acceptor_->listenning());
    //��Ϊacceptor��ָ��ָ�����⺯��get_pointer���Է���ԭ��ָ��
    loop_->runInLoop(
        boost::bind(&Acceptor::listen, get_pointer(acceptor_)));  
  }
}

//�����Ӵ�����
void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr)
{
  loop_->assertInLoopThread();
  //ʹ��round-robinѡ��һ��I/O loop
  EventLoop* ioLoop = threadPool_->getNextLoop();
  char buf[64];
  snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);  //�˿�+����id
  ++nextConnId_;  //++֮�������һ������id
  string connName = name_ + buf;

  LOG_INFO << "TcpServer::newConnection [" << name_
           << "] - new connection [" << connName
           << "] from " << peerAddr.toIpPort();

  //���챾�ص�ַ
  InetAddress localAddr(sockets::getLocalAddr(sockfd));
  // FIXME poll with zero timeout to double confirm the new connection
  // FIXME use make_shared if necessary

  TcpConnectionPtr conn(new TcpConnection(ioLoop,   //����һ�����Ӷ���ioLoop��round-robinѡ�������
                                          connName,
                                          sockfd,
                                          localAddr,
                                          peerAddr));
  //TcpConnection��use_count�˴�Ϊ1���½���һ��Tcpconnection
  connections_[connName] = conn;
  //TcpConnection��use_count�˴�Ϊ2����Ϊ���뵽connections_�С�

  //ʵ��TcpServer��connectionCallback�Ȼص������Ƕ�conn�Ļص������ķ�װ���������������ù�ȥ
  conn->setConnectionCallback(connectionCallback_);
  conn->setMessageCallback(messageCallback_);
  conn->setWriteCompleteCallback(writeCompleteCallback_);

  //��TcpServer��removeConnection��֪����TcpConnection�Ĺرջص�������
  conn->setCloseCallback(
      boost::bind(&TcpServer::removeConnection, this, _1)); // FIXME: unsafe
  ioLoop->runInLoop(boost::bind(&TcpConnection::connectEstablished, conn));
  //����TcpConenction:;connectEstablished�����ڲ��Ὣuse_count��һȻ���һ���˴���Ϊ2
  //���Ǳ��������ܽ�����conn��������������������ü���Ϊ1����ʣconnections_�б��д��һ��
}



void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{
  // FIXME: unsafe
  loop_->runInLoop(boost::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

//��Connection�Ƴ�
void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
  loop_->assertInLoopThread();
  LOG_INFO << "TcpServer::removeConnectionInLoop [" << name_
           << "] - connection " << conn->name();
  size_t n = connections_.erase(conn->name());    //ConnectionMap���������б���ɾ��������
  (void)n;
  assert(n == 1);
  EventLoop* ioLoop = conn->getLoop();   //����������ڵ�loopָ��
  ioLoop->queueInLoop(
      boost::bind(&TcpConnection::connectDestroyed, conn));   //��TcpConnection�����ټ��뵽I/O������
}

