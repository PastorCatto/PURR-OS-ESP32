# Incremental Compaction

## Background

microStore uses an append-only segmented log: records are written to up to
`USTORE_DEFAULT_SEGMENT_COUNT` (default 8) segment files of `USTORE_DEFAULT_SEGMENT_SIZE` (default 65 536 B) each.
When all segments are full, **compaction** is triggered: live records are copied to a temporary
file, all source segments are deleted, and the temporary file is renamed to segment 0.

### The space problem with the original design

The original compaction wrote the entire live-record set to `compact.tmp` **before** touching any
source segment.  At the moment of peak overlap, the filesystem held:

```
8 × USTORE_DEFAULT_SEGMENT_SIZE (sources)  +  compact.tmp (up to 8 × USTORE_DEFAULT_SEGMENT_SIZE)
```

In the worst case — all records live, all 8 segments full — this required ~2× the live-data size,
forcing roughly **50% of the filesystem to remain free** at all times just to allow compaction to
run.  On flash-constrained devices this is a severe limitation.

---

## Incremental Compaction

The new strategy processes **one source segment at a time**, deleting each source file immediately
after its live records have been durably checkpointed in `compact.tmp`.  Peak disk overhead drops
from *(N segments + compact.tmp)* to approximately *1 segment + compact.tmp (growing)*.

### Core invariant

> Source segment *S* is deleted only after:
> 1. All live records from *S* have been written to `compact.tmp`.
> 2. `compact.tmp` has been flushed to non-volatile storage (`fsync` / `flush()`).
> 3. The journal has been updated on disk with `next_seg = S + 1` and
>    `tmp_valid_size = <current byte count of compact.tmp>`.

Steps 3 and 4 (journal write then segment delete) **must occur in this order**.  If power fails
between the journal write and the `remove()`, the source segment is still present; the recovery
path handles this correctly (see §Recovery below).

---

## Journal Format

The journal file (`{prefix}_journal.dat`) uses a 16-byte packed structure:

```c
struct Journal {
    uint32_t magic;           // 0x4B564A4E — validity sentinel
    uint32_t state;           // JOURNAL_NONE=0  COMPACTING=1  COMMIT=2
    uint32_t next_seg;        // segments 0..next_seg-1 are committed to compact.tmp
    uint32_t tmp_valid_size;  // byte count of compact.tmp that is durably valid
};
```

The two new fields (`next_seg`, `tmp_valid_size`) are zero in a `JOURNAL_COMMIT` record and in any
journal written by earlier firmware (their bytes read as 0 from the old 8-byte journal, which is
smaller than `sizeof(Journal)` and therefore fails the size check — the journal is discarded and
boot proceeds normally, treating it as no journal present).

---

## Compaction Sequence

```
Phase 1  write_journal(COMPACTING, next_seg=0, tmp_valid_size=0)
         open compact.tmp  (ModeWrite — creates / truncates)

Phase 2  prune_index_to_max_recs()
         build per_seg[0..7]: sorted lists of live (offset, key) from the in-memory index

Phase 3  for s = 0 to MAX_SEGMENTS-1:

           if per_seg[s] is non-empty:
             open segment[s]  (read-only)
             for each live record in per_seg[s] (by offset order):
               read  header + key + value  from segment[s]
               write header + key + value + commit-marker  to compact.tmp
             close segment[s]

           if any write above failed → write_ok = false; break

           compact.tmp.flush()
           valid_size = compact.tmp.tell()
           write_journal(COMPACTING, next_seg=s+1, tmp_valid_size=valid_size)
           filesystem.remove(segment[s])      ← safe: journal already records s+1
           committed_segs++

Phase 4  compact.tmp.close()

         if !write_ok:
           if committed_segs == 0:
             remove compact.tmp; clear journal   ← clean abort, no data lost
           else:
             leave compact.tmp + journal in place ← recovery on next boot
           return false

         write_journal(COMMIT)
         finalize_compaction()   ← rename compact.tmp → segment[0], rebuild index
         clear_journal()
         return true
```

### Space usage during normal compaction

At the moment when segment *S* is being processed:

| On disk | Size |
|---------|------|
| compact.tmp (records from segments 0..S-1) | ≤ S × USTORE_DEFAULT_SEGMENT_SIZE |
| segment[S] (current source, not yet deleted) | ≤ USTORE_DEFAULT_SEGMENT_SIZE |
| segments S+1..7 (not yet touched) | ≤ (MAX_SEGMENTS − S − 1) × USTORE_DEFAULT_SEGMENT_SIZE |

The simultaneous overhead is always **at most one extra segment** on top of the total live data
size — compared to up to eight extra segments with the old design.

---

## Crash Safety

### Power-fail scenarios and recovery

