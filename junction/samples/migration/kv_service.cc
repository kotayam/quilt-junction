// kv_service.cc - stateful key-value microservice for migration testing.
//
// In-memory key-value store with configurable heap footprint.
// Listens on a TCP port, accepts connections, and responds to commands:
//   "GET <key>\n"              -> "VALUE <data>\n" or "NOT_FOUND\n"
//   "SET <key> <value>\n"      -> "OK\n"
//   "DEL <key>\n"              -> "OK\n" or "NOT_FOUND\n"
//   "STATS\n"                  -> "KEYS <n> BYTES <b>\n"
//   "CHECKSUM\n"               -> "CHECKSUM <hex>\n"
//   "LOAD <n> <vsize>\n"       -> "LOADED <n>\n"
//
// Usage: kv_service <port> [num_keys] [value_size]
//   port        TCP port to listen on
//   num_keys    pre-populate this many keys on startup (default: 0)
//   value_size  byte size of each pre-populated value (default: 1024)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

static std::unordered_map<std::string, std::string> store;

static void load_data(int n, int vsize) {
  std::string val(vsize, '\0');
  for (int i = 0; i < n; i++) {
    // Deterministic fill so checksum is reproducible.
    for (int j = 0; j < vsize; j++) val[j] = (char)((i + j) & 0xff);
    store["key_" + std::to_string(i)] = val;
  }
}

static size_t estimate_bytes() {
  size_t total = 0;
  for (auto &kv : store) total += kv.first.size() + kv.second.size();
  return total;
}

static uint32_t compute_checksum() {
  // FNV-1a over all key-value pairs in iteration order is
  // non-deterministic (unordered_map), so hash values sorted by key.
  // For speed, just XOR-rotate over all bytes.
  uint32_t h = 0x811c9dc5;
  for (auto &kv : store) {
    for (char c : kv.first) h = (h ^ (uint8_t)c) * 0x01000193;
    for (char c : kv.second) h = (h ^ (uint8_t)c) * 0x01000193;
  }
  return h;
}

static void send_resp(int fd, const char *resp, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, resp + sent, len - sent);
    if (n <= 0) break;
    sent += n;
  }
}

static void handle_client(int fd) {
  // Read up to 4 KB per request (enough for SET with reasonable values).
  char buf[4096];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  if (n <= 0) return;
  buf[n] = '\0';
  // Strip trailing newline.
  if (n > 0 && buf[n - 1] == '\n') buf[--n] = '\0';

  char resp[256];

  if (strncmp(buf, "GET ", 4) == 0) {
    auto it = store.find(buf + 4);
    if (it != store.end()) {
      // For large values, build response dynamically.
      std::string r = "VALUE " + it->second + "\n";
      send_resp(fd, r.c_str(), r.size());
      return;
    }
    snprintf(resp, sizeof(resp), "NOT_FOUND\n");
  } else if (strncmp(buf, "SET ", 4) == 0) {
    // SET <key> <value>
    char *space = strchr(buf + 4, ' ');
    if (space) {
      *space = '\0';
      store[buf + 4] = std::string(space + 1);
      snprintf(resp, sizeof(resp), "OK\n");
    } else {
      snprintf(resp, sizeof(resp), "ERR bad SET syntax\n");
    }
  } else if (strncmp(buf, "DEL ", 4) == 0) {
    snprintf(resp, sizeof(resp),
             store.erase(buf + 4) ? "OK\n" : "NOT_FOUND\n");
  } else if (strncmp(buf, "STATS", 5) == 0) {
    snprintf(resp, sizeof(resp), "KEYS %zu BYTES %zu\n", store.size(),
             estimate_bytes());
  } else if (strncmp(buf, "CHECKSUM", 8) == 0) {
    snprintf(resp, sizeof(resp), "CHECKSUM %08x\n", compute_checksum());
  } else if (strncmp(buf, "LOAD ", 5) == 0) {
    int num = 0, vsize = 1024;
    sscanf(buf + 5, "%d %d", &num, &vsize);
    load_data(num, vsize);
    snprintf(resp, sizeof(resp), "LOADED %d\n", num);
  } else {
    snprintf(resp, sizeof(resp), "ERR unknown command\n");
  }
  send_resp(fd, resp, strlen(resp));
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <port> [num_keys] [value_size]\n", argv[0]);
    return EXIT_FAILURE;
  }

  int port = atoi(argv[1]);
  int num_keys = argc > 2 ? atoi(argv[2]) : 0;
  int vsize = argc > 3 ? atoi(argv[3]) : 1024;

  if (num_keys > 0) {
    fprintf(stdout, "pre-loading %d keys (%d bytes each)...\n", num_keys,
            vsize);
    fflush(stdout);
    load_data(num_keys, vsize);
  }

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  bind(srv, (struct sockaddr *)&addr, sizeof(addr));
  listen(srv, 16);

  fprintf(stdout, "kv_service listening on port %d (%zu keys, %zu bytes)\n",
          port, store.size(), estimate_bytes());
  fflush(stdout);

  while (1) {
    int fd = accept(srv, nullptr, nullptr);
    if (fd < 0) continue;
    handle_client(fd);
    close(fd);
  }
}
