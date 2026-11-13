# SkyBook — Airline Reservation Data Store (C)

SkyBook is a custom airline reservation data store written in C. It manages
flights, passengers and bookings on top of a small persistent data store (PDS)
engine.

The engine provides:

- **Full CRUD** over fixed-size binary records.
- **Direct binary file indexing:** each table has a `.dat` data file and a
  matching `.ndx` index file.
- **In-memory indexes that are always kept in sync:**
  - an **open-addressing hash table** with linear probing and tombstones, for
    O(1) average primary-key lookups;
  - an **AVL tree** (self-balancing BST), for key-order traversal and range
    scans;
  - a **secondary index** from route to flight IDs.
- **Crash recovery:** `.dat` is a complete log, and if `.ndx` is missing,
  damaged or out of date, the index is rebuilt by scanning `.dat`.
- **Configurable fsync durability.**
- **Text (CSV) export and import** next to the binary storage format.

**Author:** _your name / roll number_

---

## Repository layout

```
skybook/
├── record.h / record.c         # Flight, Passenger, Booking; matchers; route key; CSV formatters
├── array_index.h / .c          # Array index: linear search + sorted binary search
├── bst.h / bst.c               # AVL tree (+ unbalanced insert kept as a benchmark baseline)
├── hashfn.h                    # Shared integer hash (lowbias32 mixer) + return codes
├── hash_chain.h / .c           # Separate-chaining hash table
├── hash_open.h / .c            # Open addressing: linear probing + tombstones (primary index)
├── secindex.h / .c             # Secondary index: int64 key -> list of primary keys
├── pds.h / pds.c               # Engine: tables, CRUD, recovery, fsync, secondary indexes, CSV
├── skybook.h / skybook.c       # Reservation rules + route index
├── main.c                      # Interactive CLI  (+ `--bench [N]`)
├── bench.h / bench.c           # 100k-record benchmark suite
├── tests.c                     # Test suite (24,449 checks)
├── Makefile                    # make / make bench / make test / make clean
└── BENCHMARK.md                # Full output of the benchmark run quoted below
```

---

## DBMS topics covered

| Topic | Where |
|-------|-------|
| Text vs. binary file I/O (`fprintf`/`fgets` vs. `fwrite`/`fread`, `"w"` vs. `"wb"`) | `pds_export_text` / `pds_import_text`, benchmark B5 |
| Append-only data file of fixed-size frames | `append_frame` in [pds.c](pds.c) |
| Key lookup with **no index** (linear O(n) scan) | `pds_get_rec_by_scan`, benchmark A3 |
| `.ndx` index file; `{key, offset}` entries; one `fseek` to the record | `load_ndx`, `write_ndx_file`, `read_rec_at` |
| **Array index**: linear search, then sorted + binary search | [array_index.c](array_index.c), benchmarks A2, A3, B4 |
| **Self-balancing tree (AVL)** vs. a plain BST | [bst.c](bst.c), benchmarks A1, A8, B3 |
| Hash index: separate chaining **and** open addressing | [hash_chain.c](hash_chain.c), [hash_open.c](hash_open.c) |
| **Secondary index** (non-key field → primary keys) | [secindex.c](secindex.c), `pds_create_secondary_index`, benchmark A4 |
| Soft deletes; tombstones in the hash table **and** in the data file | `pds_delete_rec` |
| **Crash recovery**: rebuild the index from the data file | `rebuild_from_dat`, benchmark A9 |
| **Durability**: `fsync`, write ordering, atomic index replace | `pds_set_sync_mode`, `write_ndx_file`, benchmark A10 |
| Field search with a matcher callback and a comparison count | `pds_get_rec_by_field` |
| Multi-table database: folder (`mkdir` / `chdir`), `.info` catalog | `pds_create_db`, `pds_create_table`, … |
| Update, list in key order, range scan | `pds_update_rec`, `pds_scan_range` |

---

## Architecture

