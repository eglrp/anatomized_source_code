#ifndef _LINUX_SLUB_DEF_H
#define _LINUX_SLUB_DEF_H

/*
 * SLUB : A Slab allocator without object queues.
 *
 * (C) 2007 SGI, Christoph Lameter
 */
#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>

enum stat_item {
	ALLOC_FASTPATH,		/* Allocation from cpu slab */
	ALLOC_SLOWPATH,		/* Allocation by getting a new cpu slab */
	FREE_FASTPATH,		/* Free to cpu slub */
	FREE_SLOWPATH,		/* Freeing not to cpu slab */
	FREE_FROZEN,		/* Freeing to frozen slab */
	FREE_ADD_PARTIAL,	/* Freeing moves slab to partial list */
	FREE_REMOVE_PARTIAL,	/* Freeing removes last object */
	ALLOC_FROM_PARTIAL,	/* Cpu slab acquired from partial list */
	ALLOC_SLAB,		/* Cpu slab acquired from page allocator */
	ALLOC_REFILL,		/* Refill cpu slab from slab freelist */
	FREE_SLAB,		/* Slab freed to the page allocator */
	CPUSLAB_FLUSH,		/* Abandoning of the cpu slab */
	DEACTIVATE_FULL,	/* Cpu slab was full when deactivated */
	DEACTIVATE_EMPTY,	/* Cpu slab was empty when deactivated */
	DEACTIVATE_TO_HEAD,	/* Cpu slab was moved to the head of partials */
	DEACTIVATE_TO_TAIL,	/* Cpu slab was moved to the tail of partials */
	DEACTIVATE_REMOTE_FREES,/* Slab contained remotely freed objects */
	ORDER_FALLBACK,		/* Number of times fallback was necessary */
	NR_SLUB_STAT_ITEMS };

struct kmem_cache_cpu {
	void **freelist;	//指向本地CPU的第一个空闲对象
	struct page *page;	/* The slab from which we are allocating */   //分配给本地CPU的页框
	int node;		/* The node of the page (or -1 for debug) */  //页框所处的节点，值为-1时表示DEBUG
	unsigned int offset;	/* Freepointer offset (in word units) */    //空闲对象指针的偏移，以字长为单位
	unsigned int objsize;	/* Size of an object (from kmem_cache) */   //对象的大小

#ifdef CONFIG_SLUB_STATS
	unsigned stat[NR_SLUB_STAT_ITEMS];   //用以记录slab的状态
#endif
};

//实际只有三个成员
struct kmem_cache_node {
	spinlock_t list_lock;	/* Protect partial list and nr_partial */
	unsigned long nr_partial;  //部分满链表中slab的数量
	struct list_head partial;

#ifdef CONFIG_SLUB_DEBUG
	atomic_long_t nr_slabs;
	atomic_long_t total_objects;
	struct list_head full;    //满链表只有在DEBUG的时候采用，空链表已经被弃用(以前的三链被抛弃了:(，我花了那么长时间才搞懂的三链，现在不用了...)
#endif
};

/*
 * Word size structure that can be atomically updated or read and that
 * contains both the order and the number of objects that a slab of the
 * given order would contain.
 */
struct kmem_cache_order_objects {
	unsigned long x;
};

/*
 * Slab cache management.
 */
struct kmem_cache {
	/* Used for retriving partial slabs etc */
	unsigned long flags;   //chache的属性描述标识
	int size;		//分配给对象的内存大小，可能大于实际对象的大小
	int objsize;    //对象的实际大小
	int offset;      //存放空闲对象的偏移，以字节为单位
	struct kmem_cache_order_objects oo;    //oo用来存放分配个slab的页框的阶数(高16位)和slab中对象的数量(低16位)

	/*
	 * Avoid an extra cache line for UP, SMP and for the node local to
	 * struct kmem_cache.
	 */
	struct kmem_cache_node local_node;  //本地节点的slab信息

	/* Allocation and freeing of slabs */
	 //保存slab缓冲区所需要的页框数量的order值和objects数量的值，这个是最大值
	struct kmem_cache_order_objects max; 
	//保存slab缓冲区需要的页框数量的order值和objects数量的值，这个是最小值，当默认值oo分配失败时，会尝试用最小值取分配连续页框
	struct kmem_cache_order_objects min;
	//每一次分配时所用的标志
	gfp_t allocflags;	/* gfp flags to use on each alloc */
	//缓冲区中存在的对象种类数目，因为slub允许缓存复用，因此一个缓存中可能存在多种对象类型
	int refcount;		/* Refcount for slab cache destroy */
	void (*ctor)(struct kmem_cache *, void *);
	int inuse;     /* Offset to metadata */    //元数据的偏移
	int align;		/* Alignment */  //对齐值
	const char *name;	/* Name (only for display!) */  //缓存名
	struct list_head list;	/* List of slab caches */  
#ifdef CONFIG_SLUB_DEBUG
	struct kobject kobj;	/* For sysfs */
#endif

#ifdef CONFIG_NUMA
	/*
	 * Defragmentation by allocating from a remote node.
	 */
	int remote_node_defrag_ratio;   //该值越小，越倾向于从本结点分配对象
	struct kmem_cache_node *node[MAX_NUMNODES];  //NUMA架构下每个节点对应的slab信息
#endif
#ifdef CONFIG_SMP  //注意二选一
	struct kmem_cache_cpu *cpu_slab[NR_CPUS];  //SMP系统下每个CPU对应的slab信息
#else
	struct kmem_cache_cpu cpu_slab;  //单核系统下CPU对应的信息
#endif
};

