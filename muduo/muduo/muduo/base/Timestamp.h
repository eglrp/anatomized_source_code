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
//muduo::copyable空基类，标识类，值类型，凡是继承了它就可以拷贝  
//值语义，可以拷贝，拷贝之后与原对象脱离关系
//对象语义，要么不能拷贝，要么可以拷贝，拷贝之后与原对象仍存在一定关系，比如共享一定资源，取决于自己的拷贝构造函数
class Timestamp : public muduo::copyable,
                  public boost::less_than_comparable<Timestamp>
                  //less_than_comparable<>是boost的一个模板类，继承该类要求实现<，然后它会自动实现>,<=,>=，有点牛逼。。。
{
 public:
  ///
  /// Constucts an invalid Timestamp.
  ///
  Timestamp()
    : microSecondsSinceEpoch_(0)  //从1970年到当前的时间，单位微秒
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

  //交换函数,形参是引用
  void swap(Timestamp& that)
  {
    std::swap(microSecondsSinceEpoch_, that.microSecondsSinceEpoch_);
  }

  // default copy/assignment/dtor are Okay

  string toString() const;
  string toFormattedString(bool showMicroseconds = true) const;

  bool valid() const { return microSecondsSinceEpoch_ > 0; }   //大于0有效

  // for internal usage.
  int64_t microSecondsSinceEpoch() const { return microSecondsSinceEpoch_; }
  //time_t不是结构体，定义为tydef long time_t，一般存放从unxi时间到当前的秒数
  time_t secondsSinceEpoch() const     //微秒转化为秒
  { return static_cast<time_t>(microSecondsSinceEpoch_ / kMicroSecondsPerSecond); }

  ///
  /// Get time of now.
  ///
  static Timestamp now();
  static Timestamp invalid()
  {
    return Timestamp();  //获取一个无效时间，即时间等于0
  }

  static Timestamp fromUnixTime(time_t t)   //time_t类型时间转化为内部是unix时间的Timestamp结构
  {
    return fromUnixTime(t, 0);  //调用内部函数执行实际操作，微秒忽略
  }

  static Timestamp fromUnixTime(time_t t, int microseconds)
  {
    return Timestamp(static_cast<int64_t>(t) * kMicroSecondsPerSecond + microseconds); 
  }

  static const int kMicroSecondsPerSecond = 1000 * 1000;  //一百万，一微秒等于百万分之一秒

 private:
  int64_t microSecondsSinceEpoch_;
};

//只需重载<,> <= >= 这几种less_than_comparable()帮我们实现
inline bool operator<(Timestamp lhs, Timestamp rhs)
{
  return lhs.microSecondsSinceEpoch() < rhs.microSecondsSinceEpoch();
}

//判等
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
inline double timeDifference(Timestamp high, Timestamp low)  //实现两个事件的差
{
  int64_t diff = high.microSecondsSinceEpoch() - low.microSecondsSinceEpoch();
  return static_cast<double>(diff) / Timestamp::kMicroSecondsPerSecond;   //将返回时间差的秒数，注意单位!
}

///
/// Add @c seconds to given timestamp.
///
/// @return timestamp+seconds as Timestamp
///
//timestamp没有用引用传递时因为它内部只有蔵nt64_t类型，相当于整数，8个字节，传递过程中会放在8字节寄存器当中，当然是64位平台，而不是堆栈当中。
inline Timestamp addTime(Timestamp timestamp, double seconds)
{
  //把秒转化为微秒，构造一个对象，再把它们的时间加起来，构造一个无名临时对象返回
  int64_t delta = static_cast<int64_t>(seconds * Timestamp::kMicroSecondsPerSecond);
  return Timestamp(timestamp.microSecondsSinceEpoch() + delta);
}

}
#endif  // MUDUO_BASE_TIMESTAMP_H
