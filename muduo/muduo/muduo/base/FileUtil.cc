// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//

#include <muduo/base/FileUtil.h>
#include <muduo/base/Logging.h> // strerror_tl

#include <boost/static_assert.hpp>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

using namespace muduo;

//�����̰߳�ȫ��
FileUtil::AppendFile::AppendFile(StringArg filename)
  : fp_(::fopen(filename.c_str(), "ae")),  // 'e' for O_CLOEXEC
    writtenBytes_(0)  //�Ѿ�д����ֽ���
{
  assert(fp_);
  ::setbuffer(fp_, buffer_, sizeof buffer_);  //�ļ�ָ��Ļ������趨64k
  // posix_fadvise POSIX_FADV_DONTNEED ?
}

FileUtil::AppendFile::~AppendFile()
{
  ::fclose(fp_);  //�ر��ļ�ָ��
}

//�����̰߳�ȫ�ģ���Ҫ�ⲿ����
void FileUtil::AppendFile::append(const char* logline, const size_t len)
{
  size_t n = write(logline, len);
  size_t remain = len - n;
  while (remain > 0)  //ʣ��д���ֽ�������0
  {
    size_t x = write(logline + n, remain);
    if (x == 0)
    {
      int err = ferror(fp_);
      if (err)
      {
        fprintf(stderr, "AppendFile::append() failed %s\n", strerror_tl(err));
      }
      break;
    }
    n += x;
    remain = len - n; // remain -= x
  }

  writtenBytes_ += len;
}

void FileUtil::AppendFile::flush()
{
  ::fflush(fp_);
}

size_t FileUtil::AppendFile::write(const char* logline, size_t len)
{
  // #undef fwrite_unlocked
  return ::fwrite_unlocked(logline, 1, len, fp_);  //�������ķ�ʽд�룬Ч�ʸߣ�not thread safe
}

FileUtil::ReadSmallFile::ReadSmallFile(StringArg filename)
  : fd_(::open(filename.c_str(), O_RDONLY | O_CLOEXEC)),
    err_(0)
{
  buf_[0] = '\0';
  if (fd_ < 0)
  {
    err_ = errno;
  }
}

FileUtil::ReadSmallFile::~ReadSmallFile()
{
  if (fd_ >= 0)
  {
    ::close(fd_); // FIXME: check EINTR
  }
}


/////////////////////////////////////////////////////////////////////////////////////////
//�������ṩ������ģ�庯�������ⲿʹ�ã��ֱ��Ƕ�ȡ���ַ���string���ͺͶ�ȡ��������buffer����
// return errno
template<typename String>
int FileUtil::ReadSmallFile::readToString(int maxSize,
                                          String* content,
                                          int64_t* fileSize,
                                          int64_t* modifyTime,
                                          int64_t* createTime)
{
  BOOST_STATIC_ASSERT(sizeof(off_t) == 8);
  assert(content != NULL);
  int err = err_;
  if (fd_ >= 0)
  {
    content->clear();

    if (fileSize)  //�����Ϊ�գ���ȡ�ļ���С
    {
      struct stat statbuf;  //fstat�������� ��ȡ�ļ�����ͨ�ļ���Ŀ¼���ܵ���socket���ַ����飨��������
      if (::fstat(fd_, &statbuf) == 0)   //fstat������ȡ�ļ���С�����浽����������
      {
        if (S_ISREG(statbuf.st_mode))
        {
          *fileSize = statbuf.st_size;  //stat�ṹ������st_size���������ļ���С�������������ָ��
          content->reserve(static_cast<int>(std::min(implicit_cast<int64_t>(maxSize), *fileSize)));
        }
        else if (S_ISDIR(statbuf.st_mode))  
        {
          err = EISDIR;
        }
        if (modifyTime)  //�޸�ʱ�䣬����ʱ���
        {
          *modifyTime = statbuf.st_mtime;
        }
        if (createTime)
        {
          *createTime = statbuf.st_ctime;
        }
      }
      else
      {
        err = errno;
      }
    }

    while (content->size() < implicit_cast<size_t>(maxSize))
    {
      //������
      size_t toRead = std::min(implicit_cast<size_t>(maxSize) - content->size(), sizeof(buf_));
      ssize_t n = ::read(fd_, buf_, toRead);//���ļ����ж�ȡ���ݵ��ַ���
      if (n > 0)
      {
        content->append(buf_, n);  //׷�ӵ��ַ���
      }
      else
      {
        if (n < 0)
        {
          err = errno;
        }
        break;
      }
    }
  }
  return err;
}

//��ȡ��������
int FileUtil::ReadSmallFile::readToBuffer(int* size)
{
  int err = err_;
  if (fd_ >= 0)
  {
    ssize_t n = ::pread(fd_, buf_, sizeof(buf_)-1, 0); //pread��read����pread��ȡ���ļ�offset������ģ�
    if (n >= 0)                                                        //��ǰ���Ļ����ģ���read������offset��������ֽ����ƶ�
    {
      if (size)
      {
        *size = static_cast<int>(n);
      }
      buf_[n] = '\0';
    }
    else
    {
      err = errno;
    }
  }
  return err;
}

//һЩģ����ػ�
template int FileUtil::readFile(StringArg filename,
                                int maxSize,
                                string* content,
                                int64_t*, int64_t*, int64_t*);

template int FileUtil::ReadSmallFile::readToString(
    int maxSize,
    string* content,
    int64_t*, int64_t*, int64_t*);

#ifndef MUDUO_STD_STRING
template int FileUtil::readFile(StringArg filename,
                                int maxSize,
                                std::string* content,
                                int64_t*, int64_t*, int64_t*);

template int FileUtil::ReadSmallFile::readToString(
    int maxSize,
    std::string* content,
    int64_t*, int64_t*, int64_t*);
#endif

