/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "swoole.h"

typedef struct
{
    uint8_t shared;   //可共享
    uint8_t status;    
    uint32_t size;      //内存池大小
    uint32_t alloc_offset;    //分享内存的起始长度
    uint32_t collect_offset;   //可用内存的终止长度
    uint32_t alloc_count;    
    sw_atomic_t free_count;   //有多少内存待回收
    void *memory;  //内存池的起始地址
} swRingBuffer;

typedef struct
{
    uint16_t lock;
    uint16_t index;
    uint32_t length;   //每个内存块记录长度的变量
    char data[0];
} swRingBuffer_item;

static void swRingBuffer_destory(swMemoryPool *pool);
static void* swRingBuffer_alloc(swMemoryPool *pool, uint32_t size);
static void swRingBuffer_free(swMemoryPool *pool, void *ptr);

#ifdef SW_RINGBUFFER_DEBUG
static void swRingBuffer_print(swRingBuffer *object, char *prefix);

static void swRingBuffer_print(swRingBuffer *object, char *prefix)
{
    printf("%s: size=%d, status=%d, alloc_count=%d, free_count=%d, offset=%d, next_offset=%d\n", prefix, object->size,
            object->status, object->alloc_count, object->free_count, object->alloc_offset, object->collect_offset);
}
#endif

swMemoryPool *swRingBuffer_new(uint32_t size, uint8_t shared)
{
    void *mem = (shared == 1) ? sw_shm_malloc(size) : sw_malloc(size);
    if (mem == NULL)
    {
        swWarn("malloc(%d) failed.", size);
        return NULL;
    }

    swRingBuffer *object = mem;
    mem += sizeof(swRingBuffer);
    bzero(object, sizeof(swRingBuffer));

    object->size = (size - sizeof(swRingBuffer) - sizeof(swMemoryPool));
    object->shared = shared;

    swMemoryPool *pool = mem;
    mem += sizeof(swMemoryPool);

    pool->object = object;
    pool->destroy = swRingBuffer_destory;
    pool->free = swRingBuffer_free;
    pool->alloc = swRingBuffer_alloc;

    object->memory = mem;

    swDebug("memory: ptr=%p", mem);

    return pool;
}

static void swRingBuffer_collect(swRingBuffer *object)
{
    swRingBuffer_item *item;
    sw_atomic_t *free_count = &object->free_count;   //获取带货收内存数目

    int count = object->free_count;
    int i;
    uint32_t n_size;

    for (i = 0; i < count; i++)
    {
        item = object->memory + object->collect_offset;   //从collect_offset开始回收
        if (item->lock == 0)
        {
            n_size = item->length + sizeof(swRingBuffer_item);  //每一块内存由item结构体加真实内存组成，在这里获取总长度

            object->collect_offset += n_size;

            if (object->collect_offset + sizeof(swRingBuffer_item) >object->size || object->collect_offset >= object->size)
            {
                object->collect_offset = 0;
                object->status = 0;
            }
            sw_atomic_fetch_sub(free_count, 1);   //原子操作，free_count-1
        }
        else
        {
            break;
        }
    }
}

static void* swRingBuffer_alloc(swMemoryPool *pool, uint32_t size)
{
    assert(size > 0);  //噗，很少见到源码用断言，不过我喜欢

    swRingBuffer *object = pool->object;
    swRingBuffer_item *item;
    uint32_t capacity;

    uint32_t alloc_size = size + sizeof(swRingBuffer_item);  //没错，分配内存就是一个item加真实内存的组合

    if (object->free_count > 0)   //如果有空闲内存，走到这里的时候顺便回收空闲内存
        swRingBuffer_collect(object);
    }

    if (object->status == 0)
    {
    	//特殊情况，内存在末尾，有内存但不够用
    	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        if (object->alloc_offset + alloc_size >= (object->size - sizeof(swRingBuffer_item))) //如果最末尾空闲内存不足alloc_size
        {
            uint32_t skip_n = object->size - object->alloc_offset;  //末尾可分配内存
            if (skip_n >= sizeof(swRingBuffer_item))   //大于item所用的内存，我们先单独分配一块item
            {
                item = object->memory + object->alloc_offset;  //内存池首地址+偏移量得到可分配内存首地址
                item->lock = 0;  
                item->length = skip_n - sizeof(swRingBuffer_item);  //剩下需要分配的内存
                sw_atomic_t *free_count = &object->free_count; 
                sw_atomic_fetch_add(free_count, 1);  //已分配块数+1
            }
            object->alloc_offset = 0;  //调整偏移量从头重新开始
            object->status = 1;   //
            capacity = object->collect_offset - object->alloc_offset;   //由于从头开始了，容量就等于collect_offset-alloc_offset
        }
        else
        {
            capacity = object->size - object->alloc_offset;
        }
    }
    else
    {
        capacity = object->collect_offset - object->alloc_offset;
    }

    if (capacity < alloc_size)
    {
        return NULL;
    }

    item = object->memory + object->alloc_offset;
    item->lock = 1;
    item->length = size;   
    item->index = object->alloc_count;   

    object->alloc_offset += alloc_size;   //更新alloc_offset，表示分配出去一块内存
    object->alloc_count ++;

    swDebug("alloc: ptr=%d", (void *)item->data - object->memory);

    return item->data;  
}

static void swRingBuffer_free(swMemoryPool *pool, void *ptr)
{
    swRingBuffer *object = pool->object;
    swRingBuffer_item *item = ptr - sizeof(swRingBuffer_item);

    assert(ptr >= object->memory);
    assert(ptr <= object->memory + object->size);
    assert(item->lock == 1);

    if (item->lock != 1)
    {
        swDebug("invalid free: index=%d, ptr = %d\n", item->index,  (void * ) item->data - object->memory);
    }
    else
    {
        item->lock = 0;
    }

    swDebug("free: ptr=%d", (void * ) item->data - object->memory);

    sw_atomic_t *free_count = &object->free_count;
    sw_atomic_fetch_add(free_count, 1);
}

static void swRingBuffer_destory(swMemoryPool *pool)
{
    swRingBuffer *object = pool->object;
    if (object->shared)
    {
        sw_shm_free(object);
    }
    else
    {
        sw_free(object);
    }
}
