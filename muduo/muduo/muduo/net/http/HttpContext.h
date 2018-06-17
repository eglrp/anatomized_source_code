// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_HTTP_HTTPCONTEXT_H
#define MUDUO_NET_HTTP_HTTPCONTEXT_H

#include <muduo/base/copyable.h>

#include <muduo/net/http/HttpRequest.h>

namespace muduo
{
namespace net
{

class Buffer;

class HttpContext : public muduo::copyable
{
 public:
  //��������״̬
  enum HttpRequestParseState
  {
    kExpectRequestLine,   //��ǰ�����ڽ��������е�״̬
    kExpectHeaders,          //��ǰ�����ڽ�������ͷ����״̬
    kExpectBody,               //��ǰ�����ڽ�������ʵ���״̬
    kGotAll,                       //�������
  };

  HttpContext()
    : state_(kExpectRequestLine)  //��ʼ״̬�������յ�һ��������
  {
  }

  // default copy-ctor, dtor and assignment are fine

  // return false if any error
  bool parseRequest(Buffer* buf, Timestamp receiveTime);
  
  bool gotAll() const
  { return state_ == kGotAll; }

  //����HttpContext״̬���쳣��ȫ
  void reset()
  {
    state_ = kExpectRequestLine;
    HttpRequest dummy;   //����һ����ʱ��HttpRequest���󣬺͵�ǰ�ĳ�ԱHttpRequest���󽻻��ÿգ�Ȼ����ʱ��������
    request_.swap(dummy);
  }

  const HttpRequest& request() const
  { return request_; }

  HttpRequest& request()
  { return request_; }

 private:
  bool processRequestLine(const char* begin, const char* end);

  HttpRequestParseState state_;   //����Ľ���״̬
  HttpRequest request_;    //http������
};

}
}

#endif  // MUDUO_NET_HTTP_HTTPCONTEXT_H
