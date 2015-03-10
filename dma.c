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
	/* Prepare the transfers pool area */
	zdma->page_desc_size = page_desc_size;
	zdma->page_desc_pool_size = zdma->page_desc_size * zdma->sgt.nents;
	zdma->page_desc_pool = kzalloc(zdma->page_desc_pool_size,
				       GFP_ATOMIC | GFP_DMA);
	if (!zdma->page_desc_pool) {
		dev_err(zdma->hwdev, "cannot allocate coherent dma memory\n");
		return -ENOMEM;
	}

	/* Allocate a DMA area to store the DMA transfers */
	zdma->dma_page_desc_pool = dma_map_single(zdma->hwdev,
						  zdma->page_desc_pool,
						  zdma->page_desc_pool_size,
						  DMA_TO_DEVICE);
	if (!zdma->dma_page_desc_pool) {
		kfree(zdma->page_desc_pool);
		return -ENOMEM;
	}

	pr_debug("%s:%d DMA transfer pool allocated for max %d transfers\n",
		 __func__, __LINE__, zdma->sgt.nents);
	return 0;
}


/**
 * It free a DMA area for the transfers pool
 * @param[in] zdma zio DMA descriptor from zio_dma_alloc_sg()
 */
static void __zio_dma_free_pool(struct zio_dma_sgt *zdma)
{
       dma_unmap_single(zdma->hwdev, zdma->dma_page_desc_pool,
                        zdma->page_desc_pool_size, DMA_TO_DEVICE);
       kfree(zdma->page_desc_pool);

       /* Clear data */
       zdma->page_desc_pool_size = 0;
       zdma->dma_page_desc_pool = 0;
       zdma->page_desc_pool = NULL;
}


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
	void *item_ptr;

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

		item_ptr = zdma->page_desc_pool + (zdma->page_desc_size * i);

 		/* Configure hardware pages */
		zsg.zsgt = zdma;
		zsg.sg = sg;
		zsg.dev_mem_off = dev_mem_off;
		zsg.page_desc = item_ptr;
		zsg.block_idx = i_blk;
		zsg.page_idx= i;
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
