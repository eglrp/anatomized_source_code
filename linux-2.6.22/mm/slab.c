/*
 * linux/mm/slab.c
 * Written by Mark Hemment, 1996/97.
 * (markhe@nextd.demon.co.uk)
 *
 * kmem_cache_destroy() + some cleanup - 1999 Andrea Arcangeli
 *
 * Major cleanup, different bufctl logic, per-cpu arrays
 *	(c) 2000 Manfred Spraul
 *
 * Cleanup, make the head arrays unconditional, preparation for NUMA
 * 	(c) 2002 Manfred Spraul
 *
 * An implementation of the Slab Allocator as described in outline in;
 *	UNIX Internals: The New Frontiers by Uresh Vahalia
 *	Pub: Prentice Hall	ISBN 0-13-101908-2
 * or with a little more detail in;
 *	The Slab Allocator: An Object-Caching Kernel Memory Allocator
 *	Jeff Bonwick (Sun Microsystems).
 *	Presented at: USENIX Summer 1994 Technical Conference
 *
 * The memory is organized in caches, one cache for each object type.
 * (e.g. inode_cache, dentry_cache, buffer_head, vm_area_struct)
 * Each cache consists out of many slabs (they are small (usually one
 * page long) and always contiguous), and each slab contains multiple
 * initialized objects.
 *
 * This means, that your constructor is used only for newly allocated
 * slabs and you must pass objects with the same intializations to
 * kmem_cache_free.
 *
 * Each cache can only support one memory type (GFP_DMA, GFP_HIGHMEM,
 * normal). If you need a special memory type, then must create a new
 * cache for that memory type.
 *
 * In order to reduce fragmentation, the slabs are sorted in 3 groups:
 *   full slabs with 0 free objects
 *   partial slabs
 *   empty slabs with no allocated objects
 *
 * If partial slabs exist, then new allocations come from these slabs,
 * otherwise from empty slabs or new slabs are allocated.
 *
 * kmem_cache_destroy() CAN CRASH if you try to allocate from the cache
 * during kmem_cache_destroy(). The caller must prevent concurrent allocs.
 *
 * Each cache has a short per-cpu head array, most allocs
 * and frees go into that array, and if that array overflows, then 1/2
 * of the entries in the array are given back into the global cache.
 * The head array is strictly LIFO and should improve the cache hit rates.
 * On SMP, it additionally reduces the spinlock operations.
 *
 * The c_cpuarray may not be read with enabled local interrupts -
 * it's changed with a smp_call_function().
 *
 * SMP synchronization:
 *  constructors and destructors are called without any locking.
 *  Several members in struct kmem_cache and struct slab never change, they
 *	are accessed without any locking.
 *  The per-cpu arrays are never accessed from the wrong cpu, no locking,
 *  	and local interrupts are disabled so slab code is preempt-safe.
 *  The non-constant members are protected with a per-cache irq spinlock.
 *
 * Many thanks to Mark Hemment, who wrote another per-cpu slab patch
 * in 2000 - many ideas in the current implementation are derived from
 * his patch.
 *
 * Further notes from the original documentation:
 *
 * 11 April '97.  Started multi-threading - markhe
 *	The global cache-chain is protected by the mutex 'cache_chain_mutex'.
 *	The sem is only needed when accessing/extending the cache-chain, which
 *	can never happen inside an interrupt (kmem_cache_create(),
 *	kmem_cache_shrink() and kmem_cache_reap()).
 *
 *	At present, each engine can be growing a cache.  This should be blocked.
 *
 * 15 March 2005. NUMA slab allocator.
 *	Shai Fultheim <shai@scalex86.org>.
 *	Shobhit Dayal <shobhit@calsoftinc.com>
 *	Alok N Kataria <alokk@calsoftinc.com>
 *	Christoph Lameter <christoph@lameter.com>
 *
 *	Modified the slab allocator to be node aware on NUMA systems.
 *	Each node has its own list of partial, free and full slabs.
 *	All object allocations for a node occur from node specific slab lists.
 */

#include	<linux/slab.h>
#include	<linux/mm.h>
#include	<linux/poison.h>
#include	<linux/swap.h>
#include	<linux/cache.h>
#include	<linux/interrupt.h>
#include	<linux/init.h>
#include	<linux/compiler.h>
#include	<linux/cpuset.h>
#include	<linux/seq_file.h>
#include	<linux/notifier.h>
#include	<linux/kallsyms.h>
#include	<linux/cpu.h>
#include	<linux/sysctl.h>
#include	<linux/module.h>
#include	<linux/rcupdate.h>
#include	<linux/string.h>
#include	<linux/uaccess.h>
#include	<linux/nodemask.h>
#include	<linux/mempolicy.h>
#include	<linux/mutex.h>
#include	<linux/fault-inject.h>
#include	<linux/rtmutex.h>
#include	<linux/reciprocal_div.h>

#include	<asm/cacheflush.h>
#include	<asm/tlbflush.h>
#include	<asm/page.h>

/*
 * DEBUG	- 1 for 
 _create() to honour; SLAB_RED_ZONE & SLAB_POISON.
 *		  0 for faster, smaller code (especially in the critical paths).
 *
 * STATS	- 1 to collect stats for /proc/slabinfo.
 *		  0 for faster, smaller code (especially in the critical paths).
 *
 * FORCED_DEBUG	- 1 enables SLAB_RED_ZONE and SLAB_POISON (if possible)
 */

#ifdef CONFIG_DEBUG_SLAB
#define	DEBUG		1
#define	STATS		1
#define	FORCED_DEBUG	1
#else
#define	DEBUG		0
#define	STATS		0
#define	FORCED_DEBUG	0
#endif

/* Shouldn't this be in a header file somewhere? */
#define	BYTES_PER_WORD		sizeof(void *)
#define	REDZONE_ALIGN		max(BYTES_PER_WORD, __alignof__(unsigned long long))

#ifndef cache_line_size
#define cache_line_size()	L1_CACHE_BYTES
#endif

#ifndef ARCH_KMALLOC_MINALIGN
/*
 * Enforce a minimum alignment for the kmalloc caches.
 * Usually, the kmalloc caches are cache_line_size() aligned, except when
 * DEBUG and FORCED_DEBUG are enabled, then they are BYTES_PER_WORD aligned.
 * Some archs want to perform DMA into kmalloc caches and need a guaranteed
 * alignment larger than the alignment of a 64-bit integer.
 * ARCH_KMALLOC_MINALIGN allows that.
 * Note that increasing this value may disable some debug features.
 */
#define ARCH_KMALLOC_MINALIGN __alignof__(unsigned long long)
#endif

#ifndef ARCH_SLAB_MINALIGN
/*
 * Enforce a minimum alignment for all caches.
 * Intended for archs that get misalignment faults even for BYTES_PER_WORD
 * aligned buffers. Includes ARCH_KMALLOC_MINALIGN.
 * If possible: Do not enable this flag for CONFIG_DEBUG_SLAB, it disables
 * some debug features.
 */
#define ARCH_SLAB_MINALIGN 0
#endif

#ifndef ARCH_KMALLOC_FLAGS
#define ARCH_KMALLOC_FLAGS SLAB_HWCACHE_ALIGN
#endif

/* Legal flag mask for kmem_cache_create(). */
#if DEBUG
# define CREATE_MASK	(SLAB_RED_ZONE | \
			 SLAB_POISON | SLAB_HWCACHE_ALIGN | \
			 SLAB_CACHE_DMA | \
			 SLAB_STORE_USER | \
			 SLAB_RECLAIM_ACCOUNT | SLAB_PANIC | \
			 SLAB_DESTROY_BY_RCU | SLAB_MEM_SPREAD)
#else
# define CREATE_MASK	(SLAB_HWCACHE_ALIGN | \
			 SLAB_CACHE_DMA | \
			 SLAB_RECLAIM_ACCOUNT | SLAB_PANIC | \
			 SLAB_DESTROY_BY_RCU | SLAB_MEM_SPREAD)
#endif

/*
 * kmem_bufctl_t:
 *
 * Bufctl's are used for linking objs within a slab
 * linked offsets.
 *
 * This implementation relies on "struct page" for locating the cache &
 * slab an object belongs to.
 * This allows the bufctl structure to be small (one int), but limits
 * the number of objects a slab (not a cache) can contain when off-slab
 * bufctls are used. The limit is the size of the largest general cache
 * that does not use off-slab slabs.
 * For 32bit archs with 4 kB pages, is this 56.
 * This is not serious, as it is only for large objects, when it is unwise
 * to have too many per slab.
 * Note: This limit can be raised by introducing a general cache whose size
 * is less than 512 (PAGE_SIZE<<3), but greater than 256.
 */

typedef unsigned int kmem_bufctl_t;
#define BUFCTL_END	(((kmem_bufctl_t)(~0U))-0)
#define BUFCTL_FREE	(((kmem_bufctl_t)(~0U))-1)
#define	BUFCTL_ACTIVE	(((kmem_bufctl_t)(~0U))-2)
#define	SLAB_LIMIT	(((kmem_bufctl_t)(~0U))-3)

/*
 * struct slab
 *
 * Manages the objs in a slab. Placed either at the beginning of mem allocated
 * for a slab, or allocated from an general cache.     //�ù����߿�����slabͷ�������ڴ棬Ҳ���Դ�general cache�������ڴ档
 * Slabs are chained into three list: fully used, partial, fully free slabs.
 */
struct slab {
	struct list_head list;       //���ڽ�slab��������֮��
	unsigned long colouroff; //��slab����ɫƫ��
	void *s_mem;		//ָ��slab�еĵ�һ������
	unsigned int inuse;		//slab���ѷ����ȥ�Ķ�����Ŀ
	kmem_bufctl_t free;     //��һ�����ж�����±�
	unsigned short nodeid; //�ڵ��ʶ��
};

/*
 * struct slab_rcu
 *
 * slab_destroy on a SLAB_DESTROY_BY_RCU cache uses this structure to
 * arrange for kmem_freepages to be called via RCU.  This is useful if
 * we need to approach a kernel structure obliquely, from its address
 * obtained without the usual locking.  We can lock the structure to
 * stabilize it and check it's still at the given address, only if we
 * can be sure that the memory has not been meanwhile reused for some
 * other kind of object (which our subsystem's lock might corrupt).
 *
 * rcu_read_lock before reading the address, then rcu_read_unlock after
 * taking the spinlock within the structure expected at that address.
 *
 * We assume struct slab_rcu can overlay struct slab when destroying.
 */
struct slab_rcu {
	struct rcu_head head;
	struct kmem_cache *cachep;
	void *addr;
};

/*
 * struct array_cache
 *
 * Purpose:
 * - LIFO ordering, to hand out cache-warm objects from _alloc
 * - reduce the number of linked list operations
 * - reduce spinlock operations
 *
 * The limit is stored in the per-cpu structure to reduce the data cache
 * footprint.
 *
 */  //array_cache�ж���per-cpu���ݣ����Ṳ������Լ���NUMA�ܹ��ж�CPU������������
struct array_cache {  
	unsigned int avail;     //���ػ����п��õĿ��ж�����
	unsigned int limit;     //���ػ�����ж�����Ŀ����
	unsigned int batchcount;   //���ػ���һ����ת���ת���Ķ�������
	unsigned int touched;     //��ʶ���ض����Ƿ������ʹ��
	spinlock_t lock;       //������
	void *entry[0];	/*    //����һ���������飬���ڶԺ������ڸ��ٿ��ж����ָ������ķ���
			 * Must have this definition in here for the proper
			 * alignment of array_cache. Also simplifies accessing
			 * the entries.
			 * [0] is for gcc 2.95. It should really be [].
			 */
};

/*
 * bootstrap: The caches do not work without cpuarrays anymore, but the
 * cpuarrays are allocated from the generic caches...
 */
#define BOOT_CPUCACHE_ENTRIES	1
struct arraycache_init {
	struct array_cache cache;
	void *entries[BOOT_CPUCACHE_ENTRIES];
};

/*
 * The slab lists for all objects.
 */
struct kmem_list3 {
	struct list_head slabs_partial;//��������slab����Ҳ���ǲ��ֶ����·����ȥ��slab
	struct list_head slabs_full;    //��slab����
	struct list_head slabs_free;  //��slab����
	unsigned long free_objects;  //���ж���ĸ���
	unsigned int free_limit;        //���ж����������Ŀ
	unsigned int colour_next;	/* Per-node cache coloring */   //ÿ���ڵ���һ��slabʹ�õ���ɫ
	spinlock_t list_lock;      
	struct array_cache *shared;	/* shared per node */    //ÿ���ڵ㹲���ȥ�Ļ���
	struct array_cache **alien;	/* on other nodes */    //FIXME: �����ڵ�Ļ��棬Ӧ���ǹ����
	unsigned long next_reap;	/* updated without locking */  
	int free_touched;		/* updated without locking */
};

/*
 * Need this for bootstrapping a per node allocator.
 */
#define NUM_INIT_LISTS (2 * MAX_NUMNODES + 1)
struct kmem_list3 __initdata initkmem_list3[NUM_INIT_LISTS];
#define	CACHE_CACHE 0
#define	SIZE_AC 1
#define	SIZE_L3 (1 + MAX_NUMNODES)

static int drain_freelist(struct kmem_cache *cache,
			struct kmem_list3 *l3, int tofree);
static void free_block(struct kmem_cache *cachep, void **objpp, int len,
			int node);
static int enable_cpucache(struct kmem_cache *cachep);
static void cache_reap(struct work_struct *unused);

/*
 * This function must be completely optimized away if a constant is passed to
 * it.  Mostly the same as what is in linux/slab.h except it returns an index.
 */
static __always_inline int index_of(const size_t size) //�����������ѡ���С�ģ�����Ϊmallloc_size�Ĳ���  
{
	extern void __bad_size(void);

	if (__builtin_constant_p(size)) {
		int i = 0;

#define CACHE(x) \
	if (size <=x) \     //����һ�����㹻����size�Ĵ�С
		return i; \
	else \
		i++;   //���ɹ��������������
#include "linux/kmalloc_sizes.h"
#undef CACHE
		__bad_size();
	} else
		__bad_size();
	return 0;
}

static int slab_early_init = 1;

#define INDEX_AC index_of(sizeof(struct arraycache_init))
#define INDEX_L3 index_of(sizeof(struct kmem_list3))

#define MAKE_LIST(cachep, listp, slab, nodeid)				\
	do {								\
		INIT_LIST_HEAD(listp);					\
		list_splice(&(cachep->nodelists[nodeid]->slab), listp);	\
	} while (0)

#define	MAKE_ALL_LISTS(cachep, ptr, nodeid)				\
	do {								\
	MAKE_LIST((cachep), (&(ptr)->slabs_full), slabs_full, nodeid);	\
	MAKE_LIST((cachep), (&(ptr)->slabs_partial), slabs_partial, nodeid); \
	MAKE_LIST((cachep), (&(ptr)->slabs_free), slabs_free, nodeid);	\
	} while (0)

/*
 * struct kmem_cache
 *
 * manages a cache.
 */
/*.batchcount = 1,
	.limit = BOOT_CPUCACHE_ENTRIES,
	.shared = 1,
	.buffer_size = sizeof(struct kmem_cache),
	.name = "kmem_cache",  //�Բۣ����־ͽ� kmem

*/
//������
struct kmem_cache {
/* 1) per-cpu data, touched during every alloc/free */
	//per-cpu���ݣ����ػ��棬��¼�˱��ظ��ٻ������Ϣ��Ҳ���ڸ�������ͷŵĶ���ÿ�η�����ͷŶ��ȷ�����
	struct array_cache *array[NR_CPUS];
/* 2) Cache tunables. Protected by cache_chain_mutex */
	unsigned int batchcount;  //���ػ���ת���ת���Ĵ���������Ŀ
	unsigned int limit;            //���ػ�����ж���������Ŀ
	unsigned int shared;         //�Ƿ�֧�ֱ��ڵ㹲��һ����cache�ı�־�����֧�֣��Ǿʹ��ڱ��ع�����

	unsigned int buffer_size;   //����Ķ����С
	u32 reciprocal_buffer_size;  //���������С�ĵ�����ò�������������ţ�ٵ�������ʲô:)
/* 3) touched by every alloc & free from the backend */

	unsigned int flags;		//cache �����ñ�־
	unsigned int num;		//һ�� slab �������Ķ�����Ŀ!!! Ҳ����˵��kmem_cache ������������Ͻ�����ж����С��Ŀ����������

/* 4) cache_grow/shrink */
	/* order of pgs per slab (2^n) */
	unsigned int gfporder;  //һ��slab�������� page �Ķ�����Ҳ����һ��slab���� 2^gfporder �� page

	/* force GFP flags, e.g. GFP_DMA */
	gfp_t gfpflags;     //����ϵͳ����ʱ���ṩ�ķ����ʶ

	size_t colour;			/* cache colouring range */ //��ɫ�ķ�Χ��
	unsigned int colour_off;	/* colour offset */  //��ɫ��ƫ����
	struct kmem_cache *slabp_cache;//�����slab�������洢���ⲿ����ָ��ָ��slab�������� cache������Ϊ NULL
	unsigned int slab_size;    // slab �Ĵ�С
	unsigned int dflags;		/* dynamic flags */  //FIXME: ��̬��־

	/* constructor func */
	void (*ctor) (void *, struct kmem_cache *, unsigned long);

/* 5) cache creation/removal */
	const char *name;     //����:)
	struct list_head next;    //������������

/* 6) statistics */
#if STATS
	unsigned long num_active;
	unsigned long num_allocations;
	unsigned long high_mark;
	unsigned long grown;
	unsigned long reaped;
	unsigned long errors;
	unsigned long max_freeable;
	unsigned long node_allocs;
	unsigned long node_frees;
	unsigned long node_overflow;
	atomic_t allochit;
	atomic_t allocmiss;
	atomic_t freehit;
	atomic_t freemiss;
#endif
#if DEBUG
	/*
	 * If debugging is enabled, then the allocator can add additional
	 * fields and/or padding to every object. buffer_size contains the total
	 * object size including these internal fields, the following two
	 * variables contain the offset to the user object and its size.
	 */
	int obj_offset;
	int obj_size;
#endif
	/*
	 * We put nodelists[] at the end of kmem_cache, because we want to size
	 * this array to nr_node_ids slots instead of MAX_NUMNODES
	 * (see kmem_cache_init())
	 * We still use [MAX_NUMNODES] and not [1] or [0] because cache_cache
	 * is statically defined, so we reserve the max number of nodes.
	 */
	 //nodelists ������֯���нڵ�� slab��ÿ���ڵ�Ѱ���Լ�ӵ�е� cache ���Լ���Ϊ nodelists ���±�Ϳ��Է�����
	 //������������ʵ�ֻ��ÿ���ڵ�� slab ����� cache �Լ�ÿ���ڵ�Ĺ��� cache ��per-cpu cache �������array��������
	 //��Ȼ�����ͬһ��������kmem_cache�����������ͬһ�ֶ�������ͨ���� kmem_cache �ṹ��� nodelists ��Ա���ʵ�Ҳ��ֻ��ͬ�ֶ���� cache
	struct kmem_list3 *nodelists[MAX_NUMNODES];  
	/*
	 * Do not add fields after nodelists[]�
	 */
};

#define CFLGS_OFF_SLAB		(0x80000000UL)
#define	OFF_SLAB(x)	((x)->flags & CFLGS_OFF_SLAB)

#define BATCHREFILL_LIMIT	16
/*
 * Optimization question: fewer reaps means less probability for unnessary
 * cpucache drain/refill cycles.
 *
 * OTOH the cpuarrays can contain lots of objects,
 * which could lock up otherwise freeable slabs.
 */
#define REAPTIMEOUT_CPUC	(2*HZ)
#define REAPTIMEOUT_LIST3	(4*HZ)

#if STATS
#define	STATS_INC_ACTIVE(x)	((x)->num_active++)
#define	STATS_DEC_ACTIVE(x)	((x)->num_active--)
#define	STATS_INC_ALLOCED(x)	((x)->num_allocations++)
#define	STATS_INC_GROWN(x)	((x)->grown++)
#define	STATS_ADD_REAPED(x,y)	((x)->reaped += (y))
#define	STATS_SET_HIGH(x)						\
	do {								\
		if ((x)->num_active > (x)->high_mark)			\
			(x)->high_mark = (x)->num_active;		\
	} while (0)
#define	STATS_INC_ERR(x)	((x)->errors++)
#define	STATS_INC_NODEALLOCS(x)	((x)->node_allocs++)
#define	STATS_INC_NODEFREES(x)	((x)->node_frees++)
#define STATS_INC_ACOVERFLOW(x)   ((x)->node_overflow++)
#define	STATS_SET_FREEABLE(x, i)					\
	do {								\
		if ((x)->max_freeable < i)				\
			(x)->max_freeable = i;				\
	} while (0)
#define STATS_INC_ALLOCHIT(x)	atomic_inc(&(x)->allochit)
#define STATS_INC_ALLOCMISS(x)	atomic_inc(&(x)->allocmiss)
#define STATS_INC_FREEHIT(x)	atomic_inc(&(x)->freehit)
#define STATS_INC_FREEMISS(x)	atomic_inc(&(x)->freemiss)
#else
#define	STATS_INC_ACTIVE(x)	do { } while (0)
#define	STATS_DEC_ACTIVE(x)	do { } while (0)
#define	STATS_INC_ALLOCED(x)	do { } while (0)
#define	STATS_INC_GROWN(x)	do { } while (0)
#define	STATS_ADD_REAPED(x,y)	do { } while (0)
#define	STATS_SET_HIGH(x)	do { } while (0)
#define	STATS_INC_ERR(x)	do { } while (0)
#define	STATS_INC_NODEALLOCS(x)	do { } while (0)
#define	STATS_INC_NODEFREES(x)	do { } while (0)
#define STATS_INC_ACOVERFLOW(x)   do { } while (0)
#define	STATS_SET_FREEABLE(x, i) do { } while (0)
#define STATS_INC_ALLOCHIT(x)	do { } while (0)
#define STATS_INC_ALLOCMISS(x)	do { } while (0)
#define STATS_INC_FREEHIT(x)	do { } while (0)
#define STATS_INC_FREEMISS(x)	do { } while (0)
#endif

#if DEBUG

/*
 * memory layout of objects:
 * 0		: objp
 * 0 .. cachep->obj_offset - BYTES_PER_WORD - 1: padding. This ensures that
 * 		the end of an object is aligned with the end of the real
 * 		allocation. Catches writes behind the end of the allocation.
 * cachep->obj_offset - BYTES_PER_WORD .. cachep->obj_offset - 1:
 * 		redzone word.
 * cachep->obj_offset: The real object.
 * cachep->buffer_size - 2* BYTES_PER_WORD: redzone word [BYTES_PER_WORD long]
 * cachep->buffer_size - 1* BYTES_PER_WORD: last caller address
 *					[BYTES_PER_WORD long]
 */
