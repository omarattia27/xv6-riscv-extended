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


void freerange(void *pa_start, void *pa_end, int node_id);
void kfree_initial(void *pa, int node_id);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

// struct {
//   struct spinlock lock;
//   struct run *freelist;
// } kmem;
struct numa_node {
    struct spinlock lock;
    struct run *freelist;
    int id;
};

struct numa_node nodes[NNODES];

void
kinit()
{ 
  struct numa_node *kmem0 = &nodes[0];
  struct numa_node *kmem1 = &nodes[1];
  struct numa_node *kmem2 = &nodes[2];

  initlock(&kmem0->lock, "kmem0");
  initlock(&kmem1->lock, "kmem1");
  initlock(&kmem2->lock, "kmem2");

  initlock(&refcount_lock, "refcount");
  for(int i = 0; i < PHYSTOP/PGSIZE; i++)
    refcount[i] = 0;

  // freerange per NUMA node
  freerange(end, (void*)((uint64)end + (PHYSTOP - (uint64)end)/3), 0);
  freerange((void*)((uint64)end + (PHYSTOP - (uint64)end)/3), 
            (void*)((uint64)end + (PHYSTOP - (uint64)end)*2/3), 1);
  freerange((void*)((uint64)end + (PHYSTOP - (uint64)end)*2/3), (void*)PHYSTOP, 2);
}

// Need to specify the range of physical address per numa node
void
freerange(void *pa_start, void *pa_end, int node_id)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree_initial(p, node_id);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)

int
return_numa_node_id(void *pa)
{
  uint64 pa_addr = (uint64)pa;
  if (pa_addr < (uint64)end + (PHYSTOP - (uint64)end)/3) {
    return 0;
  } else if (pa_addr < (uint64)end + (PHYSTOP - (uint64)end)*2/3) {
    return 1;
  } else if (pa_addr < PHYSTOP) {
    return 2;
  }

  panic("return_numa_node_id: invalid physical address");
  return -1; // Should never reach here
}

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
  struct numa_node *kmem = &nodes[return_numa_node_id(pa)];
  acquire(&kmem->lock);
  r->next = kmem->freelist;
  kmem->freelist = r;
  release(&kmem->lock);
}

void
kfree_initial(void *pa, int node_id)
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
  struct numa_node *kmem = &nodes[node_id];
  acquire(&kmem->lock);
  r->next = kmem->freelist;
  kmem->freelist = r;
  release(&kmem->lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  struct numa_node *kmem = &nodes[cpuid()];

  acquire(&kmem->lock);
  r = kmem->freelist;
  if(r) {
    // Sanity check: next pointer should be either NULL or a valid kernel address
    if(r->next != 0 && ((uint64)r->next < (uint64)end || (uint64)r->next >= PHYSTOP)) {
      printf("kalloc: corrupted freelist! r=%p r->next=%p\n", r, r->next);
      panic("kalloc: freelist corruption");
    }
    kmem->freelist = r->next;
    release(&kmem->lock);
    memset((char*)r, 5, PGSIZE); // fill with junk
    // Initialize refcount to 1 for newly allocated page
    acquire(&refcount_lock);
    refcount[(uint64)r/PGSIZE] = 1;
    release(&refcount_lock);
  }

  // If current node is empty, try other NUMA nodes
  if(!r) {
    release(&kmem->lock);
    for(int i = 0; i < NNODES; i++) {
      if(i == cpuid()) continue;
      struct numa_node *kmem = &nodes[i];
      acquire(&kmem->lock);
      r = kmem->freelist;
      if(r) {
        // Sanity check: next pointer should be either NULL or a valid kernel address
        if(r->next != 0 && ((uint64)r->next < (uint64)end || (uint64)r->next >= PHYSTOP)) {
          printf("kalloc: corrupted freelist! r=%p r->next=%p\n", r, r->next);
          panic("kalloc: freelist corruption");  
        }
        kmem->freelist = r->next;
        release(&kmem->lock);
        memset((char*)r, 5, PGSIZE); // fill with junk
        // Initialize refcount to 1 for newly allocated page
        acquire(&refcount_lock);
        refcount[(uint64)r/PGSIZE] = 1;
        release(&refcount_lock);
        return (void*)r;
      }
      release(&kmem->lock);
    }
  }

  return (void*)r;
}
