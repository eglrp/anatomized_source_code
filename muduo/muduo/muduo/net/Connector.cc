// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//

#include <muduo/net/Connector.h>

#include <muduo/base/Logging.h>
#include <muduo/net/Channel.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/SocketsOps.h>

#include <boost/bind.hpp>

#include <errno.h>

using namespace muduo;
using namespace muduo::net;

const int Connector::kMaxRetryDelayMs;

Connector::Connector(EventLoop* loop, const InetAddress& serverAddr)
  : loop_(loop),
    serverAddr_(serverAddr),
    connect_(false),
    state_(kDisconnected),
    retryDelayMs_(kInitRetryDelayMs)   //初始化延时
{
  LOG_DEBUG << "ctor[" << this << "]";
}

Connector::~Connector()
{
  LOG_DEBUG << "dtor[" << this << "]";
  assert(!channel_); 
}

//发起连接
void Connector::start()
{
  connect_ = true;
  loop_->runInLoop(boost::bind(&Connector::startInLoop, this)); // FIXME: unsafe
}

//发起连接
void Connector::startInLoop()
{
  loop_->assertInLoopThread();
  assert(state_ == kDisconnected);
  if (connect_)   //调用前必须connect_为true，start()函数中会这么做
  {
    connect();  //连接具体实现
  }
  else
  {
    LOG_DEBUG << "do not connect";
  }
}

//停止
void Connector::stop()
{
  connect_ = false;
  loop_->queueInLoop(boost::bind(&Connector::stopInLoop, this)); // FIXME: unsafe
  // FIXME: cancel timer
}

//停止
void Connector::stopInLoop()
{
  loop_->assertInLoopThread();
  if (state_ == kConnecting)
  {
    setState(kDisconnected);
    int sockfd = removeAndResetChannel();  //将通道从poller中移除关注，并将channel置空
    retry(sockfd);   //这里并非要重连，只是调用sockets::close(sockfd)
  }
}

//连接实现
void Connector::connect()
{
  //设置非阻塞，否则退出
  int sockfd = sockets::createNonblockingOrDie(serverAddr_.family());
  int ret = sockets::connect(sockfd, serverAddr_.getSockAddr());
  int savedErrno = (ret == 0) ? 0 : errno;
  switch (savedErrno)  //检查错误码
  {
    case 0:
    case EINPROGRESS:  //非阻塞套接字，未连接成功返回码是EINPROGRESS表示正在连接
    case EINTR:
    case EISCONN:   //连接成功
      connecting(sockfd);
      break;

    case EAGAIN:
    case EADDRINUSE:
    case EADDRNOTAVAIL:
    case ECONNREFUSED:
    case ENETUNREACH:
      retry(sockfd);   //重连
      break;

    case EACCES:
    case EPERM:
    case EAFNOSUPPORT:
    case EALREADY:
    case EBADF:
    case EFAULT:
    case ENOTSOCK:
      LOG_SYSERR << "connect error in Connector::startInLoop " << savedErrno;
      sockets::close(sockfd);   //这几种情况不能重连，
      break;

    default:
      LOG_SYSERR << "Unexpected error in Connector::startInLoop " << savedErrno;
      sockets::close(sockfd);
      // connectErrorCallback_();
      break;
  }
}

//重启
void Connector::restart()
{
  loop_->assertInLoopThread();
  setState(kDisconnected);
  retryDelayMs_ = kInitRetryDelayMs;
  connect_ = true;
  startInLoop();  //开始连接
}

//如果连接成功
void Connector::connecting(int sockfd)
{
  setState(kConnecting);
  assert(!channel_);
  //Channel与sockfd关联
  channel_.reset(new Channel(loop_, sockfd));
  //设置可写回调函数，这时候如果socket没有错误，sockfd就处于可写状态
  channel_->setWriteCallback(
      boost::bind(&Connector::handleWrite, this)); // FIXME: unsafe
      //设置错误回调函数
  channel_->setErrorCallback(
      boost::bind(&Connector::handleError, this)); // FIXME: unsafe

  // channel_->tie(shared_from_this()); is not working,
  // as channel_ is not managed by shared_ptr
  //关注可写事件
  channel_->enableWriting();
}

int Connector::removeAndResetChannel()
{
  channel_->disableAll();
  channel_->remove();   //从poller中移除
  int sockfd = channel_->fd();
  // Can't reset channel_ here, because we are inside Channel::handleEvent
  //不能在这里重置channel_，因为当前我们可能正在调用channel的handleEvent，进而调用下面的handleWrite，其中又会调用本函数，形成死循环
  //所以要加入queueInLoop去处理
  loop_->queueInLoop(boost::bind(&Connector::resetChannel, this)); // FIXME: unsafe
  return sockfd;
}

void Connector::resetChannel()
{
  channel_.reset();
}

//可写事件处理
void Connector::handleWrite()
{
  LOG_TRACE << "Connector::handleWrite " << state_;

  if (state_ == kConnecting)
  {
    //从poller中移除关注，并且将Channel置空
    int sockfd = removeAndResetChannel();
    //socket可写并不意味着连接一定建立成功
    //还需要用getsockopt(sockfd, SOL_SOCKET, SO_ERROR，...)再次确认一下。
    int err = sockets::getSocketError(sockfd); 
    if (err)  //有错误
    {
      LOG_WARN << "Connector::handleWrite - SO_ERROR = "
               << err << " " << strerror_tl(err);
      retry(sockfd);   //重连
    }
    else if (sockets::isSelfConnect(sockfd))  //自连接，loopback
    {
      LOG_WARN << "Connector::handleWrite - Self connect";
      retry(sockfd);
    }
    else  //连接成功
    {
      setState(kConnected);  //设置已连接状态 
      if (connect_)  //如果连接
      {
        newConnectionCallback_(sockfd);  //调用用户设定的新连接函数，和TcpConnection中那个不是一个
      }
      else
      {
        sockets::close(sockfd);  //否则直接关闭
      }
    }
  }
  else
  {
    // what happened?
    assert(state_ == kDisconnected);
  }
}

//错误回调
void Connector::handleError()
{
  LOG_ERROR << "Connector::handleError state=" << state_;
  if (state_ == kConnecting)
  {
    //从poller中移除并重新发起连接
    int sockfd = removeAndResetChannel();
    int err = sockets::getSocketError(sockfd);
    LOG_TRACE << "SO_ERROR = " << err << " " << strerror_tl(err);
    retry(sockfd);
  }
}

//重连函数，采用back-off策略重连，也就是退避策略
//也就是重连时间逐渐延长，0.5s,1s,2s,...一直到30s
void Connector::retry(int sockfd)
{
  sockets::close(sockfd);   //先关闭连接
  setState(kDisconnected);  
  if (connect_)
  {
    LOG_INFO << "Connector::retry - Retry connecting to " << serverAddr_.toIpPort()
             << " in " << retryDelayMs_ << " milliseconds. ";
    //隔一段时间后重连，重新启用startInLoop
    loop_->runAfter(retryDelayMs_/1000.0,
                    boost::bind(&Connector::startInLoop, shared_from_this()));
    //间隔时间2倍增长
    retryDelayMs_ = std::min(retryDelayMs_ * 2, kMaxRetryDelayMs);
  }
  else   //超出最大重连时间后，输出连接失败
  {
    LOG_DEBUG << "do not connect";
  }
}

