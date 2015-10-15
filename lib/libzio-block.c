/*
 * Copyright (c) 2012 Federico Vaga
 * Author: Federico Vaga <federico.vaga@gmail.com>
 * License: GPL v3
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "libzio.h"

/**
 * According to the parameters it reads or writes a control
 *
 * @param[in] uchan channel where get the block
 * @param[in,out] ctrl on write is the destination, on read is the source
 * @param[in] flags open(2) flags
 * @return 0 on success. Otherwise -1 and errno is appropriately set
 */
static int _uzio_block_ctrl_rw(struct uzio_channel *uchan,
			       struct zio_control *ctrl)
{
	struct uzio_cset *cset = container_of(uchan->head.parent,
					      struct uzio_cset, head);
	int i;

	/* Read data */
	if (cset->flags & UZIO_CSET_FLAG_DIRECTION)
		i = read(uchan->fd_ctrl, ctrl, __ZIO_CONTROL_SIZE);
	else
		i = write(uchan->fd_ctrl, ctrl, __ZIO_CONTROL_SIZE);
	switch (i) {
	case -1:
	        return -1;
	default:
		errno = EUZIOBLKCTRLWRONG;
	        return -1;
	case __ZIO_CONTROL_SIZE:
	        return 0;
	}
}


/**
 * It returns a partial zio block, only the control side
 *
 * @param[in] uchan channel where get the block
 * @param[out] ctrl where write the control
 * @return 0 on success. Otherwise -1 and errno is appropriately set
 */
int uzio_block_ctrl_read_raw(struct uzio_channel *uchan,
			     struct zio_control *ctrl)
{
	struct uzio_cset *cset = container_of(uchan->head.parent,
					      struct uzio_cset, head);

	/* Check if it is an input channel */
	if (cset->flags & UZIO_CSET_FLAG_DIRECTION) {
		errno = EUZIOBLKDIRECTION;
		return -1;
	}

	return _uzio_block_ctrl_rw(uchan, ctrl);
}


/**
 * It sendo to char device a partial zio block, only the control side
 *
 * @param[in] uchan channel where write the block
 * @param[out] ctrl control to write
 * @return 0 on success. Otherwise -1 and errno is appropriately set
 */
int uzio_block_ctrl_write_raw(struct uzio_channel *uchan,
			      struct zio_control *ctrl)
{
	struct uzio_cset *cset = container_of(uchan->head.parent,
					      struct uzio_cset, head);

	/* Check if it is an output channel */
	if (!(cset->flags & UZIO_CSET_FLAG_DIRECTION)) {
		errno = EUZIOBLKDIRECTION;
		return -1;
	}

	return _uzio_block_ctrl_rw(uchan, ctrl);
}


/**
 * It returns a partial zio block, only the data side
 *
 * @param[in] uchan channel where get the block
 * @param[in] datalen number of byte to acquire from char device
 * @return a valid pointer to the data buffer. Otherwise NULL and errno is
 *         appropriately set
 */
int uzio_block_data_read_raw(struct uzio_channel *uchan, void *data,
			     size_t datalen)
{
	struct uzio_cset *cset = container_of(uchan->head.parent,
					      struct uzio_cset, head);
	int i;

	/* Check if it is an input channel */
	if (cset->flags & UZIO_CSET_FLAG_DIRECTION) {
		errno = EUZIOBLKDIRECTION;
		return -1;
	}
	if (!data) {
		errno = EUZIOIDATA;
		return -1;
	}

        i = read(uchan->fd_data, data, datalen);
	return i < 0 ? -1 : 0;
}


/**
 * It writes a partial zio block, only the data side. Use the function
 * uzio_block_ctrl_write() to write the control side
 *
 * @param[in] uchan channel where write the block
 * @param[in] datalen number of byte to write to char device
 * @param[in] data bytes to write
 * @return number of written bytes. Otherwise -1 and errno is appropriately set
 */
int uzio_block_data_write_raw(struct uzio_channel *uchan, void *data,
			      size_t datalen)
{
	struct uzio_cset *cset = container_of(uchan->head.parent,
					      struct uzio_cset, head);

	/* Check if it is an output channel */
	if (!(cset->flags & UZIO_CSET_FLAG_DIRECTION)) {
		errno = EUZIOBLKDIRECTION;
		return -1;
	}

        return write(uchan->fd_data, data, datalen);
}


/**
 * It returns a complete zio block from a given channel. The block must
 * be free using uzio_block_free(). In order, it reads the control and
 * than the data side. If you need special behaviour feel free to use
 * uzio_block_ctrl_read_raw() and uzio_block_data_read_raw() functions
 *
 * @param[in] uchan channel where get the block
 * @return it returns a valid block, NULL on error and errno is set
 *         appropriately
 */
struct uzio_block *uzio_block_read(struct uzio_channel *uchan)
{
	struct uzio_cset *cset = container_of(uchan->head.parent,
					      struct uzio_cset, head);
	struct uzio_block *block;
	int err;

	/* Check if it is an input channel */
	if (cset->flags & UZIO_CSET_FLAG_DIRECTION) {
		errno = EUZIOBLKDIRECTION;
		return NULL;
	}

	block = malloc(sizeof(struct uzio_block));
	if (!block)
		goto out;
	/* Get the control */
	err = uzio_block_ctrl_read_raw(uchan, &block->ctrl);
	if (err)
		goto out_ctrl;
	/* Get data */
	block->datalen = block->ctrl.nsamples * block->ctrl.ssize;
	block->data = malloc(block->datalen);
	if (!block->data)
	        goto out_data_alloc;

        err = uzio_block_data_read_raw(uchan, block->data, block->datalen);
	if (err)
		goto out_data;

	return block;

out_data:
	free(block->data);
out_data_alloc:
out_ctrl:
	free(block);
out:
	return NULL;
}


/**
 * It writes a block into a given channel buffer. In order, it writes
 * the control and than the data side. If you need special behaviour
 * feel free to use uzio_block_ctrl_write_raw() and
 * uzio_block_data_write_raw() functions
 *
 * @param[in] chan channel to write
 * @param[in] block zio block to write
 * @return 0 on success, -1 on error and errno is appropriately set
 */
int uzio_block_write(struct uzio_channel *chan, struct uzio_block *block)
{
	struct uzio_cset *cset = container_of(chan->head.parent,
					      struct uzio_cset, head);
	int err;

	/* Check if it is an output channel */
	if (!(cset->flags & UZIO_CSET_FLAG_DIRECTION)) {
		errno = EUZIOBLKDIRECTION;
		return -1;
	}

	err = uzio_block_ctrl_write_raw(chan, &block->ctrl);
	if (err)
		return err;
	return uzio_block_data_write_raw(chan, block->data, block->datalen);
}


/**
 * It allocates a ZIO block
 * @param[in] datalen data size lenght
 * @return on success it returns an empty ZIO block, NULL on error and
 *         errno is appropriately set
 */
struct uzio_block *uzio_block_alloc(size_t datalen)
{
	struct uzio_block *block;

	block = malloc(sizeof(struct uzio_block));
	if (!block)
		return NULL;
	block->datalen = datalen;
	block->data = malloc(block->datalen);
	if (!block->data) {
		free(block);
		return NULL;
	}

	return block;
}


/**
 * It releases block resources
 *
 * @param[in] block the block to free
 */
void uzio_block_free(struct uzio_block *block)
{
	free(block->data);
	free(block);
}
