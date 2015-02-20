/*
 * Copyright CERN 2014
 * Author: Federico Vaga <federico.vaga@gmail.com>
 *
 * handle DMA mapping
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>

#include <linux/zio-dma.h>
#include <linux/zio-buffer.h>
#include "zio-internal.h"


/**
 * It allocate a DMA area for the transfers pool
 * @param[in] zdma zio DMA descriptor from zio_dma_alloc_sg()
 * @param[in] page_desc_size size of the transfer descriptor of the hardware
 */
static int __zio_dma_alloc_pool(struct zio_dma_sgt *zdma, size_t page_desc_size)
{
	unsigned int tot_nents = 0;
	int i;

	/* Prepare the transfers pool area */
	zdma->page_desc_size = page_desc_size;

	#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,6,0)
	for (i = 0; i < zdma->n_blocks; ++i)
		tot_nents += zdma->sg_blocks[i].sgt.nents;
	#else
	tot_nents = zdma->sgt.nents;
	#endif
	if (unlikely(!tot_nents)) {
		dev_warn(zdma->hwdev, "No DMA page descriptor to allocate\n");
		return -ENOMEM;
	}

	zdma->page_desc_pool_size = zdma->page_desc_size * tot_nents;
	zdma->page_desc_pool = kzalloc(zdma->page_desc_pool_size,
				       GFP_ATOMIC | GFP_DMA);
	if (!zdma->page_desc_pool) {
		dev_err(zdma->hwdev, "cannot allocate coherent dma memory\n");
		return -ENOMEM;
	}

	/* Allocate a DMA area to store the DMA transfers */
	zdma->page_desc_pool_dma = dma_map_single(zdma->hwdev,
						  zdma->page_desc_pool,
						  zdma->page_desc_pool_size,
						  DMA_TO_DEVICE);
	if (!zdma->page_desc_pool_dma) {
		kfree(zdma->page_desc_pool);
		return -ENOMEM;
	}

	pr_debug("%s:%d DMA transfer pool allocated for max %d transfers\n",
		 __func__, __LINE__, tot_nents);
	return 0;
}


/**
 * It free a DMA area for the transfers pool
 * @param[in] zdma zio DMA descriptor from zio_dma_alloc_sg()
 */
static void __zio_dma_free_pool(struct zio_dma_sgt *zdma)
{
       dma_unmap_single(zdma->hwdev, zdma->page_desc_pool_dma,
                        zdma->page_desc_pool_size, DMA_TO_DEVICE);
       kfree(zdma->page_desc_pool);

       /* Clear data */
       zdma->page_desc_pool_size = 0;
       zdma->page_desc_pool_dma = 0;
       zdma->page_desc_pool = NULL;
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,6,0)
/**
 * It convert a zio block into an array of pages
 * @param[in] block
 * @param[out] pages
 * @param[in] max_n_pages
 * @return number of real pages
 */
static unsigned int zio_block_to_pages(struct zio_block *block,
				       struct page **pages,
				       unsigned int max_n_pages)
{
	unsigned int bytesleft = block->datalen, mapbytes;
	unsigned int n_pages = 0;
	void *bufp = block->data;

	if (is_vmalloc_addr(bufp)) {
		while (bytesleft && n_pages < max_n_pages) {
			pages[n_pages] = vmalloc_to_page(bufp);
			n_pages++;
			if (bytesleft < (PAGE_SIZE - offset_in_page(bufp)))
				mapbytes = bytesleft;
			else
				mapbytes = PAGE_SIZE - offset_in_page(bufp);
			bufp += mapbytes;
			bytesleft -= mapbytes;
		}
	} else {
		while (bytesleft && n_pages < max_n_pages) {
			pages[n_pages] = virt_to_page(bufp);
			n_pages++;
			if (bytesleft < (PAGE_SIZE - offset_in_page(bufp)))
				mapbytes = bytesleft;
			else
				mapbytes = PAGE_SIZE - offset_in_page(bufp);
			bufp += mapbytes;
			bytesleft -= mapbytes;
		}
	}

	pr_debug("%s:%d found %d pages\n", __func__, __LINE__, n_pages);
	return n_pages;
}


