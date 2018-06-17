/*
 *
 * Copyright (c) 1994
 * Hewlett-Packard Company
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.  Hewlett-Packard Company makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 *
 * Copyright (c) 1996,1997
 * Silicon Graphics Computer Systems, Inc.
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.  Silicon Graphics makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 */

/* NOTE: This is an internal header file, included by other STL headers.
 *   You should not attempt to use it directly.
 */

#ifndef __SGI_STL_INTERNAL_MAP_H
#define __SGI_STL_INTERNAL_MAP_H

__STL_BEGIN_NAMESPACE

#if defined(__sgi) && !defined(__GNUC__) && (_MIPS_SIM != _MIPS_SIM_ABI32)
#pragma set woff 1174
#endif

#ifndef __STL_LIMITED_DEFAULT_TEMPLATES
template <class Key, class T, class Compare = less<Key>, class Alloc = alloc>
#else
template <class Key, class T, class Compare, class Alloc = alloc>
#endif
class map {
public:

// typedefs:

  typedef Key key_type;    //键值类型 
  typedef T data_type;       //数据类型
  typedef T mapped_type;         //键所关联的值类型
  typedef pair<const Key, T> value_type;     //元素值是对组
  typedef Compare key_compare;                 //自定义比较
    
  class value_compare     //自定义仿函数
    : public binary_function<value_type, value_type, bool> {
  friend class map<Key, T, Compare, Alloc>;   //提供友元
  protected :
    Compare comp;     //比较仿函数对象
    value_compare(Compare c) : comp(c) {}   //比较函数
  public:
    bool operator()(const value_type& x, const value_type& y) const {
      return comp(x.first, y.first);   //仿函数重载()运算符
    }
  };

private:     //select1st is a function object that takes a single argument, a pair[1], and returns the pairs first argument
  typedef rb_tree<key_type, value_type, 
                  select1st<value_type>, key_compare, Alloc> rep_type;
  rep_type t;  // red-black tree representing map     //红黑树
public:
  typedef typename rep_type::pointer pointer;
  typedef typename rep_type::const_pointer const_pointer;
  typedef typename rep_type::reference reference;
  typedef typename rep_type::const_reference const_reference;
  typedef typename rep_type::iterator iterator;
  typedef typename rep_type::const_iterator const_iterator;
  typedef typename rep_type::reverse_iterator reverse_iterator;
  typedef typename rep_type::const_reverse_iterator const_reverse_iterator;
  typedef typename rep_type::size_type size_type;
  typedef typename rep_type::difference_type difference_type;

  // allocation/deallocation

  map() : t(Compare()) {}    //初始化map，其中初始化了红黑树
  explicit map(const Compare& comp) : t(comp) {}    //禁止隐式构造的构造函数

#ifdef __STL_MEMBER_TEMPLATES
  template <class InputIterator>
  map(InputIterator first, InputIterator last)     //first-last式构造函数，调用红黑树的唯一插入方法
    : t(Compare()) { t.insert_unique(first, last); }

  template <class InputIterator>    //定义比较方式的构造函数
  map(InputIterator first, InputIterator last, const Compare& comp)
    : t(comp) { t.insert_unique(first, last); }
#else
  map(const value_type* first, const value_type* last)     //value_type是对组类型
    : t(Compare()) { t.insert_unique(first, last); }
  map(const value_type* first, const value_type* last, const Compare& comp)
    : t(comp) { t.insert_unique(first, last); }

  map(const_iterator first, const_iterator last)
    : t(Compare()) { t.insert_unique(first, last); }
  map(const_iterator first, const_iterator last, const Compare& comp)   //迭代器构造
    : t(comp) { t.insert_unique(first, last); }
#endif /* __STL_MEMBER_TEMPLATES */

  map(const map<Key, T, Compare, Alloc>& x) : t(x.t) {}   //拷贝map
  map<Key, T, Compare, Alloc>& operator=(const map<Key, T, Compare, Alloc>& x)
  {
    t = x.t;
    return *this;     //赋值map
  }