| Crash moment | Journal state on next boot | Recovery action |
|---|---|---|
| Before Phase 1 journal write | No journal | Normal boot — all segments intact |
| After Phase 1 journal write, before any segment deleted | `COMPACTING`, `next_seg = 0` | Delete `compact.tmp`; all segments intact |
| After journaling `next_seg = S+1` but before `remove(segment[S])` | `COMPACTING`, `next_seg = S+1` | Case C recovery (see below); orphaned segment[S] cleaned up by `finalize_compaction()`'s bulk remove |
| After `remove(segment[S])` and journal `next_seg = S+1` | `COMPACTING`, `next_seg = S+1` | Case C recovery |
| After `write_journal(COMMIT)` | `COMMIT` | `finalize_compaction()` — identical to original design |

### Case C recovery (`next_seg > 0`)

Source segments `0..next_seg-1` have been deleted.  `compact.tmp` holds their live records, durably
written up to byte `tmp_valid_size`.  Any bytes beyond `tmp_valid_size` are either:

- **Complete valid records** flushed to disk before the crash but not yet reflected in the journal.
  The segment-scan logic in `rebuild_index_from_segments()` processes these correctly.
- **A partial (torn) record** at the very end.  The scan stops at the first invalid record header
  or missing commit marker, so partial data is silently ignored.

Recovery procedure in `recover_if_needed()`:

```
rename compact.tmp → segment_0.dat
clear journal
(return to normal boot)
```

Normal boot then calls `rebuild_index_from_segments()` (or loads the saved index if it is still
valid) across segment 0 (the renamed `compact.tmp`) and segments `next_seg..7`.  The in-memory
index converges to the correct live-record set:

- Keys whose only live version was in segments `0..next_seg-1` are found in segment 0.
- Keys overwritten or deleted in segments `next_seg..7` are resolved by the standard
  "last occurrence wins / tombstone removes" scan logic.

The next compaction cycle (when segments fill up again) consolidates everything into a fresh
segment 0, cleaning up both the former `compact.tmp` data and any tombstones.

#### Why not "continue" the interrupted compaction during recovery?

A "continue" approach would require rebuilding the in-memory index inside `recover_if_needed()`,
which runs before the normal index-load path in `init()`.  It would also need a way to seek to
`tmp_valid_size` within `compact.tmp` and resume writing — which requires a filesystem mode not
available in all platform adapters (`O_RDWR` without truncation or append-only).

The rename approach avoids both issues: it requires only a single `rename()` call, works identically
on every platform adapter, and delegates all the hard work to the existing scan and compaction paths
that already handle mixed live/dead records correctly.

#### Space used during Case C recovery

After the rename, the filesystem holds:

| File | Size |
|------|------|
| segment_0.dat (renamed compact.tmp) | ≤ (next_seg) × USTORE_DEFAULT_SEGMENT_SIZE |
| segments next_seg..7 | ≤ (MAX_SEGMENTS − next_seg) × USTORE_DEFAULT_SEGMENT_SIZE |

Total ≤ `MAX_SEGMENTS × USTORE_DEFAULT_SEGMENT_SIZE` — no additional space is required beyond what was
already occupied at the time of the crash.

---

## Space Savings Summary

| Scenario | Old peak overhead | New peak overhead |
|---|---|---|
| All 8 segments full, all records live | 8 × 65 KB + 8 × 65 KB ≈ 1 MB | 1 × 65 KB + growing compact.tmp |
| 50% live records (typical) | 8 × 65 KB + 4 × 65 KB ≈ 780 KB | ~35 KB overhead |
| Required free space | ~50% of total filesystem | ~1 segment + epsilon |

---

## Implementation Notes

- `write_journal()` gained two optional parameters (`next_seg`, `tmp_valid_size`) with default
  values of `0`.  All call sites that pass only `state` continue to compile and behave correctly.

- The `Journal` struct grew from 8 to 16 bytes.  A journal written by old firmware is 8 bytes,
  which is smaller than `sizeof(Journal)`; the size check in `recover_if_needed()` rejects it,
  and boot proceeds as if no journal were present — a safe upgrade path.

- `compact()` now calls `outf.flush()` once per segment rather than once at the end.  On platforms
  where `flush()` calls `fsync()`, this adds up to 7 extra sync operations per full compaction
  cycle.  The additional latency is acceptable because compaction is already a relatively slow,
  infrequent background operation.

- The `committed_segs` counter in `compact()` is used solely to distinguish a safe abort (no
  segments deleted, `compact.tmp` can be discarded) from an unsafe abort (some segments deleted,
  `compact.tmp` must be preserved for recovery).

- `finalize_compaction()` is unchanged.  Its bulk `remove()` loop (which removes all segment files
  regardless of whether they exist) naturally cleans up any segment that was journaled as
  committed but not yet physically deleted before a crash.
