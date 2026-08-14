#include <stdlib.h>

#include "buddy.h"

#define PAGE_SHIFT (12)
#define PAGE_SIZE (1u << PAGE_SHIFT)
#define MAX_RANK (16)
#define MAX_BLOCK_PAGES (1 << (MAX_RANK - 1)) /* number of pages in a rank-16 block */

/*
 * Managed region: total_pages consecutive 4K pages starting at base_addr.
 * total_pages is the largest power of two not exceeding the pgcount passed
 * to init_page (the buddy system needs a power-of-two sized pool; any
 * remaining pages are wasted, as stated in the problem description).
 *
 * A block of rank r consists of 2^(r-1) pages.  The biggest block we keep
 * is rank MAX_RANK, so a pool larger than 2^15 pages is managed as several
 * rank-16 blocks.
 */

static unsigned char *base_addr = NULL; /* first managed byte           */
static int total_pages = 0;             /* managed pages (power of two) */

static int *blk_rank = NULL;  /* blk_rank[i] = rank r if page i is the head of a
                                 block (free or allocated), 0 otherwise          */
static unsigned char *blk_alloc = NULL; /* 1 if the block headed by i is allocated */

/* Per-rank free lists: doubly linked lists of page indices kept sorted in
 * ascending address order so that allocation is lowest-address-first. */
static int *fl_prev = NULL, *fl_next = NULL;
static int fl_head[MAX_RANK + 1];
static int fl_tail[MAX_RANK + 1];
static int fl_count[MAX_RANK + 1];

static void fl_remove(int rank, int idx) {
    int p = fl_prev[idx], n = fl_next[idx];
    if (p != -1)
        fl_next[p] = n;
    else
        fl_head[rank] = n;
    if (n != -1)
        fl_prev[n] = p;
    else
        fl_tail[rank] = p;
    fl_prev[idx] = fl_next[idx] = -1;
    fl_count[rank]--;
}

/* insert idx into the sorted free list of the given rank */
static void fl_insert(int rank, int idx) {
    int cur = fl_tail[rank];
    if (cur == -1) { /* empty list */
        fl_head[rank] = fl_tail[rank] = idx;
        fl_prev[idx] = fl_next[idx] = -1;
    } else if (cur < idx) { /* append to tail (common case) */
        fl_next[cur] = idx;
        fl_prev[idx] = cur;
        fl_next[idx] = -1;
        fl_tail[rank] = idx;
    } else { /* walk backwards from the tail until we find the position */
        while (cur != -1 && cur > idx) cur = fl_prev[cur];
        if (cur == -1) { /* new head */
            fl_prev[idx] = -1;
            fl_next[idx] = fl_head[rank];
            fl_prev[fl_head[rank]] = idx;
            fl_head[rank] = idx;
        } else { /* insert after cur */
            fl_prev[idx] = cur;
            fl_next[idx] = fl_next[cur];
            if (fl_next[cur] != -1)
                fl_prev[fl_next[cur]] = idx;
            else
                fl_tail[rank] = idx;
            fl_next[cur] = idx;
        }
    }
    fl_count[rank]++;
}

/* rank of a block that holds `pages` pages (pages must be a power of two) */
static int rank_of_pages(int pages) {
    int r = 1;
    while ((1 << (r - 1)) < pages) r++;
    return r;
}

int init_page(void *p, int pgcount) {
    int managed, top_pages, top_rank, off, r;

    if (p == NULL || pgcount <= 0) return -EINVAL;

    /* largest power of two not exceeding pgcount */
    managed = 1;
    while ((managed << 1) <= pgcount) managed <<= 1;

    /* (re)allocate metadata */
    if (blk_rank != NULL) {
        free(blk_rank);
        free(blk_alloc);
        free(fl_prev);
        free(fl_next);
        blk_rank = NULL;
        blk_alloc = NULL;
        fl_prev = fl_next = NULL;
    }
    blk_rank = (int *)calloc((size_t)managed, sizeof(int));
    blk_alloc = (unsigned char *)calloc((size_t)managed, 1);
    fl_prev = (int *)malloc((size_t)managed * sizeof(int));
    fl_next = (int *)malloc((size_t)managed * sizeof(int));
    if (blk_rank == NULL || blk_alloc == NULL || fl_prev == NULL ||
        fl_next == NULL)
        return -EINVAL;

    base_addr = (unsigned char *)p;
    total_pages = managed;

    for (r = 1; r <= MAX_RANK; r++) {
        fl_head[r] = fl_tail[r] = -1;
        fl_count[r] = 0;
    }

    /* split the pool into the biggest allowed blocks (rank <= MAX_RANK) */
    top_pages = managed < MAX_BLOCK_PAGES ? managed : MAX_BLOCK_PAGES;
    top_rank = rank_of_pages(top_pages);
    for (off = 0; off < managed; off += top_pages) {
        blk_rank[off] = top_rank;
        blk_alloc[off] = 0;
        fl_insert(top_rank, off);
    }

    return OK;
}