  // accessors:

  key_compare key_comp() const { return t.key_comp(); }
  value_compare value_comp() const { return value_compare(t.key_comp()); }
  iterator begin() { return t.begin(); }
  const_iterator begin() const { return t.begin(); }
  iterator end() { return t.end(); }
  const_iterator end() const { return t.end(); }
  reverse_iterator rbegin() { return t.rbegin(); }
  const_reverse_iterator rbegin() const { return t.rbegin(); }
  reverse_iterator rend() { return t.rend(); }
  const_reverse_iterator rend() const { return t.rend(); }
  bool empty() const { return t.empty(); }
  size_type size() const { return t.size(); }
  size_type max_size() const { return t.max_size(); }
  T& operator[](const key_type& k) {
    return (*((insert(value_type(k, T()))).first)).second;
  }
  void swap(map<Key, T, Compare, Alloc>& x) { t.swap(x.t); }

  // insert/erase

  pair<iterator,bool> insert(const value_type& x) { return t.insert_unique(x); }
  iterator insert(iterator position, const value_type& x) {
    return t.insert_unique(position, x);
  }
#ifdef __STL_MEMBER_TEMPLATES
  template <class InputIterator>
  void insert(InputIterator first, InputIterator last) {
    t.insert_unique(first, last);
  }
#else
  void insert(const value_type* first, const value_type* last) {
    t.insert_unique(first, last);
  }
  void insert(const_iterator first, const_iterator last) {
    t.insert_unique(first, last);
  }
#endif /* __STL_MEMBER_TEMPLATES */

  void erase(iterator position) { t.erase(position); }
  size_type erase(const key_type& x) { return t.erase(x); }
  void erase(iterator first, iterator last) { t.erase(first, last); }
  void clear() { t.clear(); }

  // map operations:

  iterator find(const key_type& x) { return t.find(x); }
  const_iterator find(const key_type& x) const { return t.find(x); }
  size_type count(const key_type& x) const { return t.count(x); }
  iterator lower_bound(const key_type& x) {return t.lower_bound(x); }
  const_iterator lower_bound(const key_type& x) const {
    return t.lower_bound(x); 
  }
  iterator upper_bound(const key_type& x) {return t.upper_bound(x); }
  const_iterator upper_bound(const key_type& x) const {
    return t.upper_bound(x); 
  }
  
  pair<iterator,iterator> equal_range(const key_type& x) {
    return t.equal_range(x);
  }
  pair<const_iterator,const_iterator> equal_range(const key_type& x) const {
    return t.equal_range(x);
  }
  friend bool operator== __STL_NULL_TMPL_ARGS (const map&, const map&);
  friend bool operator< __STL_NULL_TMPL_ARGS (const map&, const map&);
};

template <class Key, class T, class Compare, class Alloc>
inline bool operator==(const map<Key, T, Compare, Alloc>& x, 
                       const map<Key, T, Compare, Alloc>& y) {
  return x.t == y.t;
}

template <class Key, class T, class Compare, class Alloc>
inline bool operator<(const map<Key, T, Compare, Alloc>& x, 
                      const map<Key, T, Compare, Alloc>& y) {
  return x.t < y.t;
}

#ifdef __STL_FUNCTION_TMPL_PARTIAL_ORDER

template <class Key, class T, class Compare, class Alloc>
inline void swap(map<Key, T, Compare, Alloc>& x, 
                 map<Key, T, Compare, Alloc>& y) {
  x.swap(y);
}

#endif /* __STL_FUNCTION_TMPL_PARTIAL_ORDER */

#if defined(__sgi) && !defined(__GNUC__) && (_MIPS_SIM != _MIPS_SIM_ABI32)
#pragma reset woff 1174
#endif

__STL_END_NAMESPACE

#endif /* __SGI_STL_INTERNAL_MAP_H */

// Local Variables:
// mode:C++
// End:
