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

    //��ֹcopy assignment
    scoped_ptr(scoped_ptr const &);
    scoped_ptr & operator=(scoped_ptr const &);

    typedef scoped_ptr<T> this_type;

    //��ֹ�Ƚ�
    void operator==( scoped_ptr const& ) const;
    void operator!=( scoped_ptr const& ) const;

public:

    typedef T element_type;
    //ʹ����ָͨ�빹�죬��ֹ��ʽת��
    explicit scoped_ptr( T * p = 0 ): px( p ) // never throws
    {
    }

#ifndef BOOST_NO_AUTO_PTR
    //�ۣ�scoped_ptr�ǿ��Դ�auto_ptr�����ȡ�����ģ�����auto_ptr������releadse��Ȼ��ʧЧ��
    explicit scoped_ptr( std::auto_ptr<T> p ) BOOST_NOEXCEPT : px( p.release() )
    {
    }
#endif

    ~scoped_ptr() // never throws
    {
        boost::checked_delete( px );   //����sizeof(px)�Ĵ�Сȥ����һ�����飬����Ƿ���У�������˵����incomplete���ͣ���ִ��delete�����ڱ������Ϳ��Ծ���
    }

    //ʹ�øú���ɾ��scoped_ptr�ڲ�ָ�룬����Ϊpָ�롣�����ⲻ����scoped_ptr��ͼ���ú���������Ҫ��
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
#include <boost/smart_ptr/detail/operator_bool.hpp>  //�й�bool���͵����������

    //swap������ָ�룬���Ƕ�"pimpl"�ַ����Ż�
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

//boost�������ڵ�swap����һ��no-member���������ⲿ��������scoped_ptr������std::swap��ԭ����std::swap�����Ż�"pimpl"�ַ���
template<class T> inline void swap(scoped_ptr<T> & a, scoped_ptr<T> & b) BOOST_NOEXCEPT
{
    a.swap(b);
}

// get_pointer(p) is a generic way to say p.get()
//�ṩ�ⲿ��ȡָ���;��
template<class T> inline T * get_pointer(scoped_ptr<T> const & p) BOOST_NOEXCEPT
{
    return p.get();
}

} // namespace boost

#endif // #ifndef BOOST_SMART_PTR_SCOPED_PTR_HPP_INCLUDED
