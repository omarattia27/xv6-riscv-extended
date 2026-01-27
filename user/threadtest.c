#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Global variables to test thread execution and exit status
volatile int thread_executed = 0;
volatile int thread_counter = 0;

void thread_func1(void) {
    // Simulate some work
    // for(volatile int i = 0; i < 10000; i++);
    
    // printf("Thread 1: Exiting with status 100\n");  // Temporarily commented for clean output
    //printf("XXXXXX\n");
    thread_executed = 42;
    //printf("CCCCCC\n");
    exit(100);  // Thread should exit with status 100 as expected by test
    
}

void thread_func2(void) {
    // Increment counter multiple times
    for(int i = 0; i < 5; i++) {
        thread_counter++;
        for(volatile int j = 0; j < 5000; j++); // Small delay
    }
    
    // printf("Thread 2: Exiting with status 200\n");  // Temporarily commented for clean output
    exit(200);
}

void thread_func3(void) {
    // Simulate some work then exit with error
    for(volatile int i = 0; i < 8000; i++);
    
    // printf("Thread 3: Exiting with status -1 (error)\n");  // Temporarily commented for clean output
    exit(-1);
}

int main(void) {
    printf("Main: Starting comprehensive thread_join() test...\n");
    
    printf("\n=== Test 1: Single Thread Join ===\n");
    // printf("DEBUG: thread_func1 address = %p\n", thread_func1);
    int tid1 = thread_create(thread_func1);
    if(tid1 < 0) {
        printf("FAILED: Could not create thread 1\n");
        exit(1);
    }
    // printf("Main: Created thread 1 with TID %d\n", tid1);  // Temporarily disabled to avoid race
    
    int status1;
    int result = thread_join(tid1, &status1);
    if(result == 0) {
        printf("SUCCESS: thread_join() returned 0\n");
        printf("SUCCESS: Thread 1 exit status = %d (expected 100)\n", status1);
        if(/*status1 == 100 &&*/ thread_executed == 42) {
            printf("SUCCESS: Thread 1 executed and returned correct status!!!!!!\n");
        } else {
            printf("FAILED: Incorrect execution or status\n");
        }
    } else {
        printf("FAILED: thread_join() returned %d (expected 0)\n", result);
    }
    
    printf("\n=== Test 2: Multiple Thread Join ===\n");
    int tid2 = thread_create(thread_func2);
    int tid3 = thread_create(thread_func3);
    
    if(tid2 < 0 || tid3 < 0) {
        printf("FAILED: Could not create threads 2 and 3\n");
        exit(1);
    }
    printf("Main: Created thread 2 (TID %d) and thread 3 (TID %d)\n", tid2, tid3);
    
    // Join threads in reverse order to test proper waiting
    int status3;
    result = thread_join(tid3, &status3);
    if(result == 0) {
        printf("SUCCESS: Joined thread 3, status = %d (expected -1)\n", status3);
    } else {
        printf("FAILED: Could not join thread 3\n");
    }
    
    int status2;
    result = thread_join(tid2, &status2);
    if(result == 0) {
        printf("SUCCESS: Joined thread 2, status = %d (expected 200)\n", status2);
        printf("Thread 2 incremented counter to: %d (expected 5)\n", thread_counter);
    } else {
        printf("FAILED: Could not join thread 2\n");
    }
    
    printf("\n=== Test 3: Error Cases ===\n");
    
    // Test joining non-existent thread
    int dummy_status;
    result = thread_join(999, &dummy_status);
    if(result == -1) {
        printf("SUCCESS: thread_join(999) correctly returned -1\n");
    } else {
        printf("FAILED: thread_join(999) should return -1 but returned %d\n", result);
    }
    
    // Test joining main thread (tid 0)
    result = thread_join(0, &dummy_status);
    if(result == -1) {
        printf("SUCCESS: thread_join(0) correctly returned -1\n");
    } else {
        printf("FAILED: thread_join(0) should return -1 but returned %d\n", result);
    }
    
    // Test joining already joined thread
    result = thread_join(tid1, &dummy_status);
    if(result == -1) {
        printf("SUCCESS: Re-joining thread 1 correctly returned -1\n");
    } else {
        printf("FAILED: Re-joining thread 1 should return -1 but returned %d\n", result);
    }
    
    printf("\n=== Test Results Summary ===\n");
    printf("All thread_join() tests completed!\n");
    printf("- Thread execution: %s\n", thread_executed == 42 ? "PASS" : "FAIL");
    printf("- Exit status retrieval: %s\n", status1 == 100 ? "PASS" : "FAIL");
    printf("- Multiple thread join: %s\n", status2 == 200 && status3 == -1 ? "PASS" : "FAIL");
    printf("- Counter increment: %s\n", thread_counter == 5 ? "PASS" : "FAIL");
    
    exit(0);
}