
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "internal.h"

#define MEM_BLOCK_SIZE      4096
#define MEM_BLOCK_OVERHEAD    32

static void *default_alloc(size_t size, void *opaque)
{
	(void)opaque;
	return malloc(size);
}

static void default_free(void *p, void *opaque)
{
	(void)opaque;
	free(p);
}

static bool block_valid(struct mem_block *b)
{
	bool retval = true;

	if (b->prev && (b->prev->avail < b->avail))
		retval = false;

	if (b->next && (b->next->avail > b->avail))
		retval = false;

	return retval;
}

static void *block_alloc(struct mem_block *b, size_t size)
{
	assert(b->avail >= size);

	void *retval = (void *)(b->buf + b->used);
	b->used += size;
	b->avail -= size;

	return retval;
}

static void cache_insert(struct mem_block_cache *c, struct mem_block *b)
{
	if (c->head == NULL) {
		/*
		 * if head is NULL, this will be the first block
		 * in the cache; just add it to the head of the list
		 * and get out of here
		 */
		assert(c->num_blocks == 0);
		assert(c->total_used == 0);
		assert(c->total_avail == 0);
		assert(c->tail == NULL);
		assert(c->root == NULL);

		b->prev = NULL;
		b->next = NULL;

		c->num_blocks = 1;
		c->total_used = b->used;
		c->total_avail = b->avail;
		c->head = b;
		c->tail = b;
		c->root = b;

		return;
	}

	struct mem_block *node = c->head;
	bool inserted = false;

	assert(c->head != NULL);
	assert(c->tail != NULL);

	/*
	 * loop through each block in the cache. insert this
	 * block so that the list will be sorted in descending
	 * order based on avail size
	 */
	while (node) {
		if (b->avail > node->avail) {
			b->prev = node->prev;
			b->next = node;

			if (c->head == node) {
				/* this block will be the new head */
				assert(node->prev == NULL);
				c->head = b;
			} else {
				/*
				 * this block will be inserted in the middle
				 * of the list
				 */
				assert(node->prev != NULL);
				node->prev->next = b;
			}

			node->prev = b;
			inserted = true;
			break;
		}

		node = node->next;
	}

	if (!inserted) {
		/*
		 * if this block is smaller than all the others
		 * insert it at the end of the list
		 */
		b->prev = c->tail;
		b->next = NULL;
		c->tail->next = b;
		c->tail = b;
	}

	/* finally, update the cache counters */
	c->num_blocks++;
	c->total_used += b->used;
	c->total_avail += b->avail;
}

static void cache_remove(struct mem_block_cache *c, struct mem_block *b)
{
	const bool is_head = (c->head == b);
	const bool is_tail = (c->tail == b);

	if (b->prev) {
		assert(c->head != b);
		b->prev->next = b->next;

		if (is_tail)
			c->tail = b->prev;
	}

	if (b->next) {
		assert(c->tail != b);
		b->next->prev = b->prev;

		if (is_head)
			c->head = b->next;
	}

	c->num_blocks--;
}

/*
 * find a block that can satisfy an allocation of
 * the given size
 */
static struct mem_block *cache_find(struct mem_block_cache *c, size_t size)
{
	struct mem_block *block = c->head;
	struct mem_block *retval = NULL;

	/* find the smallest block that has size available */
	while (block) {
		if (block->avail >= size)
			retval = block;
		else
			break;

		block = block->next;
	}

	return retval;
}

static void cache_free(struct mem_block_cache *c)
{
}

static struct mem_block *create_block(
		struct mem_internal *mem,
		size_t min_size)
{
	size_t size;

	/* grow the allocation size by two every time */
	size = MEM_BLOCK_SIZE << mem->cache.num_blocks;
	size -= MEM_BLOCK_OVERHEAD;

	/*
	 * if the requested allocation is larger than
	 * the computed block size, use that size
	 * instead
	 */
	if (min_size > size)
		size = min_size;

	/* allocate the new block */
	struct mem_block *b;

	b = mem->funcs.alloc_func(size, mem->funcs.opaque);
	if (!b)
		return NULL;

	/* initialize the new block */
	b->buf = (unsigned char *)b;
	b->used = sizeof(struct mem_block);
	b->avail = size - b->used;

	return b;
}

void *mem_alloc(ptab_t *p, size_t size)
{
	/* ensure we're not getting garbage */
	if (!p)
		return NULL;

	struct mem_block_cache *cache = &p->mem.cache;
	struct mem_block *block;

	/* find a block large enough to allocate size */
	block = cache_find(cache, size);
	if (!block) {
		/* if none are large enough, then create a new one */
		block = create_block(&p->mem, size);
		if (!block)
			return NULL;

		/*
		 * insert the new block into the list for consistency
		 * with the other code path
		 */
		cache_insert(cache, block);
	}

	/* make the allocation */
	void *retval;
	retval = block_alloc(block, size);
	assert(retval != NULL);

	/*
	 * check if the block is in the right place:
	 *  - it is smaller than prev
	 *  - it is greater than next
	 */
	if (!block_valid(block)) {
		/*
		 * if it's not valid, remove it and add it again
		 * so that it will be in the right place
		 */
		cache_remove(cache, block);
		cache_insert(cache, block);
	}

	return retval;
}

ptab_t *mem_init(const ptab_allocator_t *funcs_)
{
	/*
	 * set up allocator functions on the stack since
	 * our internal object hasn't been created yet
	 */
	ptab_allocator_t funcs;

	if (!funcs_ || !funcs_->alloc_func || !funcs_->free_func)
		funcs = (ptab_allocator_t){default_alloc, default_free, NULL};
	else
		funcs = *funcs_;

	/*
	 * calculate initial allocation size and allocate
	 * the internal structure
	 */
	ptab_t *p;
	size_t size;

	size = MEM_BLOCK_SIZE - MEM_BLOCK_OVERHEAD;
	assert(sizeof(ptab_t) < size);

	p = funcs.alloc_func(size, funcs.opaque);
	if (!p)
		return NULL;

	/* zero the structure */
	memset(p, 0, sizeof(ptab_t));

	/*
	 * initialize root mem_block
	 * this block should never be free'd since the
	 * ptab_internal structure resides in it
	 */
	struct mem_block *block;
	block = (struct mem_block *)(p + 1);
	block->buf = (unsigned char *)p;
	block->used = sizeof(ptab_t) + sizeof(struct mem_block);
	block->avail = size - block->used;

	cache_insert(&p->mem.cache, block);
	p->mem.cache.root = block;

	/* set the allocators in the structure */
	p->mem.funcs = funcs;

	return p;
}

/* FIXME! this is temporary */
void *ptab_alloc(ptab_t *p, size_t size)
{
	void *retval;

	retval = mem_alloc(p, size);

	return retval;
}
