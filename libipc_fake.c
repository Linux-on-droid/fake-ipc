#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/tmp/ipc_service_socket"
#define MAX_FAKE_SHM_SEGMENTS 10
#define MAX_FAKE_MSG_SIZE 256

typedef struct {
  key_t key;
  size_t size;
  char name[32];
  void *address;
} fake_shm_segment_t;

typedef struct {
  long mtype;
  char mtext[MAX_FAKE_MSG_SIZE];
} fake_msg_t;

static fake_shm_segment_t fake_shm_segments[MAX_FAKE_SHM_SEGMENTS];

__attribute__((constructor)) void init() {
  printf("libipc_fake.so loaded\n");
  for (int i = 0; i < MAX_FAKE_SHM_SEGMENTS; ++i) {
    fake_shm_segments[i].key = -1;
    fake_shm_segments[i].address = NULL;
  }
}

int shmget(key_t key, size_t size, int shmflg) {
  printf("shmget intercepted: key=%d, size=%zu, shmflg=%d\n", key, size,
         shmflg);
  for (int i = 0; i < MAX_FAKE_SHM_SEGMENTS; ++i) {
    if (fake_shm_segments[i].key == -1) {
      fake_shm_segments[i].key = key;
      fake_shm_segments[i].size = size;
      snprintf(fake_shm_segments[i].name, sizeof(fake_shm_segments[i].name),
               "/fake_shm_%d_%d", getpid(), i);
      int fd = shm_open(fake_shm_segments[i].name, O_CREAT | O_RDWR, 0666);
      if (fd == -1) {
        perror("shm_open");
        fake_shm_segments[i].key = -1;
        return -1;
      }
      if (ftruncate(fd, size) == -1) {
        perror("ftruncate");
        close(fd);
        shm_unlink(fake_shm_segments[i].name);
        fake_shm_segments[i].key = -1;
        return -1;
      }
      fake_shm_segments[i].address =
          mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      close(fd);
      if (fake_shm_segments[i].address == MAP_FAILED) {
        perror("mmap");
        shm_unlink(fake_shm_segments[i].name);
        fake_shm_segments[i].key = -1;
        return -1;
      }
      return i;
    }
  }
  errno = ENOSPC;
  return -1;
}

void *shmat(int shmid, const void *shmaddr, int shmflg) {
  printf("shmat intercepted: shmid=%d, shmaddr=%p, shmflg=%d\n", shmid, shmaddr,
         shmflg);
  if (shmid >= 0 && shmid < MAX_FAKE_SHM_SEGMENTS &&
      fake_shm_segments[shmid].address != NULL) {
    return fake_shm_segments[shmid].address;
  }
  errno = EINVAL;
  return (void *)-1;
}

int shmctl(int shmid, int cmd, struct shmid_ds *buf) {
  printf("shmctl intercepted: shmid=%d, cmd=%d\n", shmid, cmd);
  if (shmid >= 0 && shmid < MAX_FAKE_SHM_SEGMENTS &&
      fake_shm_segments[shmid].address != NULL) {
    if (cmd == IPC_RMID) {
      munmap(fake_shm_segments[shmid].address, fake_shm_segments[shmid].size);
      shm_unlink(fake_shm_segments[shmid].name);
      fake_shm_segments[shmid].key = -1;
      fake_shm_segments[shmid].address = NULL;
    }
    return 0;
  }
  errno = EINVAL;
  return -1;
}

int shmdt(const void *shmaddr) {
  printf("shmdt intercepted: shmaddr=%p\n", shmaddr);
  for (int i = 0; i < MAX_FAKE_SHM_SEGMENTS; ++i) {
    if (fake_shm_segments[i].address == shmaddr) {
      return 0;
    }
  }
  errno = EINVAL;
  return -1;
}

int connect_to_service() {
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect");
    close(sock);
    return -1;
  }

  return sock;
}

int msgget(key_t key, int msgflg) {
  printf("msgget intercepted: key=%d, msgflg=%d\n", key, msgflg);
  return 0;
}

int msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg) {
  printf("msgsnd intercepted: msqid=%d, msgsz=%zu, msgflg=%d\n", msqid, msgsz,
         msgflg);
  int sock = connect_to_service();
  if (sock < 0) {
    return -1;
  }

  char buffer[sizeof("SEND") + sizeof(fake_msg_t)];
  memcpy(buffer, "SEND", 4);
  memcpy(buffer + 4, msgp, msgsz);

  if (write(sock, buffer, sizeof(buffer)) < 0) {
    perror("write");
    close(sock);
    return -1;
  }

  close(sock);
  return 0;
}

ssize_t msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg) {
  printf("msgrcv intercepted: msqid=%d, msgsz=%zu, msgtyp=%ld, msgflg=%d\n",
         msqid, msgsz, msgtyp, msgflg);
  int sock = connect_to_service();
  if (sock < 0) {
    return -1;
  }

  char buffer[sizeof("RECV")];
  memcpy(buffer, "RECV", 4);

  if (write(sock, buffer, sizeof(buffer)) < 0) {
    perror("write");
    close(sock);
    return -1;
  }

  fake_msg_t msg;
  if (read(sock, &msg, sizeof(fake_msg_t)) < 0) {
    perror("read");
    close(sock);
    return -1;
  }

  memcpy(msgp, &msg, msgsz);

  close(sock);
  return msgsz;
}

int msgctl(int msqid, int cmd, struct msqid_ds *buf) {
  printf("msgctl intercepted: msqid=%d, cmd=%d\n", msqid, cmd);
  return 0;
}
