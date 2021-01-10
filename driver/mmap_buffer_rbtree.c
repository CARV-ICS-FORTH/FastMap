#include "mmap_buffer_rbtree.h"
#include "shared_defines.h"

#ifndef USE_RADIX_TREE_FOR_DIRTY
struct tagged_page *tagged_rb_search(struct rb_root *root, pgoff_t pgoff)
{
	struct rb_node *node;

	node = root->rb_node;
	while(node){
		struct tagged_page *data = container_of(node, struct tagged_page, node);

		if(pgoff < data->page->index)
			node = node->rb_left;
		else if(pgoff > data->page->index)
			node = node->rb_right;
		else 
			return data;
	}

	return NULL;
}

struct tagged_page *tagged_rb_greater_equal(struct rb_node *node, pgoff_t pgoff)
{
	struct tagged_page *data;

	if(node == NULL)
		return NULL;

	data = container_of(node, struct tagged_page, node);
	if(data->page->index == pgoff)
		return data;

	if(node->rb_left == NULL && node->rb_right == NULL)
		return (data->page->index > pgoff)?(data):(NULL);

	if(node->rb_left == NULL){
		if(data->page->index > pgoff)
			return data;
		else if(data->page->index < pgoff)
			return tagged_rb_greater_equal(node->rb_right, pgoff);
	}

	if(node->rb_right == NULL){
		if(data->page->index > pgoff)
			return tagged_rb_greater_equal(node->rb_left, pgoff);
		else if(data->page->index < pgoff)
			return NULL;
	}

	/* here the node has 2 children */
	if(data->page->index > pgoff)
		return tagged_rb_greater_equal(node->rb_left, pgoff);
	else if(data->page->index < pgoff)
		return tagged_rb_greater_equal(node->rb_right, pgoff);

	return NULL;
}

int tagged_rb_insert(struct rb_root *root, struct tagged_page *data)
{
	struct rb_node **new, *parent;

	new = &(root->rb_node);
	parent = NULL;

	/* Figure out where to put new node */
	while(*new){
		struct tagged_page *this = container_of(*new, struct tagged_page, node);

		parent = *new;
		if(data->page->index < this->page->index)
			new = &((*new)->rb_left);
		else if(data->page->index > this->page->index)
			new = &((*new)->rb_right);
		else 
			return 0; // false
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&data->node, parent, new);
	rb_insert_color(&data->node, root);

	return 1; // true
}

int tagged_rb_erase(struct rb_root *root, pgoff_t pgoff)
{
	struct tagged_page *data;

	data = tagged_rb_search(root, pgoff);
	if(data){
		rb_erase(&data->node, root);
		return 1; // true
	}
	return 0; // false
}
#endif /* USE_RADIX_TREE_FOR_DIRTY */
