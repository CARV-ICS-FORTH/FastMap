#ifndef _BTREE_META_
#define _BTREE_META_

typedef enum
{
	leafNode = 590675399,
	internalNode = 790393380,
	rootNode = 742729384,
	leafRootNode = 748939994 /*special case for a newly created tree*/
} nodeType_t;

typedef struct block_header
{
	void *next_block;
} block_header;

/*leaf or internal node metadata, place always in the first 4KB data block*/
typedef struct node_header
{
	uint64_t epoch; /*epoch of the node. It will be used for knowing when to perform copy on write*/
	uint64_t fragmentation;
	/*data log info, KV log for leaves private for index*/
	block_header *first_key_block;
	block_header *last_key_block;
	uint64_t key_log_size;
	int32_t height;  /*0 are leaves, 1 are Bottom Internal nodes, and then we have INs and root*/
	nodeType_t type; /*internal or leaf node*/
	uint16_t numberOfEntriesInNode;
	char pad[8];
} node_header;

#endif // _BTREE_META_
