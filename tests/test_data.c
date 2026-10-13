/* test_data.c - headless tests for the simulation layer (src/data.c).
 *
 * Covers the ring buffer, initialisation, determinism, the tick
 * accumulator, long-run invariants, the alert log and the top-talker
 * query. Links only data.c and data.h, so no Raylib or display is needed.
 * `make test` builds it with ASan/UBSan; CMake registers it for ctest.
 * Exit status is 0 when every CHECK passed, 1 otherwise.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

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
 * history (60 entries), and the alert log stays within its capacity. */
static void test_init(void)
{
    static Dataset d;
    data_init(&d, 1234);
    CHECK(d.bw_in.count == RING_CAP);
    CHECK(d.bw_out.count == RING_CAP);
    CHECK(d.net_latency.count == RING_CAP);
    CHECK(d.devices[0].latency_hist.count == 60);
    CHECK(d.alert_count <= ALERT_CAP);
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
    CHECK(a.alert_serial == b.alert_serial);
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

    int saw_warning = 0, saw_down = 0, saw_peak_alert = 0;
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

    /* Informational only: whether a utilisation alert is still in the log
     * depends on the seed, so it is printed, not asserted. */
    for (int i = 0; i < data_alert_count(&d); i++) {
        if (strstr(data_alert(&d, i)->message, "utilisation"))
            saw_peak_alert = 1;
    }

    CHECK(saw_warning);
    CHECK(saw_down);
    /* Calibration bounds of the generator: busy periods must exceed
     * 300 Mbps, and it must stay physically plausible (the weights sum to
     * ~1.1 Gbps at load 1.0, load is capped at 1.3) and never idle. */
    CHECK(max_in > 300.0f && max_in < 1500.0f);
    CHECK(min_in > 50.0f);
    printf("  inbound range over run: %.0f .. %.0f Mbps, utilisation alert in log: %d\n",
           (double)min_in, (double)max_in, saw_peak_alert);
}

/* Alert log: 40000 ticks produce far more than ALERT_CAP alerts, so the
 * log has wrapped. Checks capacity, newest-first order with consecutive
 * serials, acknowledge toggling, bounds handling and acknowledge-all. */
static void test_alerts(void)
{
    static Dataset d;
    data_init(&d, 5);
    for (int i = 0; i < 40000; i++)
        data_update(&d, SIM_TICK_S);

    CHECK(data_alert_count(&d) == ALERT_CAP);
    /* Newest first: serials strictly decrease with index. */
    for (int i = 1; i < data_alert_count(&d); i++)
        CHECK(data_alert(&d, i - 1)->serial == data_alert(&d, i)->serial + 1);

    data_alert_toggle_ack(&d, 0);
    CHECK(data_alert(&d, 0)->acknowledged);
    data_alert_toggle_ack(&d, 0);
    CHECK(!data_alert(&d, 0)->acknowledged);
    data_alert_toggle_ack(&d, ALERT_CAP + 10);  /* out of range is ignored */

    data_alert_ack_all(&d);
    Summary s;
    data_summary(&d, &s);
    CHECK(s.open_alerts[SEV_INFO] + s.open_alerts[SEV_WARNING] + s.open_alerts[SEV_CRITICAL] == 0);
}

/* Top talkers: result is sorted by rx+tx descending, honours the limit,
 * and with max = DEVICE_COUNT is a permutation of all devices. */
static void test_top_talkers(void)
{
    static Dataset d;
    data_init(&d, 11);
    for (int i = 0; i < 100; i++)
        data_update(&d, SIM_TICK_S);

    int idx[8];
    int n = data_top_talkers(&d, idx, 8);
    CHECK(n == 8);
    for (int i = 1; i < n; i++) {
        const Device *a = &d.devices[idx[i - 1]];
        const Device *b = &d.devices[idx[i]];
        CHECK(a->rx_mbps + a->tx_mbps >= b->rx_mbps + b->tx_mbps);
    }

    /* Every device must appear exactly once when nothing is cut off. */
    int all[DEVICE_COUNT];
    CHECK(data_top_talkers(&d, all, DEVICE_COUNT) == DEVICE_COUNT);
    int seen[DEVICE_COUNT] = { 0 };
    for (int i = 0; i < DEVICE_COUNT; i++)
        seen[all[i]]++;
    for (int i = 0; i < DEVICE_COUNT; i++)
        CHECK(seen[i] == 1);

    CHECK(data_top_talkers(&d, idx, 3) == 3);
}

/* Runs all tests and returns 1 if any check failed. */
int main(void)
{
    test_ring();
    test_init();
    test_determinism();
    test_tick_accumulator();
    test_invariants();
    test_alerts();
    test_top_talkers();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all data tests passed\n");
    return 0;
}
