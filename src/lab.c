#define _GNU_SOURCE
#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif

#include "lab.h"


#define handle_error_and_die(msg) \
    do                            \
    {                             \
        perror(msg);              \
        raise(SIGKILL);          \
    } while (0)

/**
 * @brief Convert bytes to the correct K value
 *
 * @param bytes the number of bytes
 * @return size_t the K value that will fit bytes
 */
size_t btok(size_t bytes)
{
    if (bytes == 0) return 0;
    size_t k = 0;
    size_t power = UINT64_C(1);

    while (power < bytes) {
        power <<= 1; // shift left by 1
        k++;
    }
    return k;
}

struct avail *buddy_calc(struct buddy_pool *pool, struct avail *buddy)
{
    // Calculate the offset of the current block from the base
    size_t baseOffset = (char *)buddy - (char *)pool->base;
    
    // Calculate the size of the current block (2^kval bytes)
    size_t blockSize = UINT64_C(1) << buddy->kval;
    
    // XOR the offset with the block size to get the buddy's offset
    size_t buddyOffset = baseOffset ^ blockSize;
    
    // Calculate the buddy's address by adding the offset to the base
    struct avail *buddyPtr = (struct avail *)((char *)pool->base + buddyOffset);
    
    return buddyPtr;
}

void *buddy_malloc(struct buddy_pool *pool, size_t size)
{
    if (size == 0 || pool == NULL)
    {
        return NULL;
    }

    //////get the kval for the requested size with enough room for the tag and kval fields
    // Calculate the required size including the header
    size_t totalSize = size + sizeof(struct avail);

    // Get the smallest kval that fits the total size
    size_t kval = btok(totalSize);

    printf("size=%zu, total_size=%zu, kval=%zu, SMALLEST_K=%d, pool->kval_m=%zu\n", 
        size, totalSize, kval, SMALLEST_K, pool->kval_m);

    printf("size=%zu, total_size=%zu, kval=%zu, SMALLEST_K=%d, pool->kval_m=%zu\n", 
        size, totalSize, kval, SMALLEST_K, pool->kval_m);

    if (kval < SMALLEST_K) {
        kval = SMALLEST_K; // Enforce minimum block size
    }
    if (kval > pool->kval_m) {
        errno = ENOMEM; // Request exceeds pool size
        return NULL; // Request exceeds pool size    
    }


    /////R1 Find a block
    // Find the smallest available block that’s large enough
    size_t currentK = kval;
    struct avail *block = NULL;
    while (currentK <= pool->kval_m) {
        if (pool->avail[currentK].next != &pool->avail[currentK]) {
            // Found a block in the list
            block = pool->avail[currentK].next;
            printf("Found block at k=%zu, block->kval=%zu\n", currentK, (size_t)block->kval);
            // printf("Found block at k=%zu, block->kval=%zu\n", currentK, block->kval);
            break;
        }
        currentK++;
    }



    ////There was not enough memory to satisfy the request thus we need to set error and return NULL
    // No block found
    if (block == NULL) {
        errno = ENOMEM; // No available block
        return NULL;    
    }

    ////R2 Remove from list;
    // Remove the block from its current list
    block->prev->next = block->next;
    block->next->prev = block->prev;

    ////R3 Split required?
    // Split the block if it’s too large
    while (block->kval > kval) {
        printf("Splitting: block->kval=%zu to %zu\n", (size_t)block->kval, (size_t)(block->kval - 1));
        //printf("Splitting: block->kval=%zu to %zu\n", block->kval, block->kval - 1);
        ////R4 Split the block
        // Reduce the block’s size by 1 (halving it)
        block->kval--;
        size_t newSize = UINT64_C(1) << block->kval;

        // Create a new buddy block
        struct avail *buddy = (struct avail *)((char *)block + newSize);
        buddy->tag = BLOCK_AVAIL;
        buddy->kval = block->kval;

        // Add the buddy to the appropriate availability list
        struct avail *list_head = &pool->avail[buddy->kval];
        buddy->next = list_head->next;
        buddy->prev = list_head;
        list_head->next->prev = buddy;
        list_head->next = buddy;
    }

    // Mark the block as reserved
    block->tag = BLOCK_RESERVED;

    printf("Returning block with kval=%zu\n", (size_t)block->kval);
    // printf("Returning block with kval=%zu\n", block->kval);

    // Return pointer to user memory (after the header)
    return (void *)((char *)block + sizeof(struct avail));
}

