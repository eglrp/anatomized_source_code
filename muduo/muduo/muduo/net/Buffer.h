// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is a public header file, it must only include public header files.

#ifndef MUDUO_NET_BUFFER_H
#define MUDUO_NET_BUFFER_H

#include <muduo/base/copyable.h>
#include <muduo/base/StringPiece.h>
#include <muduo/base/Types.h>

#include <muduo/net/Endian.h>

#include <algorithm>
#include <vector>

#include <assert.h>
#include <string.h>
//#include <unistd.h>  // ssize_t

namespace muduo
{
namespace net
{

/// A buffer class modeled after org.jboss.netty.buffer.ChannelBuffer
///
/// @code
/// +-------------------+------------------+------------------+
/// | prependable bytes |  readable bytes  |  writable bytes  |
/// |                   |     (CONTENT)    |                  |
/// +-------------------+------------------+------------------+
/// |                   |                  |                  |
/// 0      <=      readerIndex   <=   writerIndex    <=     size
/// @endcode
class Buffer : public muduo::copyable
{
 public:
  static const size_t kCheapPrepend = 8;    //Ĭ��Ԥ��8���ֽ�
  static const size_t kInitialSize = 1024;   //��ʼ��С

  explicit Buffer(size_t initialSize = kInitialSize)
    : buffer_(kCheapPrepend + initialSize),   //�ܴ�СΪ1032
      readerIndex_(kCheapPrepend),   //��ʼָ��8
      writerIndex_(kCheapPrepend)    //��ʼָ��8
  {
    assert(readableBytes() == 0);
    assert(writableBytes() == initialSize);
    assert(prependableBytes() == kCheapPrepend);
  }

  // implicit copy-ctor, move-ctor, dtor and assignment are fine
  // NOTE: implicit move-ctor is added in g++ 4.6

   //��������������
  void swap(Buffer& rhs)
  {
    buffer_.swap(rhs.buffer_);
    std::swap(readerIndex_, rhs.readerIndex_);
    std::swap(writerIndex_, rhs.writerIndex_);
  }

  //�ɶ���С
  size_t readableBytes() const
  { return writerIndex_ - readerIndex_; }

   //��д��С
  size_t writableBytes() const
  { return buffer_.size() - writerIndex_; }

  //Ԥ����С
  size_t prependableBytes() const
  { return readerIndex_; }

  //�����±�
  const char* peek() const
  { return begin() + readerIndex_; }

  //����\r\n
  const char* findCRLF() const
  {
    //const char Buffer::kCRLF[] = "\r\n";
    // FIXME: replace with memmem()?
    const char* crlf = std::search(peek(), beginWrite(), kCRLF, kCRLF+2);
    return crlf == beginWrite() ? NULL : crlf;  //�����дλ�û�û�ҵ�������NULL
  }

   //��startλ�ò���/r/n
  const char* findCRLF(const char* start) const
  {
    assert(peek() <= start);
    assert(start <= beginWrite());
    // FIXME: replace with memmem()?
    const char* crlf = std::search(start, beginWrite(), kCRLF, kCRLF+2);
    return crlf == beginWrite() ? NULL : crlf;
  }

  const char* findEOL() const
  {
    const void* eol = memchr(peek(), '\n', readableBytes());
    return static_cast<const char*>(eol);
  }

  const char* findEOL(const char* start) const
  {
    assert(peek() <= start);
    assert(start <= beginWrite());
    const void* eol = memchr(start, '\n', beginWrite() - start);
    return static_cast<const char*>(eol);
  }

  // retrieve returns void, to prevent
  // string str(retrieve(readableBytes()), readableBytes());
  // the evaluation of two functions are unspecified
  //ȡ��len���ȵ�����
  void retrieve(size_t len)
  {
    assert(len <= readableBytes());
    if (len < readableBytes())  //���Ҫȡ�صĴ���ʵ�ʿɶ��ĳ��ȣ�ִ��else��ȫ��ȡ��
    {
      readerIndex_ += len;
    }
    else
    {
      retrieveAll();
    }
  }

  //ȡ��ֱ��ĳ��λ��
  void retrieveUntil(const char* end)
  {
    assert(peek() <= end);
    assert(end <= beginWrite());
    retrieve(end - peek());
  }

  //ȡ8���ֽ�
  void retrieveInt64()
  {
    retrieve(sizeof(int64_t));
  }

