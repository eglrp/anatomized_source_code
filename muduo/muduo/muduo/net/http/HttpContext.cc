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

//����������
bool HttpContext::processRequestLine(const char* begin, const char* end)
{
  bool succeed = false;
  const char* start = begin;
  const char* space = std::find(start, end, ' ');   //���ҿո�
  //��ʽ : GET / HTTP/1.1
  //�ҵ�GET���������󷽷�
  if (space != end && request_.setMethod(start, space))
  {
    start = space+1;
    space = std::find(start, end, ' ');  //�ٴβ���
    if (space != end)  //�ҵ�
    {
      const char* question = std::find(start, space, '?');
      if (question != space)  //�ҵ���'?'��˵�����������
      {
        //����·��
        request_.setPath(start, question);
        //�����������
        request_.setQuery(question, space);
      }
      else
      {
        //û���ҵ�ֻ����·��
        request_.setPath(start, space);
      }
      start = space+1;

      //������û��"HTTP/1."
      succeed = end-start == 8 && std::equal(start, end-1, "HTTP/1.");
      if (succeed)  //����ɹ����ж��ǲ���HTTP/1.1����HTTP/1.0
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
          succeed = false;  //������ʧ��
        }
      }
    }
  }
  return succeed;
}

//����http����״̬��
// return false if any error
bool HttpContext::parseRequest(Buffer* buf, Timestamp receiveTime)
{
  bool ok = true;
  bool hasMore = true;
  while (hasMore)
  {
    //��ʼ״̬�Ǵ��ڽ��������е�״̬
    if (state_ == kExpectRequestLine)
    {
      //���Ȳ���\r\n���ͻᵽGET / HTTP/1.1��������ĩβ
      const char* crlf = buf->findCRLF();
      if (crlf)
      {
        //����������
        ok = processRequestLine(buf->peek(), crlf);
        if (ok)  //����ɹ��������������¼�
        {
          //��������ʱ��
          request_.setReceiveTime(receiveTime);
          //�������д�buf��ȡ�أ�����\r\n
          buf->retrieveUntil(crlf + 2);
          state_ = kExpectHeaders;  //��Httpontext״̬��ΪKexpectHeaders״̬
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
    else if (state_ == kExpectHeaders)  //����Header״̬
    {
      const char* crlf = buf->findCRLF();
      if (crlf)
      {
        const char* colon = std::find(buf->peek(), crlf, ':');  //����:
        if (colon != crlf)
        {
          //�ҵ����ͷ�����ӵ�map����
          request_.addHeader(buf->peek(), colon, crlf);
        }
        else
        {
          // empty line, end of header
          // FIXME:
          state_ = kGotAll;  //һ��������ϣ���Ҳ�Ҳ���':'�ˣ�״̬��Ϊgotall״̬��ѭ���˳�
          hasMore = false;
        }
        buf->retrieveUntil(crlf + 2);  //�������Ҳ��crlfȡ��
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
