// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/TcpConnection.h>

#include <muduo/base/Logging.h>
#include <muduo/base/WeakCallback.h>
#include <muduo/net/Channel.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/Socket.h>
#include <muduo/net/SocketsOps.h>

#include <boost/bind.hpp>

#include <errno.h>

using namespace muduo;
using namespace muduo::net;

void muduo::net::defaultConnectionCallback(const TcpConnectionPtr& conn)
{
  LOG_TRACE << conn->localAddress().toIpPort() << " -> "
            << conn->peerAddress().toIpPort() << " is "
            << (conn->connected() ? "UP" : "DOWN");
  // do not call conn->forceClose(), because some users want to register message callback only.
}

void muduo::net::defaultMessageCallback(const TcpConnectionPtr&,
                                        Buffer* buf,
                                        Timestamp)
{
  buf->retrieveAll();
}

TcpConnection::TcpConnection(EventLoop* loop,
                             const string& nameArg,
                             int sockfd,
                             const InetAddress& localAddr,
                             const InetAddress& peerAddr)
  : loop_(CHECK_NOTNULL(loop)),
    name_(nameArg),   //��������
    state_(kConnecting),   //״̬
    socket_(new Socket(sockfd)),   //�����׽���
    channel_(new Channel(loop, sockfd)),   //����ͨ��
    localAddr_(localAddr),   //���ص�ַ
    peerAddr_(peerAddr),    //�Եȷ���ַ
    highWaterMark_(64*1024*1024),   
    reading_(true)
{
  //ͨ���ɶ��¼�������ʱ�򣬻ص�TcpConnection::handleRead����
  channel_->setReadCallback(
      boost::bind(&TcpConnection::handleRead, this, _1));
  channel_->setWriteCallback(
      boost::bind(&TcpConnection::handleWrite, this));
  channel_->setCloseCallback(
      boost::bind(&TcpConnection::handleClose, this));
  channel_->setErrorCallback(
      boost::bind(&TcpConnection::handleError, this));
  LOG_DEBUG << "TcpConnection::ctor[" <<  name_ << "] at " << this
            << " fd=" << sockfd;

  //��һ�����ҵĿɲ����ף�����keepalive
  /////////////////////////////////////////////////////////////////////////////////////////////////////////
  socket_->setKeepAlive(true);
  ///////////////////////////////////////////////////////////////////////////////////////////////////////////////
}

TcpConnection::~TcpConnection()
{
  LOG_DEBUG << "TcpConnection::dtor[" <<  name_ << "] at " << this
            << " fd=" << channel_->fd()
            << " state=" << stateToString();
  assert(state_ == kDisconnected);
}

bool TcpConnection::getTcpInfo(struct tcp_info* tcpi) const
{
  return socket_->getTcpInfo(tcpi);
}

string TcpConnection::getTcpInfoString() const
{
  char buf[1024];
  buf[0] = '\0';
  socket_->getTcpInfoString(buf, sizeof buf);
  return buf;
}

//���÷����ַ�����send�������ַ�������
void TcpConnection::send(const void* data, int len)
{
  send(StringPiece(static_cast<const char*>(data), len));
}

//ʵ��send
void TcpConnection::send(const StringPiece& message)
{
  if (state_ == kConnected)
  {
    //�����I/O�̣߳�ֱ�ӵ���
    if (loop_->isInLoopThread())
    {
      sendInLoop(message);
    }
    else
    {
      //������ڣ����̵߳��ã���Ҫ���ݻ�����message��ȥ����һ���Ŀ���
      loop_->runInLoop(
          boost::bind(&TcpConnection::sendInLoop,
                      this,     // FIXME
                      message.as_string()));
                    //std::forward<string>(message)));
    }
  }
}

//�����Զ���Buffer������
// FIXME efficiency!!!
void TcpConnection::send(Buffer* buf)
{
  if (state_ == kConnected)
  {
    if (loop_->isInLoopThread())
    {
      sendInLoop(buf->peek(), buf->readableBytes());
      buf->retrieveAll();
    }
    else
    {
      loop_->runInLoop(
          boost::bind(&TcpConnection::sendInLoop,
                      this,     // FIXME
                      buf->retrieveAllAsString()));
                    //std::forward<string>(message)));
    }
  }
}

//�����ַ���
void TcpConnection::sendInLoop(const StringPiece& message)
{
  sendInLoop(message.data(), message.size());
}

