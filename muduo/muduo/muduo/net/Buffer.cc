// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//

#include <muduo/net/Buffer.h>

#include <muduo/net/SocketsOps.h>

#include <errno.h>
#include <sys/uio.h>

using namespace muduo;
using namespace muduo::net;

const char Buffer::kCRLF[] = "\r\n";

const size_t Buffer::kCheapPrepend;
const size_t Buffer::kInitialSize;

//���ջ�Ͽռ䣬�����ڴ�ʹ�ù�������ڴ�ʹ����
//�����10K�����ӣ�ÿ�����Ӿͷ���64K�������Ļ�����ռ��640M�ڴ�
//�������ʱ����Щ��������ʹ���ʺܵ�
ssize_t Buffer::readFd(int fd, int* savedErrno)
{
  // saved an ioctl()/FIONREAD call to tell how much to read
  //��ʡһ��ioctlϵͳ����(��ȡ��ǰ�ж��ٿɶ����ݣ�
  //Ϊʲô��ô˵?��Ϊ����׼�����㹻���extrabuf����ô���ǾͲ���Ҫʹ��ioctlȡ�鿴fd�ж��ٿɶ��ֽ�����
  char extrabuf[65536];

  //ʹ��iovec�������������Ļ�����
  struct iovec vec[2];
  const size_t writable = writableBytes();
  //��һ�黺������ָ���д�ռ�
  vec[0].iov_base = begin()+writerIndex_;
  vec[0].iov_len = writable;
  //�ڶ��黺������ָ��ջ�Ͽռ�
  vec[1].iov_base = extrabuf;
  vec[1].iov_len = sizeof extrabuf;
  
  // when there is enough space in this buffer, don't read into extrabuf.
  // when extrabuf is used, we read 128k-1 bytes at most.
  const int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;  //writeableһ��С��65536
  const ssize_t n = sockets::readv(fd, vec, iovcnt);  //iovcnt=2
  if (n < 0)
  {
    *savedErrno = errno;
  }
  else if (implicit_cast<size_t>(n) <= writable)   //��һ�黺�����㹻����
  {
    writerIndex_ += n;   //ֱ�Ӽ�n
  }
  else   //��ǰ���������������ɣ�������ݱ����ܵ��˵ڶ��黺����extrabuf������append��buffer
  {
    writerIndex_ = buffer_.size();   //�ȸ��Ե�ǰwriterIndex
    append(extrabuf, n - writable);   //Ȼ��׷��ʣ����ٽ���buffer����
  }
  // if (n == writable + sizeof extrabuf)
  // {
  //   goto line_30;
  // }
  return n;
}

