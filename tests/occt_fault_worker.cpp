#include <chrono>
#include <thread>

int main() {
#if defined(TEKLA_DB1_TEST_HANG_WORKER)
  std::this_thread::sleep_for(std::chrono::seconds(30));
  return 0;
#else
  return 3;
#endif
}
