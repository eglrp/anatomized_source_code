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
	void **freelist;	//ָ�򱾵�CPU�ĵ�һ�����ж���
	struct page *page;	/* The slab from which we are allocating */   //���������CPU��ҳ��
	int node;		/* The node of the page (or -1 for debug) */  //ҳ�������Ľڵ㣬ֵΪ-1ʱ��ʾDEBUG
	unsigned int offset;	/* Freepointer offset (in word units) */    //���ж���ָ���ƫ�ƣ����ֳ�Ϊ��λ
	unsigned int objsize;	/* Size of an object (from kmem_cache) */   //����Ĵ�С

#ifdef CONFIG_SLUB_STATS
	unsigned stat[NR_SLUB_STAT_ITEMS];   //���Լ�¼slab��״̬
#endif
};

//ʵ��ֻ��������Ա
struct kmem_cache_node {
	spinlock_t list_lock;	/* Protect partial list and nr_partial */
	unsigned long nr_partial;  //������������slab������
	struct list_head partial;

#ifdef CONFIG_SLUB_DEBUG
	atomic_long_t nr_slabs;
	atomic_long_t total_objects;
	struct list_head full;    //������ֻ����DEBUG��ʱ����ã��������Ѿ�������(��ǰ��������������:(���һ�����ô��ʱ��Ÿ㶮�����������ڲ�����...)
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
	unsigned long flags;   //chache������������ʶ
	int size;		//�����������ڴ��С�����ܴ���ʵ�ʶ���Ĵ�С
	int objsize;    //�����ʵ�ʴ�С
	int offset;      //��ſ��ж����ƫ�ƣ����ֽ�Ϊ��λ
	struct kmem_cache_order_objects oo;    //oo������ŷ����slab��ҳ��Ľ���(��16λ)��slab�ж��������(��16λ)

	/*
	 * Avoid an extra cache line for UP, SMP and for the node local to
	 * struct kmem_cache.
	 */
	struct kmem_cache_node local_node;  //���ؽڵ��slab��Ϣ

	/* Allocation and freeing of slabs */
	 //����slab����������Ҫ��ҳ��������orderֵ��objects������ֵ����������ֵ
	struct kmem_cache_order_objects max; 
	//����slab��������Ҫ��ҳ��������orderֵ��objects������ֵ���������Сֵ����Ĭ��ֵoo����ʧ��ʱ���᳢������Сֵȡ��������ҳ��
	struct kmem_cache_order_objects min;
	//ÿһ�η���ʱ���õı�־
	gfp_t allocflags;	/* gfp flags to use on each alloc */
	//�������д��ڵĶ���������Ŀ����Ϊslub�����渴�ã����һ�������п��ܴ��ڶ��ֶ�������
	int refcount;		/* Refcount for slab cache destroy */
	void (*ctor)(struct kmem_cache *, void *);
	int inuse;     /* Offset to metadata */    //Ԫ���ݵ�ƫ��
	int align;		/* Alignment */  //����ֵ
	const char *name;	/* Name (only for display!) */  //������
	struct list_head list;	/* List of slab caches */  
#ifdef CONFIG_SLUB_DEBUG
	struct kobject kobj;	/* For sysfs */
#endif

#ifdef CONFIG_NUMA
	/*
	 * Defragmentation by allocating from a remote node.
	 */
	int remote_node_defrag_ratio;   //��ֵԽС��Խ�����ڴӱ����������
	struct kmem_cache_node *node[MAX_NUMNODES];  //NUMA�ܹ���ÿ���ڵ��Ӧ��slab��Ϣ
#endif
#ifdef CONFIG_SMP  //ע���ѡһ
	struct kmem_cache_cpu *cpu_slab[NR_CPUS];  //SMPϵͳ��ÿ��CPU��Ӧ��slab��Ϣ
#else
	struct kmem_cache_cpu cpu_slab;  //����ϵͳ��CPU��Ӧ����Ϣ
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
	if (!size)   //��СΪ0��ֱ��88
		return 0;

	//����С�Ƿ�С��kmalloc����Сobject
	if (size <= KMALLOC_MIN_SIZE)
		return KMALLOC_SHIFT_LOW;   //С���򷵻���Сobject�Ķ�����log

#if KMALLOC_MIN_SIZE <= 64  //��������kmalloc����СobjectС��64
	if (size > 64 && size <= 96)  //����64��С��96��ʹ��1��kmem_cache����С����96
		return 1;
	if (size > 128 && size <= 192)  //����128��С��192��ʹ��2��kmem_cache����С����192
		return 2;
#endif

	//���¸��ݴ�С�Ĳ�ͬ�����ض�Ӧ��kmem_cache�����
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
 	//�����Ƕ��ڷ�ҳ����4K��ʹ�õļ��
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
	//��������Ĵ�Сȡ�ö�Ӧkmem_cache��������
	int index = kmalloc_index(size);

	if (index == 0)
		return NULL;

	//�������ȡ�ö�Ӧ��kmem_cache����
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
	//���size�Ǳ������ǳ���������ǳ�����ִ��if
	if (__builtin_constant_p(size)) {
		if (size > PAGE_SIZE)  //�������Ĵ�С�Ƿ񳬹�1ҳ�ڴ�Ĵ�С
			//kmalloc_large���𳬹�1ҳ�ڴ�����룬����1ҳ���ڴ�����ɻ��ϵͳ���У�����slub����
			return kmalloc_large(size, flags);   //���� large �ڴ����

		if (!(flags & SLUB_DMA)) {  //���û������DMA
			struct kmem_cache *s = kmalloc_slab(size);  //��������Ĵ�Сѡ���Ӧ�Ļ���ṹ���������ڴ�

			if (!s)  //���kmem_cache�Ƿ��ȡ�ɹ�
				return ZERO_SIZE_PTR;  //���ʧ�ܣ����ؿ�ָ��

			return kmem_cache_alloc(s, flags);  //����ɹ���ʹ��kmem_cache��ȡ�ڴ�
		}
	}
	return __kmalloc(size, flags);   //�����Լ�DMAʹ��__kmalloc�����ڴ�
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
