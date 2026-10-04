/*
 * Drives the real reims-vgpu-dirty.c against a model of the hypervisor dirty
 * bitmap (fake/fake_qemu.[ch]).
 *
 * Two questions, answered separately because they fail separately:
 *
 *  1. ARMING.  Does a tracked set become readable without the guest ringing a
 *     doorbell? (directed cases)
 *
 *  2. SOUNDNESS.  Over *every* interleaving of guest writes, doorbell harvests,
 *     main-loop arming harvests, generation reads, host copies and vouch checks
 *     up to a bounded length, does arming ever let a guest write escape?
 *     (exhaustive model check)
 *
 * The protocol under check is the one the Rust consumers follow: read the
 * generation (G), THEN copy the pages (C), record G beside the copy; later a
 * read equal to the recorded G vouches for the copy (V).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "fake_qemu.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* The shim is included, not linked, so the model can read how many harvests it
 * has really run (`d->harvests`) instead of inferring it from a call the
 * mutants remove. */
#include SHIM_SRC

#define PG 4096ull
#define GPA(p) (0x100000ull + (p) * PG)

static MemoryRegion *mr(void) { return fake_ranges[0].mr; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* A world in which an earlier surface already turned logging on, which is the
 * steady state of a boot after its first tracked surface. */
static ReimsVgpuDirty *new_dirty(bool logged_already)
{
    ReimsVgpuDirty *d;

    fake_reset_world();
    d = reims_vgpu_dirty_new();
    if (logged_already) {
        uint64_t g[1] = { GPA(40) };
        uint64_t t = reims_vgpu_dirty_track(d, g, 1, PG);
        int i;

        for (i = 0; i < 4 && reims_vgpu_dirty_gen(d, t) == 0; i++) {
            fake_bh_run();
            reims_vgpu_dirty_harvest(d);
        }
    }
    return d;
}

/* ------------------------------------------------------------ directed */

static void directed(void)
{
    uint64_t g[2] = { GPA(2), GPA(3) };
    ReimsVgpuDirty *d;
    uint64_t t, runs;

    /* First surface of a boot: the harvest that arms it must first turn logging
     * on, so two harvests are owed — and neither is a doorbell. */
    d = new_dirty(false);
    t = reims_vgpu_dirty_track(d, g, 2, PG);
    CHECK(t != 0, "track refused");
    CHECK(fake_bh_pending(), "track did not ask for an arming harvest");
    CHECK(reims_vgpu_dirty_gen(d, t) == 0, "readable before any harvest");
    for (runs = 0; reims_vgpu_dirty_gen(d, t) == 0 && runs < 8; runs++) {
        CHECK(fake_bh_run(), "unarmed set with no harvest pending (run %" PRIu64 ")", runs);
    }
    CHECK(reims_vgpu_dirty_gen(d, t) != 0, "never armed without a doorbell");
    CHECK(runs == 2, "first-region arming took %" PRIu64 " harvests, want 2", runs);
    CHECK(mr()->logging, "arming did not enable logging");
    reims_vgpu_dirty_free(d);

    /* Every later surface: logging is already on, one harvest. */
    d = new_dirty(true);
    t = reims_vgpu_dirty_track(d, g, 2, PG);
    CHECK(reims_vgpu_dirty_gen(d, t) == 0, "readable before the harvest");
    CHECK(fake_bh_run(), "no harvest pending for a new set");
    CHECK(reims_vgpu_dirty_gen(d, t) == 1, "one harvest should arm at gen 1, got %" PRIu64,
          reims_vgpu_dirty_gen(d, t));
    reims_vgpu_dirty_free(d);

    /* The pre-existing behaviour, stated as a test of the *mechanism*: with no
     * main-loop turn and no doorbell nothing can arm, however often it is read.
     * This is the 8.24 s window. */
    d = new_dirty(true);
    t = reims_vgpu_dirty_track(d, g, 2, PG);
    {
        uint64_t before = fake_syncs;

        for (runs = 0; runs < 1000; runs++) {
            CHECK(reims_vgpu_dirty_gen(d, t) == 0, "armed with no harvest at all");
        }
        CHECK(fake_syncs == before, "a read ran a sync; only a harvest may");
    }
    reims_vgpu_dirty_free(d);

    /* A burst of tracks is one harvest, not one per track. */
    d = new_dirty(true);
    {
        uint64_t tok[16];
        uint64_t before = fake_syncs;
        int i;

        for (i = 0; i < 16; i++) {
            uint64_t gg[1] = { GPA(10 + i) };
            tok[i] = reims_vgpu_dirty_track(d, gg, 1, PG);
        }
        CHECK(fake_bh_run(), "burst scheduled nothing");
        CHECK(!fake_bh_pending(), "burst left a second harvest pending");
        CHECK(fake_syncs == before + 1, "burst cost %" PRIu64 " syncs, want 1",
              fake_syncs - before);
        for (i = 0; i < 16; i++) {
            CHECK(reims_vgpu_dirty_gen(d, tok[i]) != 0, "token %d unarmed after the burst", i);
        }
    }
    reims_vgpu_dirty_free(d);

    /* Untracked before the bottom half runs: harmless. */
    d = new_dirty(true);
    t = reims_vgpu_dirty_track(d, g, 2, PG);
    reims_vgpu_dirty_untrack(d, t);
    fake_bh_run();
    CHECK(reims_vgpu_dirty_gen(d, t) == 0, "released token readable");
    reims_vgpu_dirty_free(d);
}

/* ---------------------------------------------------------- exhaustive */

enum { W0, W1, H, B, G, C, V, NOPS };
static const char *opname[] = { "W0", "W1", "H", "B", "G", "C", "V" };
#define PA 2
#define PB 3

typedef struct Run {
    ReimsVgpuDirty *d;
    uint64_t token;
    /* Versions of the two tracked pages as the last harvest saw them. */
    uint64_t seen_at_harvest[2];
    uint64_t harvests_seen;
    /* The reader. */
    uint64_t last_g;
    bool have_copy;
    uint64_t copy_g;
    uint64_t copy_ver[2];
    /* Armed baseline, captured when the generation first became readable. */
    bool armed;
    uint64_t armed_gen;
    uint64_t armed_ver[2];
} Run;

static const uint64_t PAGE[2] = { PA, PB };

static void note_harvest(Run *r)
{
    if (r->d->harvests != r->harvests_seen) {
        r->harvests_seen = r->d->harvests;
        r->seen_at_harvest[0] = mr()->version[PA];
        r->seen_at_harvest[1] = mr()->version[PB];
    }
}

static void fail_run(const char *what, const int *ops, int n, bool logged)
{
    int i;

    failures++;
    fprintf(stderr, "FAIL %s  [logged_already=%d]  ops:", what, logged);
    for (i = 0; i < n; i++) {
        fprintf(stderr, " %s", ops[i] == -1 ? "T" : opname[ops[i]]);
    }
    fputc('\n', stderr);
}

static uint64_t peek_gen(Run *r)
{
    /* The header documents gen as idempotent and only a consumer's *reads*
     * schedule work, so the checker reads through the public door and accounts
     * for that by reading only where the protocol itself reads. */
    return reims_vgpu_dirty_gen(r->d, r->token);
}

static bool run_one(const int *ops, int n, bool logged)
{
    Run r;
    int i;
    bool ok = true;
    uint64_t pages[16];

    memset(&r, 0, sizeof(r));
    r.d = new_dirty(logged);
    r.harvests_seen = r.d->harvests;
    /* Pages tracked later exist as stale bits already; the model starts that
     * way (ram_bit all-set) and a prelude surface may have cleared others. */

    for (i = 0; i < n && ok; i++) {
        int op = ops[i];

        if (op == -1) {
            uint64_t g[2] = { GPA(PA), GPA(PB) };

            if (!r.token) {
                r.token = reims_vgpu_dirty_track(r.d, g, 2, PG);
            }
            continue;
        }
        switch (op) {
        case W0: fake_guest_write(mr(), PA); break;
        case W1: fake_guest_write(mr(), PB); break;
        case H:  reims_vgpu_dirty_harvest(r.d); note_harvest(&r); break;
        case B:  fake_bh_run(); note_harvest(&r); break;
        case G:
            if (r.token) {
                r.last_g = peek_gen(&r);
            }
            break;
        case C:
            if (r.token && r.last_g) {
                r.have_copy = true;
                r.copy_g = r.last_g;
                r.copy_ver[0] = mr()->version[PA];
                r.copy_ver[1] = mr()->version[PB];
            }
            break;
        case V:
            if (r.token && r.have_copy) {
                uint64_t g = peek_gen(&r);

                if (g == r.copy_g) {
                    int k;

                    /* I: a vouched copy is no older than what the last sync
                     * had already seen. */
                    for (k = 0; k < 2; k++) {
                        if (r.seen_at_harvest[k] > r.copy_ver[k]) {
                            fail_run("I: vouched a copy older than the last harvest's view",
                                     ops, i + 1, logged);
                            ok = false;
                        }
                    }
                }
            }
            break;
        }
        if (!ok || !r.token) {
            continue;
        }
        /* Every op: derive the armed baseline lazily at the first readable
         * generation, from the versions the *harvest that produced it* saw. */
        if (!r.armed) {
            uint64_t g = peek_gen(&r);

            if (g != 0) {
                r.armed = true;
                r.armed_gen = g;
                r.armed_ver[0] = r.seen_at_harvest[0];
                r.armed_ver[1] = r.seen_at_harvest[1];
            }
        }
        if (r.armed) {
            int64_t found = reims_vgpu_dirty_written_since(r.d, r.token, r.armed_gen,
                                                           pages, 16);
            int k, m;

            if (found < 0) {
                fail_run("written_since refused an armed set", ops, i + 1, logged);
                ok = false;
                continue;
            }
            for (k = 0; k < 2; k++) {
                bool reported = false;
                bool wrote_after_arm = r.seen_at_harvest[k] > r.armed_ver[k];

                for (m = 0; m < found; m++) {
                    if (pages[m] == GPA(PAGE[k])) {
                        reported = true;
                    }
                }
                /* J: nothing reported that was not written after arming. */
                if (reported && !wrote_after_arm) {
                    fail_run("J: reported a write that never happened (stale bit read as a guest store)",
                             ops, i + 1, logged);
                    ok = false;
                }
                /* K: every write a harvest saw after arming is reported. */
                if (wrote_after_arm && !reported) {
                    fail_run("K: a harvested guest write is missing from the report",
                             ops, i + 1, logged);
                    ok = false;
                }
            }
        }
    }
    reims_vgpu_dirty_free(r.d);
    return ok;
}

static uint64_t nruns;

static void enumerate(int *ops, int n, int max, bool logged, int tracked)
{
    int op;

    if (n > 0 && tracked) {
        nruns++;
        if (!run_one(ops, n, logged) && failures > 10) {
            return;
        }
    }
    if (n == max) {
        return;
    }
    if (!tracked) {
        /* A prefix of writes and harvests, then the registration. */
        ops[n] = -1;
        enumerate(ops, n + 1, max, logged, 1);
        if (n < 2) {
            for (op = W0; op <= B; op++) {
                ops[n] = op;
                enumerate(ops, n + 1, max, logged, 0);
            }
        }
        return;
    }
    for (op = 0; op < NOPS; op++) {
        ops[n] = op;
        enumerate(ops, n + 1, max, logged, 1);
    }
}

int main(int argc, char **argv)
{
    int ops[16];
    int max = argc > 1 ? atoi(argv[1]) : 8;

    if (!(argc > 2 && !strcmp(argv[2], "model-only"))) {
        directed();
    }
    if (failures) {
        fprintf(stderr, "directed cases failed: %d\n", failures);
        return 1;
    }
    enumerate(ops, 0, max, false, 0);
    enumerate(ops, 0, max, true, 0);
    printf("reims-vgpu-dirty: directed cases ok; %" PRIu64
           " interleavings checked (max length %d), %d failures\n",
           nruns, max, failures);
    return failures ? 1 : 0;
}
