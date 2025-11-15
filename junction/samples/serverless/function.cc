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
    std::cerr << "Failed to open serverless channel" << std::endl;
    return false;
  }
  std::cout << "Function process started. Waiting for requests on "
            << CHANNEL_PATH << std::endl;
  return true;
}

bool Warmup() {
  std::cout << "Handling warmup process" << std::endl;
  std::string req_line;
  while (true) {
    if (!std::getline(channel, req_line)) {
      std::cerr << "Failed to read warmup request" << std::endl;
      return false;
    }

    std::cout << "Recieved request: " << req_line << std::endl;
    if (req_line == SNAPSHOT_REQ) {
      channel << OK;
      std::cout << "Sent snapshot OK response." << std::endl;
      break;
    }
    channel << "Processed: " << req_line;
  }
  std::cout << "Completed warmup process" << std::endl;
  return true;
}

bool Function() {
  std::cout << "Waiting for request..." << std::endl;
  std::string req_line;
  while (true) {
    if (!std::getline(channel, req_line)) {
      std::cerr << "Failed to read request" << std::endl;
      return false;
    }
    std::cout << "Recieved request: " << req_line << std::endl;
    std::string res = "Echo: " + req_line;
    channel << res;
    std::cout << "Processed: " << req_line << std::endl;
  }
  return true;
}

void CloseChannel() { channel.close(); }

}  // namespace

int main() {
  if (!OpenChannel()) { return 1; }

  if (!Warmup()) {
    CloseChannel();
    return 1;
  }

  if (!Function()) {
    CloseChannel();
    return 1;
  }

  CloseChannel();

  return 0;
}
