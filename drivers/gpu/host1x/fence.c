/*
 * Copyright (C) 2016 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>
#include <linux/slab.h>

#include "fence.h"
#include "intr.h"
#include "syncpt.h"
#include "cdma.h"
#include "channel.h"
#include "dev.h"

struct host1x_fence {
	struct dma_fence base;
	spinlock_t lock;

	struct host1x_syncpt *syncpt;
	u32 threshold;

	struct host1x *host;
	void *waiter;

	char timeline_name[10];
};

static inline struct host1x_fence *to_host1x_fence(struct dma_fence *fence)
{
	return (struct host1x_fence *)fence;
}

static const char *host1x_fence_get_driver_name(struct dma_fence *fence)
{
	return "host1x";
}

static const char *host1x_fence_get_timeline_name(struct dma_fence *fence)
{
	struct host1x_fence *f = to_host1x_fence(fence);

	return f->timeline_name;
}

static bool host1x_fence_enable_signaling(struct dma_fence *fence)
{
	struct host1x_fence *f = to_host1x_fence(fence);

	if (host1x_syncpt_is_expired(f->syncpt, f->threshold))
		return false;

	return true;
}

static bool host1x_fence_signaled(struct dma_fence *fence)
{
	struct host1x_fence *f = to_host1x_fence(fence);

	return host1x_syncpt_is_expired(f->syncpt, f->threshold);
}

static void host1x_fence_release(struct dma_fence *fence)
{
	struct host1x_fence *f = to_host1x_fence(fence);

	if (f->waiter)
		host1x_intr_put_ref(f->host, f->syncpt->id, f->waiter);

	kfree(f);
}

const struct dma_fence_ops host1x_fence_ops = {
	.get_driver_name = host1x_fence_get_driver_name,
	.get_timeline_name = host1x_fence_get_timeline_name,
	.enable_signaling = host1x_fence_enable_signaling,
	.signaled = host1x_fence_signaled,
	.wait = dma_fence_default_wait,
	.release = host1x_fence_release,
};

static void host1x_fence_wait_single(struct host1x_fence *f,
				     struct host1x *host,
				     struct host1x_channel *ch)
{
	if (host1x_syncpt_is_expired(f->syncpt, f->threshold))
		return;

	host1x_hw_channel_push_wait(host, ch, f->syncpt->id, f->threshold);
}

/**
 * host1x_fence_is_waitable() - Check if DMA fence can be waited by hardware
 * @fence: DMA fence
 *
 * Check is @fence is only backed by Host1x syncpoints and can therefore be
 * waited using only hardware.
 */
bool host1x_fence_is_waitable(struct dma_fence *fence)
{
	struct dma_fence_array *array;
	int i;

	array = to_dma_fence_array(fence);
	if (!array)
		return fence->ops == &host1x_fence_ops;

	for (i = 0; i < array->num_fences; ++i) {
		if (array->fences[i]->ops != &host1x_fence_ops)
			return false;
	}

	return true;
}

/**
 * host1x_fence_wait() - Insert waits for fence into channel
 * @fence: DMA fence
 * @host: Host1x
 * @ch: Host1x channel
 *
 * Inserts wait commands into Host1x channel fences in @fence.
 * in @fence. @fence must only consist of syncpoint-backed fences.
 *
 * Return: 0 on success, -errno otherwise.
 */
int host1x_fence_wait(struct dma_fence *fence, struct host1x *host,
		      struct host1x_channel *ch)
{
	struct dma_fence_array *array;
	int i = 0;

	if (!host1x_fence_is_waitable(fence))
		return -EINVAL;

	array = to_dma_fence_array(fence);
	if (!array) {
		host1x_fence_wait_single(to_host1x_fence(fence), host, ch);
		return 0;
	}

	for (i = 0; i < array->num_fences; ++i) {
		host1x_fence_wait_single(to_host1x_fence(array->fences[i]),
					 host, ch);
	}

	return 0;
}

struct dma_fence *host1x_fence_create(struct host1x *host,
				      struct host1x_syncpt *syncpt,
				      u32 threshold)
{
	struct host1x_waitlist *waiter;
	struct host1x_fence *f;
	int err;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;

	waiter = kzalloc(sizeof(*waiter), GFP_KERNEL);
	if (!waiter) {
		kfree(f);
		return NULL;
	}

	f->host = host;
	f->syncpt = syncpt;
	f->threshold = threshold;
	f->waiter = NULL;
	snprintf(f->timeline_name, ARRAY_SIZE(f->timeline_name),
		 "%d", syncpt->id);

	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &host1x_fence_ops, &f->lock,
		       host->fence_ctx_base + syncpt->id, threshold);

	err = host1x_intr_add_action(f->host, f->syncpt->id, f->threshold,
				     HOST1X_INTR_ACTION_SIGNAL_FENCE, f,
				     waiter, &f->waiter);
	if (err) {
		kfree(waiter);
		dma_fence_put((struct dma_fence *)f);
		return NULL;
	}

	return (struct dma_fence *)f;
}
EXPORT_SYMBOL(host1x_fence_create);