  //ȡ4���ֽڣ������ͬ
  void retrieveInt32()
  {
    retrieve(sizeof(int32_t));
  }

  void retrieveInt16()
  {
    retrieve(sizeof(int16_t));
  }

  void retrieveInt8()
  {
    retrieve(sizeof(int8_t));
  }

  //ȡ��ȫ��
  void retrieveAll()
  {
    readerIndex_ = kCheapPrepend;
    writerIndex_ = kCheapPrepend;
  }

  //�����е�����ȡ�ز��ҷ����ַ���
  string retrieveAllAsString()
  {
    return retrieveAsString(readableBytes());;
  }

  //ȡ�س���Ϊlen���ַ����������string
  string retrieveAsString(size_t len)
  {
    assert(len <= readableBytes());
    string result(peek(), len);
    retrieve(len);
    return result;
  }

  //��readbale��������ת����stringPiece
  StringPiece toStringPiece() const
  {
    return StringPiece(peek(), static_cast<int>(readableBytes()));
  }

  //�������
  void append(const StringPiece& str)
  {
    append(str.data(), str.size());
  }

  //ʵ�ʵ��õ�append����
  void append(const char* /*restrict*/ data, size_t len)
  {
    ensureWritableBytes(len);   //ȷ����������д�ռ���ڵ���len��������㣬��Ҫ����
    std::copy(data, data+len, beginWrite());   //copy֮ǰ���Ѵ��ڵ�readable����
    hasWritten(len);  //д������writeindex
  }
  
  void append(const void* /*restrict*/ data, size_t len)
  {
    append(static_cast<const char*>(data), len);
  }

  void ensureWritableBytes(size_t len)
  {
    if (writableBytes() < len)  //�����д����С��len
    { 
      makeSpace(len);   //���ӿռ�
    }
    assert(writableBytes() >= len);
  }

  char* beginWrite()
  { return begin() + writerIndex_; }

  const char* beginWrite() const
  { return begin() + writerIndex_; }

  //д������writeindex_
  void hasWritten(size_t len)
  {
    assert(len <= writableBytes());
    writerIndex_ += len;
  }

  void unwrite(size_t len)
  {
    assert(len <= readableBytes());
    writerIndex_ -= len;
  }

  //������׷�Ӹ��ִ�С�ֽڵ����ݣ�ת�����������ֽ���
  ///
  /// Append int64_t using network endian
  ///
  void appendInt64(int64_t x)
  {
    int64_t be64 = sockets::hostToNetwork64(x);
    append(&be64, sizeof be64);
  }

  ///
  /// Append int32_t using network endian
  ///
  void appendInt32(int32_t x)
  {
    int32_t be32 = sockets::hostToNetwork32(x);
    append(&be32, sizeof be32);
  }

  void appendInt16(int16_t x)
  {
    int16_t be16 = sockets::hostToNetwork16(x);
    append(&be16, sizeof be16);
  }

  void appendInt8(int8_t x)
  {
    append(&x, sizeof x);
  }

  ///
  /// Read int64_t from network endian
  ///
  /// Require: buf->readableBytes() >= sizeof(int32_t)
  int64_t readInt64()
  {
    int64_t result = peekInt64();
    retrieveInt64();
    return result;
  }

  ///
  //����λ�õ���
  /// Read int32_t from network endian
  ///
  /// Require: buf->readableBytes() >= sizeof(int32_t)
  int32_t readInt32()
  {
    int32_t result = peekInt32();
    retrieveInt32();
    return result;
  }

  int16_t readInt16()
  {
    int16_t result = peekInt16();
    retrieveInt16();
    return result;
  }

  int8_t readInt8()
  {
    int8_t result = peekInt8();
    retrieveInt8();
    return result;
  }

  //�������ִ�С�ֽڳ���������ת���������ֽ��򷵻�
  ///
  /// Peek int64_t from network endian
  ///
  /// Require: buf->readableBytes() >= sizeof(int64_t)
  int64_t peekInt64() const
  {
    assert(readableBytes() >= sizeof(int64_t));
    int64_t be64 = 0;
    ::memcpy(&be64, peek(), sizeof be64);
    return sockets::networkToHost64(be64);
  }

  ///
  /// Peek int32_t from network endian
  ///
  /// Require: buf->readableBytes() >= sizeof(int32_t)
  int32_t peekInt32() const
  {
    assert(readableBytes() >= sizeof(int32_t));
    int32_t be32 = 0;
    ::memcpy(&be32, peek(), sizeof be32);
    return sockets::networkToHost32(be32);
  }

