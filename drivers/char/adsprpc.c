/*
 * Copyright (c) 2012-2015,2017-2019,2020-2021 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/completion.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/hash.h>
#include <linux/msm_ion.h>
#include <soc/qcom/smd.h>
#include <soc/qcom/subsystem_notif.h>
#include <linux/msm_iommu_domains.h>
#include <linux/scatterlist.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/dma-contiguous.h>
#include <linux/iommu.h>
#include <linux/kref.h>
#include <linux/sort.h>
#include <asm/dma-iommu.h>
#include "adsprpc_compat.h"
#include "adsprpc_shared.h"
#include <linux/msm_audio_ion.h>
#include <soc/qcom/scm.h>

#define TZ_PIL_PROTECT_MEM_SUBSYS_ID 0x0C
#define TZ_PIL_CLEAR_PROTECT_MEM_SUBSYS_ID 0x0D
#define TZ_PIL_AUTH_QDSP6_PROC 1
#define ADSP_MMAP_HEAP_ADDR 4
#define AUDIO_ADSP_STREAM_ID	1
#define STREAM_ID	((uint64_t)AUDIO_ADSP_STREAM_ID << 32)
#define ADSP_MMAP_ADD_PAGES 0x1000

#define RPC_TIMEOUT	(5 * HZ)
#define BALIGN		32
#define NUM_CHANNELS    1 /*8 compute 2 cpz 1 modem*/
#define FASTRPC_CTX_MAGIC (0xbeeddeed)
#define FASTRPC_CTX_MAX (256)
#define FASTRPC_CTXID_MASK (0xFF0)

#define ADSP_DOMAIN_ID (0)
#define MDSP_DOMAIN_ID (1)
#define SDSP_DOMAIN_ID (2)
#define CDSP_DOMAIN_ID (3)

#define IS_CACHE_ALIGNED(x) (((x) & ((L1_CACHE_BYTES)-1)) == 0)

static inline uint64_t buf_page_start(uint64_t buf)
{
	uint64_t start = (uint64_t) buf & PAGE_MASK;
	return start;
}

static inline uint64_t buf_page_offset(uint64_t buf)
{
	uint64_t offset = (uint64_t) buf & (PAGE_SIZE - 1);
	return offset;
}

static inline uint64_t buf_num_pages(uint64_t buf, size_t len)
{
	uint64_t start = buf_page_start(buf) >> PAGE_SHIFT;
	uint64_t end = (((uint64_t) buf + len - 1) & PAGE_MASK) >> PAGE_SHIFT;
	uint64_t nPages = end - start + 1;
	return nPages;
}

static inline uint64_t buf_page_size(uint32_t size)
{
	uint64_t sz = (size + (PAGE_SIZE - 1)) & PAGE_MASK;

	return sz > PAGE_SIZE ? sz : PAGE_SIZE;
}

static inline void *uint64_to_ptr(uint64_t addr)
{
	void *ptr = (void *)((uintptr_t)addr);
	return ptr;
}

static inline uint64_t ptr_to_uint64(void *ptr)
{
	uint64_t addr = (uint64_t)((uintptr_t)ptr);
	return addr;
}

struct fastrpc_file;

struct fastrpc_buf {
	struct hlist_node hn;
	struct hlist_node hn_rem;
	struct fastrpc_file *fl;
	void *virt;
	dma_addr_t phys;
	size_t size;
	struct ion_handle *handle;
	struct ion_client *client;
	struct dma_attrs attrs;
	uintptr_t raddr;
	uint32_t flags;
	int remote;
};

struct fastrpc_ctx_lst;

struct overlap {
	uintptr_t start;
	uintptr_t end;
	int raix;
	uintptr_t mstart;
	uintptr_t mend;
	uintptr_t offset;
};

struct smq_invoke_ctx {
	struct hlist_node hn;
	struct completion work;
	int retval;
	int pid;
	int tgid;
	remote_arg_t *lpra;
	remote_arg64_t *rpra;
	remote_arg64_t *lrpra;		/* Local copy of rpra for put_args */
	int *fds;
	struct fastrpc_mmap **maps;
	struct fastrpc_buf *buf;
	struct fastrpc_buf *lbuf;
	size_t used;
	struct fastrpc_file *fl;
	uint32_t sc;
	struct overlap *overs;
	struct overlap **overps;
	unsigned int magic;
	uint64_t ctxid;
};

struct fastrpc_ctx_lst {
	struct hlist_head pending;
	struct hlist_head interrupted;
};

struct fastrpc_smmu {
	struct device *dev;
	struct dma_iommu_mapping *mapping;
	int cb;
	int enabled;
};

struct fastrpc_chan_ctx {
	smd_channel_t *chan;
	struct device *dev;
	struct completion work;
	struct fastrpc_smmu smmu;
	struct kref kref;
	struct notifier_block nb;
	int ssrcount;
	int prevssrcount;
};

struct fastrpc_apps {
	struct fastrpc_chan_ctx channel[NUM_CHANNELS];
	int nchans;
	struct cdev cdev;
	struct class *class;
	struct mutex smd_mutex;
	struct smq_phy_page range;
	struct hlist_head maps;
	dev_t dev_no;
	int compat;
	struct hlist_head drivers;
	struct ion_client *client;
	spinlock_t hlock;
	int32_t domain_id;
	struct device *adsp_mem_device;
	spinlock_t ctxlock;
	struct smq_invoke_ctx *ctxtable[FASTRPC_CTX_MAX];
};

struct fastrpc_mmap {
	struct hlist_node hn;
	struct fastrpc_file *fl;
	struct fastrpc_apps *apps;
	int fd;
	uint32_t flags;
	struct dma_buf *buf;
	struct sg_table *table;
	struct dma_buf_attachment *attach;
	uintptr_t phys;
	size_t size;
	uintptr_t va;
	size_t len;
	int refs;
        int uncached;
	uintptr_t raddr;
	struct ion_handle *handle;
	struct ion_client *client;
	bool is_filemap;
	/*flag to indicate map used in process init*/
};

struct fastrpc_channel_info {
	char *name;
	char *node;
	char *group;
	char *subsys;
	int channel;
};

struct fastrpc_file {
	struct hlist_node hn;
	spinlock_t hlock;
	struct hlist_head maps;
	struct hlist_head cached_bufs;
	struct hlist_head remote_bufs;
	struct fastrpc_ctx_lst clst;
	struct fastrpc_chan_ctx *chan;
	struct fastrpc_buf *init_mem;
	uint32_t mode;
	int tgid;
	int cid;
	int ssrcount;
	struct fastrpc_apps *apps;
	struct mutex map_mutex;
};

static struct fastrpc_apps gfa;

static const struct fastrpc_channel_info gcinfo[NUM_CHANNELS] = {
	{
		.name = "adsprpc-smd",
		.node = "qcom,msm-audio-ion",
		.subsys = "adsp",
		.channel = SMD_APPS_QDSP,
	},
};

static void fastrpc_buf_free(struct fastrpc_buf *buf, int cache)
{
	int err = 0;
	struct fastrpc_file *fl = buf == NULL ? NULL : buf->fl;
	if (!fl)
		return;
	if (cache) {
		spin_lock(&fl->hlock);
		hlist_add_head(&buf->hn, &fl->cached_bufs);
		spin_unlock(&fl->hlock);
		return;
	}

	if (buf->remote) {
		spin_lock(&fl->hlock);
		hlist_del_init(&buf->hn_rem);
		spin_unlock(&fl->hlock);
		buf->remote = 0;
		buf->raddr = 0;
	}

	if (!IS_ERR_OR_NULL(buf->virt)) {
		VERIFY(err, !msm_audio_ion_free(buf->client, buf->handle));
		if (err) {
			pr_err("error freeing audio ion buffer\n");
			goto bail;
		}
	}
bail:
	kfree(buf);
}

static void fastrpc_cached_buf_list_free(struct fastrpc_file *fl)
{
	struct fastrpc_buf *buf, *free;

	do {
		struct hlist_node *n;

		free = NULL;
		spin_lock(&fl->hlock);
		hlist_for_each_entry_safe(buf, n, &fl->cached_bufs, hn) {
			hlist_del_init(&buf->hn);
			free = buf;
			break;
		}
		spin_unlock(&fl->hlock);
		if (free)
			fastrpc_buf_free(free, 0);
	} while (free);
}

static void fastrpc_remote_buf_list_free(struct fastrpc_file *fl)
{
	struct fastrpc_buf *buf, *free;

	do {
		struct hlist_node *n;

		free = NULL;
		spin_lock(&fl->hlock);
		hlist_for_each_entry_safe(buf, n, &fl->remote_bufs, hn_rem) {
			free = buf;
			break;
		}
		spin_unlock(&fl->hlock);
		if (free)
			fastrpc_buf_free(free, 0);
	} while (free);
}

static void fastrpc_mmap_add(struct fastrpc_mmap *map)
{
	if (map->flags == ADSP_MMAP_HEAP_ADDR) {
		struct fastrpc_apps *me = &gfa;
		spin_lock(&me->hlock);
		hlist_add_head(&map->hn, &me->maps);
		spin_unlock(&me->hlock);
	} else {
		struct fastrpc_file *fl = map->fl;
		spin_lock(&fl->hlock);
		hlist_add_head(&map->hn, &fl->maps);
		spin_unlock(&fl->hlock);
	}
}

