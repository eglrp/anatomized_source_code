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
  : loop_(CHECK_NOTNULL(loop)),  //不能为空，否则触发FATAL
    ipPort_(listenAddr.toIpPort()),  //端口号
    name_(nameArg),  //名称
    acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)), //创建Acceptor，使用scoped_ptr管理
    threadPool_(new EventLoopThreadPool(loop, name_)),
    connectionCallback_(defaultConnectionCallback),
    messageCallback_(defaultMessageCallback),
    nextConnId_(1)   //下一个已连接编号id
{
  acceptor_->setNewConnectionCallback(
      //Acceptor::handleRead函数中会回调用TcpServer::newConnection
      //_1对应得socket文件描述符，_2对应的是对等方的地址
      boost::bind(&TcpServer::newConnection, this, _1, _2));   //设置一个连接回调函数
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
  threadPool_->setThreadNum(numThreads);  //设置I/O线程个数，不包含main Reactor
}

//该函数可以重复调用
//该函数可以跨线程调用
void TcpServer::start()
{
  if (started_.getAndSet(1) == 0)   //先get然后得到结果是0，然后赋值为1，以后都为1就不会进入if语句  
  {
    threadPool_->start(threadInitCallback_);  //启动线程池

    assert(!acceptor_->listenning());
    //因为acceptor是指针指征，库函数get_pointer可以返回原生指针
    loop_->runInLoop(
        boost::bind(&Acceptor::listen, get_pointer(acceptor_)));  
  }
}

//新连接处理函数
void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr)
{
  loop_->assertInLoopThread();
  //使用round-robin选组一个I/O loop
  EventLoop* ioLoop = threadPool_->getNextLoop();
  char buf[64];
  snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);  //端口+连接id
  ++nextConnId_;  //++之后就是下一个连接id
  string connName = name_ + buf;

  LOG_INFO << "TcpServer::newConnection [" << name_
           << "] - new connection [" << connName
           << "] from " << peerAddr.toIpPort();

  //构造本地地址
  InetAddress localAddr(sockets::getLocalAddr(sockfd));
  // FIXME poll with zero timeout to double confirm the new connection
  // FIXME use make_shared if necessary

  TcpConnectionPtr conn(new TcpConnection(ioLoop,   //创建一个连接对象，ioLoop是round-robin选择出来的
                                          connName,
                                          sockfd,
                                          localAddr,
                                          peerAddr));
  //TcpConnection的use_count此处为1，新建了一个Tcpconnection
  connections_[connName] = conn;
  //TcpConnection的use_count此处为2，因为加入到connections_中。

  //实际TcpServer的connectionCallback等回调函数是对conn的回调函数的封装，所以在这里设置过去
  conn->setConnectionCallback(connectionCallback_);
  conn->setMessageCallback(messageCallback_);
  conn->setWriteCompleteCallback(writeCompleteCallback_);

  //将TcpServer的removeConnection设知道了TcpConnection的关闭回调函数中
  conn->setCloseCallback(
      boost::bind(&TcpServer::removeConnection, this, _1)); // FIXME: unsafe
  ioLoop->runInLoop(boost::bind(&TcpConnection::connectEstablished, conn));
  //调用TcpConenction:;connectEstablished函数内部会将use_count加一然后减一，此处仍为2
  //但是本函数介绍结束后conn对象会析构掉，所以引用计数为1，仅剩connections_列表中存活一个
}



void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{
  // FIXME: unsafe
  loop_->runInLoop(boost::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

//将Connection移除
void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
  loop_->assertInLoopThread();
  LOG_INFO << "TcpServer::removeConnectionInLoop [" << name_
           << "] - connection " << conn->name();
  size_t n = connections_.erase(conn->name());    //ConnectionMap，从连接列表中删除连接名
  (void)n;
  assert(n == 1);
  EventLoop* ioLoop = conn->getLoop();   //获得连接所在的loop指针
  ioLoop->queueInLoop(
      boost::bind(&TcpConnection::connectDestroyed, conn));   //将TcpConnection的销毁加入到I/O队列中
}