/**
 * Allocate resources for DMA stransfer of a single block
 * @param[in] sgb block which need a DMA transfer
 */
static int __zio_dma_alloc_sg_single(struct zio_blocks_sg *sgb, gfp_t gfp)
{
	unsigned int max_n_pages;
	int err;

	/* Get pages from the zio block */
	max_n_pages = (((unsigned long)sgb->block->data & ~PAGE_MASK) +
		       sgb->block->datalen + ~PAGE_MASK) >> PAGE_SHIFT;
	if (unlikely(!max_n_pages)) {
		pr_warn("%s: No page to allocate for DMA transfer\n", __func__);
		return -ENOMEM;
	}
	sgb->pages = kmalloc(max_n_pages * sizeof(struct page *), gfp);
	if (!sgb->pages)
		return -ENOMEM;

	/* get the array of pages for a given block */
	sgb->n_pages = zio_block_to_pages(sgb->block, sgb->pages,
					  max_n_pages);

	/* Allocate scatterlist table and optimize dma transfers */
	err = sg_alloc_table_from_pages(&sgb->sgt,
					sgb->pages, sgb->n_pages,
					offset_in_page(sgb->block->data),
					sgb->block->datalen,
					gfp);
	if (err)
		kfree(sgb->pages);

	pr_debug("%s:%d allocated scatter list for max %d pages\n",
		 __func__, __LINE__, sgb->n_pages);
	return err;
}


/**
 * Release resources for a single block
 * @param[in] sgb block to release
 */
static void __zio_dma_free_sg_single(struct zio_blocks_sg *sgb)
{
	sg_free_table(&sgb->sgt);
	kfree(sgb->pages);
}


/**
 *
 * @param[in] zdma zio DMA descriptor
 * @param[in] sgb block to release
 * @param[in] fill_desc callback for the driver in order to fill each transfer
 *                      descriptor.
 * @return 0 on success, otherwise en error code
 */
static int __zio_dma_map_sg_single(struct zio_dma_sgt *zdma,
				   struct zio_blocks_sg *sgb, unsigned int i_blk,
				   int (*fill_desc)(struct zio_dma_sg *zsg))
{
	uint32_t dev_mem_off = 0, sglen;
	struct scatterlist *sg;
	struct zio_dma_sg zsg;
	void *item_ptr;
	int err, i;

	dev_mem_off = sgb->dev_mem_off;
	/* Map DMA buffers */
	sglen = dma_map_sg(zdma->hwdev, sgb->sgt.sgl, sgb->sgt.nents,
			   DMA_FROM_DEVICE);
	if (!sglen) {
		dev_err(zdma->hwdev, "cannot map dma SG memory\n");
	        return -ENOMEM;
	}

	zsg.zsgt = zdma;
	for_each_sg(sgb->sgt.sgl, sg, sgb->sgt.nents, i) {
		dev_dbg(zdma->hwdev, "%s: addr 0x%x, len %d, dev_off 0x%x\n",
			__func__, sg_dma_address(sg), sg_dma_len(sg),
			dev_mem_off);
		dev_dbg(zdma->hwdev, "%d 0x%x\n", i, dev_mem_off);

		/* Configure hardware pages */
		zsg.sg = sg;
		zsg.dev_mem_off = dev_mem_off;
		zsg.page_desc = zdma->page_desc_next;
		zsg.block_idx = i_blk;
		zsg.page_idx= i;

		/* Point to the next free DMA slot for page descriptors */
		zdma->page_desc_next += zdma->page_desc_size;
		zdma->page_desc_pool_dma_next += zdma->page_desc_size;

		/* Ask driver to fill page descriptor */
		err = fill_desc(&zsg);
		if (err) {
			dev_err(zdma->hwdev, "Cannot fill descriptor %d\n", i);
			goto out;
		}
		dev_mem_off += sg_dma_len(sg);
	}

	pr_debug("%s:%d mapped %d DMA transfers\n",
		 __func__, __LINE__, i);
	return 0;

/* errors */
out:
	dma_unmap_sg(zdma->hwdev, sgb->sgt.sgl, sgb->sgt.nents,
		     DMA_FROM_DEVICE);
	return err;
}