static int obj_offset(struct kmem_cache *cachep)
{
	return cachep->obj_offset;
}

static int obj_size(struct kmem_cache *cachep)
{
	return cachep->obj_size;
}

static unsigned long long *dbg_redzone1(struct kmem_cache *cachep, void *objp)
{
	BUG_ON(!(cachep->flags & SLAB_RED_ZONE));
	return (unsigned long long*) (objp + obj_offset(cachep) -
				      sizeof(unsigned long long));
}

static unsigned long long *dbg_redzone2(struct kmem_cache *cachep, void *objp)
{
	BUG_ON(!(cachep->flags & SLAB_RED_ZONE));
	if (cachep->flags & SLAB_STORE_USER)
		return (unsigned long long *)(objp + cachep->buffer_size -
					      sizeof(unsigned long long) -
					      REDZONE_ALIGN);
	return (unsigned long long *) (objp + cachep->buffer_size -
				       sizeof(unsigned long long));
}

static void **dbg_userword(struct kmem_cache *cachep, void *objp)
{
	BUG_ON(!(cachep->flags & SLAB_STORE_USER));
	return (void **)(objp + cachep->buffer_size - BYTES_PER_WORD);
}

#else

#define obj_offset(x)			0
#define obj_size(cachep)		(cachep->buffer_size)
#define dbg_redzone1(cachep, objp)	({BUG(); (unsigned long long *)NULL;})
#define dbg_redzone2(cachep, objp)	({BUG(); (unsigned long long *)NULL;})
#define dbg_userword(cachep, objp)	({BUG(); (void **)NULL;})

#endif

/*
 * Do not go above this order unless 0 objects fit into the slab.
 */
#define	BREAK_GFP_ORDER_HI	1
#define	BREAK_GFP_ORDER_LO	0
static int slab_break_gfp_order = BREAK_GFP_ORDER_LO;

/*
 * Functions for storing/retrieving the cachep and or slab from the page
 * allocator.  These are used to find the slab an obj belongs to.  With kfree(),
 * these are used to find the cache which an obj belongs to.
 */
static inline void page_set_cache(struct page *page, struct kmem_cache *cache)
{
	page->lru.next = (struct list_head *)cache;
}

static inline struct kmem_cache *page_get_cache(struct page *page)
{
	page = compound_head(page);
	BUG_ON(!PageSlab(page));
	return (struct kmem_cache *)page->lru.next;
}

static inline void page_set_slab(struct page *page, struct slab *slab)
{
	page->lru.prev = (struct list_head *)slab;
}

static inline struct slab *page_get_slab(struct page *page)
{
	BUG_ON(!PageSlab(page));
	return (struct slab *)page->lru.prev;
}

static inline struct kmem_cache *virt_to_cache(const void *obj)
{
	struct page *page = virt_to_head_page(obj);
	return page_get_cache(page);
}

static inline struct slab *virt_to_slab(const void *obj)
{
	struct page *page = virt_to_head_page(obj);
	return page_get_slab(page);
}

static inline void *index_to_obj(struct kmem_cache *cache, struct slab *slab,
				 unsigned int idx)
{
	return slab->s_mem + cache->buffer_size * idx;  //�׵�ַ + n * index
}

/*
 * We want to avoid an expensive divide : (offset / cache->buffer_size)
 *   Using the fact that buffer_size is a constant for a particular cache,
 *   we can replace (offset / cache->buffer_size) by
 *   reciprocal_divide(offset, cache->reciprocal_buffer_size)
 */
static inline unsigned int obj_to_index(const struct kmem_cache *cache,
					const struct slab *slab, void *obj)
{
	//���������slab���׸������ƫ��
	u32 offset = (obj - slab->s_mem);
	//ͨ��ƫ�Ƽ������� kmem_bufctl_t �����е�����
	return reciprocal_divide(offset, cache->reciprocal_buffer_size);  //�ۣ�ţ�ٵ�����������
}

/*
 * These are the default caches for kmalloc. Custom caches can have other sizes.
 */
struct cache_sizes malloc_sizes[] = {  //ͨ�û������Ĵ�С��malloc_size�����
#define CACHE(x) { .cs_size = (x) },
#include <linux/kmalloc_sizes.h>   //������������ʲô�÷���
	CACHE(ULONG_MAX)
#undef CACHE
};
EXPORT_SYMBOL(malloc_sizes);

/* Must match cache_sizes above. Out of line to keep cache footprint low. */
struct cache_names {
	char *name;
	char *name_dma;
};

static struct cache_names __initdata cache_names[] = {
#define CACHE(x) { .name = "size-" #x, .name_dma = "size-" #x "(DMA)" },
#include <linux/kmalloc_sizes.h>
	{NULL,}
#undef CACHE
};

static struct arraycache_init initarray_cache /*__initdata*/ =
    { {0, BOOT_CPUCACHE_ENTRIES, 1, 0} };
static struct arraycache_init initarray_generic =
    { {0, BOOT_CPUCACHE_ENTRIES, 1, 0} };
 
/* internal cache of cache description objs */
//����Ǿ�̬�����˵�һ����ͨ�û�����
static struct kmem_cache cache_cache = {
	.batchcount = 1,
	.limit = BOOT_CPUCACHE_ENTRIES,
	.shared = 1,
	.buffer_size = sizeof(struct kmem_cache),
	.name = "kmem_cache",  //�Բۣ����־ͽ� kmem_cache
};

#define BAD_ALIEN_MAGIC 0x01020304ul

#ifdef CONFIG_LOCKDEP

/*
 * Slab sometimes uses the kmalloc slabs to store the slab headers
 * for other slabs "off slab".
 * The locking for this is tricky in that it nests within the locks
 * of all other slabs in a few places; to deal with this special
 * locking we put on-slab caches into a separate lock-class.
 *
 * We set lock class for alien array caches which are up during init.
 * The lock annotation will be lost if all cpus of a node goes down and
 * then comes back up during hotplug
 */
static struct lock_class_key on_slab_l3_key;
static struct lock_class_key on_slab_alc_key;

static inline void init_lock_keys(void)

{
	int q;
	struct cache_sizes *s = malloc_sizes;

	while (s->cs_size != ULONG_MAX) {
		for_each_node(q) {
			struct array_cache **alc;
			int r;
			struct kmem_list3 *l3 = s->cs_cachep->nodelists[q];
			if (!l3 || OFF_SLAB(s->cs_cachep))
				continue;
			lockdep_set_class(&l3->list_lock, &on_slab_l3_key);
			alc = l3->alien;
			/*
			 * FIXME: This check for BAD_ALIEN_MAGIC
			 * should go away when common slab code is taught to
			 * work even without alien caches.
			 * Currently, non NUMA code returns BAD_ALIEN_MAGIC
			 * for alloc_alien_cache,
			 */
			if (!alc || (unsigned long)alc == BAD_ALIEN_MAGIC)
				continue;
			for_each_node(r) {
				if (alc[r])
					lockdep_set_class(&alc[r]->lock,
					     &on_slab_alc_key);
			}
		}
		s++;
	}
}
#else
static inline void init_lock_keys(void)
{
}
#endif

/*
 * 1. Guard access to the cache-chain.
 * 2. Protect sanity of cpu_online_map against cpu hotplug events
 */
static DEFINE_MUTEX(cache_chain_mutex);
static struct list_head cache_chain;

/*
 * chicken and egg problem: delay the per-cpu array allocation
 * until the general caches are up.
 */
static enum {
	NONE,
	PARTIAL_AC,
	PARTIAL_L3,
	FULL
} g_cpucache_up;

/*
 * used by boot code to determine if it can use slab based allocator
 */
int slab_is_available(void)
{
	return g_cpucache_up == FULL;
}

static DEFINE_PER_CPU(struct delayed_work, reap_work);

static inline struct array_cache *cpu_cache_get(struct kmem_cache *cachep)
{
	return cachep->array[smp_processor_id()];
}

static inline struct kmem_cache *__find_general_cachep(size_t size,
							gfp_t gfpflags)
{
	struct cache_sizes *csizep = malloc_sizes;

#if 0
#if DEBUG
	/* This happens if someone tries to call
	 * kmem_cache_create(), or __kmalloc(), before
	 * the generic caches are initialized.
	 */
	BUG_ON(malloc_sizes[INDEX_AC].cs_cachep == NULL);
#endif
#endif

	//���Ǳ�����Ψһ���õĵط�������� cache �����ʴ�С
	//csizep ��ָ�ľ��� malloc_size ���飬�������ڲ�����һ���cache��С��ֵ����СΪLI_CACHE_BYTESΪ32�����Ϊ2^25=4096*8192(Ϊʲô��25�η�?)
	while (size > csizep->cs_size)   //�������Ǹ��ֶ���Ĵ�С��С��������ѡ��������ѡ������ʵ�
		csizep++;

	/*
	 * Really subtle: The last entry with cs->cs_size==ULONG_MAX
	 * has cs_{dma,}cachep==NULL. Thus no special case
	 * for large kmalloc calls required.
	 */
#ifdef CONFIG_ZONE_DMA
	if (unlikely(gfpflags & GFP_DMA))
		return csizep->cs_dmacachep;
#endif

	return csizep->cs_cachep;   //���ض�Ӧcache_sizes��cs_cachep��Ҳ����һ��kmem_cache��������ָ��
}

static struct kmem_cache *kmem_find_general_cachep(size_t size, gfp_t gfpflags)
{
	return __find_general_cachep(size, gfpflags);
}

static size_t slab_mgmt_size(size_t nr_objs, size_t align)
{
	return ALIGN(sizeof(struct slab)+nr_objs*sizeof(kmem_bufctl_t), align);
}

/*
 * Calculate the number of objects and left-over bytes for a given buffer size.
 */
 //����������Ŀ�Լ���Ƭ�Ĵ�С��estimate �ǹ��Ƶ���˼������������ align = cache_line_size()
static void cache_estimate(unsigned long gfporder, size_t buffer_size,
			   size_t align, int flags, size_t *left_over,
			   unsigned int *num)
{
	int nr_objs;
	size_t mgmt_size;
	
	//PAGE_SIZE����һ��ҳ�棬slab_size��¼��Ҫ���ٸ�ҳ�棬�������ʽ�ӵ�Ч��(1(ҳ��) * (2 ^ gfporder))
	size_t slab_size = PAGE_SIZE << gfporder;  //FIXME: #define PAGE_SIZE 0x400 (��1024)���ѵ�һ��ҳ���� 1K ����ô����?

	/*
	 * The slab management structure can be either off the slab or  //��off-slab��on-slab���ַ�ʽ
	 * on it. For the latter case, the memory allocated for a
	 * slab is used for:   //����ڴ汻�����洢:
	 *
	 * - The struct slab      //slab�ṹ��
	 * - One kmem_bufctl_t for each object    //ÿ�������kmem_bufctl_t
	 * - Padding to respect alignment of @align  //����Ĵ�С
	 * - @buffer_size bytes for each object   //ÿ������Ĵ�С
	 *
	 * If the slab management structure is off the slab, then the
	 * alignment will already be calculated into the size. Because   //�����off-slab��align���ѱ��������
	 * the slabs are all pages aligned, the objects will be at the   //��Ϊ���е�ҳ�������ˣ���������ʱ�ᴦ����ȷ��λ��
	 * correct alignment when allocated.
	 */
	 //��������slab��û��slab����������⣬ֱ��������ռ���Զ����С���Ƕ������
	if (flags & CFLGS_OFF_SLAB) {
		//����slab�����ڹ������ȫ�����ڴ洢slab����
		mgmt_size = 0;
		//���Զ������ = slab��С / �����С
		nr_objs = slab_size / buffer_size;    //ע��buffer_size�Ѿ���cache line�������

		//�������������
		if (nr_objs > SLAB_LIMIT)
			nr_objs = SLAB_LIMIT;
	} else {
		/*
		 * Ignore padding for the initial guess. The padding
		 * is at most @align-1 bytes, and @buffer_size is at
		 * least @align. In the worst case, this result will
		 * be one greater than the number of objects that fit
		 * into the memory allocation when taking the padding
		 * into account.
		 */
		 //����ʽslab��slab���������slab������һƬ�ڴ��У���ʱslabҳ�����:
		 //һ��struct slab ����һ��kmem_bufctl_t ��������(kmem_bufctl_t �����������slab������Ŀ��ͬ)
		//slab��С��Ҫ��ȥ��������С�����Զ������Ϊ ʣ���С / (ÿ�������С + sizeof(kmem_bufctl_t)), ������һһƥ��Ĺ�ϵ
		nr_objs = (slab_size - sizeof(struct slab)) /
			  (buffer_size + sizeof(kmem_bufctl_t));

		/*
		 * This calculated number will be either the right
		 * amount, or one greater than what we want.
		 */
		 //�������󳬹�slab �ܴ�С ����Ҫ��ȥһ������
		if (slab_mgmt_size(nr_objs, align) + nr_objs*buffer_size
		       > slab_size)
			nr_objs--;

		//�������������
		if (nr_objs > SLAB_LIMIT)
			nr_objs = SLAB_LIMIT;

		//����  ��������Ի�����  �������ܴ�С
		mgmt_size = slab_mgmt_size(nr_objs, align);
	}

	
	//�ó�slab���ն������
	*num = nr_objs;


	//ǰ���Ѿ��õ���slab��������С(����Ϊ0������Ҳ�Ѽ���)�������Ϳ������յĳ�slab�����˷ѿռ��С
	*left_over = slab_size - nr_objs*buffer_size - mgmt_size;
}

#define slab_error(cachep, msg) __slab_error(__FUNCTION__, cachep, msg)

static void __slab_error(const char *function, struct kmem_cache *cachep,
			char *msg)
{
	printk(KERN_ERR "slab error in %s(): cache `%s': %s\n",
	       function, cachep->name, msg);
	dump_stack();
}

/*
 * By default on NUMA we use alien caches to stage the freeing of
 * objects allocated from other nodes. This causes massive memory
 * inefficiencies when using fake NUMA setup to split memory into a
 * large number of small nodes, so it can be disabled on the command
 * line
  */

static int use_alien_caches __read_mostly = 1;
static int __init noaliencache_setup(char *s)
{
	use_alien_caches = 0;
	return 1;
}
__setup("noaliencache", noaliencache_setup);

#ifdef CONFIG_NUMA
/*
 * Special reaping functions for NUMA systems called from cache_reap().
 * These take care of doing round robin flushing of alien caches (containing
 * objects freed on different nodes from which they were allocated) and the
 * flushing of remote pcps by calling drain_node_pages.
 */
static DEFINE_PER_CPU(unsigned long, reap_node);

static void init_reap_node(int cpu)
{
	int node;

	node = next_node(cpu_to_node(cpu), node_online_map);
	if (node == MAX_NUMNODES)
		node = first_node(node_online_map);

	per_cpu(reap_node, cpu) = node;
}

static void next_reap_node(void)
{
	int node = __get_cpu_var(reap_node);

	node = next_node(node, node_online_map);
	if (unlikely(node >= MAX_NUMNODES))
		node = first_node(node_online_map);
	__get_cpu_var(reap_node) = node;
}

#else
#define init_reap_node(cpu) do { } while (0)
#define next_reap_node(void) do { } while (0)
#endif

/*
 * Initiate the reap timer running on the target CPU.  We run at around 1 to 2Hz
 * via the workqueue/eventd.
 * Add the CPU number into the expiration time to minimize the possibility of
 * the CPUs getting into lockstep and contending for the global cache chain
 * lock.
 */
static void __devinit start_cpu_timer(int cpu)
{
	struct delayed_work *reap_work = &per_cpu(reap_work, cpu);

	/*
	 * When this gets called from do_initcalls via cpucache_init(),
	 * init_workqueues() has already run, so keventd will be setup
	 * at that time.
	 */
	if (keventd_up() && reap_work->work.func == NULL) {
		init_reap_node(cpu);
		INIT_DELAYED_WORK(reap_work, cache_reap);
		schedule_delayed_work_on(cpu, reap_work,
					__round_jiffies_relative(HZ, cpu));
	}
}

//���䱾�ػ������
//�����ֱ���; node:NUMA�ڴ�ڵ�id 
//            entries: entry����Ԫ�ظ���
//            batchcount: ���ػ���һ����ת��ת����Ŀ�Ķ���
static struct array_cache *alloc_arraycache(int node, int entries,
					    int batchcount)
{
	//array_cache��������ŵ���entry���飬����һ�������ڴ�
	int memsize = sizeof(void *) * entries + sizeof(struct array_cache);
	struct array_cache *nc = NULL;

	//����һ�����ػ������kmalloc ��ֱ�Ӵ�ͨ�û������������
	nc = kmalloc_node(memsize, GFP_KERNEL, node);
	if (nc) {
		//��ʼ�����ػ���
		nc->avail = 0;
		nc->limit = entries;   //Ϊʲô��limit��ԭ�������ʱ���С�Ѿ�д����
		nc->batchcount = batchcount;   //��ʼ��batchcount
		nc->touched = 0;
		spin_lock_init(&nc->lock);
	}
	return nc;
}

/*
 * Transfer objects in one arraycache to another.
 * Locking must be handled by the caller.
 *
 * Return the number of entries transferred.
 */
static int transfer_objects(struct array_cache *to,
		struct array_cache *from, unsigned int max)
{
	/* Figure out how many entries to transfer */
	// min(from->avail, max) ����ѡ���from�ܸ���Ԫ����Ŀ�����Ϊmax��
	//min(..., to->limit - to->avail) ��ָ����Ԫ����Ŀ������ ac ��ǰ�������ɵ���Ŀ��
	int nr = min(min(from->avail, max), to->limit - to->avail);  

	if (!nr)   //���һ��������������ô����0��
		return 0;

	memcpy(to->entry + to->avail, from->entry + from->avail -nr,
			sizeof(void *) *nr);   //����ֱ��memcpy

	from->avail -= nr;  //���¸���ָ��
	to->avail += nr;
	to->touched = 1;
	return nr;  //���ظ���Ԫ����Ŀ
}

#ifndef CONFIG_NUMA

#define drain_alien_cache(cachep, alien) do { } while (0)
#define reap_alien(cachep, l3) do { } while (0)

static inline struct array_cache **alloc_alien_cache(int node, int limit)
{
	return (struct array_cache **)BAD_ALIEN_MAGIC;
}

static inline void free_alien_cache(struct array_cache **ac_ptr)
{
}

static inline int cache_free_alien(struct kmem_cache *cachep, void *objp)
{
	return 0;
}

static inline void *alternate_node_alloc(struct kmem_cache *cachep,
		gfp_t flags)
{
	return NULL;
}

static inline void *____cache_alloc_node(struct kmem_cache *cachep,
		 gfp_t flags, int nodeid)
{
	return NULL;
}

#else	/* CONFIG_NUMA */

static void *____cache_alloc_node(struct kmem_cache *, gfp_t, int);
static void *alternate_node_alloc(struct kmem_cache *, gfp_t);

static struct array_cache **alloc_alien_cache(int node, int limit)
{
	struct array_cache **ac_ptr;
	int memsize = sizeof(void *) * nr_node_ids;
	int i;

	if (limit > 1)
		limit = 12;
	ac_ptr = kmalloc_node(memsize, GFP_KERNEL, node);
	if (ac_ptr) {
		for_each_node(i) {
			if (i == node || !node_online(i)) {
				ac_ptr[i] = NULL;
				continue;
			}
			ac_ptr[i] = alloc_arraycache(node, limit, 0xbaadf00d);
			if (!ac_ptr[i]) {
				for (i--; i <= 0; i--)
					kfree(ac_ptr[i]);
				kfree(ac_ptr);
				return NULL;
			}
		}
	}
	return ac_ptr;
}

static void free_alien_cache(struct array_cache **ac_ptr)
{
	int i;

	if (!ac_ptr)
		return;
	for_each_node(i)
	    kfree(ac_ptr[i]);
	kfree(ac_ptr);
}

static void __drain_alien_cache(struct kmem_cache *cachep,
				struct array_cache *ac, int node)
{
	struct kmem_list3 *rl3 = cachep->nodelists[node];

	if (ac->avail) {
		spin_lock(&rl3->list_lock);
		/*
		 * Stuff objects into the remote nodes shared array first.
		 * That way we could avoid the overhead of putting the objects
		 * into the free lists and getting them back later.
		 */
		if (rl3->shared)
			transfer_objects(rl3->shared, ac, ac->limit);

		free_block(cachep, ac->entry, ac->avail, node);
		ac->avail = 0;
		spin_unlock(&rl3->list_lock);
	}
}

/*
 * Called from cache_reap() to regularly drain alien caches round robin.
 */
static void reap_alien(struct kmem_cache *cachep, struct kmem_list3 *l3)
{
	int node = __get_cpu_var(reap_node);

	if (l3->alien) {
		struct array_cache *ac = l3->alien[node];

		if (ac && ac->avail && spin_trylock_irq(&ac->lock)) {
			__drain_alien_cache(cachep, ac, node);
			spin_unlock_irq(&ac->lock);
		}
	}
}

static void drain_alien_cache(struct kmem_cache *cachep,
				struct array_cache **alien)
{
	int i = 0;
	struct array_cache *ac;
	unsigned long flags;

	for_each_online_node(i) {
		ac = alien[i];
		if (ac) {
			spin_lock_irqsave(&ac->lock, flags);
			__drain_alien_cache(cachep, ac, i);
			spin_unlock_irqrestore(&ac->lock, flags);
		}
	}
}

static inline int cache_free_alien(struct kmem_cache *cachep, void *objp)
{
	struct slab *slabp = virt_to_slab(objp);
	int nodeid = slabp->nodeid;
	struct kmem_list3 *l3;
	struct array_cache *alien = NULL;
	int node;

	node = numa_node_id();

	/*
	 * Make sure we are not freeing a object from another node to the array
	 * cache on this cpu.
	 */
	if (likely(slabp->nodeid == node))
		return 0;

	l3 = cachep->nodelists[node];
	STATS_INC_NODEFREES(cachep);
	if (l3->alien && l3->alien[nodeid]) {
		alien = l3->alien[nodeid];
		spin_lock(&alien->lock);
		if (unlikely(alien->avail == alien->limit)) {
			STATS_INC_ACOVERFLOW(cachep);
			__drain_alien_cache(cachep, alien, nodeid);
		}
		alien->entry[alien->avail++] = objp;
		spin_unlock(&alien->lock);
	} else {
		spin_lock(&(cachep->nodelists[nodeid])->list_lock);
		free_block(cachep, &objp, 1, nodeid);
		spin_unlock(&(cachep->nodelists[nodeid])->list_lock);
	}
	return 1;
}
#endif

