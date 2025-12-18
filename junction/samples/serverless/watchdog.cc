#include "watchdog.h"

#include <iostream>
#include <sstream>
#include <string>
#include <thread>

constexpr const char *CHANNEL_PATH_BASE = "/serverless/";
constexpr const char *SNAPSHOT_REQ = "SNAPSHOT_PREPARE";
constexpr const char *OK = "OK";

WatchDog::WatchDog(const std::string &name, RequestHandler h)
    : handler_(std::move(h)) {
  chan_path_ = CHANNEL_PATH_BASE + name;
}

bool WatchDog::OpenChannel() {
  channel_.open(chan_path_);

  if (!channel_.is_open()) {
    std::cerr << "[Watchdog] Failed to open serverless channel\n";
    return false;
  }
  std::cout << std::unitbuf
            << "[Watchdog] Function process started. Waiting for requests on "
            << chan_path_ << "\n";
  return true;
}

bool WatchDog::Warmup() {
  std::cout << std::unitbuf << "[Watchdog] Handling warmup process\n";
  std::string req_line;
  while (true) {
    if (!std::getline(channel_, req_line)) {
      std::cerr << "[Watchdog] Failed to read warmup request\n";
      return false;
    }

    std::cout << std::unitbuf << "[Watchdog] Recieved request: " << req_line
              << "\n";
    if (req_line == SNAPSHOT_REQ) {
      channel_ << OK;
      std::cout << std::unitbuf << "[Watchdog] Sent snapshot OK response.\n";
      break;
    }
    channel_ << "Processed: " << req_line;
  }
  std::cout << std::unitbuf << "[Watchdog] Completed warmup process\n";
  return true;
}

void WatchDog::Respond(const std::string &res) { channel_ << res; }

void WatchDog::Worker(const std::string &req) {
  std::string res;
  std::stringstream ss(req);
  std::string method;
  std::string path;
  std::string body;
  if (!(ss >> method >> path)) {
    std::cerr << "[Watchdog] Invalid request\n";
    res = "Invalid request: " + req;
  } else {
    ss >> body;
    res = handler_(method, path, body);
  }
  Respond(res);
}

void WatchDog::ProcessRequest() {
  std::string req;
  while (std::getline(channel_, req)) {
    std::thread(&WatchDog::Worker, this, req).detach();
  }
}

void WatchDog::Run() {
  if (!OpenChannel()) { exit(1); }

  if (!Warmup()) { exit(1); }

  ProcessRequest();
}