```
          ┌─────────────────────── skybook.c ───────────────────────┐
          │ sb_book · sb_cancel · sb_delete_flight · sb_find_route  │
          │ (seat capacity, no double booking, seat-count sync)     │
          └───────────────────────────┬─────────────────────────────┘
                                      │
          ┌──────────────────────── pds.c ──────────────────────────┐
          │ db:     create / open / close / drop  (folder + .info)  │
          │ table:  create / open (+ recovery) / close              │
          │ CRUD:   put · get · get_via_bst · get_by_scan ·         │
          │         update · delete                                 │
          │ scans:  get_rec_by_field (matcher) · scan_range (AVL)   │
          │ 2nd ix: create_secondary_index · get_by_secondary       │
          │ text:   export_text · import_text      (CSV, text mode) │
          └──┬──────────────────┬────────────────────┬──────────────┘
             │                  │                    │
 ┌───────────▼──────────┐ ┌─────▼───────────────┐ ┌──▼────────────────────┐
 │ hash_open (live)     │ │ AVL tree (all keys) │ │ secindex "route"      │
 │ key → PDS_NdxInfo*   │ │ key → PDS_NdxInfo*  │ │ DEL→BOM → [ids ...]   │
 │ O(1)                 │ │ O(log n), ordered   │ │ field → primary keys  │
 └───────────┬──────────┘ └─────┬───────────────┘ └───────────────────────┘
             └────────┬─────────┘
          PDS_NdxInfo { key, is_deleted, offset }
                      │  single fseek + fread
            ┌─────────▼─────────────────────────────────────────┐
            │ flights.dat   [key][flags][Flight] [key][flags]...│  flags: LIVE / TOMBSTONE
            └───────────────────────────────────────────────────┘
```

The array index and the chaining hash table are not part of the live engine.
The benchmark builds them from the same index entries so that every structure
can be compared on identical data.

### On-disk format

A **database** is a folder. `pds_create_db("airline")` creates:

| File | Contents |
|------|----------|
| `airline/airline.info` | Catalog: magic `SBD2`, table count, then `{ name[32], rec_size }` per table |
| `airline/<table>.dat` | Append-only log of frames `[int32 key][int32 flags][record bytes]`. `flags` = `FRAME_LIVE` (a record version) or `FRAME_TOMBSTONE` (key deleted) |
| `airline/<table>.ndx` | Index snapshot: magic `SBN2`, entry count, **`dat_end`** (the `.dat` size it describes), then `{ int32 key, int32 is_deleted, int64 offset }` per entry, in AVL level order |
| `airline/<table>.csv` | Optional text export, one record per line |

> Databases created before this version (catalog magic `SBDB`) can't be
> opened: frames used to have no `flags` field. The engine says so clearly
> (`PDS_OLD_FORMAT`). Recreate the database, or export its tables to CSV with
> the older build first.

### Records

| Table | Struct | Size | Frame on disk | Key |
|-------|--------|------|---------------|-----|
| `flights` | `Flight` | 44 B | 52 B | `flight_id` |
| `passengers` | `Passenger` | 72 B | 80 B | `passenger_id` |
| `bookings` | `Booking` | 16 B | 24 B | `booking_id` |

---

## How the engine works

### From no index to a hash index
- **No index:** `pds_get_rec_by_scan` reads `.dat` from the start, comparing
  the key of every frame. O(n).
- **Array index:** linear search is O(n). A sorted array allows binary search,
  O(log n), but then each insert shifts half the array, O(n).
- **BST → AVL tree:** a plain BST is O(log n) only on random keys. Sorted keys,
  which are the natural way to number flights, turn it into a linked list. The
  AVL tree rotates on insert so that the two subtrees of every node differ in
  height by at most 1. Height then stays below 1.44 log₂ n for *any* insert
  order. The engine uses it for key-order traversal, range scans and saving
  `.ndx`.
