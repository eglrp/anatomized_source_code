/*
 * Copyright (c) 2006 Maxim Yegorushkin <maxim.yegorushkin@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef _MIN_HEAP_H_
#define _MIN_HEAP_H_

#include "event.h"
#include "evutil.h"

typedef struct min_heap
{
    struct event** p;    //指向event*类型数组
    unsigned n, a;    //n是元素个数，a表示p指向内存的尺寸(这命名好猥琐)
} min_heap_t;

static inline void           min_heap_ctor(min_heap_t* s);
static inline void           min_heap_dtor(min_heap_t* s);
static inline void           min_heap_elem_init(struct event* e);
static inline int            min_heap_elem_greater(struct event *a, struct event *b);
static inline int            min_heap_empty(min_heap_t* s);
static inline unsigned       min_heap_size(min_heap_t* s);
static inline struct event*  min_heap_top(min_heap_t* s);
static inline int            min_heap_reserve(min_heap_t* s, unsigned n);
static inline int            min_heap_push(min_heap_t* s, struct event* e);
static inline struct event*  min_heap_pop(min_heap_t* s);
static inline int            min_heap_erase(min_heap_t* s, struct event* e);
static inline void           min_heap_shift_up_(min_heap_t* s, unsigned hole_index, struct event* e);
static inline void           min_heap_shift_down_(min_heap_t* s, unsigned hole_index, struct event* e);

int min_heap_elem_greater(struct event *a, struct event *b)   //大于
{
    return evutil_timercmp(&a->ev_timeout, &b->ev_timeout, >);
}

void min_heap_ctor(min_heap_t* s) { s->p = 0; s->n = 0; s->a = 0; }   //构造函数，零初始化
void min_heap_dtor(min_heap_t* s) { free(s->p); }   //析构函数
void min_heap_elem_init(struct event* e) { e->min_heap_idx = -1; }
int min_heap_empty(min_heap_t* s) { return 0u == s->n; }
unsigned min_heap_size(min_heap_t* s) { return s->n; }
struct event* min_heap_top(min_heap_t* s) { return s->n ? *s->p : 0; }   //堆顶元素

int min_heap_push(min_heap_t* s, struct event* e)   //入堆
{
    if(min_heap_reserve(s, s->n + 1))
        return -1;
    min_heap_shift_up_(s, s->n++, e);   //s->n++值得注意
    return 0;
}

struct event* min_heap_pop(min_heap_t* s)   //出堆
{
    if(s->n)
    {
        struct event* e = *s->p;
        min_heap_shift_down_(s, 0u, s->p[--s->n]);
        e->min_heap_idx = -1;
        return e;
    }
    return 0;
}

int min_heap_erase(min_heap_t* s, struct event* e)   //从堆里删除某事件
{
    if(((unsigned int)-1) != e->min_heap_idx)   //判断下标
    {
        struct event *last = s->p[--s->n];
        unsigned parent = (e->min_heap_idx - 1) / 2;
	/* we replace e with the last element in the heap.  We might need to
	   shift it upward if it is less than its parent, or downward if it is
	   greater than one or both its children. Since the children are known
	   to be less than the parent, it can't need to shift both up and
	   down. */
        if (e->min_heap_idx > 0 && min_heap_elem_greater(s->p[parent], last))
             min_heap_shift_up_(s, e->min_heap_idx, last);  //最后一个小，就直接shift_up
        else
             min_heap_shift_down_(s, e->min_heap_idx, last);
        e->min_heap_idx = -1;
        return 0;
    }
    return -1;
}

int min_heap_reserve(min_heap_t* s, unsigned n)  //n元素个数
{
    if(s->a < n)   
    {
        struct event** p;
        unsigned a = s->a ? s->a * 2 : 8;   //这个叼，要么初始化为8，要么每次乘以二
        if(a < n)   //仍旧不足，则分配n
            a = n;
        if(!(p = (struct event**)realloc(s->p, a * sizeof *p)))
            return -1;
        s->p = p;
        s->a = a;
    }
    return 0;
}

void min_heap_shift_up_(min_heap_t* s, unsigned hole_index, struct event* e)  //从突破口开始向上shift，找到合适位置插入e
{
    unsigned parent = (hole_index - 1) / 2;
    while(hole_index && min_heap_elem_greater(s->p[parent], e))   //只有大于父节点就循环
    {   
        (s->p[hole_index] = s->p[parent])->min_heap_idx = hole_index;  //交换位置
        hole_index = parent;
        parent = (hole_index - 1) / 2;
    }
    (s->p[hole_index] = e)->min_heap_idx = hole_index;
}

void min_heap_shift_down_(min_heap_t* s, unsigned hole_index, struct event* e)  //删除突破口，从突破口开始向下shift，
{
    unsigned min_child = 2 * (hole_index + 1);   //从零开始，举例0，1，2即可，min_child的下标为右孩子下标
    while(min_child <= s->n)   //堆的下调每次取两个孩子中最小的那个，因为你要保证和parent交换位置的child要比另外一个child小
	{                   //如果min_child==s->n，后面表达式一定为真，min_child -= 1；这个情况是保证堆的末尾出现只有左孩子，无右孩子，这种情况min_child必须-1。
        min_child -= min_child == s->n || min_heap_elem_greater(s->p[min_child], s->p[min_child - 1]); //找出两个child中较小的
        if(!(min_heap_elem_greater(e, s->p[min_child])))  //e>p[min_child]继续循环
            break;
        (s->p[hole_index] = s->p[min_child])->min_heap_idx = hole_index;
        hole_index = min_child;
        min_child = 2 * (hole_index + 1);  //min_child是下标，所以这样直接用2*(hole_Index+1)，如果没有右孩子，这个下标指向了一个虚拟的右孩子，需要在上面减1处理
	}
    min_heap_shift_up_(s, hole_index,  e);//上面的移动产生了一个空缺，将最右下的节点插到空缺处，当作插入处理
}    //一开始我在纠结为什么要当作插入处理，后来发现这样做其实画蛇添足，直接 (s->p[hole_index] = e)->min_heap_idx = hole_index;即可
       //因为上面循环中的if(!condition)已经保证e operator> parent了，根本不必shift_up。
#endif /* _MIN_HEAP_H_ */
