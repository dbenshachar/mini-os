typedef unsigned long size_t;

#define HEAP_SIZE 10000
static unsigned char heap_pool[HEAP_SIZE];

typedef struct Block {
	size_t size;
	int free;
	struct Block *next;
} Block;

static Block *freeList = (Block *)heap_pool;

void init_malloc() {
	freeList->size = HEAP_SIZE - sizeof(Block);
	freeList->free = 1;
	freeList->next = nullptr;
}

void *malloc(size_t size) {
	Block *curr = freeList;
	while (curr) {
		if (curr->free && curr->size >= size) {
			if (curr->size > size + sizeof(Block)) {
				Block *new_block = (Block *)((unsigned char *)curr + sizeof(Block) + size);
				new_block->size = curr->size - size - sizeof(Block);
				new_block->free = 1;
				new_block->next = curr->next;
				curr->size = size;
				curr->next = new_block;
			}
			curr->free = 0;
			return (void *)((unsigned char *)curr + sizeof(Block));
		}
		curr = curr->next;
	}
	return nullptr;
}

void free(void *ptr) {
	if (!ptr) return;
	Block *block = (Block *)((unsigned char *)ptr - sizeof(Block));
	block->free = 1;
	
	Block *curr = freeList;
	while (curr) {
		if (curr->free && curr->next && curr->next->free) {
			curr->size += sizeof(Block) + curr->next->size;
			curr->next = curr->next->next;
		}
		curr = curr->next;
	}
}