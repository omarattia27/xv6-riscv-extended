#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Test that specifically exercises BOTH lazy allocation AND COW
void
test_combined(void)
{
  printf("\n=== TEST: Combined Lazy Allocation + COW ===\n");
  
  // Step 1: Allocate memory (lazy - not yet allocated)
  printf("Step 1: Calling sbrk(8192) - should be lazy\n");
  char *buf = sbrk(8192);
  if(buf == (char*)-1) {
    printf("FAIL: sbrk failed\n");
    exit(1);
  }
  
  // Step 2: Write to trigger lazy allocation
  printf("Step 2: Writing to buf[0] - should trigger LAZY allocation\n");
  buf[0] = 'P';  // This should print [LAZY] message
  printf("Step 2: Writing to buf[4096] - should trigger LAZY allocation\n");
  buf[4096] = 'P';  // Another lazy page
  
  // Step 3: Fork (COW should share these now-allocated pages)
  printf("Step 3: Forking - COW should share pages\n");
  int pid = fork();
  if(pid < 0) {
    printf("FAIL: fork failed\n");
    exit(1);
  }
  
  if(pid == 0) {
    // Child
    printf("Step 4 (child): Writing to buf[0] - should trigger COW copy\n");
    buf[0] = 'C';  // This should print [COW] message with refcount > 1
    
    printf("Step 5 (child): Allocating NEW memory with sbrk\n");
    char *newbuf = sbrk(4096);
    printf("Step 6 (child): Writing to new memory - should trigger LAZY\n");
    newbuf[0] = 'N';  // This should print [LAZY] message
    
    printf("Child done\n");
    exit(0);
  } else {
    // Parent
    wait(0);
    printf("Step 7 (parent): Writing to buf[0] after child exits\n");
    buf[0] = 'X';  // Refcount should be 1 now, so [COW] promote message
    
    printf("Parent done\n");
  }
  
  printf("=== TEST PASSED ===\n\n");
}

// Test lazy allocation specifically
void
test_lazy_only(void)
{
  printf("\n=== TEST: Lazy Allocation Only ===\n");
  
  printf("Allocating 20KB with sbrk...\n");
  char *buf = sbrk(20480);
  if(buf == (char*)-1) {
    printf("FAIL: sbrk failed\n");
    exit(1);
  }
  
  printf("Writing to 5 different pages - each should trigger [LAZY]...\n");
  for(int i = 0; i < 5; i++) {
    printf("  Writing to page %d...\n", i);
    buf[i * 4096] = 'L';  // Each should print [LAZY]
  }
  
  printf("=== TEST PASSED ===\n\n");
}

// Test COW specifically
void
test_cow_only(void)
{
  printf("\n=== TEST: COW Only ===\n");
  
  printf("Allocating and initializing memory...\n");
  char *buf = sbrk(8192);
  if(buf == (char*)-1) {
    printf("FAIL: sbrk failed\n");
    exit(1);
  }
  
  // Initialize (triggers lazy allocation, but no COW yet)
  for(int i = 0; i < 2; i++) {
    buf[i * 4096] = 'I';
  }
  
  printf("Forking...\n");
  int pid = fork();
  if(pid < 0) {
    printf("FAIL: fork failed\n");
    exit(1);
  }
  
  if(pid == 0) {
    // Child writes - should trigger COW
    printf("Child writing to both pages - should trigger [COW] with refcount > 1...\n");
    buf[0] = 'C';
    buf[4096] = 'C';
    exit(0);
  } else {
    wait(0);
    // Parent writes after child exits - refcount should be 1
    printf("Parent writing after child exit - should trigger [COW] promote...\n");
    buf[0] = 'P';
    buf[4096] = 'P';
  }
  
  printf("=== TEST PASSED ===\n\n");
}

int
main(int argc, char *argv[])
{
  printf("\n");
  printf("========================================\n");
  printf("  Testing Lazy Allocation + COW Fork\n");
  printf("========================================\n");
  
  test_lazy_only();
  test_cow_only();
  test_combined();
  
  printf("\n========================================\n");
  printf("  ALL TESTS COMPLETED\n");
  printf("  Check output for [LAZY] and [COW] tags\n");
  printf("========================================\n\n");
  
  exit(0);
}
