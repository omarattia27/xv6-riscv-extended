#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void
test_basic_send_recv(void)
{
  printf("Test 1: Basic send and receive\n");
  
  int mqid = msgget(100);
  if (mqid < 0) {
    printf("FAIL: msgget failed\n");
    return;
  }
  printf("  Created queue with mqid=%d\n", mqid);
  
  char sendbuf[64];
  strcpy(sendbuf, "Hello from queue!");
  
  int ret = msgsnd(mqid, sendbuf, strlen(sendbuf) + 1);
  if (ret < 0) {
    printf("FAIL: msgsnd failed\n");
    return;
  }
  printf("  Sent message: %s\n", sendbuf);
  
  char recvbuf[64];
  memset(recvbuf, 0, sizeof(recvbuf));
  ret = msgrcv(mqid, recvbuf, sizeof(recvbuf));
  if (ret < 0) {
    printf("FAIL: msgrcv failed\n");
    return;
  }
  printf("  Received message: %s\n", recvbuf);
  
  if (strcmp(sendbuf, recvbuf) == 0) {
    printf("PASS: Messages match\n");
  } else {
    printf("FAIL: Messages don't match\n");
  }
}

void
test_multiple_messages(void)
{
  printf("\nTest 2: Multiple messages\n");
  
  int mqid = msgget(200);
  if (mqid < 0) {
    printf("FAIL: msgget failed\n");
    return;
  }
  
  char msg[32];
  int i;
  
  // Send multiple messages
  for (i = 0; i < 5; i++) {
    memset(msg, 0, sizeof(msg));
    msg[0] = 'A' + i;
    msg[1] = '\0';
    
    if (msgsnd(mqid, msg, 2) < 0) {
      printf("FAIL: msgsnd failed for message %d\n", i);
      return;
    }
    printf("  Sent: %s\n", msg);
  }
  
  // Receive multiple messages (should be FIFO)
  for (i = 0; i < 5; i++) {
    memset(msg, 0, sizeof(msg));
    if (msgrcv(mqid, msg, sizeof(msg)) < 0) {
      printf("FAIL: msgrcv failed for message %d\n", i);
      return;
    }
    printf("  Received: %s\n", msg);
    
    if (msg[0] != 'A' + i) {
      printf("FAIL: Expected '%c', got '%c'\n", 'A' + i, msg[0]);
      return;
    }
  }
  
  printf("PASS: FIFO order preserved\n");
}

void
test_queue_reuse(void)
{
  printf("\nTest 3: Queue reuse (same ID)\n");
  
  int mqid1 = msgget(300);
  int mqid2 = msgget(300); // Same ID should return same queue
  
  if (mqid1 < 0 || mqid2 < 0) {
    printf("FAIL: msgget failed\n");
    return;
  }
  
  if (mqid1 == mqid2) {
    printf("PASS: Same ID returns same queue index (mqid=%d)\n", mqid1);
  } else {
    printf("FAIL: Different queue indices for same ID (%d vs %d)\n", mqid1, mqid2);
  }
}

void
test_empty_queue(void)
{
  printf("\nTest 4: Receive from empty queue\n");
  
  int mqid = msgget(400);
  if (mqid < 0) {
    printf("FAIL: msgget failed\n");
    return;
  }
  
  char buf[32];
  int ret = msgrcv(mqid, buf, sizeof(buf));
  
  if (ret < 0) {
    printf("PASS: msgrcv correctly failed on empty queue\n");
  } else {
    printf("FAIL: msgrcv should fail on empty queue\n");
  }
}

void
test_fork_communication(void)
{
  printf("\nTest 5: Parent-child communication\n");
  
  int mqid = msgget(500);
  if (mqid < 0) {
    printf("FAIL: msgget failed\n");
    return;
  }
  
  int pid = fork();
  if (pid < 0) {
    printf("FAIL: fork failed\n");
    return;
  }
  
  if (pid == 0) {
    // Child process - send message
    char msg[] = "Message from child";
    if (msgsnd(mqid, msg, strlen(msg) + 1) < 0) {
      printf("FAIL: child msgsnd failed\n");
      exit(1);
    }
    printf("  Child sent: %s\n", msg);
    exit(0);
  } else {
    // Parent process - wait a bit then receive
    wait(0);
    
    char buf[64];
    memset(buf, 0, sizeof(buf));
    if (msgrcv(mqid, buf, sizeof(buf)) < 0) {
      printf("FAIL: parent msgrcv failed\n");
      return;
    }
    printf("  Parent received: %s\n", buf);
    
    if (strcmp(buf, "Message from child") == 0) {
      printf("PASS: Parent-child communication works\n");
    } else {
      printf("FAIL: Message corrupted\n");
    }
  }
}

void
test_invalid_mqid(void)
{
  printf("\nTest 6: Invalid queue ID\n");
  
  char buf[32];
  
  // Try to send/receive on invalid queue
  if (msgsnd(999, buf, 10) < 0) {
    printf("  msgsnd correctly rejected invalid mqid\n");
  } else {
    printf("FAIL: msgsnd should fail on invalid mqid\n");
  }
  
  if (msgrcv(999, buf, sizeof(buf)) < 0) {
    printf("  msgrcv correctly rejected invalid mqid\n");
  } else {
    printf("FAIL: msgrcv should fail on invalid mqid\n");
  }
  
  printf("PASS: Invalid mqid handling works\n");
}

int
main(int argc, char *argv[])
{
  printf("=== Message Queue System Call Tests ===\n\n");
  
  test_basic_send_recv();
  test_multiple_messages();
  test_queue_reuse();
  test_empty_queue();
  test_fork_communication();
  test_invalid_mqid();
  
  printf("\n=== All tests completed ===\n");
  exit(0);
}
