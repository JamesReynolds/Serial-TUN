#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <net/if.h>
#include <pthread.h>
#include <linux/if_tun.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "slip.h"
#include "tun-driver.h"

struct PipeCommDevices {
    int tunFileDescriptor;
    int readFileDescriptor;
    int writeFileDescriptor;
};

#define PATH_MAX 4096
#define BUFFER 4096

char adapterName[IFNAMSIZ];
char pipePortName[PATH_MAX];

static int usage(char *prog) {
    if (strchr(prog, '/')) prog = strchr(prog, '/');
    fprintf(stderr, "Usage: %s -i <adapter name> -p <pipe prefix> [-r] [-d]\n", prog);
    fprintf(stderr, "Connect to <pipe prefix>.in and <pipe prefix>.out for\n");
    fprintf(stderr, "reading and writing to <adapter name> respectively\n");
    fprintf(stderr, "-r = reverse pipe read/write\n");
    fprintf(stderr, "-d = debug\n");
    return EXIT_FAILURE;
}

/**
 * Handles getting packets from the pipe and writing them to the TUN interface
 * @param ptr       - Pointer to the PipeCommDevices struct
 */
static void *pipeToTun(void *ptr) {
  // Grab thread parameters
  struct PipeCommDevices *args = ptr;

  int tunFd = args->tunFileDescriptor;
  int readFd = args->readFileDescriptor;

  // Create two buffers, one to store raw data from the pipe and
  // one to store SLIP frames
  unsigned char inBuffer[BUFFER];
  unsigned char outBuffer[BUFFER] = {0};
  unsigned long outSize = 0;
  int inIndex = 0;

  // Incoming byte count
  long count;

  // Set blocking
  int flags = fcntl(readFd, F_GETFL, 0);
  fcntl(readFd, F_SETFL, flags & ~O_NONBLOCK);

  while (1) {
    // Read bytes from pipe
    count = read(readFd, &inBuffer[inIndex], sizeof(inBuffer) - inIndex);
    if (count > 0) {
      do_debug("Received %d bytes on pipe\n", count);
    }

    if (count < 0) {
      fprintf(stderr, "Pipe error! %s (%d)\n", strerror(errno), errno);
      return NULL;
    }
    // We need to check if there are SLIP_END sequences in the new bytes
    unsigned char *slip;
    unsigned char *buffer = inBuffer;
    inIndex += count;
    while(inIndex > 0 && (slip = memchr(buffer, SLIP_END, inIndex)) != NULL) {
      slip_decode(buffer, slip - buffer, outBuffer, sizeof(outBuffer), &outSize);

      // Write the packet to the virtual interface
      int writeResult = write(tunFd, outBuffer, outSize);
      do_debug("Wrote %d bytes on tun\n", outSize);
      if (writeResult < 0) {
          fprintf(stderr, "Tun error! %d\n", errno);
      }
      inIndex -= slip - buffer + 1;
      buffer = slip + 1;
    }

    // Copy the remaining data (belonging to the next packet)
    // to the start of the buffer
    memcpy(inBuffer, buffer, inIndex + 1);
  }
  return NULL;
}

static void *tunToPipe(void *ptr) {
  // Grab thread parameters
  struct PipeCommDevices *args = ptr;

  int tunFd = args->tunFileDescriptor;
  int writeFd = args->writeFileDescriptor;

  // Create TUN buffer
  unsigned char inBuffer[BUFFER];
  unsigned char outBuffer[BUFFER * 2];

  // Incoming byte count
  ssize_t count;

  // Encoded data size
  unsigned long encodedLength = 0;

  // Set blocking
  int flags = fcntl(writeFd, F_GETFL, 0);
  fcntl(writeFd, F_SETFL, flags & ~O_NONBLOCK);

  while (1) {
    count = read(tunFd, inBuffer, sizeof(inBuffer));
    do_debug("Received %d bytes on tun\n", count);
    if (count < 0) {
      fprintf(stderr, "Tun error! %d\n", errno);
    }

    // Encode data
    slip_encode(inBuffer, (unsigned long) count, outBuffer, sizeof(outBuffer), &encodedLength);

    // Write to pipe port
    int pipeResult = write(writeFd, outBuffer, encodedLength);
    do_debug("Wrote %d bytes to pipe\n", encodedLength);
    if (pipeResult < 0) {
      fprintf(stderr, "Pipe error! %d\n", errno);
    }
  }
  return NULL;
}

int main(int argc, char *argv[]) {
  // Grab parameters
  int param;
  int reverse = 0;
  while ((param = getopt(argc, argv, "i:p:dr")) > 0) {
    switch (param) {
      case 'i':
        strncpy(adapterName, optarg, IFNAMSIZ - 1);
        break;
      case 'p':
        strncpy(pipePortName, optarg, sizeof(pipePortName) - 1);
        break;
      case 'd':
	debug = 1;
	break;
      case 'r':
	reverse = 1;
	break;
      default:
        fprintf(stderr, "Unknown parameter %c\n", param);
        break;
    }
  }

  if (adapterName[0] == '\0' || pipePortName[0] == '\0') {
    return usage(argv[0]);
  }

  do_debug("Creating tun adapter\n");
  int tunFd = tun_alloc(adapterName, IFF_TUN | IFF_NO_PI);
  if (tunFd < 0) {
    fprintf(stderr, "Could not open /dev/net/tun\n");
    return EXIT_FAILURE;
  }

  char readName[PATH_MAX];
  char writeName[PATH_MAX];
  strncpy(readName, pipePortName, sizeof(pipePortName) - 1);
  strncpy(writeName, pipePortName, sizeof(pipePortName) - 1);
  if (reverse) {
    strncat(readName, ".in", 5);
    strncat(writeName, ".out", 5);
  } else {
    strncat(readName, ".out", 5);
    strncat(writeName, ".in", 5);
  }

  do_debug("Opening read pipe: %s\n", readName);
  int readFd = open(readName, O_RDONLY|O_NONBLOCK);
  if (readFd == -1) {
    fprintf(stderr, "Failed to open '%s': %s (%d)\n", readName, strerror(errno), errno);
  }

  do_debug("Opening write pipe: %s\n", writeName);
  int writeFd = open(writeName, O_WRONLY);
  if (writeFd == -1) {
    fprintf(stderr, "Failed to open '%s': %s (%d)\n", writeName, strerror(errno), errno);
  }
  if (readFd == -1 || writeFd == -1) {
    return 1;	 
  }

  // Create threads
  pthread_t tun2pipe, pipe2tun;
  int ret1, ret2;

  struct PipeCommDevices threadParams;
  threadParams.tunFileDescriptor = tunFd;
  threadParams.readFileDescriptor = readFd;
  threadParams.writeFileDescriptor = writeFd;

  do_debug("Starting threads\n");
  ret1 = pthread_create(&tun2pipe, NULL, tunToPipe, (void *) &threadParams);
  ret2 = pthread_create(&pipe2tun, NULL, pipeToTun, (void *) &threadParams);

  pthread_join(tun2pipe, NULL);
  printf("Thread tun-to-network returned %d\n", ret1);
  pthread_join(pipe2tun, NULL);
  printf("Thread network-to-tun returned %d\n", ret2);
}
