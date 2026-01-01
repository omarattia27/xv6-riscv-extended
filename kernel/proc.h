// Saved registers for kernel context switches.
struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// Per-CPU state.
struct cpu {
  struct proc *proc;          // The process running on this cpu, or null.
  struct thread *thread;      // The thread running on this cpu, or null.
  struct context context;     // swtch() here to enter scheduler().
  int noff;                   // Depth of push_off() nesting.
  int intena;                 // Were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];

// per-process data for the trap handling code in trampoline.S.
// sits in a page by itself just under the trampoline page in the
// user page table. not specially mapped in the kernel page table.
// uservec in trampoline.S saves user registers in the trapframe,
// then initializes registers from the trapframe's
// kernel_sp, kernel_hartid, kernel_satp, and jumps to kernel_trap.
// usertrapret() and userret in trampoline.S set up
// the trapframe's kernel_*, restore user registers from the
// trapframe, switch to the user page table, and enter user space.
// the trapframe includes callee-saved user registers like s0-s11 because the
// return-to-user path via usertrapret() doesn't return through
// the entire kernel call stack.
struct trapframe {
  /*   0 */ uint64 kernel_satp;   // kernel page table
  /*   8 */ uint64 kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64 kernel_trap;   // usertrap()
  /*  24 */ uint64 epc;           // saved user program counter
  /*  32 */ uint64 kernel_hartid; // saved kernel tp
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

#define NTHREAD 3 // Maximum number of threads per process

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  struct spinlock lock;        // Protects process lifecycle (state, pid, xstate)
  struct spinlock mem_lock;    // Protects address space (sz, pagetable modifications)
  struct spinlock fd_lock;     // Protects file descriptor table (ofile[])

  // p->lock must be held when using these:
  enum procstate state;        // Process state (UNUSED, USED, ZOMBIE)
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID

  // wait_lock must be held when using this:
  struct proc *parent;         // Parent process

  // p->mem_lock must be held when modifying these:
  uint64 sz;                   // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table

  // these are private to the process, no lock needed:
  uint64 kstack;               // Virtual address of kernel stack (unused, threads have their own)
  struct trapframe *trapframe; // data page for trampoline.S (thread 0)
  struct trapframe *trapframe2; // trapframe for thread 1
  struct trapframe *trapframe3; // trapframe for thread 2
  struct context context;      // swtch() context (unused, threads have their own)
  
  // p->fd_lock must be held when modifying these:
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  
  char name[16];               // Process name (debugging)

  // Threads within the process (allocated once, never freed)
  struct thread *threads[NTHREAD]; // Threads of the process
};

// Per-thread state
enum threadstate { T_UNUSED, T_USED, T_SLEEPING, T_RUNNABLE, T_RUNNING, T_ZOMBIE };
struct thread {
  struct spinlock lock;

  // t->lock must be held when using these:
  enum threadstate state;      // Thread state
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, thread has been killed
  int xstate;                  // Exit status to be returned to parent's thread_join
  int tid;                     // Thread ID

  // Scheduling fields
  int cpu_ticks;               // Number of ticks thread has run
  int time_slices_left;        // Time slice allocated in the CPU during current round
  int age_in_low_queue;        // Age of the thread in the low priority queue
  int age_in_high_queue;       // Age of the thread in the high priority queue
  int priority;                // Thread priority
  int in_queue;                // If non-zero, thread is in a scheduling queue

  // Thread execution context
  struct context context;      // swtch() here to run thread
  struct trapframe *trapframe; // per-thread trapframe (physical page)
  uint64 trapframe_va;         // user VA of trapframe (TRAPFRAME/TRAPFRAME2/TRAPFRAME3)
  uint64 kstack;               // Virtual address of thread's kernel stack
  uint64 stack_base;           // User stack base address

  // Parent process
  struct proc *proc;           // Owning process
  
  // Magic number for detecting corruption
  uint64 magic;                // Should always be 0xDEADBEEFCAFEBABE
};