static int __cpuinit cpuup_callback(struct notifier_block *nfb,
				    unsigned long action, void *hcpu)
{
	long cpu = (long)hcpu;
	struct kmem_cache *cachep;
	struct kmem_list3 *l3 = NULL;
	int node = cpu_to_node(cpu);
	int memsize = sizeof(struct kmem_list3);

	switch (action) {
	case CPU_LOCK_ACQUIRE:
		mutex_lock(&cache_chain_mutex);
		break;
	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
		/*
		 * We need to do this right in the beginning since
		 * alloc_arraycache's are going to use this list.
		 * kmalloc_node allows us to add the slab to the right
		 * kmem_list3 and not this cpu's kmem_list3
		 */

		list_for_each_entry(cachep, &cache_chain, next) {
			/*
			 * Set up the size64 kmemlist for cpu before we can
			 * begin anything. Make sure some other cpu on this
			 * node has not already allocated this
			 */
			if (!cachep->nodelists[node]) {
				l3 = kmalloc_node(memsize, GFP_KERNEL, node);
				if (!l3)
					goto bad;
				kmem_list3_init(l3);
				l3->next_reap = jiffies + REAPTIMEOUT_LIST3 +
				    ((unsigned long)cachep) % REAPTIMEOUT_LIST3;

				/*
				 * The l3s don't come and go as CPUs come and
				 * go.  cache_chain_mutex is sufficient
				 * protection here.
				 */
				cachep->nodelists[node] = l3;
			}

			spin_lock_irq(&cachep->nodelists[node]->list_lock);
			cachep->nodelists[node]->free_limit =
				(1 + nr_cpus_node(node)) *
				cachep->batchcount + cachep->num;
			spin_unlock_irq(&cachep->nodelists[node]->list_lock);
		}

		/*
		 * Now we can go ahead with allocating the shared arrays and
		 * array caches
		 */
		list_for_each_entry(cachep, &cache_chain, next) {
			struct array_cache *nc;
			struct array_cache *shared = NULL;
			struct array_cache **alien = NULL;

			nc = alloc_arraycache(node, cachep->limit,
						cachep->batchcount);
			if (!nc)
				goto bad;
			if (cachep->shared) {
				shared = alloc_arraycache(node,
					cachep->shared * cachep->batchcount,
					0xbaadf00d);
				if (!shared)
					goto bad;
			}
			if (use_alien_caches) {
                                alien = alloc_alien_cache(node, cachep->limit);
                                if (!alien)
                                        goto bad;
                        }
			cachep->array[cpu] = nc;
			l3 = cachep->nodelists[node];
			BUG_ON(!l3);

			spin_lock_irq(&l3->list_lock);
			if (!l3->shared) {
				/*
				 * We are serialised from CPU_DEAD or
				 * CPU_UP_CANCELLED by the cpucontrol lock
				 */
				l3->shared = shared;
				shared = NULL;
			}
#ifdef CONFIG_NUMA
			if (!l3->alien) {
				l3->alien = alien;
				alien = NULL;
			}
#endif
			spin_unlock_irq(&l3->list_lock);
			kfree(shared);
			free_alien_cache(alien);
		}
		break;
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		start_cpu_timer(cpu);
		break;
#ifdef CONFIG_HOTPLUG_CPU
  	case CPU_DOWN_PREPARE:
  	case CPU_DOWN_PREPARE_FROZEN:
		/*
		 * Shutdown cache reaper. Note that the cache_chain_mutex is
		 * held so that if cache_reap() is invoked it cannot do
		 * anything expensive but will only modify reap_work
		 * and reschedule the timer.
		*/
		cancel_rearming_delayed_work(&per_cpu(reap_work, cpu));
		/* Now the cache_reaper is guaranteed to be not running. */
		per_cpu(reap_work, cpu).work.func = NULL;
  		break;
  	case CPU_DOWN_FAILED:
  	case CPU_DOWN_FAILED_FROZEN:
		start_cpu_timer(cpu);
  		break;
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
		/*
		 * Even if all the cpus of a node are down, we don't free the
		 * kmem_list3 of any cache. This to avoid a race between
		 * cpu_down, and a kmalloc allocation from another cpu for
		 * memory from the node of the cpu going down.  The list3
		 * structure is usually allocated from kmem_cache_create() and
		 * gets destroyed at kmem_cache_destroy().
		 */
		/* fall thru */
#endif
	case CPU_UP_CANCELED:
	case CPU_UP_CANCELED_FROZEN:
		list_for_each_entry(cachep, &cache_chain, next) {
			struct array_cache *nc;
			struct array_cache *shared;
			struct array_cache **alien;
			cpumask_t mask;

			mask = node_to_cpumask(node);
			/* cpu is dead; no one can alloc from it. */
			nc = cachep->array[cpu];
			cachep->array[cpu] = NULL;
			l3 = cachep->nodelists[node];

			if (!l3)
				goto free_array_cache;

			spin_lock_irq(&l3->list_lock);

			/* Free limit for this kmem_list3 */
			l3->free_limit -= cachep->batchcount;
			if (nc)
				free_block(cachep, nc->entry, nc->avail, node);

			if (!cpus_empty(mask)) {
				spin_unlock_irq(&l3->list_lock);
				goto free_array_cache;
			}

			shared = l3->shared;
			if (shared) {
				free_block(cachep, shared->entry,
					   shared->avail, node);
				l3->shared = NULL;
			}

			alien = l3->alien;
			l3->alien = NULL;

			spin_unlock_irq(&l3->list_lock);

			kfree(shared);
			if (alien) {
				drain_alien_cache(cachep, alien);
				free_alien_cache(alien);
			}
free_array_cache:
			kfree(nc);
		}
		/*
		 * In the previous loop, all the objects were freed to
		 * the respective cache's slabs,  now we can go ahead and
		 * shrink each nodelist to its limit.
		 */
		list_for_each_entry(cachep, &cache_chain, next) {
			l3 = cachep->nodelists[node];
			if (!l3)
				continue;
			drain_freelist(cachep, l3, l3->free_objects);
		}
		break;
	case CPU_LOCK_RELEASE:
		mutex_unlock(&cache_chain_mutex);
		break;
	}
	return NOTIFY_OK;
bad:
	return NOTIFY_BAD;
}

static struct notifier_block __cpuinitdata cpucache_notifier = {
	&cpuup_callback, NULL, 0
};

/*
 * swap the static kmem_list3 with kmalloced memory
 */
static void init_list(struct kmem_cache *cachep, struct kmem_list3 *list,
			int nodeid)
{
	struct kmem_list3 *ptr;

	ptr = kmalloc_node(sizeof(struct kmem_list3), GFP_KERNEL, nodeid);
	BUG_ON(!ptr);

	local_irq_disable();
	memcpy(ptr, list, sizeof(struct kmem_list3));
	/*
	 * Do not assume that spinlocks can be initialized via memcpy:
	 */
	spin_lock_init(&ptr->list_lock);

	MAKE_ALL_LISTS(cachep, ptr, nodeid);
	cachep->nodelists[nodeid] = ptr;
	local_irq_enable();
}

/*
 * Initialisation.  Called after the page allocator have been initialised and
 * before smp_init().
 */
 //ͨ�û�������ʼ�����������������޷�ʹ��kmalloc
void __init kmem_cache_init(void)
{
	size_t left_over;
	struct cache_sizes *sizes;
	struct cache_names *names;
	int i;
	int order;
	int node;

	if (num_possible_nodes() == 1)  //ò�Ƶ�ǰ����汾����֧�ֲμ�#define num_possible_nodes()	1
		use_alien_caches = 0;             //����linux�汾��֧��
		
	//��slab��ʼ����֮ǰ���޷�ͨ��kmalloc�����ʼ�������е�һЩ��Ҫ����ֻ��ʹ�þ�̬��ȫ�ֱ���
	//��slab��ʼ�����ڣ���ʹ��kmalloc��̬��������滻ȫ�ֱ���!!
	
	//��ǰ�������Ƚ��� initkem_list3 ����slab��������ÿ���ڵ��Ӧһ������
	//initkmem_list3 �Ǹ�slab�������飬����ѭ����ʼ��ÿ���ڵ������ 
	for (i = 0; i < NUM_INIT_LISTS; i++) {       //NUM_INIT_LIST ����ô��ģ�Ϊʲô����2 * MAX_NODES+1 ????????
		kmem_list3_init(&initkmem_list3[i]);
		if (i < MAX_NUMNODES)
		//ȫ�ֱ���cache_cacheָ���slab cache�������е�kmem_cache���󣬲�����cache_cache����
		//�����ʼ�������ڴ�ڵ�kmem_cache��slab����Ϊ��
			cache_cache.nodelists[i] = NULL;
	}
	
	/*
	 * Fragmentation resistance on low memory - only use bigger
	 * page orders on machines with more than 32MB of memory.
	 */
	 //ȫ������slab_break_gfp_orderΪÿ��slab���ռ�ü���ҳ�棬����������Ƭ��
	 //�ܽ�������˼������: (1)�����������ڴ����32MB��Ҳ���ǿ����ڴ��ԣ��ʱ��BREAK_GFP_ORDER_HI������ֵ��1��
	 //���ʱ��ÿ��slab���ռ������ҳ�棬������ʱ���ܺ��3��ҳ�棬���Ƕ����С����8192Kʱ�ſ���(һ��ҳ���С��4K��Ҳ����4096);
	 //(2)��������ڴ治����32MB,��ôBREAK_GFP_ORDER_HIֵΪ0�����Ҳ��������һ��ҳ�棬���Ƕ����ˣ������ܺ��
	if (num_physpages > (32 << 20) >> PAGE_SHIFT)
		slab_break_gfp_order = BREAK_GFP_ORDER_HI;   //����ȷ��slab������С

	/* Bootstrap is tricky, because several objects are allocated
	 * from caches that do not exist yet:
	 * 1) initialize the cache_cache cache: it contains the struct
	 *    kmem_cache structures of all caches, except cache_cache itself:
	 *    cache_cache is statically allocated.
	 *    Initially an __init data area is used for the head array and the
	 *    kmem_list3 structures, it's replaced with a kmalloc allocated
	 *    array at the end of the bootstrap.
	 * 2) Create the first kmalloc cache.
	 *    The struct kmem_cache for the new cache is allocated normally.
	 *    An __init data area is used for the head array.
	 * 3) Create the remaining kmalloc caches, with minimally sized
	 *    head arrays.
	 * 4) Replace the __init data head arrays for cache_cache and the first
	 *    kmalloc cache with kmalloc allocated arrays.
	 * 5) Replace the __init data for kmem_list3 for cache_cache and
	 *    the other cache's with kmalloc allocated memory.
	 * 6) Resize the head arrays of the kmalloc caches to their final sizes.
	 */

	node = numa_node_id();   //��ȡ�ڵ�id,ȡ�õ�ֵΪ0

	/* 1) create the cache_cache */
	//��ʼ��cache_chain Ϊ kmem_cache ����ͷ��
	INIT_LIST_HEAD(&cache_chain);
	list_add(&cache_cache.next, &cache_chain);
	//����cache��ɫ��ƫ��������ֵ��Ҳ����L1�����еĴ�С
	cache_cache.colour_off = cache_line_size(); //�궨��L1�����еĴ�С #define cache_line_size() L1_CACHE_BYTES
	//��ʼ��cache_cache��per-CPU cache��ͬ������Ҳ����ʹ��kmalloc����Ҫʹ�þ�̬�����ȫ�ֱ���initarray_cache
	cache_cache.array[smp_processor_id()] = &initarray_cache.cache;
	//��ʼ��slab������ȫ�ֱ���������CACHE_CACHEֵΪ0������Ϊcache_cache����ϵͳ��һ���Ļ�����
	cache_cache.nodelists[node] = &initkmem_list3[CACHE_CACHE];

	/*
	 * struct kmem_cache size depends on nr_node_ids, which
	 * can be less than MAX_NUMNODES.
	 */
	 //buffer_sizeԭָ��������Ķ����С������cache_cache�������� kmem_cache �ķ������ģ����� buffer_size �Ĵ�С���� kmem_cache �Ĵ�С
	 //ע����������ļ��㷽����nodelists��ռ�� kmem_cache�Ĵ�С������Ҫ�ֿ����㣬����ע��nodelists������UMA�ܹ�ֻ��һ���ڵ㣬����ֻ��1��kmem_list3��ָ��
	cache_cache.buffer_size = offsetof(struct kmem_cache, nodelists) +
				 nr_node_ids * sizeof(struct kmem_list3 *);
#if 0 
#if DEBUG
	cache_cache.obj_size = cache_cache.buffer_size;
#endif
#endif

	//ע��������һ�μ����� buffer_size �Ĵ�С��ͨ����μ��㽫buffer_size�� ������ Ϊ��λ�����ϱ߽����
	//�������Ķ�����cache line�Ĵ�С�����Ĵ�С
	cache_cache.buffer_size = ALIGN(cache_cache.buffer_size,
					cache_line_size());
	//��������С�ĵ��������ڼ��������slab�е�����
	cache_cache.reciprocal_buffer_size =
		reciprocal_value(cache_cache.buffer_size);

	//����cache_cache��ʣ��ռ��Լ�slab�ж������Ŀ��order������slab�Ĵ�С(PAGE_SIZEE<<order)
	for (order = 0; order < MAX_ORDER; order++) { //#define MAX_ORDER 11
		
		cache_estimate(order, cache_cache.buffer_size,  //buffer_size�Ѿ���cache line�����
			cache_line_size(), 0, &left_over, &cache_cache.num);  //����cache_cache�Ķ�����Ŀ
			
		if (cache_cache.num) //��С�����ԣ��ܻ��ҵ��ʺϻ�������С��ҳ���С����ʱ��������������ɷ���������Ŀ����ʱnum�Ͳ�Ϊ0
			break;
	}
	BUG_ON(!cache_cache.num);  //����
	
	cache_cache.gfporder = order;  //gfporder��ʾ��slab����2^gfproder��ҳ�棬ע������ʹ������order��ֵ�� gfporder

	//cache_cache��colour_off ���������ʼ���� L1_CACHE_BYTES����Ȼ�Ѿ�֪�� L1 �����еĴ�С�������ϲ��ּ�������˷ѿռ�Ĵ�С
	//��ô���˷ѿռ�Ĵ�С / L1 �����еĴ�С���͵ó���ǰ���� colour ����Ŀ�������Ŀ���ۼ���ѭ���ģ�����Ϊ0
	cache_cache.colour = left_over / cache_cache.colour_off;   //ȷ������ colour ����Ŀ����λ�� colour_off


	//ȷ��slab�������Լ�kmem_bufctl_t���� ��Ի����ж����Ĵ�С
	cache_cache.slab_size = ALIGN(cache_cache.num * sizeof(kmem_bufctl_t) +  
				      sizeof(struct slab), cache_line_size());


	/* 2+3) create the kmalloc caches */
	sizes = malloc_sizes;   //malloc_sizes���鱣����Ҫ����Ĵ�С
	names = cache_names;  //cache_name����cache��

	/*
	 * Initialize the caches that provide memory for the array cache and the
	 * kmem_list3 structures first.  Without this, further allocations will
	 * bug.
	 */
	//���ȴ���struct array_cache_init �� struct kmem_list3 ���õ�ͨ�û�����general cache�������Ǻ�����ʼ�������Ļ���
	//INDEX_AC�Ǽ���local cache���õ�struct arraycache_init������kmalloc size�е���������������һ�����С��general cache�������˴�С�����cacheΪlocal cache����
	sizes[INDEX_AC].cs_cachep = kmem_cache_create(names[INDEX_AC].name,
					sizes[INDEX_AC].cs_size,
					ARCH_KMALLOC_MINALIGN,
					ARCH_KMALLOC_FLAGS|SLAB_PANIC,   //#define ARCH_KMALLOC_FLAGS SLAB_HWCACHE_ALIGN���Ѿ�������ı��
					NULL, NULL);

	if (INDEX_AC != INDEX_L3) {
	//���struct kmem_list3 �� struct arraycache_init��Ӧ��kmalloc size������ͬ������С���ڲ�ͬ�ļ���
	//�򴴽�struct kmem_list3���õ�cache��������һ��cache
		sizes[INDEX_L3].cs_cachep =
			kmem_cache_create(names[INDEX_L3].name,
				sizes[INDEX_L3].cs_size,
				ARCH_KMALLOC_MINALIGN,
				ARCH_KMALLOC_FLAGS|SLAB_PANIC,
				NULL, NULL);
	}

	slab_early_init = 0;//��������������ͨ�û�������slab early init�׶ν������ڴ�֮ǰ��������������ʽslab

	//sizes->cs_size ��ֵΪ��malloc_sizes[0]��ֵӦ���Ǵ�32��ʼ
	while (sizes->cs_size != ULONG_MAX) {  //ѭ������kmalloc�������ͨ�û�������ULONG_MAX �����ֵ��
		/*
		 * For performance, all the general caches are L1 aligned.
		 * This should be particularly beneficial on SMP boxes, as it
		 * eliminates(����) "false sharing".
		 * Note for systems short on memory removing the alignment will
		 * allow tighter(����) packing of the smaller caches.
		 */
		if (!sizes->cs_cachep) {   
			sizes->cs_cachep = kmem_cache_create(names->name,
					sizes->cs_size,
					ARCH_KMALLOC_MINALIGN,
					ARCH_KMALLOC_FLAGS|SLAB_PANIC,
					NULL, NULL);
		}
#ifdef CONFIG_ZONE_DMA   //�������DMA����ôΪÿ��kmem_cache ����������һ��DMA��һ������
		sizes->cs_dmacachep = kmem_cache_create(
					names->name_dma,
					sizes->cs_size,
					ARCH_KMALLOC_MINALIGN,
					ARCH_KMALLOC_FLAGS|SLAB_CACHE_DMA|
						SLAB_PANIC,
					NULL, NULL);
#endif
		sizes++;   //������������ֱ��++������ѭ����������С������������С��general caches�����ΪULONG_MAX
		names++;
	}
	/* 4) Replace the bootstrap head arrays */
	{
		struct array_cache *ptr;

		//����Ҫ����arraycache�滻֮ǰ��initarray_cache
		ptr = kmalloc(sizeof(struct arraycache_init), GFP_KERNEL);  //GFP_KERNEL ��˯������

		//���ж�
		local_irq_disable();
		BUG_ON(cpu_cache_get(&cache_cache) != &initarray_cache.cache);
		memcpy(ptr, cpu_cache_get(&cache_cache),
		       sizeof(struct arraycache_init));  //��cache_cache��per-cpu��Ӧ��array_cache������ptr
		/*
		 * Do not assume that spinlocks can be initialized via memcpy:
		 */
		spin_lock_init(&ptr->lock);

		cache_cache.array[smp_processor_id()] = ptr;  //������ָ��ptr?
		local_irq_enable();

		ptr = kmalloc(sizeof(struct arraycache_init), GFP_KERNEL);

		local_irq_disable();
		BUG_ON(cpu_cache_get(malloc_sizes[INDEX_AC].cs_cachep)
		       != &initarray_generic.cache);
		memcpy(ptr, cpu_cache_get(malloc_sizes[INDEX_AC].cs_cachep),
		       sizeof(struct arraycache_init));
		/*
		 * Do not assume that spinlocks can be initialized via memcpy:
		 */
		spin_lock_init(&ptr->lock);

		malloc_sizes[INDEX_AC].cs_cachep->array[smp_processor_id()] =
		    ptr;
		local_irq_enable();
	}
	/* 5) Replace the bootstrap kmem_list3's */
	{
		int nid;

		/* Replace the static kmem_list3 structures for the boot cpu */
		init_list(&cache_cache, &initkmem_list3[CACHE_CACHE], node);

		for_each_online_node(nid) {
			init_list(malloc_sizes[INDEX_AC].cs_cachep,
				  &initkmem_list3[SIZE_AC + nid], nid);

			if (INDEX_AC != INDEX_L3) {
				init_list(malloc_sizes[INDEX_L3].cs_cachep,
					  &initkmem_list3[SIZE_L3 + nid], nid);
			}
		}
	}

	/* 6) resize the head arrays to their final sizes */
	{
		struct kmem_cache *cachep;
		mutex_lock(&cache_chain_mutex);
		list_for_each_entry(cachep, &cache_chain, next)
			if (enable_cpucache(cachep))
				BUG();
		mutex_unlock(&cache_chain_mutex);
	}

	/* Annotate slab for lockdep -- annotate the malloc caches */
	init_lock_keys();


	/* Done! */
	g_cpucache_up = FULL;

	/*
	 * Register a cpu startup notifier callback that initializes
	 * cpu_cache_get for all new cpus
	 */
	 //ע��cpu up�ص�������cpu����ʱ���ñ��ػ���
	register_cpu_notifier(&cpucache_notifier);

	/*
	 * The reap timers are started later, with a module init call: That part
	 * of the kernel is not yet operational.
	 */
}

static int __init cpucache_init(void)
{
	int cpu;

	/*
	 * Register the timers that return unneeded pages to the page allocator
	 */
	for_each_online_cpu(cpu)
		start_cpu_timer(cpu);
	return 0;
}
__initcall(cpucache_init);

/*
 * Interface to system's page allocator. No need to hold the cache-lock.
 *
 * If we requested dmaable memory, we will get it. Even if we
 * did not request dmaable memory, we might get it, but that
 * would be relatively rare and ignorable.
 */
static void *kmem_getpages(struct kmem_cache *cachep, gfp_t flags, int nodeid)
{
	struct page *page;
	int nr_pages;
	int i;

#ifndef CONFIG_MMU
	/*
	 * Nommu uses slab's for process anonymous memory allocations, and thus
	 * requires __GFP_COMP to properly refcount higher order allocations
	 */
	flags |= __GFP_COMP;
#endif

	flags |= cachep->gfpflags;

	//��buddy��ȡ����ҳ����С��cachep->gfporder����(2^cachep->gfporder)
	page = alloc_pages_node(nodeid, flags, cachep->gfporder);
	if (!page)
		return NULL;
	//�����Ҫ��ȡ������ҳ����(2^cachep->gfporder)
	nr_pages = (1 << cachep->gfporder);
	
	//����ҳ��״̬(�Ƿ�ɻ���)����vmstat������
	if (cachep->flags & SLAB_RECLAIM_ACCOUNT)
		add_zone_page_state(page_zone(page),
			NR_SLAB_RECLAIMABLE, nr_pages);
	else
		add_zone_page_state(page_zone(page),
			NR_SLAB_UNRECLAIMABLE, nr_pages);

	//����Щ����ҳ��������Ϊslab
	for (i = 0; i < nr_pages; i++)
		__SetPageSlab(page + i);
	return page_address(page);
}

