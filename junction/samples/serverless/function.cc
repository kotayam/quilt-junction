#include <array>
#include <fstream>
#include <iostream>

constexpr const char* CHANNEL_PATH = "/serverless/chan0";
constexpr size_t BUF_SIZE = 1024;
constexpr int WARMUP_COUNT = 10;

namespace {

std::fstream channel;
std::array<char, BUF_SIZE> read_buf;

bool OpenChannel() {
  channel.open(CHANNEL_PATH);

  if (!channel.is_open()) {
    std::cerr << "Failed to open serverless channel\n";
    return false;
  }
  std::cout << "Function process started. Waiting for requests on "
            << CHANNEL_PATH << "\n";
  return true;
}

bool Warmup() {
  std::cout << "Handling warmup process\n";
  int i = 0;
  while (i < WARMUP_COUNT) {
    channel.read(read_buf.data(), read_buf.size());
    std::streamsize bytes_read = channel.gcount();

    if (bytes_read <= 0) {
      std::cerr << "Failed to read warmup request\n";
      return false;
    }
    std::cout << "Recieved request: " << read_buf.data() << "\n";
    i++;
  }
  return true;
}

void CloseChannel() { channel.close(); }

}  // namespace

int main() {
  if (!OpenChannel()) { return 1; }

  Warmup();

  CloseChannel();

  return 0;
}
