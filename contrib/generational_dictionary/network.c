/* Bounded synthetic UDP fixture. This is NOT SRFEC/2 or an authenticated wire.
 * Run only on isolated test endpoints. Repository BSD license. */
#define _POSIX_C_SOURCE 200809L
#include "dictionary.h"
#include "fixture_uuid.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#define CAP 8192
#define MTU 1200
#define HEADER 120
#define ANSWER 40
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
enum { WRITE = 1, DATA, ROTATE, STOP, INIT };
static uint64_t rng = 0x825712ULL, transmitted, received;
static unsigned random32(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 16); }
static void bytes(void* p, size_t n) { size_t i; for (i = 0; i < n; ++i) ((unsigned char*)p)[i] = (unsigned char)random32(); }
static void put32(unsigned char* p, uint32_t x) { x = htonl(x); memcpy(p, &x, 4); }
static uint32_t get32(const unsigned char* p) { uint32_t x; memcpy(&x, p, 4); return ntohl(x); }
static void put64(unsigned char* p, uint64_t x) { put32(p, (uint32_t)(x >> 32)); put32(p + 4, (uint32_t)x); }
static uint64_t get64(const unsigned char* p) { return ((uint64_t)get32(p) << 32) | get32(p + 4); }
static void putEpoch(unsigned char* p, GD_Epoch epoch) { memcpy(p, epoch.bytes, 16); }
static GD_Epoch getEpoch(const unsigned char* p) { GD_Epoch epoch; memcpy(epoch.bytes, p, 16); return epoch; }
static uint64_t digest(const unsigned char* p, size_t n) { uint64_t h = 1469598103934665603ULL; size_t i; for (i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ULL; return h; }
static int socket_for(const char* ip, unsigned port, int server)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in addr; struct timeval timeout = {2, 0};
    CHECK(fd >= 0); memset(&addr, 0, sizeof(addr)); addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)port);
    CHECK(inet_pton(AF_INET, ip, &addr.sin_addr) == 1);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    if (server) CHECK(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    else CHECK(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    return fd;
}
static unsigned request(int fd, unsigned char* packet, size_t size, unsigned id, unsigned char answer[ANSWER])
{
    unsigned tries;
    put32(packet + 4, id);
    for (tries = 0; tries < ((get32(packet) & 0xffffU) == DATA ? 1U : 3U); ++tries) {
        ssize_t n;
        CHECK(size <= MTU && send(fd, packet, size, 0) == (ssize_t)size); transmitted += size;
        while ((n = recv(fd, answer, ANSWER, 0)) >= 0) {
            received += (uint64_t)n;
            if (n == ANSWER && get32(answer) == id) return get32(answer + 4);
        }
        CHECK(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    }
    CHECK(0); return 0;
}
static void header(unsigned char* packet, unsigned type)
{ memset(packet, 0, HEADER); put32(packet, 0x47450000U | type); }
static size_t data_packet(GD_Store* tx, ZSTD_CCtx* cc, const unsigned char* frame, unsigned char* packet)
{
    GD_FrameView view; unsigned p; size_t size;
    header(packet, DATA);
    size = GD_compress(tx, cc, packet + HEADER, MTU - HEADER, frame, 1000, &view);
    CHECK(!ZSTD_isError(size)); put32(packet + 8, view.used_mask); put64(packet + 16, digest(frame, 1000));
    for (p = 0; p < GD_PARTITIONS; ++p) putEpoch(packet + 24 + p * 16, view.epoch[p]);
    return HEADER + size;
}
static int serve(const char* ip, unsigned port)
{
    GD_Store* rx = NULL; ZSTD_DCtx* dc = ZSTD_createDCtx();
    int fd = socket_for(ip, port, 1); unsigned packets = 0, good = 0, failures = 0;
    time_t end = time(NULL) + 180; unsigned char packet[MTU + 1], answer[ANSWER], output[1000];
    CHECK(dc); printf("READY UDP %u bounded_seconds=180\n", port); fflush(stdout);
    while (time(NULL) < end && packets < 20000) {
        struct sockaddr_in peer; socklen_t len = sizeof(peer); ssize_t n;
        unsigned type, result = GD_INVALID; uint32_t id;
        n = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr*)&peer, &len);
        if (n < 0) { CHECK(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR); continue; }
        if (n < HEADER || n > MTU || (get32(packet) & 0xffff0000U) != 0x47450000U) continue;
        ++packets; received += (uint64_t)n; type = get32(packet) & 0xffffU; id = get32(packet + 4);
        memset(answer, 0, sizeof(answer)); put32(answer, id);
        if (type == INIT && n == HEADER) {
            GD_Epoch epochs[GD_PARTITIONS]; unsigned p;
            for (p = 0; p < GD_PARTITIONS; ++p) epochs[p] = getEpoch(packet + 24 + p * 16);
            if (!rx) rx = GD_create(CAP, 0, epochs);
            if (rx) {
                result = GD_OK;
                for (p = 0; p < GD_PARTITIONS; ++p)
                    if (!GD_epochEqual(epochs[p], GD_epoch(rx, p))) result = GD_STALE;
            }
        }
        if (rx && type == WRITE) result = GD_write(rx, get32(packet + 8), getEpoch(packet + 16), get32(packet + 12), packet + HEADER, (size_t)n - HEADER);
        if (rx && type == ROTATE) result = GD_rotate(rx, get32(packet + 8), getEpoch(packet + 16), getEpoch(packet + 32), get32(packet + 12), GD_NO_EPOCH, NULL, 0);
        if (rx && type == DATA) {
            GD_FrameView view; unsigned p; size_t decoded;
            view.used_mask = get32(packet + 8); for (p = 0; p < GD_PARTITIONS; ++p) view.epoch[p] = getEpoch(packet + 24 + p * 16);
            decoded = GD_decompress(rx, dc, output, sizeof(output), packet + HEADER, (size_t)n - HEADER, &view);
            result = ZSTD_isError(decoded) ? (unsigned)GD_lastResult(rx) : GD_OK;
            if (result == GD_OK && (decoded != sizeof(output) || digest(output, decoded) != get64(packet + 16))) result = GD_CONFLICT;
            if (result == GD_OK) ++good; else ++failures;
            if (result == GD_MISSING) { const GD_Missing* m = GD_missing(rx); put32(answer + 8, m->partition); put32(answer + 12, m->offset); put32(answer + 16, m->length); putEpoch(answer + 24, m->epoch); }
        }
        if (type == STOP) result = GD_OK;
        put32(answer + 4, result);
        CHECK(sendto(fd, answer, sizeof(answer), 0, (struct sockaddr*)&peer, len) == sizeof(answer)); transmitted += sizeof(answer);
        if (type == STOP) break;
    }
    printf("{\"server_packets\":%u,\"decoded\":%u,\"reference_failures\":%u,\"udp_received_bytes\":%llu,\"udp_sent_bytes\":%llu}\n", packets, good, failures, (unsigned long long)received, (unsigned long long)transmitted);
    GD_free(rx); ZSTD_freeDCtx(dc); close(fd); return 0;
}
static int client(const char* ip, unsigned port)
{
    unsigned char dictionary[3][CAP], frame[1000], packet[MTU], saved[MTU], answer[ANSWER];
    GD_Epoch epochs[GD_PARTITIONS];
    GD_Store* tx; ZSTD_CCtx* cc = ZSTD_createCCtx();
    int fd = socket_for(ip, port, 0); unsigned id = 0, p, at, test; size_t size, saved_size;
    uint64_t maintenance_bytes, business_start; unsigned maintenance_retries = 0, stale = 0, rewritten = 0;
    fixture_initialEpochs(epochs); tx = GD_create(CAP, 1, epochs);
    CHECK(tx && cc); bytes(dictionary, sizeof(dictionary));
    header(packet, INIT);
    for (p = 0; p < GD_PARTITIONS; ++p) putEpoch(packet + 24 + p * 16, epochs[p]);
    CHECK(request(fd, packet, HEADER, ++id, answer) == GD_OK);
    CHECK(request(fd, packet, HEADER, ++id, answer) == GD_OK); /* Duplicate announcement. */
    for (p = 0; p < 3; ++p) {
        unsigned slot = 2 * p + 1;
        CHECK(GD_write(tx, slot, GD_epoch(tx, slot), 0, dictionary[p], CAP) == GD_OK);
        /* Reverse arrival order is deliberate. Range [0,512) in perpetual is
         * withheld to force a genuine missing-range response over the network. */
        for (at = CAP; at; at -= 512) {
            if (p == 0 && at == 512) continue;
            header(packet, WRITE); put32(packet + 8, slot); put32(packet + 12, at - 512); putEpoch(packet + 16, GD_epoch(tx, slot));
            memcpy(packet + HEADER, dictionary[p] + at - 512, 512);
            CHECK(request(fd, packet, HEADER + 512, ++id, answer) == GD_OK);
        }
    }
    maintenance_bytes = transmitted;
    for (p = 0; p < 3; ++p) memcpy(frame + p * 300, dictionary[p] + 16, 300);
    bytes(frame + 900, 100);
    size = data_packet(tx, cc, frame, packet);
    CHECK(request(fd, packet, size, ++id, answer) == GD_MISSING);
    CHECK(get32(answer + 8) == 1 && get32(answer + 12) < 512);
    header(packet, WRITE); put32(packet + 8, 1); putEpoch(packet + 16, GD_epoch(tx, 1)); memcpy(packet + HEADER, dictionary[0], 512);
    CHECK(request(fd, packet, HEADER + 512, ++id, answer) == GD_OK); ++maintenance_retries;
    maintenance_bytes += HEADER + 512;
    business_start = transmitted;
    for (test = 0; test < 100; ++test) {
        for (p = 0; p < 3; ++p) memcpy(frame + p * 300, dictionary[p] + random32() % (CAP - 300), 300);
        bytes(frame + 900, 100); size = data_packet(tx, cc, frame, packet);
        CHECK(request(fd, packet, size, ++id, answer) == GD_OK);
    }
    /* Three existing copies of one frame: first before retirement is valid,
     * second arrives after retirement and fails, third is rewritten in place. */
    memcpy(frame, dictionary[0], 1000); saved_size = data_packet(tx, cc, frame, saved);
    CHECK(request(fd, saved, saved_size, ++id, answer) == GD_OK);
    for (test = 0; test < 2; ++test) {
        unsigned source = GD_committed(tx, 0); GD_Epoch epoch = GD_epoch(tx, source), replacement = fixture_newEpoch();
        CHECK(GD_rotate(tx, 0, epoch, replacement, source, GD_NO_EPOCH, NULL, 0) == GD_OK);
        header(packet, ROTATE); put32(packet + 8, 0); put32(packet + 12, source); putEpoch(packet + 16, epoch); putEpoch(packet + 32, replacement);
        CHECK(request(fd, packet, HEADER, ++id, answer) == GD_OK);
    }
    CHECK(request(fd, saved, saved_size, ++id, answer) == GD_STALE); ++stale;
    /* Give the last planned copy a new adhoc reference, never add a fourth. */
    { unsigned slot = GD_prepare(tx, 2); GD_Epoch epoch = GD_epoch(tx, GD_committed(tx, 2)), replacement = fixture_newEpoch();
      CHECK(GD_rotate(tx, 2, epoch, replacement, 3, GD_NO_EPOCH, NULL, 0) == GD_OK);
      header(packet, ROTATE); put32(packet + 8, 2); put32(packet + 12, 3); putEpoch(packet + 16, epoch); putEpoch(packet + 32, replacement);
      CHECK(request(fd, packet, HEADER, ++id, answer) == GD_OK);
      slot = GD_prepare(tx, 2); CHECK(GD_write(tx, slot, GD_epoch(tx, slot), 0, frame, 1000) == GD_OK);
      header(packet, WRITE); put32(packet + 8, slot); putEpoch(packet + 16, GD_epoch(tx, slot)); memcpy(packet + HEADER, frame, 1000);
      CHECK(request(fd, packet, HEADER + 1000, ++id, answer) == GD_OK); }
    saved_size = data_packet(tx, cc, frame, saved); ++rewritten;
    CHECK(request(fd, saved, saved_size, ++id, answer) == GD_OK);
    header(packet, STOP); CHECK(request(fd, packet, HEADER, ++id, answer) == GD_OK);
    printf("{\"requests\":%u,\"maintenance_initial_and_hole_retry_bytes\":%llu,\"business_and_turnover_bytes\":%llu,\"maintenance_hole_repairs\":%u,\"stale_rejections\":%u,\"planned_retire_copies\":3,\"sent_retire_copies\":3,\"rewritten_unsent_copies\":%u,\"max_datagram\":1200,\"udp_sent_bytes\":%llu,\"udp_received_bytes\":%llu}\n", id,
        (unsigned long long)maintenance_bytes, (unsigned long long)(transmitted - business_start), maintenance_retries, stale, rewritten, (unsigned long long)transmitted, (unsigned long long)received);
    GD_free(tx); ZSTD_freeCCtx(cc); close(fd); return 0;
}
int main(int argc, char** argv)
{
    unsigned long port;
    CHECK(argc == 4); port = strtoul(argv[3], NULL, 10); CHECK(port > 1024 && port < 65536);
    if (!strcmp(argv[1], "serve")) return serve(argv[2], (unsigned)port);
    CHECK(!strcmp(argv[1], "send")); return client(argv[2], (unsigned)port);
}
