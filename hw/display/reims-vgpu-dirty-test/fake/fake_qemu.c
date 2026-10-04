/* See fake_qemu.h. SPDX-License-Identifier: GPL-2.0-or-later */
#include "fake_qemu.h"

AddressSpace address_space_memory;
FakeRange fake_ranges[8];
int fake_nranges;
size_t fake_target_page = 4096;
uint64_t fake_syncs;

static MemoryRegion *world_mr(void)
{
    return fake_nranges ? fake_ranges[0].mr : NULL;
}

FlatView *address_space_to_flatview(AddressSpace *as)
{
    (void)as;
    return (FlatView *)&fake_nranges;
}

void flatview_for_each_range(FlatView *fv, flatview_cb cb, void *opaque)
{
    int i;

    (void)fv;
    for (i = 0; i < fake_nranges; i++) {
        FakeRange *r = &fake_ranges[i];

        if (cb((Int128)r->gpa, (Int128)r->len, r->mr, r->offset, opaque)) {
            return;
        }
    }
}

size_t qemu_target_page_size(void)
{
    return fake_target_page;
}

static uint64_t page_of(MemoryRegion *mr, ram_addr_t addr)
{
    return (addr - mr->ram_addr) / fake_target_page;
}

void memory_global_dirty_log_sync(bool last_stage)
{
    int i;

    (void)last_stage;
    fake_syncs++;
    for (i = 0; i < fake_nranges; i++) {
        MemoryRegion *mr = fake_ranges[i].mr;
        uint64_t p;

        if (!mr->logging) {
            continue;
        }
        for (p = 0; p < mr->pages; p++) {
            if (mr->kernel_dirty[p]) {
                mr->ram_bit[p] = true;
                mr->kernel_dirty[p] = false;
            }
        }
    }
}

bool physical_memory_get_dirty_flag(ram_addr_t addr, unsigned client)
{
    MemoryRegion *mr = world_mr();

    (void)client;
    return mr->ram_bit[page_of(mr, addr)];
}

void memory_region_reset_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size,
                               unsigned client)
{
    uint64_t p;

    (void)client;
    for (p = addr / fake_target_page;
         p < (addr + size + fake_target_page - 1) / fake_target_page; p++) {
        mr->ram_bit[p] = false;
        if (mr->logging) {
            mr->protected_[p] = true;
        }
    }
}

void memory_region_set_log(MemoryRegion *mr, bool log, unsigned client)
{
    uint64_t p;

    (void)client;
    mr->logging = log;
    for (p = 0; p < mr->pages; p++) {
        /* Enabling logging write-protects the region; the QEMU bitmap is left
         * exactly as it was, which is to say all-set until something clears it. */
        mr->protected_[p] = log;
    }
}

ram_addr_t memory_region_get_ram_addr(MemoryRegion *mr)
{
    return mr->ram_addr;
}

void memory_region_ref(MemoryRegion *mr) { mr->refs++; }
void memory_region_unref(MemoryRegion *mr) { mr->refs--; }

void fake_guest_write(MemoryRegion *mr, uint64_t page)
{
    mr->version[page]++;
    if (mr->logging && mr->protected_[page]) {
        mr->kernel_dirty[page] = true;
        mr->protected_[page] = false;
    }
}

/* ---- bottom half ---- */
struct QEMUBH {
    QEMUBHFunc *cb;
    void *opaque;
    bool pending;
};
static QEMUBH the_bh;
static uint64_t bh_schedules;
static bool bh_live;

AioContext *qemu_get_aio_context(void) { return NULL; }

QEMUBH *aio_bh_new(AioContext *ctx, QEMUBHFunc *cb, void *opaque)
{
    (void)ctx;
    the_bh.cb = cb;
    the_bh.opaque = opaque;
    the_bh.pending = false;
    bh_live = true;
    return &the_bh;
}

void qemu_bh_schedule(QEMUBH *bh)
{
    __atomic_add_fetch(&bh_schedules, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&bh->pending, true, __ATOMIC_SEQ_CST);
}

void qemu_bh_delete(QEMUBH *bh)
{
    bh->pending = false;
    bh_live = false;
}

bool fake_bh_pending(void)
{
    return bh_live && __atomic_load_n(&the_bh.pending, __ATOMIC_SEQ_CST);
}

bool fake_bh_run(void)
{
    if (!fake_bh_pending()) {
        return false;
    }
    __atomic_store_n(&the_bh.pending, false, __ATOMIC_SEQ_CST);
    the_bh.cb(the_bh.opaque);
    return true;
}

uint64_t fake_bh_schedules(void)
{
    return __atomic_load_n(&bh_schedules, __ATOMIC_RELAXED);
}

/* ---- world ---- */
static MemoryRegion the_mr;

void fake_reset_world(void)
{
    uint64_t p;

    memset(&the_mr, 0, sizeof(the_mr));
    the_mr.ram = true;
    the_mr.ram_addr = 0x40000000;
    the_mr.pages = FAKE_MAX_PAGES;
    for (p = 0; p < FAKE_MAX_PAGES; p++) {
        the_mr.ram_bit[p] = true;   /* QEMU allocates RAM all-dirty */
    }
    fake_ranges[0] = (FakeRange){ .gpa = 0x100000,
                                  .len = FAKE_MAX_PAGES * 4096ull,
                                  .mr = &the_mr, .offset = 0 };
    fake_nranges = 1;
    fake_syncs = 0;
    bh_schedules = 0;
    the_bh.pending = false;
}
