#include <assert.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif
#include "harness/unity.h"
#include "../src/lab.h"

static struct buddy_pool test_pool;

void setUp(void) {
  buddy_init(&test_pool, UINT64_C(1) << MIN_K);
  assert(test_pool.base != MAP_FAILED);
  assert(test_pool.kval_m == MIN_K);
}

void tearDown(void) {
  if (test_pool.base != (void *)-1 && test_pool.base != NULL) {
    buddy_destroy(&test_pool);
  }
  memset(&test_pool, 0, sizeof(struct buddy_pool));
}



/**
 * Check the pool to ensure it is full.
 */
void check_buddy_pool_full(struct buddy_pool *pool)
{
  //A full pool should have all values 0-(kval-1) as empty
  for (size_t i = 0; i < pool->kval_m; i++)
    {
      assert(pool->avail[i].next == &pool->avail[i]);
      assert(pool->avail[i].prev == &pool->avail[i]);
      assert(pool->avail[i].tag == BLOCK_UNUSED);
      assert(pool->avail[i].kval == i);
    }

  //The avail array at kval should have the base block
  assert(pool->avail[pool->kval_m].next->tag == BLOCK_AVAIL);
  assert(pool->avail[pool->kval_m].next->next == &pool->avail[pool->kval_m]);
  assert(pool->avail[pool->kval_m].prev->prev == &pool->avail[pool->kval_m]);

  //Check to make sure the base address points to the starting pool
  //If this fails either buddy_init is wrong or we have corrupted the
  //buddy_pool struct.
  assert(pool->avail[pool->kval_m].next == pool->base);
}

/**
 * Check the pool to ensure it is empty.
 */
void check_buddy_pool_empty(struct buddy_pool *pool)
{
  //An empty pool should have all values 0-(kval) as empty
  for (size_t i = 0; i <= pool->kval_m; i++)
    {
      assert(pool->avail[i].next == &pool->avail[i]);
      assert(pool->avail[i].prev == &pool->avail[i]);
      assert(pool->avail[i].tag == BLOCK_UNUSED);
      assert(pool->avail[i].kval == i);
    }
}

/**
 * Test allocating 1 byte to make sure we split the blocks all the way down
 * to MIN_K size. Then free the block and ensure we end up with a full
 * memory pool again
 */
void test_buddy_malloc_one_byte(void)
{
  fprintf(stderr, "->Test allocating and freeing 1 byte\n");
  struct buddy_pool pool;
  int kval = MIN_K;
  size_t size = UINT64_C(1) << kval;
  buddy_init(&pool, size);
  void *mem = buddy_malloc(&pool, 1);
  //Make sure correct kval was allocated
  buddy_free(&pool, mem);
  check_buddy_pool_full(&pool);
  buddy_destroy(&pool);
}

/**
 * Tests the allocation of one massive block that should consume the entire memory
 * pool and makes sure that after the pool is empty we correctly fail subsequent calls.
 */
void test_buddy_malloc_one_large(void)
{
  fprintf(stderr, "->Testing size that will consume entire memory pool\n");
  struct buddy_pool pool;
  size_t bytes = UINT64_C(1) << MIN_K;
  buddy_init(&pool, bytes);

  //Ask for an exact K value to be allocated. This test makes assumptions on
  //the internal details of buddy_init.
  size_t ask = bytes - sizeof(struct avail);
  void *mem = buddy_malloc(&pool, ask);
  assert(mem != NULL);

  //Move the pointer back and make sure we got what we expected
  struct avail *tmp = (struct avail *)mem - 1;
  assert(tmp->kval == MIN_K);
  assert(tmp->tag == BLOCK_RESERVED);
  check_buddy_pool_empty(&pool);

  //Verify that a call on an empty tool fails as expected and errno is set to ENOMEM.
  void *fail = buddy_malloc(&pool, 5);
  assert(fail == NULL);
  assert(errno = ENOMEM);

  //Free the memory and then check to make sure everything is OK
  buddy_free(&pool, mem);
  check_buddy_pool_full(&pool);
  buddy_destroy(&pool);
}

/**
 * Tests to make sure that the struct buddy_pool is correct and all fields
 * have been properly set kval_m, avail[kval_m], and base pointer after a
 * call to init
 */
void test_buddy_init(void)
{
  fprintf(stderr, "->Testing buddy init\n");
  //Loop through all kval MIN_k-DEFAULT_K and make sure we get the correct amount allocated.
  //We will check all the pointer offsets to ensure the pool is all configured correctly
  for (size_t i = MIN_K; i <= DEFAULT_K; i++)
    {
      size_t size = UINT64_C(1) << i;
      struct buddy_pool pool;
      buddy_init(&pool, size);
      check_buddy_pool_full(&pool);
      buddy_destroy(&pool);
    }
}

