#include <string>

inline void CheckSilentMode(int argc, char *argv[]) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--silent") {
      std::freopen("/dev/null", "w", stdout);
      std::freopen("/dev/null", "w", stderr);
      return;
    }
  }
}
