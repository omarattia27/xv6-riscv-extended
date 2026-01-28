#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

int queue_size_0 = 0;
int queue_size_1 = 0;

// Queues now hold threads (max NPROC * NTHREAD threads)
struct thread *queue_0[NPROC * NTHREAD];
struct thread *queue_1[NPROC * NTHREAD];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;
struct spinlock queue_lock;

extern void forkret(void);
extern char userret[];
static void freeproc(struct proc *p);
void threadret(void);  // Forward declaration

// Forward declarations for queue functions
void queue_push(struct thread *t);
struct thread *queue_pop(void);
struct thread *queue_remove(int tid, int priority);
void queue_update_priorities(void);
void loop_proc_and_update_queues(void);
void set_threading_initialized(void);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    // Map main thread kernel stack
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    
    // Map thread 2 kernel stack
    pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    va = KSTACK2((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    
    // Map thread 3 kernel stack
    pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    va = KSTACK3((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

int init_thread(struct thread *t) {
  initlock(&t->lock, "thread");
  t->state = T_UNUSED;
  t->chan = 0;
  t->killed = 0;
  t->xstate = 0;
  t->tid = -1;
  t->thread_slot = -1;  // Initialize thread slot
  t->cpu_ticks = 0;
  t->time_slices_left = 0;
  t->age_in_low_queue = 0;
  t->age_in_high_queue = 0;
  t->priority = -1; // Initialize priority to -1 to mark as not queued yet
  t->magic = 0xDEADBEEFCAFEBABE;
  return 0;
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  initlock(&queue_lock, "queue_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      initlock(&p->mem_lock, "proc_mem");
      initlock(&p->fd_lock, "proc_fd");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
      
      // Allocate and initialize threads
      for(int i = 0; i < NTHREAD; i++) {
          p->threads[i] = (struct thread *)kalloc();  // Allocate memory!
          if(p->threads[i] == 0)
              panic("thread alloc");
          init_thread(p->threads[i]);
      }
      
      p->threads[0]->kstack = KSTACK((int) (p - proc));
      p->threads[1]->kstack = KSTACK2((int) (p - proc));
      p->threads[2]->kstack = KSTACK3((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

// Return the current struct thread *, or zero if none.
struct thread*
mythread(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct thread *t = c->thread;
  
  // CRITICAL DEBUG: Track mythread() calls and detect wrong thread
  static int debug_count = 0;
  if(debug_count < 10) {
    // Try to find the thread that should be executing based on current context
    struct proc *p = c->proc;
    struct thread *expected = 0;
    if(p) {
      // Check if this looks like main thread execution by looking at registers
      // This is a rough heuristic - main thread typically has different register patterns
      expected = p->threads[0];  // Assume main thread for now
    }
    
    // printf("MYTHREAD DEBUG %d: c->thread=%p (slot=%d tid=%d), expected=%p, proc=%p\n", 
    //        debug_count, t, t ? t->thread_slot : -1, t ? t->tid : -1, expected, p);
    
    // if(t && expected && t != expected && t->thread_slot != 0) {
    //   printf("  WARNING: c->thread points to Thread %d but should probably be main thread!\n", t->thread_slot);
    // }
    debug_count++;
  }
  
  pop_off();
  return t;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  printf("allocproc: no free process slots!\n");
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->home_node = p->pid; // Simple home node assignment based on pid

  // Threads are already allocated in procinit(), just reinitialize them
  for(int i = 0; i < NTHREAD; i++) {
    if(p->threads[i] == 0)
      panic("allocproc: thread not allocated");
    init_thread(p->threads[i]);
  }
  
  // Set kernel stack addresses
  p->threads[0]->kstack = KSTACK((int) (p - proc));
  p->threads[1]->kstack = KSTACK2((int) (p - proc));
  p->threads[2]->kstack = KSTACK3((int) (p - proc));

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Allocate trapframe2 for thread 2
  if((p->trapframe2 = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Allocate trapframe3 for thread 3
  if((p->trapframe3 = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Initialize main thread (thread 0)
  struct thread *t = p->threads[0];
  t->proc = p;
  t->tid = 0;  // Main thread always has tid 0
  t->thread_slot = 0;  // Main thread is always slot 0
  t->state = T_USED;
  t->trapframe = p->trapframe;
  t->trapframe_va = TRAPFRAME;
  t->stack_base = 0;  // Will be set in exec
  
  // Set up thread context to start executing at forkret
  memset(&t->context, 0, sizeof(t->context));
  t->context.ra = (uint64)forkret;
  t->context.sp = t->kstack + PGSIZE;

  // Keep old proc context for compatibility (can remove later)
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{ 
  // Debug: track freeproc calls
  // printf("[DEBUG] freeproc: pid=%d\n", p->pid);
  
  // deallocate user pages.
  // decrement reference counts on physical pages
  //def decrement_refcount_deallocate():
    // for PTE in PT:
    //   if pa=PE2PA(PTE) != 0:
    //     refcount[pa/PGSIZE]--
    //     if refcount[pa/PGSIZE]==0:
    //       freepa(pa)
  // printf("[DEBUG] freeproc: pid=%d sz=0x%lx\n", p->pid, p->sz);
  decrement_refcount_deallocate(p->pagetable, p->sz);
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->trapframe2)
    kfree((void*)p->trapframe2);
  p->trapframe2 = 0;
  if(p->trapframe3)
    kfree((void*)p->trapframe3);
  p->trapframe3 = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->xstate = 0;
  p->state = UNUSED;
  
  // First, remove all threads from queues while they're still valid
  acquire(&queue_lock);
  for (int i=0; i<NTHREAD; i++) {
    if(p->threads[i] != 0 && p->threads[i]->priority != -1) {
      int priority = p->threads[i]->priority;
      int tid = p->threads[i]->tid;
      
      // Remove from queue manually
      if (priority == 0) {
        for (int j = 0; j < queue_size_0; j++) {
          if (queue_0[j] == p->threads[i]) {
            for (int k = j + 1; k < queue_size_0; k++) {
              queue_0[k - 1] = queue_0[k];
            }
            queue_size_0--;
            break;
          }
        }
      } else if (priority == 1) {
        for (int j = 0; j < queue_size_1; j++) {
          if (queue_1[j] == p->threads[i]) {
            for (int k = j + 1; k < queue_size_1; k++) {
              queue_1[k - 1] = queue_1[k];
            }
            queue_size_1--;
            break;
          }
        }
      }
    }
  }
  release(&queue_lock);
  
  // Now reset threads to UNUSED state but DON'T free them
  // Keep them allocated for reuse by next process
  for (int i=0; i<NTHREAD; i++) {
      if(p->threads[i] != 0) {
        if(mythread() == p->threads[i]) {
          // If freeing the current thread, skip resetting to 
          p->threads[i]->state = T_UNUSED;
          p->threads[i]->chan = 0;
          p->threads[i]->killed = 0;
          p->threads[i]->xstate = 0;
          p->threads[i]->tid = -1;
          p->threads[i]->cpu_ticks = 0;
          p->threads[i]->time_slices_left = 0;
          p->threads[i]->age_in_low_queue = 0;
          p->threads[i]->age_in_high_queue = 0;
          p->threads[i]->priority = -1; 
          release(&p->threads[i]->lock);
        }else{
          acquire(&p->threads[i]->lock);
          p->threads[i]->state = T_UNUSED;
          p->threads[i]->chan = 0;
          p->threads[i]->killed = 0;
          p->threads[i]->xstate = 0;
          p->threads[i]->tid = -1;
          p->threads[i]->cpu_ticks = 0;
          p->threads[i]->time_slices_left = 0;
          p->threads[i]->age_in_low_queue = 0;
          p->threads[i]->age_in_high_queue = 0;
          p->threads[i]->priority = -1;
          release(&p->threads[i]->lock);
          // DON'T free - keep allocated for reuse
          // kfree((void*)p->threads[i]);
          // p->threads[i] = 0;
        }
      }
  }
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  // map trapframe2 for thread 2
  if(mappages(pagetable, TRAPFRAME2, PGSIZE,
              (uint64)(p->trapframe2), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  // map trapframe3 for thread 3
  if(mappages(pagetable, TRAPFRAME3, PGSIZE,
              (uint64)(p->trapframe3), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmunmap(pagetable, TRAPFRAME2, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmunmap(pagetable, TRAPFRAME2, 1, 0);
  uvmunmap(pagetable, TRAPFRAME3, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  // Process state is kept for compatibility, but scheduling is done via threads
  p->state = RUNNABLE;
  
  // Make main thread runnable
  struct thread *t = p->threads[0];
  acquire(&t->lock);
  t->state = T_RUNNABLE;
  release(&t->lock);

  release(&p->lock);
  
  // Threading system is now initialized
  set_threading_initialized();
}

// Wait for a thread with given tid to exit and return its exit status.
// Return 0 on success with exit status, -1 on error.
int thread_join(int tid, uint64 addr)
{
  struct thread *target_thread = 0;
  struct proc *p = myproc();
  int found_thread = 0;

  // Can't join the main thread (tid 0) or yourself
  if(tid <= 0) {
    return -1;
  }
  
  struct thread *current = mythread();
  if(current && current->tid == tid) {
    return -1; // Can't join yourself
  }

  acquire(&p->lock);

  for(;;){
    // Scan through this process's threads looking for the target tid
    found_thread = 0;
    target_thread = 0;
    
    for(int i = 0; i < NTHREAD; i++) {
      struct thread *t = p->threads[i];
      if(t == 0)
        continue;
        
      acquire(&t->lock);
      if(t->tid == tid && t->state != T_UNUSED) {
        found_thread = 1;
        target_thread = t;
        
        if(t->state == T_ZOMBIE) {
          // Found zombie thread - get exit status and clean up
          int xstate = t->xstate;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&xstate,
                                  sizeof(xstate)) < 0) {
            release(&t->lock);
            release(&p->lock);
            return -1;
          }
          
          // Clean up thread but don't free memory (reuse for next thread)
          t->state = T_UNUSED;
          t->chan = 0;
          t->killed = 0;
          t->xstate = 0;
          t->tid = -1;
          t->cpu_ticks = 0;
          t->time_slices_left = 0;
          t->age_in_low_queue = 0;
          t->age_in_high_queue = 0;
          t->priority = -1;
          
          release(&t->lock);
          release(&p->lock);
          return 0; // Success
        }
        release(&t->lock);
        break; // Found thread but not zombie yet
      }
      release(&t->lock);
    }

    // No thread with this tid exists
    if(!found_thread) {
      release(&p->lock);
      return -1;
    }
    
    // Thread exists but not zombie yet - check if current thread was killed
    if(killed_thread(mythread())) {
      release(&p->lock);
      return -1;
    }
    
    // Wait for the thread to exit
    // Sleep on the specific tid as the channel
    sleep((void*)(uint64)tid, &p->lock);
    // p->lock will be reacquired when we wake up
  }
}

int thread_create(void (*fn)(void)){
  // printf("thread_create: kernel received fn=0x%lx\n", (uint64)fn);
  struct proc *p = myproc();
  struct thread *t = 0;

  // Find an UNUSED thread slot (skip 0, as it's the main thread)
  for(int i = 1; i < NTHREAD; i++) {
    if(p->threads[i]) {
      acquire(&p->threads[i]->lock);
      if(p->threads[i]->state == T_UNUSED) {
        t = p->threads[i];
        
        // Basic thread setup
        t->proc = p;
        t->state = T_USED;
        
        // Assign the correct trapframe and trapframe_va based on thread index
        if(i == 1) {
          t->trapframe = p->trapframe2;
          t->trapframe_va = TRAPFRAME2;
          // printf("DEBUG: Thread 1 trapframe=%p, main trapframe=%p\n", p->trapframe2, p->trapframe);
        } else if(i == 2) {
          t->trapframe = p->trapframe3;
          t->trapframe_va = TRAPFRAME3;
          // printf("DEBUG: Thread 2 trapframe=%p, main trapframe=%p\n", p->trapframe3, p->trapframe);
        }

        // stack_base should already be set by exec
        if(t->stack_base == 0) {
          printf("ERROR: stack_base not set for thread %d\n", i);
          release(&p->threads[i]->lock);
          return -1;
        }
        
        // printf("DEBUG: thread_create slot %d, stack_base=0x%lx\n", i, t->stack_base);
        // printf("DEBUG: trapframe=%p trapframe_va=0x%lx sp=0x%lx\n", 
        //        t->trapframe, t->trapframe_va, t->trapframe->sp);

        // Set up trapframe completely independently - no dependency on p->trapframe
        if(t->trapframe) {
          // Initialize trapframe to zero first
          memset(t->trapframe, 0, sizeof(struct trapframe));
          
          // Set up all fields independently
          t->trapframe->kernel_satp = r_satp();              // Current kernel page table
          t->trapframe->kernel_sp = t->kstack + PGSIZE;      // Thread's kernel stack
          t->trapframe->kernel_trap = (uint64)usertrap;      // Trap handler
          t->trapframe->kernel_hartid = r_tp();              // Current hart ID
          t->trapframe->epc = (uint64)fn;                    // Thread function entry point
          t->trapframe->sp = t->stack_base + USERSTACK*PGSIZE;  // Stack pointer at top of usable stack
          t->trapframe->a0 = 0;                              // Clear return value
          
          // CRITICAL: Initialize all general purpose registers to known safe values
          // For threads, ra should point to exit function so when thread_func returns, it calls exit(0)
          t->trapframe->ra = 0x11a6;  // Correct address of exit function (from symbol table)
          t->trapframe->gp = 0;     // Global pointer
          t->trapframe->tp = 0;     // Thread pointer (user mode)
          
          // Initialize all temporary and saved registers
          t->trapframe->t0 = t->trapframe->t1 = t->trapframe->t2 = 0;
          t->trapframe->s0 = 0;  // No previous frame - let first function set up s0 properly
          t->trapframe->s1 = 0;
          t->trapframe->a1 = t->trapframe->a2 = t->trapframe->a3 = 0;
          t->trapframe->a4 = t->trapframe->a5 = t->trapframe->a6 = t->trapframe->a7 = 0;
          t->trapframe->s2 = t->trapframe->s3 = t->trapframe->s4 = t->trapframe->s5 = 0;
          t->trapframe->s6 = t->trapframe->s7 = t->trapframe->s8 = t->trapframe->s9 = 0;
          t->trapframe->s10 = t->trapframe->s11 = 0;
          t->trapframe->t3 = t->trapframe->t4 = t->trapframe->t5 = t->trapframe->t6 = 0;
          
          // CRITICAL: Double-check that epc is still set correctly after register init
          if(t->trapframe->epc != (uint64)fn) {
            printf("ERROR: epc corrupted during init! expected=0x%lx actual=0x%lx\n", 
                   (uint64)fn, t->trapframe->epc);
            panic("thread_create: epc corrupted");
          }
          
          // printf("DEBUG: Created thread slot %d, sp=0x%lx, epc=0x%lx\n", i, t->trapframe->sp, t->trapframe->epc);
          // printf("DEBUG: trapframe=%p trapframe_va=0x%lx\n", t->trapframe, t->trapframe_va);
          
          // Thread setup complete
        } else {
          printf("ERROR: trapframe is NULL!\n");
          release(&p->threads[i]->lock);
          return -1;
        }
        
        // Set up minimal thread context
        memset(&t->context, 0, sizeof(t->context));
        t->context.sp = t->kstack + PGSIZE;
        t->context.ra = (uint64)threadret;

        // Get TID - use process-local TID assignment
        // Find the next available TID within this process
        int assigned_tid = -1;
        for(int tid_candidate = 1; tid_candidate < 1000; tid_candidate++) {
          int tid_in_use = 0;
          
          // Check if this TID is already used by another thread in this process
          for(int j = 0; j < NTHREAD; j++) {
            if(p->threads[j] && j != i) {
              if(p->threads[j]->tid == tid_candidate) {
                tid_in_use = 1;
                break;
              }
            }
          }
          
          if(!tid_in_use) {
            assigned_tid = tid_candidate;
            break;
          }
        }
        
        if(assigned_tid == -1) {
          printf("ERROR: Could not assign TID\n");
          release(&p->threads[i]->lock);
          return -1;
        }
        
        t->tid = assigned_tid;
        t->thread_slot = i;  // Set thread slot within process (1 or 2)

        // Initialize priority to -1 so scheduler will add it to queue
        t->priority = -1;
        
        // Make thread runnable
        t->state = T_RUNNABLE;
        release(&p->threads[i]->lock);

        return t->tid;
      }
      release(&p->threads[i]->lock);
    }
  }
  
  return -1; // No available thread slots
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  acquire(&p->mem_lock);
  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME3) {
      release(&p->mem_lock);
      return -1;
    }
    // growproc() always allocates eagerly
    // Lazy allocation is handled in sys_sbrk() by skipping growproc()
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      release(&p->mem_lock);
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  release(&p->mem_lock);
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy-on-write: share parent's memory with child
  if(uvmcow_copy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  acquire(&p->fd_lock);
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);
  release(&p->fd_lock);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  
  // Make child's main thread runnable
  struct thread *nt = np->threads[0];
  acquire(&nt->lock);
  nt->state = T_RUNNABLE;
  release(&nt->lock);
  
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();
  struct thread *t = mythread();

  if(p == initproc)
    panic("init exiting");
    
  // If this is not the main thread (tid != 0), just exit the thread
  if(t->tid != 0) {
    
    // printf("DEBUG: kexit() called for thread %d with status %d\n", t->tid, status);
    
    // Mark this thread as zombie and wake up any joiners
    acquire(&t->lock);
    t->xstate = status;
    t->state = T_ZOMBIE;
    
    // printf("DEBUG: Thread %d marked as T_ZOMBIE, attempting queue removal\n", t->tid);
    
    // CRITICAL FIX: Remove thread from scheduler queues before exiting
    // This prevents the zombie thread from being scheduled again
    struct thread *removed = queue_remove(t->tid, t->priority);
    // if(removed == t) {
    //   printf("DEBUG: Thread %d successfully removed from queue\n", t->tid);
    // } else {
    //   printf("DEBUG: WARNING - Thread %d NOT found in queue (removed=%p)\n", t->tid, removed);
    // }
    
    // Wake up any threads waiting to join this thread
    wakeup((void*)(uint64)t->tid);
    
    // printf("DEBUG: Thread %d about to call sched()\n", t->tid);
    sched();
    
    printf("ERROR: Thread %d returned from sched() - this should never happen!\n", t->tid);
    panic("thread should not return from sched");
  }

  // This is the main thread - exit the entire process
  //printf("kexit: main thread exiting, killing entire process\n");

  // Close all open files.
  acquire(&p->fd_lock);
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      p->ofile[fd] = 0;
      release(&p->fd_lock);
      fileclose(f);  // Release lock before fileclose (may sleep)
      acquire(&p->fd_lock);
    }
  }
  release(&p->fd_lock);

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  // Mark process as zombie (holds p->lock briefly)
  acquire(&p->lock);
  p->xstate = status;
  p->state = ZOMBIE;
  release(&p->lock);

  release(&wait_lock);

  // Now mark this thread as zombie and schedule out
  acquire(&t->lock);
  t->xstate = status;
  t->state = T_ZOMBIE;
  sched();
  
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed_thread(mythread())){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    // Note: sleep() will acquire mythread()->lock itself, so we must not hold it
    struct thread *t = mythread();
    if(t && holding(&t->lock)) {
      // Unexpected: we shouldn't normally hold the lock here
      panic("kwait: thread lock held unexpectedly");
    }
    // Normal case: lock not held, sleep will acquire it
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

void queue_push(struct thread *t) {
  acquire(&t->lock);
  int priority = t->priority;
  
  if (priority == 0) {
    // Release thread lock before acquiring queue_lock (correct lock ordering)
    release(&t->lock);
    
    acquire(&queue_lock);
    if(queue_size_0 < NPROC * NTHREAD) {
      queue_0[queue_size_0++] = t;
      release(&queue_lock);
    } else {
      // Queue full - mark thread as not queued
      release(&queue_lock);
      
      // Reacquire thread lock to update priority
      acquire(&t->lock);
      t->priority = -1;  // Mark as not in queue
      release(&t->lock);
    }
  } else {
    release(&t->lock);
  } 
}

struct thread *queue_pop() {
  struct thread *t = 0;
  
  // Loop to skip stale entries (threads that are no longer runnable)
  while(1) {
    acquire(&queue_lock);
    
    if (queue_size_0 > 0) {
      t = queue_0[0];
      for (int i = 1; i < queue_size_0; i++) {
        queue_0[i - 1] = queue_0[i];
      }
      queue_size_0--;
    } else if (queue_size_1 > 0) {
      t = queue_1[0];
      for (int i = 1; i < queue_size_1; i++) {
        queue_1[i - 1] = queue_1[i];
      }
      queue_size_1--;
    } else {
      t = 0; // No thread available
      release(&queue_lock);
      break;
    }
    
    release(&queue_lock);
    
    // Validate the thread is still runnable
    if (t != 0) {
      acquire(&t->lock);
      if(t->state == T_RUNNABLE) {
        // Valid thread - mark as not in queue and return WITHOUT lock held
        t->priority = -1;
        release(&t->lock);
        return t;  // Caller must acquire t->lock if needed
      } else {
        // Stale entry - just drop it and continue to next
        t->priority = -1;
        release(&t->lock);
        t = 0;
        // Loop will try next queue entry
      }
    } else {
      break;
    }
  }
  
  return 0;  // No runnable thread found
}

struct thread *queue_remove(int tid, int priority) {
  struct thread *t = 0;
  acquire(&queue_lock);
  if (priority == 0) {
    for (int i = 0; i < queue_size_0; i++) {
      if (queue_0[i]->tid == tid) {
        t = queue_0[i];
        for (int j = i + 1; j < queue_size_0; j++) {
          queue_0[j - 1] = queue_0[j];
        }
        queue_size_0--;
        break;
      }
    }
  }else if (priority == 1) {
    for (int i = 0; i < queue_size_1; i++) {
      if (queue_1[i]->tid == tid) {
        t = queue_1[i];
        for (int j = i + 1; j < queue_size_1; j++) {
          queue_1[j - 1] = queue_1[j];
        }
        queue_size_1--;
        break;
      }
    }
  }
  release(&queue_lock);

  if (t != 0) {
    acquire(&t->lock);
    t->priority = -1; // Mark as not in any queue
    release(&t->lock);
  }

  return t;
}

// Global flag to indicate if threading system is fully initialized
static int threading_initialized = 0;

void set_threading_initialized() {
  threading_initialized = 1;
}

struct thread *queue_get_numa_friendly_thread() {
  // Placeholder for NUMA-aware scheduling logic
  if(!threading_initialized)
    return 0;
    
  struct proc *p;
  struct thread *t;

  //for priority levels 0 and 1
  for(int priority = 0; priority <= 1; priority++) {
    // Scan the queue for a thread matching the current CPU
    acquire(&queue_lock);
    int queue_size = (priority == 0) ? queue_size_0 : queue_size_1;
    struct thread **queue = (priority == 0) ? queue_0 : queue_1;

    for(p = proc; p < &proc[NPROC]; p++) {
      // Skip unused processes to avoid unnecessary work
      if(p->state == UNUSED)
        continue;
        
      for(int i = 0; i < NTHREAD; i++) {
        t = p->threads[i];
        if(t == 0)
          continue;
          
        // Skip threads whose lock is already locked to avoid double-acquire
        if(t->lock.locked)
          continue;
          
        acquire(&t->lock);
        if (t->state != T_RUNNABLE) {
          release(&t->lock);
          continue;
        }

        // Check if thread's last_cpu matches current CPU
        if(t->proc->home_node == cpuid()) {
          // Found a NUMA-friendly thread
          t->priority = -1; // Mark as not in queue
          release(&t->lock);
          release(&queue_lock);
          // Remove from queue
          struct thread *removed = queue_remove(t->tid, 0);
          return t;
        } else {
          release(&t->lock);
        }


      }
  }
    release(&queue_lock);
  }
  return 0; // No NUMA-friendly thread found
}

void queue_update_priorities() {
  // Don't do queue management until threading is fully initialized
  if(!threading_initialized)
    return;
    
  struct proc *p;
  struct thread *t;
  for(p = proc; p < &proc[NPROC]; p++) {
    // Skip unused processes to avoid unnecessary work
    if(p->state == UNUSED)
      continue;
      
    for(int i = 0; i < NTHREAD; i++) {
      t = p->threads[i];
      if(t == 0)
        continue;
      
      // Skip threads that aren't properly initialized
      if(t->magic != 0xDEADBEEFCAFEBABE)
        continue;
        
      // Skip threads whose lock is already locked to avoid double-acquire
      if(t->lock.locked)
        continue;
        
      acquire(&t->lock);
      if (t->state != T_RUNNABLE) {
        release(&t->lock);
        continue;
      }
      if (t->priority == 1) {
        t->age_in_low_queue++;
        if (t->age_in_low_queue >= 5) {
          // Promote to high priority queue
          t->priority = 1;
          t->age_in_low_queue = 0; // Reset age counter
          t->time_slices_left = 1; // Reset time slices
          release(&t->lock);
          queue_remove(t->tid, 1);
          queue_push(t);
        } else {
          release(&t->lock);
        }
      } else if (t->priority == 0) {
        t->age_in_high_queue++;
        if (t->age_in_high_queue >= 5) {
          // Promote to high priority queue
          t->priority = 0;
          t->age_in_high_queue = 0; // Reset age counter
          release(&t->lock);
          queue_remove(t->tid, 0);
          queue_push(t);
        } else {
          release(&t->lock);
        }
      } else {
        release(&t->lock);
      }
    }
  }
}

void loop_proc_and_update_queues() {
  // Don't do queue management until threading is fully initialized  
  if(!threading_initialized)
    return;
    
  struct proc *p;
  struct thread *t;
  
  // Simply scan for runnable threads not in any queue
  for(p = proc; p < &proc[NPROC]; p++) {
    // Skip unused processes to avoid unnecessary work
    if(p->state == UNUSED)
      continue;
      
    for(int i = 0; i < NTHREAD; i++) {
      t = p->threads[i];
      if(t == 0)
        continue;
        
      // Skip threads that aren't properly initialized
      if(t->magic != 0xDEADBEEFCAFEBABE)
        continue;
        
      // Skip threads whose lock is already locked to avoid double-acquire
      if(t->lock.locked)
        continue;
        
      acquire(&t->lock);
      if(t->state == T_RUNNABLE && t->priority == -1) {
        // If thread is not already in a queue, add it
        //printf("DEBUG: Adding new thread %d to queue (state=%d)\n", t->tid, t->state);
        t->priority = 0; // Default to high priority queue
        t->time_slices_left = 3; // Initial time slice allocation
        release(&t->lock);
        queue_push(t);
      } else {
        if(t->state != T_RUNNABLE) {
          //printf("DEBUG: Thread %d not runnable (state=%d, priority=%d)\n", t->tid, t->state, t->priority);
        }
        release(&t->lock);
      }
    }
  }
}

// Per-CPU thread scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a thread to run.
//  - swtch to start running that thread.
//  - eventually that thread transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct thread *t;
  struct cpu *c = mycpu();

  c->proc = 0;
  c->thread = 0;
  for(;;){
    // The most recent thread to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // threads are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    int found = 0;

    loop_proc_and_update_queues();

    // we should adjust this function to find a thread with a NUMA locality match first
    t = queue_get_numa_friendly_thread();
    if(t == 0) {
      t = queue_pop();
    }
    
    if (t != 0) {
      acquire(&t->lock);
      if(t->state == T_RUNNABLE) {
        // Switch to chosen thread.  It is the thread's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        t->state = T_RUNNING;
        c->thread = t;
        c->proc = t->proc;  // Also set proc for compatibility

        swtch(&c->context, &t->context);
        
        if(t->state == T_RUNNING)
          panic("scheduler: thread still RUNNING after swtch");
        // Thread is done running for now.
        // It should have changed its t->state before coming back.
        c->proc = 0;
        c->thread = 0;
        found = 1;
      } else if(t->state == T_ZOMBIE) {
        // CRITICAL: This should not happen if queue_remove is working
        printf("ERROR: Scheduler found zombie thread %d in queue! state=%d\n", t->tid, t->state);
        // Don't run zombie threads
      } else {
        // Thread is in some other state (sleeping, etc)
        // This is normal, just skip it
      }
      // Always release the thread lock after we acquired it
      if(holding(&t->lock)) {
        release(&t->lock);
      }
    }
    queue_update_priorities();

    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only t->lock
// and have changed thread->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be thread->intena and thread->noff, but that would
// break in the few places where a lock is held but
// there's no thread.
void
sched(void)
{
  int intena;
  struct thread *t = mythread();

  if(t == 0)
    panic("sched: no thread");
  if(!holding(&t->lock))
    panic("sched t->lock");
  if(mycpu()->noff != 1) {
    printf("sched: noff=%d intena=%d intr_get()=%d\n", mycpu()->noff, mycpu()->intena, intr_get());
    panic("sched locks");
  }
  if(t->state == T_RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&t->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct thread *t = mythread();
  if(t == 0)
    return;  // Nothing to yield if no thread running
    
  // Check if we already hold the lock (scheduler context)
  // if(holding(&t->lock)) {
  //   // We already hold the lock (scheduler has it), just change state and sched
  //   t->state = T_RUNNABLE;
  //   sched();
  // } else {
    // Normal case - acquire lock, change state, sched
    // Note: sched() never returns here, scheduler releases lock
  if(holding(&t->lock))
    panic("yield: already holding t->lock");

  acquire(&t->lock);
  t->state = T_RUNNABLE;
  sched();
  release(&t->lock);
    // This line is never reached - scheduler releases the lock
  //}
}

// A thread's very first return to user space
// will jump here.  "Return" to user space is
// handled in trampoline.S.
void threadret(void) {
  struct thread *t = mythread();
  struct proc *p = t->proc;
  extern char userret[];
  extern char uservec[];

  // Validate thread state before proceeding
  if(!t || t->magic != 0xDEADBEEFCAFEBABE) {
    panic("threadret: invalid thread");
  }
  
  if(!t->trapframe) {
    panic("threadret: no trapframe");
  }

  // Validate that the trapframe->epc is not zero
  if(t->trapframe->epc == 0) {
    printf("ERROR: threadret: epc is zero for thread %d!\n", t->tid);
    panic("threadret: epc is zero");
  }

  // Still holding t->lock from scheduler.
  release(&t->lock);

  // Thread-specific version of prepare_return() using thread's own trapframe
  intr_off();
  
  // Send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // CRITICAL: Refresh trapframe values to ensure they're current
  // Do this BEFORE setting sscratch to ensure trapframe is ready
  t->trapframe->kernel_satp = r_satp();
  t->trapframe->kernel_sp = t->kstack + PGSIZE;  // Thread's kernel stack
  t->trapframe->kernel_trap = (uint64)usertrap;
  t->trapframe->kernel_hartid = r_tp();

  // Validate trapframe_va before using it
  if(t->trapframe_va == 0) {
    panic("threadret: trapframe_va is zero");
  }

  // CRITICAL: Set sscratch to point to thread's trapframe virtual address
  // The trampoline needs this to save/restore registers
  w_sscratch(t->trapframe_va);
  
  // Verify sscratch was set correctly
  uint64 sscratch_check = r_sscratch();
  if(sscratch_check != t->trapframe_va) {
    printf("ERROR: sscratch mismatch! set=0x%lx read=0x%lx\n", t->trapframe_va, sscratch_check);
    panic("threadret: sscratch mismatch");
  }

  // Set up user mode
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // CRITICAL: Double-check epc before setting sepc
  if(t->trapframe->epc == 0) {
    printf("CRITICAL ERROR: epc became zero before w_sepc!\n");
    panic("threadret: epc corrupted");
  }

  // CRITICAL: Validate stack pointer is within reasonable bounds
  if(t->trapframe->sp < t->stack_base || t->trapframe->sp > t->stack_base + USERSTACK*PGSIZE + 0x1000) {
    printf("ERROR: corrupted stack pointer! sp=0x%lx stack_base=0x%lx\n", t->trapframe->sp, t->stack_base);
    panic("threadret: stack pointer corrupted");
  }

  // CRITICAL: Final debug before Thread 1 enters user space
  static int thread1_user_entry = 0;
  // if(t->thread_slot == 1 && !thread1_user_entry) {
  //   printf("\\n=== CRITICAL: Thread 1 entering user space ===\\n");
  //   printf("Final EPC: 0x%lx (will be set to sepc)\\n", t->trapframe->epc);
  //   printf("Final SP: 0x%lx\\n", t->trapframe->sp);
  //   printf("Thread slot: %d, TID: %d\\n", t->thread_slot, t->tid);
  //   printf("sscratch: 0x%lx\\n", r_sscratch());
  //   printf("=== Thread 1 should execute thread_func1 at 0x1000 ===\\n\\n");
  //   thread1_user_entry = 1;
  // }

  // Set S Exception Program Counter
  w_sepc(t->trapframe->epc);
  
  // CRITICAL: Verify trapframe->epc after w_sepc
  // if(t->thread_slot == 1) {
  //   printf("THREADRET: After w_sepc, thread_slot=%d tid=%d trapframe->epc=0x%lx sepc=0x%lx\n", 
  //          t->thread_slot, t->tid, t->trapframe->epc, r_sepc());
  // }
  
  // Verify that sepc was set correctly
  uint64 sepc_check = r_sepc();
  if(sepc_check != t->trapframe->epc) {
    printf("ERROR: sepc mismatch! set=0x%lx read=0x%lx\n", t->trapframe->epc, sepc_check);
    panic("threadret: sepc mismatch");
  }
  
  // Return to user space using thread's own trapframe
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  
  // Validate trapframe mapping before jumping
  pte_t *pte = walk(p->pagetable, t->trapframe_va, 0);
  if(!pte || !(*pte & PTE_V)) {
    printf("ERROR: trapframe_va 0x%lx not mapped!\n", t->trapframe_va);
    panic("threadret: trapframe not mapped");
  }
  
  // CRITICAL: Final trapframe check before userret
  // if(t->thread_slot == 1) {
  //   printf("THREADRET: Just before userret jump, thread_slot=%d tid=%d trapframe->epc=0x%lx sepc=0x%lx\n", 
  //          t->thread_slot, t->tid, t->trapframe->epc, r_sepc());
  //   
  //   // CRITICAL: Verify TRAPFRAME2 mapping points to correct physical trapframe
  //   pte_t *pte = walk(p->pagetable, TRAPFRAME2, 0);
  //   uint64 mapped_pa = PTE2PA(*pte);
  //   printf("CRITICAL: TRAPFRAME2 maps to PA=0x%lx, should be 0x%lx\n", 
  //          mapped_pa, (uint64)t->trapframe);
  //   printf("CRITICAL: Main trapframe PA=0x%lx\n", (uint64)p->trapframe);
  // }
  
  // Jump to user space
  // if(t->thread_slot == 1) {
  //   printf("THREADRET: FINAL CHECK - trapframe->epc=0x%lx sepc=0x%lx\n", 
  //          t->trapframe->epc, r_sepc());
  // }
  ((void (*)(uint64))trampoline_userret)(satp);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();
  struct thread *t = mythread();

  // Still holding t->lock from scheduler (not p->lock anymore).
  release(&t->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

void threadret2(void){
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();
  struct thread *t = mythread();

  // Still holding t->lock from scheduler (not p->lock anymore).
  release(&t->lock);

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
 
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct thread *t = mythread();
  
  if(t == 0)
    panic("sleep: no thread");

  // Must acquire t->lock in order to
  // change t->state and then call sched.
  // Once we hold t->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks t->lock),
  // so it's okay to release lk.

  acquire(&t->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  t->chan = chan;
  t->state = T_SLEEPING;

  sched();

  // Tidy up.
  t->chan = 0;

  // Reacquire original lock.
  release(&t->lock);
  acquire(lk);
}

// Wake up all threads sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;
  struct thread *t;

  for(p = proc; p < &proc[NPROC]; p++) {
    for(int i = 0; i < NTHREAD; i++) {
      t = p->threads[i];
      if(t == 0)
        continue;
      if(t->state == T_UNUSED)
        continue;
      
      // During early boot, only wake up properly initialized threads
      if(!threading_initialized && t->magic != 0xDEADBEEFCAFEBABE)
        continue;
      
      // Skip the current thread to avoid deadlock
      if(t == mythread())
        continue;
      
      // Only try to acquire lock if we don't already hold it
      if(holding(&t->lock))
        continue;
        
      acquire(&t->lock);
      if(t->state == T_SLEEPING && t->chan == chan) {
        t->state = T_RUNNABLE;
      }
      release(&t->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;
  struct thread *t;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      // Kill all threads in this process
      for(int i = 0; i < NTHREAD; i++){
        t = p->threads[i];
        if(t == 0)
          continue;
        acquire(&t->lock);
        t->killed = 1;
        if(t->state == T_SLEEPING){
          // Wake thread from sleep().
          t->state = T_RUNNABLE;
        }
        release(&t->lock);
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Thread-specific killed functions
void
setkilled_thread(struct thread *t)
{
  if(t == 0)
    return;
  
  acquire(&t->lock);
  t->killed = 1;
  release(&t->lock);
}

int
killed_thread(struct thread *t)
{
  int k;
  
  if(t == 0)
    return 0;
  
  // Check magic number to detect corruption
  if(t->magic != 0xDEADBEEFCAFEBABE) {
    printf("killed_thread: corrupted thread! t=%p magic=0x%lx expected=0x%lx\n", 
           t, t->magic, 0xDEADBEEFCAFEBABE);
    return 0; // Assume not killed if corrupted
  }
  
  // Don't try to acquire our own lock - the scheduler may be holding it
  if(t == mythread()) {
    return t->killed;  // Safe to read without lock for current thread
  }
  
  //acquire(&t->lock);
  k = t->killed;
  //release(&t->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