void test_btok(void) {
  assert(btok(0) == 0);          // edge case
  assert(btok(1) == 0);          // 2^0 = 1
  assert(btok(1024) == 10);      // 2^10 = 1024
  assert(btok(1000) == 10);      // rounds up to next power of 2
  assert(btok(2048) == 11);
  assert(btok(4096) == 12);
}



void test_malloc_null_and_zero(void) {
  assert(buddy_malloc(NULL, 64) == NULL);

  struct buddy_pool pool;
  buddy_init(&pool, 1024);

  assert(buddy_malloc(&pool, 0) == NULL); // can't malloc 0 bytes

  buddy_destroy(&pool);
}

void test_simple_malloc_and_free(void) {
  void* ptr = buddy_malloc(&test_pool, 1);
  assert(ptr != NULL);
  buddy_free(&test_pool, ptr);
  buddy_destroy(&test_pool);
}



void test_double_free_and_invalid_free(void) {
  void *mem = buddy_malloc(&test_pool, 1);
  buddy_free(&test_pool, mem);
  buddy_free(&test_pool, mem); // Double free (should do nothing)
  check_buddy_pool_full(&test_pool);
  mem = buddy_malloc(&test_pool, 1);
  struct avail *block = (struct avail *)((char *)mem - sizeof(struct avail));
  block->tag = BLOCK_AVAIL; // Invalid
  buddy_free(&test_pool, mem); // Should return
  block->tag = BLOCK_RESERVED;
  buddy_free(&test_pool, mem);
  check_buddy_pool_full(&test_pool);
}

void test_coalescing_on_free(void) {
  void *mem1 = buddy_malloc(&test_pool, 1); 
  void *mem2 = buddy_malloc(&test_pool, 1); 
  buddy_free(&test_pool, mem1);
  assert(test_pool.avail[SMALLEST_K].next != &test_pool.avail[SMALLEST_K]); // Partial
  buddy_free(&test_pool, mem2);
  check_buddy_pool_full(&test_pool);
}

void test_buddy_calc(void)
{
    fprintf(stderr, "->Testing buddy_calc\n");
    struct buddy_pool pool;
    buddy_init(&pool, UINT64_C(1) << MIN_K); // 1 MiB pool

    // Allocate a small block to split the pool
    void *mem = buddy_malloc(&pool, 1);
    struct avail *block = (struct avail *)((char *)mem - sizeof(struct avail));
    struct avail *buddy = buddy_calc(&pool, block);

    // Check buddy properties (kval=6, offset by 2^6 = 64 bytes)
    assert(buddy->kval == block->kval);
    assert((char *)buddy == (char *)block + (UINT64_C(1) << block->kval));
    assert(buddy->tag == BLOCK_AVAIL); // Should be the split buddy

    buddy_free(&pool, mem);
    buddy_destroy(&pool);
}

void test_buddy_free_null(void)
{
    fprintf(stderr, "->Testing buddy_free with NULL\n");
    buddy_free(&test_pool, NULL);
    check_buddy_pool_full(&test_pool);
}

void test_buddy_free_invalid(void)
{
    fprintf(stderr, "->Testing buddy_free with invalid block\n");
    struct buddy_pool pool;
    buddy_init(&pool, UINT64_C(1) << MIN_K);
    void *mem = buddy_malloc(&pool, 1);
    struct avail *block = (struct avail *)((char *)mem - sizeof(struct avail));
    block->tag = BLOCK_AVAIL; // Simulate invalid state
    buddy_free(&pool, mem); // Should return without action
    block->tag = BLOCK_RESERVED; // Restore for proper free
    buddy_free(&pool, mem);
    check_buddy_pool_full(&pool);
    buddy_destroy(&pool);
}

void test_buddy_malloc_smallest_k(void)
{
    fprintf(stderr, "->Testing buddy_malloc splitting to SMALLEST_K\n");
    void *mem = buddy_malloc(&test_pool, 1);
    struct avail *block = (struct avail *)((char *)mem - sizeof(struct avail));
    assert(block->kval == SMALLEST_K);
    buddy_free(&test_pool, mem);
}


int main(void) {
  time_t t;
  unsigned seed = (unsigned)time(&t);
  fprintf(stderr, "Random seed:%d\n", seed);
  srand(seed);
  printf("Running memory tests.\n");

  UNITY_BEGIN();
  RUN_TEST(test_buddy_init);
  RUN_TEST(test_buddy_malloc_one_byte);
  RUN_TEST(test_buddy_malloc_one_large);


  //Additional tests
  RUN_TEST(test_btok);
  RUN_TEST(test_malloc_null_and_zero);
  RUN_TEST(test_simple_malloc_and_free);
  RUN_TEST(test_coalescing_on_free);
  RUN_TEST(test_double_free_and_invalid_free);
  RUN_TEST(test_buddy_calc);
  RUN_TEST(test_buddy_free_null);
  RUN_TEST(test_buddy_free_invalid);
  RUN_TEST(test_buddy_malloc_smallest_k);
return UNITY_END();
}
