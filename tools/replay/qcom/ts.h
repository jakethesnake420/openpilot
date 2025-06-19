#ifndef _TS_H
#define _TS_H

#include <stdint.h>
#include <stdlib.h>
#include "common.h"
// Add this include if you have a list implementation header, e.g.:



struct ts_entry {
	uint64_t pts;
	uint64_t dts;
	uint64_t duration;
	uint64_t base;
	struct list_head link;
};


#define TIMESTAMP_NONE	((uint64_t)-1)

static struct ts_entry *
ts_insert(struct video *vid, uint64_t pts, uint64_t dts, uint64_t duration,
	  uint64_t base)
{
	struct ts_entry *l;

	l = (struct ts_entry *)malloc(sizeof (*l));
	if (!l)
		return NULL;

	l->pts = pts;
	l->dts = dts;
	l->duration = duration;
	l->base = base;

	list_add_tail(&l->link, &vid->pending_ts_list);

	return l;
}

static void
ts_remove(struct ts_entry *l)
{
	list_del(&l->link);
	free(l);
}

#endif // _TS_H