void TcpConnection::sendInLoop(const void* data, size_t len)
{
  loop_->assertInLoopThread();
  ssize_t nwrote = 0;
  size_t remaining = len;
  bool faultError = false;
  if (state_ == kDisconnected)
  {
    LOG_WARN << "disconnected, give up writing";
    return;
  }
  //û�й�ע��д�¼������һ�������ĿΪ0��ֱ�ӷ���
  // if no thing in output queue, try writing directly
  if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
  {
    nwrote = sockets::write(channel_->fd(), data, len);
    if (nwrote >= 0)
    {
      remaining = len - nwrote;
      if (remaining == 0 && writeCompleteCallback_)
      {
        loop_->queueInLoop(boost::bind(writeCompleteCallback_, shared_from_this()));
      }
    }
    else // nwrote < 0
    {
      nwrote = 0;
      if (errno != EWOULDBLOCK)
      {
        LOG_SYSERR << "TcpConnection::sendInLoop";
        if (errno == EPIPE || errno == ECONNRESET) // FIXME: any others?
        {
          faultError = true;
        }
      }
    }
  }

  //û�д��󣬲��һ���δд�������(˵���ں˻���������Ҫ��δд���������ӵ�output buffer��
  assert(remaining <= len);
  if (!faultError && remaining > 0)
  {
    //���������ˮλ��־���߼����������⣬�ص�hishWaterMarkCallback����������Ҫд
    size_t oldLen = outputBuffer_.readableBytes();
    if (oldLen + remaining >= highWaterMark_
        && oldLen < highWaterMark_
        && highWaterMarkCallback_)
    {
      loop_->queueInLoop(boost::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
    }

    //��ʣ������׷�ӵ�outputbuffer����ע��POLLOUT�¼�
    outputBuffer_.append(static_cast<const char*>(data)+nwrote, remaining);
    if (!channel_->isWriting())
    {
      channel_->enableWriting();
    }
  }
}


//�����÷�
void TcpConnection::shutdown()
{
  // FIXME: use compare and swap
  if (state_ == kConnected)  //�������������״̬
  {
    setState(kDisconnecting);  //��Ϊȡ��������״̬���ز��ر����ӻ�Ҫ��������
    // FIXME: shared_from_this()?
    //I/O�̵߳���
    loop_->runInLoop(boost::bind(&TcpConnection::shutdownInLoop, this));
  }
}

void TcpConnection::shutdownInLoop()
{
  loop_->assertInLoopThread();
  if (!channel_->isWriting())  //������ٹ�עPOLLOUT�¼��������ܹرգ��Ǿͽ����ǽ�״̬����ΪkDisconnecting�����ܹر�����
  {
    // we are not writing
    socket_->shutdownWrite();
  }
}

// void TcpConnection::shutdownAndForceCloseAfter(double seconds)
// {
//   // FIXME: use compare and swap
//   if (state_ == kConnected)
//   {
//     setState(kDisconnecting);
//     loop_->runInLoop(boost::bind(&TcpConnection::shutdownAndForceCloseInLoop, this, seconds));
//   }
// }

// void TcpConnection::shutdownAndForceCloseInLoop(double seconds)
// {
//   loop_->assertInLoopThread();
//   if (!channel_->isWriting())
//   {
//     // we are not writing
//     socket_->shutdownWrite();
//   }
//   loop_->runAfter(
//       seconds,
//       makeWeakCallback(shared_from_this(),
//                        &TcpConnection::forceCloseInLoop));
// }

void TcpConnection::forceClose()
{
  // FIXME: use compare and swap
  if (state_ == kConnected || state_ == kDisconnecting)
  {
    setState(kDisconnecting);
    loop_->queueInLoop(boost::bind(&TcpConnection::forceCloseInLoop, shared_from_this()));
  }
}

void TcpConnection::forceCloseWithDelay(double seconds)
{
  if (state_ == kConnected || state_ == kDisconnecting)
  {
    setState(kDisconnecting);
    loop_->runAfter(
        seconds,
        makeWeakCallback(shared_from_this(),
                         &TcpConnection::forceClose));  // not forceCloseInLoop to avoid race condition
  }
}

void TcpConnection::forceCloseInLoop()
{
  loop_->assertInLoopThread();
  if (state_ == kConnected || state_ == kDisconnecting)
  {
    // as if we received 0 byte in handleRead();
    handleClose();
  }
}

const char* TcpConnection::stateToString() const
{
  switch (state_)
  {
    case kDisconnected:
      return "kDisconnected";
    case kConnecting:
      return "kConnecting";
    case kConnected:
      return "kConnected";
    case kDisconnecting:
      return "kDisconnecting";
    default:
      return "unknown state";
  }
}

void TcpConnection::setTcpNoDelay(bool on)
{
  socket_->setTcpNoDelay(on);
}

void TcpConnection::startRead()
{
  loop_->runInLoop(boost::bind(&TcpConnection::startReadInLoop, this));
}

void TcpConnection::startReadInLoop()
{
  loop_->assertInLoopThread();
  if (!reading_ || !channel_->isReading())
  {
    channel_->enableReading();
    reading_ = true;
  }
}

void TcpConnection::stopRead()
{
  loop_->runInLoop(boost::bind(&TcpConnection::stopReadInLoop, this));
}

void TcpConnection::stopReadInLoop()
{
  loop_->assertInLoopThread();
  if (reading_ || channel_->isReading())
  {
    channel_->disableReading();
    reading_ = false;
  } 
}

//
void TcpConnection::connectEstablished()
{
  loop_->assertInLoopThread();   //���Դ���loop�߳�
  assert(state_ == kConnecting);   //���Դ���δ����״̬
  setState(kConnected);   //��״̬����Ϊ������

  //֮ǰ���ü���Ϊ2
  channel_->tie(shared_from_this());   //���������TcpConnection��������������������ָ�룬���Բ���ֱ����this
  //shared_from_this()֮�����ü���+1��Ϊ3������shared_from_this()����ʱ�����������ֻ��һ��
  //��tie��weak_ptr������ı����ü��������Ըú���ִ����֮�����ü����������
  
  channel_->enableReading();   //һ�����ӳɹ��͹�ע���Ŀɶ��¼������뵽Poller�й�ע

  connectionCallback_(shared_from_this());
}

//�ú�������TcpConnection�Ͽ���Ž�I/O�¼�������У��ȴ�Functors����
void TcpConnection::connectDestroyed()
{
  loop_->assertInLoopThread();
  if (state_ == kConnected)     //��ɾ������
  {
    setState(kDisconnected);
    channel_->disableAll();

    connectionCallback_(shared_from_this());   //�ص��û�����
  }
  channel_->remove();  //��ͨ����Poller���Ƴ�
}

//���ص������������������ӶϿ�
void TcpConnection::handleRead(Timestamp receiveTime)
{
  loop_->assertInLoopThread();
  int savedErrno = 0;
  //��ȡ
  ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
  if (n > 0)
  {
    messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
  }
  else if (n == 0)  //�������0���������ӶϿ�
  {
    handleClose();
  }
  else
  {
    errno = savedErrno;
    LOG_SYSERR << "TcpConnection::handleRead";
    handleError();
  }
}

//�ں˻������пռ��ˣ��ص��ú���
void TcpConnection::handleWrite()
{
  loop_->assertInLoopThread();
  if (channel_->isWriting())  //��ע�˿�д�¼�
  {
    //��outputBuffer����������д��fd
    ssize_t n = sockets::write(channel_->fd(),
                               outputBuffer_.peek(),  //���ؿɶ��±�,
                               outputBuffer_.readableBytes());
    if (n > 0)
    {
      outputBuffer_.retrieve(n);  //д��n���ֽڣ��ƶ��±�n��
      if (outputBuffer_.readableBytes() == 0)  //���ȫ��д��
      {
        channel_->disableWriting();   //���ݷ�����ϣ�ֹͣ�ر�POLLOUT�¼�
        if (writeCompleteCallback_) //�ص�writeCompleteCallback
        {
          //����д�����writeCompleteCallback
          loop_->queueInLoop(boost::bind(writeCompleteCallback_, shared_from_this()));
        }
        if (state_ == kDisconnecting)  
        {
          shutdownInLoop();   //������Ϲر�����
        }
      }
    }
    else
    {
      LOG_SYSERR << "TcpConnection::handleWrite";
      // if (state_ == kDisconnecting)
      // {
      //   shutdownInLoop();
      // }
    }
  }
  else
  {
    LOG_TRACE << "Connection fd = " << channel_->fd()
              << " is down, no more writing";
  }
}

//�������ӶϿ�
void TcpConnection::handleClose()
{
  loop_->assertInLoopThread();
  LOG_TRACE << "fd = " << channel_->fd() << " state = " << stateToString();
  assert(state_ == kConnected || state_ == kDisconnecting);
  // we don't close fd, leave it to dtor, so we can find leaks easily.
  setState(kDisconnected);   //���öϿ�״̬
  channel_->disableAll();    //�ص����й�ע�¼�

  //�����������ľֲ�shared_ptr
  //���ü���+1������ʱ��-1
  TcpConnectionPtr guardThis(shared_from_this());
  
  connectionCallback_(guardThis);    
  // must be the last line
  closeCallback_(guardThis);  //����TcpServer��removeConnection����
}

void TcpConnection::handleError()
{
  int err = sockets::getSocketError(channel_->fd());
  LOG_ERROR << "TcpConnection::handleError [" << name_
            << "] - SO_ERROR = " << err << " " << strerror_tl(err);
}