void *alloc_pages(int rank) {
    int s, idx, buddy;

    if (rank < 1 || rank > MAX_RANK) return ERR_PTR(-EINVAL);
    if (base_addr == NULL) return ERR_PTR(-ENOSPC);

    /* find the smallest free block big enough */
    s = rank;
    while (s <= MAX_RANK && fl_count[s] == 0) s++;
    if (s > MAX_RANK) return ERR_PTR(-ENOSPC);

    /* take the lowest-address block of that rank */
    idx = fl_head[s];
    fl_remove(s, idx);

    /* split it down to the requested rank, freeing the upper halves */
    while (s > rank) {
        s--;
        buddy = idx + (1 << (s - 1)); /* upper half */
        blk_rank[idx] = s;
        blk_rank[buddy] = s;
        blk_alloc[idx] = 0;
        blk_alloc[buddy] = 0;
        fl_insert(s, buddy);
    }

    blk_rank[idx] = rank;
    blk_alloc[idx] = 1;
    return (void *)(base_addr + ((size_t)idx << PAGE_SHIFT));
}

int return_pages(void *p) {
    unsigned char *cp = (unsigned char *)p;
    size_t off;
    int idx, r, buddy, lo, hi;

    if (cp == NULL || base_addr == NULL || cp < base_addr) return -EINVAL;
    off = (size_t)(cp - base_addr);
    if (off & (PAGE_SIZE - 1)) return -EINVAL; /* not page aligned */
    if ((off >> PAGE_SHIFT) >= (size_t)total_pages) return -EINVAL;

    idx = (int)(off >> PAGE_SHIFT);
    r = blk_rank[idx];
    if (r == 0 || !blk_alloc[idx]) return -EINVAL; /* not an allocated head */

    blk_alloc[idx] = 0;

    /* coalesce with the buddy as long as possible */
    while (r < MAX_RANK) {
        buddy = idx ^ (1 << (r - 1));
        if (buddy >= total_pages) break;
        if (blk_rank[buddy] != r || blk_alloc[buddy]) break; /* buddy busy */
        fl_remove(r, buddy);
        lo = idx < buddy ? idx : buddy;
        hi = idx < buddy ? buddy : idx;
        blk_rank[hi] = 0; /* hi is no longer a block head */
        blk_alloc[hi] = 0;
        idx = lo;
        r++;
    }

    blk_rank[idx] = r;
    blk_alloc[idx] = 0;
    fl_insert(r, idx);
    return OK;
}

int query_ranks(void *p) {
    unsigned char *cp = (unsigned char *)p;
    size_t off;
    int idx, r, head;

    if (cp == NULL || base_addr == NULL || cp < base_addr) return -EINVAL;
    off = (size_t)(cp - base_addr);
    if (off & (PAGE_SIZE - 1)) return -EINVAL;
    if ((off >> PAGE_SHIFT) >= (size_t)total_pages) return -EINVAL;

    idx = (int)(off >> PAGE_SHIFT);

    /* the block of rank r containing page idx starts at
     * idx rounded down to a multiple of 2^(r-1) */
    for (r = 1; r <= MAX_RANK; r++) {
        head = idx & ~((1 << (r - 1)) - 1);
        if (blk_rank[head] == r) return r;
    }
    return -EINVAL; /* unreachable for a consistent state */
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;
    return fl_count[rank];
}
