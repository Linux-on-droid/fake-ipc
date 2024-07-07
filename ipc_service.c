#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/tmp/ipc_service_socket"
#define MAX_MSG_SIZE 256
#define MAX_MSG_QUEUE 10

typedef struct {
  long mtype;
  char mtext[MAX_MSG_SIZE];
} message_t;

typedef struct {
  message_t messages[MAX_MSG_QUEUE];
  int head;
  int tail;
  int count;
  sem_t sem_mutex;
  sem_t sem_empty;
  sem_t sem_full;
} msg_queue_t;

msg_queue_t msg_queue;

void init_msg_queue(msg_queue_t *queue) {
  queue->head = 0;
  queue->tail = 0;
  queue->count = 0;
  sem_init(&queue->sem_mutex, 1, 1);
  sem_init(&queue->sem_empty, 1, MAX_MSG_QUEUE);
  sem_init(&queue->sem_full, 1, 0);
}

void enqueue_message(msg_queue_t *queue, message_t *msg) {
  sem_wait(&queue->sem_empty);
  sem_wait(&queue->sem_mutex);

  memcpy(&queue->messages[queue->tail], msg, sizeof(message_t));
  queue->tail = (queue->tail + 1) % MAX_MSG_QUEUE;
  queue->count++;

  sem_post(&queue->sem_mutex);
  sem_post(&queue->sem_full);
}

void dequeue_message(msg_queue_t *queue, message_t *msg) {
  sem_wait(&queue->sem_full);
  sem_wait(&queue->sem_mutex);

  memcpy(msg, &queue->messages[queue->head], sizeof(message_t));
  queue->head = (queue->head + 1) % MAX_MSG_QUEUE;
  queue->count--;

  sem_post(&queue->sem_mutex);
  sem_post(&queue->sem_empty);
}

void handle_client(int client_sock) {
  char buffer[MAX_MSG_SIZE];
  while (1) {
    int bytes_read = read(client_sock, buffer, sizeof(buffer));
    if (bytes_read <= 0)
      break;

    if (strncmp(buffer, "SEND", 4) == 0) {
      message_t msg;
      memcpy(&msg, buffer + 4, sizeof(message_t));
      enqueue_message(&msg_queue, &msg);
    } else if (strncmp(buffer, "RECV", 4) == 0) {
      message_t msg;
      dequeue_message(&msg_queue, &msg);
      write(client_sock, &msg, sizeof(message_t));
    }
  }
  close(client_sock);
}

void *ipc_service(void *arg) {
  int server_sock, client_sock;
  struct sockaddr_un server_addr;

  server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_sock < 0) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, SOCKET_PATH, sizeof(server_addr.sun_path) - 1);
  unlink(SOCKET_PATH);

  if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    perror("bind");
    close(server_sock);
    exit(EXIT_FAILURE);
  }

  if (listen(server_sock, 5) < 0) {
    perror("listen");
    close(server_sock);
    exit(EXIT_FAILURE);
  }

  while (1) {
    client_sock = accept(server_sock, NULL, NULL);
    if (client_sock < 0) {
      perror("accept");
      continue;
    }
    handle_client(client_sock);
  }

  close(server_sock);
  return NULL;
}

int main() {
  init_msg_queue(&msg_queue);

  pthread_t service_thread;
  if (pthread_create(&service_thread, NULL, ipc_service, NULL) != 0) {
    perror("pthread_create");
    exit(EXIT_FAILURE);
  }

  pthread_join(service_thread, NULL);
  return 0;
}
