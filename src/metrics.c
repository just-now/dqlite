#include <stdlib.h>

#include "./lib/assert.h"

#include "metrics.h"
#include "utils.h"

void dqlite__metrics_init(struct dqlite__metrics *m)
{
	assert(m != NULL);

	m->requests = 0;
	m->duration = 0;
}

#include <stdatomic.h>
uint64_t id_generate(void)
{
	static int64_t an_id = 0;
	return (uint64_t) __sync_add_and_fetch(&an_id, 1);
}
