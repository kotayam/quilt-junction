#include <fstream>
#include <iostream>
#include <string>

// The "magic" file that the Junction serverless implementation provides.
#define CHANNEL_PATH "/serverless/chan0"

int main() {
  std::string line;
  char read_buf[1024];
  char write_buf[1024];
  ssize_t bytes_read;
  int fd;

  // Open the channel for both reading and writing.
  // This is the C equivalent of the app getting its "port".
  fd = open(CHANNEL_PATH, O_RDWR);
  if (fd < 0) {
    perror("Failed to open serverless channel");
    return 1;
  }

  printf("Function process started. Waiting for requests on %s\n",
         CHANNEL_PATH);

  // This is the main serverless loop, processing one request at a time.
  while (1) {
    // Clear the read buffer
    memset(read_buf, 0, sizeof(read_buf));

    // 1. READ REQUEST
    // This read() call will BLOCK and sleep until the junction
    // runtime provides a request.
    printf("Waiting for next request...\n");
    bytes_read = read(fd, read_buf, sizeof(read_buf) - 1);

    if (bytes_read <= 0) {
      // Error or channel closed
      perror("Error reading from channel");
      break;
    }

    // The 'serverless.cc' code adds a newline. We'll strip it.
    if (read_buf[bytes_read - 1] == '\n') { read_buf[bytes_read - 1] = '\0'; }

    printf("Received request: '%s'\n", read_buf);

    // 2. PROCESS LOGIC
    // Our "business logic" is to create a greeting string.
    // snprintf(write_buf, sizeof(write_buf), "Hello, %s!", read_buf);
    sprintf(write_buf, "OK");

    // 3. WRITE RESPONSE
    // This write() call sends the response back to the junction
    // runtime and unblocks the waiting ChannelWorker.
    printf("Sending response: '%s'\n", write_buf);
    write(fd, write_buf, strlen(write_buf));
  }

  close(fd);
  return 0;
}