static int fastrpc_mmap_remove(struct fastrpc_file *fl, uintptr_t va,
			       size_t len, struct fastrpc_mmap **ppmap)
{
	struct fastrpc_mmap *match = NULL, *map = NULL;
	struct hlist_node *n = NULL;
	struct fastrpc_apps *me = &gfa;
	spin_lock(&me->hlock);
	hlist_for_each_entry_safe(map, n, &me->maps, hn) {
		if (map->refs == 1 && map->raddr == va &&
				map->raddr + map->len == va + len &&
				/*Remove map if not used in process initialization*/
				!map->is_filemap) {
			match = map;
			hlist_del_init(&map->hn);
			break;
		}
	}
	spin_unlock(&me->hlock);
	if (match) {
		*ppmap = match;
		return 0;
	}
	spin_lock(&fl->hlock);
	hlist_for_each_entry_safe(map, n, &fl->maps, hn) {
		if (map->refs == 1 && map->raddr == va &&
				map->raddr + map->len == va + len &&
				/* Remove map if not used in process initialization*/
				!map->is_filemap) {
			match = map;
			hlist_del_init(&map->hn);
			break;
		}
	}
	spin_unlock(&fl->hlock);
	if (match) {
		*ppmap = match;
		return 0;
	}
	return -ENOTTY;
}

static void fastrpc_mmap_free(struct fastrpc_mmap *map)
{
	struct fastrpc_apps *me = &gfa;
	struct fastrpc_file *fl = NULL;
	int cid = -1, err = 0;
	if (!map)
		return;

	cid = fl->cid;
	VERIFY(err, cid >= ADSP_DOMAIN_ID && cid < NUM_CHANNELS);
	if (err) {
		err = -ECHRNG;
		pr_err("adsprpc: ERROR:%s, Invalid channel id: %d, err:%d",
			__func__, cid, err);
		return;
	}
	if (map->flags == ADSP_MMAP_HEAP_ADDR) {
		spin_lock(&me->hlock);
		map->refs--;
		if (!map->refs)
			hlist_del_init(&map->hn);
		spin_unlock(&me->hlock);
	} else {
		fl = map->fl;
		spin_lock(&fl->hlock);
		map->refs--;
		if (!map->refs)
			hlist_del_init(&map->hn);
		spin_unlock(&fl->hlock);
	}
	if (map->refs)
		return;
	if (map->flags == ADSP_MMAP_HEAP_ADDR) {
		DEFINE_DMA_ATTRS(attrs);

		dma_set_attr(DMA_ATTR_SKIP_ZEROING, &attrs);
		dma_set_attr(DMA_ATTR_NO_KERNEL_MAPPING, &attrs);
		dma_free_attrs(me->adsp_mem_device, map->size,
				&(map->va), map->phys,	&attrs);
	} else {
		ion_unmap_iommu(map->client, map->handle,
				me->domain_id, 0);
		ion_unmap_kernel(map->client, map->handle);
		msm_audio_ion_client_destroy(map->client);
	}
	kfree(map);
}

static int fastrpc_buf_alloc(struct fastrpc_file *fl, size_t size,
				uint32_t rflags, int remote,
				struct fastrpc_buf **obuf)
{
	int err = 0;
	struct fastrpc_buf *buf = NULL, *fr = NULL;
	struct hlist_node *n = NULL;
	size_t len = 0;

	VERIFY(err, size > 0);
	if (err)
		goto bail;

	if (!remote) {
		/* find the smallest buffer that fits in the cache */
		spin_lock(&fl->hlock);
		hlist_for_each_entry_safe(buf, n, &fl->cached_bufs, hn) {
			if (buf->size >= size && (!fr || fr->size > buf->size))
				fr = buf;
		}
		if (fr)
			hlist_del_init(&fr->hn);
		spin_unlock(&fl->hlock);
		if (fr) {
			*obuf = fr;
			return 0;
		}
	}
	VERIFY(err, NULL != (buf = kzalloc(sizeof(*buf), GFP_KERNEL)));
	if (err)
		goto bail;
	INIT_HLIST_NODE(&buf->hn);

	buf->handle = NULL;
	buf->client = NULL;
	buf->fl = fl;
	buf->virt = NULL;
	buf->phys = 0;
	buf->size = size;
	buf->flags = rflags;
	buf->raddr = 0;
	buf->remote = 0;
	VERIFY(err, !msm_audio_ion_alloc(DEVICE_NAME, &(buf->client),
								&(buf->handle), buf->size,
								&(buf->phys), &len, &(buf->virt)));
	if (err) {
		pr_err("%s: Error allocating audio ion buf size: %zd\n",
					__func__, buf->size);
		goto bail;
	}

	if (len != buf->size)
		pr_info("allocating memory from audio ion size is %zd\n",
				len);

	buf->size = len;

	if (remote) {
		INIT_HLIST_NODE(&buf->hn_rem);
		spin_lock(&fl->hlock);
		hlist_add_head(&buf->hn_rem, &fl->remote_bufs);
		spin_unlock(&fl->hlock);
		buf->remote = remote;
	}
	*obuf = buf;
 bail:
	if (err && buf)
		fastrpc_buf_free(buf, 0);
	return err;
}

static int context_restore_interrupted(struct fastrpc_file *fl,
				       struct fastrpc_ioctl_invoke_fd *invokefd,
				       struct smq_invoke_ctx **po)
{
	int err = 0;
	struct smq_invoke_ctx *ctx = NULL, *ictx = NULL;
	struct hlist_node *n = NULL;
	struct fastrpc_ioctl_invoke *invoke = &invokefd->inv;

	spin_lock(&fl->hlock);
	hlist_for_each_entry_safe(ictx, n, &fl->clst.interrupted, hn) {
		if (ictx->pid == current->pid) {
			if (invoke->sc != ictx->sc || ictx->fl != fl)
				err = -1;
			else {
				ctx = ictx;
				hlist_del_init(&ctx->hn);
				hlist_add_head(&ctx->hn, &fl->clst.pending);
			}
			break;
		}
	}
	spin_unlock(&fl->hlock);
	if (ctx)
		*po = ctx;
	return err;
}

#define CMP(aa, bb) ((aa) == (bb) ? 0 : (aa) < (bb) ? -1 : 1)
static int overlap_ptr_cmp(const void *a, const void *b)
{
	struct overlap *pa = *((struct overlap **)a);
	struct overlap *pb = *((struct overlap **)b);
	/* sort with lowest starting buffer first */
	int st = CMP(pa->start, pb->start);
	/* sort with highest ending buffer first */
	int ed = CMP(pb->end, pa->end);
	return st == 0 ? ed : st;
}

static int context_build_overlap(struct smq_invoke_ctx *ctx)
{
	int i, err = 0;
	remote_arg_t *lpra = ctx->lpra;
	int inbufs = REMOTE_SCALARS_INBUFS(ctx->sc);
	int outbufs = REMOTE_SCALARS_OUTBUFS(ctx->sc);
	int nbufs = inbufs + outbufs;
	struct overlap max;

	for (i = 0; i < nbufs; ++i) {
		ctx->overs[i].start = (uintptr_t)lpra[i].buf.pv;
		ctx->overs[i].end = ctx->overs[i].start + lpra[i].buf.len;
		if (lpra[i].buf.len) {
			VERIFY(err, ctx->overs[i].end > ctx->overs[i].start);
			if (err)
				goto bail;
		}
		ctx->overs[i].raix = i;
		ctx->overps[i] = &ctx->overs[i];
	}
	sort(ctx->overps, nbufs, sizeof(*ctx->overps), overlap_ptr_cmp, NULL);
	max.start = 0;
	max.end = 0;
	for (i = 0; i < nbufs; ++i) {
		if (ctx->overps[i]->start < max.end) {
			ctx->overps[i]->mstart = max.end;
			ctx->overps[i]->mend = ctx->overps[i]->end;
			ctx->overps[i]->offset = max.end -
				ctx->overps[i]->start;
			if (ctx->overps[i]->end > max.end) {
				max.end = ctx->overps[i]->end;
			} else {
				ctx->overps[i]->mend = 0;
				ctx->overps[i]->mstart = 0;
			}
		} else  {
			ctx->overps[i]->mend = ctx->overps[i]->end;
			ctx->overps[i]->mstart = ctx->overps[i]->start;
			ctx->overps[i]->offset = 0;
			max = *ctx->overps[i];
		}
	}
bail:
	return err;
}

#define K_COPY_FROM_USER(err, kernel, dst, src, size) \
	do {\
		if (!(kernel))\
			VERIFY(err, 0 == copy_from_user((dst),\
			(void const __user *)(src),\
							(size)));\
		else\
			memmove((dst), (src), (size));\
	} while (0)

#define K_COPY_TO_USER(err, kernel, dst, src, size) \
	do {\
		if (!(kernel))\
			VERIFY(err, 0 == copy_to_user((void __user *)(dst), \
						(src), (size)));\
		else\
			memmove((dst), (src), (size));\
	} while (0)


static void context_free(struct smq_invoke_ctx *ctx);

static int context_alloc(struct fastrpc_file *fl, uint32_t kernel,
			 struct fastrpc_ioctl_invoke_fd *invokefd,
			 struct smq_invoke_ctx **po)
{
	int err = 0, bufs, ii, size = 0;
	struct fastrpc_apps *me = &gfa;
	struct smq_invoke_ctx *ctx = NULL;
	struct fastrpc_ctx_lst *clst = &fl->clst;
	struct fastrpc_ioctl_invoke *invoke = &invokefd->inv;