void buddy_free(struct buddy_pool *pool, void *ptr)
{
    if (ptr == NULL) return;

    struct avail *block = (struct avail *)((char *)ptr - sizeof(struct avail));
    if (block->tag != BLOCK_RESERVED) return;

    block->tag = BLOCK_AVAIL;
    size_t current_k = block->kval;

    while (current_k < pool->kval_m) {
        struct avail *buddy = buddy_calc(pool, block);
        
        // Check if buddy is valid and available
        if ((char *)buddy >= (char *)pool->base + pool->numbytes ||
            buddy->tag != BLOCK_AVAIL || 
            buddy->kval != current_k) {
            break;
        }

        // Remove buddy from its list
        buddy->prev->next = buddy->next;
        buddy->next->prev = buddy->prev;

        // Use the lower address as the new block
        block = (block < buddy) ? block : buddy;
        current_k++;
        block->kval = current_k;
    }

    // Add the block to its availability list
    struct avail *list_head = &pool->avail[current_k];
    block->next = list_head->next;
    block->prev = list_head;
    list_head->next->prev = block;
    list_head->next = block;
}

/**
 * @brief This is a simple version of realloc.
 *
 * @param poolThe memory pool
 * @param ptr  The user memory
 * @param size the new size requested
 * @return void* pointer to the new user memory
 */
void *buddy_realloc(struct buddy_pool *pool, void *ptr, size_t size)
{
    //Required for Grad Students
    //Optional for Undergrad Students
    printf("buddy_realloc not implemented\n");
    return NULL;
}

void buddy_init(struct buddy_pool *pool, size_t size)
{
    size_t kval = 0;
    if (size == 0)
        kval = DEFAULT_K;
    else
        kval = btok(size);

    if (kval < MIN_K)
        kval = MIN_K;
    if (kval > MAX_K)
        kval = MAX_K - 1;

    //make sure pool struct is cleared out
    memset(pool,0,sizeof(struct buddy_pool));
    pool->kval_m = kval;
    pool->numbytes = (UINT64_C(1) << pool->kval_m);
    //Memory map a block of raw memory to manage
    pool->base = mmap(
        NULL,                               /*addr to map to*/
        pool->numbytes,                     /*length*/
        PROT_READ | PROT_WRITE,             /*prot*/
        MAP_PRIVATE | MAP_ANONYMOUS,        /*flags*/
        -1,                                 /*fd -1 when using MAP_ANONYMOUS*/
        0                                   /* offset 0 when using MAP_ANONYMOUS*/
    );
    if (MAP_FAILED == pool->base)
    {
        handle_error_and_die("buddy_init avail array mmap failed");
    }

    //Set all blocks to empty. We are using circular lists so the first elements just point
    //to an available block. Thus the tag, and kval feild are unused burning a small bit of
    //memory but making the code more readable. We mark these blocks as UNUSED to aid in debugging.
    for (size_t i = 0; i <= kval; i++)
    {
        pool->avail[i].next = pool->avail[i].prev = &pool->avail[i];
        pool->avail[i].kval = i;
        pool->avail[i].tag = BLOCK_UNUSED;
    }

    //Add in the first block
    pool->avail[kval].next = pool->avail[kval].prev = (struct avail *)pool->base;
    struct avail *m = pool->avail[kval].next;
    m->tag = BLOCK_AVAIL;
    m->kval = kval;
    m->next = m->prev = &pool->avail[kval];
}

void buddy_destroy(struct buddy_pool *pool)
{
    int rval = munmap(pool->base, pool->numbytes);
    if (-1 == rval)
    {
        handle_error_and_die("buddy_destroy avail array");
    }
    //Zero out the array so it can be reused it needed
    memset(pool,0,sizeof(struct buddy_pool));
}

#define UNUSED(x) (void)x

/**
 * This function can be useful to visualize the bits in a block. This can
 * help when figuring out the buddy_calc function!
 */
static void printb(unsigned long int b)
{
     size_t bits = sizeof(b) * 8;
     unsigned long int curr = UINT64_C(1) << (bits - 1);
     for (size_t i = 0; i < bits; i++)
     {
          if (b & curr)
          {
               printf("1");
          }
          else
          {
               printf("0");
          }
          curr >>= 1L;
     }
}