/**
 *
 * @param[in] zdma zio DMA descriptor
 * @param[in] sgb block to release
 */
static void __zio_dma_unmap_sg_single(struct zio_dma_sgt *zdma,
				     struct zio_blocks_sg *sgb)
{
	dma_unmap_sg(zdma->hwdev, sgb->sgt.sgl, sgb->sgt.nents,
		     DMA_FROM_DEVICE);
}


/**
 * @param[in] chan zio channel associated to this scatterlist
 * @param[in] hwdev low level device responsible of the DMA
 * @param[in] blocks array of zio_block to transfer
 * @param[in] n_blocks number of blocks to transfer
 * @param[in] gfp gfp flags for memory allocation
 *
 * The function allocates and initializes a scatterlist ready for DMA
 * transfer
 */
struct zio_dma_sgt *zio_dma_alloc_sg(struct zio_channel *chan,
				    struct device *hwdev,
				    struct zio_block **blocks, /* FIXME to array */
				    unsigned int n_blocks, gfp_t gfp)
{
	struct zio_dma_sgt *zdma;
	unsigned int i;
	int err;

	if (unlikely(!chan || !hwdev || !blocks || !n_blocks))
		return ERR_PTR(-EINVAL);

	/*
	 * Allocate a new zio_dma_sgt structure that will contains all necessary
	 * information for DMA
	 */
	zdma = kzalloc(sizeof(struct zio_dma_sgt), gfp);
	if (!zdma)
		return ERR_PTR(-ENOMEM);
	zdma->chan = chan;

	/* Allocate a new list of blocks with sg information */
	zdma->sg_blocks = kzalloc(sizeof(struct zio_blocks_sg) * n_blocks, gfp);
	if (!zdma->sg_blocks) {
		err = -ENOMEM;
		goto out;
	}

	/* fill the zio_dma_sgt structure for each sg_block */
	zdma->hwdev = hwdev;
	zdma->n_blocks = n_blocks;
	for (i = 0; i < n_blocks; ++i) {
		zdma->sg_blocks[i].block = blocks[i];
		err = __zio_dma_alloc_sg_single(&zdma->sg_blocks[i], gfp);
		if (err)
			goto out_alloc;
	}

	pr_debug("%s:%d allocated scatter lists for %d blocks\n",
		 __func__, __LINE__, n_blocks);
	return zdma;

/* errors */
out_alloc:
	while (--i)
		__zio_dma_free_sg_single(&zdma->sg_blocks[i]);
	kfree(zdma->sg_blocks);
out:
	kfree(zdma);

	return ERR_PTR(err);
}
EXPORT_SYMBOL(zio_dma_alloc_sg);


/**
 * It releases resources
 * @param[in] zdma: zio DMA transfer descriptor
 */
void zio_dma_free_sg(struct zio_dma_sgt *zdma)
{
	int i;

	/* release all sgt tables and array of pages */
	for (i = 0; i < zdma->n_blocks; ++i)
		__zio_dma_free_sg_single(&zdma->sg_blocks[i]);

	kfree(zdma->sg_blocks);
	kfree(zdma);
}
EXPORT_SYMBOL(zio_dma_free_sg);


/**
 * It maps a sg table
 * @param[in] zdma zio DMA descriptor from zio_dma_alloc_sg()
 * @param[in] page_desc_size the size (in byte) of the dma transfer descriptor
 *                           of the specific hw
 * @param[in] fill_desc callback for the driver in order to fill each transfer
 *                      descriptor.
 * @return 0 on success, otherwise an error code
 */
