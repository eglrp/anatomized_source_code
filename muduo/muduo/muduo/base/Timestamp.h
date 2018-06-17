#ifndef MUDUO_BASE_TIMESTAMP_H
#define MUDUO_BASE_TIMESTAMP_H

#include <muduo/base/copyable.h>
#include <muduo/base/Types.h>

#include <boost/operators.hpp>

namespace muduo
{

///
/// Time stamp in UTC, in microseconds resolution.
///
/// This class is immutable.
/// It's recommended to pass it by value, since it's passed in register on x64.
///
//muduo::copyable�ջ��࣬��ʶ�ֵ࣬���ͣ����Ǽ̳������Ϳ��Կ���  
//ֵ���壬���Կ���������֮����ԭ���������ϵ
//�������壬Ҫô���ܿ�����Ҫô���Կ���������֮����ԭ�����Դ���һ����ϵ�����繲��һ����Դ��ȡ�����Լ��Ŀ������캯��
class Timestamp : public muduo::copyable,
                  public boost::less_than_comparable<Timestamp>
                  //less_than_comparable<>��boost��һ��ģ���࣬�̳и���Ҫ��ʵ��<��Ȼ�������Զ�ʵ��>,<=,>=���е�ţ�ơ�����
{
 public:
  ///
  /// Constucts an invalid Timestamp.
  ///
  Timestamp()
    : microSecondsSinceEpoch_(0)  //��1970�굽��ǰ��ʱ�䣬��λ΢��
  {
  }

  ///
  /// Constucts a Timestamp at specific time
  ///
  /// @param microSecondsSinceEpoch
  explicit Timestamp(int64_t microSecondsSinceEpochArg)
    : microSecondsSinceEpoch_(microSecondsSinceEpochArg)
  {
  }

  //��������,�β�������
  void swap(Timestamp& that)
  {
    std::swap(microSecondsSinceEpoch_, that.microSecondsSinceEpoch_);
  }

  // default copy/assignment/dtor are Okay

  string toString() const;
  string toFormattedString(bool showMicroseconds = true) const;

  bool valid() const { return microSecondsSinceEpoch_ > 0; }   //����0��Ч

  // for internal usage.
  int64_t microSecondsSinceEpoch() const { return microSecondsSinceEpoch_; }
  //time_t���ǽṹ�壬����Ϊtydef long time_t��һ���Ŵ�unxiʱ�䵽��ǰ������
  time_t secondsSinceEpoch() const     //΢��ת��Ϊ��
  { return static_cast<time_t>(microSecondsSinceEpoch_ / kMicroSecondsPerSecond); }

  ///
  /// Get time of now.
  ///
  static Timestamp now();
  static Timestamp invalid()
  {
    return Timestamp();  //��ȡһ����Чʱ�䣬��ʱ�����0
  }

  static Timestamp fromUnixTime(time_t t)   //time_t����ʱ��ת��Ϊ�ڲ���unixʱ���Timestamp�ṹ
  {
    return fromUnixTime(t, 0);  //�����ڲ�����ִ��ʵ�ʲ�����΢�����
  }

  static Timestamp fromUnixTime(time_t t, int microseconds)
  {
    return Timestamp(static_cast<int64_t>(t) * kMicroSecondsPerSecond + microseconds); 
  }

  static const int kMicroSecondsPerSecond = 1000 * 1000;  //һ����һ΢����ڰ����֮һ��

 private:
  int64_t microSecondsSinceEpoch_;
};

//ֻ������<,> <= >= �⼸��less_than_comparable()������ʵ��
inline bool operator<(Timestamp lhs, Timestamp rhs)
{
  return lhs.microSecondsSinceEpoch() < rhs.microSecondsSinceEpoch();
}

//�е�
inline bool operator==(Timestamp lhs, Timestamp rhs)
{
  return lhs.microSecondsSinceEpoch() == rhs.microSecondsSinceEpoch();
}

///
/// Gets time difference of two timestamps, result in seconds.
///
/// @param high, low
/// @return (high-low) in seconds
/// @c double has 52-bit precision, enough for one-microsecond
/// resolution for next 100 years.
inline double timeDifference(Timestamp high, Timestamp low)  //ʵ�������¼��Ĳ�
{
  int64_t diff = high.microSecondsSinceEpoch() - low.microSecondsSinceEpoch();
  return static_cast<double>(diff) / Timestamp::kMicroSecondsPerSecond;   //������ʱ����������ע�ⵥλ!
}

///
/// Add @c seconds to given timestamp.
///
/// @return timestamp+seconds as Timestamp
///
//timestampû�������ô���ʱ��Ϊ���ڲ�ֻ���int64_t���ͣ��൱��������8���ֽڣ����ݹ����л����8�ֽڼĴ������У���Ȼ��64λƽ̨�������Ƕ�ջ���С�
inline Timestamp addTime(Timestamp timestamp, double seconds)
{
  //����ת��Ϊ΢�룬����һ�������ٰ����ǵ�ʱ�������������һ��������ʱ���󷵻�
  int64_t delta = static_cast<int64_t>(seconds * Timestamp::kMicroSecondsPerSecond);
  return Timestamp(timestamp.microSecondsSinceEpoch() + delta);
}

}
#endif  // MUDUO_BASE_TIMESTAMP_H
