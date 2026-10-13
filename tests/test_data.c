/* test_data.c - headless tests for the simulation layer (src/data.c).
 *
 * Covers the ring buffer, initialisation, determinism, the tick
 * accumulator and long-run invariants. Links only data.c and data.h, so no
 * Raylib or display is needed.
 * `make test` builds it with ASan/UBSan; CMake registers it for ctest.
 * Exit status is 0 when every CHECK passed, 1 otherwise.
 */
#include <math.h>
#include <stdio.h>

#include "data.h"

/* Number of failed CHECKs; reported and turned into the exit code in main. */
static int failures;

/* Records a failure with file, line and expression, but keeps running so
 * one run reports every broken check. */
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* Ring: overwrite of the oldest sample when full, index order, copy and
 * clamping of an oversized capacity. */
static void test_ring(void)
{
    Ring r;
    ring_init(&r, 4);
    CHECK(r.count == 0);
    /* 6 pushes into a ring of 4: samples 1 and 2 must have been dropped. */
    for (int i = 1; i <= 6; i++)
        ring_push(&r, (float)i);
    CHECK(r.count == 4);
    CHECK(ring_at(&r, 0) == 3.0f);
    CHECK(ring_at(&r, 3) == 6.0f);
    CHECK(ring_last(&r) == 6.0f);

    float out[RING_CAP];
    CHECK(ring_copy(&r, out) == 4);
    CHECK(out[0] == 3.0f && out[1] == 4.0f && out[2] == 5.0f && out[3] == 6.0f);

    /* Capacity beyond the backing array is clamped. */
    ring_init(&r, RING_CAP * 4);
    CHECK(r.cap == RING_CAP);
}

/* Init: the warm-up fills every history ring and the device latency
 * history (60 entries). */
static void test_init(void)
{
    static Dataset d;
    data_init(&d, 1234);
    CHECK(d.bw_in.count == RING_CAP);
    CHECK(d.bw_out.count == RING_CAP);
    CHECK(d.net_latency.count == RING_CAP);
    CHECK(d.devices[0].latency_hist.count == 60);
}

/* Determinism: the same seed gives the same trajectory, a different seed
 * a different one. 2000 updates of 16 ms (~60 FPS frames) are ~64 ticks. */
static void test_determinism(void)
{
    static Dataset a, b;
    data_init(&a, 42);
    data_init(&b, 42);
    for (int i = 0; i < 2000; i++) {
        data_update(&a, 0.016);
        data_update(&b, 0.016);
    }
    CHECK(a.tick_count == b.tick_count);
    CHECK(ring_last(&a.bw_in) == ring_last(&b.bw_in));
    for (int i = 0; i < DEVICE_COUNT; i++)
        CHECK(a.devices[i].latency_ms == b.devices[i].latency_ms);

    static Dataset c;
    data_init(&c, 43);
    for (int i = 0; i < 2000; i++)
        data_update(&c, 0.016);
    CHECK(ring_last(&a.bw_in) != ring_last(&c.bw_in));
}

/* Tick accumulator: partial intervals accumulate without ticking, the
 * remainder is carried over, and one long step runs several ticks. */
static void test_tick_accumulator(void)
{
    static Dataset d;
    data_init(&d, 7);
    uint32_t base = d.tick_count;
    data_update(&d, SIM_TICK_S * 0.4);
    CHECK(d.tick_count == base);
    /* 0.4 + 0.7 = 1.1 ticks: one tick, 0.1 left in the accumulator. */
    data_update(&d, SIM_TICK_S * 0.7);
    CHECK(d.tick_count == base + 1);
    CHECK(d.tick_acc >= 0.0f && d.tick_acc < SIM_TICK_S);
    /* 0.1 + 5.0 = 5.1 ticks: five more ticks. */
    data_update(&d, SIM_TICK_S * 5.0);
    CHECK(d.tick_count == base + 6);
}

/* Long-run invariants over 20000 ticks (about 2.8 h simulated; with an
 * incident every ~13 s this yields hundreds of faults): value ranges,
 * status counts, outage semantics, aggregate = sum of devices, and that
 * both WARNING and DOWN states actually occur. */
static void test_invariants(void)
{
    static Dataset d;
    data_init(&d, 99);

    int saw_warning = 0, saw_down = 0;
    float max_in = 0.0f, min_in = 1e9f;

    for (int step = 0; step < 20000; step++) {
        data_update(&d, SIM_TICK_S);

        Summary s;
        data_summary(&d, &s);
        CHECK(s.up + s.warning + s.down == DEVICE_COUNT);

        float rx = 0.0f, tx = 0.0f;
        for (int i = 0; i < DEVICE_COUNT; i++) {
            const Device *dev = &d.devices[i];
            CHECK(dev->latency_ms >= 0.0f && dev->latency_ms < 1000.0f);
            CHECK(dev->loss_pct >= 0.0f && dev->loss_pct <= 100.0f);
            CHECK(dev->rx_mbps >= 0.0f && dev->tx_mbps >= 0.0f);
            /* A DOWN device carries no traffic. */
            if (dev->status == STATUS_DOWN) {
                CHECK(dev->rx_mbps == 0.0f && dev->tx_mbps == 0.0f);
                saw_down = 1;
            }
            if (dev->status == STATUS_WARNING)
                saw_warning = 1;
            rx += dev->rx_mbps;
            tx += dev->tx_mbps;
        }
        /* Aggregate history must equal the sum of device rates. */
        CHECK(fabsf(ring_last(&d.bw_in) - rx) < 0.01f);
        CHECK(fabsf(ring_last(&d.bw_out) - tx) < 0.01f);
        if (rx > max_in) max_in = rx;
        if (rx < min_in) min_in = rx;
    }

    CHECK(saw_warning);
    CHECK(saw_down);
    /* Calibration bounds of the generator: busy periods must exceed
     * 300 Mbps, and it must stay physically plausible (the weights sum to
     * ~1.1 Gbps at load 1.0, load is capped at 1.3) and never idle. */
    CHECK(max_in > 300.0f && max_in < 1500.0f);
    CHECK(min_in > 50.0f);
    printf("  inbound range over run: %.0f .. %.0f Mbps\n", (double)min_in, (double)max_in);
}

/* Runs all tests and returns 1 if any check failed. */
int main(void)
{
    test_ring();
    test_init();
    test_determinism();
    test_tick_accumulator();
    test_invariants();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all data tests passed\n");
    return 0;
}
