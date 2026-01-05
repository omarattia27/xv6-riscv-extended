#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Global variable to test if thread executes
volatile int thread_executed = 0;

void thread_func1(void) {
    // Use a simple write to test if thread actually starts
    // This avoids printf complications
    
    // Write to global variable to indicate execution - no printf to avoid locks
    thread_executed = 42;
    
    // Try to write a simple message directly to avoid printf
    write(1, "Thread: executing!\n", 19);
    
    exit(0);
}

int main(void) {
    printf("Main: Starting thread test...\n");
    
    printf("\n=== Test 1: Single Thread ===\n");
    int tid1 = thread_create(thread_func1);
    if(tid1 < 0) {
        printf("Failed to create thread 1\n");
        exit(1);
    }
    printf("Main: Created thread 1 with TID %d\n", tid1);
    
    // Give the thread time to execute before we exit
    printf("Main: Waiting a moment for thread execution...\n");
    for(volatile int i = 0; i < 1000000000; i++); // Simple delay loop
    
    // Check if thread executed by looking at global variable
    if(thread_executed == 42) {
        printf("SUCCESS: Thread executed successfully!\n");
    } else {
        printf("FAILED: Thread did not execute (thread_executed=%d)\n", thread_executed);
    }
    
    printf("Main: Test completed successfully!\n");
    exit(0);
}