	bufs = REMOTE_SCALARS_INBUFS(invoke->sc) +
			REMOTE_SCALARS_OUTBUFS(invoke->sc);
	size = bufs * sizeof(*ctx->lpra) + bufs * sizeof(*ctx->maps) +
		sizeof(*ctx->fds) * (bufs) +
		sizeof(*ctx->overs) * (bufs) +
		sizeof(*ctx->overps) * (bufs);

	VERIFY(err, NULL != (ctx = kzalloc(sizeof(*ctx) + size, GFP_KERNEL)));
	if (err)
		goto bail;

	INIT_HLIST_NODE(&ctx->hn);
	hlist_add_fake(&ctx->hn);
	ctx->fl = fl;
	ctx->maps = (struct fastrpc_mmap **)(&ctx[1]);
	ctx->lpra = (remote_arg_t *)(&ctx->maps[bufs]);
	ctx->fds = (int *)(&ctx->lpra[bufs]);
	ctx->overs = (struct overlap *)(&ctx->fds[bufs]);
	ctx->overps = (struct overlap **)(&ctx->overs[bufs]);

	K_COPY_FROM_USER(err, kernel, (void *)ctx->lpra, invoke->pra,
					bufs * sizeof(*ctx->lpra));
	if (err)
		goto bail;

	if (invokefd->fds) {
		K_COPY_FROM_USER(err, kernel, ctx->fds, invokefd->fds,
						bufs * sizeof(*ctx->fds));
		if (err)
			goto bail;
	}
	ctx->sc = invoke->sc;
	if (bufs) {
		VERIFY(err, 0 == context_build_overlap(ctx));
		if (err)
			goto bail;
	}
	ctx->retval = -1;
	ctx->pid = current->pid;
	ctx->tgid = current->tgid;
	init_completion(&ctx->work);
	ctx->magic = FASTRPC_CTX_MAGIC;

	spin_lock(&fl->hlock);
	hlist_add_head(&ctx->hn, &clst->pending);
	spin_unlock(&fl->hlock);

	spin_lock(&me->ctxlock);
	for (ii = 0; ii < FASTRPC_CTX_MAX; ii++) {
		if (!me->ctxtable[ii]) {
			me->ctxtable[ii] = ctx;
			ctx->ctxid = (ptr_to_uint64(ctx) & ~0xFFF)|(ii << 4);
			break;
		}
	}
	spin_unlock(&me->ctxlock);
	VERIFY(err, ii < FASTRPC_CTX_MAX);
	if (err) {
		pr_err("adsprpc: out of context memory\n");
		goto bail;
	}

	*po = ctx;
bail:
	if (ctx && err)
		context_free(ctx);
	return err;
}

static void context_save_interrupted(struct smq_invoke_ctx *ctx)
{
	struct fastrpc_ctx_lst *clst = &ctx->fl->clst;

	spin_lock(&ctx->fl->hlock);
	hlist_del_init(&ctx->hn);
	hlist_add_head(&ctx->hn, &clst->interrupted);
	spin_unlock(&ctx->fl->hlock);
	/* free the cache on power collapse */
	fastrpc_cached_buf_list_free(ctx->fl);
}

static void context_free(struct smq_invoke_ctx *ctx)
{
	int i;
	struct fastrpc_apps *me = &gfa;
	int nbufs = REMOTE_SCALARS_INBUFS(ctx->sc) +
		    REMOTE_SCALARS_OUTBUFS(ctx->sc);
	spin_lock(&ctx->fl->hlock);
	hlist_del_init(&ctx->hn);
	spin_unlock(&ctx->fl->hlock);
	for (i = 0; i < nbufs; ++i)
		fastrpc_mmap_free(ctx->maps[i]);
	fastrpc_buf_free(ctx->buf, 1);
	fastrpc_buf_free(ctx->lbuf, 1);
	ctx->magic = 0;
	ctx->ctxid = 0;

	spin_lock(&me->ctxlock);
	for (i = 0; i < FASTRPC_CTX_MAX; i++) {
		if (me->ctxtable[i] == ctx) {
			me->ctxtable[i] = NULL;
			break;
		}
	}
	spin_unlock(&me->ctxlock);

	kfree(ctx);
}

static void context_notify_user(struct smq_invoke_ctx *ctx, int retval)
{
	ctx->retval = retval;
	complete(&ctx->work);
}

static void fastrpc_notify_users(struct fastrpc_file *me)
{
	struct smq_invoke_ctx *ictx;
	struct hlist_node *n;

	spin_lock(&me->hlock);
	hlist_for_each_entry_safe(ictx, n, &me->clst.pending, hn) {
		complete(&ictx->work);
	}
	hlist_for_each_entry_safe(ictx, n, &me->clst.interrupted, hn) {
		complete(&ictx->work);
	}
	spin_unlock(&me->hlock);

}

static void fastrpc_notify_drivers(struct fastrpc_apps *me, int cid)
{
	struct fastrpc_file *fl;
	struct hlist_node *n;

	spin_lock(&me->hlock);
	hlist_for_each_entry_safe(fl, n, &me->drivers, hn) {
		if (fl->cid == cid)
			fastrpc_notify_users(fl);
	}
	spin_unlock(&me->hlock);

}

static void context_list_ctor(struct fastrpc_ctx_lst *me)
{
	INIT_HLIST_HEAD(&me->interrupted);
	INIT_HLIST_HEAD(&me->pending);
}

static void fastrpc_context_list_dtor(struct fastrpc_file *fl)
{
	struct fastrpc_ctx_lst *clst = &fl->clst;
	struct smq_invoke_ctx *ictx = NULL, *ctxfree = NULL;
	struct hlist_node *n;

	do {
		ctxfree = NULL;
		spin_lock(&fl->hlock);
		hlist_for_each_entry_safe(ictx, n, &clst->interrupted, hn) {
			hlist_del_init(&ictx->hn);
			ctxfree = ictx;
			break;
		}
		spin_unlock(&fl->hlock);
		if (ctxfree)
			context_free(ctxfree);
	} while (ctxfree);
	do {
		ctxfree = NULL;
		spin_lock(&fl->hlock);
		hlist_for_each_entry_safe(ictx, n, &clst->pending, hn) {
			hlist_del_init(&ictx->hn);
			ctxfree = ictx;
			break;
		}
		spin_unlock(&fl->hlock);
		if (ctxfree)
			context_free(ctxfree);
	} while (ctxfree);
}

static int fastrpc_file_free(struct fastrpc_file *fl);

static void fastrpc_file_list_dtor(struct fastrpc_apps *me)
{
	struct fastrpc_file *fl, *free;
	struct hlist_node *n;

	do {
		free = NULL;
		spin_lock(&me->hlock);
		hlist_for_each_entry_safe(fl, n, &me->drivers, hn) {
			hlist_del_init(&fl->hn);
			free = fl;
			break;
		}
		spin_unlock(&me->hlock);
		if (free)
			fastrpc_file_free(free);
	} while (free);
}

static int fastrpc_mmap_create(struct fastrpc_file *fl, int fd, uintptr_t va,
		size_t len, int mflags, struct fastrpc_mmap **ppmap);

