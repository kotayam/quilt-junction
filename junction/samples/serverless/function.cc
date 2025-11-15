#include <fstream>
#include <iostream>
#include <string>

constexpr const char* CHANNEL_PATH = "/serverless/chan0";
constexpr const char* SNAPSHOT_REQ = "SNAPSHOT_PREPARE";
constexpr const char* OK = "OK";

namespace {

std::fstream channel;

bool OpenChannel() {
  channel.open(CHANNEL_PATH);

  if (!channel.is_open()) {
    std::cerr << "Failed to open serverless channel\n";
    return false;
  }
  std::cout << "Function process started. Waiting for requests on "
            << CHANNEL_PATH << std::endl;
  return true;
}

bool Warmup() {
  std::cout << "Handling warmup process\n";
  std::string req_line;
  while (true) {
    if (!std::getline(channel, req_line)) {
      std::cerr << "Failed to read warmup request\n";
      return false;
    }

    std::cout << "Recieved request: " << req_line << "\n";
    if (req_line == SNAPSHOT_REQ) {
      channel << OK;
      std::cout << "Sent snapshot OK response.\n";
    } else {
      channel << "Processed: " << req_line << "\n";
    }
  }
  std::cout << "Completed warmup process\n";
  return true;
}

void Function() {
  std::string req_line;
  while (std::getline(channel, req_line)) {
    std::string res = "Echo: " + req_line;
    channel << res;
  }
}

void CloseChannel() { channel.close(); }

}  // namespace

int main() {
  if (!OpenChannel()) { return 1; }

  Warmup();

  Function();

  CloseChannel();

  return 0;
}