int zio_dma_map_sg(struct zio_dma_sgt *zdma, size_t page_desc_size,
		   int (*fill_desc)(struct zio_dma_sg *zsg))
{
	unsigned int i;
	int err;

	if (unlikely(!zdma || !fill_desc || !page_desc_size))
		return -EINVAL;

	err = __zio_dma_alloc_pool(zdma, page_desc_size);
	if (err)
		return err;

	/* Configure a DMA transfer for each block */
	zdma->page_desc_next = zdma->page_desc_pool;
	zdma->page_desc_pool_dma_next = zdma->page_desc_pool_dma;
	for (i = 0; i < zdma->n_blocks; ++i) {
		err = __zio_dma_map_sg_single(zdma, &zdma->sg_blocks[i],
					      i, fill_desc);
		if (err)
			goto out;
	}

	pr_debug("%s:%d mapped %d blocks\n", __func__, __LINE__, zdma->n_blocks);
	return 0;

/* errors */
out:
	while (--i)
		__zio_dma_unmap_sg_single(zdma, &zdma->sg_blocks[i]);
	__zio_dma_free_pool(zdma);

	return err;
}
EXPORT_SYMBOL(zio_dma_map_sg);


/**
 * It unmaps a sg table
 * @param[im] zdma zio DMA descriptor from zio_dma_alloc_sg()
 */
void zio_dma_unmap_sg(struct zio_dma_sgt *zdma)
{
	struct zio_blocks_sg *sgb;
	size_t size;
	int i;

	/* release all the mapped areas */
	for (i = 0; i < zdma->n_blocks; ++i) {
		sgb = &zdma->sg_blocks[i];
		size = zdma->page_desc_size * sgb->sgt.nents;
		dma_unmap_sg(zdma->hwdev, sgb->sgt.sgl, sgb->sgt.nents,
			     DMA_FROM_DEVICE);
	}

	__zio_dma_free_pool(zdma);
}
EXPORT_SYMBOL(zio_dma_unmap_sg);

#else /* LINUX KERNEL < 3.2.0 */


static int zio_calculate_nents(struct zio_blocks_sg *sg_blocks,
			       unsigned int n_blocks)
{
	int i, nents = 0;
	void *bufp;

	for (i = 0; i < n_blocks; ++i) {
		bufp = sg_blocks[i].block->data;
		sg_blocks[i].first_nent = nents;

		nents += sg_blocks[i].block->datalen / PAGE_SIZE + 1;
	}

	return nents;
}

static void zio_dma_setup_scatter(struct zio_dma_sgt *zdma)
{
	struct scatterlist *sg;
	int bytesleft = 0;
	void *bufp = NULL;
	int mapbytes;
	int i, i_blk;

	i_blk = 0;
	for_each_sg(zdma->sgt.sgl, sg, zdma->sgt.nents, i) {
		if (i_blk < zdma->n_blocks && i == zdma->sg_blocks[i_blk].first_nent) {
			WARN(bytesleft, "unmapped byte in block %i\n",
			     i_blk - 1);
			/*
			 * Configure the DMA for a new block, reset index and
			 * data pointer
			 */
			bytesleft = zdma->sg_blocks[i_blk].block->datalen;
			bufp = zdma->sg_blocks[i_blk].block->data;

			i_blk++; /* index the next block */
			if (unlikely(i_blk > zdma->n_blocks))
				BUG();
		}

		/*
		 * If there are less bytes left than what fits
		 * in the current page (plus page alignment offset)
		 * we just feed in this, else we stuff in as much
		 * as we can.
		 */
		if (bytesleft < (PAGE_SIZE - offset_in_page(bufp)))
			mapbytes = bytesleft;
		else
			mapbytes = PAGE_SIZE - offset_in_page(bufp);
		/* Map the page */
		if (is_vmalloc_addr(bufp))
			sg_set_page(sg, vmalloc_to_page(bufp), mapbytes,
				    offset_in_page(bufp));
		else
			sg_set_buf(sg, bufp, mapbytes);
		/* Configure next values */
		bufp += mapbytes;
		bytesleft -= mapbytes;
		pr_debug("sg item (%p(+0x%lx), len:%d, left:%d)\n",
			 virt_to_page(bufp), offset_in_page(bufp),
			 mapbytes, bytesleft);
	}
}

