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
 * for a slab, or allocated from an general cache.     //¸Ã¹ÜÀíÕß¿ÉÒÔÔÚslabÍ·²¿ÉêÇëÄÚ´æ£¬Ò²¿ÉÒÔ´Ógeneral cache´¦ÉêÇëÄÚ´æ¡£
 * Slabs are chained into three list: fully used, partial, fully free slabs.
 */
struct slab {
	struct list_head list;       //ÓÃÓÚ½«slabÄÉÈëÈıÁ´Ö®ÖĞ
	unsigned long colouroff; //¸ÃslabµÄ×ÅÉ«Æ«ÒÆ
	void *s_mem;		//Ö¸ÏòslabÖĞµÄµÚÒ»¸ö¶ÔÏó
	unsigned int inuse;		//slabÖĞÒÑ·ÖÅä³öÈ¥µÄ¶ÔÏóÊıÄ¿
	kmem_bufctl_t free;     //ÏÂÒ»¸ö¿ÕÏĞ¶ÔÏóµÄÏÂ±ê
	unsigned short nodeid; //½Úµã±êÊ¶ºÅ
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
 */  //array_cacheÖĞ¶¼ÊÇper-cpuÊı¾İ£¬²»»á¹²Ïí£¬Õâ¿ÉÒÔ¼õÉÙNUMA¼Ü¹¹ÖĞ¶àCPUµÄ×ÔĞıËø¾ºÕù
struct array_cache {  
	unsigned int avail;     //±¾µØ»º´æÖĞ¿ÉÓÃµÄ¿ÕÏĞ¶ÔÏóÊı
	unsigned int limit;     //±¾µØ»º´æ¿ÕÏĞ¶ÔÏóÊıÄ¿ÉÏÏŞ
	unsigned int batchcount;   //±¾µØ»º´æÒ»´ÎĞÔ×ªÈëºÍ×ª³öµÄ¶ÔÏóÊıÁ¿
	unsigned int touched;     //±êÊ¶±¾µØ¶ÔÏóÊÇ·ñ×î½ü±»Ê¹ÓÃ
	spinlock_t lock;       //×ÔĞıËø
	void *entry[0];	/*    //ÕâÊÇÒ»¸öÈáĞÔÊı×é£¬±ãÓÚ¶ÔºóÃæÓÃÓÚ¸ú×Ù¿ÕÏĞ¶ÔÏóµÄÖ¸ÕëÊı×éµÄ·ÃÎÊ
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
	struct list_head slabs_partial;//²¿·ÖÂúµÄslabÁ´±í£¬Ò²¾ÍÊÇ²¿·Ö¶ÔÏóßÂ·ÖÅä³öÈ¥µÄslab
	struct list_head slabs_full;    //ÂúslabÁ´±í
	struct list_head slabs_free;  //¿ÕslabÁ´±í
	unsigned long free_objects;  //¿ÕÏĞ¶ÔÏóµÄ¸öÊı
	unsigned int free_limit;        //¿ÕÏĞ¶ÔÏóµÄÉÏÏŞÊıÄ¿
	unsigned int colour_next;	/* Per-node cache coloring */   //Ã¿¸ö½ÚµãÏÂÒ»¸öslabÊ¹ÓÃµÄÑÕÉ«
	spinlock_t list_lock;      
	struct array_cache *shared;	/* shared per node */    //Ã¿¸ö½Úµã¹²Ïí³öÈ¥µÄ»º´æ
	struct array_cache **alien;	/* on other nodes */    //FIXME: ÆäËû½ÚµãµÄ»º´æ£¬Ó¦¸ÃÊÇ¹²ÏíµÄ
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
static __always_inline int index_of(const size_t size) //Õâ¸öº¯ÊıÓÃÀ´Ñ¡Ôñ´óĞ¡µÄ£¬¿É×÷Îªmallloc_sizeµÄ²ÎÊı  
{
	extern void __bad_size(void);

	if (__builtin_constant_p(size)) {
		int i = 0;

#define CACHE(x) \
	if (size <=x) \     //ÊÊÅäÒ»¸ö¸Õ×ã¹»ÈİÄÉsizeµÄ´óĞ¡
		return i; \
	else \
		i++;   //²»³É¹¦£¬Ôö´ó¼ÌĞøÊÊÅä
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
	.name = "kmem_cache",  //ÎÔ²Û£¬Ãû×Ö¾Í½Ğ kmem

*/
//»º´æÆ÷
struct kmem_cache {
/* 1) per-cpu data, touched during every alloc/free */
	//per-cpuÊı¾İ£¬±¾µØ»º´æ£¬¼ÇÂ¼ÁË±¾µØ¸ßËÙ»º´æµÄĞÅÏ¢£¬Ò²ÓÃÓÚ¸ú×Ù×î½üÊÍ·ÅµÄ¶ÔÏó£¬Ã¿´Î·ÖÅäºÍÊÍ·Å¶¼ÏÈ·ÃÎÊËü
	struct array_cache *array[NR_CPUS];
/* 2) Cache tunables. Protected by cache_chain_mutex */
	unsigned int batchcount;  //±¾µØ»º´æ×ªÈë»ò×ª³öµÄ´óÅú¶ÔÏóÊıÄ¿
	unsigned int limit;            //±¾µØ»º´æ¿ÕÏĞ¶ÔÏóµÄ×î´óÊıÄ¿
	unsigned int shared;         //ÊÇ·ñÖ§³Ö±¾½Úµã¹²ÏíÒ»²¿·ÖcacheµÄ±êÖ¾£¬Èç¹ûÖ§³Ö£¬ÄÇ¾Í´æÔÚ±¾µØ¹²Ïí»º´æ

	unsigned int buffer_size;   //¹ÜÀíµÄ¶ÔÏó´óĞ¡
	u32 reciprocal_buffer_size;  //ÉÏÃæÕâ¸ö´óĞ¡µÄµ¹Êı£¬Ã²ËÆÀûÓÃÕâ¸ö¿ÉÓÃÅ£¶Ùµü´ú·¨ÇóÊ²Ã´:)
/* 3) touched by every alloc & free from the backend */

	unsigned int flags;		//cache µÄÓÀ¾Ã±êÖ¾
	unsigned int num;		//Ò»¸ö slab Ëù°üº¬µÄ¶ÔÏóÊıÄ¿!!! Ò²¾ÍÊÇËµ£¬kmem_cache ¿ØÖÆÁËËüËù¹ÜÏ½µÄËùÓĞ¶ÔÏó´óĞ¡ÊıÄ¿¼°ÆäËûÊôĞÔ

/* 4) cache_grow/shrink */
	/* order of pgs per slab (2^n) */
	unsigned int gfporder;  //Ò»¸öslabËù°üº¬µÄ page µÄ¶ÔÊı£¬Ò²¾ÍÊÇÒ»¸öslab·ÖÅä 2^gfporder ¸ö page

	/* force GFP flags, e.g. GFP_DMA */
	gfp_t gfpflags;     //Óë»ï°éÏµÍ³½»»¥Ê±ËùÌá¹©µÄ·ÖÅä±êÊ¶

	size_t colour;			/* cache colouring range */ //×ÅÉ«µÄ·¶Î§°É
	unsigned int colour_off;	/* colour offset */  //×ÅÉ«µÄÆ«ÒÆÁ¿
	struct kmem_cache *slabp_cache;//Èç¹û½«slabÃèÊö·û´æ´¢ÔÚÍâ²¿£¬¸ÃÖ¸ÕëÖ¸ÏòslabÃèÊö·ûµÄ cache£¬·ñÔòÎª NULL
	unsigned int slab_size;    // slab µÄ´óĞ¡
	unsigned int dflags;		/* dynamic flags */  //FIXME: ¶¯Ì¬±êÖ¾

	/* constructor func */
	void (*ctor) (void *, struct kmem_cache *, unsigned long);

/* 5) cache creation/removal */
	const char *name;     //Ãû×Ö:)
	struct list_head next;    //¹¹ÔìÁ´±íËùÓÃ

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
	 //nodelists ÓÃÓÚ×éÖ¯ËùÓĞ½ÚµãµÄ slab£¬Ã¿¸ö½ÚµãÑ°ÕÒ×Ô¼ºÓµÓĞµÄ cache ½«×Ô¼º×÷Îª nodelists µÄÏÂ±ê¾Í¿ÉÒÔ·ÃÎÊÁË
	 //²»¹ı´ÓÕâÀï·ÃÎÊµÄÖ»ÊÇÃ¿¸ö½ÚµãµÄ slab ¹ÜÀíµÄ cache ÒÔ¼°Ã¿¸ö½ÚµãµÄ¹²Ïí cache £¬per-cpu cache ÊÇÉÏÃæµÄarrayÊı×é¹ÜÀíµÄ
	 //µ±È»£¬Õë¶ÔÍ¬Ò»¸ö»º´æÆ÷kmem_cache£¬Ëü¹ÜÀíµÄÊÇÍ¬Ò»ÖÖ¶ÔÏó£¬ËùÒÔÍ¨¹ı±¾ kmem_cache ½á¹¹ÌåµÄ nodelists ³ÉÔ±·ÃÎÊµÄÒ²¾ÍÖ»ÊÇÍ¬ÖÖ¶ÔÏóµÄ cache
	struct kmem_list3 *nodelists[MAX_NUMNODES];  
	/*
	 * Do not add fields after nodelists[]å
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
	return slab->s_mem + cache->buffer_size * idx;  //Ê×µØÖ· + n * index
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
	//¼ÆËã¶ÔÏóÓëslabÖĞÊ×¸ö¶ÔÏóµÄÆ«ÒÆ
	u32 offset = (obj - slab->s_mem);
	//Í¨¹ıÆ«ÒÆ¼ÆËãÆäÔÚ kmem_bufctl_t Êı×éÖĞµÄË÷Òı
	return reciprocal_divide(offset, cache->reciprocal_buffer_size);  //àÛ£¬Å£¶Ùµü´ú·¨ÓÃÉÏÁË
}

/*
 * These are the default caches for kmalloc. Custom caches can have other sizes.
 */
struct cache_sizes malloc_sizes[] = {  //Í¨ÓÃ»º´æÆ÷µÄ´óĞ¡ÓÉmalloc_size±í¾ö¶¨
#define CACHE(x) { .cs_size = (x) },
#include <linux/kmalloc_sizes.h>   //ÖÕÓÚÃ÷°×ÕâÊÇÊ²Ã´ÓÃ·¨ÁË
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
//Õâ¾ÍÊÇ¾²Ì¬¶¨ÒåÁËµÚÒ»¸ö¼´Í¨ÓÃ»º´æÆ÷
static struct kmem_cache cache_cache = {
	.batchcount = 1,
	.limit = BOOT_CPUCACHE_ENTRIES,
	.shared = 1,
	.buffer_size = sizeof(struct kmem_cache),
	.name = "kmem_cache",  //ÎÔ²Û£¬Ãû×Ö¾Í½Ğ kmem_cache
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

	//ÕâÊÇ±¾º¯ÊıÎ¨Ò»ÓĞÓÃµÄµØ·½£¬¼ÆËã³ö cache µÄ×îÊÊ´óĞ¡
	//csizep ËùÖ¸µÄ¾ÍÊÇ malloc_size Êı×é£¬¸ÃÊı×éÄÚ²¿¾ÍÊÇÒ»´ó¶Ñcache´óĞ¡µÄÖµ£¬×îĞ¡ÎªLI_CACHE_BYTESÎª32£¬×î´óÎª2^25=4096*8192(ÎªÊ²Ã´ÊÇ25´Î·½?)
	while (size > csizep->cs_size)   //·´Õı¾ÍÊÇ¸÷ÖÖ¶ÔÏóµÄ´óĞ¡ÓÉĞ¡µ½´óÈÎÄãÑ¡£¬²»¹ıÊÇÑ¡Ôñ×îºÏÊÊµÄ
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

	return csizep->cs_cachep;   //·µ»Ø¶ÔÓ¦cache_sizesµÄcs_cachep£¬Ò²¾ÍÊÇÒ»¸ökmem_cacheµÄÃèÊö·ûÖ¸Õë
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
 //¼ÆËã¶ÔÏóµÄÊıÄ¿ÒÔ¼°ËéÆ¬µÄ´óĞ¡£¬estimate ÊÇ¹À¼ÆµÄÒâË¼£¬µÚÈı¸ö²ÎÊı align = cache_line_size()
static void cache_estimate(unsigned long gfporder, size_t buffer_size,
			   size_t align, int flags, size_t *left_over,
			   unsigned int *num)
{
	int nr_objs;
	size_t mgmt_size;
	
	//PAGE_SIZE´ú±íÒ»¸öÒ³Ãæ£¬slab_size¼ÇÂ¼ĞèÒª¶àÉÙ¸öÒ³Ãæ£¬ÏÂÃæÕâ¸öÊ½×ÓµÈĞ§ÓÚ(1(Ò³Ãæ) * (2 ^ gfporder))
	size_t slab_size = PAGE_SIZE << gfporder;  //FIXME: #define PAGE_SIZE 0x400 (¼´1024)£¬ÄÑµÀÒ»¸öÒ³ÃæÊÇ 1K £¬ÔõÃ´¿ÉÄÜ?

	/*
	 * The slab management structure can be either off the slab or  //ÓĞoff-slabºÍon-slabÁ½ÖÖ·½Ê½
	 * on it. For the latter case, the memory allocated for a
	 * slab is used for:   //Õâ¶ÎÄÚ´æ±»ÓÃÀ´´æ´¢:
	 *
	 * - The struct slab      //slab½á¹¹Ìå
	 * - One kmem_bufctl_t for each object    //Ã¿¸ö¶ÔÏóµÄkmem_bufctl_t
	 * - Padding to respect alignment of @align  //¶ÔÆëµÄ´óĞ¡
	 * - @buffer_size bytes for each object   //Ã¿¸ö¶ÔÏóµÄ´óĞ¡
	 *
	 * If the slab management structure is off the slab, then the
	 * alignment will already be calculated into the size. Because   //Èç¹ûÊÇoff-slab£¬alignÔçÒÑ±»¼ÆËã³öÀ´
	 * the slabs are all pages aligned, the objects will be at the   //ÒòÎªËùÓĞµÄÒ³Ãæ¶ÔÆë¹ıÁË£¬¶ÔÏóÉêÇëÊ±»á´¦ÔÚÕıÈ·µÄÎ»ÖÃ
	 * correct alignment when allocated.
	 */
	 //¶ÔÓÚÍâÖÃslab£¬Ã»ÓĞslab¹ÜÀí¶ÔÏóÎÊÌâ£¬Ö±½ÓÓÃÉêÇë¿Õ¼ä³ıÒÔ¶ÔÏó´óĞ¡¾ÍÊÇ¶ÔÏó¸öÊı
	if (flags & CFLGS_OFF_SLAB) {
		//ÍâÖÃslab²»´æÔÚ¹ÜÀí¶ÔÏó£¬È«²¿ÓÃÓÚ´æ´¢slab¶ÔÏó
		mgmt_size = 0;
		//ËùÒÔ¶ÔÏó¸öÊı = slab´óĞ¡ / ¶ÔÏó´óĞ¡
		nr_objs = slab_size / buffer_size;    //×¢Òâbuffer_sizeÒÑ¾­ºÍcache line¶ÔÆë¹ıÁË

		//¶ÔÏó¸öÊı²»Ğí³¬ÏŞ
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
		 //ÄÚÖÃÊ½slab£¬slab¹ÜÀí¶ÔÏóÓëslab¶ÔÏó¶¼ÔÚÒ»Æ¬ÄÚ´æÖĞ£¬´ËÊ±slabÒ³Ãæ°üº¬:
		 //Ò»¸östruct slab ¶ÔÏó£¬Ò»¸ökmem_bufctl_t ÀàĞÍÊı×é(kmem_bufctl_t Êı×éµÄÏîÊıºÍslab¶ÔÏóÊıÄ¿ÏàÍ¬)
		//slab´óĞ¡ĞèÒª¼õÈ¥¹ÜÀí¶ÔÏó´óĞ¡£¬ËùÒÔ¶ÔÏó¸öÊıÎª Ê£Óà´óĞ¡ / (Ã¿¸ö¶ÔÏó´óĞ¡ + sizeof(kmem_bufctl_t)), ËüÃÇÊÇÒ»Ò»Æ¥ÅäµÄ¹ØÏµ
		nr_objs = (slab_size - sizeof(struct slab)) /
			  (buffer_size + sizeof(kmem_bufctl_t));

		/*
		 * This calculated number will be either the right
		 * amount, or one greater than what we want.
		 */
		 //Èç¹û¶ÔÆëºó³¬¹ıslab ×Ü´óĞ¡ £¬ĞèÒª¼õÈ¥Ò»¸ö¶ÔÏó
		if (slab_mgmt_size(nr_objs, align) + nr_objs*buffer_size
		       > slab_size)
			nr_objs--;

		//¶ÔÏó¸öÊı²»Ğí³¬ÏŞ
		if (nr_objs > SLAB_LIMIT)
			nr_objs = SLAB_LIMIT;

		//¼ÆËã  ¹ÜÀí¶ÔÏóÒÔ»º´æĞĞ  ¶ÔÆëºóµÄ×Ü´óĞ¡
		mgmt_size = slab_mgmt_size(nr_objs, align);
	}

	
	//µÃ³öslab×îÖÕ¶ÔÏó¸öÊı
	*num = nr_objs;


	//Ç°ÃæÒÑ¾­µÃµ½ÁËslab¹ÜÀí¶ÔÏó´óĞ¡(ÍâÖÃÎª0£¬ÄÚÖÃÒ²ÒÑ¼ÆËã)£¬ÕâÑù¾Í¿ÉÒÔ×îÖÕµÄ³öslab×îÖÕÀË·Ñ¿Õ¼ä´óĞ¡
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

//·ÖÅä±¾µØ»º´æ¶ÔÏó
//²ÎÊı·Ö±ğÊÇ; node:NUMAÄÚ´æ½Úµãid 
//            entries: entryÊı×éÔªËØ¸öÊı
//            batchcount: ±¾µØ»º´æÒ»´ÎĞÔ×ªÈë×ªÈëÊıÄ¿µÄ¶àÉÙ
static struct array_cache *alloc_arraycache(int node, int entries,
					    int batchcount)
{
	//array_cacheºóÃæ½ô½Ó×ÅµÄÊÇentryÊı×é£¬ºÏÔÚÒ»ÆğÉêÇëÄÚ´æ
	int memsize = sizeof(void *) * entries + sizeof(struct array_cache);
	struct array_cache *nc = NULL;

	//·ÖÅäÒ»¸ö±¾µØ»º´æ¶ÔÏó£¬kmalloc ÊÇÖ±½Ó´ÓÍ¨ÓÃ»º´æÆ÷À´·ÖÅäµÄ
	nc = kmalloc_node(memsize, GFP_KERNEL, node);
	if (nc) {
		//³õÊ¼»¯±¾µØ»º´æ
		nc->avail = 0;
		nc->limit = entries;   //ÎªÊ²Ã´ÓĞlimitµÄÔ­Òò£¬ÉêÇëµÄÊ±ºò´óĞ¡ÒÑ¾­Ğ´ËÀÁË
		nc->batchcount = batchcount;   //³õÊ¼»¯batchcount
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
	// min(from->avail, max) ÕâÊÇÑ¡Ôñ³öfromÄÜ¸øµÄÔªËØÊıÄ¿£¬×î¶àÎªmax¸ö
	//min(..., to->limit - to->avail) ÊÇÖ¸¸øµÄÔªËØÊıÄ¿²»³¬¹ı ac µ±Ç°»¹¿ÉÈİÄÉµÄÊıÄ¿¡£
	int nr = min(min(from->avail, max), to->limit - to->avail);  

	if (!nr)   //Èç¹ûÒ»¸ö¶¼¸ø²»³ö£¬ÄÇÃ´·µ»Ø0¡£
		return 0;

	memcpy(to->entry + to->avail, from->entry + from->avail -nr,
			sizeof(void *) *nr);   //·ñÔòÖ±½Ómemcpy

	from->avail -= nr;  //¸üĞÂ¸÷¸öÖ¸±ê
	to->avail += nr;
	to->touched = 1;
	return nr;  //·µ»Ø¸øµÄÔªËØÊıÄ¿
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
 //Í¨ÓÃ»º´æÆ÷³õÊ¼»¯º¯Êı£¬·ñÔòÎÒÃÇÎŞ·¨Ê¹ÓÃkmalloc
void __init kmem_cache_init(void)
{
	size_t left_over;
	struct cache_sizes *sizes;
	struct cache_names *names;
	int i;
	int order;
	int node;

	if (num_possible_nodes() == 1)  //Ã²ËÆµ±Ç°Õâ¸ö°æ±¾»¹²»Ö§³Ö²Î¼û#define num_possible_nodes()	1
		use_alien_caches = 0;             //ºóĞølinux°æ±¾»áÖ§³Ö
		
	//ÔÚslab³õÊ¼»¯ºÃÖ®Ç°£¬ÎŞ·¨Í¨¹ıkmalloc·ÖÅä³õÊ¼»¯¹ı³ÌÖĞµÄÒ»Ğ©±ØÒª¶ÔÏó£¬Ö»ÄÜÊ¹ÓÃ¾²Ì¬µÄÈ«¾Ö±äÁ¿
	//´ıslab³õÊ¼»¯ºóÆÚ£¬ÔÙÊ¹ÓÃkmalloc¶¯Ì¬·ÖÅä¶ÔÏóÌæ»»È«¾Ö±äÁ¿!!
	
	//ÈçÇ°ËùÊö£¬ÏÈ½èÓÃ initkem_list3 ´úÌæslabµÄÈıÁ´£¬Ã¿¸ö½Úµã¶ÔÓ¦Ò»×éÈıÁ´
	//initkmem_list3 ÊÇ¸öslabÈıÁ´Êı×é£¬ÕâÀïÑ­»·³õÊ¼»¯Ã¿¸ö½ÚµãµÄÈıÁ´ 
	for (i = 0; i < NUM_INIT_LISTS; i++) {       //NUM_INIT_LIST ÊÇÔõÃ´ÇóµÄ£¬ÎªÊ²Ã´µÈÓÚ2 * MAX_NODES+1 ????????
		kmem_list3_init(&initkmem_list3[i]);
		if (i < MAX_NUMNODES)
		//È«¾Ö±äÁ¿cache_cacheÖ¸ÏòµÄslab cache°üº¬ËùÓĞµÄkmem_cache¶ÔÏó£¬²»°üº¬cache_cache±¾Éí
		//ÕâÀï³õÊ¼»¯ËùÓĞÄÚ´æ½Úµãkmem_cacheµÄslabÈıÁ´Îª¿Õ
			cache_cache.nodelists[i] = NULL;
	}
	
	/*
	 * Fragmentation resistance on low memory - only use bigger
	 * page orders on machines with more than 32MB of memory.
	 */
	 //È«²¿±äÁ¿slab_break_gfp_orderÎªÃ¿¸öslab×î¶àÕ¼ÓÃ¼¸¸öÒ³Ãæ£¬ÓÃÀ´¼õÉÙËéÆ¬¡£
	 //×Ü½áÆğÀ´ÒâË¼¾ÍÊÇÊÇ: (1)Èç¹ûÎïÀí¿ÉÓÃÄÚ´æ´óÓÚ32MB£¬Ò²¾ÍÊÇ¿ÉÓÃÄÚ´æ³äÔ£µÄÊ±ºò£¬BREAK_GFP_ORDER_HIÕâ¸öºêµÄÖµÊÇ1£¬
	 //Õâ¸öÊ±ºòÃ¿¸öslab×î¶àÕ¼ÓÃÁ½¸öÒ³Ãæ£¬²»¹ı´ËÊ±²»ÄÜºá¿ç3¸öÒ³Ãæ£¬³ı·Ç¶ÔÏó´óĞ¡´óÓÚ8192KÊ±²Å¿ÉÒÔ(Ò»¸öÒ³Ãæ´óĞ¡ÊÇ4K£¬Ò²¾ÍÊÇ4096);
	 //(2)Èç¹û¿ÉÓÃÄÚ´æ²»´óÓÚ32MB,ÄÇÃ´BREAK_GFP_ORDER_HIÖµÎª0£¬×î¶àÒ²¾ÍÊÇÔÊĞíÒ»¸öÒ³Ãæ£¬³ı·Ç¶ÔÏó³¬ÁË£¬·ñÔò²»ÄÜºá¿ç
	if (num_physpages > (32 << 20) >> PAGE_SHIFT)
		slab_break_gfp_order = BREAK_GFP_ORDER_HI;   //ÓÃÀ´È·¶¨slabµÄ×î´ó´óĞ¡

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

	node = numa_node_id();   //»ñÈ¡½Úµãid,È¡µÃµÄÖµÎª0

	/* 1) create the cache_cache */
	//³õÊ¼»¯cache_chain Îª kmem_cache Á´±íÍ·²¿
	INIT_LIST_HEAD(&cache_chain);
	list_add(&cache_cache.next, &cache_chain);
	//ÉèÖÃcache×ÅÉ«µÄÆ«ÒÆÁ¿»ù±¾Öµ£¬Ò²¾ÍÊÇL1»º´æĞĞµÄ´óĞ¡
	cache_cache.colour_off = cache_line_size(); //ºê¶¨ÒåL1»º´æĞĞµÄ´óĞ¡ #define cache_line_size() L1_CACHE_BYTES
	//³õÊ¼»¯cache_cacheµÄper-CPU cache£¬Í¬ÑùÕâÀïÒ²²»ÄÜÊ¹ÓÃkmalloc£¬ĞèÒªÊ¹ÓÃ¾²Ì¬·ÖÅäµÄÈ«¾Ö±äÁ¿initarray_cache
	cache_cache.array[smp_processor_id()] = &initarray_cache.cache;
	//³õÊ¼»¯slabÁ´±í£¬ÓÃÈ«¾Ö±äÁ¿£¬ÕâÀïCACHE_CACHEÖµÎª0£¬ÊÇÒòÎªcache_cache¾ÍÊÇÏµÍ³µÚÒ»¸öµÄ»º´æÆ÷
	cache_cache.nodelists[node] = &initkmem_list3[CACHE_CACHE];

	/*
	 * struct kmem_cache size depends on nr_node_ids, which
	 * can be less than MAX_NUMNODES.
	 */
	 //buffer_sizeÔ­Ö¸ÓÃÀ´·ÖÅäµÄ¶ÔÏó´óĞ¡£¬ÓÉÓÚcache_cacheÊÇÓÃÀ´×ö kmem_cache µÄ·ÖÅäÆ÷µÄ£¬ËùÒÔ buffer_size µÄ´óĞ¡¾ÍÊÇ kmem_cache µÄ´óĞ¡
	 //×¢ÒâÈáĞÔÊı×éµÄ¼ÆËã·½·¨£¬nodelists²»Õ¼¾İ kmem_cacheµÄ´óĞ¡£¬ËùÒÔÒª·Ö¿ª¼ÆËã£¬²¢ÇÒ×¢ÒânodelistsÊı×éÔÚUMA¼Ü¹¹Ö»ÓĞÒ»¸ö½Úµã£¬ËùÒÔÖ»ÓĞ1¸ökmem_list3µÄÖ¸Õë
	cache_cache.buffer_size = offsetof(struct kmem_cache, nodelists) +
				 nr_node_ids * sizeof(struct kmem_list3 *);
#if 0 
#if DEBUG
	cache_cache.obj_size = cache_cache.buffer_size;
#endif
#endif

	//×¢ÒâÕâÀïÓÖÒ»´Î¼ÆËãÁË buffer_size µÄ´óĞ¡£¬Í¨¹ıÕâ´Î¼ÆËã½«buffer_sizeÒÔ »º´æĞĞ Îªµ¥Î»½øĞĞÉÏ±ß½ç¶ÔÆë
	//¼ÆËã·ÖÅäµÄ¶ÔÏóÓëcache lineµÄ´óĞ¡¶ÔÆëºóµÄ´óĞ¡
	cache_cache.buffer_size = ALIGN(cache_cache.buffer_size,
					cache_line_size());
	//¼ÆËã¶ÔÏó´óĞ¡µÄµ¹Êı£¬ÓÃÓÚ¼ÆËã¶ÔÏóÔÚslabÖĞµÄË÷Òı
	cache_cache.reciprocal_buffer_size =
		reciprocal_value(cache_cache.buffer_size);

	//¼ÆËãcache_cacheµÄÊ£Óà¿Õ¼äÒÔ¼°slabÖĞ¶ÔÏóµÄÊıÄ¿£¬order¾ö¶¨ÁËslabµÄ´óĞ¡(PAGE_SIZEE<<order)
	for (order = 0; order < MAX_ORDER; order++) { //#define MAX_ORDER 11
		
		cache_estimate(order, cache_cache.buffer_size,  //buffer_sizeÒÑ¾­ºÍcache line¶ÔÆë¹ı
			cache_line_size(), 0, &left_over, &cache_cache.num);  //¼ÆËãcache_cacheµÄ¶ÔÏóÊıÄ¿
			
		if (cache_cache.num) //´ÓĞ¡µ½´ó³¢ÊÔ£¬×Ü»áÕÒµ½ÊÊºÏ»º´æÆ÷´óĞ¡µÄÒ³Ãæ´óĞ¡£¬ÕâÊ±ºò»á¼ÆËã³ö»º´æÆ÷¿É·ÖÅä¶ÔÏóµÄÊıÄ¿£¬´ËÊ±num¾Í²»Îª0
			break;
	}
	BUG_ON(!cache_cache.num);  //¶ÏÑÔ
	
	cache_cache.gfporder = order;  //gfporder±íÊ¾±¾slab°üº¬2^gfproder¸öÒ³Ãæ£¬×¢ÒâÕâÀï¾Í´ÓÉÏÃæµÄorder¸³Öµ¸ø gfporder

	//cache_cacheµÄcolour_off ¾ÍÊÇÉÏÃæ³õÊ¼»¯µÄ L1_CACHE_BYTES£¬¼ÈÈ»ÒÑ¾­ÖªµÀ L1 »º´æĞĞµÄ´óĞ¡£¬ÎÒÃÇÉÏ²½ÓÖ¼ÆËã³öÁËÀË·Ñ¿Õ¼äµÄ´óĞ¡
	//ÄÇÃ´ÓÃÀË·Ñ¿Õ¼äµÄ´óĞ¡ / L1 »º´æĞĞµÄ´óĞ¡£¬¾ÍµÃ³öµ±Ç°¿ÉÓÃ colour µÄÊıÄ¿£¬Õâ¸öÊıÄ¿ÊÇÀÛ¼ÓÇÒÑ­»·µÄ£¬¿ÉÒÔÎª0
	cache_cache.colour = left_over / cache_cache.colour_off;   //È·¶¨¿ÉÓÃ colour µÄÊıÄ¿£¬µ¥Î»ÊÇ colour_off


	//È·¶¨slabÃèÊö·ûÒÔ¼°kmem_bufctl_tÊı×é Õë¶Ô»º´æĞĞ¶ÔÆëºóµÄ´óĞ¡
	cache_cache.slab_size = ALIGN(cache_cache.num * sizeof(kmem_bufctl_t) +  
				      sizeof(struct slab), cache_line_size());


	/* 2+3) create the kmalloc caches */
	sizes = malloc_sizes;   //malloc_sizesÊı×é±£´æ×ÅÒª·ÖÅäµÄ´óĞ¡
	names = cache_names;  //cache_name±£´æcacheÃû

	/*
	 * Initialize the caches that provide memory for the array cache and the
	 * kmem_list3 structures first.  Without this, further allocations will
	 * bug.
	 */
	//Ê×ÏÈ´´½¨struct array_cache_init ºÍ struct kmem_list3 ËùÓÃµÄÍ¨ÓÃ»º´æÆ÷general cache£¬ËüÃÇÊÇºóĞø³õÊ¼»¯¶¯×÷µÄ»ù´¡
	//INDEX_ACÊÇ¼ÆËãlocal cacheËùÓÃµÄstruct arraycache_init¶ÔÏóÔÚkmalloc sizeÖĞµÄË÷Òı£¬¼´ÊôÓÚÄÄÒ»¼¶±ğ´óĞ¡µÄgeneral cache£¬´´½¨´Ë´óĞ¡¼¶±ğµÄcacheÎªlocal cacheËùÓÃ
	sizes[INDEX_AC].cs_cachep = kmem_cache_create(names[INDEX_AC].name,
					sizes[INDEX_AC].cs_size,
					ARCH_KMALLOC_MINALIGN,
					ARCH_KMALLOC_FLAGS|SLAB_PANIC,   //#define ARCH_KMALLOC_FLAGS SLAB_HWCACHE_ALIGN£¬ÒÑ¾­¶ÔÆë¹ıµÄ±ê¼Ç
					NULL, NULL);

	if (INDEX_AC != INDEX_L3) {
	//Èç¹ûstruct kmem_list3 ºÍ struct arraycache_init¶ÔÓ¦µÄkmalloc sizeË÷Òı²»Í¬£¬¼´´óĞ¡ÊôÓÚ²»Í¬µÄ¼¶±ğ£¬
	//Ôò´´½¨struct kmem_list3ËùÓÃµÄcache£¬·ñÔò¹²ÓÃÒ»¸öcache
		sizes[INDEX_L3].cs_cachep =
			kmem_cache_create(names[INDEX_L3].name,
				sizes[INDEX_L3].cs_size,
				ARCH_KMALLOC_MINALIGN,
				ARCH_KMALLOC_FLAGS|SLAB_PANIC,
				NULL, NULL);
	}

	slab_early_init = 0;//´´½¨ÍêÉÏÊöÁ½¸öÍ¨ÓÃ»º´æÆ÷ºó£¬slab early init½×¶Î½áÊø£¬ÔÚ´ËÖ®Ç°£¬²»ÔÊĞí´´½¨ÍâÖÃÊ½slab

	//sizes->cs_size ³õÖµÎªÊÇmalloc_sizes[0]£¬ÖµÓ¦¸ÃÊÇ´Ó32¿ªÊ¼
	while (sizes->cs_size != ULONG_MAX) {  //Ñ­»·´´½¨kmalloc¸÷¼¶±ğµÄÍ¨ÓÃ»º´æÆ÷£¬ULONG_MAX ÊÇ×î´óÖµ£¬
		/*
		 * For performance, all the general caches are L1 aligned.
		 * This should be particularly beneficial on SMP boxes, as it
		 * eliminates(Ïû³ı) "false sharing".
		 * Note for systems short on memory removing the alignment will
		 * allow tighter(½ôµÄ) packing of the smaller caches.
		 */
		if (!sizes->cs_cachep) {   
			sizes->cs_cachep = kmem_cache_create(names->name,
					sizes->cs_size,
					ARCH_KMALLOC_MINALIGN,
					ARCH_KMALLOC_FLAGS|SLAB_PANIC,
					NULL, NULL);
		}
#ifdef CONFIG_ZONE_DMA   //Èç¹ûÅäÖÃDMA£¬ÄÇÃ´ÎªÃ¿¸ökmem_cache ·ÖÅäÁ½¸ö£¬Ò»¸öDMA£¬Ò»¸ö³£¹æ
		sizes->cs_dmacachep = kmem_cache_create(
					names->name_dma,
					sizes->cs_size,
					ARCH_KMALLOC_MINALIGN,
					ARCH_KMALLOC_FLAGS|SLAB_CACHE_DMA|
						SLAB_PANIC,
					NULL, NULL);
#endif
		sizes++;   //¶¼ÊÇÊı×éÃû£¬Ö±½Ó++£¬½øĞĞÑ­»·µü´ú£¬ÓÉĞ¡µ½´ó·ÖÅä¸÷¸ö´óĞ¡µÄgeneral caches£¬×î´óÎªULONG_MAX
		names++;
	}
	/* 4) Replace the bootstrap head arrays */
	{
		struct array_cache *ptr;

		//ÏÖÔÚÒªÉêÇëarraycacheÌæ»»Ö®Ç°µÄinitarray_cache
		ptr = kmalloc(sizeof(struct arraycache_init), GFP_KERNEL);  //GFP_KERNEL ¿ÉË¯ÃßÉêÇë

		//¹ØÖĞ¶Ï
		local_irq_disable();
		BUG_ON(cpu_cache_get(&cache_cache) != &initarray_cache.cache);
		memcpy(ptr, cpu_cache_get(&cache_cache),
		       sizeof(struct arraycache_init));  //½«cache_cacheÖĞper-cpu¶ÔÓ¦µÄarray_cache¿½±´µ½ptr
		/*
		 * Do not assume that spinlocks can be initialized via memcpy:
		 */
		spin_lock_init(&ptr->lock);

		cache_cache.array[smp_processor_id()] = ptr;  //ÔÙÈÃËüÖ¸Ïòptr?
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
	 //×¢²ácpu up»Øµ÷º¯Êı£¬cpuÆô¶¯Ê±ÅäÖÃ±¾µØ»º´æ
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

	//´Óbuddy»ñÈ¡ÎïÀíÒ³£¬´óĞ¡ÓÉcachep->gfporder¾ö¶¨(2^cachep->gfporder)
	page = alloc_pages_node(nodeid, flags, cachep->gfporder);
	if (!page)
		return NULL;
	//¼ÆËã³öÒª»ñÈ¡µÄÎïÀíÒ³¸öÊı(2^cachep->gfporder)
	nr_pages = (1 << cachep->gfporder);
	
	//ÉèÖÃÒ³µÄ×´Ì¬(ÊÇ·ñ¿É»ØÊÕ)£¬ÔÚvmstatÖĞÉèÖÃ
	if (cachep->flags & SLAB_RECLAIM_ACCOUNT)
		add_zone_page_state(page_zone(page),
			NR_SLAB_RECLAIMABLE, nr_pages);
	else
		add_zone_page_state(page_zone(page),
			NR_SLAB_UNRECLAIMABLE, nr_pages);

	//°ÑÕâĞ©ÎïÀíÒ³ÉèÖÃÊôĞÔÎªslab
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
	//½«ĞéÄâµØÖ·×ª»»Îª¸ÃÒ³µÄÃèÊö½á¹¹ struct page* 
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
 //Ïú»Ùslab£¬ĞèÒªÊÍ·Å¹ÜÀí¶ÔÏóºÍslab¶ÔÏó
static void slab_destroy(struct kmem_cache *cachep, struct slab *slabp)
{
	//»ñµÃslabÒ³ÃæµÄÊ×µØÖ·£¬ÊÇÓÃµÚÒ»¸ö¶ÔÏóµÄµØÖ·colouroff(¶ÔÓÚÄÚÖÃÊ½slab,colouroffÒÑ¾­½«slab¹ÜÀíÕß°üÀ¨ÔÚÄÚÁË)
	void *addr = slabp->s_mem - slabp->colouroff;
	//debugÓÃ
	slab_destroy_objs(cachep, slabp);

	//Ê¹ÓÃSLAB_DESTROY_BY_RCUÀ´´´½¨µÄ¸ßËÙ»º´æ
	if (unlikely(cachep->flags & SLAB_DESTROY_BY_RCU)) {
		//rcu·½Ê½ÊÍ·Å,ÔİÊ±²»×ö·ÖÎö£¬Ö÷ÒªÊÇ×ö²¢ĞĞÓÅ»¯
		struct slab_rcu *slab_rcu;

		slab_rcu = (struct slab_rcu *)slabp;
		slab_rcu->cachep = cachep;
		slab_rcu->addr = addr;
		//×¢²áÒ»¸ö»Øµ÷À´ÑÓÆÚÊÍ·Åslab
		call_rcu(&slab_rcu->head, kmem_rcu_free);
	} else {
		//ÊÍ·ÅslabÕ¼ÓÃµÄÒ³Ãæµ½»ï°éÏµÍ³ÖĞ
		//Èç¹ûÊÇÄÚÖÃÊ½£¬slab¹ÜÀíÕßºÍslab¶ÔÏóÔÚÒ»Æğ£¬¿ÉÒÔÍ¬Ê±ÊÍ·Å
		kmem_freepages(cachep, addr);
		if (OFF_SLAB(cachep))
			//ÍâÖÃÊ½£¬»¹ĞèÊÍ·Åslab¹ÜÀí¶ÔÏó
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

//»º´æÆ÷µÄÏú»ÙºÜ¼òµ¥£¬ÒÀ´Î¼ì²éºÍÊÍ·Å±¾µØCPU»º´æ£¬±¾µØ¹²Ïí£¬ÈıÁ´£¬ÒÔ¼°»º´æÆ÷±¾Éí¡£
//¸Ãº¯ÊıÍ¨³£Ö»·¢ÉúÔÚĞ¶ÔØmodule(Ä£¿é)µÄÊ±ºò
static void __kmem_cache_destroy(struct kmem_cache *cachep)
{
	int i;
	struct kmem_list3 *l3;

	//ÊÍ·ÅÃ¿¸öCPU±¾µØ»º´æ£¬×¢Òâ´ËÊ±CPUÊÇ online ÔÚÏß×´Ì¬£¬Èç¹ûÊÇdown×´Ì¬£¬²¢Ã»ÓĞÊÍ·Å¡£( ¶ÔÀëÏßÎŞ·¨ÊÍ·Å¸Ğµ½ÎŞÓï:) )
	for_each_online_cpu(i)   // online
	    kfree(cachep->array[i]);

	/* NUMA: free the list3 structures */
	for_each_online_node(i) {  //¶ÔÃ¿¸öÔÚÏßµÄ½Úµã
		l3 = cachep->nodelists[i];
		if (l3) {
			//ÊÍ·Å±¾µØ¹²Ïí»º´æÊ¹ÓÃµÄarray_cache¶ÔÏó
			kfree(l3->shared);
			free_alien_cache(l3->alien);
			kfree(l3);  //ÊÍ·ÅÈıÁ´
		}
	}
	//ÊÍ·Å»º´æÆ÷£¬ÒòÎª»º´æÆ÷ÊÇÊôÓÚ cache_cache µÄ¶ÔÏó£¬ËùÒÔµ÷ÓÃ¶ÔÏóÊÍ·Åº¯Êı£¬¸Ãº¯ÊıÊÍ·ÅslabÖ®Ç°ÉêÇë¹ıµÄÄ³¸ö¶ÔÏó
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
 //¼ÆËã¸ÃslabµÄorder£¬¼´¸ÃslabÊÇÓÉ¼¸¸öÒ³Ãæ¹¹³É
static size_t calculate_slab_order(struct kmem_cache *cachep,
			size_t size, size_t align, unsigned long flags)
{
	unsigned long offslab_limit;
	size_t left_over = 0;
	int gfporder;

	//Ê×ÏÈ´ÓorderÎª0¿ªÊ¼£¬ÖªµÀ×î´óorder(KMALLOC_MAX_ORDER)ÎªÖ¹
	for (gfporder = 0; gfporder <= KMALLOC_MAX_ORDER; gfporder++) {
		unsigned int num;
		size_t remainder;

		//Í¨¹ıcache¼ÆËãº¯Êı£¬Èç¹û¸Ãorder·Å²»ÏÂÒ»¸ösize´óĞ¡µÄ¶ÔÏó£¬¼´numÎª0£¬±íÊ¾orderÌ«Ğ¡£¬ĞèÒªµ÷Õû¡£
		//²ÎÊı: gfproder: slab´óĞ¡Îª2^gfporder ¸öÒ³Ãæ
		//buffer_size: ¶ÔÏó´óĞ¡
		//align: ¶ÔÏóµÄ¶ÔÆë·½Ê½
		//flags: ÊÇÍâÖÃslab»¹ÊÇÄÚÖÃslab
		//remainder: slabÖĞÀË·ÑµÄ¿Õ¼äËéÆ¬ÊÇ¶àÉÙ
		//num: slabÖĞ¶ÔÏóµÄ¸öÊı
		cache_estimate(gfporder, size, align, flags, &remainder, &num);
		if (!num)
			continue;

		//Èç¹ûslab¹ÜÀíÕßÃ»ÓĞºÍ¶ÔÏó·ÅÔÚÒ»Æğ£¬²¢ÇÒ¸Ãorder´æ·Å¶ÔÏóÊıÁ¿´óÓÚÃ¿¸öslabÔÊĞí´æ·Å×î´óµÄ¶ÔÏóÊıÁ¿£¬Ôò·µ»Ø
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
		//ÕÒµ½ÁËºÏÊÊµÄorder£¬½«Ïà¹Ø²ÎÊı±£´æ
		cachep->num = num;   //slabÖĞµÄ¶ÔÏóÊıÁ¿
		cachep->gfporder = gfporder;   //orderÖµ
		left_over = remainder;   //slabÖĞµÄËéÆ¬´óĞ¡

		/*
		 * A VFS-reclaimable slab tends to have most allocations
		 * as GFP_NOFS and we really don't want to have to be allocating
		 * higher-order pages when we are unable to shrink dcache.
		 */
		 //SLAB_RECLAIM_ACCOUNT±íÊ¾´ËslabËùÕ¼Ò³ÃæÎª¿É»ØÊÕµÄ£¬µ±ÄÚºË¼ì²âÊÇ·ñÓĞ×ã¹»µÄÒ³ÃæÂú×ãÓÃ»§Ì¬µÄĞèÇóÊ±£¬
		 //´ËÀàÒ³Ãæ½«±»¼ÆËãÔÚÄÚ£¬Í¨¹ıµ÷ÓÃkmem_freepages()º¯Êı¿ÉÒÔ½«ÊÍ·Å·ÖÅä¸øslabµÄÒ³¿ò¡£
		 //ÓÉÓÚÊÇ¿É»ØÊÕµÄ£¬ËùÒÔ²»ÓÃ×öºóÃæµÄËéÆ¬¼ì²âÁË¡£
		if (flags & SLAB_RECLAIM_ACCOUNT)
			break;

		/*
		 * Large number of objects is good, but very large slabs are
		 * currently bad for the gfp()s.
		 */
		 //slab_break_gfp_order³õÊ¼»¯Îª1£¬¼´slab×î¶àÊÇ2^1=2¸öÒ³
		if (gfporder >= slab_break_gfp_order)
			break;

		/*
		 * Acceptable internal fragmentation?
		 */
		 //slabËùÕ¼Ò³ÃæµÄ´óĞ¡ÊÇËéÆ¬´óĞ¡µÄ8±¶ÒÔÉÏ£¬Ò³ÃæÀûÓÃÂÊ½Ï¸ß£¬¿ÉÒÔ½ÓÊÕÕâÑùµÄorder
		if (left_over * 8 <= (PAGE_SIZE << gfporder))
			break;
	}
	//·µ»ØËéÆ¬´óĞ¡
	return left_over;
}

static int __init_refok setup_cpu_cache(struct kmem_cache *cachep)
{
	//´ËÊ±³õÊ¼»¯ÒÑ¾­Íê±Ï,Ö±½ÓÊ¹ÄÜlocal cache
	if (g_cpucache_up == FULL)    
		return enable_cpucache(cachep);

	//Èç¹û³ÌĞòÖ´ĞĞµ½ÕâÀï£¬ÄÇ¾ÍËµÃ÷µ±Ç°»¹ÔÚ³õÊ¼»¯½×¶Î
	//g_cpucache_up¼ÇÂ¼³õÊ¼»¯µÄ½ø¶È£¬±ÈÈçPARTIAL_AC±íÊ¾ struct array_cache µÄ cache ÒÑ¾­´´½¨
	//PARTIAL_L3 ±íÊ¾struct kmem_list3 ËùÔÚµÄ cache ÒÑ¾­´´½¨£¬×¢Òâ´´½¨ÕâÁ½¸ö cache µÄÏÈºóË³Ğò¡£ÔÚ³õÊ¼»¯½×¶ÎÖ»ĞèÅäÖÃÖ÷cpuµÄlocal cacheºÍslabÈıÁ´
	//Èôg_cpucache_up Îª NONE£¬ËµÃ÷ sizeof(struct array)´óĞ¡µÄ cache »¹Ã»ÓĞ´´½¨£¬³õÊ¼»¯½×¶Î´´½¨ sizeof(struct array) ´óĞ¡µÄcache Ê±½øÈëÕâÁ÷³Ì
	//´ËÊ± struct arraycache_init ËùÔÚµÄ general cache »¹Î´´´½¨£¬Ö»ÄÜÊ¹ÓÃ¾²Ì¬·ÖÅäµÄÈ«¾Ö±äÁ¿ initarray_eneric ±íÊ¾µÄ local cache
	if (g_cpucache_up == NONE) {
		/*
		 * Note: the first kmem_cache_create must create the cache
		 * that's used by kmalloc(24), otherwise the creation of
		 * further caches will BUG().
		 */
		cachep->array[smp_processor_id()] = &initarray_generic.cache; //arraycache_initµÄ»º´æÆ÷»¹Ã»ÓĞ´´½¨£¬ÏÈÊ¹ÓÃ¾²Ì¬µÄ

		/*
		 * If the cache that's used by kmalloc(sizeof(kmem_list3)) is
		 * the first cache, then we need to set up all its list3s,
		 * otherwise the creation of further caches will BUG().
		 */
		 //chuangjian struct kmem_list3 ËùÔÚµÄcacheÊÇÔÚstruct array_cacheËùÔÚcacheÖ®ºó
		 //ËùÒÔ´ËÊ± struct kmem_list3 ËùÔÚµÄ cache Ò²Ò»¶¨Ã»ÓĞ´´½¨£¬Ò²ĞèÒªÊ¹ÓÃÈ«¾Ö±äÁ¿ initkmem_list3

		 //#define SIZE_AC 1£¬µÚÒ»´Î°Ñarraycache_initµÄ»º´æÆ÷ºÍinitkmem_list3[1]¹ØÁªÆğÀ´
		 //ÏÂÒ»´Î»áÌî³ä
		set_up_list3s(cachep, SIZE_AC);  

		//Ö´ĞĞµ½ÕâÀïstruct array_cacheËùÔÚµÄ cache ´´½¨Íê±Ï£¬
		//Èç¹ûstruct kmem_list3ºÍstruct array_cache µÄ´óĞ¡Ò»Ñù´ó£¬ÄÇÃ´¾Í²»ÓÃÔÙÖØ¸´´´½¨ÁË£¬g_cpucache_up±íÊ¾µÄ½ø¶È¸ü½øÒ»²½
		if (INDEX_AC == INDEX_L3) 
			g_cpucache_up = PARTIAL_L3;  //¸üĞÂcpu up ×´Ì¬
		else
			g_cpucache_up = PARTIAL_AC;
	} else {
		//g_cache_upÖÁÉÙÎªPARTIAL_ACÊ±½øÈëÕâÁ÷³Ì£¬struct arraycache_initËùÔÚµÄgeneral cacheÒÑ¾­½¨Á¢ÆğÀ´£¬¿ÉÒÔÍ¨¹mkalloc·ÖÅäÁË¡£
		cachep->array[smp_processor_id()] =
			kmalloc(sizeof(struct arraycache_init), GFP_KERNEL);

		//struct kmem_list3 ËùÔÚµÄcacheÈÔÎ´´´½¨Íê±Ï£¬»¹ĞèÊ¹ÓÃÈ«¾ÖµÄslabÈıÁ´
		if (g_cpucache_up == PARTIAL_AC) {
			set_up_list3s(cachep, SIZE_L3);
			g_cpucache_up = PARTIAL_L3;
		} else { 
		//ÄÜ½øÈëµ½ÕâÀïËµÃ÷struct kmem_list3ËùÔÚµÄcacheºÍstruct array_cacheËùÔÚµÄcache¶¼ÒÑ´´½¨Íê±Ï£¬ÎŞĞèÈ«¾Ö±äÁ¿
			int node;
			for_each_online_node(node) {
				//Í¨¹ıkmalloc·ÖÅästruct kmem_list3¶ÔÏó
				cachep->nodelists[node] =
				    kmalloc_node(sizeof(struct kmem_list3),
						GFP_KERNEL, node);
				BUG_ON(!cachep->nodelists[node]);
				//³õÊ¼»¯slabÈıÁ´
				kmem_list3_init(cachep->nodelists[node]);
			}
		}
	}
	//FIXME: ¼ÆËã»ØÊÕÊ±¼ä
	cachep->nodelists[numa_node_id()]->next_reap =
			jiffies + REAPTIMEOUT_LIST3 +
			((unsigned long)cachep) % REAPTIMEOUT_LIST3;

	//³õÊ¼»¯acµÄÒ»Ğ©±äÁ¿
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
 * @align: The required(±ØĞëµÄµÄ) alignment for the objects.  //
 * @flags: SLAB flags
 * @ctor: A constructor for the objects.
 * @dtor: A destructor for the objects (not implemented anymore).
 *
 * Returns a ptr to the cache on success, NULL on failure.  //³É¹¦·µ»ØcacheÖ¸Õë£¬Ê§°Ü·µ»Ø¿Õ
 * Cannot be called within a int, but can be interrupted.   //²»ÄÜÔÚÖĞ¶ÏÖĞµ÷ÓÃ£¬µ«ÊÇ¿ÉÒÔ±»´ò¶Ï
 * The @ctor is run when new pages are allocated by the cache
 * and the @dtor is run before the pages are handed back.
 *
 * @name must be valid until the cache is destroyed. This implies that
 * the module calling this has to destroy the cache before getting unloaded.
 *
 * The flags are  //Ìî³ä±ê¼Ç
 * 
 * %SLAB_POISON - Poison(Ê¹ÎÛÈ¾) the slab with a known test pattern (a5a5a5a5)  //Ê¹ÓÃa5a5a5a5Ìî³äÕâÆ¬Î´³õÊ¼»¯ÇøÓò 
 * to catch references to uninitialised memory.
 *
 * %SLAB_RED_ZONE - Insert `Red' zones around the allocated memory to check  //Ìí¼ÓºìÉ«¾¯½äÇø£¬¼ì²âÔ½½ç
 * for buffer overruns.
 *
 * %SLAB_HWCACHE_ALIGN - Align the objects in this cache to a hardware    //ÎïÀí»º´æĞĞ¶ÔÆë£¬
 * cacheline.  This can be beneficial if you're counting cycles as closely
 * as davem.
 */
 //´´½¨»º´æÆ÷
 /* gfporder: È¡Öµ0¡«11±éÀúÖ±µ½¼ÆËã³öcacheµÄ¶ÔÏóÊıÁ¿Ìø³öÑ­»·£¬slabÓÉ2^gfporder¸öÒ³Ãæ×é³É
   buffer_size: Îªµ±Ç°cacheÖĞ¶ÔÏó¾­¹ıcache_line_size¶ÔÆëºóµÄ´óĞ¡
   align: ÊÇcache_line_size£¬°´ÕÕ¸Ã´óĞ¡¶ÔÆë
   flags: ´Ë´¦Îª0£¬ÓÃÓÚ±êÊ¶ÄÚÖÃslab»¹ÊÇÍâÖÃslab
   left_over: Êä³öÖµ£¬¼ÇÂ¼slabÖĞÀË·Ñ¿Õ¼äµÄ´óĞ¡
   num£ºÊä³öÖµ£¬ÓÃÓÚ¼ÇÂ¼µ±Ç°cacheÖĞÔÊĞí´æÔÚµÄ¶ÔÏóÊıÄ¿
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
	 //²ÎÊı¼ì²é£¬Ãû×Ö²»ÄÜÎªNULL£¬²»ÄÜÔÚÖĞ¶ÏÖĞµ÷ÓÃ±¾º¯Êı(±¾º¯Êı¿ÉÄÜ»áË¯Ãß)
	 //»ñÈ¡³¤¶È²»µÃĞ¡ÓÚ4×Ö½Ú£¬¼´CPU×Ö³¤£¬   »ñÈ¡³¤¶È²»µÃ´óÓÚ×î´óÖµ(ÎÒÆÊÎöµÄÕâ¸ö°æ±¾ÊÇ2^25£¬ÓĞµÄ¿ÉÄÜÊÇ2^22)
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

#if 0    //DEBUG ²¿·Ö±»ÎÒ×¢ÊÍµôÁË£¬ÃâµÃµ²µã      //Ò»Ğ©¼ì²é»úÖÆ£¬ÎŞĞè¹Ø×¢
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
	 * Check that size is in terms of(ÒÀ¾İ) words.  This is needed to avoid
	 * unaligned accesses for some archs(¹°) when redzoning is used, and makes  //±ÜÃâµ±ºìÉ«¾¯½äÇø±»Ê¹ÓÃÊ±£¬±ÜÃâÎ´¶ÔÆëµÄ·ÃÎÊ½Ó´¥ºìÇø
	 * sure any on-slab bufctl's are also correctly aligned.   //Í¬Ê±È·±£ÈÎºÎon-slabµÄbfclt ÕıÈ·¶ÔÆë
	 */ 

	 ////
	//	ÎªÊ²Ã´kmem_cache_initº¯ÊıÒÑ¾­¼ÆËã¹ısize£¬alignÁË£¬ÕâÀï»¹Òª¼ÆËã?
	//     ÒòÎªÕâÀïÊÇÓÃÀ´´´½¨»º´æÆ÷µÄ£¬Ö»ÊÇ½èÓÃÁËcache_cache£¬¶ø kmem_cache_initº¯ÊıÖĞ³õÊ¼»¯µÄÊÇcache_cahceµÄ
	//     size,alignµÈ³ÉÔ±£¬ËùÒÔÎŞ¹ØÏµ¡£
	////
	
	//ÏÈ¼ì²é   ¶ÔÏó !!!!       ÊÇ²»ÊÇ32Î»¶ÔÆë£¬Èç¹û²»ÊÇÔò½øĞĞµ÷Õû
	if (size & (BYTES_PER_WORD - 1)) {    
		size += (BYTES_PER_WORD - 1);
		size &= ~(BYTES_PER_WORD - 1);                                  
	}

	/* calculate the final buffer alignment: */
	/* 1) arch recommendation: can be overridden for debug */
	//ÔÙ¼ì²é  ¶ÔÏó!!!      Òª²»ÒªÇó°´ÕÕ»º³åĞĞ¶ÔÆë 
	if (flags & SLAB_HWCACHE_ALIGN) {
		/*
		 * Default alignment: as specified by the arch code.  Except if
		 * an object is really small, then squeeze multiple objects into
		 * one cacheline.
		 */
		ralign = cache_line_size();
		while (size <= ralign / 2)    //½øĞĞ¶ÔÆë´óĞ¡µÄµ÷Õû£¬ÎÒÃÇÒª±£Ö¤¶ÔÏóµÄ´óĞ¡´óÓÚ  Õë¶ÔÓ²¼ş»º³åĞĞ¶ÔÆëËùĞèµÄ´óĞ¡
			ralign /= 2;
	} else {  //²»ĞèÒª°´Ó²¼ş»º³åĞĞ¶ÔÆë£¬ÄÇ¾ÍÄ¬ÈÏ4×Ö½Ú£¬¼´32Î»
		ralign = BYTES_PER_WORD;
	}

	/*
	 * Redzoning and user store require word alignment or possibly larger.
	 * Note this will be overridden by architecture or caller mandated
	 * alignment if either is greater than BYTES_PER_WORD.
	 */
	 //Èç¹û¿ªÆôÁËDEBUG£¬Ôò°´ĞèÒª½øĞĞÏàÓ¦µÄ¶ÔÆë
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
	align = ralign;   //Í¨¹ıÉÏÃæÒ»´ó¶Ñ¼ÆËã£¬Ëã³öÁËalignÖµ

	/* Get cache's description obj. */
	//°´ÕÕcache_cacheµÄ´óĞ¡·ÖÅäÒ»¸ökmem_cacheĞÂÊµÀı£¬Êµ¼ÊÉÏcache_cacheÔÚÄÚºË³õÊ¼»¯Íê³Éºó¾ÍÊÇkmem_cacheÁË£¬ÎªÁËÄÚºË³õÊ¼»¯Ê±¿ÉÊ¹ÓÃkmalloc£¬ËùÒÔÕâÀïÒªÓÃcache_cache
	cachep = kmem_cache_zalloc(&cache_cache, GFP_KERNEL); //¹ş¹ş£¬Õâ¾ÍÊÇÊ¹ÓÃcache_cache
	//ÕâÀï»á·ÖÅäÒ»¿é¸É¾»µÄÇåÁã¹ıµÄÄÚ´æ
	if (!cachep)
		goto oops;

#if 0   //×¢ÊÍµô DEBUG
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
	//µÚÒ»¸öÌõ¼şÍ¨¹ıPAGE_SIZEÈ·¶¨slab¹ÜÀí¶ÔÏóµÄ´æ´¢·½Ê½£¬ÄÚÖÃ»¹ÊÇÍâÖÃ¡£
	//³õÊ¼»¯½×¶Î²ÉÓÃÄÚÖÃÊ½(kmem_cache_init()ÖĞ´´½¨Á½¸öÆÕÍ¨¸ßËÙ»º´æºó¾Í°Ñslab_early_initÖÃ0ÁË
	if ((size >= (PAGE_SIZE >> 3)) && !slab_early_init)
		/*
		 * Size is large, assume best to place the slab management obj
		 * off-slab (should allow better packing of objs).
		 */
		flags |= CFLGS_OFF_SLAB;


	size = ALIGN(size, align); //´ÓÕâÒ»²½¿ÉÖª£¬slab»úÖÆÏÈ°Ñ¶ÔÏóÕë¶Ô¼°Æä×Ö³¤½øĞĞ¶ÔÆë£¬È»ºóÔÙÔÚ´Ë»ù´¡ÉÏÓÖÕë¶ÔÓ²¼ş»º³åĞĞ½øĞĞ¶ÔÆë¡£
	//ÒÔºóËùÓĞµÄ¶ÔÆë¶¼ÒªÕÕÕâ¸ö×ÜµÄ¶ÔÆëÖµ¶ÔÆë
	

	//¼ÆËãËéÆ¬´óĞ¡£¬¼ÆËãslabÓÉ¼¸¸öÒ³Ãæ(order)×é³É£¬Í¬Ê±¼ÆËãÃ¿¸öslabÖĞÓĞ¶àÉÙ¸ö¶ÔÏó
	left_over = calculate_slab_order(cachep, size, align, flags); //Õâ´Î¼ÆËãµÄ²»ÊÇcache_cacheÁË

	if (!cachep->num) {
		printk(KERN_ERR
		       "kmem_cache_create: couldn't create cache %s.\n", name);
		kmem_cache_free(&cache_cache, cachep);
		cachep = NULL;
		goto oops;
	}
	
	//¼ÆËãslab¹ÜÀí¶ÔÏóµÄ´óĞ¡£¬°üÀ¨struct slab¶ÔÏóºÍ kmem_bufctl_t Êı×é
	slab_size = ALIGN(cachep->num * sizeof(kmem_bufctl_t)
			  + sizeof(struct slab), align);

	/*
	 * If the slab has been placed off-slab, and we have enough space then
	 * move it on-slab. This is at the expense of any extra colouring.
	 */
	 //Èç¹ûÊÇÒ»¸öÍâÖÃslab£¬²¢ÇÒËéÆ¬´óĞ¡´óÓÚslab¹ÜÀí¶ÔÏóµÄ´óĞ¡£¬Ôò¿É½«slab¹ÜÀí¶ÔÏóÒÆµ½slabÖĞ£¬¸ÄÔì³ÉÒ»¸öÄÚÖÃslab!!!!!
	if (flags & CFLGS_OFF_SLAB && left_over >= slab_size) {
		flags &= ~CFLGS_OFF_SLAB;
		left_over -= slab_size;   //slab_size ¾ÍÊÇ slab ¹ÜÀí¶ÔÏó´óĞ¡
	}

	if (flags & CFLGS_OFF_SLAB) {
		//alignÊÇÕë¶Ôslab¶ÔÏóµÄ£¬Èç¹û slab¹ÜÀíÕß ÊÇÍâÖÃ´æ´¢£¬×ÔÈ»Ò²²»»áÏñÄÚÖÃÄÇÑùÓ°Ïìµ½ºóÃæslab¶ÔÏóµÄ´æ´¢Î»ÖÃ
		//slab¹ÜÀíÕßÒ²¾Í²»ĞèÒª¶ÔÆëÁË
		/* really off slab. No need for manual alignment */
		slab_size =
		    cachep->num * sizeof(kmem_bufctl_t) + sizeof(struct slab);
	}

	//×ÅÉ«¿éµ¥Î»£¬ÎªL1_CACHE_BYTES£¬¼´32×Ö½Ú
	cachep->colour_off = cache_line_size();
	/* Offset must be a multiple of the alignment. */
	//×ÅÉ«µ¥Î»±ØĞëÊÇ¶ÔÆëµ¥Î»µÄÕûÊı±¶
	if (cachep->colour_off < align)
		cachep->colour_off = align;
	//¼ÆËãËéÆ¬ÇøÓòĞèÒª¶àÉÙ¸ö×ÅÉ«¿é
	cachep->colour = left_over / cachep->colour_off;
	//¹ÜÀí¶ÔÏóµÄ´óĞ¡
	cachep->slab_size = slab_size;
	cachep->flags = flags;
	cachep->gfpflags = 0;
	if (CONFIG_ZONE_DMA_FLAG && (flags & SLAB_CACHE_DMA))   //Óë»ï°éÏµÍ³½»»¥µÄDMA±êÖ¾
		cachep->gfpflags |= GFP_DMA;
	//slab¶ÔÏóµÄ´óĞ¡
	cachep->buffer_size = size;
	//µ¹Êı
	cachep->reciprocal_buffer_size = reciprocal_value(size);


	//Èç¹ûÊÇÍâÖÃslab£¬ÕâÀïÒª·ÖÅäÒ»¸ö¹ÜÀí¶ÔÏó£¬±£´æÔÚslabp_cacheÖĞ£¬Èç¹ûÊÇÄÚÖÃÊ½µÄslab£¬´ËÖ¸ÕëÎª¿Õ
	//array_cachine, cache_cache, 3list Õâ¼¸¸ö¿Ï¶¨ÊÇÄÚÖÃÊ½£¬²»»á½øÈëÕâ¸ö
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
	
	//kmem_cachµÄÃû×ÖºÍËü¹ÜÀíµÄ¶ÔÏóµÄ¹¹Ôìº¯Êı
	cachep->ctor = ctor;
	cachep->name = name;

	//ÉèÖÃÃ¿¸öCPUÉÏµÄlocal cache£¬ÅäÖÃlocal cacheºÍslab ÈıÁ´
	if (setup_cpu_cache(cachep)) {
		__kmem_cache_destroy(cachep);
		cachep = NULL;
		goto oops;
	}

	//½«kmem_cache¼ÓÈëµ½cache_chainÎªÍ·µÄkmem_cacheÁ´±íÖĞ
	/* cache setup completed, link it into the list */
	list_add(&cachep->next, &cache_chain);   //»¹ÊÇÓÃÁËcache_chain
oops:
	if (!cachep && (flags & SLAB_PANIC))
		panic("kmem_cache_create(): failed to create slab `%s'\n",
		      name);
	mutex_unlock(&cache_chain_mutex);  //mutex
	return cachep;   //·µ»Ø¸Ã kmem_cache
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

//ÊÍ·Å±¾µØ»º´æÖĞµÄ¶ÔÏó
static void do_drain(void *arg)
{
	struct kmem_cache *cachep = arg;
	struct array_cache *ac;
	int node = numa_node_id();
	check_irq_off();

	//»ñµÃ±¾µØ»º´æ
	ac = cpu_cache_get(cachep);
	spin_lock(&cachep->nodelists[node]->list_lock);

	//ÊÍ·Å±¾µØ»º´æÖĞµÄ¶ÔÏó
	free_block(cachep, ac->entry, ac->avail, node);

	spin_unlock(&cachep->nodelists[node]->list_lock);
	ac->avail = 0;
}

//ÊÍ·Å±¾µØ»º´æºÍ±¾µØ¹²Ïí»º´æÖĞµÄ¶ÔÏó
static void drain_cpu_caches(struct kmem_cache *cachep)
{
	struct kmem_list3 *l3;
	int node;

	//ÊÍ·ÅÃ¿¸ö±¾µØ»º´æÖĞµÄ¶ÔÏó£¬×¢ÒâÃ»ÓĞ "online"
	on_each_cpu(do_drain, cachep, 1, 1);  //µ÷ÓÃÁËdo_drain()º¯Êı
	check_irq_on();

	//NUMAÏà¹Ø£¬ÊÍ·ÅÃ¿¸öNUMA½ÚµãµÄalien
	for_each_online_node(node) {
		l3 = cachep->nodelists[node];
		if (l3 && l3->alien)
			//±¾°æ±¾Ä¿Ç°ÊÇ¿Õº¯Êı£¬Ôİ²»Ö§³Ö
			drain_alien_cache(cachep, l3->alien);
	}

	//ÊÍ·Å±¾µØ¹²Ïí»º´æÖĞµÄ¶ÔÏó
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
 //Ïú»Ù¿ÕslabÁ´±íÖĞµÄslab
static int drain_freelist(struct kmem_cache *cache,
			struct kmem_list3 *l3, int tofree)
{
	struct list_head *p;
	int nr_freed;
	struct slab *slabp;

	nr_freed = 0;
	//ÊÍ·Å¿ÕÁ´ÖĞµÄslab
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
		//½«slab´Ó¿ÕÁ´ÖĞÕª³ı
		list_del(&slabp->list);
		/*
		 * Safe to drop the lock. The slab is no longer linked
		 * to the cache.
		 */
		//×Ü¿ÕÏĞ¶ÔÊı¼õÈ¥Ã¿¸öslabÖĞµÄ¶ÔÏóÊı
		l3->free_objects -= cache->num;
		spin_unlock_irq(&l3->list_lock);
		//Ïú»Ùslab£¬°üÀ¨slab¹ÜÀí¶ÔÏóºÍslab¶ÔÏó
		slab_destroy(cache, slabp);
		nr_freed++;
	}
out:
	return nr_freed;
}

/* Called with cache_chain_mutex held to protect against cpu hotplug */
//ÊÍ·Å¿ÕÁ´±íÖĞµÄslab
static int __cache_shrink(struct kmem_cache *cachep)
{
	int ret = 0, i = 0;
	struct kmem_list3 *l3;

	//ÊÍ·Å±¾µØ»º´æÖĞ¶ÔÏó
	drain_cpu_caches(cachep);

	check_irq_on();
	for_each_online_node(i) {
		l3 = cachep->nodelists[i];
		if (!l3)
			continue;

		//ÊÍ·Å¿ÕÁ´±íÖĞµÄslab
		drain_freelist(cachep, l3, l3->free_objects);
		//¼ì²éÂúslabÁ´±íºÍ²¿·ÖÂúslabÁ´±íÊÇ·ñ»¹ÓĞslab
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
 //Ïú»ÙÒ»¸ö»º´æÆ÷£¬Í¨³£ÕâÖ»·¢ÉúÔÚĞ¶ÔØmoduleÊ±
void kmem_cache_destroy(struct kmem_cache *cachep)
{
	BUG_ON(!cachep || in_interrupt());

	/* Find the cache in the chain of caches. */
	mutex_lock(&cache_chain_mutex);
	/*
	 * the chain is never empty, cache_cache is never destroyed
	 */
	 //½«»º´æÆ÷´Ócache_chainµÄÁ´±íÖĞÕª³ı
	list_del(&cachep->next);
	if (__cache_shrink(cachep)) {  //ÊÍ·Å¿ÕÁ´±íÖĞµÄslab£¬²¢¼ì²éÆäËûÁ½¸öÁ´±í¡£ÔÚÏú»Ù»º´æÆ÷Ç°£¬±ØĞëÏÈÏú»ÙÆäÖĞµÄslab
		//ÂúslabÁ´»ò²¿·ÖÂúslabÁ´²»Îª¿Õ
		slab_error(cachep, "Can't free all objects");
		//»º´æÆ÷·Ç¿Õ£¬²»ÄÜÏú»Ù£¬ÖØĞÂ¼ÓÈëµ½cache_chainÁ´±íÖĞ
		list_add(&cachep->next, &cache_chain);

		mutex_unlock(&cache_chain_mutex);
		return;
	}

	//ÓĞ¹Ørcu
	if (unlikely(cachep->flags & SLAB_DESTROY_BY_RCU))
		synchronize_rcu();

	//µ×²ãµ÷ÓÃ__kmem_cache_destroy()º¯ÊıÀ´ÊµÏÖ
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
 * descriptors in kmem_cache_create, we search through the malloc_sizes array.  //Í¨¹ımalloc_sizes±í
 * If we are creating a malloc_sizes cache here it would not be visible(Ã÷ÏÔµÄ) to
 * kmem_find_general_cachep till the initialization is complete.
 * Hence(Òò´Ë£¬½ñºó) we cannot have slabp_cache same as the original cache.
 */
static struct slab *alloc_slabmgmt(struct kmem_cache *cachep, void *objp,
				   int colour_off, gfp_t local_flags,
				   int nodeid)
{
	struct slab *slabp;

	//Èç¹ûÊÇÍâÖÃslab
	if (OFF_SLAB(cachep)) {
		/* Slab management obj is off-slab. */
		//ÍâÖÃslabÎÒÃÇ·ÖÅäµ½slabpÖ¸ÕëÉÏ£¬ÔÚÍâÃæ´´½¨Ò»¸öslab:)
		slabp = kmem_cache_alloc_node(cachep->slabp_cache,
					      local_flags & ~GFP_THISNODE, nodeid);
		if (!slabp)
			return NULL;
	} else {
		//¶ÔÓÚÄÚÖÃslab£¬slabÃèÊö·û¾ÍÔÚ¸ÃslabËùÕ¼¿Õ¼äµÄÆğÊ¼£¬¼´ËùÕ¼ĞéÄâÆğÊ¼µØÖ·¼ÓÉÏËüµÄ×ÅÉ«Æ«ÒÆ
		//È»ºóÓ¦¸Ã¸üĞÂ¸Ã×ÅÉ«Æ«ÒÆ¼´¼ÓÉÏ¹ÜÀí¶ÔÏóµÄ¿Õ¼ä
		slabp = objp + colour_off;
		colour_off += cachep->slab_size;
	}
	
	//×¢ÒâÉÏÃæÓĞÁ½ÖÖÇé¿ö£¬Èç¹ûslab¹ÜÀíÕßÊÇÍâÖÃµÄ£¬ÔÚÉÏÃæslabpµÄµØÖ·¿ÉÄÜ¸Ä±ä¡£²»¹ıÏÂÃæ²»¿¼ÂÇÕâĞ©£¬Ö±½ÓÔÚslabpµØÖ·ÉÏÉèÖÃslab¹ÜÀíÕß¾ÍĞĞÁË
	
	slabp->inuse = 0;
	//¶ÔÓÚµÚÒ»¸ö¶ÔÏóÒ³ÄÚÆ«ÒÆ£¬ÓÉÉÏÃæ¿ÉÖªÈç¹ûÊÇÄÚÖÃÊ½slab£¬colouroff³ÉÔ±²»½ö°üÀ¨×ÅÉ«Çø£¬»¹°üÀ¨¹ÜÀí¶ÔÏóÕ¼ÓÃµÄ¿Õ¼ä
	//¶ÔÓÚÍâÖÃÊ½slab£¬colouroff³ÉÔ±Ö»°üÀ¨×ÅÉ«Çø
	slabp->colouroff = colour_off;
	//µÚÒ»¸ö¶ÔÏóµÄĞéÄâµØÖ·£¬ÕâÊ±×ÅÉ«Æ«ÒÆ¶ÔÓÚÄÚÖÃslabÒÑ¼ÓÉÏÁË¹ÜÀí¶ÔÏóµÄ¿Õ¼ä
	slabp->s_mem = objp + colour_off;
	//¸üĞÂ½Úµãid
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

	//Ñ­»·´´½¨¸÷¸ö¶ÔÏó
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
		slab_bufctl(slabp)[i] = i + 1;   //Ê¹ÓÃbufctlÊı×é´æ´¢Ë÷Òı
	}
	slab_bufctl(slabp)[i - 1] = BUFCTL_END;  //×îºóÒ»¸öÖ¸ÏòBUFCTL_END
	slabp->free = 0;  //ÉèÖÃ¿ÕÏĞ¸öÊı
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
	//»ñµÃÒ»¸ö¿ÕÏĞµÄ¶ÔÏó£¬free³ÉÔ±ÊÇ±¾slabÖĞµÚÒ»¸ö¿ÕÏĞ¶ÔÏóµÄË÷Òı
	void *objp = index_to_obj(cachep, slabp, slabp->free);
	kmem_bufctl_t next;

	//¸üĞÂÔÚÓÃ¶ÔÏó¼ÆÊıÖµ
	slabp->inuse++;
	
	//»ñµÃÏÂÒ»´ÎµÄµÚÒ»¸ö¿ÕÏĞ¶ÔÏóË÷Òı
	next = slab_bufctl(slabp)[slabp->free];
#if DEBUG
	slab_bufctl(slabp)[slabp->free] = BUFCTL_FREE;
	WARN_ON(slabp->nodeid != nodeid);
#endif
	slabp->free = next;  //¸üĞÂ¿ÕÏĞ¶ÔÏóË÷Òı£¬ÓëÉÏÃæ¶ÔÓ¦

	return objp;
}

static void slab_put_obj(struct kmem_cache *cachep, struct slab *slabp,
				void *objp, int nodeid)
{
	//»ñµÃ¶ÔÏóÔÚ kmem_bufctl_t Êı×éÖĞµÄË÷Òı
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
	//Ö¸ÏòslabÖĞÔ­À´µÄµÚÒ»¸ö¿ÕÏĞ¶ÔÏó
	slab_bufctl(slabp)[objnr] = slabp->free;
	//½«ÒªÊÍ·ÅµÄ¶ÔÏó×÷ÎªµÚÒ»¸ö¿ÕÏĞ¶ÔÏó£¬Õâ¸ö±»ÊÍ·ÅÁËµÄ¶ÔÏó°²ÖÃÔÚslabÖĞÁË
	slabp->free = objnr;
	//ÒÑ·ÖÅä¶ÔÏóÊı¼õÒ»
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
		nr_pages <<= cache->gfporder;   //·ÖÅä¸øslabµÄÒ³¿òÊı

	do {
		page_set_cache(page, cache);    //½¨Á¢µ½cacheµÄÓ³Éä
		page_set_slab(page, slab);         //½¨Á¢µ½slabµÄÓ³Éä
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
	//È·¶¨¹ØÁËÖĞ¶Ï
	check_irq_off();
	//»ñÈ¡±¾½Úµãcache·ÖÅäÆ÷µÄslabÈıÁ´
	l3 = cachep->nodelists[nodeid];
	spin_lock(&l3->list_lock);

	/* Get colour for the slab, and cal the next value. */
	//»ñÈ¡´ı´´½¨slabµÄ×ÅÉ«ÊıÄ¿
	offset = l3->colour_next;
	//»ñÈ¡ÍêÁËÒª++£¬¸üĞÂÏÂÒ»´ÎÒª´´½¨µÄslabµÄ×ÅÉ«ÊıÄ¿
	l3->colour_next++;
	if (l3->colour_next >= cachep->colour)
	//ÑÕÉ«±àºÅ±ØĞëĞ¡ÓÚÑÕÉ«¸öÊı£¬Èç¹û³¬¹ıÁË£¬ÖØÖÃÎª0£¬Õâ¾ÍÊÇ×ÅÉ«Ñ­»·ÎÊÌâ¡£ÊÂÊµÉÏ£¬Èç¹ûslabÖĞÀË·ÑµÄ¿Õ¼äºÜÉÙ£¬ÄÇÃ´ºÜ¿ì¾Í»áÑ­»·Ò»´Î
		l3->colour_next = 0;
	spin_unlock(&l3->list_lock);

	//¸Ãcache¿éµÄ×ÅÉ«Æ«ÒÆ£¬ ×¢Òâ *=
	offset *= cachep->colour_off;  //colour_offÊÇµ¥Î»

	//Ê¹ÓÃÁË_GFP_WAIT¾Í»á¿ªÆôÖĞ¶Ï
	if (local_flags & __GFP_WAIT)
		local_irq_enable();

	/*
	 * The test for missing atomic flag is performed here, rather than
	 * the more obvious place, simply to reduce the critical path length
	 * in kmem_cache_alloc(). If a caller is seriously mis-behaving they
	 * will eventually be caught here (where it matters).
	 */
	 //¼ì²éÓë»ï°éÏµÍ³½»»¥µÄ±ê¼Ç
	kmem_flagcheck(cachep, flags);

	/*
	 * Get mem for the objs.  Attempt to allocate a physical page from
	 * 'nodeid'.
	 */
	 //´Óbuddy»ñÈ¡ÎïÀíÒ³£¬·µ»ØµÄÊÇĞéÄâµØÖ·objp
	if (!objp)
		objp = kmem_getpages(cachep, flags, nodeid);
	if (!objp)  //Ê§°Ü¾Ífailed
		goto failed;

	/* Get slab management. */
	//»ñµÃÒ»¸öĞÂµÄslabÃèÊö·û
	slabp = alloc_slabmgmt(cachep, objp, offset,
			local_flags & ~GFP_THISNODE, nodeid);
	if (!slabp)
		goto opps1;

	//ÉèÖÃ½Úµãid
	slabp->nodeid = nodeid;   
	//°ÑslabÃèÊö·ûslabp¸³¸øÎïÀíµÄprev×Ö¶Î£¬°Ñ¸ßËÙ»º´æÃèÊö·û cachep ¸³¸øÎïÀíÒ³µÄ LRU ×Ö¶Î
	//±¾ÖÊÊÇ½¨Á¢slabºÍcacheµ½ÎïÀíÒ³µÄÓ³Éä£¬ÓÃÓÚ¿ìËÙ¸ù¾İÎïÀíÒ³¶¨Î»slabÃèÊö·ûºÍcacheÃèÊö·û
	slab_map_pages(cachep, slabp, objp);

	//³õÊ¼»¯cacheÃèÊö·ûºÏslab¶ÔÏóÃèÊö·û
	cache_init_objs(cachep, slabp);

	if (local_flags & __GFP_WAIT)
		local_irq_disable();
	check_irq_off();
	spin_lock(&l3->list_lock);

	/* Make slab active. */
	//°ÑÉÏÃæ³õÊ¼»¯ºÃµÄslabÎ²²å·¨¼ÓÈëµ½ÈıÁ´µÄÈ«¿ÕÁ´±í
	list_add_tail(&slabp->list, &(l3->slabs_free));
	STATS_INC_GROWN(cachep);
	//¸üĞÂ¸ßËÙ»º´æÖĞ¿ÕÏĞ¶ÔÏó¼ÆÊıÆ÷
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
	//±¾µÍ¸ßËÙ»º´æ±äÁ¿
	ac = cpu_cache_get(cachep);
retry:
	//×¼±¸Ìî³ä±¾µØ¸ßËÙ»º´æ£¬ÕâÀïÏÈ¼ÇÂ¼Ìî³ä¶ÔÏó¸öÊı£¬¼´batchcount³ÉÔ±(ÅúÁ¿×ªÈë×ª³öµÄ¸öÊı)
	batchcount = ac->batchcount;
	if (!ac->touched && batchcount > BATCHREFILL_LIMIT) {
		/*
		 * If there was little recent activity on this cache, then
		 * perform only a partial refill.  Otherwise we could generate
		 * refill bouncing.
		 */
		batchcount = BATCHREFILL_LIMIT;
	}

	//»ñµÃ±¾ÄÚ´æ½ÚµãµÄkmem_cacheµÄÈıÁ´
	l3 = cachep->nodelists[node];

	BUG_ON(ac->avail > 0 || !l3);
	spin_lock(&l3->list_lock);

	/* See if we can refill from the shared array */
	//Èı²½ÇúÖ®µÚ¶ş²½:
	//Èç¹ûÓĞ±¾µØ¹²Ïí¸ßËÙ»º´æ£¬Ôò´Ó¹²Ïí±¾µØ¸ßËÙ»º´æÌî³ä£¬½öÓÃÓÚ¶àºË£¬¶à¸öCPU¹²ÏíµÄ¸ßËÙ»º´æ
	if (l3->shared && transfer_objects(ac, l3->shared, batchcount))
		goto alloc_done;  //Ò»´ÎĞÔÖ»»áÉêÇëÒ»¸ö¿é£¬·ÖÅä³É¹¦¾ÍÌøÈ¥alloc_down£¬ac->availÒÑ¾­±»ĞŞ¸Ä

	//Èı²½ÇúµÚÈı²½:´Ó±¾µØµÄ¸ßËÙ»º´æµÄÈıÁ´ÖĞ·ÖÅä
	while (batchcount > 0) {
		struct list_head *entry;
		struct slab *slabp;
		/* Get slab alloc is to come from. */
		//Ê×ÏÈ·ÃÎÊ²¿·ÖÂúµÄslabÁ´±í£¬ÊÇentryÖ¸ÏòµÚÒ»¸ö½Úµã
		entry = l3->slabs_partial.next;
		//Èç¹û²¿·ÖÂúÁ´±íµÄ¶¼Ã»ÁË£¬¾ÍÕÒÈ«¿ÕÏĞµÄ
		if (entry == &l3->slabs_partial) {
			//ÔÚ·ÃÎÊÈ«¿ÕÏĞslabÁ´±íÇ°ÏÈ×öÒ»¸ö±ê¼Ç£¬±íÃ÷È«¿ÕÏĞslabÁ´±í±»Ê¹ÓÃ¹ıÁË
			l3->free_touched = 1;
			//Ê¹entryÖ¸ÏòÈ«¿ÕÏĞÁ´±íµÚÒ»¸ö½Úµã
			entry = l3->slabs_free.next;
			//È«¿ÕÏĞµÄÒ²Ã»ÁË£¬Ò²¾ÍÊÇÎÒÃÇµÄÈı²½Çú¶¼Ê§°ÜÁË£¬Ôò±ØĞëÀ©³äÁË:)
			if (entry == &l3->slabs_free)
				goto must_grow;  
		}

		//ÉÏÃæ³öÀ´µÄÖ»ÓĞÁ½ÖÖÇé¿ö¾ÍÊÇ²¿·ÖÂúµÄÁ´±í»òÕßÈ«¿ÕÏĞµÄÁ´±íÓĞ¿É·ÖÅä¶ÔÏó£¬ÎÒÃÇ¾ÍÈ¥¸ÃÁ´£¬×¼±¸µ÷Õû
		slabp = list_entry(entry, struct slab, list);
		//µ×ÏÂÊÇÁ½¸ö¿Õº¯Êı
		check_slabp(cachep, slabp);
		check_spinlock_acquired(cachep);

		/*
		 * The slab was either on partial or free list so
		 * there must be at least one object available for
		 * allocation.
		 */
		BUG_ON(slabp->inuse < 0 || slabp->inuse >= cachep->num);

		//Èç¹ûÈıÁ´ÖĞ»¹´æÔÚ¶ÔÏóÎ´·ÖÅä£¬¾Í°ÑËüÌî³äµ½acÖĞ£¬×î¶àbatchcount¸ö
		while (slabp->inuse < cachep->num && batchcount--) {
			//Ò»°ãÇé¿öÏÂÏÂÃæÊÇÈı¸ö¿Õº¯Êı
			STATS_INC_ALLOCED(cachep);
			STATS_INC_ACTIVE(cachep);
			STATS_SET_HIGH(cachep);

			//´ÓslabÖĞÌáÈ¡¿ÕÏĞ¶ÔÏó£¬²åÈëµ½acÖĞ
			ac->entry[ac->avail++] = slab_get_obj(cachep, slabp,
							    node);
		}
		check_slabp(cachep, slabp);  //¿Õº¯Êı

		/* move slabp to correct slabp list: */
		//ÓÉÓÚÈıÁ´ÔÚÉÏÃæ»á·ÖÅä³ö²¿·Ö¶ÔÏó£¬ËùÒÔÔÚ´Ë´¦ĞèÒªµ÷Õû¡£Èç¹ûslabÖĞÃ»ÓĞ¿ÕÏĞ¶ÔÏó£¬Ìí¼Óµ½ÈıÁ´µÄfullÁ´±í£»
		//»¹ÓĞ¿ÕÏĞ¶ÔÏó£¬Ìí¼Óµ½ÈıÁ´µÄpartial slabÁ´±íÖĞ
		list_del(&slabp->list);
		if (slabp->free == BUFCTL_END)
			list_add(&slabp->list, &l3->slabs_full);
		else
			list_add(&slabp->list, &l3->slabs_partial);
	}

must_grow:
	//Ç°Ãæ´ÓÈıÁ´ÖĞÌí¼ÓÁËavail¸ö¿ÕÏĞ¶ÔÏóµ½acÖĞ£¬´ËÊ±ĞèÒª¸üĞÂÈıÁ´µÄ¿ÕÏĞ¶ÔÏóÊı
	l3->free_objects -= ac->avail;
alloc_done:
	spin_unlock(&l3->list_lock);

	//´Ë´¦ÓĞ¿ÉÄÜÖ®Ç°shared array Í¨¹ı transfer ¸øÁË ac Ò»²¿·Ö£¬Ò²²»»á½øÈëÕâÀïÁË
	//ÈıÁ´×ªÒÆ³É¹¦Ò²²»»á½øÈëÁË
	if (unlikely(!ac->avail)) {
		int x;
		//Ê¹ÓÃcache_growÎª¸ßËÙ»º´æ·ÖÅäÒ»¸öĞÂµÄslab
		//²ÎÊı·Ö±ğÊÇ: cacheÖ¸Õë¡¢±êÖ¾¡¢ÄÚ´æ½Úµã¡¢Ò³ĞéÄâµØÖ·(Îª¿Õ±íÊ¾»¹Î´ÉêÇëÄÚ´æÒ³£¬²»Îª¿Õ£¬ËµÃ÷ÒÑÉêÇëÄÚ´æÒ³£¬¿ÉÖ±½ÓÓÃÀ´´´½¨slab)
		//·µ»ØÖµ: 1Îª³É¹¦£¬0ÎªÊ§°Ü
		x = cache_grow(cachep, flags | GFP_THISNODE, node, NULL);

		/* cache_grow can reenable interrupts, then ac could change. */
		//ÉÏÃæµÄ²Ù×÷Ê¹ÄÜÁËÖĞ¶Ï£¬´ËÆÚ¼älocal cacheÖ¸Õë¿ÉÄÜ·¢ÉúÁË±ä»¯£¬ĞèÒªÖØĞÂ»ñµÃ
		ac = cpu_cache_get(cachep);

		//Èç¹ûcache_growÊ§°Ü£¬local cacheÖĞÒ²Ã»ÓĞ¿ÕÏĞ¶ÔÏó£¬ËµÃ÷ÎÒÃÇÎŞ·¨·ÖÅäÄÚ´æÁË£¬ÄÇ¾Í¹ÒÁË:)
		if (!x && ac->avail == 0)	/* no objects in sight? abort */
			return NULL;

		//Èç¹ûacÖĞµÄ¿ÉÓÃ¶ÔÏóÎª0£¬ÎÒÃÇ¾Í°ÑÉÏÃæÍ¨¹ı»ï°éÏµÍ³¸øÈıÁ´µÄslab·Ö¸øacÒ»²¿·Ö£¬±Ï¾¹per-cpuÓÅÏÈÂï
		//²»¹ı£¬Ö»Òªac_>avail²»Îª0£¬ËµÃ÷ÆäËû½ø³ÌÌî³äÁËac£¬ÒòÎª±¾½ø³ÌÖ»Ìî³ä¸øÁ´±í£¬ËùÒÔ²»ĞíÒªÔÙ·ÖÅä¶ÔÏó¸øacÁË£¬²»ÓÃÖ´ĞĞretry
		if (!ac->avail)		/* objects refilled by interrupt? */
			goto retry;
	}
	//ÖØÌîÁËlocal cache£¬¼´ac£¬ÉèÖÃ½üÆÚ·ÃÎÊ±êÖ¾ touch
	ac->touched = 1; 
	//ÎÒÃÇĞèÒª·µ»ØacÖĞ×îºóÒ»¸ö¶ÔÏóµÄµØÖ·£¬ÒòÎª´Ólocal cacheÖĞ·ÖÅä¶ÔÏó×ÜÊÇ×îÓÅµÄ
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

//·ÖÅäµÄÖØµã
static inline void *____cache_alloc(struct kmem_cache *cachep, gfp_t flags)
{
	void *objp;
	struct array_cache *ac;

	check_irq_off();

	//»ñµÃ»º´æµÄ±¾µØ¸ßËÙ»º´æµÄÃèÊö·û array_cache
	ac = cpu_cache_get(cachep);   //Èç¹ûÊÇµÚÒ»¸öcache_cache£¬ÔÚÕâÀï¾Í·µ»Ø0
	//Èı²¿Çú£¬µÚÒ»²½: ÏÈ¿´local cacheÖĞÓĞÃ»ÓĞ·ÖÅäµÄ¶ÔÏó£¬Èç¹ûÓĞ¾ÍÖ±½ÓÓÃ£¬Ã»ÓĞ¾ÍÖ»ÄÜÖ´ĞĞcache_alloc_refill£¬ÀïÃæÓĞÈı²¿ÇúµÄÁíÍâÁ½²½
	if (likely(ac->avail)) {
		STATS_INC_ALLOCHIT(cachep); //FIXME: ¿Õº¯Êı, ¶¨ÒåÊÇ#define STATS_INC_ALLOCHIT(x)	do { } while (0), ÕâÓĞÊ²Ã´ÓÃ?
		//½«availµÄÖµ¼õ1£¬ÕâÑùavail¶ÔÓ¦µÄ¿ÕÏĞ¶ÔÏóÊÇ×îÈÈµÄ£¬¼´×î½üÊÍ·Å³öÀ´µÄ£¬¸üÓĞ¿ÉÄÜ×¤ÁôÔÚCPU¸ßËÙ»º´æÖĞ
		ac->touched = 1;   //slab·ÖÅä¶ÔÏóÊÇ°´¿é·ÖÅä£¬ÕâÀï¿Ï¶¨Ö»ÒªÒ»¿é£¬ÄÃ×ß¾ÍĞĞÁË¡£
		//ÓÉÓÚacÊÇ¼ÇÂ¼ÕâÖÁ´Ëstruct array_cache½á¹¹Ìå´æ·ÅµØÖ·£¬Í¨¹ıac->entry[]ºó£¬ÎÒÃÇ¾ÍµÃµ½Ò»¸öµØÖ·£¬
		//Õâ¸öµØÖ·¿ÉÒÔ¿´×öÊÇÎª local cache ¿ÉÓÃ¶ÔÏóµÄÊ×µØÖ·£¬´ÓÕâÀï¿ÉÒÔ¿´³ö£¬ÊÇ´Ó×îºóÒ»¸ö¶ÔÏó¿ªÊ¼·ÖÅäµÄ,¼´LIFO½á¹¹¡£
		objp = ac->entry[--ac->avail];
	} else {
		STATS_INC_ALLOCMISS(cachep);  //¿Õº¯Êı
		objp = cache_alloc_refill(cachep, flags);  //Îª¸ßËÙ»º´æÄÚ´æ¿Õ¼äÔö¼ÓĞÂµÄÄÚ´æ¶ÔÏó£¬½«»áÖ´ĞĞÈı²¿ÇúµÄ¶şÈı²½
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
	
	//²ÎÊı¼ì²â
	if (should_failslab(cachep, flags))
		return NULL;
	//Í¬ÑùÊÇ²ÎÊı¼ì²â:)
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
	//×ªµ÷ÓÃ___cache_alloc
	return ____cache_alloc(cachep, flags);
}

#endif /* CONFIG_NUMA */

////²»¹ÜÊÇ kmalloc »¹ÊÇ kmem_cache_alloc£¬kmem_cache_zalloc£¬×îÖÕ¶¼ÊÇµ÷ÓÃ__cache_allocº¯Êı£¬ÕâÊÇ¸øµ÷ÓÃÕß·ÖÅäslabµÄ×Ü½Ó¿Ú
static __always_inline void *
__cache_alloc(struct kmem_cache *cachep, gfp_t flags, void *caller)
{
	unsigned long save_flags;
	void *objp;

	if (should_failslab(cachep, flags))
		return NULL;
	//ºÍÉÏÃæÒ»Ñù¶¼ÊÇ²ÎÊı¼ì²â
	cache_alloc_debugcheck_before(cachep, flags);
	local_irq_save(save_flags);
	//µ×²ãµ÷ÓÃ__do_cache_allocº¯Êı
	objp = __do_cache_alloc(cachep, flags);
	local_irq_restore(save_flags);
	objp = cache_alloc_debugcheck_after(cachep, flags, objp, caller);
	prefetchw(objp);

	return objp;
}

/*
 * Caller needs to acquire correct kmem_list's list_lock
 */   //ÊÍ·ÅÒ»¶¨ÊıÄ¿µÄ¶ÔÏó
static void free_block(struct kmem_cache *cachep, void **objpp, int nr_objects,
		       int node)
{
	int i;
	struct kmem_list3 *l3;

	//ÖğÒ»ÊÍ·Å¶ÔÏóµ½ÈıÁ´ÖĞ
	for (i = 0; i < nr_objects; i++) {
		void *objp = objpp[i];
		struct slab *slabp;

		//Í¨¹ı¶ÔÏóµÄĞéÄâµØÖ·µÃµ½page£¬ÔÙÍ¨¹ıpageµÃµ½slab
		slabp = virt_to_slab(objp);
		//»ñµÃslabÈıÁ´
		l3 = cachep->nodelists[node];
		//ÏÈ½«¶ÔÏóËùÔÚµÄslab´ÓÁ´±íÖĞÕª³ı
		list_del(&slabp->list);
		check_spinlock_acquired_node(cachep, node);
		check_slabp(cachep, slabp);
		//½«¶ÔÏó·Åµ½Æä slab ÖĞ
		slab_put_obj(cachep, slabp, objp, node);
		STATS_DEC_ACTIVE(cachep);
		//Ôö¼Ó¿ÕÏĞ¶ÔÏó¼ÆÊı
		l3->free_objects++;
		check_slabp(cachep, slabp);

		/* fixup slab chains */
		//Èç¹ûslabÖĞÈ«¶¼ÊÇ¿ÕÏĞ¶ÔÏó
		if (slabp->inuse == 0) {
			//Èç¹ûÈıÁ´ÖĞ¿ÕÏĞ¶ÔÏóÊıÄ¿³¬¹ıÉÏÏŞ£¬Ö±½Ó»ØÊÕÕû¸ö slab µ½ÄÚ´æ£¬¿ÕÏĞ¶ÔÏóÊı¼õÈ¥Ã¿¸öslabÖĞ¶ÔÏóÊı
			if (l3->free_objects > l3->free_limit) {
				l3->free_objects -= cachep->num;
				/* No need to drop any previously held
				 * lock here, even if we have a off-slab slab
				 * descriptor it is guaranteed to come from
				 * a different cache, refer to comments before
				 * alloc_slabmgmt.
				 */
				 //Ïú»Ùslab¶ÔÏó
				slab_destroy(cachep, slabp);
			} else {  //µ½ÕâÀïËµÃ÷¿ÕÏĞ¶ÔÏóÊıÄ¿»¹Ã»ÓĞ³¬¹ıÈıÁ´ÉèÖÃµÄÉÏÏŞ
				//Ö»Ğè½«´ËslabÌí¼Óµ½¿ÕslabÁ´±íÖĞ
				list_add(&slabp->list, &l3->slabs_free);
			}
		} else {
			/* Unconditionally move a slab to the end of the
			 * partial list on free - maximum time for the
			 * other objects to be freed, too.
			 */
			 //½«´ËslabÌí¼Óµ½²¿·ÖÂúµÄÁ´±íÖĞ
			list_add_tail(&slabp->list, &l3->slabs_partial);
		}
	}
}

static void cache_flusharray(struct kmem_cache *cachep, struct array_cache *ac)
{
	int batchcount;
	struct kmem_list3 *l3;
	int node = numa_node_id();

	//±¾µØ»º´æÄÜÒ»´Î×ª³ö¶àÉÙ¸ö¶ÔÏó£¬Õâ¸öÊÇÖ®Ç°¹æ¶¨µÄ
	batchcount = ac->batchcount;
#if DEBUG
	BUG_ON(!batchcount || batchcount > ac->avail);
#endif
	check_irq_off();
	//»ñµÃ´Ë»º´æÆ÷µÄÈıÁ´
	l3 = cachep->nodelists[node];
	spin_lock(&l3->list_lock);
	//¿´ÊÇ·ñ´æÔÚ±¾µØ¹²Ïí»º´æ
	if (l3->shared) {
		struct array_cache *shared_array = l3->shared;
		//±¾µØ ¹²Ïí »º´æ»¹¿É³ĞÔØµÄ×î´óÊıÄ¿
		int max = shared_array->limit - shared_array->avail;
		if (max) {
			//×î´óÖ»ÄÜÎªmax
			if (batchcount > max)
				batchcount = max;
			//½«±¾µØ»º´æÇ°ÃæµÄ¼¸¸ö¶ÔÏó×ªÈë±¾µØ¹²Ïí»º´æÖĞ£¬ÒòÎªÇ°ÃæµÄÊÇ×îÔç²»ÓÃµÄ
			memcpy(&(shared_array->entry[shared_array->avail]),
			       ac->entry, sizeof(void *) * batchcount);
			//¸üĞÂ±¾µØ¹²Ïí»º´æ
			shared_array->avail += batchcount;
			goto free_done;
		}
	}

	//Ã»ÓĞÅäÖÃ±¾µØ¹²Ïí»º´æ£¬Ö»ÄÜÊÍ·Å¶ÔÏóµ½ÈıÁ´ÖĞ
	//×¢Òâ´ËÊ±µÄ batchcount ¾ÍÊÇÔ­Ê¼µÄ batchcount£¬Ò²¾ÍÊÇËµ¿ÉÒÔ°Ñ´ïµ½±¾µØ»º´æÒ»´ÎĞÔ×ª³ö batchcount µÄÄ¿±ê
	//¶øÉÏÃæµÄ±¾µØ¹²Ïí»º´æÈç¹ûÊ¹ÓÃµÄ»°£¬ÓĞ¿ÉÄÜ´ï²»µ½Õâ¸öÄ¿±ê£¬ÒòÎªËüÒ²ÓĞ limit
	//²»¹ı¼´±ã´ï²»µ½£¬ÓÉÓÚ±¾µØ¹²Ïí»º´æĞ§ÂÊ±ÈÈıÁ´¸ß£¬ÕâÖÖÇé¿öÒ²²»»áÔÚµ½ÈıÁ´À´£¬¶øÊÇÖ±½Ógoto free_done¡£
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
	//¸üĞÂ±¾µØ»º´æµÄÇé¿ö
	ac->avail -= batchcount;
	//°ÑºóÃæµÄÒÆ¶¯µ½±¾µØ»º´æÊı×éÇ°ÃæÀ´
	memmove(ac->entry, &(ac->entry[batchcount]), sizeof(void *)*ac->avail);
}

/*
 * Release an obj back to its cache. If the obj has a constructed state, it must
 * be in this state _before_ it is released.  Called with disabled ints.
 */   //»ØÊÕº¯Êı
static inline void __cache_free(struct kmem_cache *cachep, void *objp)
{
	//»ñµÃ±¾CPUµÄ±¾µØ»º´æ
	struct array_cache *ac = cpu_cache_get(cachep);

	check_irq_off();
	objp = cache_free_debugcheck(cachep, objp, __builtin_return_address(0));

	//NUMAÏà¹Ø£¬Ä¿Ç°°æ±¾ÊÇ¿Õº¯Êı
	if (cache_free_alien(cachep, objp))
		return;

	//ÏÂÃæ¿ªÊ¼Ñ¡ÔñÊÍ·ÅÎ»ÖÃ½øĞĞÊÍ·Å

	//±¾µØ»º´æÖĞµÄ¿ÕÏĞ¶ÔÏóĞ¡ÓÚÉÏÏŞÊ±£¬Ö»Ğè½«¶ÔÏóÊÍ·Å»ØentryÊı×éÖĞ
	if (likely(ac->avail < ac->limit)) {
		STATS_INC_FREEHIT(cachep);
		ac->entry[ac->avail++] = objp;
		return;
	} else {
		//ÕâÊÇ±¾µØ»º´æ¿ÕÏĞ¶ÔÏó´óÓÚÉÏÏŞµÄÇé¿ö£¬ÏÈµ÷Õû±¾µØ»º´æ
		STATS_INC_FREEMISS(cachep);
		cache_flusharray(cachep, ac);
		//²»¹ıÖ®ºó»¹ÊÇÒª°Ñ¸Ã¶ÔÏóÊÍ·Å¸ø±¾µØ»º´æ
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
	//µ×²ãµ÷ÓÃ__cache_allocº¯Êı
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
	cachep = __find_general_cachep(size, flags);  //ÕÒµ½Ò»¿éºÏÊÊ´óĞ¡µÄkmem_cache£¬·µ»ØÖµÊÇËüµÄÖ¸Õë
	if (unlikely(cachep == NULL))
		return NULL;
	return __cache_alloc(cachep, flags, caller);  //µ×²ãµ÷ÓÃ__cache_alloc
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
 //    »ØÊÕ !!  Ö®Ç°´ÓslabÖĞÉêÇëµÄ¶ÔÏó
void kmem_cache_free(struct kmem_cache *cachep, void *objp)
{
	unsigned long flags;
	BUG_ON(virt_to_cache(objp) != cachep);
	local_irq_save(flags);
	debug_check_no_locks_freed(objp, obj_size(cachep));
	
	//µ×²ãµ÷ÓÃ__cache_free()º¯Êı
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
 //³õÊ¼»¯±¾µØ ¹²Ïí »º´æºÍÈıÁ´£¬³õÊ¼»¯²»»áÎªÈıÁ´·ÖÅäslab
static int alloc_kmemlist(struct kmem_cache *cachep)
{
	int node;
	struct kmem_list3 *l3;
	struct array_cache *new_shared;
	struct array_cache **new_alien = NULL;

	for_each_online_node(node) {
		//NUMAÏà¹Ø
                if (use_alien_caches) {
                        new_alien = alloc_alien_cache(node, cachep->limit);
                        if (!new_alien)
                                goto fail;
                }

		new_shared = NULL;
		if (cachep->shared) {
			//Èç¹ûÖ§³Öshared£¬¾Í·ÖÅä±¾µØ¹²Ïí»º´æ
			new_shared = alloc_arraycache(node,
				cachep->shared*cachep->batchcount,
					0xbaadf00d);   //batchcountÕâÃ´´ó£¬3131961357
			if (!new_shared) {
				free_alien_cache(new_alien);
				goto fail;
			}
		}

		//»ñµÃ¾ÉµÄÈıÁ´
		l3 = cachep->nodelists[node];
		if (l3) {  //¾ÉÈıÁ´Ö¸Õë²»Îª¿Õ£¬ĞèÒªÏÈÊÍ·Å¾ÉµÄ×ÊÔ´
			struct array_cache *shared = l3->shared;

			spin_lock_irq(&l3->list_lock);

			if (shared)  //ÊÍ·Å¾ÉµÄ±¾µØ¹²Ïí»º´æ
				free_block(cachep, shared->entry,
						shared->avail, node);

			//Ö¸ÏòĞÂµÄ±¾µØ¹²Ïí»º´æ
			l3->shared = new_shared;
			if (!l3->alien) {
				l3->alien = new_alien;
				new_alien = NULL;
			}
			//¼ÆËã»º´æÆ÷ÖĞ¿ÕÏĞ¶ÔÏóµÄÉÏÏŞ
			l3->free_limit = (1 + nr_cpus_node(node)) *
					cachep->batchcount + cachep->num;
			spin_unlock_irq(&l3->list_lock);
			//ÊÍ·Å¾ÉµÄ±¾µØ¹²Ïí»º´æºÍ±¾µØ»º´æ
			kfree(shared);
			free_alien_cache(new_alien);
			continue;
		}
		//Èç¹ûÃ»ÓĞ¾ÉµÄÈıÁ´£¬ÄÇ¾ÍÒª·ÖÅäÒ»¸öĞÂµÄÈıÁ´
		l3 = kmalloc_node(sizeof(struct kmem_list3), GFP_KERNEL, node);
		if (!l3) {
			free_alien_cache(new_alien);
			kfree(new_shared);
			goto fail;
		}

		//³õÊ¼»¯ÈıÁ´
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

//¸üĞÂÃ¿¸öCPUµÄarray_cache¶ÔÏó
static void do_ccupdate_local(void *info)
{
	struct ccupdate_struct *new = info;  //¶î£¬ºÍlibeventÒ»ÑùµÄÇ¿ÖÆ×ª»»£¬»»C++¿Ï¶¨±¨´íÁË:)
	struct array_cache *old;

	check_irq_off();
	//»ñµÃ¾ÉµÄ±¾µØ»º´æ
	old = cpu_cache_get(new->cachep);

	//Ö¸ÏòĞÂµÄ array_cache ¶ÔÏó£¬new ÊÇÖ®Ç°·ÖÅäµÄ±¾µØ»º´æµÄÒıÓÃ
	new->cachep->array[smp_processor_id()] = new->new[smp_processor_id()];
	//±£´æ¾ÉµÄ array_cache ¶ÔÏó
	new->new[smp_processor_id()] = old;
}
/*
struct ccupdate_struct {
	struct kmem_cache *cachep;
	struct array_cache *new[NR_CPUS];
};
*/
/* Always called with the cache_chain_mutex held */
//ÅäÖÃ±¾µØ»º´æ¡¢±¾µØ¹²Ïí»º´æºÍÈıÁ´
static int do_tune_cpucache(struct kmem_cache *cachep, int limit,
				int batchcount, int shared)
{
	struct ccupdate_struct *new;
	int i;

	//ÉêÇë·ÖÅäÒ»¸ö ccupdate_struct ²¢ÇåÁã£¬×¢ÒâÕâÀï g_cpucache_up == FULL ²Åµ½ÕâÀïÀ´µÄ£¬ËùÒÔ¿ÉÒÔÓÃ kmalloc
	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return -ENOMEM;

	//ÎªÃ¿¸öCPU·ÖÅäĞÂµÄarray_cache¶ÔÏó
	for_each_online_cpu(i) {
		new->new[i] = alloc_arraycache(cpu_to_node(i), limit,
						batchcount);
		if (!new->new[i]) {  //Èç¹ûÊ§°Ü
			for (i--; i >= 0; i--)  //commit and rollback
				kfree(new->new[i]);
			kfree(new);
			return -ENOMEM;
		}
	}
	new->cachep = cachep;   //ÓÃ new °Ñ¾ÉµÄ»º´æÆ÷×÷Îª×Ô¼ºµÄ³ÉÔ±£¬ÔÚon_each_cpu() º¯ÊıÖĞ·½±ã¸üĞÂ¾ÉµÄ»º´æÆ÷µÄ±¾µØ»º´æ
	
	//ÓÃĞÂµÄarray_cache¶ÔÏóÌæ»»¾ÉµÄarray_cache¶ÔÏó£¬ÔÚÖ§³ÖCPUÈÈ²å°ÎµÄÏµÍ³ÉÏ£¬ÀëÏßCPU¿ÉÄÜÃ»ÓĞÊÍ·Å±¾µØ»º´æ£¬Ê¹ÓÃµÄÈÔÊÇ¾É±¾µØ»º´æ
	//²Î¼û__kmem_cache_destroy()º¯Êı¡£ËäÈ»cpu up Ê±ÒªÖØĞÂÅäÖÃ±¾µØ»º´æ£¬Ò²ÎŞ¼ÃÓÚÊÂ¡£
	//¿¼ÂÇÏÂÃæµÄÇé¾°; ¹²ÓĞCPUA ºÍ CPUB£¬CPUB downºó£¬destroy Cache X£¬ÓÉÓÚ´ËÊ±CPUB ÊÇdown×´Ì¬£¬
	//ËùÒÔCache XÖĞµÄ CPUB µÄ±¾µØ»º´æÎ´ÊÍ·Å£¬¹ıÒ»¶ÎÊ±¼äºóCPUBÓÖÆô¶¯ÁË£¬¸üĞÂ cache_chain Á´ÖĞËùÓĞcacheµÄ±¾µØ»º´æ
	//µ«´ËÊ±Cache X¶ÔÏóÒÑ¾­ÊÍ·Å»Ø cache_cacheÖĞÁË£¬ÆäCPUB µÄ±¾µØ»º´æ²¢Î´¸üĞÂ¡£ÓÖ¹ıÁËÒ»¶ÎÊ±¼ä£¬ÏµÍ³ĞèÒª´´½¨ĞÂµÄcache£¬
	//½« Cache X¶ÔÏó·ÖÅä³öÈ¥£¬ÆäCPUB ÈÔÈ»ÊÇ¾ÉµÄ±¾µØ»º´æ£¬ĞèÒª½øĞĞ¸üĞÂ
	on_each_cpu(do_ccupdate_local, (void *)new, 1, 1);  //µ÷ÓÃÁËdo_ccpudate_localº¯Êı£¬ÓÃĞÂµÄÌæ»»¾ÉµÄ

	check_irq_on();
	cachep->batchcount = batchcount;
	cachep->limit = limit;
	cachep->shared = shared;

	for_each_online_cpu(i) {
		struct array_cache *ccold = new->new[i];
		if (!ccold)
			continue;
		spin_lock_irq(&cachep->nodelists[cpu_to_node(i)]->list_lock);
		//ÊÍ·Å¾ÉµÄ±¾µØ»º´æÖĞµÄ  ¶ÔÏó  
		free_block(cachep, ccold->entry, ccold->avail, cpu_to_node(i));
		spin_unlock_irq(&cachep->nodelists[cpu_to_node(i)]->list_lock);
		//ÊÍ·Å¾ÉµÄarray_cache
		kfree(ccold);
	}
	kfree(new);
	//³õÊ¼»¯±¾µØ ¹²Ïí »º´æºÍÈıÁ´
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
	 //¸ù¾İÃ¿¸ö»º´æÆ÷·ÖÅä ¶ÔÏó µÄ´óĞ¡¼ÆËã  ±¾µØ»º´æ!!!  ÖĞµÄ¶ÔÏóÊıÄ¿ÉÏÏŞ
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
	 * CPU bound(ÓĞÒåÎñµÄ) tasks (e.g. network routing) can exhibit(Õ¹ÀÀ£¬Õ¹Ê¾) cpu bound
	 * allocation behaviour: Most allocs on one cpu, most free operations    //´ó¶àÊıÇé¿öÔÚ±¾CPUÉêÇë»º´æ£¬ÔÚÆäËûCPUÊÍ·Å»º´æ¡£(Õı½â)
	 * on another cpu. For these cases, an efficient object passing between
	 * cpus is necessary. This is provided by a shared array. The array
	 * replaces Bonwick's magazine layer.
	 * On uniprocessor(µ¥½ø³Ì), it's functionally equivalent(ÏàµÈµÄ) (but less efficient)
	 * to a larger limit. Thus disabled by default.   //µ¥´¦ÀíÆ÷Ä¬ÈÏÊÇ¹Ø±ÕµÄ
	*/
	shared = 0;
	//¶àºËÏµÍ³£¬ÉèÖÃ±¾µØ¹²Ïí»º´æÖĞ¶ÔÏóÊıÄ¿
	if (cachep->buffer_size <= PAGE_SIZE && num_possible_cpus() > 1)
		shared = 8;   //ÉèÖÃÎª8

#if DEBUG
	/*
	 * With debugging enabled, large batchcount lead to excessively long
	 * periods with disabled local interrupts. Limit the batchcount
	 */
	if (limit > 32)
		limit = 32;
#endif
	//ÅäÖÃ±¾µØ»º´æ
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
			//¼ÆËãÊÍ·Å¶ÔÏóµÄÊıÄ¿£¬¿É¼ûÕâ¸öº¯Êı»¹Ö§³Ö²¿·ÖÊÍ·Å£¬È¡¾öÓÚforceµÄboolÊôĞÔ
			//´Ó drain_cpu_caches()½øÈëÊ±£¬force=1£¬ÊÇÒªÈ«²¿ÊÍ·ÅµÄ
			tofree = force ? ac->avail : (ac->limit + 4) / 5;
			if (tofree > ac->avail)
				tofree = (ac->avail + 1) / 2;
			//ÊÍ·Å¶ÔÏó£¬´ÓentryÇ°Ãæ¿ªÊ¼
			free_block(cachep, ac->entry, tofree, node);
			ac->avail -= tofree;
			//ºóÃæµÄ¶ÔÏóÇ°ÒÆ
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
			//ÊÍ·Å¿ÕÁ´±íµÄslab
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
