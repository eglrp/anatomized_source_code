#include <muduo/base/Timestamp.h>

#include <sys/time.h>
#include <stdio.h>

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>

#include <boost/static_assert.hpp>

using namespace muduo;

//����׶ζ���
BOOST_STATIC_ASSERT(sizeof(Timestamp) == sizeof(int64_t));

string Timestamp::toString() const
{
  char buf[32] = {0};
  int64_t seconds = microSecondsSinceEpoch_ / kMicroSecondsPerSecond;
  int64_t microseconds = microSecondsSinceEpoch_ % kMicroSecondsPerSecond;
  //PRId64��ƽ̨��ӡ64λ��������Ϊint64_t������ʾ64λ��������32λϵͳ����long long int��64λϵͳ����long int
  //���Դ�ӡ64λ��%ld��%lld������ֲ�Խϲ����ͳһͬPRID64����ӡ��
  snprintf(buf, sizeof(buf)-1, "%" PRId64 ".%06" PRId64 "", seconds, microseconds);
  return buf;
}

//����ת����һ����ʽ���ַ���
string Timestamp::toFormattedString(bool showMicroseconds) const
{
  char buf[32] = {0};
  //����1970�����ڵ�����
  time_t seconds = static_cast<time_t>(microSecondsSinceEpoch_ / kMicroSecondsPerSecond);
  struct tm tm_time;
  gmtime_r(&seconds, &tm_time);  //������ת����tm�ṹ�壬��_r��ʾ��һ���̰߳�ȫ�ĺ���

  if (showMicroseconds)  //�Ƿ���ʾ΢���ʶ 
  {
    //ȡ����΢��
    int microseconds = static_cast<int>(microSecondsSinceEpoch_ % kMicroSecondsPerSecond);
    //������ʱ����
    snprintf(buf, sizeof(buf), "%4d%02d%02d %02d:%02d:%02d.%06d",
             tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
             tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec,
             microseconds);
  }
  else
  {
    snprintf(buf, sizeof(buf), "%4d%02d%02d %02d:%02d:%02d",
             tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
             tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec);
  }
  return buf;   //����string����ʽ����!!!
}

Timestamp Timestamp::now()
{
  struct timeval tv;    
  gettimeofday(&tv, NULL);   //��õ�ǰʱ�䣬�ڶ���������һ��ʱ������ǰ����Ҫ����ʱ���������ָ��
  int64_t seconds = tv.tv_sec;   //ȡ������
  return Timestamp(seconds * kMicroSecondsPerSecond + tv.tv_usec);  //��*100��+΢�룬���Ǵ�1970�����ڵ�΢����
}

