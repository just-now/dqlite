#ifndef ___COROUTINE_H__
#define ___COROUTINE_H__

#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "../tracing.h"

#define ASSERT assert
#define ergo ERGO
#define LOG(arg, ...) tracef(__VA_ARGS__)

struct raft_message;
struct co_context *co_context(struct raft_message *message);

enum {
	MCC_STACK_NR            = 0x20,
	MCC_LOCALS_ALLOC_SZ     = 4096,
	MCC_LOCALS_ALLOC_SHIFT  = 3,
	MCC_LOCALS_ALLOC_ALIGN  = 1ULL << MCC_LOCALS_ALLOC_SHIFT,
	MCC_LOCALS_ALLOC_PAD_SZ = 2 * MCC_LOCALS_ALLOC_ALIGN,
};

struct co_la_item {
	void     *lai_addr;
	uint64_t  lai_size;
};

struct co_locals_allocator {
	struct co_la_item la_items[MCC_STACK_NR];
	uint64_t          la_frame;
	void             *la_pool;
	uint64_t          la_total;
};

struct co_context {
	void                         *mc_stack[MCC_STACK_NR];
	void                         *mc_locals[MCC_STACK_NR];
	uint64_t                      mc_frame;
	bool                          mc_yield;
	uint64_t                      mc_yield_frame;
	struct co_locals_allocator    mc_alloc;
};

#define CO_START(context)						\
({									\
	ASSERT((context)->mc_yield_frame == 0);				\
})

#define CO_END(context)							\
({                                                                      \
	int _rc = ((context)->mc_yield ? -EAGAIN : 0);			\
	if (_rc == 0) {							\
		ASSERT((context)->mc_frame == 0);			\
		ASSERT((context)->mc_yield_frame == 0);			\
		co_context_locals_free((context));			\
	}								\
	_rc;								\
})

#define CO_FUN(context, function)					\
({                                                                      \
	__label__ save;							\
	LOG(CALL, "CO_FUN: context=%p yeild=%d",			\
	    context, !!context->mc_yield);				\
	ASSERT(context->mc_frame < MCC_STACK_NR);			\
	context->mc_stack[context->mc_frame++] = &&save;		\
  save:   (function);							\
	if (context->mc_yield) {					\
		return;							\
	} else {							\
		co_context_locals_free(context);			\
		context->mc_frame--;					\
	}								\
})

#define CO_FRAME_DATA(field) (__frame_data__->field)
#define CO_FRAME_DATA_L __frame_data__

#define CO_REENTER(context, ...)					\
	struct foo_context {						\
		__VA_ARGS__						\
			};						\
	struct foo_context *__frame_data__;				\
	CO__REENTER((context), __frame_data__);

#define CO__REENTER(context, frame_data)				\
({                                                                      \
	uint64_t size = sizeof(*frame_data);				\
	LOG(CALL, "CO_REENTER: context=%p yeild=%d",			\
	    context, !!context->mc_yield);				\
	if (!context->mc_yield) {					\
		co_context_locals_alloc(context, (size));		\
		frame_data = co_context_locals(context);		\
	} else {							\
		ASSERT(context->mc_yield_frame < MCC_STACK_NR);		\
		frame_data = co_context_locals(context);		\
		goto *context->mc_stack[context->mc_yield_frame++];	\
	}								\
})

#define CO_YIELD(context)						\
({                                                                      \
	__label__ save;							\
	LOG(CALL, "CO_YIELD: context=%p yeild=%d",			\
	    context, !!context->mc_yield);				\
	context->mc_yield = true;					\
	ASSERT(context->mc_frame < MCC_STACK_NR);			\
	context->mc_stack[context->mc_frame++] = &&save;		\
	return;								\
  save:									\
	ASSERT(context->mc_yield);					\
	ASSERT(context->mc_frame == context->mc_yield_frame);		\
	context->mc_yield = false;					\
	context->mc_yield_frame = 0;					\
	context->mc_frame--;						\
})


int co_context_init(struct co_context *context);
void co_context_fini(struct co_context *context);

void *co_context_locals(struct co_context *context);
void co_context_locals_alloc(struct co_context *context,
				uint64_t size);
void co_context_locals_free(struct co_context *context);

#endif /* ___COROUTINE_H__ */