/*
 * Interface to system's page release.
 */
static void kmem_freepages(struct kmem_cache *cachep, void *addr)
{
	unsigned long i = (1 << cachep->gfporder);
	//�������ַת��Ϊ��ҳ�������ṹ struct page* 
	struct page *page = virt_to_page(addr);
	const unsigned long nr_freed = i;

	if (cachep->flags & SLAB_RECLAIM_ACCOUNT)
		sub_zone_page_state(page_zone(page),
				NR_SLAB_RECLAIMABLE, nr_freed);
	else
		sub_zone_page_state(page_zone(page),
				NR_SLAB_UNRECLAIMABLE, nr_freed);
	while (i--) {
		BUG_ON(!PageSlab(page));
		__ClearPageSlab(page);
		page++;
	}
	if (current->reclaim_state)
		current->reclaim_state->reclaimed_slab += nr_freed;
	free_pages((unsigned long)addr, cachep->gfporder);
}

static void kmem_rcu_free(struct rcu_head *head)
{
	struct slab_rcu *slab_rcu = (struct slab_rcu *)head;
	struct kmem_cache *cachep = slab_rcu->cachep;

	kmem_freepages(cachep, slab_rcu->addr);
	if (OFF_SLAB(cachep))
		kmem_cache_free(cachep->slabp_cache, slab_rcu);
}

#if DEBUG

#ifdef CONFIG_DEBUG_PAGEALLOC
static void store_stackinfo(struct kmem_cache *cachep, unsigned long *addr,
			    unsigned long caller)
{
	int size = obj_size(cachep);

	addr = (unsigned long *)&((char *)addr)[obj_offset(cachep)];

	if (size < 5 * sizeof(unsigned long))
		return;

	*addr++ = 0x12345678;
	*addr++ = caller;
	*addr++ = smp_processor_id();
	size -= 3 * sizeof(unsigned long);
	{
		unsigned long *sptr = &caller;
		unsigned long svalue;

		while (!kstack_end(sptr)) {
			svalue = *sptr++;
			if (kernel_text_address(svalue)) {
				*addr++ = svalue;
				size -= sizeof(unsigned long);
				if (size <= sizeof(unsigned long))
					break;
			}
		}

	}
	*addr++ = 0x87654321;
}
#endif

static void poison_obj(struct kmem_cache *cachep, void *addr, unsigned char val)
{
	int size = obj_size(cachep);
	addr = &((char *)addr)[obj_offset(cachep)];

	memset(addr, val, size);
	*(unsigned char *)(addr + size - 1) = POISON_END;
}

static void dump_line(char *data, int offset, int limit)
{
	int i;
	unsigned char error = 0;
	int bad_count = 0;

	printk(KERN_ERR "%03x:", offset);
	for (i = 0; i < limit; i++) {
		if (data[offset + i] != POISON_FREE) {
			error = data[offset + i];
			bad_count++;
		}
		printk(" %02x", (unsigned char)data[offset + i]);
	}
	printk("\n");

	if (bad_count == 1) {
		error ^= POISON_FREE;
		if (!(error & (error - 1))) {
			printk(KERN_ERR "Single bit error detected. Probably "
					"bad RAM.\n");
#ifdef CONFIG_X86
			printk(KERN_ERR "Run memtest86+ or a similar memory "
					"test tool.\n");
#else
			printk(KERN_ERR "Run a memory test tool.\n");
#endif
		}
	}
}
#endif

#if DEBUG

static void print_objinfo(struct kmem_cache *cachep, void *objp, int lines)
{
	int i, size;
	char *realobj;

	if (cachep->flags & SLAB_RED_ZONE) {
		printk(KERN_ERR "Redzone: 0x%llx/0x%llx.\n",
			*dbg_redzone1(cachep, objp),
			*dbg_redzone2(cachep, objp));
	}

	if (cachep->flags & SLAB_STORE_USER) {
		printk(KERN_ERR "Last user: [<%p>]",
			*dbg_userword(cachep, objp));
		print_symbol("(%s)",
				(unsigned long)*dbg_userword(cachep, objp));
		printk("\n");
	}
	realobj = (char *)objp + obj_offset(cachep);
	size = obj_size(cachep);
	for (i = 0; i < size && lines; i += 16, lines--) {
		int limit;
		limit = 16;
		if (i + limit > size)
			limit = size - i;
		dump_line(realobj, i, limit);
	}
}

static void check_poison_obj(struct kmem_cache *cachep, void *objp)
{
	char *realobj;
	int size, i;
	int lines = 0;

	realobj = (char *)objp + obj_offset(cachep);
	size = obj_size(cachep);

	for (i = 0; i < size; i++) {
		char exp = POISON_FREE;
		if (i == size - 1)
			exp = POISON_END;
		if (realobj[i] != exp) {
			int limit;
			/* Mismatch ! */
			/* Print header */
			if (lines == 0) {
				printk(KERN_ERR
					"Slab corruption: %s start=%p, len=%d\n",
					cachep->name, realobj, size);
				print_objinfo(cachep, objp, 0);
			}
			/* Hexdump the affected line */
			i = (i / 16) * 16;
			limit = 16;
			if (i + limit > size)
				limit = size - i;
			dump_line(realobj, i, limit);
			i += 16;
			lines++;
			/* Limit to 5 lines */
			if (lines > 5)
				break;
		}
	}
	if (lines != 0) {
		/* Print some data about the neighboring objects, if they
		 * exist:
		 */
		struct slab *slabp = virt_to_slab(objp);
		unsigned int objnr;

		objnr = obj_to_index(cachep, slabp, objp);
		if (objnr) {
			objp = index_to_obj(cachep, slabp, objnr - 1);
			realobj = (char *)objp + obj_offset(cachep);
			printk(KERN_ERR "Prev obj: start=%p, len=%d\n",
			       realobj, size);
			print_objinfo(cachep, objp, 2);
		}
		if (objnr + 1 < cachep->num) {
			objp = index_to_obj(cachep, slabp, objnr + 1);
			realobj = (char *)objp + obj_offset(cachep);
			printk(KERN_ERR "Next obj: start=%p, len=%d\n",
			       realobj, size);
			print_objinfo(cachep, objp, 2);
		}
	}
}
#endif

#if DEBUG
/**
 * slab_destroy_objs - destroy a slab and its objects
 * @cachep: cache pointer being destroyed
 * @slabp: slab pointer being destroyed
 *
 * Call the registered destructor for each object in a slab that is being
 * destroyed.
 */
static void slab_destroy_objs(struct kmem_cache *cachep, struct slab *slabp)
{
	int i;
	for (i = 0; i < cachep->num; i++) {
		void *objp = index_to_obj(cachep, slabp, i);

		if (cachep->flags & SLAB_POISON) {
#ifdef CONFIG_DEBUG_PAGEALLOC
			if (cachep->buffer_size % PAGE_SIZE == 0 &&
					OFF_SLAB(cachep))
				kernel_map_pages(virt_to_page(objp),
					cachep->buffer_size / PAGE_SIZE, 1);
			else
				check_poison_obj(cachep, objp);
#else
			check_poison_obj(cachep, objp);
#endif
		}
		if (cachep->flags & SLAB_RED_ZONE) {
			if (*dbg_redzone1(cachep, objp) != RED_INACTIVE)
				slab_error(cachep, "start of a freed object "
					   "was overwritten");
			if (*dbg_redzone2(cachep, objp) != RED_INACTIVE)
				slab_error(cachep, "end of a freed object "
					   "was overwritten");
		}
	}
}
#else
static void slab_destroy_objs(struct kmem_cache *cachep, struct slab *slabp)
{
}
#endif

/**
 * slab_destroy - destroy and release all objects in a slab
 * @cachep: cache pointer being destroyed   
 * @slabp: slab pointer being destroyed      
 *
 * Destroy all the objs in a slab, and release the mem back to the system.
 * Before calling the slab must have been unlinked from the cache.  The
 * cache-lock is not held/needed. 
 */
 //����slab����Ҫ�ͷŹ�������slab����
static void slab_destroy(struct kmem_cache *cachep, struct slab *slabp)
{
	//���slabҳ����׵�ַ�����õ�һ������ĵ�ַcolouroff(��������ʽslab,colouroff�Ѿ���slab�����߰���������)
	void *addr = slabp->s_mem - slabp->colouroff;
	//debug��
	slab_destroy_objs(cachep, slabp);

	//ʹ��SLAB_DESTROY_BY_RCU�������ĸ��ٻ���
	if (unlikely(cachep->flags & SLAB_DESTROY_BY_RCU)) {
		//rcu��ʽ�ͷ�,��ʱ������������Ҫ���������Ż�
		struct slab_rcu *slab_rcu;

		slab_rcu = (struct slab_rcu *)slabp;
		slab_rcu->cachep = cachep;
		slab_rcu->addr = addr;
		//ע��һ���ص��������ͷ�slab
		call_rcu(&slab_rcu->head, kmem_rcu_free);
	} else {
		//�ͷ�slabռ�õ�ҳ�浽���ϵͳ��
		//���������ʽ��slab�����ߺ�slab������һ�𣬿���ͬʱ�ͷ�
		kmem_freepages(cachep, addr);
		if (OFF_SLAB(cachep))
			//����ʽ�������ͷ�slab�������
			kmem_cache_free(cachep->slabp_cache, slabp);
	}
}

/*
 * For setting up all the kmem_list3s for cache whose buffer_size is same as
 * size of kmem_list3.
 */
static void __init set_up_list3s(struct kmem_cache *cachep, int index)
{
	int node;

	for_each_online_node(node) {
		cachep->nodelists[node] = &initkmem_list3[index + node];
		cachep->nodelists[node]->next_reap = jiffies +
		    REAPTIMEOUT_LIST3 +
		    ((unsigned long)cachep) % REAPTIMEOUT_LIST3;
	}
}

//�����������ٺܼ򵥣����μ����ͷű���CPU���棬���ع����������Լ�����������
//�ú���ͨ��ֻ������ж��module(ģ��)��ʱ��
static void __kmem_cache_destroy(struct kmem_cache *cachep)
{
	int i;
	struct kmem_list3 *l3;

	//�ͷ�ÿ��CPU���ػ��棬ע���ʱCPU�� online ����״̬�������down״̬����û���ͷš�( �������޷��ͷŸе�����:) )
	for_each_online_cpu(i)   // online
	    kfree(cachep->array[i]);

	/* NUMA: free the list3 structures */
	for_each_online_node(i) {  //��ÿ�����ߵĽڵ�
		l3 = cachep->nodelists[i];
		if (l3) {
			//�ͷű��ع�����ʹ�õ�array_cache����
			kfree(l3->shared);
			free_alien_cache(l3->alien);
			kfree(l3);  //�ͷ�����
		}
	}
	//�ͷŻ���������Ϊ������������ cache_cache �Ķ������Ե��ö����ͷź������ú����ͷ�slab֮ǰ�������ĳ������
	kmem_cache_free(&cache_cache, cachep);
}


/**
 * calculate_slab_order - calculate size (page order) of slabs
 * @cachep: pointer to the cache that is being created
 * @size: size of objects to be created in this cache.
 * @align: required alignment for the objects.
 * @flags: slab allocation flags
 *
 * Also calculates the number of objects per slab.
 *
 * This could be made much more intelligent.  For now, try to avoid using
 * high order pages for slabs.  When the gfp() functions are more friendly
 * towards high-order requests, this should be changed.
 */
 //�����slab��order������slab���ɼ���ҳ�湹��
static size_t calculate_slab_order(struct kmem_cache *cachep,
			size_t size, size_t align, unsigned long flags)
{
	unsigned long offslab_limit;
	size_t left_over = 0;
	int gfporder;

	//���ȴ�orderΪ0��ʼ��֪�����order(KMALLOC_MAX_ORDER)Ϊֹ
	for (gfporder = 0; gfporder <= KMALLOC_MAX_ORDER; gfporder++) {
		unsigned int num;
		size_t remainder;

		//ͨ��cache���㺯���������order�Ų���һ��size��С�Ķ��󣬼�numΪ0����ʾorder̫С����Ҫ������
		//����: gfproder: slab��СΪ2^gfporder ��ҳ��
		//buffer_size: �����С
		//align: ����Ķ��뷽ʽ
		//flags: ������slab��������slab
		//remainder: slab���˷ѵĿռ���Ƭ�Ƕ���
		//num: slab�ж���ĸ���
		cache_estimate(gfporder, size, align, flags, &remainder, &num);
		if (!num)
			continue;

		//���slab������û�кͶ������һ�𣬲��Ҹ�order��Ŷ�����������ÿ��slab���������Ķ����������򷵻�
		if (flags & CFLGS_OFF_SLAB) {
			/*
			 * Max number of objs-per-slab for caches which
			 * use off-slab slabs. Needed to avoid a possible
			 * looping condition in cache_grow().
			 */
			offslab_limit = size - sizeof(struct slab);
			offslab_limit /= sizeof(kmem_bufctl_t);

 			if (num > offslab_limit)
				break;
		}

		/* Found something acceptable - save it away */
		//�ҵ��˺��ʵ�order������ز�������
		cachep->num = num;   //slab�еĶ�������
		cachep->gfporder = gfporder;   //orderֵ
		left_over = remainder;   //slab�е���Ƭ��С

		/*
		 * A VFS-reclaimable slab tends to have most allocations
		 * as GFP_NOFS and we really don't want to have to be allocating
		 * higher-order pages when we are unable to shrink dcache.
		 */
		 //SLAB_RECLAIM_ACCOUNT��ʾ��slab��ռҳ��Ϊ�ɻ��յģ����ں˼���Ƿ����㹻��ҳ�������û�̬������ʱ��
		 //����ҳ�潫���������ڣ�ͨ������kmem_freepages()�������Խ��ͷŷ����slab��ҳ��
		 //�����ǿɻ��յģ����Բ������������Ƭ����ˡ�
		if (flags & SLAB_RECLAIM_ACCOUNT)
			break;

		/*
		 * Large number of objects is good, but very large slabs are
		 * currently bad for the gfp()s.
		 */
		 //slab_break_gfp_order��ʼ��Ϊ1����slab�����2^1=2��ҳ
		if (gfporder >= slab_break_gfp_order)
			break;

		/*
		 * Acceptable internal fragmentation?
		 */
		 //slab��ռҳ��Ĵ�С����Ƭ��С��8�����ϣ�ҳ�������ʽϸߣ����Խ���������order
		if (left_over * 8 <= (PAGE_SIZE << gfporder))
			break;
	}
	//������Ƭ��С
	return left_over;
}

static int __init_refok setup_cpu_cache(struct kmem_cache *cachep)
{
	//��ʱ��ʼ���Ѿ����,ֱ��ʹ��local cache
	if (g_cpucache_up == FULL)    
		return enable_cpucache(cachep);

	//�������ִ�е�����Ǿ�˵����ǰ���ڳ�ʼ���׶�
	//g_cpucache_up��¼��ʼ���Ľ��ȣ�����PARTIAL_AC��ʾ struct array_cache �� cache �Ѿ�����
	//PARTIAL_L3 ��ʾstruct kmem_list3 ���ڵ� cache �Ѿ�������ע�ⴴ�������� cache ���Ⱥ�˳���ڳ�ʼ���׶�ֻ��������cpu��local cache��slab����
	//��g_cpucache_up Ϊ NONE��˵�� sizeof(struct array)��С�� cache ��û�д�������ʼ���׶δ��� sizeof(struct array) ��С��cache ʱ����������
	//��ʱ struct arraycache_init ���ڵ� general cache ��δ������ֻ��ʹ�þ�̬�����ȫ�ֱ��� initarray_eneric ��ʾ�� local cache
	if (g_cpucache_up == NONE) {
		/*
		 * Note: the first kmem_cache_create must create the cache
		 * that's used by kmalloc(24), otherwise the creation of
		 * further caches will BUG().
		 */
		cachep->array[smp_processor_id()] = &initarray_generic.cache; //arraycache_init�Ļ�������û�д�������ʹ�þ�̬��

		/*
		 * If the cache that's used by kmalloc(sizeof(kmem_list3)) is
		 * the first cache, then we need to set up all its list3s,
		 * otherwise the creation of further caches will BUG().
		 */
		 //chuangjian struct kmem_list3 ���ڵ�cache����struct array_cache����cache֮��
		 //���Դ�ʱ struct kmem_list3 ���ڵ� cache Ҳһ��û�д�����Ҳ��Ҫʹ��ȫ�ֱ��� initkmem_list3

		 //#define SIZE_AC 1����һ�ΰ�arraycache_init�Ļ�������initkmem_list3[1]��������
		 //��һ�λ����
		set_up_list3s(cachep, SIZE_AC);  

		//ִ�е�����struct array_cache���ڵ� cache ������ϣ�
		//���struct kmem_list3��struct array_cache �Ĵ�Сһ������ô�Ͳ������ظ������ˣ�g_cpucache_up��ʾ�Ľ��ȸ���һ��
		if (INDEX_AC == INDEX_L3) 
			g_cpucache_up = PARTIAL_L3;  //����cpu up ״̬
		else
			g_cpucache_up = PARTIAL_AC;
	} else {
		//g_cache_up����ΪPARTIAL_ACʱ���������̣�struct arraycache_init���ڵ�general cache�Ѿ���������������ͨ�mkalloc�����ˡ�
		cachep->array[smp_processor_id()] =
			kmalloc(sizeof(struct arraycache_init), GFP_KERNEL);

		//struct kmem_list3 ���ڵ�cache��δ������ϣ�����ʹ��ȫ�ֵ�slab����
		if (g_cpucache_up == PARTIAL_AC) {
			set_up_list3s(cachep, SIZE_L3);
			g_cpucache_up = PARTIAL_L3;
		} else { 
		//�ܽ��뵽����˵��struct kmem_list3���ڵ�cache��struct array_cache���ڵ�cache���Ѵ�����ϣ�����ȫ�ֱ���
			int node;
			for_each_online_node(node) {
				//ͨ��kmalloc����struct kmem_list3����
				cachep->nodelists[node] =
				    kmalloc_node(sizeof(struct kmem_list3),
						GFP_KERNEL, node);
				BUG_ON(!cachep->nodelists[node]);
				//��ʼ��slab����
				kmem_list3_init(cachep->nodelists[node]);
			}
		}
	}
	//FIXME: �������ʱ��
	cachep->nodelists[numa_node_id()]->next_reap =
			jiffies + REAPTIMEOUT_LIST3 +
			((unsigned long)cachep) % REAPTIMEOUT_LIST3;

	//��ʼ��ac��һЩ����
	cpu_cache_get(cachep)->avail = 0;
	cpu_cache_get(cachep)->limit = BOOT_CPUCACHE_ENTRIES;
	cpu_cache_get(cachep)->batchcount = 1;
	cpu_cache_get(cachep)->touched = 0;
	cachep->batchcount = 1;
	cachep->limit = BOOT_CPUCACHE_ENTRIES;
	return 0;
}

/**
 * kmem_cache_create - Create a cache.
 * @name: A string which is used in /proc/slabinfo to identify this cache.
 * @size: The size of objects to be created in this cache.
 * @align: The required(����ĵ�) alignment for the objects.  //
 * @flags: SLAB flags
 * @ctor: A constructor for the objects.
 * @dtor: A destructor for the objects (not implemented anymore).
 *
 * Returns a ptr to the cache on success, NULL on failure.  //�ɹ�����cacheָ�룬ʧ�ܷ��ؿ�
 * Cannot be called within a int, but can be interrupted.   //�������ж��е��ã����ǿ��Ա����
 * The @ctor is run when new pages are allocated by the cache
 * and the @dtor is run before the pages are handed back.
 *
 * @name must be valid until the cache is destroyed. This implies that
 * the module calling this has to destroy the cache before getting unloaded.
 *
 * The flags are  //�����
 * 
 * %SLAB_POISON - Poison(ʹ��Ⱦ) the slab with a known test pattern (a5a5a5a5)  //ʹ��a5a5a5a5�����Ƭδ��ʼ������ 
 * to catch references to uninitialised memory.
 *
 * %SLAB_RED_ZONE - Insert `Red' zones around the allocated memory to check  //��Ӻ�ɫ�����������Խ��
 * for buffer overruns.
 *
 * %SLAB_HWCACHE_ALIGN - Align the objects in this cache to a hardware    //�������ж��룬
 * cacheline.  This can be beneficial if you're counting cycles as closely
 * as davem.
 */
 //����������
 /* gfporder: ȡֵ0��11����ֱ�������cache�Ķ�����������ѭ����slab��2^gfporder��ҳ�����
   buffer_size: Ϊ��ǰcache�ж��󾭹�cache_line_size�����Ĵ�С
   align: ��cache_line_size�����ոô�С����
   flags: �˴�Ϊ0�����ڱ�ʶ����slab��������slab
   left_over: ���ֵ����¼slab���˷ѿռ�Ĵ�С
   num�����ֵ�����ڼ�¼��ǰcache��������ڵĶ�����Ŀ
 */