- **Hash table:** O(1) average. The engine's primary index uses open
  addressing; separate chaining is benchmarked against it.

### AVL tree ([bst.c](bst.c))
- Each node stores its subtree height.
- An insert records the path down from the root. On the way back up it updates
  heights and applies whichever rotation fixes the imbalance: left-left,
  left-right, right-right or right-left. It stops as soon as a subtree's height
  is unchanged.
- **Saving the tree without losing its shape:** the `.ndx` is written in
  **level order** (breadth-first). Re-inserting an AVL tree's nodes level by
  level never triggers a rotation, so the next open rebuilds exactly the same
  tree. The tests check this node by node. Pre-order would *not* work for an
  AVL tree, because the left subtree arrives first and forces rotations.
- `bst_add_node_unbalanced` keeps the old plain-BST insert, but only so the
  benchmark can show sorted inserts before and after.

### Secondary index: route → flight IDs ([secindex.c](secindex.c))
- A route is packed into one `int64`: the 6 bytes of `"DEL"` + `"BOM"`. This
  gives a unique key per route (`route_key`, `flight_route_key`).
- The index maps each route key to a growable array of flight IDs, stored in
  a chained hash table that doubles its bucket count as routes are added.
- **Kept in sync:**
  - `put` adds the new flight's ID.
  - `delete` removes it.
  - `update` re-reads the old record and moves the ID only if the route
    changed. A fare change doesn't touch the index.
- **Not persisted:** it's rebuilt with one sequential pass over `.dat` when
  `sb_open` registers it, which takes 14 ms for 100,000 flights.
- `pds_get_by_secondary` fetches only the matching records. A full scan would
  compare every record.

### Crash recovery
1. **`.dat` is a complete log.**
   - Inserts append a `LIVE` frame.
   - Deletes append a `TOMBSTONE` frame.
   - Updates overwrite the payload of the current frame in place, which
     doesn't change the frame's offset.

   So the index can always be rebuilt from `.dat` alone.
2. **`.ndx` is a snapshot, stamped with `dat_end`:** the `.dat` size it
   describes. If any append happened after the last clean close, the sizes
   differ.
3. **On open** (`pds_open_table`), the index is rebuilt from `.dat` when:
   - `.ndx` is **missing**;
   - `.ndx` is **unreadable** (bad magic, truncated, impossible offsets);
   - `.ndx` is **out of date** (`dat_end` ≠ actual size).

   The rebuild (`rebuild_from_dat`) replays every frame in order: a `LIVE`
   frame sets the key's offset, and a `TOMBSTONE` marks it deleted. Zeroed
   alignment gaps and a half-written frame at the end are skipped. The
   repaired index is written back right away, so the next open is a normal
   one.
4. **Why this is safe:** `.ndx` is replaced atomically. It's written to
   `<table>.ndx.tmp` and fsynced, the old `.ndx` is removed, and the new file
   is renamed into place. A crash at any step leaves either a good `.ndx`, an
   old one, or none at all, and the last two trigger a rebuild.
5. `pds_table_stats(...).recovery` tells you what happened: CLI option 17
   shows it, and opening a database prints a "Recovery:" line.

### Durability: fsync
`fflush` only moves data from the program into the operating system's cache.
`fsync` (`_commit` on Windows) forces the OS to write it to the disk and wait,
which is what survives a power cut.

| Mode | What is fsynced | Guarantee |
|------|-----------------|-----------|
| `PDS_SYNC_NONE` | nothing | survives a program crash, not a power cut |
| `PDS_SYNC_ON_CLOSE` *(default)* | on close: `.dat` first, then `.ndx` (and `.info`) | everything up to the last clean close |
| `PDS_SYNC_ALWAYS` | `.dat` after every put/update/delete, plus the above | every write that returned success |

- **Order matters:** the data is fsynced *before* the index that points into
  it.
- On Linux/macOS the folder is also fsynced after the rename, so the rename
  itself is durable.
