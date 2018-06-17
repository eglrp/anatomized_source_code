#ifndef BOOST_SMART_PTR_SCOPED_PTR_HPP_INCLUDED
#define BOOST_SMART_PTR_SCOPED_PTR_HPP_INCLUDED

#include <boost/config.hpp>
#include <boost/assert.hpp>
#include <boost/checked_delete.hpp>
#include <boost/smart_ptr/detail/sp_nullptr_t.hpp>
#include <boost/detail/workaround.hpp>

#ifndef BOOST_NO_AUTO_PTR
# include <memory>          // for std::auto_ptr
#endif

namespace boost
{

//  scoped_ptr mimics a built-in pointer except that it guarantees deletion
//  of the object pointed to, either on destruction of the scoped_ptr or via
//  an explicit reset(). scoped_ptr is a simple solution for simple needs;
//  use shared_ptr or std::auto_ptr if your needs are more complex.

template<class T> class scoped_ptr // noncopyable
{
private:

    T * px;     

    //禁止copy assignment
    scoped_ptr(scoped_ptr const &);
    scoped_ptr & operator=(scoped_ptr const &);

    typedef scoped_ptr<T> this_type;

    //禁止比较
    void operator==( scoped_ptr const& ) const;
    void operator!=( scoped_ptr const& ) const;

public:

    typedef T element_type;
    //使用普通指针构造，禁止隐式转换
    explicit scoped_ptr( T * p = 0 ): px( p ) // never throws
    {
    }

#ifndef BOOST_NO_AUTO_PTR
    //噗，scoped_ptr是可以从auto_ptr那里夺取过来的，不过auto_ptr调用了releadse自然就失效了
    explicit scoped_ptr( std::auto_ptr<T> p ) BOOST_NOEXCEPT : px( p.release() )
    {
    }
#endif

    ~scoped_ptr() // never throws
    {
        boost::checked_delete( px );   //利用sizeof(px)的大小去声明一个数组，检查是否可行，不可行说明是incomplete类型，不执行delete，这在编译器就可以决断
    }

    //使用该函数删除scoped_ptr内部指针，重置为p指针。不过这不符合scoped_ptr意图，该函数尽量不要用
    void reset(T * p = 0) // never throws
    {
        BOOST_ASSERT( p == 0 || p != px ); // catch self-reset errors
        this_type(p).swap(*this);
    }

    T & operator*() const // never throws
    {
        BOOST_ASSERT( px != 0 );
        return *px;
    }

    T * operator->() const // never throws
    {
        BOOST_ASSERT( px != 0 );
        return px;
    }

    T * get() const BOOST_NOEXCEPT
    {
        return px;
    }

// implicit conversion to "bool"
#include <boost/smart_ptr/detail/operator_bool.hpp>  //有关bool类型的运算符重载

    //swap仅交换指针，这是对"pimpl"手法的优化
    void swap(scoped_ptr & b) BOOST_NOEXCEPT
    {
        T * tmp = b.px;
        b.px = px;
        px = tmp;
    }
};

#if !defined( BOOST_NO_CXX11_NULLPTR )

template<class T> inline bool operator==( scoped_ptr<T> const & p, boost::detail::sp_nullptr_t ) BOOST_NOEXCEPT
{
    return p.get() == 0;
}

template<class T> inline bool operator==( boost::detail::sp_nullptr_t, scoped_ptr<T> const & p ) BOOST_NOEXCEPT
{
    return p.get() == 0;
}

template<class T> inline bool operator!=( scoped_ptr<T> const & p, boost::detail::sp_nullptr_t ) BOOST_NOEXCEPT
{
    return p.get() != 0;
}

template<class T> inline bool operator!=( boost::detail::sp_nullptr_t, scoped_ptr<T> const & p ) BOOST_NOEXCEPT
{
    return p.get() != 0;
}

#endif

//boost作用域内的swap，是一个no-member函数，供外部交换两个scoped_ptr。不用std::swap的原因是std::swap不能优化"pimpl"手法。
template<class T> inline void swap(scoped_ptr<T> & a, scoped_ptr<T> & b) BOOST_NOEXCEPT
{
    a.swap(b);
}

// get_pointer(p) is a generic way to say p.get()
//提供外部获取指针的途径
template<class T> inline T * get_pointer(scoped_ptr<T> const & p) BOOST_NOEXCEPT
{
    return p.get();
}

} // namespace boost

#endif // #ifndef BOOST_SMART_PTR_SCOPED_PTR_HPP_INCLUDED