struct kmem_cache *
kmem_cache_create (const char *name, size_t size, size_t align,
	unsigned long flags,
	void (*ctor)(void*, struct kmem_cache *, unsigned long),
	void (*dtor)(void*, struct kmem_cache *, unsigned long))
{
	size_t left_over, slab_size, ralign;
	struct kmem_cache *cachep = NULL, *pc;

	/*
	 * Sanity checks... these are all serious usage bugs.
	 */
	 //������飬���ֲ���ΪNULL���������ж��е��ñ�����(���������ܻ�˯��)
	 //��ȡ���Ȳ���С��4�ֽڣ���CPU�ֳ���   ��ȡ���Ȳ��ô������ֵ(������������汾��2^25���еĿ�����2^22)
	if (!name || in_interrupt() || (size < BYTES_PER_WORD) ||
	    size > KMALLOC_MAX_SIZE || dtor) {
		printk(KERN_ERR "%s: Early error in slab %s\n", __FUNCTION__,
				name);
		BUG();
	}

	/*
	 * We use cache_chain_mutex to ensure a consistent view of
	 * cpu_online_map as well.  Please see cpuup_callback
	 */
	mutex_lock(&cache_chain_mutex);

#if 0    //DEBUG ���ֱ���ע�͵��ˣ���õ���      //һЩ�����ƣ������ע
	list_for_each_entry(pc, &cache_chain, next) {
		char tmp;
		int res;

		/*
		 * This happens when the module gets unloaded and doesn't
		 * destroy its slab cache and no-one else reuses the vmalloc
		 * area of the module.  Print a warning.
		 */
		res = probe_kernel_address(pc->name, tmp);
		if (res) {
			printk(KERN_ERR
			       "SLAB: cache with size %d has lost its name\n",
			       pc->buffer_size);
			continue;
		}

		if (!strcmp(pc->name, name)) {
			printk(KERN_ERR
			       "kmem_cache_create: duplicate cache %s\n", name);
			dump_stack();
			goto oops;
		}
	}

#if DEBUG
	WARN_ON(strchr(name, ' '));	/* It confuses parsers */
#if FORCED_DEBUG
	/*
	 * Enable redzoning and last user accounting, except for caches with
	 * large objects, if the increased size would increase the object size
	 * above the next power of two: caches with object sizes just above a
	 * power of two have a significant amount of internal fragmentation.
	 */
	if (size < 4096 || fls(size - 1) == fls(size-1 + REDZONE_ALIGN +
						2 * sizeof(unsigned long long)))
		flags |= SLAB_RED_ZONE | SLAB_STORE_USER;
	if (!(flags & SLAB_DESTROY_BY_RCU))
		flags |= SLAB_POISON;
#endif
	if (flags & SLAB_DESTROY_BY_RCU)
		BUG_ON(flags & SLAB_POISON);
#endif
	/*
	 * Always checks flags, a caller might be expecting debug support which
	 * isn't available.
	 */
	BUG_ON(flags & ~CREATE_MASK);
#endif   

	/*
	 * Check that size is in terms of(����) words.  This is needed to avoid
	 * unaligned accesses for some archs(��) when redzoning is used, and makes  //���⵱��ɫ��������ʹ��ʱ������δ����ķ��ʽӴ�����
	 * sure any on-slab bufctl's are also correctly aligned.   //ͬʱȷ���κ�on-slab��bfclt ��ȷ����
	 */ 

	 ////
	//	Ϊʲôkmem_cache_init�����Ѿ������size��align�ˣ����ﻹҪ����?
	//     ��Ϊ���������������������ģ�ֻ�ǽ�����cache_cache���� kmem_cache_init�����г�ʼ������cache_cahce��
	//     size,align�ȳ�Ա�������޹�ϵ��
	////
	
	//�ȼ��   ���� !!!!       �ǲ���32λ���룬�����������е���
	if (size & (BYTES_PER_WORD - 1)) {    
		size += (BYTES_PER_WORD - 1);
		size &= ~(BYTES_PER_WORD - 1);                                  
	}

	/* calculate the final buffer alignment: */
	/* 1) arch recommendation: can be overridden for debug */
	//�ټ��  ����!!!      Ҫ��Ҫ���ջ����ж��� 
	if (flags & SLAB_HWCACHE_ALIGN) {
		/*
		 * Default alignment: as specified by the arch code.  Except if
		 * an object is really small, then squeeze multiple objects into
		 * one cacheline.
		 */
		ralign = cache_line_size();
		while (size <= ralign / 2)    //���ж����С�ĵ���������Ҫ��֤����Ĵ�С����  ���Ӳ�������ж�������Ĵ�С
			ralign /= 2;
	} else {  //����Ҫ��Ӳ�������ж��룬�Ǿ�Ĭ��4�ֽڣ���32λ
		ralign = BYTES_PER_WORD;
	}

	/*
	 * Redzoning and user store require word alignment or possibly larger.
	 * Note this will be overridden by architecture or caller mandated
	 * alignment if either is greater than BYTES_PER_WORD.
	 */
	 //���������DEBUG������Ҫ������Ӧ�Ķ���
	if (flags & SLAB_STORE_USER)
		ralign = BYTES_PER_WORD;

	if (flags & SLAB_RED_ZONE) {
		ralign = REDZONE_ALIGN;
		/* If redzoning, ensure that the second redzone is suitably
		 * aligned, by adjusting the object size accordingly. */
		size += REDZONE_ALIGN - 1;
		size &= ~(REDZONE_ALIGN - 1);
	}

	/* 2) arch mandated alignment */
	if (ralign < ARCH_SLAB_MINALIGN) {
		ralign = ARCH_SLAB_MINALIGN;
	}
	/* 3) caller mandated alignment */
	if (ralign < align) {
		ralign = align;
	}
	/* disable debug if necessary */
	if (ralign > __alignof__(unsigned long long))
		flags &= ~(SLAB_RED_ZONE | SLAB_STORE_USER);
	/*
	 * 4) Store it.
	 */
	align = ralign;   //ͨ������һ��Ѽ��㣬�����alignֵ

	/* Get cache's description obj. */
	//����cache_cache�Ĵ�С����һ��kmem_cache��ʵ����ʵ����cache_cache���ں˳�ʼ����ɺ����kmem_cache�ˣ�Ϊ���ں˳�ʼ��ʱ��ʹ��kmalloc����������Ҫ��cache_cache
	cachep = kmem_cache_zalloc(&cache_cache, GFP_KERNEL); //�����������ʹ��cache_cache
	//��������һ��ɾ�����������ڴ�
	if (!cachep)
		goto oops;

#if 0   //ע�͵� DEBUG
#if DEBUG
	cachep->obj_size = size;

	/*
	 * Both debugging options require word-alignment which is calculated
	 * into align above.
	 */
	if (flags & SLAB_RED_ZONE) {
		/* add space for red zone words */
		cachep->obj_offset += sizeof(unsigned long long);
		size += 2 * sizeof(unsigned long long);
	}
	if (flags & SLAB_STORE_USER) {
		/* user store requires one word storage behind the end of
		 * the real object. But if the second red zone needs to be
		 * aligned to 64 bits, we must allow that much space.
		 */
		if (flags & SLAB_RED_ZONE)
			size += REDZONE_ALIGN;
		else
			size += BYTES_PER_WORD;
	}
#if FORCED_DEBUG && defined(CONFIG_DEBUG_PAGEALLOC)
	if (size >= malloc_sizes[INDEX_L3 + 1].cs_size
	    && cachep->obj_size > cache_line_size() && size < PAGE_SIZE) {
		cachep->obj_offset += PAGE_SIZE - size;
		size = PAGE_SIZE;
	}
#endif
#endif
#endif

	/*
	 * Determine if the slab management is 'on' or 'off' slab.
	 * (bootstrapping cannot cope with offslab caches so don't do
	 * it too early on.)
	 */
	//��һ������ͨ��PAGE_SIZEȷ��slab�������Ĵ洢��ʽ�����û������á�
	//��ʼ���׶β�������ʽ(kmem_cache_init()�д���������ͨ���ٻ����Ͱ�slab_early_init��0��
	if ((size >= (PAGE_SIZE >> 3)) && !slab_early_init)
		/*
		 * Size is large, assume best to place the slab management obj
		 * off-slab (should allow better packing of objs).
		 */
		flags |= CFLGS_OFF_SLAB;


	size = ALIGN(size, align); //����һ����֪��slab�����ȰѶ�����Լ����ֳ����ж��룬Ȼ�����ڴ˻����������Ӳ�������н��ж��롣
	//�Ժ����еĶ��붼Ҫ������ܵĶ���ֵ����
	

	//������Ƭ��С������slab�ɼ���ҳ��(order)��ɣ�ͬʱ����ÿ��slab���ж��ٸ�����
	left_over = calculate_slab_order(cachep, size, align, flags); //��μ���Ĳ���cache_cache��

	if (!cachep->num) {
		printk(KERN_ERR
		       "kmem_cache_create: couldn't create cache %s.\n", name);
		kmem_cache_free(&cache_cache, cachep);
		cachep = NULL;
		goto oops;
	}
	
	//����slab�������Ĵ�С������struct slab����� kmem_bufctl_t ����
	slab_size = ALIGN(cachep->num * sizeof(kmem_bufctl_t)
			  + sizeof(struct slab), align);

	/*
	 * If the slab has been placed off-slab, and we have enough space then
	 * move it on-slab. This is at the expense of any extra colouring.
	 */
	 //�����һ������slab��������Ƭ��С����slab�������Ĵ�С����ɽ�slab��������Ƶ�slab�У������һ������slab!!!!!
	if (flags & CFLGS_OFF_SLAB && left_over >= slab_size) {
		flags &= ~CFLGS_OFF_SLAB;
		left_over -= slab_size;   //slab_size ���� slab ��������С
	}

	if (flags & CFLGS_OFF_SLAB) {
		//align�����slab����ģ���� slab������ �����ô洢����ȻҲ��������������Ӱ�쵽����slab����Ĵ洢λ��
		//slab������Ҳ�Ͳ���Ҫ������
		/* really off slab. No need for manual alignment */
		slab_size =
		    cachep->num * sizeof(kmem_bufctl_t) + sizeof(struct slab);
	}

	//��ɫ�鵥λ��ΪL1_CACHE_BYTES����32�ֽ�
	cachep->colour_off = cache_line_size();
	/* Offset must be a multiple of the alignment. */
	//��ɫ��λ�����Ƕ��뵥λ��������
	if (cachep->colour_off < align)
		cachep->colour_off = align;
	//������Ƭ������Ҫ���ٸ���ɫ��
	cachep->colour = left_over / cachep->colour_off;
	//�������Ĵ�С
	cachep->slab_size = slab_size;
	cachep->flags = flags;
	cachep->gfpflags = 0;
	if (CONFIG_ZONE_DMA_FLAG && (flags & SLAB_CACHE_DMA))   //����ϵͳ������DMA��־
		cachep->gfpflags |= GFP_DMA;
	//slab����Ĵ�С
	cachep->buffer_size = size;
	//����
	cachep->reciprocal_buffer_size = reciprocal_value(size);


	//���������slab������Ҫ����һ��������󣬱�����slabp_cache�У����������ʽ��slab����ָ��Ϊ��
	//array_cachine, cache_cache, 3list �⼸���϶�������ʽ������������
	if (flags & CFLGS_OFF_SLAB) {
		cachep->slabp_cache = kmem_find_general_cachep(slab_size, 0u);
		/*
		 * This is a possibility for one of the malloc_sizes caches.
		 * But since we go off slab only for object size greater than
		 * PAGE_SIZE/8, and malloc_sizes gets created in ascending order,
		 * this should not happen at all.
		 * But leave a BUG_ON for some lucky dude.
		 */
		BUG_ON(!cachep->slabp_cache);
	}
	
	//kmem_cach�����ֺ�������Ķ���Ĺ��캯��
	cachep->ctor = ctor;
	cachep->name = name;

	//����ÿ��CPU�ϵ�local cache������local cache��slab ����
	if (setup_cpu_cache(cachep)) {
		__kmem_cache_destroy(cachep);
		cachep = NULL;
		goto oops;
	}

	//��kmem_cache���뵽cache_chainΪͷ��kmem_cache������
	/* cache setup completed, link it into the list */
	list_add(&cachep->next, &cache_chain);   //��������cache_chain
oops:
	if (!cachep && (flags & SLAB_PANIC))
		panic("kmem_cache_create(): failed to create slab `%s'\n",
		      name);
	mutex_unlock(&cache_chain_mutex);  //mutex
	return cachep;   //���ظ� kmem_cache
}
EXPORT_SYMBOL(kmem_cache_create);

#if DEBUG
static void check_irq_off(void)
{
	BUG_ON(!irqs_disabled());
}

static void check_irq_on(void)
{
	BUG_ON(irqs_disabled());
}

static void check_spinlock_acquired(struct kmem_cache *cachep)
{
#ifdef CONFIG_SMP
	check_irq_off();
	assert_spin_locked(&cachep->nodelists[numa_node_id()]->list_lock);
#endif
}

static void check_spinlock_acquired_node(struct kmem_cache *cachep, int node)
{
#ifdef CONFIG_SMP
	check_irq_off();
	assert_spin_locked(&cachep->nodelists[node]->list_lock);
#endif
}

#else
#define check_irq_off()	do { } while(0)
#define check_irq_on()	do { } while(0)
#define check_spinlock_acquired(x) do { } while(0)
#define check_spinlock_acquired_node(x, y) do { } while(0)
#endif

static void drain_array(struct kmem_cache *cachep, struct kmem_list3 *l3,
			struct array_cache *ac,
			int force, int node);

//�ͷű��ػ����еĶ���
static void do_drain(void *arg)
{
	struct kmem_cache *cachep = arg;
	struct array_cache *ac;
	int node = numa_node_id();
	check_irq_off();

	//��ñ��ػ���
	ac = cpu_cache_get(cachep);
	spin_lock(&cachep->nodelists[node]->list_lock);

	//�ͷű��ػ����еĶ���
	free_block(cachep, ac->entry, ac->avail, node);

	spin_unlock(&cachep->nodelists[node]->list_lock);
	ac->avail = 0;
}

//�ͷű��ػ���ͱ��ع������еĶ���
static void drain_cpu_caches(struct kmem_cache *cachep)
{
	struct kmem_list3 *l3;
	int node;

	//�ͷ�ÿ�����ػ����еĶ���ע��û�� "online"
	on_each_cpu(do_drain, cachep, 1, 1);  //������do_drain()����
	check_irq_on();

	//NUMA��أ��ͷ�ÿ��NUMA�ڵ��alien
	for_each_online_node(node) {
		l3 = cachep->nodelists[node];
		if (l3 && l3->alien)
			//���汾Ŀǰ�ǿպ������ݲ�֧��
			drain_alien_cache(cachep, l3->alien);
	}

	//�ͷű��ع������еĶ���
	for_each_online_node(node) {
		l3 = cachep->nodelists[node];
		if (l3)
			drain_array(cachep, l3, l3->shared, 1, node);
	}
}

/*
 * Remove slabs from the list of free slabs.
 * Specify the number of slabs to drain in tofree.
 *
 * Returns the actual number of slabs released.
 */
 //���ٿ�slab�����е�slab
static int drain_freelist(struct kmem_cache *cache,
			struct kmem_list3 *l3, int tofree)
{
	struct list_head *p;
	int nr_freed;
	struct slab *slabp;

	nr_freed = 0;
	//�ͷſ����е�slab
	while (nr_freed < tofree && !list_empty(&l3->slabs_free)) {

		spin_lock_irq(&l3->list_lock);
		p = l3->slabs_free.prev;
		if (p == &l3->slabs_free) {
			spin_unlock_irq(&l3->list_lock);
			goto out;
		}

		slabp = list_entry(p, struct slab, list);
#if DEBUG
		BUG_ON(slabp->inuse);
#endif	
		//��slab�ӿ�����ժ��
		list_del(&slabp->list);
		/*
		 * Safe to drop the lock. The slab is no longer linked
		 * to the cache.
		 */
		//�ܿ��ж�����ȥÿ��slab�еĶ�����
		l3->free_objects -= cache->num;
		spin_unlock_irq(&l3->list_lock);
		//����slab������slab��������slab����
		slab_destroy(cache, slabp);
		nr_freed++;
	}
out:
	return nr_freed;
}

/* Called with cache_chain_mutex held to protect against cpu hotplug */
//�ͷſ������е�slab
static int __cache_shrink(struct kmem_cache *cachep)
{
	int ret = 0, i = 0;
	struct kmem_list3 *l3;

	//�ͷű��ػ����ж���
	drain_cpu_caches(cachep);

	check_irq_on();
	for_each_online_node(i) {
		l3 = cachep->nodelists[i];
		if (!l3)
			continue;

		//�ͷſ������е�slab
		drain_freelist(cachep, l3, l3->free_objects);
		//�����slab����Ͳ�����slab�����Ƿ���slab
		ret += !list_empty(&l3->slabs_full) ||
			!list_empty(&l3->slabs_partial);
	}
	return (ret ? 1 : 0);
}

/**
 * kmem_cache_shrink - Shrink a cache.
 * @cachep: The cache to shrink.
 *
 * Releases as many slabs as possible for a cache.
 * To help debugging, a zero exit status indicates all slabs were released.
 */
int kmem_cache_shrink(struct kmem_cache *cachep)
{
	int ret;
	BUG_ON(!cachep || in_interrupt());

	mutex_lock(&cache_chain_mutex);
	ret = __cache_shrink(cachep);
	mutex_unlock(&cache_chain_mutex);
	return ret;
}
EXPORT_SYMBOL(kmem_cache_shrink);

/**
 * kmem_cache_destroy - delete a cache
 * @cachep: the cache to destroy
 *
 * Remove a &struct kmem_cache object from the slab cache.
 *
 * It is expected this function will be called by a module when it is
 * unloaded.  This will remove the cache completely, and avoid a duplicate
 * cache being allocated each time a module is loaded and unloaded, if the
 * module doesn't have persistent in-kernel storage across loads and unloads.
 *
 * The cache must be empty before calling this function.
 *
 * The caller must guarantee that noone will allocate memory from the cache
 * during the kmem_cache_destroy().
 */
 //����һ����������ͨ����ֻ������ж��moduleʱ
void kmem_cache_destroy(struct kmem_cache *cachep)
{
	BUG_ON(!cachep || in_interrupt());

	/* Find the cache in the chain of caches. */
	mutex_lock(&cache_chain_mutex);
	/*
	 * the chain is never empty, cache_cache is never destroyed
	 */
	 //����������cache_chain��������ժ��
	list_del(&cachep->next);
	if (__cache_shrink(cachep)) {  //�ͷſ������е�slab������������������������ٻ�����ǰ���������������е�slab
		//��slab���򲿷���slab����Ϊ��
		slab_error(cachep, "Can't free all objects");
		//�������ǿգ��������٣����¼��뵽cache_chain������
		list_add(&cachep->next, &cache_chain);

		mutex_unlock(&cache_chain_mutex);
		return;
	}

	//�й�rcu
	if (unlikely(cachep->flags & SLAB_DESTROY_BY_RCU))
		synchronize_rcu();

	//�ײ����__kmem_cache_destroy()������ʵ��
	__kmem_cache_destroy(cachep);
	mutex_unlock(&cache_chain_mutex);
}
EXPORT_SYMBOL(kmem_cache_destroy);

/*
 * Get the memory for a slab management obj.
 * For a slab cache when the slab descriptor is off-slab, slab descriptors
 * always come from malloc_sizes caches.  The slab descriptor cannot
 * come from the same cache which is getting created because,
 * when we are searching for an appropriate cache for these
 * descriptors in kmem_cache_create, we search through the malloc_sizes array.  //ͨ��malloc_sizes��
 * If we are creating a malloc_sizes cache here it would not be visible(���Ե�) to
 * kmem_find_general_cachep till the initialization is complete.
 * Hence(��ˣ����) we cannot have slabp_cache same as the original cache.
 */
static struct slab *alloc_slabmgmt(struct kmem_cache *cachep, void *objp,
				   int colour_off, gfp_t local_flags,
				   int nodeid)
{
	struct slab *slabp;

	//���������slab
	if (OFF_SLAB(cachep)) {
		/* Slab management obj is off-slab. */
		//����slab���Ƿ��䵽slabpָ���ϣ������洴��һ��slab:)
		slabp = kmem_cache_alloc_node(cachep->slabp_cache,
					      local_flags & ~GFP_THISNODE, nodeid);
		if (!slabp)
			return NULL;
	} else {
		//��������slab��slab���������ڸ�slab��ռ�ռ����ʼ������ռ������ʼ��ַ����������ɫƫ��
		//Ȼ��Ӧ�ø��¸���ɫƫ�Ƽ����Ϲ������Ŀռ�
		slabp = objp + colour_off;
		colour_off += cachep->slab_size;
	}
	
	//ע��������������������slab�����������õģ�������slabp�ĵ�ַ���ܸı䡣�������治������Щ��ֱ����slabp��ַ������slab�����߾�����
	
	slabp->inuse = 0;
	//���ڵ�һ������ҳ��ƫ�ƣ��������֪���������ʽslab��colouroff��Ա����������ɫ�����������������ռ�õĿռ�
	//��������ʽslab��colouroff��Աֻ������ɫ��
	slabp->colouroff = colour_off;
	//��һ������������ַ����ʱ��ɫƫ�ƶ�������slab�Ѽ����˹������Ŀռ�
	slabp->s_mem = objp + colour_off;
	//���½ڵ�id
	slabp->nodeid = nodeid;
	return slabp;
}

static inline kmem_bufctl_t *slab_bufctl(struct slab *slabp)
{
	return (kmem_bufctl_t *) (slabp + 1);
}

static void cache_init_objs(struct kmem_cache *cachep,
			    struct slab *slabp)
{
	int i;

