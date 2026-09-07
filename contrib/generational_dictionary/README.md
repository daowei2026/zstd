# Append-only generational dictionary prototype

This fork research starts from zstd v1.5.7, commit
`f8745da6ff1ad1e7bab384bd1f9d742439278e99`, on
`codex/append-only-generational-prototype`. It investigates the dictionary
mechanism for SRFEC/2; it does not implement or deploy that protocol.

The measured first-round results are in [EVALUATION.md](EVALUATION.md).

The next implementation follows the SRFEC
[persistent dictionary design](https://github.com/daowei2026/srfec/blob/main/docs/development/line-compression-refinement.md):
one mapped payload file per generation, independent partition indexes and
cross-partition copying before retirement. The codec now uses continuous
partition backing and can borrow the six half ranges of three mapped files via
`GD_createWithBuffers`. It never initializes or frees borrowed payload. The
standalone constructor allocates the same layout internally. File mapping,
separate validity metadata, index snapshots and restart recovery still belong
to the pending product integration; this interface does not implement them.

`GD_Store` now keeps each half's logical addresses independent. Its matcher
finds variable-length regions from business bytes and the existing local index,
then emits separate native frames for accepted regions in original byte order.
Each frame names one half with Dictionary_ID 32768+partition; ID 0 selects
ordinary zstd for remaining bytes. Epochs remain in the enclosing frame view.
Distances use only that half's fixed capacity, so append or an unrelated half's
capacity/retirement cannot change their meaning. No shared history, fixed input
slicing, or private chunk header is used. This replaces the old experimental
format; SRFEC adoption and authenticated format/version changes remain pending.

The external sequence APIs now take an explicit `dictionaryID`. Nonzero IDs
use zstd's native frame header; decoding checks the expected ID before any
dictionary callback. The callback receives offsets within the one selected
dictionary, with no index or payload migration. Zero preserves unspecified-ID
frames. The caller still checks
partition epoch and valid ranges. Independent frames can be concatenated and
discovered with `ZSTD_findFrameCompressedSize`, without a private chunk header.
Tests cover a fixed native decoder vector, independent dictionary contents,
reverse-order decoding, extent growth, missing data, wrong IDs, truncation and
context reuse after failure. Store tests additionally cover differing unrelated
half capacities, mixed dynamic regions, old-first selection, rejected-output
heat, and a 250/200/50 MB half-capacity configuration with sparse valid bytes.

The external sequence APIs now accept dictionary addresses through
`ZSTD_EXTERNAL_DICT_SIZE_MAX` (UINT32_MAX minus 65,535 frame bytes and three
repeat-offset codes). `GD_Store` applies this limit independently to each half;
the product's current configuration limit remains unchanged until adoption.
Sparse callback tests exercise 1 GiB, 1 GiB + 1, 2.5 GB and the maximum address
space, including a maximum-size frame and rejected overflowing offsets. They
verify encoding and decoding, not resident memory, mapped storage or throughput.

## Current codec prototype

- Payload bytes are written once. Appending exposes new bytes without rebuilding
  or moving existing payload. Index and descriptor writes are measured separately.
- Each direction has perpetual, maturing and adhoc dictionaries, each with
  prepare and committed partitions. The running prototype uses 30,000,000 bytes
  per partition: 180,000,000 bytes per direction, 360,000,000 bytes per endpoint
  for both directions. The original 50 MB partition / 600 MB endpoint capacity
  remains a measured comparison. Indexes, validity metadata and codec workspaces
  are additional. `GD_createWithCapacities` accepts a separate capacity for each
  generation; `GD_create` and the capacity benchmark use three equal values.
- Dictionary turnover adapts slowly and steadily to recurring payload patterns.
  Older dictionaries receive higher-quality match indexes and query optimization.
  A frame can reference multiple partitions, searching perpetual before maturing
  before adhoc for each remaining original region. Ordinary hits do not copy
  payload. Within a tier, accepted committed regions precede prepare regions.
  `GD_learn` appends only unmatched spans of at least eight bytes to adhoc;
  matching old payload is not reinserted. It returns one contiguous maintenance
  range and does not increase retention heat. Capacity failure writes no bytes.
  `GD_compressTracked` can suppress retention observations while re-encoding an
  existing redundant copy or a just-learned frame. This keeps repetitions and
  self-references from masquerading as independent reuse. Codec match/byte totals
  count selected matches including re-encodes, but exclude discarded plans;
  compressed bytes are unchanged by this flag. `GD_matches` exposes these actual
  local references for workload attribution without a second search/encode.
  Each serialized sender store owns and reuses its sequence workspace; it does
  not allocate a maximum-frame sequence array on the C thread stack. Tests also
  encode/decode a maximum-size frame on a 128 KiB pthread stack.
- Initial business loss on previously unseen, hard-to-compress input is expected.
  Success means that these samples drive dictionary adaptation and subsequent
  similar input becomes referenceable. Learning must survive failed initial
  business delivery; perfect first-frame delivery is not an acceptance condition.
- Before retiring a partition, selected regions are copied into a more mature
  prepare partition; perpetual retention copies into its current prepare.
  Retire invalidates the old epoch and clears its index, readiness and usage.
  The backing bytes remain unchanged until subsequent new-epoch writes. There
  is no payload ownership transfer or reference-counted retirement delay. The
  excess reserved bytes caused by metadata-region granularity are reported.
  `GD_selectMoves` ranks tracked reused bytes against actual retained capacity;
  each current move reserves 4 KiB of destination capacity, even when only a short range
  was used. On equal value, exact referenced edges meeting across adjacent
  blocks take precedence, then lower offsets. The bounded scan runs entirely
  in C. A single genuine reuse can qualify; the old two-touch threshold is not
  used by this selector. Ordinary copies inherit usage; time decay and its
  configuration remain pending. These retention choices are evaluation policy, not
  a wire-format rule or a claim of optimal long-term allocation.
  `GD_compactMoves` provides a bounded cross-tier rotation step: disjoint pairs
  of adjacent selected hot ranges may share a new appended location when direct
  overlap or containment covers at least half of the shorter range and at least
  eight bytes. These are initial evaluation thresholds. Exact per-block hot
  bounding ranges can include cold gaps, whose retained bytes are still charged.
  Comparison is linear in range length; there is no object graph or all-pairs
  search. Capacity is checked against the actual merged append plus ordinary
  moves before mutating either payload or the move list. Perpetual retention
  uses ordinary copies. Source bytes remain unchanged even after rotation. Merged
  bytes are uniquely owned by destination prepare, and returned as one range
  for maintenance; callers must publish it before dependent references. Ordinary
  unmerged regions are copied. New merged payload does not yet inherit heat;
  the planned decay/compaction work must resolve overlapping usage without
  double counting. `payload_relocated` counts all new copies and
  `payload_written` includes them. `payload_transferred` records reserved
  destination capacity, not transferred ownership. `payload_allocated` and
  `payload_peak_allocated` report fixed backing reservation (including borrowed
  buffers), not resident RAM. Retirement does not release that reservation, so
  `payload_freed` remains zero. Valid bytes, RSS, codec scratch and metadata are
  separate quantities. Large retention plans still need a batched product path.
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
test-only per-frame raw dictionary verifies the compressed blocks independently
(the oracle removes the native ID field because ordinary raw dictionaries have ID 0).
Frames are bounded to 65,535 decoded bytes; streaming, trained entropy tables
and split-literal-buffer decoding are not implemented by this experimental API.

The store addresses payload directly by partition base plus offset. A dense
metadata array currently tracks 4 KiB regions; these are a prototype choice,
not a zstd requirement or separate payload allocations. Full receiver regions
release their temporary per-byte validity bitmap. Sender indexes use 8/4/2 ways and maximum
hash logs 20/19/18 from oldest to youngest, with 8/16/32-byte sampling for large
partitions. Each index stores a 32-bit local offset and a separate 16-bit tag;
larger offsets do not reduce fingerprint width. Small correctness fixtures index
every byte. Promotion validates both source and destination epochs.

The runtime selector greedily finds variable-length verified matches, including
after long unmatched prefixes. A linear prefix-cost scan selects the longest
candidate between match boundaries; it can join multiple matches and literal
gaps. The cost estimate uses eight bytes per sequence and sixteen per frame.
Actual encoded bytes, including native headers, must meet the configured ratio.
`GD_setSegmentRatio` accepts 1..100 percent; 50 is the current research assumption,
not an approved deployment default. It is unrelated to promotion capacity limits.
Rejected candidates pass their original bytes to the next half; accepted regions
leave their preceding/following original ranges available for further matching.
A shared scan budget of 24 times input length bounds all six stages together.
Pending ranges and codec workspaces belong to the serialized store, avoiding
recursive C stacks and per-region Go/C calls. Unselected ranges use ordinary
zstd level 3; if the aggregate cannot fit the caller buffer, the store tries one
ordinary frame without committing speculative match statistics.

This is a bounded heuristic, not a proof of the globally longest compressible
byte interval. Greedy matches, estimated costs, match-only endpoints and budget
exhaustion can omit other useful regions. There is no all-pairs trial compression
or fixed-size slicing. Unclaimed scanning and product deadline integration remain
pending. Performance measurements are host codec evidence, not device acceptance.
`GD_observePartition` reports backing reservation, present bytes, extent and a referenced
byte upper bound without exposing dictionary contents. Aggregate matched bytes
are counted separately for each generation.

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

## Observing application traffic

`workload.c` observes an explicitly scoped **inner Ethernet** classic-pcap stream
on stdin without saving packet contents. It evaluates each original frame before
learning from it. Do not feed outer SRFEC copies, loop a recording to warm the
dictionary, join TCP streams, decrypt TLS, or replace an application's encrypted
bytes with its JSON/text/display data. A real workload must retain its normal
encryption and codec settings. HTTPS and secured RDP therefore need measurements
of the encrypted traffic that actually reaches the gateway.

Build using an already built prototype library, then pipe the scoped capture
directly to the observer (arguments: partition bytes, admission interval, client
IPv4 address):

```sh
python3 contrib/generational_dictionary/run.py workload --variant prototype --compile-only --output-root /absolute/task-output
/absolute/task-output/artifacts/prototype/generational_workload 30000000 1 192.0.2.1 < /dev/stdin
python3 contrib/generational_dictionary/workload_test.py /absolute/task-output/artifacts/prototype/generational_workload
```

The byte-only research admission policy appends every Nth original frame whose
external dictionary coverage is below 50%, without application labels or future
knowledge. N=1 deliberately exposes the cost of learning nonrepeating traffic;
larger N explores sampling. Promotion ranks the existing block match counters,
retaining at most a quarter partition. These are experimental choices.

Statistics separate directions and report cumulative input/encoded bytes, a
no-dictionary level-3 comparator, external matches by tier and by header/transport
payload, new writes, transferred/freed bytes and six partition watermarks at
one-second capture-time intervals. `extent` includes reserved gaps; `allocated`
counts fixed backing reservation, not resident RAM or useful hot bytes. `present`
counts valid payload bytes and excludes holes. Transport payload includes
TLS ciphertext and must not be described as application plaintext. Cold-retention
counts are lower bounds. Counters include real inner TCP retransmissions if the
input contains them; retransmission attribution is not yet implemented.

Maintenance is mirrored immediately to a verifier. Each original frame is
round-trip checked once, without loss simulation or ring-copy repair. The byte
model uses business R=2, maintenance R=3, 1,000-byte maintenance chunks and
32-byte record overhead; it is **not measured SRFEC/2 wire traffic**, and it does
not measure packing, PMTU delivery, maintenance retransmissions or latency.
There is no raw fallback in the candidate's encoded-byte count. CPU throughput
is not inferred from capture timestamps. Analyzer RSS includes sender/receiver
verification states for both observed directions, so it is not gateway RSS.

Use a targeted test-flow capture filter and preserve the capture producer's exit
status/drop counters alongside the observer result. The observer rejects clipped
records, unsupported link types, IPv4 fragments, IP packets exceeding 1500 bytes
(including offload super-packets), and timestamp regression. It never silently
splits, sorts or invents wire frames. A successful observer exit alone does not
prove a complete capture. Empty input is an error. No LLM API or RDP workload has
yet been measured with this entry point; its generated behavior fixtures only
validate observation, causality, turnover and invalid-input handling.

For real LLM calls, record streaming versus nonstreaming, short versus growing
context, connection reuse and concurrency. For RDP, record the negotiated
transport/security/graphics mode and exercise idle typing, scrolling, window
movement and video separately. Keep both directions, cold starts, workload
transitions and unchanged-workload periods visible; insufficient traffic to
rotate an older tier is an observation, not a reason to repeat captured bytes.
Use one observation point per original frame. This observer alone cannot measure
the effect of compression on API completion, RDP responsiveness or delivered MTU.
