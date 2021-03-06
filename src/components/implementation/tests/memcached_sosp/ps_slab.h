/***
 * Copyright 2011-2015 by Gabriel Parmer.  All rights reserved.
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 *
 * Author: Gabriel Parmer, gparmer@gwu.edu, 2011
 *
 * History:
 * - Initial slab allocator, 2011
 * - Adapted for parsec, 2015
 */

#ifndef  PS_SLAB_H
#define  PS_SLAB_H

#include "ps_list.h"
#include "ps_plat.h"
#include "ps_global.h"

/* #define PS_SLAB_DEBUG 1 */

/* The header for a slab. */
struct ps_slab {
	/*
	 * Read-only data.  coreid is read by _other_ cores, so we
	 * want it on a separate cache-line from the frequently
	 * modified stuff.
	 */
	void  *memory;		/* != NULL iff slab is separately allocated */
	/* ps_desc_t start, end;	/\* A slab used as a namespace: min and max descriptor ids *\/ */
	size_t    memsz;	/* size of backing memory */
	coreid_t  coreid;	/* which is the home core for this slab? */
	char   pad[PS_CACHE_LINE-(sizeof(void *)+sizeof(size_t)+sizeof(u16_t)+sizeof(ps_desc_t)*2)];

	/* Frequently modified data on the owning core... */
	struct ps_mheader *freelist; /* free objs in this slab */
	struct ps_list     list;     /* freelist of slabs */
	size_t             nfree;    /* # allocations in freelist */
} PS_PACKED;


static inline struct ps_mheader *
__ps_qsc_peek(struct ps_qsc_list *ql)
{ return ql->head; }

static inline void
__ps_qsc_enqueue(struct ps_qsc_list *ql, struct ps_mheader *n)
{
	struct ps_mheader *t;

	t = ql->tail;
	if (likely(t)) t->next  = ql->tail = n;
	else           ql->head = ql->tail = n;
#ifdef ENABLE_CC_OP
	if (likely(t)) cos_wb_cache(&(t->next));
	cos_wb_cache(ql);
	asm volatile ("sfence"); /* serialize */
#endif
}

static inline struct ps_mheader *
__ps_qsc_dequeue(struct ps_qsc_list *ql)
{
	struct ps_mheader *a = ql->head;

	if (a) {
		ql->head = a->next;
		if (unlikely(ql->tail == a)) ql->tail = NULL;
		a->next = NULL;
#ifdef ENABLE_CC_OP
		clwb_range(ql, (char *)ql + CACHE_LINE);
#endif
	}
	return a;
}

static inline struct ps_mheader *
__ps_qsc_clear(struct ps_qsc_list *l)
{
	struct ps_mheader *m = l->head;

	l->head = l->tail = NULL;

	return m;
}

/*** Operations on the freelist of slabs ***/

/*
 * These functions should really must be statically computed for
 * efficiency (see macros below)...
 */
static inline unsigned long
__ps_slab_objmemsz(size_t obj_sz)
{ return PS_RNDUP(obj_sz + sizeof(struct ps_mheader), PS_WORD); }
static inline unsigned long
__ps_slab_max_nobjs(size_t obj_sz, size_t allocsz, size_t headoff)
{ return (allocsz - headoff) / __ps_slab_objmemsz(obj_sz); }
/* The offset of the given object in its slab */
static inline unsigned long
__ps_slab_objsoff(struct ps_slab *s, struct ps_mheader *h, size_t obj_sz, size_t headoff)
{ return ((unsigned long)h - ((unsigned long)s->memory + headoff)) / __ps_slab_objmemsz(obj_sz); }

#ifdef PS_SLAB_DEBUG
static inline void
__ps_slab_check_consistency(struct ps_slab *s)
{
	struct ps_mheader *h;
	unsigned int i;

	assert(s);
	h = s->freelist;
	for (i = 0 ; h ; i++) {
		assert(h->slab == s);
		assert(h->tsc_free != 0);
		h = h->next;
	}
	assert(i == s->nfree);
}

static inline void
__ps_slab_freelist_check(struct ps_slab_freelist *fl)
{
	struct ps_slab *s = fl->list;
	
	if (!s) return;
	do {
		assert(s->memory && s->freelist);
		assert(ps_list_prev(ps_list_next(s, list), list) == s);
		assert(ps_list_next(ps_list_prev(s, list), list) == s);
		__ps_slab_check_consistency(s);
	} while ((s = ps_list_first(s, list)) != fl->list);
}
#else  /* PS_SLAB_DEBUG */
static inline void __ps_slab_check_consistency(struct ps_slab *s) { (void)s; }
static inline void __ps_slab_freelist_check(struct ps_slab_freelist *fl) { (void)fl; }
#endif /* PS_SLAB_DEBUG */

