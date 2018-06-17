// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//

#include <muduo/net/http/HttpServer.h>

#include <muduo/base/Logging.h>
#include <muduo/net/http/HttpContext.h>
#include <muduo/net/http/HttpRequest.h>
#include <muduo/net/http/HttpResponse.h>

#include <boost/bind.hpp>

using namespace muduo;
using namespace muduo::net;

namespace muduo
{
namespace net
{
namespace detail
{

//默认http回调，设置close为true
void defaultHttpCallback(const HttpRequest&, HttpResponse* resp)
{
  resp->setStatusCode(HttpResponse::k404NotFound);
  resp->setStatusMessage("Not Found");
  resp->setCloseConnection(true);
}

}
}
}

HttpServer::HttpServer(EventLoop* loop,
                       const InetAddress& listenAddr,
                       const string& name,
                       TcpServer::Option option)
  : server_(loop, listenAddr, name, option),
    httpCallback_(detail::defaultHttpCallback)
{
  //连接到来回调该函数
  server_.setConnectionCallback(
      boost::bind(&HttpServer::onConnection, this, _1));  
  //消息到来回调该函数
  server_.setMessageCallback(
      boost::bind(&HttpServer::onMessage, this, _1, _2, _3));
}

HttpServer::~HttpServer()
{
}

//默认不是多线程，需要提前调用setThreadNum
void HttpServer::start()
{
  LOG_WARN << "HttpServer[" << server_.name()
    << "] starts listenning on " << server_.ipPort();
  server_.start();
}

//
void HttpServer::onConnection(const TcpConnectionPtr& conn)
{
  if (conn->connected())
  {
    //构造一个http上下文对象，用来解析http请求
    conn->setContext(HttpContext()); //TcpConnection和一个HttpContext绑定，利用boost::any
  }
}

void HttpServer::onMessage(const TcpConnectionPtr& conn,
                           Buffer* buf,
                           Timestamp receiveTime)
{
  //取出请求，mutable可以改变
  HttpContext* context = boost::any_cast<HttpContext>(conn->getMutableContext());

  //调用context的parseRequest解析请求，返回bool是否请求成功
  if (!context->parseRequest(buf, receiveTime))
  {
    conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");   //失败，发送400
    conn->shutdown();  //关闭连接
  }

  if (context->gotAll())  //请求成功
  {
    //调用onRequest
    onRequest(conn, context->request());
    //一旦请求处理完毕，重置context，因为HttpContext和TcpConnection绑定了，我们需要解绑重复使用。
    context->reset();
  }
}

//
void HttpServer::onRequest(const TcpConnectionPtr& conn, const HttpRequest& req)
{
  //取出头部
  const string& connection = req.getHeader("Connection");
  // 如果connection为close或者1.0版本不支持keep-alive，标志着我们处理完请求要关闭连接
  bool close = connection == "close" ||
    (req.getVersion() == HttpRequest::kHttp10 && connection != "Keep-Alive");
  //使用close构造一个HttpResponse对象，该对象可以通过方法.closeConnection()判断是否关闭连接
  HttpResponse response(close);
  //typedef boost::function<void (const HttpRequest&, HttpResponse*)> HttpCallback;
  //执行用户注册的回调函数
  httpCallback_(req, &response);
  Buffer buf;   //用户处理后的信息，追加到缓冲区
  response.appendToBuffer(&buf);
  conn->send(&buf);  //发送数据
  if (response.closeConnection())  //如果关闭
  {
    conn->shutdown();  //关了它
  }
}

