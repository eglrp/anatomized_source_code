#ifndef BOOST_SMART_PTR_DETAIL_SP_COUNTED_BASE_NT_HPP_INCLUDED
#define BOOST_SMART_PTR_DETAIL_SP_COUNTED_BASE_NT_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//
//  detail/sp_counted_base_nt.hpp
//
//  Copyright (c) 2001, 2002, 2003 Peter Dimov and Multi Media Ltd.
//  Copyright 2004-2005 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#include <boost/detail/sp_typeinfo.hpp>

namespace boost
{

namespace detail
{

//计数器基类
class sp_counted_base
{
private:

    sp_counted_base( sp_counted_base const & );
    sp_counted_base & operator= ( sp_counted_base const & );

    long use_count_;        // #shared
    long weak_count_;       // #weak + (#shared != 0)

public:

    sp_counted_base(): use_count_( 1 ), weak_count_( 1 )
    {
    }

    virtual ~sp_counted_base() // nothrow
    {
    }

    // dispose() is called when use_count_ drops to zero, to release
    // the resources managed by *this.

    virtual void dispose() = 0; // nothrow

    // destroy() is called when weak_count_ drops to zero.

    virtual void destroy() // nothrow
    {
        delete this;
    }

    virtual void * get_deleter( sp_typeinfo const & ti ) = 0;
    virtual void * get_untyped_deleter() = 0;

    void add_ref_copy()
    {
        ++use_count_;
    }

    bool add_ref_lock() // true on success
    {
        if( use_count_ == 0 ) return false;
        ++use_count_;
        return true;
    }

    void release() // nothrow
    {
        if( --use_count_ == 0 )
        {
            dispose();
            weak_release();   //也要执行weak_release，不过由于weak_count不一定为0，所以本release函数没有调用destroy
        }
    }

    //卧槽weak_ptr的weak_count虽然不影响shared_ptr的计数，但是weak_ptr自身也是引用计数只能指针，自身拷贝会增加weak_count
    void weak_add_ref() // nothrow
    {
        ++weak_count_;
    }

    void weak_release() // nothrow  //本函数是release函数中调用的，但只有use_count和weak_count都为0，才销毁sp_counted_bases
    {
        if( --weak_count_ == 0 )
        {
            destroy();
        }
    }

    long use_count() const // nothrow
    {
        return use_count_;
    }
};

} // namespace detail

} // namespace boost

#endif  // #ifndef BOOST_SMART_PTR_DETAIL_SP_COUNTED_BASE_NT_HPP_INCLUDED