static void
__slab_freelist_rem(struct ps_slab_freelist *fl, struct ps_slab *s)
{
	assert(s && fl);
	if (fl->list == s) {
		if (ps_list_empty(s, list)) fl->list = NULL;
		else                        fl->list = ps_list_first(s, list);
	}
	ps_list_rem(s, list);
}

static void
__slab_freelist_add(struct ps_slab_freelist *fl, struct ps_slab *s)
{
	assert(s && fl);
	assert(ps_list_empty(s, list));
	assert(s != fl->list);
	if (fl->list) ps_list_add(fl->list, s, list);
	fl->list = s;
	/* TODO: sort based on emptiness...just use N bins */
}

/*** Alloc and free ***/

#define PS_SLAB_PARAMS coreid_t coreid, size_t obj_sz, size_t allocsz, size_t headoff, ps_alloc_fn_t afn, ps_free_fn_t ffn
#define PS_SLAB_ARGS coreid, obj_sz, allocsz, headoff, afn, ffn
#define PS_SLAB_DEWARN (void)coreid; (void)obj_sz; (void)allocsz; (void)headoff; (void)afn; (void)ffn

/* Create function prototypes for cross-object usage */
#define PS_SLAB_CREATE_PROTOS(name)			\
inline void  *ps_slab_alloc_##name(void);		\
inline void   ps_slab_free_##name(void *buf);		\
inline size_t ps_slab_objmem_##name(void);		\
inline size_t ps_slab_nobjs_##name(void);

void __ps_slab_mem_remote_free(struct ps_mem *mem, struct ps_mheader *h, coreid_t core_target);
void __ps_slab_mem_remote_process(struct ps_mem *mem, struct ps_slab_info *si, PS_SLAB_PARAMS);
void __ps_slab_init(struct ps_slab *s, struct ps_slab_info *si, PS_SLAB_PARAMS);
void ps_slab_deffree(struct ps_mem *m, struct ps_slab *x, size_t sz, coreid_t coreid);
struct ps_slab *ps_slab_defalloc(struct ps_mem *m, size_t sz, coreid_t coreid);
void ps_slabptr_init(struct ps_mem *m);
int ps_slabptr_isempty(struct ps_mem *m);

static inline void
__ps_slab_mem_free(void *buf, struct ps_mem *mem, PS_SLAB_PARAMS)
{
	struct ps_slab *s;
	struct ps_mheader *h, *next;
	unsigned int max_nobjs = __ps_slab_max_nobjs(obj_sz, allocsz, headoff);
	struct ps_slab_freelist *fl, *el;
	coreid_t target;
	assert(__ps_slab_objmemsz(obj_sz) + headoff <= allocsz);
	PS_SLAB_DEWARN;

	h = __ps_mhead_get(buf);
	assert(!__ps_mhead_isfree(h)); /* freeing freed memory? */
	s = h->slab;
	assert(s);
	assert(s->coreid == coreid);

	__ps_mhead_setfree(h, SLAB_FREE);
	next        = s->freelist;
	s->freelist = h; 	/* TODO: should be atomic/locked */
	h->next     = next;
	s->nfree++;		/* TODO: ditto */
	if (s->nfree == max_nobjs) {
		struct ps_slab_info *si = &mem->percore[coreid].slab_info;

		/* remove from the freelist */
		__slab_freelist_rem(&si->fl, s);
		si->nslabs--;
		assert(ps_list_empty(s, list));
		s->list.next = NULL;
	 	ffn(mem, s, s->memsz, coreid);
	}
	else if (s->nfree == 1) {
		fl = &mem->percore[coreid].slab_info.fl;
		el = &mem->percore[coreid].slab_info.el;
		__slab_freelist_rem(el, s);
		/* add back onto the freelists */
		assert(ps_list_empty(s, list));
		assert(s->memory && s->freelist);
		__slab_freelist_add(fl, s);
	}
	__ps_slab_freelist_check(&mem->percore[coreid].slab_info.fl);

	return;
}

static inline void *
__ps_slab_mem_alloc(struct ps_mem *mem, PS_SLAB_PARAMS)
{
	struct ps_slab      *s;
	struct ps_mheader   *h;
	struct ps_slab_info *si = &mem->percore[coreid].slab_info;
	assert(obj_sz + headoff <= allocsz);
	PS_SLAB_DEWARN;

	s = si->fl.list;
	if (unlikely(!s)) {
		/* allocation function must initialize s->memory */
		s = afn(mem, allocsz, coreid);
		if (unlikely(!s)) return NULL;
		
		__ps_slab_init(s, si, PS_SLAB_ARGS);
		si->nslabs++;
		assert(s->memory && s->freelist);
	}

	assert(s && s->freelist);
	/* TODO: atomic modification to the freelist */
	h           = s->freelist;
	s->freelist = h->next;
	h->next     = NULL;
	s->nfree--;
	__ps_mhead_reset(h);

	/* remove from the freelist */
	if (s->nfree == 0) {
		__slab_freelist_rem(&si->fl, s);
		assert(ps_list_empty(s, list));
		__slab_freelist_add(&si->el, s);
	}
	assert(!__ps_mhead_isfree(h));
	__ps_slab_freelist_check(&si->fl);

	return __ps_mhead_mem(h);
}