- Set the mode with `pds_set_sync_mode`, or with CLI option 24.

### Soft deletes, at three levels
- **Data file:** a tombstone frame is appended, so the delete survives a crash.
- **AVL tree:** the entry is flagged `is_deleted` and saved to `.ndx`.
  Booking IDs come from `max key + 1`, and the max includes deleted keys, so
  an ID is never reused.
- **Hash table:** the key's slot becomes a slot tombstone. Lookups probe past
  it, inserts reuse it, and a rehash purges it.

### Field search, range scans, text I/O, reservation rules
- **Field search:** `pds_get_rec_by_field` reads `.dat` sequentially in 64 KB
  blocks, calls a matcher on every live record and counts the comparisons.
  Matchers return `0` for a match, `1` for no match and `>1` for an error.
- **Range scans:** `pds_scan_range` walks the AVL tree in order and skips
  subtrees outside `[lo, hi]`. The cost is O(log n + k) for k results.
- **CSV export/import:**
  - Export writes in storage order through a text-mode file.
  - Import reads lines with `fgets`, parses them field by field and skips bad
    lines.
  - Commas are rejected when records are added, so every record can be
    exported.
- **Reservation rules:**
  - `sb_book` checks that the flight exists and isn't full, that the passenger
    exists, and that they have no booking on that flight yet. It then writes
    the booking and updates the seat count, rolling the booking back if that
    update fails.
  - `sb_cancel` gives the seat back.
  - `sb_delete_flight` refuses while the flight still has bookings.
  - CSV import restores raw rows and does not re-run these rules.

---

## Benchmark results

These numbers come from `./skybook --bench 100000` on Windows 11 with GCC 15.2
(`-O2`), run from a local (non-OneDrive) folder. The full output is in
[BENCHMARK.md](BENCHMARK.md). Over two runs, most timings agreed within about
±15%, but lookups in the 1M-node trees varied more, because they depend on the
CPU cache. Heights, counts and every correctness check were identical.

### 1. AVL tree vs. plain BST: sorted inserts before and after

**In memory (B3):**

| Tree | Keys | Insert order | Height | Per insert | Lookup |
|------|------|--------------|--------|------------|--------|
| Plain BST *(before)* | 20,000 | Sorted | **20,000** | 91,977 ns | 68,396 ns |
| Plain BST *(before)* | 20,000 | Random | 34 | 173 ns | 86 ns |
| **AVL** *(after)* | 20,000 | Sorted | **15** | **97 ns** | **50 ns** |
| **AVL** *(after)* | 20,000 | Random | 17 | 228 ns | 59 ns |
| **AVL** | 1,000,000 | Sorted | 20 | 145 ns | 302 ns |
| **AVL** | 1,000,000 | Random | 24 | 1,603 ns | 439 ns |

With sorted keys the AVL tree is about **950× faster to build** and **1,370×
faster to search** than the plain BST at 20,000 keys. Sorted input actually
gives the AVL tree its *shortest* shape. A plain BST with a million sorted keys
would take hours to build, so it isn't measured.

**In the engine:**

| Operation | Before (plain BST) | After (AVL) |
|-----------|-------------------|-------------|
| Insert 100,000 flights with sorted IDs (A1) | O(n) per insert; tree height 100,000 | **69 ms** (693 ns/insert), height 17 |
| Import a **key-sorted** CSV of 90,000 flights (A8) | **131.6 s** (measured with the previous build) | **144 ms**, height 17, ≈ 900× faster |
| Insert 100,000 flights with random IDs (A1) | — | 104 ms (1,040 ns/insert), height 20 |

### 2. Crash recovery (A9)

After a clean close: 1,000 deletes, 1,000 updates and 1,000 inserts, then
`pds_simulate_crash`, which leaves the process "dead" with no `.ndx` written.