static int get_args(uint32_t kernel, struct smq_invoke_ctx *ctx)
{
	remote_arg64_t *rpra, *lrpra;
	remote_arg_t *lpra = ctx->lpra;
	struct smq_invoke_buf *list;
	struct smq_phy_page *pages, *ipage;
	uint32_t sc = ctx->sc;
	int inbufs = REMOTE_SCALARS_INBUFS(sc);
	int outbufs = REMOTE_SCALARS_OUTBUFS(sc);
	int bufs = inbufs + outbufs;
	uintptr_t args;
	size_t rlen = 0, copylen = 0, metalen = 0, lrpralen = 0;
	int i, inh, oix;
	int err = 0;
	int mflags = 0;

	/* calculate size of the metadata */
	rpra = NULL;
	list = smq_invoke_buf_start(rpra, sc);
	pages = smq_phy_page_start(sc, list);
	ipage = pages;

	for (i = 0; i < bufs; ++i) {
		uintptr_t buf = (uintptr_t)lpra[i].buf.pv;
		size_t len = lpra[i].buf.len;
		if (ctx->fds[i] > 0) {
			VERIFY(err, !fastrpc_mmap_create(ctx->fl,
						ctx->fds[i], buf, len, mflags,
						&ctx->maps[i]));
			if (err) {
				pr_err("error mapping the buffer\n");
				goto bail;
			}
		}
		ipage += 1;
	}
	metalen = copylen = (size_t)&ipage[0];

	/* allocate new local rpra buffer */
	lrpralen = (size_t)&list[0];
	if (lrpralen) {
		err = fastrpc_buf_alloc(ctx->fl, lrpralen,
					0, 0, &ctx->lbuf);
		if (err)
			goto bail;
	}
	if (ctx->lbuf->virt)
		memset(ctx->lbuf->virt, 0, lrpralen);

	lrpra = ctx->lbuf->virt;
	ctx->lrpra = lrpra;

	/* calculate len required for copying */
	for (oix = 0; oix < inbufs + outbufs; ++oix) {
		int i = ctx->overps[oix]->raix;
		uintptr_t mstart, mend;
		size_t len = lpra[i].buf.len;

		if (!len)
			continue;
		if (ctx->maps[i])
			continue;
		if (ctx->overps[oix]->offset == 0)
			copylen = ALIGN(copylen, BALIGN);
		mstart = ctx->overps[oix]->mstart;
		mend = ctx->overps[oix]->mend;
		VERIFY(err, (mend - mstart) <= LONG_MAX);
		if (err)
			goto bail;
		copylen += mend - mstart;
		VERIFY(err, copylen >= 0);
		if (err)
			goto bail;
	}
	ctx->used = copylen;

	/* allocate new buffer */
	if (copylen) {
		err = fastrpc_buf_alloc(ctx->fl, copylen,
					0, 0, &ctx->buf);
		if (err)
			goto bail;
	}
	/* copy metadata */
	rpra = ctx->buf->virt;
	ctx->rpra = rpra;
	list = smq_invoke_buf_start(rpra, sc);
	pages = smq_phy_page_start(sc, list);
	ipage = pages;
	args = (uintptr_t)ctx->buf->virt + metalen;
	for (i = 0; i < bufs; ++i) {
		size_t len = lpra[i].buf.len;
		list[i].num = 0;
		list[i].pgidx = 0;
		if (!len)
			continue;
		list[i].num = 1;
		list[i].pgidx = ipage - pages;
		ipage++;
	}
	/* map ion buffers */
	for (i = 0; rpra && lrpra && i < inbufs + outbufs; ++i) {
		struct fastrpc_mmap *map = ctx->maps[i];
		uint64_t buf = ptr_to_uint64(lpra[i].buf.pv);
		size_t len = lpra[i].buf.len;

		rpra[i].buf.pv = lrpra[i].buf.pv = 0;
		rpra[i].buf.len = lrpra[i].buf.len = len;
		if (!len)
			continue;
		if (map) {
			struct vm_area_struct *vma;
			uintptr_t offset;
			uint64_t num = buf_num_pages(buf, len);
			int idx = list[i].pgidx;

			VERIFY(err, NULL != (vma = find_vma(current->mm,
								map->va)));
			if (err)
				goto bail;
			offset = buf_page_start(buf) - vma->vm_start;
			pages[idx].addr = map->phys + offset;
			if (msm_audio_ion_is_smmu_available())
				pages[idx].addr |= STREAM_ID;
			pages[idx].size = num << PAGE_SHIFT;
		}
		rpra[i].buf.pv = lrpra[i].buf.pv = buf;
	}
	/* copy non ion buffers */
	rlen = copylen - metalen;
	for (oix = 0; rpra && lrpra && oix < inbufs + outbufs; ++oix) {
		int i = ctx->overps[oix]->raix;
		struct fastrpc_mmap *map = ctx->maps[i];
		size_t mlen = ctx->overps[oix]->mend - ctx->overps[oix]->mstart;
		uint64_t buf;
		size_t len = lpra[i].buf.len;
		if (!len)
			continue;
		if (map)
			continue;
		if (ctx->overps[oix]->offset == 0) {
			rlen -= ALIGN(args, BALIGN) - args;
			args = ALIGN(args, BALIGN);
		}
		VERIFY(err, rlen >= mlen);
		if (err)
			goto bail;
		rpra[i].buf.pv = lrpra[i].buf.pv =
			 (args - ctx->overps[oix]->offset);
		pages[list[i].pgidx].addr = ctx->buf->phys -
					    ctx->overps[oix]->offset +
					    (copylen - rlen);
		if (msm_audio_ion_is_smmu_available())
			pages[list[i].pgidx].addr |= STREAM_ID;
		pages[list[i].pgidx].addr =
			buf_page_start(pages[list[i].pgidx].addr);
		buf = rpra[i].buf.pv;
		pages[list[i].pgidx].size = buf_num_pages(buf, len) * PAGE_SIZE;
		if (i < inbufs) {
			K_COPY_FROM_USER(err, kernel, uint64_to_ptr(buf),
					lpra[i].buf.pv, len);
			if (err)
				goto bail;
		}
		args = args + mlen;
		rlen -= mlen;
	}

	for (oix = 0; oix < inbufs + outbufs; ++oix) {
		int i = ctx->overps[oix]->raix;
		struct fastrpc_mmap *map = ctx->maps[i];
		if (map && map->uncached)
			continue;
		if (rpra[i].buf.len && ctx->overps[oix]->mstart) {
			if (map && map->handle)
				msm_ion_do_cache_op(ctx->fl->apps->client,
					map->handle,
					uint64_to_ptr(rpra[i].buf.pv),
					rpra[i].buf.len,
					ION_IOC_CLEAN_INV_CACHES);
			else
				dmac_flush_range(uint64_to_ptr(rpra[i].buf.pv),
					uint64_to_ptr(rpra[i].buf.pv
						+ rpra[i].buf.len));
		}

	}
	inh = inbufs + outbufs;
	for (i = 0; i < REMOTE_SCALARS_INHANDLES(sc); i++) {
		rpra[inh + i].buf.pv = lrpra[inh + i].buf.pv =
			ptr_to_uint64(ctx->lpra[inh + i].buf.pv);
		rpra[inh + i].buf.len = lrpra[inh + i].buf.len =
			ctx->lpra[inh + i].buf.len;
		rpra[inh + i].h = lrpra[inh + i].h =
			ctx->lpra[inh + i].h;
	}
 bail:
	return err;
}

static int put_args(uint32_t kernel, struct smq_invoke_ctx *ctx,
		    remote_arg_t *upra)
{
	uint32_t sc = ctx->sc;
	remote_arg64_t *rpra = ctx->lrpra;
	int i, inbufs, outbufs, outh, num;
	int err = 0;

	inbufs = REMOTE_SCALARS_INBUFS(sc);
	outbufs = REMOTE_SCALARS_OUTBUFS(sc);
	for (i = inbufs; i < inbufs + outbufs; ++i) {
		if (!ctx->maps[i]) {
			K_COPY_TO_USER(err, kernel,
				ctx->lpra[i].buf.pv,
				uint64_to_ptr(rpra[i].buf.pv),
				rpra[i].buf.len);
			if (err)
				goto bail;
		} else {
			fastrpc_mmap_free(ctx->maps[i]);
			ctx->maps[i] = NULL;
		}
	}
	num = REMOTE_SCALARS_OUTHANDLES(sc);
	if (num) {
		outh = inbufs + outbufs + REMOTE_SCALARS_INHANDLES(sc);
		K_COPY_TO_USER(err, kernel, &upra[outh], &ctx->lpra[outh],
				num * sizeof(*ctx->lpra));
		if (err)
			goto bail;
	}
 bail:
	return err;
}

static void inv_args_pre(struct smq_invoke_ctx *ctx)
{
	int i, inbufs, outbufs;
	uint32_t sc = ctx->sc;
	remote_arg64_t *rpra = ctx->rpra;
	uintptr_t end;

	inbufs = REMOTE_SCALARS_INBUFS(sc);
	outbufs = REMOTE_SCALARS_OUTBUFS(sc);
	for (i = inbufs; i < inbufs + outbufs; ++i) {
		struct fastrpc_mmap *map = ctx->maps[i];

		if (!rpra[i].buf.len)
			continue;
		if (buf_page_start(ptr_to_uint64((void *)rpra)) ==
				buf_page_start(rpra[i].buf.pv))
			continue;
		if (!IS_CACHE_ALIGNED((uintptr_t)
				uint64_to_ptr(rpra[i].buf.pv))) {
			if (map && map->handle)
				msm_ion_do_cache_op(ctx->fl->apps->client,
					map->handle,
					uint64_to_ptr(rpra[i].buf.pv),
					sizeof(uintptr_t),
					ION_IOC_CLEAN_INV_CACHES);
			else
				dmac_flush_range(
					uint64_to_ptr(rpra[i].buf.pv), (char *)
					uint64_to_ptr(rpra[i].buf.pv + 1));
		}
		end = (uintptr_t)uint64_to_ptr(rpra[i].buf.pv +
							rpra[i].buf.len);
		if (!IS_CACHE_ALIGNED(end)) {
			if (map && map->handle)
				msm_ion_do_cache_op(ctx->fl->apps->client,
						map->handle,
						uint64_to_ptr(end),
						sizeof(uintptr_t),
						ION_IOC_CLEAN_INV_CACHES);
			else
				dmac_flush_range((char *)end,
					(char *)end + 1);
		}
	}
}

static void inv_args(struct smq_invoke_ctx* ctx)
{
	int i, inbufs, outbufs;
	uint32_t sc = ctx->sc;
	remote_arg64_t *rpra = ctx->lrpra;
	int inv = 0;

	inbufs = REMOTE_SCALARS_INBUFS(sc);
	outbufs = REMOTE_SCALARS_OUTBUFS(sc);
	for (i = inbufs; i < inbufs + outbufs; ++i) {
		struct fastrpc_mmap *map = ctx->maps[i];

		if (!rpra[i].buf.len)
			continue;
		if (buf_page_start(ptr_to_uint64((void *)rpra)) ==
				buf_page_start(rpra[i].buf.pv)) {
			inv = 1;
			continue;
		}
		if (map && map->handle)
			msm_ion_do_cache_op(map->client, map->handle,
				uint64_to_ptr(rpra[i].buf.pv), rpra[i].buf.len,
				ION_IOC_INV_CACHES);
		else
			dmac_inv_range((char *)uint64_to_ptr(rpra[i].buf.pv),
				(char *)uint64_to_ptr(rpra[i].buf.pv
						 + rpra[i].buf.len));
	}

}

