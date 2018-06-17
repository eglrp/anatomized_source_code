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
  //解析请求状态
  enum HttpRequestParseState
  {
    kExpectRequestLine,   //当前正处于解析请求行的状态
    kExpectHeaders,          //当前正处于解析请求头部的状态
    kExpectBody,               //当前正处于解析请求实体的状态
    kGotAll,                       //解析完毕
  };

  HttpContext()
    : state_(kExpectRequestLine)  //初始状态，期望收到一个请求行
  {
  }

  // default copy-ctor, dtor and assignment are fine

  // return false if any error
  bool parseRequest(Buffer* buf, Timestamp receiveTime);
  
  bool gotAll() const
  { return state_ == kGotAll; }

  //重置HttpContext状态，异常安全
  void reset()
  {
    state_ = kExpectRequestLine;
    HttpRequest dummy;   //构造一个临时空HttpRequest对象，和当前的成员HttpRequest对象交换置空，然后临时对象析构
    request_.swap(dummy);
  }

  const HttpRequest& request() const
  { return request_; }

  HttpRequest& request()
  { return request_; }

 private:
  bool processRequestLine(const char* begin, const char* end);

  HttpRequestParseState state_;   //请求的解析状态
  HttpRequest request_;    //http请求类
};

}
}

#endif  // MUDUO_NET_HTTP_HTTPCONTEXT_H