	//ѭ��������������
	for (i = 0; i < cachep->num; i++) {
		void *objp = index_to_obj(cachep, slabp, i);
#if 0
#if DEBUG
		/* need to poison the objs? */
		if (cachep->flags & SLAB_POISON)
			poison_obj(cachep, objp, POISON_FREE);
		if (cachep->flags & SLAB_STORE_USER)
			*dbg_userword(cachep, objp) = NULL;

		if (cachep->flags & SLAB_RED_ZONE) {
			*dbg_redzone1(cachep, objp) = RED_INACTIVE;
			*dbg_redzone2(cachep, objp) = RED_INACTIVE;
		}
		/*
		 * Constructors are not allowed to allocate memory from the same
		 * cache which they are a constructor for.  Otherwise, deadlock.
		 * They must also be threaded.
		 */
		if (cachep->ctor && !(cachep->flags & SLAB_POISON))
			cachep->ctor(objp + obj_offset(cachep), cachep,
				     0);

		if (cachep->flags & SLAB_RED_ZONE) {
			if (*dbg_redzone2(cachep, objp) != RED_INACTIVE)
				slab_error(cachep, "constructor overwrote the"
					   " end of an object");
			if (*dbg_redzone1(cachep, objp) != RED_INACTIVE)
				slab_error(cachep, "constructor overwrote the"
					   " start of an object");
		}
		if ((cachep->buffer_size % PAGE_SIZE) == 0 &&
			    OFF_SLAB(cachep) && cachep->flags & SLAB_POISON)
			kernel_map_pages(virt_to_page(objp),
#endif				 cachep->buffer_size / PAGE_SIZE, 0);
#else
		if (cachep->ctor)
			cachep->ctor(objp, cachep, 0);
#endif
		slab_bufctl(slabp)[i] = i + 1;   //ʹ��bufctl����洢����
	}
	slab_bufctl(slabp)[i - 1] = BUFCTL_END;  //���һ��ָ��BUFCTL_END
	slabp->free = 0;  //���ÿ��и���
}

static void kmem_flagcheck(struct kmem_cache *cachep, gfp_t flags)
{
	if (CONFIG_ZONE_DMA_FLAG) {
		if (flags & GFP_DMA)
			BUG_ON(!(cachep->gfpflags & GFP_DMA));
		else
			BUG_ON(cachep->gfpflags & GFP_DMA);
	}
}

static void *slab_get_obj(struct kmem_cache *cachep, struct slab *slabp,
				int nodeid)
{
	//���һ�����еĶ���free��Ա�Ǳ�slab�е�һ�����ж��������
	void *objp = index_to_obj(cachep, slabp, slabp->free);
	kmem_bufctl_t next;

	//�������ö������ֵ
	slabp->inuse++;
	
	//�����һ�εĵ�һ�����ж�������
	next = slab_bufctl(slabp)[slabp->free];
#if DEBUG
	slab_bufctl(slabp)[slabp->free] = BUFCTL_FREE;
	WARN_ON(slabp->nodeid != nodeid);
#endif
	slabp->free = next;  //���¿��ж����������������Ӧ

	return objp;
}

static void slab_put_obj(struct kmem_cache *cachep, struct slab *slabp,
				void *objp, int nodeid)
{
	//��ö����� kmem_bufctl_t �����е�����
	unsigned int objnr = obj_to_index(cachep, slabp, objp);
#if 0
#if DEBUG
	/* Verify that the slab belongs to the intended node */
	WARN_ON(slabp->nodeid != nodeid);

	if (slab_bufctl(slabp)[objnr] + 1 <= SLAB_LIMIT + 1) {
		printk(KERN_ERR "slab: double free detected in cache "
				"'%s', objp %p\n", cachep->name, objp);
		BUG();
	}
#endif 
#endif
	//ָ��slab��ԭ���ĵ�һ�����ж���
	slab_bufctl(slabp)[objnr] = slabp->free;
	//��Ҫ�ͷŵĶ�����Ϊ��һ�����ж���������ͷ��˵Ķ�������slab����
	slabp->free = objnr;
	//�ѷ����������һ
	slabp->inuse--;
}

/*
 * Map pages beginning at addr to the given cache and slab. This is required
 * for the slab allocator to be able to lookup the cache and slab of a
 * virtual address for kfree, ksize, kmem_ptr_validate, and slab debugging.
 */
static void slab_map_pages(struct kmem_cache *cache, struct slab *slab,
			   void *addr)
{
	int nr_pages;
	struct page *page;

	page = virt_to_page(addr);  //

	nr_pages = 1;
	if (likely(!PageCompound(page)))
		nr_pages <<= cache->gfporder;   //�����slab��ҳ����

	do {
		page_set_cache(page, cache);    //������cache��ӳ��
		page_set_slab(page, slab);         //������slab��ӳ��
		page++;
	} while (--nr_pages);
}

/*
 * Grow (by 1) the number of slabs within a cache.  This is called by
 * kmem_cache_alloc() when there are no active objs left in a cache.
 */
static int cache_grow(struct kmem_cache *cachep,
		gfp_t flags, int nodeid, void *objp)
{
	struct slab *slabp;
	size_t offset;
	gfp_t local_flags;
	struct kmem_list3 *l3;

	/*
	 * Be lazy and only check for valid flags here,  keeping it out of the
	 * critical path in kmem_cache_alloc().
	 */
	BUG_ON(flags & ~(GFP_DMA | GFP_LEVEL_MASK));

	local_flags = (flags & GFP_LEVEL_MASK);
	/* Take the l3 list lock to change the colour_next on this node */
	//ȷ�������ж�
	check_irq_off();
	//��ȡ���ڵ�cache��������slab����
	l3 = cachep->nodelists[nodeid];
	spin_lock(&l3->list_lock);

	/* Get colour for the slab, and cal the next value. */
	//��ȡ������slab����ɫ��Ŀ
	offset = l3->colour_next;
	//��ȡ����Ҫ++��������һ��Ҫ������slab����ɫ��Ŀ
	l3->colour_next++;
	if (l3->colour_next >= cachep->colour)
	//��ɫ��ű���С����ɫ��������������ˣ�����Ϊ0���������ɫѭ�����⡣��ʵ�ϣ����slab���˷ѵĿռ���٣���ô�ܿ�ͻ�ѭ��һ��
		l3->colour_next = 0;
	spin_unlock(&l3->list_lock);

	//��cache�����ɫƫ�ƣ� ע�� *=
	offset *= cachep->colour_off;  //colour_off�ǵ�λ

	//ʹ����_GFP_WAIT�ͻῪ���ж�
	if (local_flags & __GFP_WAIT)
		local_irq_enable();

	/*
	 * The test for missing atomic flag is performed here, rather than
	 * the more obvious place, simply to reduce the critical path length
	 * in kmem_cache_alloc(). If a caller is seriously mis-behaving they
	 * will eventually be caught here (where it matters).
	 */
	 //�������ϵͳ�����ı��
	kmem_flagcheck(cachep, flags);

	/*
	 * Get mem for the objs.  Attempt to allocate a physical page from
	 * 'nodeid'.
	 */
	 //��buddy��ȡ����ҳ�����ص��������ַobjp
	if (!objp)
		objp = kmem_getpages(cachep, flags, nodeid);
	if (!objp)  //ʧ�ܾ�failed
		goto failed;

	/* Get slab management. */
	//���һ���µ�slab������
	slabp = alloc_slabmgmt(cachep, objp, offset,
			local_flags & ~GFP_THISNODE, nodeid);
	if (!slabp)
		goto opps1;

	//���ýڵ�id
	slabp->nodeid = nodeid;   
	//��slab������slabp���������prev�ֶΣ��Ѹ��ٻ��������� cachep ��������ҳ�� LRU �ֶ�
	//�����ǽ���slab��cache������ҳ��ӳ�䣬���ڿ��ٸ�������ҳ��λslab��������cache������
	slab_map_pages(cachep, slabp, objp);

	//��ʼ��cache��������slab����������
	cache_init_objs(cachep, slabp);

	if (local_flags & __GFP_WAIT)
		local_irq_disable();
	check_irq_off();
	spin_lock(&l3->list_lock);

	/* Make slab active. */
	//�������ʼ���õ�slabβ�巨���뵽������ȫ������
	list_add_tail(&slabp->list, &(l3->slabs_free));
	STATS_INC_GROWN(cachep);
	//���¸��ٻ����п��ж��������
	l3->free_objects += cachep->num;
	spin_unlock(&l3->list_lock);
	return 1;
opps1:
	kmem_freepages(cachep, objp);
failed:
	if (local_flags & __GFP_WAIT)
		local_irq_disable();
	return 0;
}

#if DEBUG

/*
 * Perform extra freeing checks:
 * - detect bad pointers.
 * - POISON/RED_ZONE checking
 */
static void kfree_debugcheck(const void *objp)
{
	if (!virt_addr_valid(objp)) {
		printk(KERN_ERR "kfree_debugcheck: out of range ptr %lxh.\n",
		       (unsigned long)objp);
		BUG();
	}
}

static inline void verify_redzone_free(struct kmem_cache *cache, void *obj)
{
	unsigned long long redzone1, redzone2;

	redzone1 = *dbg_redzone1(cache, obj);
	redzone2 = *dbg_redzone2(cache, obj);

	/*
	 * Redzone is ok.
	 */
	if (redzone1 == RED_ACTIVE && redzone2 == RED_ACTIVE)
		return;

	if (redzone1 == RED_INACTIVE && redzone2 == RED_INACTIVE)
		slab_error(cache, "double free detected");
	else
		slab_error(cache, "memory outside object was overwritten");

	printk(KERN_ERR "%p: redzone 1:0x%llx, redzone 2:0x%llx.\n",
			obj, redzone1, redzone2);
}

static void *cache_free_debugcheck(struct kmem_cache *cachep, void *objp,
				   void *caller)
{
	struct page *page;
	unsigned int objnr;
	struct slab *slabp;

	objp -= obj_offset(cachep);
	kfree_debugcheck(objp);
	page = virt_to_head_page(objp);

	slabp = page_get_slab(page);

	if (cachep->flags & SLAB_RED_ZONE) {
		verify_redzone_free(cachep, objp);
		*dbg_redzone1(cachep, objp) = RED_INACTIVE;
		*dbg_redzone2(cachep, objp) = RED_INACTIVE;
	}
	if (cachep->flags & SLAB_STORE_USER)
		*dbg_userword(cachep, objp) = caller;

	objnr = obj_to_index(cachep, slabp, objp);

	BUG_ON(objnr >= cachep->num);
	BUG_ON(objp != index_to_obj(cachep, slabp, objnr));

#ifdef CONFIG_DEBUG_SLAB_LEAK
	slab_bufctl(slabp)[objnr] = BUFCTL_FREE;
#endif
	if (cachep->flags & SLAB_POISON) {
#ifdef CONFIG_DEBUG_PAGEALLOC
		if ((cachep->buffer_size % PAGE_SIZE)==0 && OFF_SLAB(cachep)) {
			store_stackinfo(cachep, objp, (unsigned long)caller);
			kernel_map_pages(virt_to_page(objp),
					 cachep->buffer_size / PAGE_SIZE, 0);
		} else {
			poison_obj(cachep, objp, POISON_FREE);
		}
#else
		poison_obj(cachep, objp, POISON_FREE);
#endif
	}
	return objp;
}

static void check_slabp(struct kmem_cache *cachep, struct slab *slabp)
{
	kmem_bufctl_t i;
	int entries = 0;

	/* Check slab's freelist to see if this obj is there. */
	for (i = slabp->free; i != BUFCTL_END; i = slab_bufctl(slabp)[i]) {
		entries++;
		if (entries > cachep->num || i >= cachep->num)
			goto bad;
	}
	if (entries != cachep->num - slabp->inuse) {
bad:
		printk(KERN_ERR "slab: Internal list corruption detected in "
				"cache '%s'(%d), slabp %p(%d). Hexdump:\n",
			cachep->name, cachep->num, slabp, slabp->inuse);
		for (i = 0;
		     i < sizeof(*slabp) + cachep->num * sizeof(kmem_bufctl_t);
		     i++) {
			if (i % 16 == 0)
				printk("\n%03x:", i);
			printk(" %02x", ((unsigned char *)slabp)[i]);
		}
		printk("\n");
		BUG();
	}
}
#else
#define kfree_debugcheck(x) do { } while(0)
#define cache_free_debugcheck(x,objp,z) (objp)
#define check_slabp(x,y) do { } while(0)
#endif

static void *cache_alloc_refill(struct kmem_cache *cachep, gfp_t flags)
{
	int batchcount;
	struct kmem_list3 *l3;
	struct array_cache *ac;
	int node;

	node = numa_node_id();

	check_irq_off();
	//���͸��ٻ������
	ac = cpu_cache_get(cachep);
retry:
	//׼����䱾�ظ��ٻ��棬�����ȼ�¼�������������batchcount��Ա(����ת��ת���ĸ���)
	batchcount = ac->batchcount;
	if (!ac->touched && batchcount > BATCHREFILL_LIMIT) {
		/*
		 * If there was little recent activity on this cache, then
		 * perform only a partial refill.  Otherwise we could generate
		 * refill bouncing.
		 */
		batchcount = BATCHREFILL_LIMIT;
	}

	//��ñ��ڴ�ڵ��kmem_cache������
	l3 = cachep->nodelists[node];

	BUG_ON(ac->avail > 0 || !l3);
	spin_lock(&l3->list_lock);

	/* See if we can refill from the shared array */
	//������֮�ڶ���:
	//����б��ع�����ٻ��棬��ӹ����ظ��ٻ�����䣬�����ڶ�ˣ����CPU����ĸ��ٻ���
	if (l3->shared && transfer_objects(ac, l3->shared, batchcount))
		goto alloc_done;  //һ����ֻ������һ���飬����ɹ�����ȥalloc_down��ac->avail�Ѿ����޸�

	//������������:�ӱ��صĸ��ٻ���������з���
	while (batchcount > 0) {
		struct list_head *entry;
		struct slab *slabp;
		/* Get slab alloc is to come from. */
		//���ȷ��ʲ�������slab������entryָ���һ���ڵ�
		entry = l3->slabs_partial.next;
		//�������������Ķ�û�ˣ�����ȫ���е�
		if (entry == &l3->slabs_partial) {
			//�ڷ���ȫ����slab����ǰ����һ����ǣ�����ȫ����slab����ʹ�ù���
			l3->free_touched = 1;
			//ʹentryָ��ȫ���������һ���ڵ�
			entry = l3->slabs_free.next;
			//ȫ���е�Ҳû�ˣ�Ҳ�������ǵ���������ʧ���ˣ������������:)
			if (entry == &l3->slabs_free)
				goto must_grow;  
		}

		//���������ֻ������������ǲ��������������ȫ���е������пɷ���������Ǿ�ȥ������׼������
		slabp = list_entry(entry, struct slab, list);
		//�����������պ���
		check_slabp(cachep, slabp);
		check_spinlock_acquired(cachep);

		/*
		 * The slab was either on partial or free list so
		 * there must be at least one object available for
		 * allocation.
		 */
		BUG_ON(slabp->inuse < 0 || slabp->inuse >= cachep->num);

		//��������л����ڶ���δ���䣬�Ͱ�����䵽ac�У����batchcount��
		while (slabp->inuse < cachep->num && batchcount--) {
			//һ������������������պ���
			STATS_INC_ALLOCED(cachep);
			STATS_INC_ACTIVE(cachep);
			STATS_SET_HIGH(cachep);

			//��slab����ȡ���ж��󣬲��뵽ac��
			ac->entry[ac->avail++] = slab_get_obj(cachep, slabp,
							    node);
		}
		check_slabp(cachep, slabp);  //�պ���

		/* move slabp to correct slabp list: */
		//����������������������ֶ��������ڴ˴���Ҫ���������slab��û�п��ж�����ӵ�������full����
		//���п��ж�����ӵ�������partial slab������
		list_del(&slabp->list);
		if (slabp->free == BUFCTL_END)
			list_add(&slabp->list, &l3->slabs_full);
		else
			list_add(&slabp->list, &l3->slabs_partial);
	}

must_grow:
	//ǰ��������������avail�����ж���ac�У���ʱ��Ҫ���������Ŀ��ж�����
	l3->free_objects -= ac->avail;
alloc_done:
	spin_unlock(&l3->list_lock);

	//�˴��п���֮ǰshared array ͨ�� transfer ���� ac һ���֣�Ҳ�������������
	//����ת�Ƴɹ�Ҳ���������
	if (unlikely(!ac->avail)) {
		int x;
		//ʹ��cache_growΪ���ٻ������һ���µ�slab
		//�����ֱ���: cacheָ�롢��־���ڴ�ڵ㡢ҳ�����ַ(Ϊ�ձ�ʾ��δ�����ڴ�ҳ����Ϊ�գ�˵���������ڴ�ҳ����ֱ����������slab)
		//����ֵ: 1Ϊ�ɹ���0Ϊʧ��
		x = cache_grow(cachep, flags | GFP_THISNODE, node, NULL);

		/* cache_grow can reenable interrupts, then ac could change. */
		//����Ĳ���ʹ�����жϣ����ڼ�local cacheָ����ܷ����˱仯����Ҫ���»��
		ac = cpu_cache_get(cachep);

		//���cache_growʧ�ܣ�local cache��Ҳû�п��ж���˵�������޷������ڴ��ˣ��Ǿ͹���:)
		if (!x && ac->avail == 0)	/* no objects in sight? abort */
			return NULL;

		//���ac�еĿ��ö���Ϊ0�����ǾͰ�����ͨ�����ϵͳ��������slab�ָ�acһ���֣��Ͼ�per-cpu������
		//������ֻҪac_>avail��Ϊ0��˵���������������ac����Ϊ������ֻ�����������Բ���Ҫ�ٷ�������ac�ˣ�����ִ��retry
		if (!ac->avail)		/* objects refilled by interrupt? */
			goto retry;
	}
	//������local cache����ac�����ý��ڷ��ʱ�־ touch
	ac->touched = 1; 
	//������Ҫ����ac�����һ������ĵ�ַ����Ϊ��local cache�з�������������ŵ�
	return ac->entry[--ac->avail];
}

static inline void cache_alloc_debugcheck_before(struct kmem_cache *cachep,
						gfp_t flags)
{
	might_sleep_if(flags & __GFP_WAIT);
#if DEBUG
	kmem_flagcheck(cachep, flags);
#endif
}

#if DEBUG
static void *cache_alloc_debugcheck_after(struct kmem_cache *cachep,
				gfp_t flags, void *objp, void *caller)
{
	if (!objp)
		return objp;
	if (cachep->flags & SLAB_POISON) {
#ifdef CONFIG_DEBUG_PAGEALLOC
		if ((cachep->buffer_size % PAGE_SIZE) == 0 && OFF_SLAB(cachep))
			kernel_map_pages(virt_to_page(objp),
					 cachep->buffer_size / PAGE_SIZE, 1);
		else
			check_poison_obj(cachep, objp);
#else
		check_poison_obj(cachep, objp);
#endif
		poison_obj(cachep, objp, POISON_INUSE);
	}
	if (cachep->flags & SLAB_STORE_USER)
		*dbg_userword(cachep, objp) = caller;

	if (cachep->flags & SLAB_RED_ZONE) {
		if (*dbg_redzone1(cachep, objp) != RED_INACTIVE ||
				*dbg_redzone2(cachep, objp) != RED_INACTIVE) {
			slab_error(cachep, "double free, or memory outside"
						" object was overwritten");
			printk(KERN_ERR
				"%p: redzone 1:0x%llx, redzone 2:0x%llx\n",
				objp, *dbg_redzone1(cachep, objp),
				*dbg_redzone2(cachep, objp));
		}
		*dbg_redzone1(cachep, objp) = RED_ACTIVE;
		*dbg_redzone2(cachep, objp) = RED_ACTIVE;
	}
#ifdef CONFIG_DEBUG_SLAB_LEAK
	{
		struct slab *slabp;
		unsigned objnr;

		slabp = page_get_slab(virt_to_head_page(objp));
		objnr = (unsigned)(objp - slabp->s_mem) / cachep->buffer_size;
		slab_bufctl(slabp)[objnr] = BUFCTL_ACTIVE;
	}
#endif
	objp += obj_offset(cachep);
	if (cachep->ctor && cachep->flags & SLAB_POISON)
		cachep->ctor(objp, cachep, 0);
#if ARCH_SLAB_MINALIGN
	if ((u32)objp & (ARCH_SLAB_MINALIGN-1)) {
		printk(KERN_ERR "0x%p: not aligned to ARCH_SLAB_MINALIGN=%d\n",
		       objp, ARCH_SLAB_MINALIGN);
	}
#endif
	return objp;
}
#else
#define cache_alloc_debugcheck_after(a,b,objp,d) (objp)
#endif

#ifdef CONFIG_FAILSLAB

static struct failslab_attr {

	struct fault_attr attr;

