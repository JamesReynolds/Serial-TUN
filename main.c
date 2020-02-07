#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <net/if.h>
#include <libserialport.h>
#include <pthread.h>
#include <linux/if_tun.h>

#include "slip.h"
#include "tun-driver.h"

struct CommDevices {
    int tunFileDescriptor;
    struct sp_port *serialPort;
};

char adapterName[IFNAMSIZ];

char serialPortName[128];
int serialBaudRate = 9600;

#define BUFFER (16384)

/**
 * Handles getting packets from the serial port and writing them to the TUN interface
 * @param ptr       - Pointer to the CommDevices struct
 */
static void *serialToTun(void *ptr) {
  // Grab thread parameters
  struct CommDevices *args = ptr;

  int tunFd = args->tunFileDescriptor;
  struct sp_port *serialPort = args->serialPort;

  // Create two buffers, one to store raw data from the serial port and
  // one to store SLIP frames
  unsigned char inBuffer[BUFFER];
  unsigned char outBuffer[2 * BUFFER] = {0};
  unsigned long outSize = 0;
  int inIndex = 0;

  // Incoming byte count
  size_t count;

  // Serial result
  enum sp_return serialResult;

  // Add 'RX ready' event to serial port
  struct sp_event_set *eventSet;
  sp_new_event_set(&eventSet);
  sp_add_port_events(eventSet, serialPort, SP_EVENT_RX_READY);

  while (1) {
    // Wait for the event (RX Ready)
    sp_wait(eventSet, 0);
    count = sp_input_waiting(serialPort); // Bytes ready for reading
    if (count + inIndex > sizeof(inBuffer)) count = sizeof(inBuffer) - inIndex;
    do_debug("Received %d bytes on serial\n", count);

    // Read bytes from serial
    serialResult = sp_blocking_read(serialPort, &inBuffer[inIndex], count, 0);

    if (serialResult < 0) {
      fprintf(stderr, "Serial error! %d\n", serialResult);
      return NULL;
    }
    // We need to check if there is an SLIP_END sequence in the new bytes
    unsigned char *slip;
    unsigned char *buffer = inBuffer;
    inIndex += count;
    do_debug("Serial buffer: %d\n", (100 * inIndex) / sizeof(inBuffer));
    while(inIndex > 0 && (slip = memchr(buffer, SLIP_END, inIndex)) != NULL) {
      slip_decode(buffer, slip - buffer, outBuffer, sizeof(outBuffer), &outSize);

      // Write the packet to the virtual interface
      int writeResult = write(tunFd, outBuffer, outSize);
      do_debug("Wrote %d bytes on tun\n", outSize);
      if (writeResult < 0) {
          fprintf(stderr, "Tun error! %s(%d)\n", strerror(errno), errno);
      }
      inIndex -= slip - buffer + 1;
      buffer = slip + 1;
    }

    // Copy the remaining data (belonging to the next packet)
    // to the start of the buffer
    memcpy(inBuffer, buffer, inIndex + 1);

  }
  return 0;
}

static void *tunToSerial(void *ptr) {
  // Grab thread parameters
  struct CommDevices *args = ptr;

  int tunFd = args->tunFileDescriptor;
  struct sp_port *serialPort = args->serialPort;

  // Create TUN buffer
  unsigned char inBuffer[BUFFER];
  unsigned char outBuffer[BUFFER * 2];

  // Incoming byte count
  ssize_t count;

  // Encoded data size
  unsigned long encodedLength = 0;

  // Serial error messages
  enum sp_return serialResult;

  while (1) {
    count = read(tunFd, inBuffer, sizeof(inBuffer));
    if (count < 0) {
      fprintf(stderr, "Could not read from interface\n");
    }
    do_debug("Received %d bytes on tun\n", count);

    // Encode data
    slip_encode(inBuffer, (unsigned long) count, outBuffer, BUFFER * 2, &encodedLength);

    // Write to serial port
    serialResult = sp_nonblocking_write(serialPort, outBuffer, encodedLength);
    do_debug("Wrote %d bytes on serial\n", encodedLength);
    if (serialResult < 0) {
      fprintf(stderr, "Could not send data to serial port: %d\n", serialResult);
    }
  }
  return NULL;
}

int main(int argc, char *argv[]) {
  // Grab parameters
  int param;
  while ((param = getopt(argc, argv, "i:p:b:d")) > 0) {
    switch (param) {
      case 'i':
        strncpy(adapterName, optarg, IFNAMSIZ - 1);
        break;
      case 'p':
        strncpy(serialPortName, optarg, sizeof(serialPortName) - 1);
        break;
      case 'b':
        serialBaudRate = atoi(optarg);
        break;
      case 'd':
        debug = 1;
        break;	
      default:
        fprintf(stderr, "Unknown parameter %c\n", param);
        break;
    }
  }

  if (adapterName[0] == '\0') {
    fprintf(stderr, "Adapter name required (-i)\n");
    return EXIT_FAILURE;
  }
  if (serialPortName[0] == '\0') {
    fprintf(stderr, "Serial port required (-p)\n");
    return EXIT_FAILURE;
  }

  int tunFd = tun_alloc(adapterName, IFF_TUN | IFF_NO_PI);
  if (tunFd < 0) {
    fprintf(stderr, "Could not open /dev/net/tun\n");
    return EXIT_FAILURE;
  }

  // Configure & open serial port
  struct sp_port *serialPort;
  sp_get_port_by_name(serialPortName, &serialPort);

  enum sp_return status = sp_open(serialPort, SP_MODE_READ_WRITE);

  sp_set_bits(serialPort, 8);
  sp_set_parity(serialPort, SP_PARITY_NONE);
  sp_set_stopbits(serialPort, 1);
  sp_set_baudrate(serialPort, serialBaudRate);
  sp_set_xon_xoff(serialPort, SP_XONXOFF_DISABLED);
  sp_set_flowcontrol(serialPort, SP_FLOWCONTROL_NONE);

  if (status < 0) {
    fprintf(stderr, "Could not open serial port: ");
    switch (status) {
      case SP_ERR_ARG:
        fprintf(stderr, "Invalid argument\n");
        break;
      case SP_ERR_FAIL:
        fprintf(stderr, "System error\n");
        break;
      case SP_ERR_MEM:
        fprintf(stderr, "Memory allocation error\n");
        break;
      case SP_ERR_SUPP:
        fprintf(stderr, "Operation not supported by device\n");
        break;
      default:
        fprintf(stderr, "Unknown error\n");
        break;
    }
    return EXIT_FAILURE;
  }

  // Create threads
  pthread_t tun2serial, serial2tun;
  int ret1, ret2;

  struct CommDevices threadParams;
  threadParams.tunFileDescriptor = tunFd;
  threadParams.serialPort = serialPort;

  printf("Starting threads\n");
  ret1 = pthread_create(&tun2serial, NULL, tunToSerial, (void *) &threadParams);
  ret2 = pthread_create(&serial2tun, NULL, serialToTun, (void *) &threadParams);

  pthread_join(tun2serial, NULL);
  printf("Thread tun-to-network returned %d\n", ret1);
  pthread_join(serial2tun, NULL);
  printf("Thread network-to-tun returned %d\n", ret2);
}
