// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is a public header file, it must only include public header files.

#ifndef MUDUO_NET_HTTP_HTTPREQUEST_H
#define MUDUO_NET_HTTP_HTTPREQUEST_H

#include <muduo/base/copyable.h>
#include <muduo/base/Timestamp.h>
#include <muduo/base/Types.h>

#include <map>
#include <assert.h>
#include <stdio.h>

namespace muduo
{
namespace net
{

class HttpRequest : public muduo::copyable
{
 public:
  enum Method  //请求方法
  {
    kInvalid, kGet, kPost, kHead, kPut, kDelete
  };
  enum Version  //协议版本
  {
    kUnknown, kHttp10, kHttp11
  };

  HttpRequest()  
    : method_(kInvalid),
      version_(kUnknown)
  {
  }

  //设置版本
  void setVersion(Version v)
   {
    version_ = v;
  }

  Version getVersion() const
  { return version_; }

  //设置方法，
  bool setMethod(const char* start, const char* end)
  {
    assert(method_ == kInvalid);
    //使用字符串首尾构造string，不包括尾部，如char *s="123", string s=(s,s+3),则s输出为123
    string m(start, end);
    if (m == "GET")
    {
      method_ = kGet;
    }
    else if (m == "POST")
    {
      method_ = kPost;
    }
    else if (m == "HEAD")
    {
      method_ = kHead;
    }
    else if (m == "PUT")
    {
      method_ = kPut;
    }
    else if (m == "DELETE")
    {
      method_ = kDelete;
    }
    else
    {
      method_ = kInvalid;
    }
    return method_ != kInvalid;
  }

  //返回请求方法
  Method method() const
  { return method_; }

  //请求方法转换成字符串
  const char* methodString() const
  {
    const char* result = "UNKNOWN";
    switch(method_)
    {
      case kGet:
        result = "GET";
        break;
      case kPost:
        result = "POST";
        break;
      case kHead:
        result = "HEAD";
        break;
      case kPut:
        result = "PUT";
        break;
      case kDelete:
        result = "DELETE";
        break;
      default:
        break;
    }
    return result;
  }

  //设置路径
  void setPath(const char* start, const char* end)
  {
    path_.assign(start, end);
  }

  const string& path() const
  { return path_; }

  //
  void setQuery(const char* start, const char* end)
  {
    query_.assign(start, end);
  }

  const string& query() const
  { return query_; }

  //设置接收时间
  void setReceiveTime(Timestamp t)
  { receiveTime_ = t; }

  Timestamp receiveTime() const
  { return receiveTime_; }

  //添加头部信息，客户传来一个字符串，我们把它转化成field: value的形式
  void addHeader(const char* start, const char* colon, const char* end)
  {
    string field(start, colon);  //header域
    ++colon;
    //去除左空格
    while (colon < end && isspace(*colon))
    {
      ++colon;
    }
    string value(colon, end);   //heade值
    //去除右空格，如果右边有空格会一直resize-1
    while (!value.empty() && isspace(value[value.size()-1]))
    {
      value.resize(value.size()-1);
    }
    //std::map<string, string> headers_;  
    headers_[field] = value;
  }

  //根据头域返回值
  string getHeader(const string& field) const
  {
    string result;
    std::map<string, string>::const_iterator it = headers_.find(field);
    if (it != headers_.end())
    {
      result = it->second;
    }
    return result;
  }

  //返回头部
  const std::map<string, string>& headers() const
  { return headers_; }

  //交换
  void swap(HttpRequest& that)
  {
    std::swap(method_, that.method_);
    path_.swap(that.path_);
    query_.swap(that.query_);
    receiveTime_.swap(that.receiveTime_);
    headers_.swap(that.headers_);
  }

 private:
  Method method_;      //请求方法
  Version version_;      //请求版本
  string path_;             //请求路径
  string query_;           //请求参数
  Timestamp receiveTime_;    //请求接收时间
  std::map<string, string> headers_;    //header列表
};

}
}

#endif  // MUDUO_NET_HTTP_HTTPREQUEST_H
