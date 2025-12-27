#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"

extern int refcount[PHYSTOP/PGSIZE];
extern struct spinlock refcount_lock;

void
debug_refcounts(void)
{
  int nonzero = 0;
  int total = 0;
  
  acquire(&refcount_lock);
  for(int i = 0; i < PHYSTOP/PGSIZE; i++) {
    if(refcount[i] > 0) {
      nonzero++;
      if(nonzero <= 10) { // Print first 10
        printf("  page %d (pa=0x%lx): refcount=%d\n", i, i * PGSIZE, refcount[i]);
      }
    }
    total++;
  }
  release(&refcount_lock);
  
  printf("Total pages with refcount > 0: %d (out of %d total)\n", nonzero, total);
}
