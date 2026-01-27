// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

// reference count per physical page where the index is the physical page number
int refcount[PHYSTOP/PGSIZE];
struct spinlock refcount_lock; 


void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&refcount_lock, "refcount");
  for(int i = 0; i < PHYSTOP/PGSIZE; i++)
    refcount[i] = 0;
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&refcount_lock);
  // If refcount is 0, this is initial free during boot, set to 1 then free
  if(refcount[(uint64)pa/PGSIZE] == 0) {
    refcount[(uint64)pa/PGSIZE] = 1;
  }
  
  if(refcount[(uint64)pa/PGSIZE] > 1) {
    refcount[(uint64)pa/PGSIZE]--;
    release(&refcount_lock);
    return;  // Don't free yet
  }
  
  // Check for double-free
  if(refcount[(uint64)pa/PGSIZE] != 1) {
    printf("kfree: double free detected! pa=%p refcount=%d\n", pa, refcount[(uint64)pa/PGSIZE]);
    release(&refcount_lock);
    panic("kfree: double free");
  }
  
  refcount[(uint64)pa/PGSIZE] = 0;
  release(&refcount_lock);
  
  // Fill with junk to catch dangling refs.
  // Use 0xAA instead of 0x01 to help debug
  memset(pa, 0xAA, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r) {
    // Sanity check: next pointer should be either NULL or a valid kernel address
    if(r->next != 0 && ((uint64)r->next < (uint64)end || (uint64)r->next >= PHYSTOP)) {
      printf("kalloc: corrupted freelist! r=%p r->next=%p\n", r, r->next);
      panic("kalloc: freelist corruption");
    }
    kmem.freelist = r->next;
  }
  release(&kmem.lock);

  if(r) {
    memset((char*)r, 5, PGSIZE); // fill with junk
    // Initialize refcount to 1 for newly allocated page
    acquire(&refcount_lock);
    refcount[(uint64)r/PGSIZE] = 1;
    release(&refcount_lock);
  }
  return (void*)r;
}