	u32 ignore_gfp_wait;
#ifdef CONFIG_FAULT_INJECTION_DEBUG_FS
	struct dentry *ignore_gfp_wait_file;
#endif

} failslab = {
	.attr = FAULT_ATTR_INITIALIZER,
	.ignore_gfp_wait = 1,
};

static int __init setup_failslab(char *str)
{
	return setup_fault_attr(&failslab.attr, str);
}
__setup("failslab=", setup_failslab);

static int should_failslab(struct kmem_cache *cachep, gfp_t flags)
{
	if (cachep == &cache_cache)
		return 0;
	if (flags & __GFP_NOFAIL)
		return 0;
	if (failslab.ignore_gfp_wait && (flags & __GFP_WAIT))
		return 0;

	return should_fail(&failslab.attr, obj_size(cachep));
}

#ifdef CONFIG_FAULT_INJECTION_DEBUG_FS

static int __init failslab_debugfs(void)
{
	mode_t mode = S_IFREG | S_IRUSR | S_IWUSR;
	struct dentry *dir;
	int err;

	err = init_fault_attr_dentries(&failslab.attr, "failslab");
	if (err)
		return err;
	dir = failslab.attr.dentries.dir;

	failslab.ignore_gfp_wait_file =
		debugfs_create_bool("ignore-gfp-wait", mode, dir,
				      &failslab.ignore_gfp_wait);

	if (!failslab.ignore_gfp_wait_file) {
		err = -ENOMEM;
		debugfs_remove(failslab.ignore_gfp_wait_file);
		cleanup_fault_attr_dentries(&failslab.attr);
	}

	return err;
}

late_initcall(failslab_debugfs);

#endif /* CONFIG_FAULT_INJECTION_DEBUG_FS */

#else /* CONFIG_FAILSLAB */

static inline int should_failslab(struct kmem_cache *cachep, gfp_t flags)
{
	return 0;
}

#endif /* CONFIG_FAILSLAB */

//������ص�
static inline void *____cache_alloc(struct kmem_cache *cachep, gfp_t flags)
{
	void *objp;
	struct array_cache *ac;

	check_irq_off();

	//��û���ı��ظ��ٻ���������� array_cache
	ac = cpu_cache_get(cachep);   //����ǵ�һ��cache_cache��������ͷ���0
	//����������һ��: �ȿ�local cache����û�з���Ķ�������о�ֱ���ã�û�о�ֻ��ִ��cache_alloc_refill������������������������
	if (likely(ac->avail)) {
		STATS_INC_ALLOCHIT(cachep); //FIXME: �պ���, ������#define STATS_INC_ALLOCHIT(x)	do { } while (0), ����ʲô��?
		//��avail��ֵ��1������avail��Ӧ�Ŀ��ж��������ȵģ�������ͷų����ģ����п���פ����CPU���ٻ�����
		ac->touched = 1;   //slab��������ǰ�����䣬����϶�ֻҪһ�飬���߾����ˡ�
		//����ac�Ǽ�¼������struct array_cache�ṹ���ŵ�ַ��ͨ��ac->entry[]�����Ǿ͵õ�һ����ַ��
		//�����ַ���Կ�����Ϊ local cache ���ö�����׵�ַ����������Կ������Ǵ����һ������ʼ�����,��LIFO�ṹ��
		objp = ac->entry[--ac->avail];
	} else {
		STATS_INC_ALLOCMISS(cachep);  //�պ���
		objp = cache_alloc_refill(cachep, flags);  //Ϊ���ٻ����ڴ�ռ������µ��ڴ���󣬽���ִ���������Ķ�����
	}
	return objp;
}

#ifdef CONFIG_NUMA
/*
 * Try allocating on another node if PF_SPREAD_SLAB|PF_MEMPOLICY.
 *
 * If we are in_interrupt, then process context, including cpusets and
 * mempolicy, may not apply and should not be used for allocation policy.
 */
static void *alternate_node_alloc(struct kmem_cache *cachep, gfp_t flags)
{
	int nid_alloc, nid_here;

	if (in_interrupt() || (flags & __GFP_THISNODE))
		return NULL;
	nid_alloc = nid_here = numa_node_id();
	if (cpuset_do_slab_mem_spread() && (cachep->flags & SLAB_MEM_SPREAD))
		nid_alloc = cpuset_mem_spread_node();
	else if (current->mempolicy)
		nid_alloc = slab_node(current->mempolicy);
	if (nid_alloc != nid_here)
		return ____cache_alloc_node(cachep, flags, nid_alloc);
	return NULL;
}

/*
 * Fallback function if there was no memory available and no objects on a
 * certain node and fall back is permitted. First we scan all the
 * available nodelists for available objects. If that fails then we
 * perform an allocation without specifying a node. This allows the page
 * allocator to do its reclaim / fallback magic. We then insert the
 * slab into the proper nodelist and then allocate from it.
 */
static void *fallback_alloc(struct kmem_cache *cache, gfp_t flags)
{
	struct zonelist *zonelist;
	gfp_t local_flags;
	struct zone **z;
	void *obj = NULL;
	int nid;

	if (flags & __GFP_THISNODE)
		return NULL;

	zonelist = &NODE_DATA(slab_node(current->mempolicy))
			->node_zonelists[gfp_zone(flags)];
	local_flags = (flags & GFP_LEVEL_MASK);

retry:
	/*
	 * Look through allowed nodes for objects available
	 * from existing per node queues.
	 */
	for (z = zonelist->zones; *z && !obj; z++) {
		nid = zone_to_nid(*z);

		if (cpuset_zone_allowed_hardwall(*z, flags) &&
			cache->nodelists[nid] &&
			cache->nodelists[nid]->free_objects)
				obj = ____cache_alloc_node(cache,
					flags | GFP_THISNODE, nid);
	}

	if (!obj) {
		/*
		 * This allocation will be performed within the constraints
		 * of the current cpuset / memory policy requirements.
		 * We may trigger various forms of reclaim on the allowed
		 * set and go into memory reserves if necessary.
		 */
		if (local_flags & __GFP_WAIT)
			local_irq_enable();
		kmem_flagcheck(cache, flags);
		obj = kmem_getpages(cache, flags, -1);
		if (local_flags & __GFP_WAIT)
			local_irq_disable();
		if (obj) {
			/*
			 * Insert into the appropriate per node queues
			 */
			nid = page_to_nid(virt_to_page(obj));
			if (cache_grow(cache, flags, nid, obj)) {
				obj = ____cache_alloc_node(cache,
					flags | GFP_THISNODE, nid);
				if (!obj)
					/*
					 * Another processor may allocate the
					 * objects in the slab since we are
					 * not holding any locks.
					 */
					goto retry;
			} else {
				/* cache_grow already freed obj */
				obj = NULL;
			}
		}
	}
	return obj;
}

/*
 * A interface to enable slab creation on nodeid
 */
static void *____cache_alloc_node(struct kmem_cache *cachep, gfp_t flags,
				int nodeid)
{
	struct list_head *entry;
	struct slab *slabp;
	struct kmem_list3 *l3;
	void *obj;
	int x;

	l3 = cachep->nodelists[nodeid];
	BUG_ON(!l3);

retry:
	check_irq_off();
	spin_lock(&l3->list_lock);
	entry = l3->slabs_partial.next;
	if (entry == &l3->slabs_partial) {
		l3->free_touched = 1;
		entry = l3->slabs_free.next;
		if (entry == &l3->slabs_free)
			goto must_grow;
	}

	slabp = list_entry(entry, struct slab, list);
	check_spinlock_acquired_node(cachep, nodeid);
	check_slabp(cachep, slabp);

	STATS_INC_NODEALLOCS(cachep);
	STATS_INC_ACTIVE(cachep);
	STATS_SET_HIGH(cachep);

	BUG_ON(slabp->inuse == cachep->num);

	obj = slab_get_obj(cachep, slabp, nodeid);
	check_slabp(cachep, slabp);
	l3->free_objects--;
	/* move slabp to correct slabp list: */
	list_del(&slabp->list);

	if (slabp->free == BUFCTL_END)
		list_add(&slabp->list, &l3->slabs_full);
	else
		list_add(&slabp->list, &l3->slabs_partial);

	spin_unlock(&l3->list_lock);
	goto done;

must_grow:
	spin_unlock(&l3->list_lock);
	x = cache_grow(cachep, flags | GFP_THISNODE, nodeid, NULL);
	if (x)
		goto retry;

	return fallback_alloc(cachep, flags);

done:
	return obj;
}

/**
 * kmem_cache_alloc_node - Allocate an object on the specified node
 * @cachep: The cache to allocate from.
 * @flags: See kmalloc().
 * @nodeid: node number of the target node.
 * @caller: return address of caller, used for debug information
 *
 * Identical to kmem_cache_alloc but it will allocate memory on the given
 * node, which can improve the performance for cpu bound structures.
 *
 * Fallback to other node is possible if __GFP_THISNODE is not set.
 */
static __always_inline void *
__cache_alloc_node(struct kmem_cache *cachep, gfp_t flags, int nodeid,
		   void *caller)
{
	unsigned long save_flags;
	void *ptr;
	
	//�������
	if (should_failslab(cachep, flags))
		return NULL;
	//ͬ���ǲ������:)
	cache_alloc_debugcheck_before(cachep, flags);
	local_irq_save(save_flags);  
	
	if (unlikely(nodeid == -1))
		nodeid = numa_node_id();

	if (unlikely(!cachep->nodelists[nodeid])) {
		/* Node not bootstrapped yet */
		ptr = fallback_alloc(cachep, flags);
		goto out;
	}

	if (nodeid == numa_node_id()) {
		/*
		 * Use the locally cached objects if possible.
		 * However ____cache_alloc does not allow fallback
		 * to other nodes. It may fail while we still have
		 * objects on other nodes available.
		 */
		ptr = ____cache_alloc(cachep, flags);
		if (ptr)
			goto out;
	}
	/* ___cache_alloc_node can fall back to other nodes */
	ptr = ____cache_alloc_node(cachep, flags, nodeid);
  out:
	local_irq_restore(save_flags);
	ptr = cache_alloc_debugcheck_after(cachep, flags, ptr, caller);

	return ptr;
}

static __always_inline void *
__do_cache_alloc(struct kmem_cache *cache, gfp_t flags)
{
	void *objp;

	if (unlikely(current->flags & (PF_SPREAD_SLAB | PF_MEMPOLICY))) {
		objp = alternate_node_alloc(cache, flags);
		if (objp)
			goto out;
	}
	objp = ____cache_alloc(cache, flags);

	/*
	 * We may just have run out of memory on the local node.
	 * ____cache_alloc_node() knows how to locate memory on other nodes
	 */
 	if (!objp)
 		objp = ____cache_alloc_node(cache, flags, numa_node_id());

  out:
	return objp;
}
#else

static __always_inline void *
__do_cache_alloc(struct kmem_cache *cachep, gfp_t flags)
{
	//ת����___cache_alloc
	return ____cache_alloc(cachep, flags);
}

#endif /* CONFIG_NUMA */

////������ kmalloc ���� kmem_cache_alloc��kmem_cache_zalloc�����ն��ǵ���__cache_alloc���������Ǹ������߷���slab���ܽӿ�
static __always_inline void *
__cache_alloc(struct kmem_cache *cachep, gfp_t flags, void *caller)
{
	unsigned long save_flags;
	void *objp;

	if (should_failslab(cachep, flags))
		return NULL;
	//������һ�����ǲ������
	cache_alloc_debugcheck_before(cachep, flags);
	local_irq_save(save_flags);
	//�ײ����__do_cache_alloc����
	objp = __do_cache_alloc(cachep, flags);
	local_irq_restore(save_flags);
	objp = cache_alloc_debugcheck_after(cachep, flags, objp, caller);
	prefetchw(objp);

	return objp;
}

/*
 * Caller needs to acquire correct kmem_list's list_lock
 */   //�ͷ�һ����Ŀ�Ķ���
static void free_block(struct kmem_cache *cachep, void **objpp, int nr_objects,
		       int node)
{
	int i;
	struct kmem_list3 *l3;

	//��һ�ͷŶ���������
	for (i = 0; i < nr_objects; i++) {
		void *objp = objpp[i];
		struct slab *slabp;

		//ͨ������������ַ�õ�page����ͨ��page�õ�slab
		slabp = virt_to_slab(objp);
		//���slab����
		l3 = cachep->nodelists[node];
		//�Ƚ��������ڵ�slab��������ժ��
		list_del(&slabp->list);
		check_spinlock_acquired_node(cachep, node);
		check_slabp(cachep, slabp);
		//������ŵ��� slab ��
		slab_put_obj(cachep, slabp, objp, node);
		STATS_DEC_ACTIVE(cachep);
		//���ӿ��ж������
		l3->free_objects++;
		check_slabp(cachep, slabp);

		/* fixup slab chains */
		//���slab��ȫ���ǿ��ж���
		if (slabp->inuse == 0) {
			//��������п��ж�����Ŀ�������ޣ�ֱ�ӻ������� slab ���ڴ棬���ж�������ȥÿ��slab�ж�����
			if (l3->free_objects > l3->free_limit) {
				l3->free_objects -= cachep->num;
				/* No need to drop any previously held
				 * lock here, even if we have a off-slab slab
				 * descriptor it is guaranteed to come from
				 * a different cache, refer to comments before
				 * alloc_slabmgmt.
				 */
				 //����slab����
				slab_destroy(cachep, slabp);
			} else {  //������˵�����ж�����Ŀ��û�г����������õ�����
				//ֻ�轫��slab��ӵ���slab������
				list_add(&slabp->list, &l3->slabs_free);
			}
		} else {
			/* Unconditionally move a slab to the end of the
			 * partial list on free - maximum time for the
			 * other objects to be freed, too.
			 */
			 //����slab��ӵ���������������
			list_add_tail(&slabp->list, &l3->slabs_partial);
		}
	}
}

static void cache_flusharray(struct kmem_cache *cachep, struct array_cache *ac)
{
	int batchcount;
	struct kmem_list3 *l3;
	int node = numa_node_id();

	//���ػ�����һ��ת�����ٸ����������֮ǰ�涨��
	batchcount = ac->batchcount;
#if DEBUG
	BUG_ON(!batchcount || batchcount > ac->avail);
#endif
	check_irq_off();
	//��ô˻�����������
	l3 = cachep->nodelists[node];
	spin_lock(&l3->list_lock);
	//���Ƿ���ڱ��ع�����
	if (l3->shared) {
		struct array_cache *shared_array = l3->shared;
		//���� ���� ���滹�ɳ��ص������Ŀ
		int max = shared_array->limit - shared_array->avail;
		if (max) {
			//���ֻ��Ϊmax
			if (batchcount > max)
				batchcount = max;
			//�����ػ���ǰ��ļ�������ת�뱾�ع������У���Ϊǰ��������粻�õ�
			memcpy(&(shared_array->entry[shared_array->avail]),
			       ac->entry, sizeof(void *) * batchcount);
			//���±��ع�����
			shared_array->avail += batchcount;
			goto free_done;
		}
	}

	//û�����ñ��ع����棬ֻ���ͷŶ���������
	//ע���ʱ�� batchcount ����ԭʼ�� batchcount��Ҳ����˵���԰Ѵﵽ���ػ���һ����ת�� batchcount ��Ŀ��
	//������ı��ع��������ʹ�õĻ����п��ܴﲻ�����Ŀ�꣬��Ϊ��Ҳ�� limit
	//��������ﲻ�������ڱ��ع�����Ч�ʱ������ߣ��������Ҳ�����ڵ�������������ֱ��goto free_done��
	free_block(cachep, ac->entry, batchcount, node);
free_done:
#if 0
#if STATS
	{
		int i = 0;
		struct list_head *p;

		p = l3->slabs_free.next;
		while (p != &(l3->slabs_free)) {
			struct slab *slabp;

			slabp = list_entry(p, struct slab, list);
			BUG_ON(slabp->inuse);

			i++;
			p = p->next;
		}
		STATS_SET_FREEABLE(cachep, i);
	}
#endif
#endif
	spin_unlock(&l3->list_lock);
	//���±��ػ�������
	ac->avail -= batchcount;
	//�Ѻ�����ƶ������ػ�������ǰ����
	memmove(ac->entry, &(ac->entry[batchcount]), sizeof(void *)*ac->avail);
}

/*
 * Release an obj back to its cache. If the obj has a constructed state, it must
 * be in this state _before_ it is released.  Called with disabled ints.
 */   //���պ���
static inline void __cache_free(struct kmem_cache *cachep, void *objp)
{
	//��ñ�CPU�ı��ػ���
	struct array_cache *ac = cpu_cache_get(cachep);

	check_irq_off();
	objp = cache_free_debugcheck(cachep, objp, __builtin_return_address(0));

	//NUMA��أ�Ŀǰ�汾�ǿպ���
	if (cache_free_alien(cachep, objp))
		return;

	//���濪ʼѡ���ͷ�λ�ý����ͷ�

	//���ػ����еĿ��ж���С������ʱ��ֻ�轫�����ͷŻ�entry������
	if (likely(ac->avail < ac->limit)) {
		STATS_INC_FREEHIT(cachep);
		ac->entry[ac->avail++] = objp;
		return;
	} else {
		//���Ǳ��ػ�����ж���������޵�������ȵ������ػ���
		STATS_INC_FREEMISS(cachep);
		cache_flusharray(cachep, ac);
		//����֮����Ҫ�Ѹö����ͷŸ����ػ���
		ac->entry[ac->avail++] = objp; 
	}
}

/**
 * kmem_cache_alloc - Allocate an object
 * @cachep: The cache to allocate from.
 * @flags: See kmalloc().
 *
 * Allocate an object from this cache.  The flags are only relevant
 * if the cache has no available objects.
 */
void *kmem_cache_alloc(struct kmem_cache *cachep, gfp_t flags)
{
	return __cache_alloc(cachep, flags, __builtin_return_address(0));
}
EXPORT_SYMBOL(kmem_cache_alloc);

/**
 * kmem_cache_zalloc - Allocate an object. The memory is set to zero.
 * @cache: The cache to allocate from.
 * @flags: See kmalloc().
 *
 * Allocate an object from this cache and set the allocated memory to zero.
 * The flags are only relevant if the cache has no available objects.
 */
void *kmem_cache_zalloc(struct kmem_cache *cache, gfp_t flags)
{
	//�ײ����__cache_alloc����
	void *ret = __cache_alloc(cache, flags, __builtin_return_address(0));
	if (ret)
		memset(ret, 0, obj_size(cache));
	return ret;
}
EXPORT_SYMBOL(kmem_cache_zalloc);

/**
 * kmem_ptr_validate - check if an untrusted pointer might
 *	be a slab entry.
 * @cachep: the cache we're checking against
 * @ptr: pointer to validate
 *
 * This verifies that the untrusted pointer looks sane:
 * it is _not_ a guarantee that the pointer is actually
 * part of the slab cache in question, but it at least
 * validates that the pointer can be dereferenced and
 * looks half-way sane.
 *
 * Currently only used for dentry validation.
 */
int kmem_ptr_validate(struct kmem_cache *cachep, const void *ptr)
{
	unsigned long addr = (unsigned long)ptr;
	unsigned long min_addr = PAGE_OFFSET;
	unsigned long align_mask = BYTES_PER_WORD - 1;
	unsigned long size = cachep->buffer_size;
	struct page *page;

	if (unlikely(addr < min_addr))
		goto out;
	if (unlikely(addr > (unsigned long)high_memory - size))
		goto out;
	if (unlikely(addr & align_mask))
		goto out;
	if (unlikely(!kern_addr_valid(addr)))
		goto out;
	if (unlikely(!kern_addr_valid(addr + size - 1)))
		goto out;
	page = virt_to_page(ptr);
	if (unlikely(!PageSlab(page)))
		goto out;
	if (unlikely(page_get_cache(page) != cachep))
		goto out;
	return 1;
out:
	return 0;
}

#ifdef CONFIG_NUMA
void *kmem_cache_alloc_node(struct kmem_cache *cachep, gfp_t flags, int nodeid)
{
	return __cache_alloc_node(cachep, flags, nodeid,
			__builtin_return_address(0));
}
EXPORT_SYMBOL(kmem_cache_alloc_node);

static __always_inline void *
__do_kmalloc_node(size_t size, gfp_t flags, int node, void *caller)
{
	struct kmem_cache *cachep;

	cachep = kmem_find_general_cachep(size, flags);
	if (unlikely(cachep == NULL))
		return NULL;
	return kmem_cache_alloc_node(cachep, flags, node);
}

#ifdef CONFIG_DEBUG_SLAB
void *__kmalloc_node(size_t size, gfp_t flags, int node)
{
	return __do_kmalloc_node(size, flags, node,
			__builtin_return_address(0));
}
EXPORT_SYMBOL(__kmalloc_node);

void *__kmalloc_node_track_caller(size_t size, gfp_t flags,
		int node, void *caller)
{
	return __do_kmalloc_node(size, flags, node, caller);
}
EXPORT_SYMBOL(__kmalloc_node_track_caller);
#else
void *__kmalloc_node(size_t size, gfp_t flags, int node)
{
	return __do_kmalloc_node(size, flags, node, NULL);
}
EXPORT_SYMBOL(__kmalloc_node);
#endif /* CONFIG_DEBUG_SLAB */
#endif /* CONFIG_NUMA */

/**
 * __do_kmalloc - allocate memory
 * @size: how many bytes of memory are required.
 * @flags: the type of memory to allocate (see kmalloc).
 * @caller: function caller for debug tracking of the caller
 */
static __always_inline void *__do_kmalloc(size_t size, gfp_t flags,
					  void *caller)
{
	struct kmem_cache *cachep;

	/* If you want to save a few bytes .text space: replace
	 * __ with kmem_.
	 * Then kmalloc uses the uninlined functions instead of the inline
	 * functions.
	 */
	cachep = __find_general_cachep(size, flags);  //�ҵ�һ����ʴ�С��kmem_cache������ֵ������ָ��
	if (unlikely(cachep == NULL))
		return NULL;
	return __cache_alloc(cachep, flags, caller);  //�ײ����__cache_alloc
}


#ifdef CONFIG_DEBUG_SLAB
void *__kmalloc(size_t size, gfp_t flags)
{
	return __do_kmalloc(size, flags, __builtin_return_address(0));
}
EXPORT_SYMBOL(__kmalloc);

void *__kmalloc_track_caller(size_t size, gfp_t flags, void *caller)
{
	return __do_kmalloc(size, flags, caller);
}
EXPORT_SYMBOL(__kmalloc_track_caller);

#else
void *__kmalloc(size_t size, gfp_t flags)
{
	return __do_kmalloc(size, flags, NULL);
}
EXPORT_SYMBOL(__kmalloc);
#endif

/**
 * krealloc - reallocate memory. The contents will remain unchanged.
 * @p: object to reallocate memory for.
 * @new_size: how many bytes of memory are required.
 * @flags: the type of memory to allocate.
 *
 * The contents of the object pointed to are preserved up to the
 * lesser of the new and old sizes.  If @p is %NULL, krealloc()
 * behaves exactly like kmalloc().  If @size is 0 and @p is not a
 * %NULL pointer, the object pointed to is freed.
 */
void *krealloc(const void *p, size_t new_size, gfp_t flags)
{
	struct kmem_cache *cache, *new_cache;
	void *ret;

	if (unlikely(!p))
		return kmalloc_track_caller(new_size, flags);

	if (unlikely(!new_size)) {
		kfree(p);
		return NULL;
	}

	cache = virt_to_cache(p);
	new_cache = __find_general_cachep(new_size, flags);

	/*
 	 * If new size fits in the current cache, bail out.
 	 */
	if (likely(cache == new_cache))
		return (void *)p;

	/*
 	 * We are on the slow-path here so do not use __cache_alloc
 	 * because it bloats kernel text.
 	 */
	ret = kmalloc_track_caller(new_size, flags);
	if (ret) {
		memcpy(ret, p, min(new_size, ksize(p)));
		kfree(p);
	}
	return ret;
}
EXPORT_SYMBOL(krealloc);

/**
 * kmem_cache_free - Deallocate an object
 * @cachep: The cache the allocation was from.
 * @objp: The previously allocated object.
 *
 * Free an object which was previously allocated from this
 * cache.
 */
 //    ���� !!  ֮ǰ��slab������Ķ���
void kmem_cache_free(struct kmem_cache *cachep, void *objp)
{
	unsigned long flags;
	BUG_ON(virt_to_cache(objp) != cachep);
	local_irq_save(flags);
	debug_check_no_locks_freed(objp, obj_size(cachep));
	
	//�ײ����__cache_free()����
	__cache_free(cachep, objp);

	local_irq_restore(flags);
}
EXPORT_SYMBOL(kmem_cache_free);

/**
 * kfree - free previously allocated memory
 * @objp: pointer returned by kmalloc.
 *
 * If @objp is NULL, no operation is performed.
 *
 * Don't free memory not originally allocated by kmalloc()
 * or you will run into trouble.
 */
void kfree(const void *objp)
{
	struct kmem_cache *c;
	unsigned long flags;

	if (unlikely(!objp))
		return;
	local_irq_save(flags);
	kfree_debugcheck(objp);
	
	c = virt_to_cache(objp);  
	debug_check_no_locks_freed(objp, obj_size(c));

	__cache_free(c, (void *)objp);

	local_irq_restore(flags);
}
EXPORT_SYMBOL(kfree);

unsigned int kmem_cache_size(struct kmem_cache *cachep)
{
	return obj_size(cachep);
}
EXPORT_SYMBOL(kmem_cache_size);

const char *kmem_cache_name(struct kmem_cache *cachep)
{
	return cachep->name;
}
EXPORT_SYMBOL_GPL(kmem_cache_name);

/*
 * This initializes kmem_list3 or resizes varioius caches for all nodes.
 */
 //��ʼ������ ���� �������������ʼ������Ϊ��������slab
static int alloc_kmemlist(struct kmem_cache *cachep)
{
	int node;
	struct kmem_list3 *l3;
	struct array_cache *new_shared;
	struct array_cache **new_alien = NULL;

	for_each_online_node(node) {
		//NUMA���
                if (use_alien_caches) {
                        new_alien = alloc_alien_cache(node, cachep->limit);
                        if (!new_alien)
                                goto fail;
                }

		new_shared = NULL;
		if (cachep->shared) {
			//���֧��shared���ͷ��䱾�ع�����
			new_shared = alloc_arraycache(node,
				cachep->shared*cachep->batchcount,
					0xbaadf00d);   //batchcount��ô��3131961357
			if (!new_shared) {
				free_alien_cache(new_alien);
				goto fail;
			}
		}

		//��þɵ�����
		l3 = cachep->nodelists[node];
		if (l3) {  //������ָ�벻Ϊ�գ���Ҫ���ͷžɵ���Դ
			struct array_cache *shared = l3->shared;

			spin_lock_irq(&l3->list_lock);

			if (shared)  //�ͷžɵı��ع�����
				free_block(cachep, shared->entry,
						shared->avail, node);

			//ָ���µı��ع�����
			l3->shared = new_shared;
			if (!l3->alien) {
				l3->alien = new_alien;
				new_alien = NULL;
			}
			//���㻺�����п��ж��������
			l3->free_limit = (1 + nr_cpus_node(node)) *
					cachep->batchcount + cachep->num;
			spin_unlock_irq(&l3->list_lock);
			//�ͷžɵı��ع�����ͱ��ػ���
			kfree(shared);
			free_alien_cache(new_alien);
			continue;
		}
		//���û�оɵ��������Ǿ�Ҫ����һ���µ�����
		l3 = kmalloc_node(sizeof(struct kmem_list3), GFP_KERNEL, node);
		if (!l3) {
			free_alien_cache(new_alien);
			kfree(new_shared);
			goto fail;
		}

		//��ʼ������
		kmem_list3_init(l3);
		l3->next_reap = jiffies + REAPTIMEOUT_LIST3 +
				((unsigned long)cachep) % REAPTIMEOUT_LIST3;
		l3->shared = new_shared;
		l3->alien = new_alien;
		l3->free_limit = (1 + nr_cpus_node(node)) *
					cachep->batchcount + cachep->num;
		cachep->nodelists[node] = l3;
	}
	return 0;

fail:
	if (!cachep->next.next) {
		/* Cache is not active yet. Roll back what we did */
		node--;
		while (node >= 0) {
			if (cachep->nodelists[node]) {
				l3 = cachep->nodelists[node];

				kfree(l3->shared);
				free_alien_cache(l3->alien);
				kfree(l3);
				cachep->nodelists[node] = NULL;
			}
			node--;
		}
	}
	return -ENOMEM;
}

struct ccupdate_struct {
	struct kmem_cache *cachep;
	struct array_cache *new[NR_CPUS];
};

//����ÿ��CPU��array_cache����
static void do_ccupdate_local(void *info)
{
	struct ccupdate_struct *new = info;  //���libeventһ����ǿ��ת������C++�϶�������:)
	struct array_cache *old;

	check_irq_off();
	//��þɵı��ػ���
	old = cpu_cache_get(new->cachep);

	//ָ���µ� array_cache ����new ��֮ǰ����ı��ػ��������
	new->cachep->array[smp_processor_id()] = new->new[smp_processor_id()];
	//����ɵ� array_cache ����
	new->new[smp_processor_id()] = old;
}
/*
struct ccupdate_struct {
	struct kmem_cache *cachep;
	struct array_cache *new[NR_CPUS];
};
*/
/* Always called with the cache_chain_mutex held */
//���ñ��ػ��桢���ع����������
static int do_tune_cpucache(struct kmem_cache *cachep, int limit,
				int batchcount, int shared)
{
	struct ccupdate_struct *new;
	int i;

	//�������һ�� ccupdate_struct �����㣬ע������ g_cpucache_up == FULL �ŵ��������ģ����Կ����� kmalloc
	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return -ENOMEM;

	//Ϊÿ��CPU�����µ�array_cache����
	for_each_online_cpu(i) {
		new->new[i] = alloc_arraycache(cpu_to_node(i), limit,
						batchcount);
		if (!new->new[i]) {  //���ʧ��
			for (i--; i >= 0; i--)  //commit and rollback
				kfree(new->new[i]);
			kfree(new);
			return -ENOMEM;
		}
	}
	new->cachep = cachep;   //�� new �ѾɵĻ�������Ϊ�Լ��ĳ�Ա����on_each_cpu() �����з�����¾ɵĻ������ı��ػ���
	
	//���µ�array_cache�����滻�ɵ�array_cache������֧��CPU�Ȳ�ε�ϵͳ�ϣ�����CPU����û���ͷű��ػ��棬ʹ�õ����Ǿɱ��ػ���
	//�μ�__kmem_cache_destroy()��������Ȼcpu up ʱҪ�������ñ��ػ��棬Ҳ�޼����¡�
	//����������龰; ����CPUA �� CPUB��CPUB down��destroy Cache X�����ڴ�ʱCPUB ��down״̬��
	//����Cache X�е� CPUB �ı��ػ���δ�ͷţ���һ��ʱ���CPUB�������ˣ����� cache_chain ��������cache�ı��ػ���
	//����ʱCache X�����Ѿ��ͷŻ� cache_cache���ˣ���CPUB �ı��ػ��沢δ���¡��ֹ���һ��ʱ�䣬ϵͳ��Ҫ�����µ�cache��
	//�� Cache X��������ȥ����CPUB ��Ȼ�Ǿɵı��ػ��棬��Ҫ���и���
	on_each_cpu(do_ccupdate_local, (void *)new, 1, 1);  //������do_ccpudate_local���������µ��滻�ɵ�

	check_irq_on();
	cachep->batchcount = batchcount;
	cachep->limit = limit;
	cachep->shared = shared;

	for_each_online_cpu(i) {
		struct array_cache *ccold = new->new[i];
		if (!ccold)
			continue;
		spin_lock_irq(&cachep->nodelists[cpu_to_node(i)]->list_lock);
		//�ͷžɵı��ػ����е�  ����  
		free_block(cachep, ccold->entry, ccold->avail, cpu_to_node(i));
		spin_unlock_irq(&cachep->nodelists[cpu_to_node(i)]->list_lock);
		//�ͷžɵ�array_cache
		kfree(ccold);
	}
	kfree(new);
	//��ʼ������ ���� ���������
	return alloc_kmemlist(cachep);
}

/* Called with cache_chain_mutex held always */
static int enable_cpucache(struct kmem_cache *cachep)
{
	int err;
	int limit, shared;

	/*
	 * The head array serves three purposes:
	 * - create a LIFO ordering, i.e. return objects that are cache-warm
	 * - reduce the number of spinlock operations.
	 * - reduce the number of linked list operations on the slab and
	 *   bufctl chains: array operations are cheaper.
	 * The numbers are guessed, we should auto-tune as described by
	 * Bonwick.
	 */
	 //����ÿ������������ ���� �Ĵ�С����  ���ػ���!!!  �еĶ�����Ŀ����
	if (cachep->buffer_size > 131072)
		limit = 1;
	else if (cachep->buffer_size > PAGE_SIZE)
		limit = 8;
	else if (cachep->buffer_size > 1024)
		limit = 24;
	else if (cachep->buffer_size > 256)
		limit = 54;
	else
		limit = 120;

	/*
	 * CPU bound(�������) tasks (e.g. network routing) can exhibit(չ����չʾ) cpu bound
	 * allocation behaviour: Most allocs on one cpu, most free operations    //���������ڱ�CPU���뻺�棬������CPU�ͷŻ��档(����)
	 * on another cpu. For these cases, an efficient object passing between
	 * cpus is necessary. This is provided by a shared array. The array
	 * replaces Bonwick's magazine layer.
	 * On uniprocessor(������), it's functionally equivalent(��ȵ�) (but less efficient)
	 * to a larger limit. Thus disabled by default.   //��������Ĭ���ǹرյ�
	*/
	shared = 0;
	//���ϵͳ�����ñ��ع������ж�����Ŀ
	if (cachep->buffer_size <= PAGE_SIZE && num_possible_cpus() > 1)
		shared = 8;   //����Ϊ8

#if DEBUG
	/*
	 * With debugging enabled, large batchcount lead to excessively long
	 * periods with disabled local interrupts. Limit the batchcount
	 */
	if (limit > 32)
		limit = 32;
#endif
	//���ñ��ػ���
	err = do_tune_cpucache(cachep, limit, (limit + 1) / 2, shared);
	if (err)
		printk(KERN_ERR "enable_cpucache failed for %s, error %d.\n",
		       cachep->name, -err);
	return err;
}

/*
 * Drain an array if it contains any elements taking the l3 lock only if
 * necessary. Note that the l3 listlock also protects the array_cache
 * if drain_array() is used on the shared array.
 */
void drain_array(struct kmem_cache *cachep, struct kmem_list3 *l3,
			 struct array_cache *ac, int force, int node)
{
	int tofree;

