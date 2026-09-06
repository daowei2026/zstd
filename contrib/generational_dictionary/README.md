# Append-only generational dictionary prototype

This fork research starts from zstd v1.5.7, commit
`f8745da6ff1ad1e7bab384bd1f9d742439278e99`, on
`codex/append-only-generational-prototype`. It investigates the dictionary
mechanism for SRFEC/2; it does not implement or deploy that protocol.

The measured first-round results are in [EVALUATION.md](EVALUATION.md).

## Agreed research contract

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
  before adhoc at each matching position. Ordinary hits do not copy payload.
  Within a tier, a usable committed match also precedes any prepare match.
  `GD_learn` appends only unmatched spans of at least eight bytes to adhoc;
  matching old payload is not reinserted. It returns one contiguous maintenance
  range and does not increase retention heat. Capacity failure writes no bytes.
  `GD_compressTracked` can suppress retention observations while re-encoding an
  existing redundant copy or a just-learned frame. This keeps repetitions and
  self-references from masquerading as independent reuse. Codec match/byte totals
  still count the performed work; compressed bytes are unchanged by this flag.
  Each serialized sender store owns and reuses its sequence workspace; it does
  not allocate a maximum-frame sequence array on the C thread stack. Tests also
  encode/decode a maximum-size frame on a 128 KiB pthread stack.
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
Virtual slots use the largest of the three capacities as their stride; each
partition enforces its own usable bound, and virtual gaps allocate no payload.
`GD_observePartition` reports allocation, present bytes, extent and a referenced
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
counts owned allocation blocks, not useful hot bytes. Transport payload includes
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
