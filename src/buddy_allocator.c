#include "buddy_allocator.h"

#include "sys/mem/mem.h"

#include "util/ktypes.h"
#include "util/string.h"

#define NUM_LISTS 5

static char *base;     // address of the first managed page
static long num_pages; // number of pages managed by the allocator

// HELPERS

// Number of pages in each block on lists[list]: 1, 2, 4, 8 or 16
static long block_pages(int list) {
    long pages = 1;
    for (int i = 0; i < list; i++)
        pages *= 2;
    return pages;
}

// Index of the list whose blocks are the smallest that still hold size bytes
static int list_for_size(long size) {
    if (size <= 0 || size > MAX_CHUNK_SIZE)
        return -1;
    long pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    int list = 0;
    while (block_pages(list) < pages)
        list++;
    return list;
}

// Pages taken by the allocator's own bookkeeping
static long metadata_pages(long npages) {
    long bytes = sizeof(free_list_t) + NUM_LISTS * sizeof(chunk_t *) +
                 npages * sizeof(chunk_t);
    return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
}

static void free_list_push(int list, chunk_t *chunk) {
    chunk->next = free_list->lists[list];
    free_list->lists[list] = chunk;
}

static chunk_t *free_list_pop(int list) {
    chunk_t *chunk = free_list->lists[list];
    if (chunk != NULL)
        free_list->lists[list] = chunk->next;
    return chunk;
}

// Unlink chunk from lists[list]
static int free_list_remove(int list, chunk_t *chunk) {
    for (chunk_t **link = &free_list->lists[list]; *link != NULL;
         link = &(*link)->next) {
        if (*link == chunk) {
            *link = chunk->next;
            return 1;
        }
    }
    return 0;
}

/*
 * Metadata at the start of dram and every remaining page to
 * the free lists
 */
void buddy_init() {
    spinlock_init(&mm_buddy_mutex);

    // Start by managing every page, then give pages up to the metadata (which
    // needs one chunk_t per managed page) until both fit in memory
    long total_pages = mem_size / PAGE_SIZE;
    num_pages = total_pages;
    while (num_pages > 0 && metadata_pages(num_pages) + num_pages > total_pages)
        num_pages--;

    char *p = (char *)dram;
    free_list = (free_list_t *)p;
    p += sizeof(free_list_t);
    free_list->lists = (chunk_t **)p;
    p += NUM_LISTS * sizeof(chunk_t *);
    all_chunks = (chunk_t *)p;
    base = (char *)dram + metadata_pages(num_pages) * PAGE_SIZE;

    for (int list = 0; list < NUM_LISTS; list++)
        free_list->lists[list] = NULL;

    for (long page = 0; page < num_pages; page++) {
        all_chunks[page].addr = base + page * PAGE_SIZE;
        all_chunks[page].next = NULL;
    }

    // Split the pages into blocks
    long next_page = 0;
    for (int list = NUM_LISTS - 1; list >= 0; list--) {
        long size = block_pages(list);
        while (next_page + size <= num_pages) {
            free_list_push(list, &all_chunks[next_page]);
            next_page += size;
        }
    }
}

/*
 * Take a block from the smallest non-empty list that fits
 * Splitting it in half when needed
 */
void *buddy_obj_alloc(long size) {
    int wanted = list_for_size(size);
    if (wanted < 0)
        return NULL;

    spinlock_lock(&mm_buddy_mutex);

    int list = wanted;
    while (list < NUM_LISTS && free_list->lists[list] == NULL)
        list++;
    if (list == NUM_LISTS) {
        spinlock_unlock(&mm_buddy_mutex);
        return NULL;
    }

    chunk_t *chunk = free_list_pop(list);
    long first_page = chunk - all_chunks;

    // Each split keeps the first half and frees the second half
    while (list > wanted) {
        list--;
        free_list_push(list, &all_chunks[first_page + block_pages(list)]);
    }

    spinlock_unlock(&mm_buddy_mutex);
    return chunk->addr;
}

/*
 * Return a block to the free lists
 * Merge it with its buddy for as long as the buddy is free too
 */
void buddy_obj_free(long size, void *obj) {
    int list = list_for_size(size);
    char *addr = obj;
    if (list < 0 || addr < base || addr >= base + num_pages * PAGE_SIZE)
        return;

    // Ignore pointers the allocator could not have returned for this size
    long first_page = (addr - base) / PAGE_SIZE;
    if ((addr - base) % PAGE_SIZE != 0 || first_page % block_pages(list) != 0)
        return;

    spinlock_lock(&mm_buddy_mutex);

    while (list < NUM_LISTS - 1) {
        long size = block_pages(list);
        long buddy;
        if ((first_page / size) % 2 == 0)
            buddy = first_page + size;
        else
            buddy = first_page - size;

        if (buddy >= num_pages || !free_list_remove(list, &all_chunks[buddy]))
            break;

        // The merged block starts at whichever half comes first
        if (buddy < first_page)
            first_page = buddy;
        list++;
    }
    free_list_push(list, &all_chunks[first_page]);

    spinlock_unlock(&mm_buddy_mutex);
}

/*
 * Allocate npages contiguous pages
 */
void *page_alloc_n(int npages) {
    if (npages <= 0)
        return NULL;
    return buddy_obj_alloc((long)npages * PAGE_SIZE);
}

/*
 * Free pages from page_alloc_n
 */
void page_free_n(void *ptr, int npages) {
    if (npages <= 0)
        return;
    buddy_obj_free((long)npages * PAGE_SIZE, ptr);
}