static int fastrpc_invoke_send(struct smq_invoke_ctx *ctx,
			       uint32_t kernel, uint32_t handle)
{
	struct smq_msg msg = {0};
	struct fastrpc_file *fl = ctx->fl;
	int err = 0, len, cid = -1;

	cid = fl->cid;
	VERIFY(err, cid >= ADSP_DOMAIN_ID && cid < NUM_CHANNELS);
	if (err) {
		err = -ECHRNG;
		goto bail;
	}

	msg.pid = current->tgid;
	msg.tid = current->pid;
	if (kernel)
		msg.pid = 0;
	msg.invoke.header.ctx = ctx->ctxid;
	msg.invoke.header.handle = handle;
	msg.invoke.header.sc = ctx->sc;
	msg.invoke.page.addr = ctx->buf ? ctx->buf->phys : 0;
	msg.invoke.page.size = buf_page_size(ctx->used);
	if (msm_audio_ion_is_smmu_available()
		&& msg.invoke.page.addr != 0)
		msg.invoke.page.addr |= STREAM_ID;
	spin_lock(&fl->apps->hlock);
	len = smd_write(fl->chan->chan, &msg, sizeof(msg));
	spin_unlock(&fl->apps->hlock);
	VERIFY(err, len == sizeof(msg));
bail:
	return err;
}

static void fastrpc_deinit(void)
{
	struct fastrpc_apps *me = &gfa;
	int i;

	for (i = 0; i < NUM_CHANNELS; i++) {
		struct fastrpc_chan_ctx *chan = &me->channel[i];
		if (chan->chan) {
			(void)smd_close(chan->chan);
			chan->chan = 0;
		}
		if (chan->smmu.dev)
			chan->smmu.dev = 0;
		if (chan->smmu.mapping)
			chan->smmu.mapping = 0;
	}
}

static void fastrpc_read_handler(int cid)
{
	struct fastrpc_apps *me = &gfa;
	struct smq_invoke_rsp rsp = {0};
	int ret = 0, err = 0;
	uint32_t index;

	do {
		ret = smd_read_from_cb(me->channel[cid].chan, &rsp,
					sizeof(rsp));
		if (ret != sizeof(rsp))
			break;

		index = (uint32_t)((rsp.ctx & FASTRPC_CTXID_MASK) >> 4);
		VERIFY(err, index < FASTRPC_CTX_MAX);
		if (err)
			goto bail;

		VERIFY(err, !IS_ERR_OR_NULL(me->ctxtable[index]));
		if (err)
			goto bail;

		VERIFY(err, ((me->ctxtable[index]->ctxid == (rsp.ctx)) &&
			me->ctxtable[index]->magic == FASTRPC_CTX_MAGIC));
		if (err)
			goto bail;

		context_notify_user(me->ctxtable[index], rsp.retval);
	} while (ret == sizeof(rsp));

bail:
	if (err)
			pr_err("adsprpc: invalid response or context\n");
}

static void smd_event_handler(void *priv, unsigned event)
{
	struct fastrpc_apps *me = &gfa;
	int cid = (int)(uintptr_t)priv;

	switch (event) {
	case SMD_EVENT_OPEN:
		complete(&me->channel[cid].work);
		break;
	case SMD_EVENT_CLOSE:
		fastrpc_notify_drivers(me, cid);
		break;
	case SMD_EVENT_DATA:
		fastrpc_read_handler(cid);
		break;
	}
}

static void fastrpc_init(struct fastrpc_apps *me)
{
	int i;

	INIT_HLIST_HEAD(&me->drivers);
	INIT_HLIST_HEAD(&me->maps);
	spin_lock_init(&me->hlock);
	spin_lock_init(&me->ctxlock);
	mutex_init(&me->smd_mutex);
	for (i = 0; i < NUM_CHANNELS; i++)
		init_completion(&me->channel[i].work);
}

static int fastrpc_release_current_dsp_process(struct fastrpc_file *fl);

static int fastrpc_internal_invoke(struct fastrpc_file *fl, uint32_t mode,
				   uint32_t kernel,
				   struct fastrpc_ioctl_invoke_fd *invokefd)
{
	struct smq_invoke_ctx *ctx = NULL;
	struct fastrpc_ioctl_invoke *invoke = &invokefd->inv;
	int err = 0, cid = -1, interrupted = 0;

	cid = fl->cid;
	VERIFY(err, cid >= ADSP_DOMAIN_ID && cid < NUM_CHANNELS);
	if (err) {
		err = -ECHRNG;
		goto bail;
	}

	if (!kernel) {
		VERIFY(err, 0 == context_restore_interrupted(fl, invokefd,
								&ctx));
		if (err)
			goto bail;
		if (ctx)
			goto wait;
	}

	VERIFY(err, 0 == context_alloc(fl, kernel, invokefd, &ctx));
	if (err)
		goto bail;

	if (REMOTE_SCALARS_LENGTH(ctx->sc)) {
		VERIFY(err, 0 == get_args(kernel, ctx));
		if (err)
			goto bail;
	}

	inv_args_pre(ctx);
	if (FASTRPC_MODE_SERIAL == mode)
		inv_args(ctx);
	VERIFY(err, 0 == fastrpc_invoke_send(ctx, kernel, invoke->handle));
	if (err)
		goto bail;
	if (FASTRPC_MODE_PARALLEL == mode)
		inv_args(ctx);
 wait:
	if (kernel)
		wait_for_completion(&ctx->work);
	else {
		interrupted = wait_for_completion_interruptible(&ctx->work);
		VERIFY(err, 0 == (err = interrupted));
		if (err)
			goto bail;
	}
	VERIFY(err, 0 == (err = ctx->retval));
	if (err)
		goto bail;
	VERIFY(err, 0 == put_args(kernel, ctx, invoke->pra));
	if (err)
		goto bail;
 bail:
	if (ctx && interrupted == -ERESTARTSYS)
		context_save_interrupted(ctx);
	else if (ctx)
		context_free(ctx);
	if (fl->ssrcount != fl->apps->channel[cid].ssrcount)
		err = ECONNRESET;
	return err;
}

static int fastrpc_init_process(struct fastrpc_file *fl,
				struct fastrpc_ioctl_init *init)
{
	int err = 0;
	struct fastrpc_ioctl_invoke_fd ioctl;
	struct smq_phy_page pages[1];
	struct fastrpc_mmap *file = NULL, *mem = NULL;
	struct fastrpc_buf *imem = NULL;

	if (init->flags == FASTRPC_INIT_ATTACH) {
		remote_arg_t ra[1];
		int tgid = current->tgid;

		ra[0].buf.pv = (void *)&tgid;
		ra[0].buf.len = sizeof(tgid);
		ioctl.inv.handle = 1;
		ioctl.inv.sc = REMOTE_SCALARS_MAKE(0, 1, 0);
		ioctl.inv.pra = ra;
		ioctl.fds = NULL;
		VERIFY(err, !(err = fastrpc_internal_invoke(fl,
			FASTRPC_MODE_PARALLEL, 1, &ioctl)));
		if (err)
			goto bail;
	} else if (init->flags == FASTRPC_INIT_CREATE) {
		remote_arg_t ra[4];
		int fds[4];
		int mflags = 0;
		int memlen;
		struct {
			int pgid;
			int namelen;
			int filelen;
			int pageslen;
		} inbuf;
		inbuf.pgid = current->tgid;
		inbuf.namelen = strlen(current->comm);
		inbuf.filelen = init->filelen;
		VERIFY(err, !fastrpc_mmap_create(fl, init->filefd, init->file,
						 init->filelen, mflags, &file));
		if (file)
			file->is_filemap = true;
		if (err)
			goto bail;
		inbuf.pageslen = 1;

		VERIFY(err, !init->mem);
		if (err) {
			err = -EINVAL;
			pr_err("adsprpc: %s: %s: ERROR: donated memory allocated in userspace\n",
				current->comm, __func__);
			goto bail;
		}
		memlen = ALIGN(max(1024*1024*3, (int)init->filelen * 4),
						1024*1024);

		err = fastrpc_buf_alloc(fl, memlen, 0, 0, &imem);
		if (err)
			goto bail;
		fl->init_mem = imem;

		inbuf.pageslen = 1;
		ra[0].buf.pv = (void *)&inbuf;
		ra[0].buf.len = sizeof(inbuf);
		fds[0] = 0;

		ra[1].buf.pv = (void *)current->comm;
		ra[1].buf.len = inbuf.namelen;
		fds[1] = 0;

		ra[2].buf.pv = (void *)init->file;
		ra[2].buf.len = inbuf.filelen;
		fds[2] = init->filefd;

		pages[0].addr = imem->phys;
		if (msm_audio_ion_is_smmu_available())
			pages[0].addr |= STREAM_ID;
		pages[0].size = imem->size;
		ra[3].buf.pv = (void *)pages;
		ra[3].buf.len = 1 * sizeof(*pages);
		fds[3] = 0;

		ioctl.inv.handle = 1;
		ioctl.inv.sc = REMOTE_SCALARS_MAKE(6, 4, 0);
		ioctl.inv.pra = ra;
		ioctl.fds = fds;
		VERIFY(err, !(err = fastrpc_internal_invoke(fl,
			FASTRPC_MODE_PARALLEL, 1, &ioctl)));
		if (err)
			goto bail;
	} else {
		err = -ENOTTY;
	}
bail:
	if (mem && err)
		fastrpc_mmap_free(mem);
	if (file)
		fastrpc_mmap_free(file);
	return err;
}

