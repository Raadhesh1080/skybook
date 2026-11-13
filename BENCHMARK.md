## SkyBook benchmark

Compiler: 15.2.0. Records: 100000 flights. sizeof(Flight) = 44 bytes.

## Part A - engine on disk

### A1. Insert 100000 flights (disk append + hash + AVL)

| Key order | Total | Per insert | AVL height | Avg depth |
|---|---|---|---|---|
| Random | 104.0 ms | 1040 ns | 20 | 15.94 |
| Sorted (1, 2, 3, ...) | 69.3 ms | 693 ns | 17 | 15.69 |

Record frame on disk: 52 bytes (4-byte key + 4-byte flags + 44-byte Flight). Hash index: cap 262144, load 0.38.

### A2. In-memory index lookup (1000000 random keys each, 100000 records)

| Index | Hit | Miss | Avg probes (hit / miss) |
|---|---|---|---|
| Array, linear search (2000 lookups) | 23830 ns | - | 48513 entries / - |
| Sorted array, binary search | 163.4 ns | 22.9 ns | 15.69 / 16.00 entries |
| AVL tree (height 20) | 103.8 ns | 19.3 ns | ~15.9 nodes (avg depth) |
| Separate chaining (200003 buckets, load 0.50) | 22.3 ns | 22.9 ns | 1.25 / 0.50 nodes |
| Open addressing, linear probing (cap 262144, load 0.38) | **13.4 ns** | **24.8 ns** | 1.31 / 1.82 slots |

Open addressing on hits: **1778x** faster than a linear array search, 12.2x faster than binary search, **7.7x** faster than the AVL tree, 1.66x faster than chaining.

### A3. Full record fetch by key (random keys)

| Path | Lookups | Per fetch |
|---|---|---|
| No index: `pds_get_rec_by_scan` reads `.dat` from the start | 200 | 0.65 ms (avg 49237 frames compared) |
| Sorted array + `fseek` + `fread` | 200000 | 7239 ns |
| AVL tree + `fseek` + `fread` (`pds_get_rec_via_bst`) | 200000 | 9566 ns |
| Hash + `fseek` + `fread` (`pds_get_rec`) | 200000 | 7088 ns |

Any index is **92x** faster than scanning the data file.

### A4. Field search: all DEL -> BOM flights, full scan vs route secondary index

| Method | Matcher comparisons | Records read from disk | Time |
|---|---|---|---|
| Full scan (`pds_get_rec_by_field`) | 100000 | 100000 (sequential) | 6.52 ms |
| Route index: IDs only (`si_find`) | 0 | 0 | 6 ns |
| Route index + fetch each flight (`pds_get_by_secondary`) | 0 | 1111 (random) | 4.35 ms |

Both return 1111 flights (identical). Building the index (one sequential pass over 100000 flights) took 10.5 ms; it then stays in sync on every insert, update and delete.

### A5. AVL range scan, flight IDs 50000..50999

| Metric | Value |
|---|---|
| Records returned (in key order) | 1000 |
| Time | 6.353 ms |

### A6. Mutations

| Operation | Count | Per op |
|---|---|---|
| Read-modify-write fare (in-place overwrite) | 10000 | 9883 ns |
| Soft delete (tombstone frame appended to `.dat`) | 10000 | 7815 ns |

With the route index registered, each update and delete first re-reads the old record to keep the index in sync.

### A7. Persistence: close, reopen, verify every key

| Metric | Value |
|---|---|
| `.dat` size | 5.46 MB |
| `.ndx` size | 1.53 MB (100000 entries incl. tombstones) |
| Close (fsync `.dat`, write + fsync `.ndx`) | 70.7 ms |
| Reopen (load hash + AVL from `.ndx`) | 44.3 ms |
| Verify all 100000 keys after reopen | 618.8 ms |
| Live records after reopen | 90000 (expected 90000) |
| Result | **PASS** (0 mismatches) |

### A8. Text export / import round trip (`pds_export_text` -> `pds_import_text`)

| Metric | Value |
|---|---|
| Export 90000 live flights to CSV (storage order, one sequential pass) | 178.1 ms |
| Import CSV into a new table (parse + insert) | 159.0 ms |
| Imported / skipped | 90000 / 0 |
| Every record identical in both tables | **PASS** (0 mismatches) |
| Import a **key-sorted** CSV (90000 rows, IDs ascending) | 144.0 ms, AVL height 17 |

### A9. Crash recovery: rebuild the index from `.dat`

After the clean close in A7: 1000 deletes, 1000 updates and 1000 inserts, then `pds_simulate_crash` (no `.ndx` written).

| Metric | Value |
|---|---|
| Detected on open | rebuilt from .dat (.ndx out of date) |
| Reopen with rebuild (replay 5.55 MB of `.dat`) | 97.8 ms (normal open from `.ndx`: 44.3 ms) |
| Live / deleted after rebuild | 90000 / 11000 (expected 90000 live) |
| Verify all 101000 keys | 629.5 ms |
| Result | **PASS** (0 mismatches) |