/***
 * This macro is very important for high-performance.  It creates the
 * functions for allocation and deallocation passing in the freelist
 * directly, and size information for these objects, thus enabling the
 * compiler to do partial evaluation.  This avoids freelist lookups,
 * and relies on the compilers optimizations to generate specialized
 * code for the given sizes -- requiring function inlining, constant
 * propagation, and dead-code elimination.  To me, relying on these
 * optimizations is better than putting all of the code for allocation
 * and deallocation in the macro due to maintenance and readability.
 */
#define __PS_SLAB_CREATE_FNS(name, obj_sz, allocsz, headoff, afn, ffn)			\
inline void *										\
ps_slabptr_alloc_##name(struct ps_mem *m, int node)					\
{ return __ps_slab_mem_alloc(m, node, obj_sz, allocsz, headoff, afn, ffn); }            \
inline void										\
ps_slabptr_free_coreid_##name(struct ps_mem *m, void *buf, coreid_t coreid)		\
{ __ps_slab_mem_free(buf, m, coreid, obj_sz, allocsz, headoff, afn, ffn); }		\
inline void										\
ps_slabptr_free_##name(struct ps_mem *m, void *buf)					\
{ ps_slabptr_free_coreid_##name(m, buf, ps_coreid()); }					\
inline void *										\
ps_slab_alloc_##name(int node)								\
{ return ps_slabptr_alloc_##name(&__ps_mem_##name, node); }				\
inline void										\
ps_slab_free_##name(void *buf)								\
{ ps_slabptr_free_##name(&__ps_mem_##name, buf); }					\
inline void										\
ps_slab_free_coreid_##name(void *buf, coreid_t curr)					\
{ ps_slabptr_free_coreid_##name(&__ps_mem_##name, buf, curr); }				\
inline void										\
ps_slabptr_init_##name(struct ps_mem *m)						\
{ ps_slabptr_init(m); }									\
inline void										\
ps_slab_init_##name(void)								\
{ ps_slabptr_init_##name(&__ps_mem_##name); }						\
inline struct ps_mem *									\
ps_slabptr_create_##name(coreid_t coreid)								\
{											\
	struct ps_mem *m = ps_plat_alloc(sizeof(struct ps_mem), coreid);		\
	if (m) ps_slabptr_init_##name(m);						\
	return m;									\
}											\
inline void										\
ps_slabptr_delete_##name(struct ps_mem *m)						\
{ ps_plat_free(m, sizeof(struct ps_mem), ps_coreid()); }				\
inline size_t										\
ps_slab_objmem_##name(void)								\
{ return __ps_slab_objmemsz(obj_sz); }							\
inline size_t										\
ps_slab_nobjs_##name(void)								\
{ return __ps_slab_max_nobjs(obj_sz, allocsz, headoff); }				\
inline unsigned int									\
ps_slab_objoff_##name(void *obj)							\
{											\
	struct ps_mheader *h = __ps_mhead_get(obj);					\
	return __ps_slab_objsoff(h->slab, h, obj_sz, headoff);				\
}

/*
 * allocsz is the size of the backing memory allocation, and
 * headintern is 0 or 1, should the ps_slab header be internally
 * allocated from that slab of memory, or from elsewhere.
 *
 * Note: if you use headintern == 1, then you must manually create
 * PS_SLAB_CREATE_DEF(meta, sizeof(struct ps_slab));
 */
#define PS_SLAB_CREATE_AFNS(name, size, allocsz, headoff, allocfn, freefn)		\
__PS_MEM_CREATE_DATA(name)								\
__PS_SLAB_CREATE_FNS(name, size, allocsz, headoff, allocfn, freefn)

#define PS_SLAB_CREATE(name, size, allocsz)								\
PS_SLAB_CREATE_AFNS(name, size, allocsz, sizeof(struct ps_slab), ps_slab_defalloc, ps_slab_deffree)

#define PS_SLAB_CREATE_DEF(name, size)							\
PS_SLAB_CREATE(name, size, PS_PAGE_SIZE)

#endif /* PS_SLAB_H */