/*
 * Kmalloc subsystem.
 */
#if defined(ARCH_KMALLOC_MINALIGN) && ARCH_KMALLOC_MINALIGN > 8
#define KMALLOC_MIN_SIZE ARCH_KMALLOC_MINALIGN
#else
#define KMALLOC_MIN_SIZE 8
#endif

#define KMALLOC_SHIFT_LOW ilog2(KMALLOC_MIN_SIZE)

/*
 * We keep the general caches in an array of slab caches that are used for
 * 2^x bytes of allocations.
 */
extern struct kmem_cache kmalloc_caches[PAGE_SHIFT + 1];   

/*
 * Sorry that the following has to be that ugly but some versions of GCC
 * have trouble with constant propagation and loops.
 */
static __always_inline int kmalloc_index(size_t size)
{
	if (!size)   //大小为0，直接88
		return 0;

	//检测大小是否小于kmalloc的最小object
	if (size <= KMALLOC_MIN_SIZE)
		return KMALLOC_SHIFT_LOW;   //小于则返回最小object的对数，log

#if KMALLOC_MIN_SIZE <= 64  //如果定义的kmalloc的最小object小于64
	if (size > 64 && size <= 96)  //大于64而小于96则使用1号kmem_cache，大小就是96
		return 1;
	if (size > 128 && size <= 192)  //大于128而小于192则使用2号kmem_cache，大小就是192
		return 2;
#endif

	//以下根据大小的不同，返回对应的kmem_cache缓冲号
	if (size <=          8) return 3;
	if (size <=         16) return 4;
	if (size <=         32) return 5;
	if (size <=         64) return 6;
	if (size <=        128) return 7;
	if (size <=        256) return 8;
	if (size <=        512) return 9;
	if (size <=       1024) return 10;
	if (size <=   2 * 1024) return 11;
	if (size <=   4 * 1024) return 12;
/*
 * The following is only needed to support architectures with a larger page
 * size than 4k.
 */
 	//以下是对于分页大于4K所使用的检测
	if (size <=   8 * 1024) return 13;
	if (size <=  16 * 1024) return 14;
	if (size <=  32 * 1024) return 15;
	if (size <=  64 * 1024) return 16;
	if (size <= 128 * 1024) return 17;
	if (size <= 256 * 1024) return 18;
	if (size <= 512 * 1024) return 19;
	if (size <= 1024 * 1024) return 20;
	if (size <=  2 * 1024 * 1024) return 21;
	return -1;

/*
 * What we really wanted to do and cannot do because of compiler issues is:
 *	int i;
 *	for (i = KMALLOC_SHIFT_LOW; i <= KMALLOC_SHIFT_HIGH; i++)
 *		if (size <= (1 << i))
 *			return i;
 */
}

/*
 * Find the slab cache for a given combination of allocation flags and size.
 *
 * This ought to end up with a global pointer to the right cache
 * in kmalloc_caches.
 */
static __always_inline struct kmem_cache *kmalloc_slab(size_t size)
{
	//根据申请的大小取得对应kmem_cache缓冲的序号
	int index = kmalloc_index(size);

	if (index == 0)
		return NULL;

	//根据序号取得对应的kmem_cache缓冲
	return &kmalloc_caches[index];
}

#ifdef CONFIG_ZONE_DMA
#define SLUB_DMA __GFP_DMA
#else
/* Disable DMA functionality */
#define SLUB_DMA (__force gfp_t)0
#endif

void *kmem_cache_alloc(struct kmem_cache *, gfp_t);
void *__kmalloc(size_t size, gfp_t flags);

static __always_inline void *kmalloc_large(size_t size, gfp_t flags)
{
	return (void *)__get_free_pages(flags | __GFP_COMP, get_order(size));
}

static __always_inline void *kmalloc(size_t size, gfp_t flags)
{
	//检测size是变量还是常量，如果是常量就执行if
	if (__builtin_constant_p(size)) {
		if (size > PAGE_SIZE)  //检测申请的大小是否超过1页内存的大小
			//kmalloc_large负责超过1页内存的申请，超过1页的内存分配由伙伴系统进行，不由slub进行
			return kmalloc_large(size, flags);   //调用 large 内存分配

		if (!(flags & SLUB_DMA)) {  //如果没有设置DMA
			struct kmem_cache *s = kmalloc_slab(size);  //根据申请的大小选择对应的缓冲结构，不申请内存

			if (!s)  //检测kmem_cache是否获取成功
				return ZERO_SIZE_PTR;  //如果失败，返回空指针

			return kmem_cache_alloc(s, flags);  //如果成功，使用kmem_cache获取内存
		}
	}
	return __kmalloc(size, flags);   //变量以及DMA使用__kmalloc分配内存
}

#ifdef CONFIG_NUMA
void *__kmalloc_node(size_t size, gfp_t flags, int node);
void *kmem_cache_alloc_node(struct kmem_cache *, gfp_t flags, int node);

static __always_inline void *kmalloc_node(size_t size, gfp_t flags, int node)
{
	if (__builtin_constant_p(size) &&
		size <= PAGE_SIZE && !(flags & SLUB_DMA)) {
			struct kmem_cache *s = kmalloc_slab(size);

		if (!s)
			return ZERO_SIZE_PTR;

		return kmem_cache_alloc_node(s, flags, node);
	}
	return __kmalloc_node(size, flags, node);
}
#endif

#endif /* _LINUX_SLUB_DEF_H */
