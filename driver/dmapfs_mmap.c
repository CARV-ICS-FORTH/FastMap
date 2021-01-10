#include "wrapfs.h"

static ssize_t wrapfs_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
	/*
	 * This function should never be called directly.  We need it
	 * to exist, to get past a check in open_check_o_direct(),
	 * which is called from do_last().
	 */
	return -EINVAL;
}

const struct address_space_operations wrapfs_aops = {
	.direct_IO = wrapfs_direct_IO,
};
