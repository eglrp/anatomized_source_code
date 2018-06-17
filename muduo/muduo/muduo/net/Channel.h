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
  typedef boost::function<void()> EventCallback;  //�¼��ص�����
  typedef boost::function<void(Timestamp)> ReadEventCallback;  //���¼��Ļص�������һ��ʱ���

  Channel(EventLoop* loop, int fd);  //һ��Channelһ��EventLoop
  ~Channel();

  void handleEvent(Timestamp receiveTime);  //�����¼�
  void setReadCallback(const ReadEventCallback& cb)   //���ø��ֻص�����
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
  void tie(const boost::shared_ptr<void>&); //��TcpConnection�йأ���ֹ�¼������١�

  int fd() const { return fd_; }  //������
  int events() const { return events_; }   //ע����¼�
  void set_revents(int revt) { revents_ = revt; } // used by pollers
  // int revents() const { return revents_; }
  bool isNoneEvent() const { return events_ == kNoneEvent; }  //�ж��Ƿ��޹�ע�¼����ͣ�eventsΪ0

  void enableReading() { events_ |= kReadEvent; update(); }   //�����¼������ǹ�ע�ɶ��¼���ע�ᵽEventLoop��ͨ����ע�ᵽPoller��
  void disableReading() { events_ &= ~kReadEvent; update(); }  //ȡ����ע
  void enableWriting() { events_ |= kWriteEvent; update(); }   //��ע��д�¼�
  void disableWriting() { events_ &= ~kWriteEvent; update(); }  //�ر�
  void disableAll() { events_ = kNoneEvent; update(); }  //ȫ���ر�
  bool isWriting() const { return events_ & kWriteEvent; }  //�Ƿ��ע��д
  bool isReading() const { return events_ & kReadEvent; }  //�Ƿ��ע��

  // for Poller
  int index() { return index_; }   //pollfd�����е��±�
  void set_index(int idx) { index_ = idx; }

  // for debug
  string reventsToString() const;  //�¼�ת��Ϊ�ַ����������ӡ����
  string eventsToString() const;  //ͬ��

  void doNotLogHup() { logHup_ = false; }

  EventLoop* ownerLoop() { return loop_; }
  void remove();  //�Ƴ���ȷ������ǰ����disableall��

 private:
  static string eventsToString(int fd, int ev);

  void update();
  void handleEventWithGuard(Timestamp receiveTime);

  static const int kNoneEvent;   //����������û���¼���static�������ⲿ�ж��壬��.cc�ļ���
  static const int kReadEvent;   //�ɶ��¼�
  static const int kWriteEvent;  //��д�¼�

  EventLoop* loop_;   //��¼����EventLoop
  const int  fd_;         //�ļ�����������������رո�������
  int        events_;   //��ע��ʱ������
  int        revents_; // it's the received event types of epoll or poll
  int        index_; // used by Poller.����ʾ��Poller�¼������е����
  bool       logHup_;  //for POLLHUP

  boost::weak_ptr<void> tie_;  //���������ڿ���
  bool tied_;
  bool eventHandling_;         //�Ƿ��ڴ����¼���
  bool addedToLoop_;
  ReadEventCallback readCallback_;   //�����¼�����ص�
  EventCallback writeCallback_;
  EventCallback closeCallback_;
  EventCallback errorCallback_;
};

}
}
#endif  // MUDUO_NET_CHANNEL_H