static int fastrpc_release_current_dsp_process(struct fastrpc_file *fl)
{
	int err = 0;
	struct fastrpc_ioctl_invoke_fd ioctl;
	remote_arg_t ra[1];
	int tgid = 0;

	tgid = fl->tgid;
	ra[0].buf.pv = (void *)&tgid;
	ra[0].buf.len = sizeof(tgid);
	ioctl.inv.handle = 1;
	ioctl.inv.sc = REMOTE_SCALARS_MAKE(1, 1, 0);
	ioctl.inv.pra = ra;
	ioctl.fds = NULL;
	VERIFY(err, 0 == (err = fastrpc_internal_invoke(fl,
		FASTRPC_MODE_PARALLEL, 1, &ioctl)));
	return err;
}

static int fastrpc_mmap_on_dsp(struct fastrpc_file *fl, uint32_t flags,
					uintptr_t va, uint64_t phys,
					size_t size, uintptr_t *raddr)
{
	struct fastrpc_ioctl_invoke_fd ioctl;
	struct smq_phy_page page;
	int num = 1;
	remote_arg_t ra[3];
	int err = 0;
	struct {
		int pid;
		uint32_t flags;
		uintptr_t vaddrin;
		int num;
	} inargs;

	struct {
		uintptr_t vaddrout;
	} routargs;

	inargs.pid = current->tgid;
	inargs.vaddrin = (uintptr_t)va;
	inargs.flags = flags;
	inargs.num = fl->apps->compat ? num * sizeof(page) : num;
	ra[0].buf.pv = (void *)&inargs;
	ra[0].buf.len = sizeof(inargs);
	page.addr = phys;
	if (msm_audio_ion_is_smmu_available() && flags != ADSP_MMAP_HEAP_ADDR)
		page.addr |= STREAM_ID;
	page.size = size;
	ra[1].buf.pv = (void *)&page;
	ra[1].buf.len = num * sizeof(page);

	ra[2].buf.pv = (void *)&routargs;
	ra[2].buf.len = sizeof(routargs);

	ioctl.inv.handle = 1;
	if (fl->apps->compat)
		ioctl.inv.sc = REMOTE_SCALARS_MAKE(4, 2, 1);
	else
		ioctl.inv.sc = REMOTE_SCALARS_MAKE(2, 2, 1);
	ioctl.inv.pra = ra;
	ioctl.fds = NULL;
	VERIFY(err, 0 == (err = fastrpc_internal_invoke(fl,
		FASTRPC_MODE_PARALLEL, 1, &ioctl)));
	*raddr = (uintptr_t)routargs.vaddrout;
	if (err)
		goto bail;
	if (flags == ADSP_MMAP_HEAP_ADDR) {
		struct scm_desc desc = {0};
		desc.args[0] = TZ_PIL_AUTH_QDSP6_PROC;
		desc.args[1] = phys;
		desc.args[2] = size;
		desc.arginfo = SCM_ARGS(3);
		err = scm_call2(SCM_SIP_FNID(SCM_SVC_PIL,
			TZ_PIL_PROTECT_MEM_SUBSYS_ID), &desc);
	}

bail:
	return err;
}

static int fastrpc_munmap_on_dsp_rh(struct fastrpc_file *fl, uint64_t phys,
						size_t size, uint32_t flags)
{
	struct fastrpc_ioctl_invoke_fd ioctl;
	struct scm_desc desc = {0};
	remote_arg_t ra[1];
	int err = 0;
	struct {
		uint8_t skey;
	} routargs;

	ra[0].buf.pv = (void *)&routargs;
	ra[0].buf.len = sizeof(routargs);

	ioctl.inv.handle = 1;
	ioctl.inv.sc = REMOTE_SCALARS_MAKE(7, 0, 1);
	ioctl.inv.pra = ra;
	ioctl.fds = NULL;

	VERIFY(err, 0 == (err = fastrpc_internal_invoke(fl,
			FASTRPC_MODE_PARALLEL, 1, &ioctl)));
	if (err)
		goto bail;
	desc.args[0] = TZ_PIL_AUTH_QDSP6_PROC;
	desc.args[1] = phys;
	desc.args[2] = size;
	desc.args[3] = routargs.skey;
	desc.arginfo = SCM_ARGS(4);
	err = scm_call2(SCM_SIP_FNID(SCM_SVC_PIL,
		TZ_PIL_CLEAR_PROTECT_MEM_SUBSYS_ID), &desc);

bail:
	return err;
}

static int fastrpc_munmap_on_dsp(struct fastrpc_file *fl, uintptr_t raddr,
				uint64_t phys, size_t size, uint32_t flags)
{
	struct fastrpc_ioctl_invoke_fd ioctl;
	int err = 0;
	remote_arg_t ra[1];
	struct {
		int pid;
		uintptr_t vaddrout;
		size_t size;
	} inargs;
	if (flags == ADSP_MMAP_HEAP_ADDR) {
		VERIFY(err, !fastrpc_munmap_on_dsp_rh(fl, phys, size, flags));
		if (err)
			goto bail;
	}

	inargs.pid = current->tgid;
	inargs.size = size;
	inargs.vaddrout = raddr;
	ra[0].buf.pv = (void *)&inargs;
	ra[0].buf.len = sizeof(inargs);

	ioctl.inv.handle = 1;
	if (fl->apps->compat)
		ioctl.inv.sc = REMOTE_SCALARS_MAKE(5, 1, 0);
	else
		ioctl.inv.sc = REMOTE_SCALARS_MAKE(3, 1, 0);
	ioctl.inv.pra = ra;
	ioctl.fds = NULL;

	VERIFY(err, 0 == (err = fastrpc_internal_invoke(fl,
			FASTRPC_MODE_PARALLEL, 1, &ioctl)));

bail:
	return err;
}

static int fastrpc_mmap_remove_ssr(struct fastrpc_file *fl)
{
	struct fastrpc_mmap *match = NULL, *map = NULL;
	struct hlist_node *n = NULL;
	int err = 0;
	struct fastrpc_apps *me = &gfa;
	spin_lock(&me->hlock);
	hlist_for_each_entry_safe(map, n, &me->maps, hn) {
			match = map;
			hlist_del_init(&map->hn);
			break;
	}
	spin_unlock(&me->hlock);

	if (match) {
		VERIFY(err, !fastrpc_munmap_on_dsp_rh(fl, match->phys,
						match->size, match->flags));
		if (err)
			goto bail;
		fastrpc_mmap_free(match);
	}
bail:
	if (err && match)
		fastrpc_mmap_add(match);
	return err;
}

static int fastrpc_mmap_remove(struct fastrpc_file *fl, uintptr_t va,
			     size_t len, struct fastrpc_mmap **ppmap);

static void fastrpc_mmap_add(struct fastrpc_mmap *map);

static int fastrpc_internal_munmap(struct fastrpc_file *fl,
				   struct fastrpc_ioctl_munmap *ud)
{
	int err = 0;
	struct fastrpc_mmap *map = NULL;
	struct fastrpc_buf *rbuf = NULL, *free = NULL;
	struct hlist_node *n;

	mutex_lock(&fl->map_mutex);
	spin_lock(&fl->hlock);
	hlist_for_each_entry_safe(rbuf, n, &fl->remote_bufs, hn_rem) {
		if (rbuf->raddr && (rbuf->flags == ADSP_MMAP_ADD_PAGES)) {
			if ((rbuf->raddr == ud->vaddrout) &&
				(rbuf->size == ud->size)) {
				free = rbuf;
				break;
			}
		}
	}
	spin_unlock(&fl->hlock);

	if (free) {
		VERIFY(err, !fastrpc_munmap_on_dsp(fl, free->raddr,
			free->phys, free->size, free->flags));
		if (err)
			goto bail;
		fastrpc_buf_free(rbuf, 0);
		mutex_unlock(&fl->map_mutex);
		return err;
	}
	if (!fastrpc_mmap_remove(fl, ud->vaddrout, ud->size,
				 &map)) {
		VERIFY(err, !fastrpc_munmap_on_dsp(fl, map->raddr,
				map->phys, map->size, map->flags));
		if (err)
			goto bail;
		fastrpc_mmap_free(map);
	}
bail:
	if (err && map)
		fastrpc_mmap_add(map);
	return err;
}

static int fastrpc_mmap_find(struct fastrpc_file *fl, int fd, uintptr_t va,
			size_t len, int mflags, struct fastrpc_mmap **ppmap)
{
	struct fastrpc_mmap *match = NULL, *map = NULL;
	struct hlist_node *n = NULL;

	if (mflags == ADSP_MMAP_HEAP_ADDR) {
		struct fastrpc_apps *me = &gfa;
		spin_lock(&me->hlock);
		hlist_for_each_entry_safe(map, n, &me->maps, hn) {
			if (map->va >= va &&
					map->va + map->len <= va + len &&
					map->fd == fd) {
				map->refs++;
				match = map;
				break;
			}
		}
		spin_unlock(&me->hlock);
	} else {
		spin_lock(&fl->hlock);
		hlist_for_each_entry_safe(map, n, &fl->maps, hn) {
			if (map->va >= va &&
					map->va + map->len <= va + len &&
					map->fd == fd) {
				map->refs++;
				match = map;
				break;
			}
		}
		spin_unlock(&fl->hlock);
	}
	if (match) {
		*ppmap = match;
		return 0;
	}
	return -ENOTTY;
}