	if (!ac || !ac->avail)
		return;
	if (ac->touched && !force) {
		ac->touched = 0;
	} else {
		spin_lock_irq(&l3->list_lock);
		if (ac->avail) {
			//�����ͷŶ������Ŀ���ɼ����������֧�ֲ����ͷţ�ȡ����force��bool����
			//�� drain_cpu_caches()����ʱ��force=1����Ҫȫ���ͷŵ�
			tofree = force ? ac->avail : (ac->limit + 4) / 5;
			if (tofree > ac->avail)
				tofree = (ac->avail + 1) / 2;
			//�ͷŶ��󣬴�entryǰ�濪ʼ
			free_block(cachep, ac->entry, tofree, node);
			ac->avail -= tofree;
			//����Ķ���ǰ��
			memmove(ac->entry, &(ac->entry[tofree]),
				sizeof(void *) * ac->avail);
		}
		spin_unlock_irq(&l3->list_lock);
	}
}

/**
 * cache_reap - Reclaim memory from caches.
 * @w: work descriptor
 *
 * Called from workqueue/eventd every few seconds.
 * Purpose:
 * - clear the per-cpu caches for this CPU.
 * - return freeable pages to the main free memory pool.
 *
 * If we cannot acquire the cache chain mutex then just give up - we'll try
 * again on the next iteration.
 */
static void cache_reap(struct work_struct *w)
{
	struct kmem_cache *searchp;
	struct kmem_list3 *l3;
	int node = numa_node_id();
	struct delayed_work *work =
		container_of(w, struct delayed_work, work);

	if (!mutex_trylock(&cache_chain_mutex))
		/* Give up. Setup the next iteration. */
		goto out;

	list_for_each_entry(searchp, &cache_chain, next) {
		check_irq_on();

		/*
		 * We only take the l3 lock if absolutely necessary and we
		 * have established with reasonable certainty that
		 * we can do some work if the lock was obtained.
		 */
		l3 = searchp->nodelists[node];

		reap_alien(searchp, l3);

		drain_array(searchp, l3, cpu_cache_get(searchp), 0, node);

		/*
		 * These are racy checks but it does not matter
		 * if we skip one check or scan twice.
		 */
		if (time_after(l3->next_reap, jiffies))
			goto next;

		l3->next_reap = jiffies + REAPTIMEOUT_LIST3;

		drain_array(searchp, l3, l3->shared, 0, node);

		if (l3->free_touched)
			l3->free_touched = 0;
		else {
			int freed;
			//�ͷſ������slab
			freed = drain_freelist(searchp, l3, (l3->free_limit +
				5 * searchp->num - 1) / (5 * searchp->num));
			STATS_ADD_REAPED(searchp, freed);
		}
next:
		cond_resched();
	}
	check_irq_on();
	mutex_unlock(&cache_chain_mutex);
	next_reap_node();
out:
	/* Set up the next iteration */
	schedule_delayed_work(work, round_jiffies_relative(REAPTIMEOUT_CPUC));
}

#ifdef CONFIG_PROC_FS

static void print_slabinfo_header(struct seq_file *m)
{
	/*
	 * Output format version, so at least we can change it
	 * without _too_ many complaints.
	 */
#if STATS
	seq_puts(m, "slabinfo - version: 2.1 (statistics)\n");
#else
	seq_puts(m, "slabinfo - version: 2.1\n");
#endif
	seq_puts(m, "# name            <active_objs> <num_objs> <objsize> "
		 "<objperslab> <pagesperslab>");
	seq_puts(m, " : tunables <limit> <batchcount> <sharedfactor>");
	seq_puts(m, " : slabdata <active_slabs> <num_slabs> <sharedavail>");
#if STATS
	seq_puts(m, " : globalstat <listallocs> <maxobjs> <grown> <reaped> "
		 "<error> <maxfreeable> <nodeallocs> <remotefrees> <alienoverflow>");
	seq_puts(m, " : cpustat <allochit> <allocmiss> <freehit> <freemiss>");
#endif
	seq_putc(m, '\n');
}

static void *s_start(struct seq_file *m, loff_t *pos)
{
	loff_t n = *pos;
	struct list_head *p;

	mutex_lock(&cache_chain_mutex);
	if (!n)
		print_slabinfo_header(m);
	p = cache_chain.next;
	while (n--) {
		p = p->next;
		if (p == &cache_chain)
			return NULL;
	}
	return list_entry(p, struct kmem_cache, next);
}

static void *s_next(struct seq_file *m, void *p, loff_t *pos)
{
	struct kmem_cache *cachep = p;
	++*pos;
	return cachep->next.next == &cache_chain ?
		NULL : list_entry(cachep->next.next, struct kmem_cache, next);
}

static void s_stop(struct seq_file *m, void *p)
{
	mutex_unlock(&cache_chain_mutex);
}

static int s_show(struct seq_file *m, void *p)
{
	struct kmem_cache *cachep = p;
	struct slab *slabp;
	unsigned long active_objs;
	unsigned long num_objs;
	unsigned long active_slabs = 0;
	unsigned long num_slabs, free_objects = 0, shared_avail = 0;
	const char *name;
	char *error = NULL;
	int node;
	struct kmem_list3 *l3;

	active_objs = 0;
	num_slabs = 0;
	for_each_online_node(node) {
		l3 = cachep->nodelists[node];
		if (!l3)
			continue;

		check_irq_on();
		spin_lock_irq(&l3->list_lock);

		list_for_each_entry(slabp, &l3->slabs_full, list) {
			if (slabp->inuse != cachep->num && !error)
				error = "slabs_full accounting error";
			active_objs += cachep->num;
			active_slabs++;
		}
		list_for_each_entry(slabp, &l3->slabs_partial, list) {
			if (slabp->inuse == cachep->num && !error)
				error = "slabs_partial inuse accounting error";
			if (!slabp->inuse && !error)
				error = "slabs_partial/inuse accounting error";
			active_objs += slabp->inuse;
			active_slabs++;
		}
		list_for_each_entry(slabp, &l3->slabs_free, list) {
			if (slabp->inuse && !error)
				error = "slabs_free/inuse accounting error";
			num_slabs++;
		}
		free_objects += l3->free_objects;
		if (l3->shared)
			shared_avail += l3->shared->avail;

		spin_unlock_irq(&l3->list_lock);
	}
	num_slabs += active_slabs;
	num_objs = num_slabs * cachep->num;
	if (num_objs - active_objs != free_objects && !error)
		error = "free_objects accounting error";

	name = cachep->name;
	if (error)
		printk(KERN_ERR "slab: cache %s error: %s\n", name, error);

	seq_printf(m, "%-17s %6lu %6lu %6u %4u %4d",
		   name, active_objs, num_objs, cachep->buffer_size,
		   cachep->num, (1 << cachep->gfporder));
	seq_printf(m, " : tunables %4u %4u %4u",
		   cachep->limit, cachep->batchcount, cachep->shared);
	seq_printf(m, " : slabdata %6lu %6lu %6lu",
		   active_slabs, num_slabs, shared_avail);
#if STATS
	{			/* list3 stats */
		unsigned long high = cachep->high_mark;
		unsigned long allocs = cachep->num_allocations;
		unsigned long grown = cachep->grown;
		unsigned long reaped = cachep->reaped;
		unsigned long errors = cachep->errors;
		unsigned long max_freeable = cachep->max_freeable;
		unsigned long node_allocs = cachep->node_allocs;
		unsigned long node_frees = cachep->node_frees;
		unsigned long overflows = cachep->node_overflow;

		seq_printf(m, " : globalstat %7lu %6lu %5lu %4lu \
				%4lu %4lu %4lu %4lu %4lu", allocs, high, grown,
				reaped, errors, max_freeable, node_allocs,
				node_frees, overflows);
	}
	/* cpu stats */
	{
		unsigned long allochit = atomic_read(&cachep->allochit);
		unsigned long allocmiss = atomic_read(&cachep->allocmiss);
		unsigned long freehit = atomic_read(&cachep->freehit);
		unsigned long freemiss = atomic_read(&cachep->freemiss);

		seq_printf(m, " : cpustat %6lu %6lu %6lu %6lu",
			   allochit, allocmiss, freehit, freemiss);
	}
#endif
	seq_putc(m, '\n');
	return 0;
}

/*
 * slabinfo_op - iterator that generates /proc/slabinfo
 *
 * Output layout:
 * cache-name
 * num-active-objs
 * total-objs
 * object size
 * num-active-slabs
 * total-slabs
 * num-pages-per-slab
 * + further values on SMP and with statistics enabled
 */

const struct seq_operations slabinfo_op = {
	.start = s_start,
	.next = s_next,
	.stop = s_stop,
	.show = s_show,
};

#define MAX_SLABINFO_WRITE 128
/**
 * slabinfo_write - Tuning for the slab allocator
 * @file: unused
 * @buffer: user buffer
 * @count: data length
 * @ppos: unused
 */
ssize_t slabinfo_write(struct file *file, const char __user * buffer,
		       size_t count, loff_t *ppos)
{
	char kbuf[MAX_SLABINFO_WRITE + 1], *tmp;
	int limit, batchcount, shared, res;
	struct kmem_cache *cachep;

	if (count > MAX_SLABINFO_WRITE)
		return -EINVAL;
	if (copy_from_user(&kbuf, buffer, count))
		return -EFAULT;
	kbuf[MAX_SLABINFO_WRITE] = '\0';

	tmp = strchr(kbuf, ' ');
	if (!tmp)
		return -EINVAL;
	*tmp = '\0';
	tmp++;
	if (sscanf(tmp, " %d %d %d", &limit, &batchcount, &shared) != 3)
		return -EINVAL;

	/* Find the cache in the chain of caches. */
	mutex_lock(&cache_chain_mutex);
	res = -EINVAL;
	list_for_each_entry(cachep, &cache_chain, next) {
		if (!strcmp(cachep->name, kbuf)) {
			if (limit < 1 || batchcount < 1 ||
					batchcount > limit || shared < 0) {
				res = 0;
			} else {
				res = do_tune_cpucache(cachep, limit,
						       batchcount, shared);
			}
			break;
		}
	}
	mutex_unlock(&cache_chain_mutex);
	if (res >= 0)
		res = count;
	return res;
}

#ifdef CONFIG_DEBUG_SLAB_LEAK

static void *leaks_start(struct seq_file *m, loff_t *pos)
{
	loff_t n = *pos;
	struct list_head *p;

	mutex_lock(&cache_chain_mutex);
	p = cache_chain.next;
	while (n--) {
		p = p->next;
		if (p == &cache_chain)
			return NULL;
	}
	return list_entry(p, struct kmem_cache, next);
}

static inline int add_caller(unsigned long *n, unsigned long v)
{
	unsigned long *p;
	int l;
	if (!v)
		return 1;
	l = n[1];
	p = n + 2;
	while (l) {
		int i = l/2;
		unsigned long *q = p + 2 * i;
		if (*q == v) {
			q[1]++;
			return 1;
		}
		if (*q > v) {
			l = i;
		} else {
			p = q + 2;
			l -= i + 1;
		}
	}
	if (++n[1] == n[0])
		return 0;
	memmove(p + 2, p, n[1] * 2 * sizeof(unsigned long) - ((void *)p - (void *)n));
	p[0] = v;
	p[1] = 1;
	return 1;
}

static void handle_slab(unsigned long *n, struct kmem_cache *c, struct slab *s)
{
	void *p;
	int i;
	if (n[0] == n[1])
		return;
	for (i = 0, p = s->s_mem; i < c->num; i++, p += c->buffer_size) {
		if (slab_bufctl(s)[i] != BUFCTL_ACTIVE)
			continue;
		if (!add_caller(n, (unsigned long)*dbg_userword(c, p)))
			return;
	}
}

static void show_symbol(struct seq_file *m, unsigned long address)
{
#ifdef CONFIG_KALLSYMS
	unsigned long offset, size;
	char modname[MODULE_NAME_LEN + 1], name[KSYM_NAME_LEN + 1];

	if (lookup_symbol_attrs(address, &size, &offset, modname, name) == 0) {
		seq_printf(m, "%s+%#lx/%#lx", name, offset, size);
		if (modname[0])
			seq_printf(m, " [%s]", modname);
		return;
	}
#endif
	seq_printf(m, "%p", (void *)address);
}

static int leaks_show(struct seq_file *m, void *p)
{
	struct kmem_cache *cachep = p;
	struct slab *slabp;
	struct kmem_list3 *l3;
	const char *name;
	unsigned long *n = m->private;
	int node;
	int i;

	if (!(cachep->flags & SLAB_STORE_USER))
		return 0;
	if (!(cachep->flags & SLAB_RED_ZONE))
		return 0;

	/* OK, we can do it */

	n[1] = 0;

	for_each_online_node(node) {
		l3 = cachep->nodelists[node];
		if (!l3)
			continue;

		check_irq_on();
		spin_lock_irq(&l3->list_lock);

		list_for_each_entry(slabp, &l3->slabs_full, list)
			handle_slab(n, cachep, slabp);
		list_for_each_entry(slabp, &l3->slabs_partial, list)
			handle_slab(n, cachep, slabp);
		spin_unlock_irq(&l3->list_lock);
	}
	name = cachep->name;
	if (n[0] == n[1]) {
		/* Increase the buffer size */
		mutex_unlock(&cache_chain_mutex);
		m->private = kzalloc(n[0] * 4 * sizeof(unsigned long), GFP_KERNEL);
		if (!m->private) {
			/* Too bad, we are really out */
			m->private = n;
			mutex_lock(&cache_chain_mutex);
			return -ENOMEM;
		}
		*(unsigned long *)m->private = n[0] * 2;
		kfree(n);
		mutex_lock(&cache_chain_mutex);
		/* Now make sure this entry will be retried */
		m->count = m->size;
		return 0;
	}
	for (i = 0; i < n[1]; i++) {
		seq_printf(m, "%s: %lu ", name, n[2*i+3]);
		show_symbol(m, n[2*i+2]);
		seq_putc(m, '\n');
	}

	return 0;
}

const struct seq_operations slabstats_op = {
	.start = leaks_start,
	.next = s_next,
	.stop = s_stop,
	.show = leaks_show,
};
#endif
#endif

/**
 * ksize - get the actual amount of memory allocated for a given object
 * @objp: Pointer to the object
 *
 * kmalloc may internally round up allocations and return more memory
 * than requested. ksize() can be used to determine the actual amount of
 * memory allocated. The caller may use this additional memory, even though
 * a smaller amount of memory was initially specified with the kmalloc call.
 * The caller must guarantee that objp points to a valid object previously
 * allocated with either kmalloc() or kmem_cache_alloc(). The object
 * must not be freed during the duration of the call.
 */
size_t ksize(const void *objp)
{
	if (unlikely(objp == NULL))
		return 0;

	return obj_size(virt_to_cache(objp));
}
