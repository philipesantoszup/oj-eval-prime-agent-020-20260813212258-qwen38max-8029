#ifndef OS_MM_H
#define OS_MM_H
#define MAX_ERRNO 4095

#define OK          0
#define EINVAL      22  /* Invalid argument */    
#define ENOSPC      28  /* No page left */  


#define IS_ERR_VALUE(x) ((x) >= (unsigned long)-MAX_ERRNO)
static inline void *ERR_PTR(long error) { return (void *)error; }
/* PTR_ERR as a macro so it accepts both pointers and integer return values
 * (e.g. PTR_ERR(return_pages(...))) on strict compilers (GCC >= 14 treats
 * implicit int-to-pointer conversion as an error). */
#define PTR_ERR(x) ((long)(unsigned long)(x))
static inline long IS_ERR(const void *ptr) { return IS_ERR_VALUE((unsigned long)ptr); }


int init_page(void *p, int pgcount);
void *alloc_pages(int rank);
int return_pages(void *p);
int query_ranks(void *p);
int query_page_counts(int rank);

#endif