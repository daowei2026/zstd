# Append-only generational dictionary prototype

This fork research starts from zstd v1.5.7, commit
`f8745da6ff1ad1e7bab384bd1f9d742439278e99`, on
`codex/append-only-generational-prototype`. It investigates the dictionary
mechanism for a future SRFEC/2; it does not implement or deploy that protocol.

The measured first-round results are in [EVALUATION.md](EVALUATION.md).

## Agreed research contract

- Payload bytes are written once. Appending exposes new bytes without rebuilding
  or moving existing payload. Index and descriptor writes are measured separately.
- Each direction has perpetual, maturing and adhoc dictionaries, each with
  prepare and committed partitions. The running prototype uses 30,000,000 bytes
  per partition: 180,000,000 bytes per direction, 360,000,000 bytes per endpoint
  for both directions. The original 50 MB partition / 600 MB endpoint capacity
  remains a measured comparison. Indexes, validity metadata and codec workspaces
  are additional. Later experiments will tune each generation's size separately;
  the present store deliberately uses equal capacities.
- Dictionary turnover adapts slowly and steadily to recurring payload patterns.
  Older dictionaries receive higher-quality match indexes and query optimization.
  A frame can reference multiple partitions, searching perpetual before maturing
  before adhoc at each matching position. Ordinary hits do not copy payload.
- Initial business loss on previously unseen, hard-to-compress input is expected.
  Success means that these samples drive dictionary adaptation and subsequent
  similar input becomes referenceable. Learning must survive failed initial
  business delivery; perfect first-frame delivery is not an acceptance condition.
- Before retiring a partition, selected storage blocks transfer ownership to a
  more mature partition; perpetual retention uses its next partition. Retire
  invalidates the old epoch and releases all remaining owned payload immediately.
  There are no shared cross-generation owners that postpone retirement. The
  excess retained bytes caused by storage-block granularity must be reported.
- A sender can reference an appended range after its initial maintenance send,
  without an acknowledgement gate. A receiver may have holes. Missing ranges,
  duplicate/conflicting writes, delayed updates and retired references must be
  distinguished without returning incorrect decoded bytes.
- Initial dictionary maintenance and maintenance retries have separate
  coefficients applied to business R. All effective R values share max_r;
  smaller retry coefficients are a recommendation, not a validation rule.
  Business repair can only rewrite existing unsent ring copies.

## First evaluation

The deliverable is an executable prototype, behavioral tests, reproducible
measurements against the fixed upstream baseline, and a report of the observed
limits. Measure fully touched capacity, total resident memory, index costs,
append/query/codec latency and throughput, payload relocation, promotion and
retirement, maintenance plus business bytes, and adaptation to changing input.
Synthetic traffic is reproducible and contains no captured user packets.

Existing VPS, MT6000 and DTD resources are authorized for isolated testing.
Current trial deployments, service configuration and business paths remain
unchanged. Remote targets and local instructions must be read from the existing
sibling inventories. Native x86 checks, ARM execution and real-network evidence
are reported separately.

All compilation, caches, generated inputs and evidence use an explicitly supplied
absolute output directory outside source checkouts. Compiler installation,
implicit toolchain bootstrap, product deployment, and an upstream PR are outside
this prototype's work. Test and benchmark parameters are research choices, not
frozen SRFEC/2 wire or configuration contracts.

## Reproduction

Use Python 3.12+ and an existing C99 compiler/archive tool. The runner never
downloads a toolchain. Supply an absolute output root outside this checkout:

```sh
python3 contrib/generational_dictionary/run.py build --variant prototype --output-root /absolute/task-output
python3 contrib/generational_dictionary/run.py test --variant prototype --output-root /absolute/task-output
python3 contrib/generational_dictionary/run.py regression --variant prototype --output-root /absolute/task-output
python3 contrib/generational_dictionary/run.py bench --variant prototype --output-root /absolute/task-output --partition-bytes 30000000 --frames 10000
python3 contrib/generational_dictionary/run.py adapt --variant prototype --output-root /absolute/task-output
python3 contrib/generational_dictionary/run.py network --variant prototype --output-root /absolute/task-output
```

Repeat build/test/adapt/network with `--variant sanitize` for ASan/UBSan.
The `baseline` build extracts the fixed upstream commit into the output root;
baseline benchmarks accept `--baseline-level 0`, `3`, or `9` (the last is an
explicit wide-index parameter set, not upstream compression level 9).
`--cc`, `--ar`, `--compile-only` and `--static` support existing cross compilers.
`--block-bytes 4080` is an optional allocator-size experiment; 4096 is the default.
Build and test logs include exact commands, input hashes and actual exit codes.

`network.c` is a synthetic observation fixture with bounded duration, small
fixed allocations, 1,200-byte datagrams and 1,000-byte decoded inputs. Its reply
per request makes individual outcomes observable; it is not the proposed
ACK-independent maintenance scheduler, an authenticated protocol, a throughput
benchmark, or a validation of inner MTU 1500. DATA requests never add retries.
Run it only on isolated endpoints; the caller owns port admission and cleanup.
The default local check uses UDP loopback port 43967 and refuses an occupied port.

## Prototype choices

The core patch accepts externally found zstd sequences and resolves decoder
dictionary reads through a synchronous callback. It preserves zstd's sequence
encoding, entropy coding and ordinary API paths. Standard decoding with a
test-only contiguous dictionary verifies the output bitstream independently.
Frames are bounded to 65,535 decoded bytes; streaming, trained entropy tables
and split-literal-buffer decoding are not implemented by this experimental API.

The store uses 4 KiB separately owned blocks. Full receiver blocks release their
temporary per-byte validity bitmap. Sender indexes use 8/4/2 ways and maximum
hash logs 20/19/18 from oldest to youngest, with 8/16/32-byte sampling for large
partitions and packed offset fingerprints. An unsuccessful literal run stops
dictionary search after 128 positions; a frame with no dictionary match uses
ordinary zstd level 3, preserving within-frame matching. These are measured
research tradeoffs, not frozen product defaults. Small correctness fixtures
index every byte. Promotion validates both source and destination epochs.

The adaptation experiment uses repeated-sample admission, a quarter-partition
retention cap, and hit ranking per original frame. It counts 32-byte modeled
headers and first maintenance R=3 against business R=2, without loss injection.
It deliberately uses compressible synthetic templates plus random traffic;
its ratios are not estimates of Internet traffic compression. Network missing
ranges and stale references are tested separately.

The focused learning test introduces three completely unseen patterns and loses
their initial business delivery and maintenance arrival. Each sender sample is
still admitted once; after maintenance recovery subsequent similar frames fit
the modeled 1,200-byte outer limit and decode correctly without additional writes.
This is a convergence check, not a PMTU transport implementation. Promotion
accounting also reports a lower bound on retained cold bytes using 64-byte
reference regions, and exact unused block padding. The scheduling model in
`repetition.py` independently exercises separate R coefficients, max_r, bounded
maintenance retries, short-lived ring copies and two turnover speeds.