| Metric | Result |
|--------|--------|
| Detected on reopen | `.ndx` out of date → rebuilt from `.dat` |
| Reopen with rebuild (replays 5.55 MB of `.dat`) | **97.8 ms** (normal open from `.ndx`: 44.3 ms) |
| Live / deleted after rebuild | 90,000 / 11,000 (exactly as expected) |
| Verify all 101,000 keys (content, deletes, updates, new inserts) | **PASS**, 0 mismatches |

The tests also cover the other recovery cases: `.ndx` deleted, `.ndx`
overwritten with garbage, and a half-written frame at the end of `.dat`. In
every case the data comes back intact and the next insert still lands on a
frame boundary.

### 3. Cost of fsync (A10, 1,000 inserts)

| Mode | Per insert | Close | Survives a power cut |
|------|-----------|-------|----------------------|
| `PDS_SYNC_NONE` | 0.7 µs | 4.8 ms | not guaranteed |
| `PDS_SYNC_ON_CLOSE` *(default)* | 0.9 µs | 8.0 ms | everything up to the last close |
| `PDS_SYNC_ALWAYS` | **641 µs** | 7.8 ms | every acknowledged write |

Forcing every write to disk makes inserts about **900× slower**. That's why it
is optional, and why the default syncs only at close.

### 4. Secondary index: field search (A4, all DEL → BOM flights)

| Method | Matcher comparisons | Records read | Time |
|--------|---------------------|--------------|------|
| Full scan (before) | **100,000** | 100,000, sequential | 6.52 ms |
| Route index, IDs only | **0** | 0 | **6 ns** |
| Route index + fetch each flight | **0** | 1,111, random | 4.35 ms |

All three return the same 1,111 flights. The route index replaces 100,000
comparisons with one hash lookup: the matching IDs come back in 6 ns, about
1,000,000× faster than the scan. Fetching the full records is only about 1.5×
faster than scanning, though. Those 1,111 reads are random `fseek`s, while the
scan reads the file sequentially in 64 KB blocks. A secondary index pays off
most when you only need the keys, when few records match, or when the records
are cached in memory.

### 5. Other results

**Primary-key lookup in memory (A2, 100,000 flights):**

| Index | Hit | Probes per hit |
|-------|-----|----------------|
| Array, linear search | 23,830 ns | 48,513 entries |
| Sorted array, binary search | 163 ns | 15.7 entries |
| AVL tree (height 20) | 104 ns | ~15.9 nodes |
| Separate chaining | 22.3 ns | 1.25 nodes |
| **Open addressing** | **13.4 ns** | 1.31 slots |

**Full record fetch (A3):**
- With no index: 0.65 ms.
- With the hash index: 7.1 µs, **92× faster**.
- With the AVL tree: 9.6 µs.

**Engine operations:**

| Operation | Result |
|-----------|--------|
| Read-modify-write fare, with the route index in sync (A6) | 9.9 µs |
| Soft delete, with the route index in sync (A6) | 7.8 µs* |
| Close: fsync `.dat`, write + fsync `.ndx` (A7) | 70.7 ms |
| Reopen from `.ndx`, then verify all 100,000 keys (A7) | **PASS** |
| CSV export / import of 90,000 flights, round trip identical (A8) | 178 ms / 159 ms, **PASS** |

\* Soft delete used to cost 81 ns, when it only touched memory. It now appends
a tombstone frame, and to keep the route index correct it also re-reads the
old record. That `fseek` also forces the pending appends out of the stdio
buffer, so a delete now costs about the same as a random read.

**Text vs. binary (B5):**
- Binary is 6.6× faster to write and 4.1× faster to read.
- Binary reaches record *r* in 4.7 µs, against 11.1 ms for text.

**Load factor (B1):** the measured probe counts match Knuth's formulas.

| Load α | Open addressing, probes hit / miss | Theory |
|--------|------------------------------------|--------|
| 0.25 | 1.16 / 1.39 | 1.17 / 1.39 |
| 0.50 | 1.49 / 2.51 | 1.50 / 2.50 |
| 0.75 | 2.54 / 8.63 | 2.50 / 8.50 |
| 0.90 | 5.85 / 54.14 | 5.50 / 50.50 |