/*
 * zio_alloc_scatterlist
 * @chan: zio channel associated to this scatterlist
 * @hwdev: low level device responsible of the DMA
 * @blocks: array of zio_block to transfer
 * @n_blocks: number of blocks to transfer
 * @gfp: gfp flags for memory allocation
 *
 * The function allocates and initializes a scatterlist ready for DMA
 * transfer
 */
struct zio_dma_sgt *zio_dma_alloc_sg(struct zio_channel *chan,
				    struct device *hwdev,
				    struct zio_block **blocks, /* FIXME to array */
				    unsigned int n_blocks, gfp_t gfp)
{
	struct zio_dma_sgt *zdma;
	unsigned int i, pages;
	int err;

	if (unlikely(!chan || !hwdev || !blocks || !n_blocks))
		return ERR_PTR(-EINVAL);

	/*
	 * Allocate a new zio_dma_sgt structure that will contains all necessary
	 * information for DMA
	 */
	zdma = kzalloc(sizeof(struct zio_dma_sgt), gfp);
	if (!zdma)
		return ERR_PTR(-ENOMEM);
	zdma->chan = chan;
	/* Allocate a new list of blocks with sg information */
	zdma->sg_blocks = kzalloc(sizeof(struct zio_blocks_sg) * n_blocks, gfp);
	if (!zdma->sg_blocks) {
		err = -ENOMEM;
		goto out;
	}

	/* fill the zio_dma_sgt structure */
	zdma->hwdev = hwdev;
	zdma->n_blocks = n_blocks;
	for (i = 0; i < n_blocks; ++i)
		zdma->sg_blocks[i].block = blocks[i];


	/* calculate the number of necessary pages to transfer */
	pages = zio_calculate_nents(zdma->sg_blocks, zdma->n_blocks);
	if (!pages) {
		err = -EINVAL;
		goto out_calc_nents;
	}

	/* Create sglists for the transfers */
	err = sg_alloc_table(&zdma->sgt, pages, gfp);
	if (err)
		goto out_alloc_sg;

	/* Setup the scatter list for the provided block */
	zio_dma_setup_scatter(zdma);

	return zdma;

out_alloc_sg:
out_calc_nents:
	kfree(zdma->sg_blocks);
out:
	kfree(zdma);

	return ERR_PTR(err);
}
EXPORT_SYMBOL(zio_dma_alloc_sg);


/*
 * zio_free_scatterlist
 * @zdma: zio DMA transfer descriptor
 *
 * It releases resources
 */
void zio_dma_free_sg(struct zio_dma_sgt *zdma)
{
	sg_free_table(&zdma->sgt);
	kfree(zdma->sg_blocks);
	kfree(zdma);
}
EXPORT_SYMBOL(zio_dma_free_sg);


/*
 * zio_dma_map_sg
 * @zdma: zio DMA descriptor from zio_dma_alloc_sg()
 * @page_desc_size: the size (in byte) of the dma transfer descriptor of the
 *                  specific hw
 * @fill_desc: callback for the driver in order to fill each transfer
 *             descriptor
 *
 *It maps a sg table
 *
 * fill_desc
 * @zdma: zio DMA descriptor from zio_dma_alloc_sg()
 * @page_idx: index of the current page transfer
 * @block_idx: index of the current zio_block
 * @page_desc: current descriptor to fill
 * @dev_mem_offset: offset within the device memory
 * @sg: current sg descriptor
 */
