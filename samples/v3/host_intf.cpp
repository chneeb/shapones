#include <shapones/host_intf.hpp>

#include <pico/sem.h>
#include <pico/stdlib.h>
#include <pico/time.h>
#include <hardware/sync.h>

#include <cstdlib>
#include <cstring>

namespace shapones {

static constexpr int HW_NUM_SPINLOCKS = 16;
static uint32_t used_spinlocks = 0;

struct spin_lock_handle_t {
  spin_lock_t *hw_lock;
  uint32_t irqs;
  int id;
};

struct spinlock_t {
  spin_lock_handle_t *handle;
};

struct semaphore_slot_t {
  semaphore_t sem;
  bool valid;
};

static spinlock_t spinlocks[NUM_SPINLOCKS];
static semaphore_slot_t semaphores[NUM_SEMAPHORES];

result_t ram_alloc(size_t size, void **out_ptr) {
  *out_ptr = malloc(size);
  if (!*out_ptr) {
    SHAPONES_RET_ERR(result_t::ERR_RAM_ALLOC_FAILED);
  }
  return result_t::SUCCESS;
}

void ram_free(void *ptr) { free(ptr); }

result_t spinlock_init(int id) {
  int hw_id = __builtin_ffs(~used_spinlocks) - 1;
  if (hw_id < 0 || hw_id >= HW_NUM_SPINLOCKS) {
    SHAPONES_RET_ERR(result_t::ERR_RAM_ALLOC_FAILED);
  }
  spin_lock_handle_t *handle = new spin_lock_handle_t;
  handle->hw_lock = spin_lock_init(hw_id);
  handle->id = hw_id;
  spinlocks[id].handle = handle;
  spin_lock_claim(hw_id);
  used_spinlocks |= (1U << hw_id);
  return result_t::SUCCESS;
}

void spinlock_deinit(int id) {
  auto *handle = spinlocks[id].handle;
  if (!handle) return;
  spin_lock_unclaim(handle->id);
  used_spinlocks &= ~(1U << handle->id);
  delete handle;
  spinlocks[id].handle = nullptr;
}

void spinlock_get(int id) {
  auto *handle = spinlocks[id].handle;
  if (!handle) return;
  handle->irqs = spin_lock_blocking(handle->hw_lock);
}

void spinlock_release(int id) {
  auto *handle = spinlocks[id].handle;
  if (!handle) return;
  spin_unlock(handle->hw_lock, handle->irqs);
}

result_t semaphore_init(int id) {
  sem_init(&semaphores[id].sem, 1, 1);
  semaphores[id].valid = true;
  return result_t::SUCCESS;
}

void semaphore_deinit(int id) { semaphores[id].valid = false; }

void semaphore_take(int id) {
  sem_acquire_blocking(&semaphores[id].sem);
}

bool semaphore_try_take(int id) {
  return sem_try_acquire(&semaphores[id].sem);
}

void semaphore_give(int id) {
  sem_release(&semaphores[id].sem);
}

result_t load_ines(const char *path, const uint8_t **out_ines, size_t *out_size) {
  (void)path; (void)out_ines; (void)out_size;
  SHAPONES_RET_ERR(result_t::ERR_FS_OPEN_FAILED);
}

void unload_ines() {}

namespace fsys {

result_t mount() { SHAPONES_RET_ERR(result_t::ERR_FS_NO_DISK); }
void unmount() {}
void get_ines_dir(char *out_path) { strcpy(out_path, "/"); }
void get_config_dir(char *out_path) { strcpy(out_path, "/"); }
result_t enum_files(const char *path, enum_files_cb_t callback) {
  (void)path; (void)callback;
  SHAPONES_RET_ERR(result_t::ERR_FS_ENUM_FAILED);
}
bool exists(const char *path) { (void)path; return false; }
result_t open(const char *path, bool write, void **handle) {
  (void)path; (void)write; (void)handle;
  SHAPONES_RET_ERR(result_t::ERR_FS_OPEN_FAILED);
}
void close(void *handle) { (void)handle; }
result_t seek(void *handle, size_t offset) {
  (void)handle; (void)offset;
  SHAPONES_RET_ERR(result_t::ERR_FS_SEEK_FAILED);
}
bool eof(void *handle) { (void)handle; return true; }
result_t read(void *handle, uint8_t *buff, size_t size) {
  (void)handle; (void)buff; (void)size;
  SHAPONES_RET_ERR(result_t::ERR_FS_READ_FAILED);
}
result_t write(void *handle, const uint8_t *buff, size_t size) {
  (void)handle; (void)buff; (void)size;
  SHAPONES_RET_ERR(result_t::ERR_FS_WRITE_FAILED);
}
result_t size(void *handle, size_t *out_size) {
  (void)handle; (void)out_size;
  SHAPONES_RET_ERR(result_t::ERR_FS_OPEN_FAILED);
}
result_t remove(const char *path) {
  (void)path;
  SHAPONES_RET_ERR(result_t::ERR_FS_DELETE_FAILED);
}
result_t make_dir(const char *path) {
  (void)path;
  SHAPONES_RET_ERR(result_t::ERR_FS_WRITE_FAILED);
}

}  // namespace fsys

uint64_t get_time_us() { return to_us_since_boot(get_absolute_time()); }

}  // namespace shapones
