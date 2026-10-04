/*
 * A model of exactly the QEMU surface reims-vgpu-dirty.c uses, so the real
 * adapter source can be driven by scripted guests. Not QEMU: it models the
 * three layers that decide whether a guest store can be seen —
 *
 *   protected[]   the hypervisor's write protection of a logged page
 *   kernel_dirty[] what the hypervisor recorded since the last sync
 *   ram_bit[]     QEMU's DIRTY_MEMORY_VGA bitmap, which starts all-set
 *
 * — and nothing else. SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef FAKE_QEMU_H
#define FAKE_QEMU_H

#include <glib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t hwaddr;
typedef uint64_t ram_addr_t;
typedef __int128 Int128;
#define DIRTY_MEMORY_VGA 0
#define HWADDR_PRIx PRIx64

static inline bool int128_nz(Int128 a) { return a != 0; }
static inline uint64_t int128_get64(Int128 a) { return (uint64_t)a; }

typedef struct QemuMutex { pthread_mutex_t m; } QemuMutex;
static inline void qemu_mutex_init(QemuMutex *m) { pthread_mutex_init(&m->m, NULL); }
static inline void qemu_mutex_destroy(QemuMutex *m) { pthread_mutex_destroy(&m->m); }
static inline void qemu_mutex_lock(QemuMutex *m) { pthread_mutex_lock(&m->m); }
static inline void qemu_mutex_unlock(QemuMutex *m) { pthread_mutex_unlock(&m->m); }

#define RCU_READ_LOCK_GUARD() do { } while (0)

#define FAKE_MAX_PAGES 64

typedef struct MemoryRegion {
    bool ram;
    ram_addr_t ram_addr;
    uint64_t pages;
    bool logging;
    bool protected_[FAKE_MAX_PAGES];
    bool kernel_dirty[FAKE_MAX_PAGES];
    bool ram_bit[FAKE_MAX_PAGES];
    uint64_t version[FAKE_MAX_PAGES];   /* guest memory "contents" */
    int refs;
} MemoryRegion;

typedef struct FakeRange {
    hwaddr gpa;
    uint64_t len;
    MemoryRegion *mr;
    hwaddr offset;
} FakeRange;

typedef struct FlatView FlatView;
typedef struct AddressSpace { int unused; } AddressSpace;
extern AddressSpace address_space_memory;

typedef bool (*flatview_cb)(Int128 start, Int128 len, const MemoryRegion *mr,
                            hwaddr offset_in_region, void *opaque);

/* The test configures the flat view. */
extern FakeRange fake_ranges[8];
extern int fake_nranges;
extern size_t fake_target_page;
extern uint64_t fake_syncs;

FlatView *address_space_to_flatview(AddressSpace *as);
void flatview_for_each_range(FlatView *fv, flatview_cb cb, void *opaque);
size_t qemu_target_page_size(void);
void memory_global_dirty_log_sync(bool last_stage);
bool physical_memory_get_dirty_flag(ram_addr_t addr, unsigned client);
void memory_region_reset_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size,
                               unsigned client);
void memory_region_set_log(MemoryRegion *mr, bool log, unsigned client);
ram_addr_t memory_region_get_ram_addr(MemoryRegion *mr);
void memory_region_ref(MemoryRegion *mr);
void memory_region_unref(MemoryRegion *mr);

typedef struct QEMUBH QEMUBH;
typedef struct AioContext AioContext;
typedef void QEMUBHFunc(void *opaque);
AioContext *qemu_get_aio_context(void);
QEMUBH *aio_bh_new(AioContext *ctx, QEMUBHFunc *cb, void *opaque);
void qemu_bh_schedule(QEMUBH *bh);
void qemu_bh_delete(QEMUBH *bh);

/* Test-side controls. */
void fake_reset_world(void);
void fake_guest_write(MemoryRegion *mr, uint64_t page);
bool fake_bh_pending(void);
/* Run the pending bottom half under the (modelled) BQL; false if none. */
bool fake_bh_run(void);
uint64_t fake_bh_schedules(void);

#endif
