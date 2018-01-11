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

#ifndef __HOST1X_FENCE_H
#define __HOST1X_FENCE_H

struct host1x;
struct host1x_channel;
struct dma_fence;

bool host1x_fence_is_waitable(struct dma_fence *fence);
int host1x_fence_wait(struct dma_fence *fence, struct host1x *host,
		      struct host1x_channel *ch);

#endif