static int dma_alloc_memory(phys_addr_t *region_start, size_t size)
{
	struct fastrpc_apps *me = &gfa;
	void *vaddr = NULL;
	DEFINE_DMA_ATTRS(attrs);

	dma_set_attr(DMA_ATTR_SKIP_ZEROING, &attrs);
	dma_set_attr(DMA_ATTR_NO_KERNEL_MAPPING, &attrs);
	vaddr = dma_alloc_attrs(me->adsp_mem_device, size,
							region_start, GFP_KERNEL,
							&attrs);
	if (!vaddr) {
		pr_err("ADSPRPC: Failed to allocate remote heap memory\n");
		return -ENOTTY;
	}
	return 0;
}

static int fastrpc_mmap_create(struct fastrpc_file *fl, int fd, uintptr_t va,
			size_t len, int mflags, struct fastrpc_mmap **ppmap)
{
	int err = 0;
	struct fastrpc_mmap *map = NULL;
	struct fastrpc_apps *me = &gfa;
	unsigned long ionflag = 0;
	size_t pa_len = 0;
	ion_phys_addr_t paddr = 0;
	void *virtual_addr = NULL;
	phys_addr_t region_start = 0;

	if (!fastrpc_mmap_find(fl, fd, va, len, mflags, ppmap))
		return 0;
	VERIFY(err, map = kzalloc(sizeof(*map), GFP_KERNEL));
	if (err)
		goto bail;
	map->flags = mflags;
	map->client = NULL;
	map->handle = NULL;
	map->fl = fl;
	map->fd = fd;
	map->is_filemap = false;
	if (mflags == ADSP_MMAP_HEAP_ADDR) {
		map->apps = me;
		map->fl = NULL;
		VERIFY(err, !dma_alloc_memory(&region_start, len));
		if (err)
			goto bail;
		map->phys = (uintptr_t)region_start;
		map->size = len;
	} else {
		VERIFY(err, !msm_audio_ion_import(DEVICE_NAME, &(map->client),
						&(map->handle), fd,
						&ionflag, len,
					&paddr, &pa_len, &virtual_addr));
		if (err) {
			pr_err("msm_audio_ion_import failed\n");
			goto bail;
		}
		map->phys = paddr;
		map->size = pa_len;
	}
	map->va = va;
	map->refs = 1;
	map->len = len;
	INIT_HLIST_NODE(&map->hn);
	fastrpc_mmap_add(map);

	*ppmap = map;

bail:
	if (err && map)
		fastrpc_mmap_free(map);
	return err;
}

static int fastrpc_internal_mmap(struct fastrpc_file *fl,
				 struct fastrpc_ioctl_mmap *ud)
{
	struct fastrpc_mmap *map = NULL;
	struct fastrpc_buf *rbuf = NULL;
	uintptr_t raddr = 0;
	int err = 0;

	mutex_lock(&fl->map_mutex);

	if (ud->flags == ADSP_MMAP_ADD_PAGES) {


		if (ud->vaddrin) {
			err = -EINVAL;
			pr_err("adsprpc: %s: %s: ERROR: adding user allocated pages is not supported\n",
					current->comm, __func__);
			goto bail;
		}
		err = fastrpc_buf_alloc(fl, ud->size, ud->flags,
								1, &rbuf);
		if (err)
			goto bail;
		err = fastrpc_mmap_on_dsp(fl, ud->flags, 0,
				rbuf->phys, rbuf->size, &raddr);
		if (err)
			goto bail;
		rbuf->raddr = raddr;
	} else {
		uintptr_t va_to_dsp;

		VERIFY(err, !fastrpc_mmap_create(fl, ud->fd,
				(uintptr_t)ud->vaddrin, ud->size,
				 ud->flags, &map));
		if (err)
			goto bail;

		if (ud->flags == ADSP_MMAP_HEAP_ADDR)
			va_to_dsp = 0;
		else
			va_to_dsp = (uintptr_t)map->va;
		VERIFY(err, 0 == fastrpc_mmap_on_dsp(fl, ud->flags, va_to_dsp,
				map->phys, map->size, &raddr));
		if (err)
			goto bail;
		map->raddr = raddr;
	}
	ud->vaddrout = raddr;
 bail:
	if (err && map)
		fastrpc_mmap_free(map);
	return err;
}

static void fastrpc_channel_close(struct kref *kref)
{
	struct fastrpc_apps *me = &gfa;
	struct fastrpc_chan_ctx *ctx;
	int cid;

	ctx = container_of(kref, struct fastrpc_chan_ctx, kref);
	smd_close(ctx->chan);
	ctx->chan = NULL;
	mutex_unlock(&me->smd_mutex);
	cid = ctx - &me->channel[0];
	pr_info("'closed /dev/%s c %d %d'\n", gcinfo[cid].name,
						MAJOR(me->dev_no), cid);
}

static void fastrpc_context_list_dtor(struct fastrpc_file *fl);

static int fastrpc_file_free(struct fastrpc_file *fl)
{
	struct hlist_node *n;
	struct fastrpc_mmap *map = NULL;
	int cid;

	if (!fl)
		return 0;
	cid = fl->cid;

	spin_lock(&fl->apps->hlock);
	hlist_del_init(&fl->hn);
	spin_unlock(&fl->apps->hlock);

	(void)fastrpc_release_current_dsp_process(fl);
	if (!IS_ERR_OR_NULL(fl->init_mem))
		fastrpc_buf_free(fl->init_mem, 0);
	fastrpc_context_list_dtor(fl);
	fastrpc_cached_buf_list_free(fl);
	hlist_for_each_entry_safe(map, n, &fl->maps, hn) {
		fastrpc_mmap_free(map);
	}
	if (fl->ssrcount == fl->apps->channel[cid].ssrcount)
		kref_put_mutex(&fl->apps->channel[cid].kref,
				fastrpc_channel_close, &fl->apps->smd_mutex);

	mutex_destroy(&fl->map_mutex);
	fastrpc_remote_buf_list_free(fl);
	kfree(fl);
	return 0;
}

static int fastrpc_device_release(struct inode *inode, struct file *file)
{
	fastrpc_file_free((struct fastrpc_file *)file->private_data);
	file->private_data = NULL;
	return 0;
}

static int fastrpc_device_open(struct inode *inode, struct file *filp)
{
	int cid = MINOR(inode->i_rdev);
	int err = 0;
	struct fastrpc_apps *me = &gfa;
	struct fastrpc_file *fl = NULL;

	VERIFY(err, fl = kzalloc(sizeof(*fl), GFP_KERNEL));
	if (err)
		return err;

	filp->private_data = fl;

	mutex_lock(&me->smd_mutex);

	context_list_ctor(&fl->clst);
	spin_lock_init(&fl->hlock);
	INIT_HLIST_HEAD(&fl->maps);
	INIT_HLIST_HEAD(&fl->cached_bufs);
	INIT_HLIST_HEAD(&fl->remote_bufs);
	INIT_HLIST_NODE(&fl->hn);
	fl->chan = &me->channel[cid];
	fl->cid = cid;
	fl->tgid = current->tgid;
	fl->apps = me;
	fl->init_mem = NULL;

	fl->ssrcount = me->channel[cid].ssrcount;
	if ((kref_get_unless_zero(&me->channel[cid].kref) == 0) ||
	    (me->channel[cid].chan == NULL)) {
		VERIFY(err, !smd_named_open_on_edge(FASTRPC_SMD_GUID,
						    gcinfo[cid].channel,
						    &me->channel[cid].chan,
						    (void *)(uintptr_t)cid,
						    smd_event_handler));
		if (err)
			goto bail;
		VERIFY(err, wait_for_completion_timeout(&me->channel[cid].work,
							 RPC_TIMEOUT));
		if (err)
			goto bail;
		kref_init(&me->channel[cid].kref);
		pr_info("'opened /dev/%s c %d %d'\n", gcinfo[cid].name,
						MAJOR(me->dev_no), cid);
		if (me->channel[cid].ssrcount !=
				 me->channel[cid].prevssrcount) {
			if (fastrpc_mmap_remove_ssr(fl))
				pr_err("ADSPRPC: SSR: Failed to unmap remote heap\n");
			me->channel[cid].prevssrcount =
						me->channel[cid].ssrcount;
		}
	}
	spin_lock(&me->hlock);
	hlist_add_head(&fl->hn, &me->drivers);
	spin_unlock(&me->hlock);

bail:
	mutex_unlock(&me->smd_mutex);

	if (err && fl)
		fastrpc_device_release(inode, filp);
	return err;
}

static int fastrpc_internal_control(struct fastrpc_file *fl,
					struct fastrpc_ioctl_control *cp)
{
	int err = 0;

	VERIFY(err, !IS_ERR_OR_NULL(fl) && !IS_ERR_OR_NULL(fl->apps));
	if (err)
		goto bail;
	VERIFY(err, !IS_ERR_OR_NULL(cp));
	if (err)
		goto bail;

	switch (cp->req) {
	case FASTRPC_CONTROL_KALLOC:
		cp->kalloc.kalloc_support = 1;
		break;
	default:
		err = -ENOTTY;
		break;
	}
bail:
	return err;
}

