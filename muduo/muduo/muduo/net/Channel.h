// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_CHANNEL_H
#define MUDUO_NET_CHANNEL_H

#include <boost/function.hpp>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>

#include <muduo/base/Timestamp.h>

namespace muduo
{
namespace net
{

class EventLoop;

///
/// A selectable I/O channel.
///
/// This class doesn't own the file descriptor.
/// The file descriptor could be a socket,
/// an eventfd, a timerfd, or a signalfd
class Channel : boost::noncopyable
{
 public:
  typedef boost::function<void()> EventCallback;  //事件回调处理
  typedef boost::function<void(Timestamp)> ReadEventCallback;  //读事件的回调处理，传一个时间戳

  Channel(EventLoop* loop, int fd);  //一个Channel一个EventLoop
  ~Channel();

  void handleEvent(Timestamp receiveTime);  //处理事件
  void setReadCallback(const ReadEventCallback& cb)   //设置各种回调函数
  { readCallback_ = cb; }
  void setWriteCallback(const EventCallback& cb)
  { writeCallback_ = cb; }
  void setCloseCallback(const EventCallback& cb)
  { closeCallback_ = cb; }
  void setErrorCallback(const EventCallback& cb)
  { errorCallback_ = cb; }
#ifdef __GXX_EXPERIMENTAL_CXX0X__
  void setReadCallback(ReadEventCallback&& cb)
  { readCallback_ = std::move(cb); }
  void setWriteCallback(EventCallback&& cb)
  { writeCallback_ = std::move(cb); }
  void setCloseCallback(EventCallback&& cb)
  { closeCallback_ = std::move(cb); }
  void setErrorCallback(EventCallback&& cb)
  { errorCallback_ = std::move(cb); }
#endif

  /// Tie this channel to the owner object managed by shared_ptr,
  /// prevent the owner object being destroyed in handleEvent.
  void tie(const boost::shared_ptr<void>&); //与TcpConnection有关，防止事件被销毁。

  int fd() const { return fd_; }  //描述符
  int events() const { return events_; }   //注册的事件
  void set_revents(int revt) { revents_ = revt; } // used by pollers
  // int revents() const { return revents_; }
  bool isNoneEvent() const { return events_ == kNoneEvent; }  //判断是否无关注事件类型，events为0

  void enableReading() { events_ |= kReadEvent; update(); }   //或上事件，就是关注可读事件，注册到EventLoop，通过它注册到Poller中
  void disableReading() { events_ &= ~kReadEvent; update(); }  //取消关注
  void enableWriting() { events_ |= kWriteEvent; update(); }   //关注可写事件
  void disableWriting() { events_ &= ~kWriteEvent; update(); }  //关闭
  void disableAll() { events_ = kNoneEvent; update(); }  //全部关闭
  bool isWriting() const { return events_ & kWriteEvent; }  //是否关注了写
  bool isReading() const { return events_ & kReadEvent; }  //是否关注读

  // for Poller
  int index() { return index_; }   //pollfd数组中的下标
  void set_index(int idx) { index_ = idx; }

  // for debug
  string reventsToString() const;  //事件转化为字符串，方便打印调试
  string eventsToString() const;  //同理

  void doNotLogHup() { logHup_ = false; }

  EventLoop* ownerLoop() { return loop_; }
  void remove();  //移除，确保调用前调用disableall。

 private:
  static string eventsToString(int fd, int ev);

  void update();
  void handleEventWithGuard(Timestamp receiveTime);

  static const int kNoneEvent;   //三个常量，没有事件，static常量类外部有定义，在.cc文件中
  static const int kReadEvent;   //可读事件
  static const int kWriteEvent;  //可写事件

  EventLoop* loop_;   //记录所属EventLoop
  const int  fd_;         //文件描述符，但不负责关闭改描述符
  int        events_;   //关注的时间类型
  int        revents_; // it's the received event types of epoll or poll
  int        index_; // used by Poller.，表示在Poller事件数组中的序号
  bool       logHup_;  //for POLLHUP

  boost::weak_ptr<void> tie_;  //负责生存期控制
  bool tied_;
  bool eventHandling_;         //是否处于处理事件中
  bool addedToLoop_;
  ReadEventCallback readCallback_;   //几种事件处理回调
  EventCallback writeCallback_;
  EventCallback closeCallback_;
  EventCallback errorCallback_;
};

}
}
#endif  // MUDUO_NET_CHANNEL_H
