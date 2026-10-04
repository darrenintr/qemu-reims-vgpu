# reims-vgpu-dirty model check

Drives the real `../reims-vgpu-dirty.c` against a model of the hypervisor dirty
bitmap. No QEMU build is needed: `fake/` models exactly the QEMU surface the
adapter touches (flat view, `DIRTY_MEMORY_VGA` bitmap, region logging, a
main-loop bottom half) and nothing else.

    ./run.sh [max-length]        # default 8; ~3.6 M interleavings in a few seconds
    ./run.sh --mutant NAME 6     # a deliberately wrong shim; must FAIL

## What it checks

*Directed:* a tracked set arms with **no guest doorbell** — one main-loop harvest
when logging is already on, two for the first region of a boot — and a burst of
tracks costs one harvest.

*Exhaustive:* every interleaving (to the length given) of guest writes `W0 W1`,
doorbell harvest `H`, arming bottom half `B`, generation read `G`, host copy `C`
and vouch check `V`, with the registration `T` at any point after up to two
writes and harvests, in a world where logging is off and one where an earlier
surface already turned it on. The reader follows the Rust protocol: read the
generation, then copy, record the generation beside the copy.

* **I** a vouched copy (generation unchanged) is never older than what the last
  harvest had already seen — no guest write escapes;
* **J** `written_since(armed_gen)` never names a page nobody wrote — a stale bit
  must not read as a guest store, because the writeback rail drops the GPU's
  store for pages it believes the guest wrote;
* **K** every write a harvest saw after arming is named.

## Mutants

| name | what it breaks | caught by |
|---|---|---|
| `arm_at_track` | a "synchronous baseline": gen readable at track | J |
| `no_unlogged` | arm in the harvest that enabled logging | J |
| `no_hit_stamp` | do not stamp the pages the arming harvest absorbed | K |
| `no_sync` | read the bitmap without folding hypervisor bits in | K |