static long fastrpc_device_ioctl(struct file *file, unsigned int ioctl_num,
				 unsigned long ioctl_param)
{
	union {
		struct fastrpc_ioctl_invoke_fd invokefd;
		struct fastrpc_ioctl_mmap mmap;
		struct fastrpc_ioctl_munmap munmap;
		struct fastrpc_ioctl_init init;
		struct fastrpc_ioctl_control cp;
	} p;
	void *param = (char *)ioctl_param;
	struct fastrpc_file *fl = (struct fastrpc_file *)file->private_data;
	int size = 0, err = 0;

	switch (ioctl_num) {
	case FASTRPC_IOCTL_INVOKE_FD:
	case FASTRPC_IOCTL_INVOKE:
		p.invokefd.fds = 0;
		size = (ioctl_num == FASTRPC_IOCTL_INVOKE) ?
				sizeof(p.invokefd.inv) : sizeof(p.invokefd);
		K_COPY_FROM_USER(err, 0, &p.invokefd, param, size);
		if (err)
			goto bail;
		VERIFY(err, 0 == (err = fastrpc_internal_invoke(fl, fl->mode,
						0, &p.invokefd)));
		if (err)
			goto bail;
		break;
	case FASTRPC_IOCTL_MMAP:
		K_COPY_FROM_USER(err, 0, &p.mmap, param,
						sizeof(p.mmap));
		if (err)
			goto bail;
		VERIFY(err, 0 == (err = fastrpc_internal_mmap(fl, &p.mmap)));
		if (err)
			goto bail;
		K_COPY_TO_USER(err, 0, param, &p.mmap, sizeof(p.mmap));
		if (err)
			goto bail;
		break;
	case FASTRPC_IOCTL_MUNMAP:
		K_COPY_FROM_USER(err, 0, &p.munmap, param,
						sizeof(p.munmap));
		if (err)
			goto bail;
		VERIFY(err, 0 == (err = fastrpc_internal_munmap(fl,
							&p.munmap)));
		if (err)
			goto bail;
		break;
	case FASTRPC_IOCTL_SETMODE:
		switch ((uint32_t)ioctl_param) {
		case FASTRPC_MODE_PARALLEL:
		case FASTRPC_MODE_SERIAL:
			fl->mode = (uint32_t)ioctl_param;
			break;
		default:
			err = -ENOTTY;
			break;
		}
		break;
	case FASTRPC_IOCTL_CONTROL:
		K_COPY_FROM_USER(err, 0, &p.cp, param,
				sizeof(p.cp));
		if (err)
			goto bail;
		VERIFY(err, 0 == (err = fastrpc_internal_control(fl, &p.cp)));
		if (err)
			goto bail;
		if (p.cp.req == FASTRPC_CONTROL_KALLOC) {
			K_COPY_TO_USER(err, 0, param, &p.cp, sizeof(p.cp));
			if (err)
				goto bail;
		}
		break;
	case FASTRPC_IOCTL_INIT:
		K_COPY_FROM_USER(err, 0, &p.init, param, sizeof(p.init));
		if (err)
			goto bail;
		VERIFY(err, 0 == fastrpc_init_process(fl, &p.init));
		if (err)
			goto bail;
		break;

	default:
		err = -ENOTTY;
		break;
	}
 bail:
	return err;
}

static int fastrpc_restart_notifier_cb(struct notifier_block *nb,
					unsigned long code,
					void *data)
{
	struct fastrpc_apps *me = &gfa;
	struct fastrpc_chan_ctx *ctx;
	int cid;

	ctx = container_of(nb, struct fastrpc_chan_ctx, nb);
	cid = ctx - &me->channel[0];
	if (code == SUBSYS_BEFORE_SHUTDOWN) {
		mutex_lock(&me->smd_mutex);
		ctx->ssrcount++;
		if (ctx->chan) {
			smd_close(ctx->chan);
			ctx->chan = NULL;
			pr_info("'closed /dev/%s c %d %d'\n", gcinfo[cid].name,
						MAJOR(me->dev_no), cid);
		}
		mutex_unlock(&me->smd_mutex);
		fastrpc_notify_drivers(me, cid);
	}

	return NOTIFY_DONE;
}

static const struct file_operations fops = {
	.open = fastrpc_device_open,
	.release = fastrpc_device_release,
	.unlocked_ioctl = fastrpc_device_ioctl,
	.compat_ioctl = compat_fastrpc_device_ioctl,
};

static int adsp_mem_driver_probe(struct platform_device *pdev)
{
	struct fastrpc_apps *me = &gfa;
	struct device *dev = &pdev->dev;

	if (of_device_is_compatible(dev->of_node,
					"qcom,msm-adsprpc-mem-region")) {
		me->adsp_mem_device = dev;
		return 0;
	}
	return -EINVAL;
}

static struct of_device_id adsp_mem_match_table[] = {
	{ .compatible = "qcom,msm-adsprpc-mem-region" },
	{}
};

static struct platform_driver adsp_memory_driver = {
	.probe = adsp_mem_driver_probe,
	.driver = {
		.name = "msm-adsprpc-mem-region",
		.of_match_table = adsp_mem_match_table,
		.owner = THIS_MODULE,
	},
};

static int __init fastrpc_device_init(void)
{
	struct fastrpc_apps *me = &gfa;
	struct device_node *node = NULL;
	struct platform_device *pdev = NULL;
	struct iommu_group *group = NULL;
	struct iommu_domain *domain = NULL;
	int err = 0, i;

	memset(me, 0, sizeof(*me));

	fastrpc_init(me);
	VERIFY(err, 0 == alloc_chrdev_region(&me->dev_no, 0, NUM_CHANNELS,
					DEVICE_NAME));
	if (err)
		goto alloc_chrdev_bail;
	cdev_init(&me->cdev, &fops);
	me->cdev.owner = THIS_MODULE;
	VERIFY(err, 0 == cdev_add(&me->cdev, MKDEV(MAJOR(me->dev_no), 0),
				NUM_CHANNELS));
	if (err)
		goto cdev_init_bail;
	me->class = class_create(THIS_MODULE, "fastrpc");
	VERIFY(err, !IS_ERR(me->class));
	if (err)
		goto class_create_bail;
	me->compat = (NULL == fops.compat_ioctl) ? 0 : 1;
	for (i = 0; i < NUM_CHANNELS; i++) {
		me->channel[i].dev = device_create(me->class, NULL,
					MKDEV(MAJOR(me->dev_no), i),
					NULL, gcinfo[i].name);
		VERIFY(err, !IS_ERR(me->channel[i].dev));
		if (err)
			goto device_create_bail;
		me->channel[i].ssrcount = 0;
		me->channel[i].prevssrcount = 0;
		me->channel[i].nb.notifier_call = fastrpc_restart_notifier_cb,
		(void)subsys_notif_register_notifier(gcinfo[i].subsys,
							&me->channel[i].nb);
		if (!gcinfo[i].node)
			continue;
		node = of_find_compatible_node(NULL, NULL, gcinfo[i].node);
		if (node) {
			pdev = of_find_device_by_node(node);
			if (pdev) {
				me->channel[i].smmu.dev = &pdev->dev;
				me->channel[i].smmu.enabled = 1;
				me->channel[i].smmu.cb = 1;
				dev_dbg(me->channel[i].dev,
					"%s: Using audio Context bank\n",
					__func__);
			}
		}
	}
	group = iommu_group_find("lpass_audio");
	if (!group) {
		pr_debug("Failed to find group lpass_audio deferred\n");
		err = -1;
		goto register_bail;
	}
	domain = iommu_group_get_iommudata(group);
	if (IS_ERR_OR_NULL(domain)) {
		pr_err("Failed to get domain data for group %p\n",
				group);
		err = -1;
		goto register_bail;
	}
	me->domain_id = msm_find_domain_no(domain);
	if (me->domain_id < 0) {
		pr_err("Failed to get domain index for domain %p\n",
				domain);
		err = -1;
		goto register_bail;
	}
	pr_debug("domain=%p, domain_id=%d, group=%p\n", domain,
			me->domain_id, group);
	err = platform_driver_register(&adsp_memory_driver);
	if (err) {
		pr_err("ADSPRPC: Failed to register adsp memory driver");
		goto register_bail;
	}

	return 0;

device_create_bail:
	class_destroy(me->class);
class_create_bail:
	cdev_del(&me->cdev);
cdev_init_bail:
	unregister_chrdev_region(me->dev_no, NUM_CHANNELS);
alloc_chrdev_bail:
	fastrpc_deinit();
register_bail:
	return err;
}

static void __exit fastrpc_device_exit(void)
{
	struct fastrpc_apps *me = &gfa;
	int i;

	fastrpc_file_list_dtor(me);
	fastrpc_deinit();
	for (i = 0; i < NUM_CHANNELS; i++) {
		device_destroy(me->class, MKDEV(MAJOR(me->dev_no), i));
		subsys_notif_unregister_notifier(gcinfo[i].subsys,
						&me->channel[i].nb);
	}
	class_destroy(me->class);
	cdev_del(&me->cdev);
	unregister_chrdev_region(me->dev_no, NUM_CHANNELS);
}

late_initcall(fastrpc_device_init);
module_exit(fastrpc_device_exit);

MODULE_LICENSE("GPL v2");
