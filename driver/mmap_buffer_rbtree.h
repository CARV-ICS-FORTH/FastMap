#ifndef __MMAP_BUFFER_RBTREE_H
#define __MMAP_BUFFER_RBTREE_H

#include <linux/types.h>
#include <linux/rbtree.h>

#include "tagged_page.h"
#include "shared_defines.h"

int tagged_rb_insert(struct rb_root *root, struct tagged_page *data);
struct tagged_page *tagged_rb_search(struct rb_root *root, pgoff_t pgoff);
struct tagged_page *tagged_rb_greater_equal(struct rb_node *node, pgoff_t pgoff);
int tagged_rb_erase(struct rb_root *root, pgoff_t pgoff);

#endif // __MMAP_BUFFER_RBTREE_H