### A10. Cost of fsync (1000 inserts into a fresh table, then close)

| Mode | Per insert | Close | Survives a power cut |
|---|---|---|---|
| `PDS_SYNC_NONE` | 0.7 us | 4.8 ms | not guaranteed |
| `PDS_SYNC_ON_CLOSE` (default) | 0.9 us | 8.0 ms | everything up to the last close |
| `PDS_SYNC_ALWAYS` | 641.3 us | 7.8 ms | every acknowledged write |

## Part B - index structures in memory

### B1. Load factor sweep (131072 slots, 1000000 lookups per cell)

Expected probes for linear probing: hit = (1 + 1/(1-a))/2, miss = (1 + 1/(1-a)^2)/2. For chaining: hit = 1 + a/2, miss = a.

| Load a | Open: probes hit / miss (expected) | Open: ns hit / miss | Chain: nodes hit / miss (expected) | Chain: ns hit / miss |
|---|---|---|---|---|
| 0.25 | 1.16 / 1.39 (1.17 / 1.39) | 10.0 / 13.9 | 1.12 / 0.25 (1.12 / 0.25) | 11.1 / 14.3 |
| 0.50 | 1.49 / 2.51 (1.50 / 2.50) | 15.1 / 28.3 | 1.25 / 0.50 (1.25 / 0.50) | 21.1 / 30.1 |
| 0.75 | 2.54 / 8.63 (2.50 / 8.50) | 29.5 / 54.9 | 1.38 / 0.75 (1.38 / 0.75) | 34.9 / 39.2 |
| 0.90 | 5.85 / 54.14 (5.50 / 50.50) | 43.3 / 134.5 | 1.45 / 0.90 (1.45 / 0.90) | 37.3 / 33.5 |

### B2. Tombstone build-up (open addressing, 131072 slots, 65536 live keys)

Deleting half the keys and inserting replacements keeps the live load at 0.50, but tombstones still lengthen every unsuccessful probe run.

| State | Tombstones | Occupancy | Avg probes per miss |
|---|---|---|---|
| Fresh | 0 | 0.50 | 2.52 |
| After churn | 21832 | 0.67 | 4.09 |
| After `ho_rehash` | 0 | 0.50 | 2.53 |

### B3. Plain BST vs AVL tree, sorted vs random insert order

| Tree | Keys | Insert order | Height | Avg depth | Build | Per insert | Lookup |
|---|---|---|---|---|---|---|---|
| Plain BST | 20000 | Sorted (1, 2, 3, ...) | 20000 | 10000.5 | 1839.5 ms | 91977 ns | 68396 ns |
| Plain BST | 20000 | Random | 34 | 17.3 | 3.5 ms | 173 ns | 86 ns |
| **AVL** | 20000 | Sorted (1, 2, 3, ...) | 15 | 13.4 | 1.9 ms | 97 ns | 50 ns |
| **AVL** | 20000 | Random | 17 | 13.6 | 4.6 ms | 228 ns | 59 ns |
| **AVL** | 1000000 | Sorted (1, 2, 3, ...) | 20 | 19.0 | 144.8 ms | 145 ns | 302 ns |
| **AVL** | 1000000 | Random | 24 | 19.3 | 1603.2 ms | 1603 ns | 439 ns |

Sorted IDs turn the plain BST into a linked list (height = number of keys). The AVL tree rotates on insert, so sorted input gives the *shortest* tree.

### B4. Insert cost, 20000 keys in random order

| Index | Per insert | Why |
|---|---|---|
| Sorted array | 2327 ns | shifts on average half the array (`memmove`) |
| AVL tree | 203 ns | walks ~log n nodes, may rotate, one malloc |
| Separate chaining | 190 ns | one malloc per key |
| Open addressing | 70 ns | writes a slot; occasional doubling rehash |

The sorted array has fast lookups (binary search) but O(n) inserts, which is why the engine keeps an AVL tree for ordering instead.

### B5. Text vs binary file I/O (100000 flight records)

| | Binary (`fwrite` / `fread`, "wb"/"rb") | Text (`fprintf` / `fgets` + parse, "w"/"r") |
|---|---|---|
| File size | 4.20 MB (44 bytes/record, fixed) | 4.21 MB (~44.1 bytes/record, varies) |
| Write all | 25.1 ms | 165.7 ms |
| Read all + verify | 20.1 ms | 83.1 ms |
| Read record #r at random | 4749 ns (`fseek` to (r-1) x 44) | 11.11 ms (read r lines from the top) |
| Round trip correct | PASS | PASS |

Fixed-size binary records are what make `fseek`-based random access - and therefore the `.ndx` offset index - possible. Text is human-readable and portable, which is why SkyBook uses it only for export/import.

Overall: PASS
