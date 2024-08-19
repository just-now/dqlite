#include "coro.h"

#define ASSERT assert

static inline uint64_t align(uint64_t val, uint64_t alignment)
{
	uint64_t mask;

	PRE(is_po2(alignment));
	mask = alignment - 1;
	return (val + mask) & ~mask;
}

static inline bool is_aligned(uint64_t val, uint64_t alignment)
{
	uint64_t mask;

	PRE(is_po2(alignment));
	mask = alignment - 1;
	return (val & mask) == 0;
}

static void free_aligned(void *data, size_t size, unsigned shift)
{
	(void) size;
	(void) shift;
	free(data);
}

static void *alloc_aligned(size_t size, size_t alignment)
{
	int   rc;
	void *result;

	rc = posix_memalign(&result, 1 << alignment, size);
	if (rc != 0)
		result = NULL;

	return result;
}

static int locals_alloc_init(struct co_locals_allocator *alloc)
{
	alloc->la_pool = alloc_aligned(MCC_LOCALS_ALLOC_SZ,
					  MCC_LOCALS_ALLOC_SHIFT);
	alloc->la_frame = 0;
	return alloc->la_pool == NULL ? -ENOMEM : 0;
}

static void locals_alloc_fini(struct co_locals_allocator *alloc)
{
	PRE(alloc->la_frame == 0);
	free_aligned(alloc->la_pool, MCC_LOCALS_ALLOC_SZ,
			MCC_LOCALS_ALLOC_SHIFT);
}

static void *locals_alloc(struct co_locals_allocator *alloc, uint64_t frame,
			  uint64_t size)
{
	struct co_la_item *curr;
	struct co_la_item *prev;
	uint64_t              i;
	uint64_t              aligned_sz = align(size +
						    MCC_LOCALS_ALLOC_PAD_SZ,
						    MCC_LOCALS_ALLOC_ALIGN);
	PRE(alloc->la_frame == frame);

	curr = &alloc->la_items[alloc->la_frame];
	if (alloc->la_frame == 0) {
		curr->lai_addr = alloc->la_pool;
		curr->lai_size = aligned_sz;
		alloc->la_total = curr->lai_size;
	} else {
		prev = &alloc->la_items[alloc->la_frame - 1];
		curr->lai_addr = prev->lai_addr + prev->lai_size;
		ASSERT(is_aligned((uint64_t) curr->lai_addr,
					MCC_LOCALS_ALLOC_ALIGN));
		curr->lai_size = aligned_sz;
		alloc->la_total += curr->lai_size;
	}

	ASSERT(alloc->la_total < MCC_LOCALS_ALLOC_SZ);
	ASSERT(alloc->la_frame < MCC_STACK_NR);

	/* test memory's zeroed */
	for (i = 0; i < curr->lai_size; ++i)
		ASSERT(((uint8_t*) curr->lai_addr)[i] == 0x00);

	memset(curr->lai_addr, 0xCC, aligned_sz);
	alloc->la_frame++;

	return curr->lai_addr;
}

static void locals_free(struct co_locals_allocator *alloc, uint64_t frame)
{
	uint64_t              i;
	struct co_la_item *curr;

	curr = &alloc->la_items[--alloc->la_frame];
	//PRE(alloc->la_frame >= 0);
	PRE(alloc->la_frame == frame);

	/* test pad is CC-ed */
	for (i = curr->lai_size - MCC_LOCALS_ALLOC_PAD_SZ;
	     i < curr->lai_size; ++i)
		ASSERT(((uint8_t*) curr->lai_addr)[i] == 0xCC);

	memset(curr->lai_addr, 0x00, curr->lai_size);
	alloc->la_total -= curr->lai_size;
	curr->lai_addr = NULL;
	curr->lai_size = 0;

	ASSERT(ergo(frame == 0, alloc->la_total == 0));
}

void co_context_locals_alloc(struct co_context *context,
					    uint64_t size)
{
	context->mc_locals[context->mc_frame] =
		locals_alloc(&context->mc_alloc, context->mc_frame, size);

	LOG(CALL, "alloc=%p size=%"PRIu64,
	       context->mc_locals[context->mc_frame], size);
}

void co_context_locals_free(struct co_context *context)
{
	LOG(CALL, "free=%p", context->mc_locals[context->mc_frame]);

	locals_free(&context->mc_alloc, context->mc_frame);
	context->mc_locals[context->mc_frame] = NULL;
}

void *co_context_locals(struct co_context *context)
{
	return context->mc_locals[context->mc_yield ? context->mc_yield_frame :
				  context->mc_frame];
}

int co_context_init(struct co_context *context)
{
	*context = (struct co_context) { .mc_yield = false };
	return locals_alloc_init(&context->mc_alloc);
}

void co_context_fini(struct co_context *context)
{
	locals_alloc_fini(&context->mc_alloc);
}