  int16_t peekInt16() const
  {
    assert(readableBytes() >= sizeof(int16_t));
    int16_t be16 = 0;
    ::memcpy(&be16, peek(), sizeof be16);
    return sockets::networkToHost16(be16);
  }

  int8_t peekInt8() const
  {
    assert(readableBytes() >= sizeof(int8_t));
    int8_t x = *peek();
    return x;
  }

  //���涼����prepend��Ӹ��ִ�С�ֽڵ�����
  ///
  /// Prepend int64_t using network endian
  ///
  void prependInt64(int64_t x)
  {
    int64_t be64 = sockets::hostToNetwork64(x);
    prepend(&be64, sizeof be64);
  }

  ///
  /// Prepend int32_t using network endian
  ///
  void prependInt32(int32_t x)
  {
    int32_t be32 = sockets::hostToNetwork32(x);
    prepend(&be32, sizeof be32);
  }

  void prependInt16(int16_t x)
  {
    int16_t be16 = sockets::hostToNetwork16(x);
    prepend(&be16, sizeof be16);
  }

  void prependInt8(int8_t x)
  {
    prepend(&x, sizeof x);
  }

  //��������prependInt*����ʵ�ʵ��õĺ�����������ݵ�prepend����
  void prepend(const void* /*restrict*/ data, size_t len)��
  {
    assert(len <= prependableBytes());
    readerIndex_ -= len;
    const char* d = static_cast<const char*>(data);
    std::copy(d, d+len, begin()+readerIndex_);
  }

  //�����ռ䣬����reserver���ֽڣ����ܶ�ζ�д��buffer̫���ˣ���������
  void shrink(size_t reserve)
  {
    // FIXME: use vector::shrink_to_fit() in C++ 11 if possible.
    //ΪʲôҪʹ��Buffer���͵�other�������ռ���?����������ַ�ʽ�����ǿ�ѡ����ʹ��resize()����������resize()z��
    Buffer other;  //������ʱ���񣬱���readable���ݣ�Ȼ���������������ʱ������������
    //ensureWritableBytes()�������������ܣ�һ���ǿռ䲻��resize�ռ䣬һ���ǿռ��㹻�ڲ���Ų�����������õ��Ǻ��ߡ�
    other.ensureWritableBytes(readableBytes()+reserve);  //ȷ�����㹻�Ŀռ䣬�ڲ���ʱ�Ѿ���Ų
    other.append(toStringPiece());   //�ѵ�ǰ������׷�ӵ�other���棬Ȼ���ٽ�����
    swap(other);   //Ȼ���ٽ���
  }

  //
  size_t internalCapacity() const
  {
    return buffer_.capacity();
  }

  /// Read data directly into buffer.
  ///
  /// It may implement with readv(2)
  /// @return result of read(2), @c errno is saved
  //���׽��ֶ�ȡ����
  ssize_t readFd(int fd, int* savedErrno);

 private:

  char* begin()
  { return &*buffer_.begin(); }

  const char* begin() const
  { return &*buffer_.begin(); }

  void makeSpace(size_t len)  //vector���ӿռ�
  {
    if (writableBytes() + prependableBytes() < len + kCheapPrepend)  //ȷ���ռ�����Ĳ�����������Ų���Ϳ����ڳ��ռ�
    {
      // FIXME: move readable data
      buffer_.resize(writerIndex_+len);
    }
    else
    {
      //�ڲ���Ų���㹻append����ô���ڲ���Ųһ�¡�
      // move readable data to the front, make space inside buffer
      assert(kCheapPrepend < readerIndex_);
      size_t readable = readableBytes();
      std::copy(begin()+readerIndex_,    //ԭ���Ŀɶ�����ȫ��copy��Prependλ�ã��൱����ǰŲ����Ϊwriteable�����ռ�
                begin()+writerIndex_,
                begin()+kCheapPrepend);
      readerIndex_ = kCheapPrepend;   //�����±�
      writerIndex_ = readerIndex_ + readable;
      assert(readable == readableBytes());
    }
  }

 private:
  std::vector<char> buffer_;    //vector��������̶�����
  size_t readerIndex_;            //��λ��
  size_t writerIndex_;             //дλ��

  //const char Buffer::kCRLF[] = "\r\n";
  static const char kCRLF[];      //'\r\n'��ʹ����������
};

}
}

#endif  // MUDUO_NET_BUFFER_H
