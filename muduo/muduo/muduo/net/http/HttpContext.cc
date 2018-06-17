// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//

#include <muduo/net/Buffer.h>
#include <muduo/net/http/HttpContext.h>

using namespace muduo;
using namespace muduo::net;

//处理请求行
bool HttpContext::processRequestLine(const char* begin, const char* end)
{
  bool succeed = false;
  const char* start = begin;
  const char* space = std::find(start, end, ' ');   //查找空格
  //格式 : GET / HTTP/1.1
  //找到GET并设置请求方法
  if (space != end && request_.setMethod(start, space))
  {
    start = space+1;
    space = std::find(start, end, ' ');  //再次查找
    if (space != end)  //找到
    {
      const char* question = std::find(start, space, '?');
      if (question != space)  //找到了'?'，说明有请求参数
      {
        //设置路径
        request_.setPath(start, question);
        //设置请求参数
        request_.setQuery(question, space);
      }
      else
      {
        //没有找到只设置路径
        request_.setPath(start, space);
      }
      start = space+1;

      //查找有没有"HTTP/1."
      succeed = end-start == 8 && std::equal(start, end-1, "HTTP/1.");
      if (succeed)  //如果成功，判断是采用HTTP/1.1还是HTTP/1.0
      {
        if (*(end-1) == '1')
        {
          request_.setVersion(HttpRequest::kHttp11);
        }
        else if (*(end-1) == '0')
        {
          request_.setVersion(HttpRequest::kHttp10);
        }
        else
        {
          succeed = false;  //请求行失败
        }
      }
    }
  }
  return succeed;
}

//处理http请求，状态机
// return false if any error
bool HttpContext::parseRequest(Buffer* buf, Timestamp receiveTime)
{
  bool ok = true;
  bool hasMore = true;
  while (hasMore)
  {
    //初始状态是处于解析请求行的状态
    if (state_ == kExpectRequestLine)
    {
      //首先查找\r\n，就会到GET / HTTP/1.1的请求行末尾
      const char* crlf = buf->findCRLF();
      if (crlf)
      {
        //解析请求行
        ok = processRequestLine(buf->peek(), crlf);
        if (ok)  //如果成功，设置请求行事件
        {
          //设置请求时间
          request_.setReceiveTime(receiveTime);
          //将请求行从buf中取回，包括\r\n
          buf->retrieveUntil(crlf + 2);
          state_ = kExpectHeaders;  //将Httpontext状态改为KexpectHeaders状态
        }
        else
        {
          hasMore = false;
        }
      }
      else
      {
        hasMore = false;
      }
    }
    else if (state_ == kExpectHeaders)  //处于Header状态
    {
      const char* crlf = buf->findCRLF();
      if (crlf)
      {
        const char* colon = std::find(buf->peek(), crlf, ':');  //查找:
        if (colon != crlf)
        {
          //找到添加头部，加到map容器
          request_.addHeader(buf->peek(), colon, crlf);
        }
        else
        {
          // empty line, end of header
          // FIXME:
          state_ = kGotAll;  //一旦请求完毕，再也找不到':'了，状态改为gotall状态，循环退出
          hasMore = false;
        }
        buf->retrieveUntil(crlf + 2);  //请求完毕也把crlf取回
      }
      else
      {
        hasMore = false;
      }
    }
    else if (state_ == kExpectBody)
    {
      // FIXME:
    }
  }
  return ok;
}