At α = 0.9 an unsuccessful lookup takes 134.5 ns, against 33.5 ns with
chaining. This is why the engine caps occupancy at 0.7.

**Hash tombstones (B2):** churn raised probes per miss from 2.52 to 4.09, and
`ho_rehash` brought them back to 2.53.

**Insert cost (B4, 20,000 random keys):**

| Index | Per insert |
|-------|-----------|
| Sorted array | 2,327 ns |
| AVL tree | 203 ns |
| Separate chaining | 190 ns |
| Open addressing | 70 ns |

---

## Building and running

You need GCC and GNU make. On Windows, MSYS2/MinGW works; use `mingw32-make`
there. These commands work from PowerShell, Command Prompt and Git Bash.

```bash
cd skybook
make            # build the CLI (skybook / skybook.exe)
make test       # build and run the test suite
make bench      # build and run the 100k-record benchmark
make clean      # remove binaries and any leftover test/bench files
```

### CLI

```
==================== SkyBook ====================
  1. Create database          2. Open database
  3. Load sample data
  4. Add flight               5. Add passenger
  6. Book a seat              7. Cancel booking
  8. Find flight by ID        9. Find passenger by ID
 10. Find booking by ID
 11. Search flights by route (route index)
 12. Search passengers by name (field scan)
 13. Bookings for a flight (field scan)
 14. List flights in ID range (AVL tree)
 15. Update flight fare      16. Delete flight
 17. Table statistics        18. Run benchmark
 19. Close database
 20. Export table to CSV     21. Import table from CSV
 22. Find flight by ID without the index (linear scan)
 23. Search flights by route without the index (full scan)
 24. Set fsync (durability) mode
  0. Exit
```

A quick tour:
1. `1` → `airline`, then `2` → `airline`, then `3` to load sample data.
2. Compare the route searches: `11` (`DEL` → `BOM`, uses the index) against
   `23` (same route, full scan).
3. Try `14` (range scan), `22` (no-index lookup) and `17`. The statistics show
   AVL heights, the secondary-index count, whether each table was recovered,
   and the fsync mode.
4. **Try a recovery:**
   1. Exit with `0`.
   2. Delete `airline\flights.ndx`.
   3. Run the program again and open the database with `2`.

   It prints `Recovery: flights index rebuilt from .dat (.ndx missing)`, and
   every flight is still there.

---

## Design notes

- **Appends are frame-aligned.** The next append offset is rounded up to a
  whole frame on open, so a torn write can't misalign later frames.
- **Back-to-back appends don't seek,** so stdio batches them.
- **Integrity check on read:** every fetch checks that the frame at the
  indexed offset has the expected key and is `LIVE`. Otherwise it returns
  `PDS_CORRUPT`.
- **Guard clauses:** every data operation checks that the database is open and
  the table is open.
- **Hash function:** keys go through an integer mixer (lowbias32) before
  `& (cap − 1)`, so sequential IDs don't cluster in linear probing.

## Limitations and possible extensions

- **In-place updates aren't journaled.** A crash in the middle of an update
  can leave that one record half old, half new. A write-ahead log or
  copy-on-write updates would fix this.
- **Default mode can lose recent writes.** In `PDS_SYNC_ON_CLOSE` mode, writes
  still in stdio's buffer when the process dies are lost. Only
  `PDS_SYNC_ALWAYS` guarantees every acknowledged write.
- **No compaction.** Old versions and tombstone frames stay in `.dat`.
- **Secondary indexes are rebuilt on every open** instead of being persisted,
  and there is only one (route). Duplicate-booking and passenger-name checks
  are still full scans.
- **One database at a time.** The engine changes into the database folder and
  keeps one global handle.
- **Native byte order.** The binary files aren't portable between machines;
  the CSV export is.
