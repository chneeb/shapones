// Host-interface stubs for the offline harness.
//
// core/ is platform-independent, so it runs on a PC once these are provided.
// Everything is single-threaded here: cpu::service() and ppu::service() are
// called in turn from one loop, so the locks are no-ops. That is deliberate -
// it means anything reproducing here is NOT a cross-core race, which is a
// useful thing to learn for free.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

#include "shapones/shapones.hpp"
#include "shapones/host_intf.hpp"

namespace shapones {

// The ROM is loaded by main(); these two just hand it over.
static const uint8_t *g_ines = nullptr;
static size_t g_ines_size = 0;
void hosttest_set_ines(const uint8_t *p, size_t n) { g_ines = p; g_ines_size = n; }

result_t load_ines(const char *, const uint8_t **out_ines, size_t *out_size) {
  *out_ines = g_ines;
  *out_size = g_ines_size;
  return g_ines ? result_t::SUCCESS : result_t::ERR_FS_OPEN_FAILED;
}
void unload_ines() {}

result_t ram_alloc(size_t size, void **out_ptr) {
  *out_ptr = malloc(size);
  return *out_ptr ? result_t::SUCCESS : result_t::ERR_RAM_ALLOC_FAILED;
}
void ram_free(void *ptr) { free(ptr); }

result_t spinlock_init(int) { return result_t::SUCCESS; }
void spinlock_deinit(int) {}
void spinlock_get(int) {}
void spinlock_release(int) {}

result_t semaphore_init(int) { return result_t::SUCCESS; }
void semaphore_deinit(int) {}
void semaphore_take(int) {}
bool semaphore_try_take(int) { return true; }
void semaphore_give(int) {}

uint64_t get_time_us() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

namespace fsys {
result_t mount() { return result_t::SUCCESS; }
void unmount() {}
void get_ines_dir(char *out_path) { strcpy(out_path, "."); }
void get_config_dir(char *out_path) { strcpy(out_path, "."); }
result_t enum_files(const char *, enum_files_cb_t) { return result_t::SUCCESS; }
bool exists(const char *) { return false; }
result_t open(const char *, bool, void **) { return result_t::ERR_FS_OPEN_FAILED; }
void close(void *) {}
result_t seek(void *, size_t) { return result_t::SUCCESS; }
bool eof(void *) { return true; }
result_t read(void *, uint8_t *, size_t) { return result_t::SUCCESS; }
result_t write(void *, const uint8_t *, size_t) { return result_t::SUCCESS; }
result_t size(void *, size_t *out_size) { *out_size = 0; return result_t::SUCCESS; }
result_t remove(const char *) { return result_t::SUCCESS; }
result_t make_dir(const char *) { return result_t::SUCCESS; }
}  // namespace fsys

}  // namespace shapones
