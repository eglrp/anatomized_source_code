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

//Ĭ��http�ص�������closeΪtrue
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
  //���ӵ����ص��ú���
  server_.setConnectionCallback(
      boost::bind(&HttpServer::onConnection, this, _1));  
  //��Ϣ�����ص��ú���
  server_.setMessageCallback(
      boost::bind(&HttpServer::onMessage, this, _1, _2, _3));
}

HttpServer::~HttpServer()
{
}

//Ĭ�ϲ��Ƕ��̣߳���Ҫ��ǰ����setThreadNum
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
    //����һ��http�����Ķ�����������http����
    conn->setContext(HttpContext()); //TcpConnection��һ��HttpContext�󶨣�����boost::any
  }
}

void HttpServer::onMessage(const TcpConnectionPtr& conn,
                           Buffer* buf,
                           Timestamp receiveTime)
{
  //ȡ������mutable���Ըı�
  HttpContext* context = boost::any_cast<HttpContext>(conn->getMutableContext());

  //����context��parseRequest�������󣬷���bool�Ƿ�����ɹ�
  if (!context->parseRequest(buf, receiveTime))
  {
    conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");   //ʧ�ܣ�����400
    conn->shutdown();  //�ر�����
  }

  if (context->gotAll())  //����ɹ�
  {
    //����onRequest
    onRequest(conn, context->request());
    //һ����������ϣ�����context����ΪHttpContext��TcpConnection���ˣ�������Ҫ����ظ�ʹ�á�
    context->reset();
  }
}

//
void HttpServer::onRequest(const TcpConnectionPtr& conn, const HttpRequest& req)
{
  //ȡ��ͷ��
  const string& connection = req.getHeader("Connection");
  // ���connectionΪclose����1.0�汾��֧��keep-alive����־�����Ǵ���������Ҫ�ر�����
  bool close = connection == "close" ||
    (req.getVersion() == HttpRequest::kHttp10 && connection != "Keep-Alive");
  //ʹ��close����һ��HttpResponse���󣬸ö������ͨ������.closeConnection()�ж��Ƿ�ر�����
  HttpResponse response(close);
  //typedef boost::function<void (const HttpRequest&, HttpResponse*)> HttpCallback;
  //ִ���û�ע��Ļص�����
  httpCallback_(req, &response);
  Buffer buf;   //�û���������Ϣ��׷�ӵ�������
  response.appendToBuffer(&buf);
  conn->send(&buf);  //��������
  if (response.closeConnection())  //����ر�
  {
    conn->shutdown();  //������
  }
}

