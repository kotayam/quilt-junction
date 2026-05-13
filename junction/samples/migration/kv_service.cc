// kv_service.cc - stateful key-value microservice for migration testing.
//
// In-memory key-value store with configurable heap footprint, using only
// C APIs (no libstdc++) for migration compatibility.
//
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

// Simple open-addressing hash table (no libstdc++ dependency).
struct Entry {
  char *key;
  char *value;
  size_t vlen;
};

static Entry *table;
static size_t table_cap;
static size_t table_count;
static size_t total_bytes;

static void table_init(size_t cap) {
  table_cap = cap;
  table = (Entry *)calloc(cap, sizeof(Entry));
  table_count = 0;
  total_bytes = 0;
}

static size_t hash_str(const char *s) {
  size_t h = 0x811c9dc5;
  for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x01000193;
  return h;
}

static Entry *table_find(const char *key) {
  size_t idx = hash_str(key) % table_cap;
  for (size_t i = 0; i < table_cap; i++) {
    size_t pos = (idx + i) % table_cap;
    if (!table[pos].key) return nullptr;
    if (strcmp(table[pos].key, key) == 0) return &table[pos];
  }
  return nullptr;
}

static void table_grow(void);

static void table_set(const char *key, const char *value, size_t vlen) {
  if (table_count * 2 >= table_cap) table_grow();
  size_t idx = hash_str(key) % table_cap;
  for (size_t i = 0; i < table_cap; i++) {
    size_t pos = (idx + i) % table_cap;
    if (!table[pos].key) {
      table[pos].key = strdup(key);
      table[pos].value = (char *)malloc(vlen);
      memcpy(table[pos].value, value, vlen);
      table[pos].vlen = vlen;
      table_count++;
      total_bytes += strlen(key) + vlen;
      return;
    }
    if (strcmp(table[pos].key, key) == 0) {
      total_bytes -= table[pos].vlen;
      free(table[pos].value);
      table[pos].value = (char *)malloc(vlen);
      memcpy(table[pos].value, value, vlen);
      table[pos].vlen = vlen;
      total_bytes += vlen;
      return;
    }
  }
}

static void table_grow(void) {
  Entry *old = table;
  size_t old_cap = table_cap;
  table_cap *= 2;
  table = (Entry *)calloc(table_cap, sizeof(Entry));
  table_count = 0;
  total_bytes = 0;
  for (size_t i = 0; i < old_cap; i++) {
    if (old[i].key) {
      table_set(old[i].key, old[i].value, old[i].vlen);
      free(old[i].key);
      free(old[i].value);
    }
  }
  free(old);
}

static int table_del(const char *key) {
  Entry *e = table_find(key);
  if (!e) return 0;
  total_bytes -= strlen(e->key) + e->vlen;
  free(e->key);
  free(e->value);
  e->key = nullptr;
  e->value = nullptr;
  e->vlen = 0;
  table_count--;
  // Rehash subsequent entries to fix probe chain.
  size_t pos = (size_t)(e - table);
  for (size_t i = 1; i < table_cap; i++) {
    size_t next = (pos + i) % table_cap;
    if (!table[next].key) break;
    char *k = table[next].key;
    char *v = table[next].value;
    size_t vl = table[next].vlen;
    total_bytes -= strlen(k) + vl;
    table[next].key = nullptr;
    table[next].value = nullptr;
    table[next].vlen = 0;
    table_count--;
    table_set(k, v, vl);
    free(k);
    free(v);
  }
  return 1;
}

static uint32_t compute_checksum(void) {
  uint32_t h = 0x811c9dc5;
  for (size_t i = 0; i < table_cap; i++) {
    if (!table[i].key) continue;
    for (const char *c = table[i].key; *c; c++)
      h = (h ^ (uint8_t)*c) * 0x01000193;
    for (size_t j = 0; j < table[i].vlen; j++)
      h = (h ^ (uint8_t)table[i].value[j]) * 0x01000193;
  }
  return h;
}

static void load_data(int n, int vsize) {
  char keybuf[32];
  char *val = (char *)malloc(vsize);
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < vsize; j++) val[j] = (char)((i + j) & 0xff);
    snprintf(keybuf, sizeof(keybuf), "key_%d", i);
    table_set(keybuf, val, vsize);
  }
  free(val);
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
  char buf[4096];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  if (n <= 0) return;
  buf[n] = '\0';
  if (n > 0 && buf[n - 1] == '\n') buf[--n] = '\0';

  char resp[256];

  if (strncmp(buf, "GET ", 4) == 0) {
    Entry *e = table_find(buf + 4);
    if (e) {
      // "VALUE " + value + "\n"
      size_t rlen = 6 + e->vlen + 1;
      char *r = (char *)malloc(rlen);
      memcpy(r, "VALUE ", 6);
      memcpy(r + 6, e->value, e->vlen);
      r[rlen - 1] = '\n';
      send_resp(fd, r, rlen);
      free(r);
      return;
    }
    snprintf(resp, sizeof(resp), "NOT_FOUND\n");
  } else if (strncmp(buf, "SET ", 4) == 0) {
    char *space = strchr(buf + 4, ' ');
    if (space) {
      *space = '\0';
      size_t vlen = n - (space + 1 - buf);
      table_set(buf + 4, space + 1, vlen);
      snprintf(resp, sizeof(resp), "OK\n");
    } else {
      snprintf(resp, sizeof(resp), "ERR bad SET syntax\n");
    }
  } else if (strncmp(buf, "DEL ", 4) == 0) {
    snprintf(resp, sizeof(resp), table_del(buf + 4) ? "OK\n" : "NOT_FOUND\n");
  } else if (strncmp(buf, "STATS", 5) == 0) {
    snprintf(resp, sizeof(resp), "KEYS %zu BYTES %zu\n", table_count,
             total_bytes);
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

  table_init(1024);

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
          port, table_count, total_bytes);
  fflush(stdout);

  while (1) {
    int fd = accept(srv, nullptr, nullptr);
    if (fd < 0) continue;
    handle_client(fd);
    close(fd);
  }
}