int zio_dma_map_sg(struct zio_dma_sgt *zdma, size_t page_desc_size,
			int (*fill_desc)(struct zio_dma_sg *zsg))
{
	unsigned int i, err = 0, sglen, i_blk;
	uint32_t dev_mem_off = 0;
	struct scatterlist *sg;
	struct zio_dma_sg zsg;

	if (unlikely(!zdma || !fill_desc || !page_desc_size))
		return -EINVAL;

	err = __zio_dma_alloc_pool(zdma, page_desc_size);
	if (err)
		return err;

	/* Map DMA buffers */
	sglen = dma_map_sg(zdma->hwdev, zdma->sgt.sgl, zdma->sgt.nents,
			   DMA_FROM_DEVICE);
	if (!sglen) {
		dev_err(zdma->hwdev, "cannot map dma SG memory\n");
		goto out_map_sg;
	}

	i_blk = 0;
	zdma->page_desc_next = zdma->page_desc_pool;
	zdma->page_desc_pool_dma_next = zdma->page_desc_pool_dma;
	for_each_sg(zdma->sgt.sgl, sg, zdma->sgt.nents, i) {
		dev_dbg(zdma->hwdev, "%d 0x%x\n", i, dev_mem_off);
		if (i_blk < zdma->n_blocks && i == zdma->sg_blocks[i_blk].first_nent) {
			dev_dbg(zdma->hwdev, "%d is the first nent of block %d\n", i, i_blk);
			dev_mem_off = zdma->sg_blocks[i_blk].dev_mem_off;

			i_blk++; /* index the next block */
			if (unlikely(i_blk > zdma->n_blocks)) {
				dev_err(zdma->hwdev, "DMA map out of block\n");
				BUG();
			}
		}

 		/* Configure hardware pages */
		zsg.zsgt = zdma;
		zsg.sg = sg;
		zsg.dev_mem_off = dev_mem_off;
		zsg.page_desc = zdma->page_desc_next;
		zsg.block_idx = i_blk - 1;
		zsg.page_idx= i;

		dev_dbg(zdma->hwdev, "%d/%d DMA page_desc addr: 0x%lx\n",
			zsg.page_idx, zsg.block_idx,
			zdma->page_desc_pool_dma_next);

		/* Point to the next free DMA slot for page descriptors */
		zdma->page_desc_next += zdma->page_desc_size;
		zdma->page_desc_pool_dma_next += zdma->page_desc_size;

		/* Ask driver to fill the page descriptor */
		err = fill_desc(&zsg);
		if (err) {
			dev_err(zdma->hwdev, "Cannot fill descriptor %d\n", i);
			goto out_fill_desc;
		}

		dev_mem_off += sg_dma_len(sg);
	}

	return 0;

out_fill_desc:
	dma_unmap_sg(zdma->hwdev, zdma->sgt.sgl, zdma->sgt.nents,
		     DMA_FROM_DEVICE);
out_map_sg:
	__zio_dma_free_pool(zdma);

	return err;
}
EXPORT_SYMBOL(zio_dma_map_sg);


/*
 * zio_dma_unmap_sg
 * @zdma: zio DMA descriptor from zio_dma_alloc_sg()
 *
 * It unmaps a sg table
 */
void zio_dma_unmap_sg(struct zio_dma_sgt *zdma)
{
	size_t size;

	size = zdma->page_desc_size * zdma->sgt.nents;
	dma_unmap_sg(zdma->hwdev, zdma->sgt.sgl, zdma->sgt.nents,
		     DMA_FROM_DEVICE);

	__zio_dma_free_pool(zdma);
}
EXPORT_SYMBOL(zio_dma_unmap_sg);

#endif
/**
 * Notify zio about a DMA error so it can clean up ZIO structures,
 * free all blocks programmed for the DMA and the active_block
 * then, it raises the LOST_BLOCK alarm.
 */
void zio_dma_error(struct zio_dma_sgt *zdma)
{
	struct zio_channel *chan = zdma->chan;
	int i;

	/*
	 * If the DMA transfer fail, the current block is not valid
	 * then remove it and raise an alarm
	 */
	zio_buffer_free_block(chan->bi,	chan->active_block);
	chan->active_block = NULL;
	chan->current_ctrl->zio_alarms |= ZIO_ALARM_LOST_BLOCK;
}
EXPORT_SYMBOL(zio_dma_error);
