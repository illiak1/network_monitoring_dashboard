/* data.c - network simulation: generators, fault injection, alert log.
 *
 * Sections: RNG (xorshift32 helpers), Ring (history buffer), Alerts
 * (circular log), Devices (catalogue, per-device step, status
 * classification), Simulation (one tick, warm-up, update loop) and
 * Queries (summary, top talkers).
 * No Raylib dependency; the dashboard reads the result through data.h and
 * tests/test_data.c exercises it headless. Simulated values are a pure
 * function of the seed and the tick count, so runs are reproducible (only
 * alert timestamps use the wall clock).
 */
#include "data.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TWO_PI 6.28318530717958647692f
/* Ticks simulated at start-up so every history ring is full on the first frame. */
#define WARMUP_TICKS RING_CAP

/* ---------------------------------------------------------------- RNG */

/* xorshift32 (Marsaglia, shifts 13/17/5): full period 2^32 - 1, so the
 * state must never be 0. Cheap, deterministic and portable. */
static uint32_t rng_u32(Dataset *d)
{
    uint32_t x = d->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    d->rng_state = x;
    return x;
}

/* Uniform float in [0, 1). Only the top 24 bits are used because a float
 * mantissa holds 24 bits; this keeps the result exactly representable and
 * strictly below 1. */
static float rng_unit(Dataset *d)
{
    return (float)(rng_u32(d) >> 8) * (1.0f / 16777216.0f);
}

/* Uniform float in [lo, hi). */
static float rng_range(Dataset *d, float lo, float hi)
{
    return lo + (hi - lo) * rng_unit(d);
}

/* Irwin-Hall approximation of N(0,1): sum of 4 uniforms, variance 1/3. */
static float rng_gauss(Dataset *d)
{
    /* The sum has mean 2 and standard deviation 1/sqrt(3); the factor
     * sqrt(3) = 1.7320508 normalises it to unit variance. */
    float s = rng_unit(d) + rng_unit(d) + rng_unit(d) + rng_unit(d);
    return (s - 2.0f) * 1.7320508f;
}

/* Clamps x to [lo, hi]. */
static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

/* --------------------------------------------------------------- Ring */

/* Empties the ring. cap is clamped to RING_CAP (fixed backing array). */
void ring_init(Ring *r, int cap)
{
    memset(r, 0, sizeof *r);
    r->cap = cap > RING_CAP ? RING_CAP : cap;
}

/* Appends a sample, overwriting the oldest one once the ring is full. */
void ring_push(Ring *r, float value)
{
    r->v[r->head] = value;
    r->head = (r->head + 1) % r->cap;
    if (r->count < r->cap)
        r->count++;
}

/* Returns sample i counted from the oldest. The oldest element sits at
 * head - count; C's % can go negative, hence the wrap fix-up. */
float ring_at(const Ring *r, int i)
{
    int idx = (r->head - r->count + i) % r->cap;
    if (idx < 0)
        idx += r->cap;
    return r->v[idx];
}

/* Newest sample, or 0 for an empty ring. */
float ring_last(const Ring *r)
{
    return r->count > 0 ? ring_at(r, r->count - 1) : 0.0f;
}

/* Linearises the ring oldest to newest into out (needs r->count floats).
 * Returns the number of samples copied. */
int ring_copy(const Ring *r, float *out)
{
    for (int i = 0; i < r->count; i++)
        out[i] = ring_at(r, i);
    return r->count;
}

/* ------------------------------------------------------------- Alerts */

/* Appends an alert to the circular log (overwriting the oldest when full).
 * device is a device index or -1; source is the label shown in the log;
 * fmt/... build the message (truncated to the buffer). Side effects:
 * advances alert_head and alert_serial. The timestamp is back-dated by
 * time_shift while the warm-up runs. */
static void alert_add(Dataset *d, AlertSeverity sev, int device, const char *source, const char *fmt, ...)
{
    Alert *a = &d->alerts[d->alert_head];
    memset(a, 0, sizeof *a);
    a->serial = ++d->alert_serial;
    a->tick = d->tick_count;
    a->severity = sev;
    a->when = time(NULL) - (time_t)d->time_shift;
    a->device = device;
    snprintf(a->source, sizeof a->source, "%s", source);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->message, sizeof a->message, fmt, ap);
    va_end(ap);

    d->alert_head = (d->alert_head + 1) % ALERT_CAP;
    if (d->alert_count < ALERT_CAP)
        d->alert_count++;
}

/* Number of alerts currently in the log. */
int data_alert_count(const Dataset *d)
{
    return d->alert_count;
}

/* Maps an age index (0 = newest) to the storage index in the circular
 * array. The newest entry is at alert_head - 1. */
static int alert_index(const Dataset *d, int i)
{
    int idx = (d->alert_head - 1 - i) % ALERT_CAP;
    return idx < 0 ? idx + ALERT_CAP : idx;
}

/* Alert by age (0 = newest). i must be in [0, data_alert_count). */
const Alert *data_alert(const Dataset *d, int i)
{
    return &d->alerts[alert_index(d, i)];
}

/* Flips the acknowledged flag of alert i; out-of-range indices are ignored. */
void data_alert_toggle_ack(Dataset *d, int i)
{
    if (i < 0 || i >= d->alert_count)
        return;
    Alert *a = &d->alerts[alert_index(d, i)];
    a->acknowledged = !a->acknowledged;
}

/* Marks every alert in the log as acknowledged. */
void data_alert_ack_all(Dataset *d)
{
    for (int i = 0; i < d->alert_count; i++)
        d->alerts[alert_index(d, i)].acknowledged = true;
}

/* ------------------------------------------------------------ Devices */

/* Static description of one monitored device; copied into Device by
 * device_init. rx/tx are the rates at load 1.0 (see device_step). */
typedef struct {
    const char *name;
    const char *ip;
    DeviceKind kind;
    float base_latency_ms;  /* latency of an unloaded, healthy device */
    float rx_mbps;
    float tx_mbps;
    float burst_chance;     /* per-tick probability of a traffic burst */
} DeviceSpec;

/* Device catalogue, one entry per DEVICE_COUNT slot. Servers, storage and
 * backup carry most of the traffic and burst more often, network gear is
 * light. At the mean load of 0.5 the aggregate is about 550 Mbps inbound,
 * so the 85% utilisation alarm needs a coincident peak, not steady state. */
static const DeviceSpec DEVICE_SPECS[DEVICE_COUNT] = {
    { "core-rtr-01", "10.0.0.1",    KIND_ROUTER,       6.0f,   4.0f,   4.0f, 0.005f },
    { "edge-fw-01",  "10.0.0.2",    KIND_FIREWALL,     9.0f,   6.0f,   6.0f, 0.005f },
    { "core-sw-01",  "10.0.1.1",    KIND_SWITCH,       4.0f,   3.0f,   3.0f, 0.005f },
    { "core-sw-02",  "10.0.1.2",    KIND_SWITCH,       5.0f,   3.0f,   3.0f, 0.005f },
    { "ap-floor1",   "10.0.2.11",   KIND_ACCESS_POINT, 14.0f, 85.0f,  30.0f, 0.010f },
    { "ap-floor2",   "10.0.2.12",   KIND_ACCESS_POINT, 16.0f, 70.0f,  25.0f, 0.010f },
    { "web-01",      "10.0.10.21",  KIND_SERVER,       22.0f, 60.0f, 220.0f, 0.012f },
    { "web-02",      "10.0.10.22",  KIND_SERVER,       24.0f, 55.0f, 200.0f, 0.012f },
    { "db-01",       "10.0.10.31",  KIND_SERVER,       28.0f, 80.0f,  70.0f, 0.008f },
    { "nas-01",      "10.0.10.40",  KIND_STORAGE,      18.0f, 220.0f, 150.0f, 0.015f },
    { "backup-srv",  "10.0.10.50",  KIND_SERVER,       32.0f, 300.0f, 20.0f, 0.035f },
    { "vpn-gw-01",   "10.0.0.9",    KIND_GATEWAY,      46.0f, 90.0f,  80.0f, 0.010f },
    { "dns-01",      "10.0.10.5",   KIND_SERVER,       12.0f,  6.0f,   6.0f, 0.005f },
    { "cctv-nvr",    "10.0.20.5",   KIND_STORAGE,      20.0f, 120.0f, 10.0f, 0.004f },
};

/* Display name of a device kind. */
const char *device_kind_name(DeviceKind k)
{
    static const char *const names[] = { "Router", "Firewall", "Switch", "Access point", "Server", "Storage", "Gateway" };
    return names[k];
}

/* Display name of a device status. */
const char *device_status_name(DeviceStatus s)
{
    static const char *const names[] = { "Up", "Warning", "Down" };
    return names[s];
}

/* Display name of an alert severity. */
const char *alert_severity_name(AlertSeverity s)
{
    static const char *const names[] = { "Info", "Warning", "Critical" };
    return names[s];
}

/* Initialises one device from its spec. Consumes RNG values so that phases,
 * periods and uptimes differ per device (no synchronised curves). */
static void device_init(Dataset *d, Device *dev, const DeviceSpec *s)
{
    memset(dev, 0, sizeof *dev);
    dev->name = s->name;
    dev->ip = s->ip;
    dev->kind = s->kind;
    dev->status = STATUS_UP;
    dev->base_latency_ms = s->base_latency_ms;
    dev->rx_weight = s->rx_mbps;
    dev->tx_weight = s->tx_mbps;
    dev->burst_chance = s->burst_chance;
    dev->phase_lat = rng_range(d, 0.0f, TWO_PI);
    dev->phase_load = rng_range(d, 0.0f, TWO_PI);
    dev->period_load = rng_range(d, 35.0f, 90.0f);
    dev->uptime_s = rng_range(d, 3.0f * 86400.0f, 90.0f * 86400.0f);
    ring_init(&dev->latency_hist, 60);  /* 60 ticks = 30 s, the detail chart window */
    dev->latency_ms = s->base_latency_ms;
}

/* Starts a degraded-performance incident: random duration and one of three
 * latency/loss profiles. At full fault level every profile crosses at
 * least one WARNING threshold (latency >= 100 ms or loss >= 1.5%). */
static void fault_begin_degraded(Dataset *d, Device *dev)
{
    dev->fault = FAULT_DEGRADED;
    dev->fault_timer = rng_range(d, 10.0f, 28.0f);

    /* Latency-only, loss-only, or both. */
    switch (rng_u32(d) % 3) {
    case 0:
        dev->fault_extra_lat = rng_range(d, 110.0f, 220.0f);
        dev->fault_extra_loss = rng_range(d, 0.2f, 0.8f);
        break;
    case 1:
        dev->fault_extra_lat = rng_range(d, 15.0f, 45.0f);
        dev->fault_extra_loss = rng_range(d, 2.5f, 9.0f);
        break;
    default:
        dev->fault_extra_lat = rng_range(d, 90.0f, 160.0f);
        dev->fault_extra_loss = rng_range(d, 1.8f, 5.0f);
        break;
    }
}

/* Starts an outage of 8-20 s. Uptime restarts from 0 as after a reboot. */
static void fault_begin_outage(Dataset *d, Device *dev)
{
    dev->fault = FAULT_OUTAGE;
    dev->fault_timer = rng_range(d, 8.0f, 20.0f);
    dev->uptime_s = 0.0f;
}

/* Advances the incident state of one device by a tick: counts down an
 * active fault, or rolls for a new one. */
static void fault_step(Dataset *d, Device *dev)
{
    if (dev->fault != FAULT_NONE) {
        dev->fault_timer -= SIM_TICK_S;
        if (dev->fault_timer <= 0.0f)
            dev->fault = FAULT_NONE;
        return;
    }

    /* Per-tick incident probabilities: roughly one incident every 13 s fleet-wide
     * (0.05% outage + 0.22% degraded = 0.27% per device and tick;
     * x 14 devices x 2 ticks/s = ~0.076 incidents/s). */
    float roll = rng_unit(d);
    if (roll < 0.0005f)
        fault_begin_outage(d, dev);
    else if (roll < 0.0027f)
        fault_begin_degraded(d, dev);
}

/* Hysteresis keeps a metric hovering at the threshold from flapping status. */
static DeviceStatus classify(const Device *dev)
{
    if (dev->fault == FAULT_OUTAGE)
        return STATUS_DOWN;
    /* Once in WARNING, the metrics must fall to 80% of the limit to recover. */
    float k = dev->status == STATUS_WARNING ? 0.8f : 1.0f;
    if (dev->latency_ms >= DEVICE_LATENCY_WARN_MS * k || dev->loss_pct >= DEVICE_LOSS_WARN_PCT * k)
        return STATUS_WARNING;
    return STATUS_UP;
}

/* Logs an alert for a status change of device idx: DOWN -> critical,
 * WARNING -> warning (message names the metric that tripped), recovery ->
 * info. Side effect: appends to the alert log. */
static void report_transition(Dataset *d, int idx, DeviceStatus from, DeviceStatus to)
{
    const Device *dev = &d->devices[idx];
    if (to == STATUS_DOWN) {
        alert_add(d, SEV_CRITICAL, idx, dev->name, "Host unreachable: ICMP timeout");
    } else if (to == STATUS_WARNING) {
        if (dev->latency_ms >= DEVICE_LATENCY_WARN_MS)
            alert_add(d, SEV_WARNING, idx, dev->name, "High latency: %.0f ms", (double)dev->latency_ms);
        else
            alert_add(d, SEV_WARNING, idx, dev->name, "Packet loss: %.1f%%", (double)dev->loss_pct);
    } else if (from == STATUS_DOWN) {
        alert_add(d, SEV_INFO, idx, dev->name, "Host back online");
    } else {
        alert_add(d, SEV_INFO, idx, dev->name, "Metrics back to normal");
    }
}

/* Advances one device by one tick and updates its measured values.
 * global_load is the shared traffic component for this tick. Side effects:
 * pushes latency history, may raise an alert on a status change. */
static void device_step(Dataset *d, int idx, float global_load)
{
    Device *dev = &d->devices[idx];
    const float t = (float)d->sim_time;

    /* 1. Incident state, then ease fault_level toward its target so a
     * fault ramps in (30% of the gap per tick) and out (20%) instead of
     * stepping. */
    fault_step(d, dev);

    float level_target = dev->fault == FAULT_DEGRADED ? 1.0f : 0.0f;
    float rate = level_target > dev->fault_level ? 0.30f : 0.20f;
    dev->fault_level += (level_target - dev->fault_level) * rate;
    if (dev->fault == FAULT_NONE && dev->fault_level < 0.01f)
        dev->fault_level = 0.0f;

    /* 2. Noise processes. lat_noise and load_walk are first-order
     * low-pass filtered noise (decay 0.80 and 0.92 per tick) so the
     * curves wander instead of jittering. Bursts decay by 0.88 per tick
     * (half-life ~2.7 s) and retrigger with burst_chance. */
    dev->lat_noise = dev->lat_noise * 0.80f + rng_gauss(d) * dev->base_latency_ms * 0.035f;
    dev->load_walk = dev->load_walk * 0.92f + rng_gauss(d) * 0.045f;
    dev->burst *= 0.88f;
    if (rng_unit(d) < dev->burst_chance)
        dev->burst = rng_range(d, 0.25f, 0.55f);

    /* 3. Load factor: 0.5 baseline + per-device sine + shared component +
     * walk + burst. Range [0.03, 1.30]: the floor keeps idle devices
     * visibly alive, values above 1 represent overload peaks. */
    float load = 0.50f + 0.20f * sinf(TWO_PI * t / dev->period_load + dev->phase_load) + global_load + dev->load_walk + dev->burst;
    load = clampf(load, 0.03f, 1.30f);

    /* 4. Measured values. An outage reads as unreachable (0 latency,
     * 100% loss, no traffic). Otherwise latency is base x slow sine
     * (+-10%, 31 s period) + |noise| (never below the physical floor) +
     * fault contribution; a degraded link also carries up to 50% less
     * traffic (throttle). */
    if (dev->fault == FAULT_OUTAGE) {
        dev->latency_ms = 0.0f;
        dev->loss_pct = 100.0f;
        dev->rx_mbps = 0.0f;
        dev->tx_mbps = 0.0f;
    } else {
        float lat = dev->base_latency_ms * (1.0f + 0.10f * sinf(TWO_PI * t / 31.0f + dev->phase_lat))
                  + fabsf(dev->lat_noise)
                  + dev->fault_level * dev->fault_extra_lat;
        float loss = 0.05f + 0.04f * fabsf(rng_gauss(d)) + dev->fault_level * dev->fault_extra_loss;
        float throttle = 1.0f - 0.5f * dev->fault_level;
        dev->latency_ms = lat;
        dev->loss_pct = clampf(loss, 0.0f, 100.0f);
        dev->rx_mbps = dev->rx_weight * load * throttle;
        dev->tx_mbps = dev->tx_weight * load * throttle;
        dev->uptime_s += SIM_TICK_S;
    }

    ring_push(&dev->latency_hist, dev->latency_ms);

    /* 5. Status from the new values; log only actual changes. */
    DeviceStatus next = classify(dev);
    if (next != dev->status) {
        DeviceStatus prev = dev->status;
        dev->status = next;
        report_transition(d, idx, prev, next);
    }
}

/* -------------------------------------------------------- Simulation */

/* Adds a harmless Info entry (backup done, poll finished, ...) so the log
 * is not made of incidents only. Skipped when the picked device is DOWN,
 * since an unreachable host cannot report anything. */
static void emit_info_event(Dataset *d)
{
    static const char *const events[] = {
        "Configuration backup completed",
        "SNMP poll cycle finished",
        "Firmware check: up to date",
        "ARP table refreshed",
        "NTP offset within tolerance",
    };
    int idx = (int)(rng_u32(d) % DEVICE_COUNT);
    if (d->devices[idx].status == STATUS_DOWN)
        return;
    alert_add(d, SEV_INFO, idx, d->devices[idx].name, "%s", events[rng_u32(d) % (sizeof events / sizeof events[0])]);
}

/* Raises a warning when link utilisation passes 85% and an info when it
 * falls below 75% again. The 10-point gap is the hysteresis that prevents
 * a flood of alerts while the value hovers near the limit. The alert has
 * no device (-1) and uses the source "wan-uplink". */
static void check_utilization(Dataset *d, float in_mbps, float out_mbps)
{
    float util = fmaxf(in_mbps, out_mbps) / LINK_CAPACITY_MBPS;
    if (!d->util_alarm && util > 0.85f) {
        d->util_alarm = true;
        alert_add(d, SEV_WARNING, -1, "wan-uplink", "Link utilisation at %.0f%%", (double)(util * 100.0f));
    } else if (d->util_alarm && util < 0.75f) {
        d->util_alarm = false;
        alert_add(d, SEV_INFO, -1, "wan-uplink", "Link utilisation back to %.0f%%", (double)(util * 100.0f));
    }
}

/* Runs one simulation tick (SIM_TICK_S of simulated time): steps every
 * device, records the aggregate histories and raises link-level alerts. */
static void sim_tick(Dataset *d)
{
    /* 1. Advance the clock. sim_time is derived from the integer tick
     * count instead of being accumulated, so it cannot drift. */
    d->tick_count++;
    d->sim_time = (double)d->tick_count * (double)SIM_TICK_S;
    const float t = (float)d->sim_time;

    /* 2. Shared slow components (70 s and 23 s periods, incommensurate)
     * give the aggregate a visible rhythm that does not repeat quickly. */
    float global_load = 0.12f * sinf(TWO_PI * t / 70.0f) + 0.05f * sinf(TWO_PI * t / 23.0f + 1.3f);

    float in_total = 0.0f, out_total = 0.0f;
    float lat_sum = 0.0f, loss_sum = 0.0f;
    int live = 0;

    /* 3. Step all devices and aggregate. DOWN devices report 0 ms and
     * 100% loss, so they are excluded from the averages to keep the
     * network gauges meaningful. */
    for (int i = 0; i < DEVICE_COUNT; i++) {
        device_step(d, i, global_load);
        const Device *dev = &d->devices[i];
        in_total += dev->rx_mbps;
        out_total += dev->tx_mbps;
        if (dev->status != STATUS_DOWN) {
            lat_sum += dev->latency_ms;
            loss_sum += dev->loss_pct;
            live++;
        }
    }

    /* 4. Record the aggregate history. */
    ring_push(&d->bw_in, in_total);
    ring_push(&d->bw_out, out_total);
    ring_push(&d->net_latency, live ? lat_sum / (float)live : 0.0f);
    ring_push(&d->net_loss, live ? loss_sum / (float)live : 0.0f);

    /* 5. Link alarm and a random info event (p = 0.012 per tick, about
     * one every 42 s). */
    check_utilization(d, in_total, out_total);
    if (rng_unit(d) < 0.012f)
        emit_info_event(d);
}

/* Resets the dataset and runs WARMUP_TICKS ticks so charts, gauges and the
 * alert log are populated at the first frame. seed 0 is replaced because
 * xorshift32 would stay at 0 forever. */
void data_init(Dataset *d, uint32_t seed)
{
    /* 1. Clear and seed. */
    memset(d, 0, sizeof *d);
    d->rng_state = seed ? seed : 0x9E3779B9u;

    /* 2. Devices and histories. */
    for (int i = 0; i < DEVICE_COUNT; i++)
        device_init(d, &d->devices[i], &DEVICE_SPECS[i]);
    ring_init(&d->bw_in, RING_CAP);
    ring_init(&d->bw_out, RING_CAP);
    ring_init(&d->net_latency, RING_CAP);
    ring_init(&d->net_loss, RING_CAP);

    /* 3. Warm-up. time_shift counts down the simulated seconds that are
     * still to come, so alerts raised here appear spread over the last
     * two minutes instead of all carrying the start-up time. */
    for (int i = 0; i < WARMUP_TICKS; i++) {
        d->time_shift = (float)(WARMUP_TICKS - i) * SIM_TICK_S;
        sim_tick(d);
    }
    d->time_shift = 0.0f;

    /* 4. History from the warm-up is closed unless its device is still unhealthy. */
    for (int i = 0; i < d->alert_count; i++) {
        Alert *a = &d->alerts[alert_index(d, i)];
        bool healthy = a->device < 0 || d->devices[a->device].status == STATUS_UP;
        if (a->severity == SEV_INFO || healthy)
            a->acknowledged = true;
    }
}

/* Advances the simulation by dt seconds of real time. Time is
 * accumulated and consumed in fixed SIM_TICK_S steps, so the data does not
 * depend on the frame rate; several ticks run after a long frame. */
void data_update(Dataset *d, double dt)
{
    d->tick_acc += (float)dt;
    while (d->tick_acc >= SIM_TICK_S) {
        d->tick_acc -= SIM_TICK_S;
        sim_tick(d);
    }
}

/* ----------------------------------------------------------- Queries */

/* Fills out with the current snapshot: status counts, aggregate rates,
 * utilisation, averages over reachable devices and open alerts per
 * severity. Read-only on d. */
void data_summary(const Dataset *d, Summary *out)
{
    memset(out, 0, sizeof *out);
    float lat_sum = 0.0f, loss_sum = 0.0f;
    int live = 0;

    for (int i = 0; i < DEVICE_COUNT; i++) {
        const Device *dev = &d->devices[i];
        switch (dev->status) {
        case STATUS_UP:      out->up++; break;
        case STATUS_WARNING: out->warning++; break;
        case STATUS_DOWN:    out->down++; break;
        }
        out->in_mbps += dev->rx_mbps;
        out->out_mbps += dev->tx_mbps;
        if (dev->status != STATUS_DOWN) {
            lat_sum += dev->latency_ms;
            loss_sum += dev->loss_pct;
            live++;
        }
    }
    /* Links are full duplex: each direction has its own capacity, so the
     * busier direction defines utilisation. */
    out->utilization = fmaxf(out->in_mbps, out->out_mbps) / LINK_CAPACITY_MBPS;
    out->avg_latency_ms = live ? lat_sum / (float)live : 0.0f;
    out->avg_loss_pct = live ? loss_sum / (float)live : 0.0f;

    for (int i = 0; i < d->alert_count; i++) {
        const Alert *a = data_alert(d, i);
        if (!a->acknowledged)
            out->open_alerts[a->severity]++;
    }
}

/* Writes the indices of the (up to) max busiest devices into out, ordered
 * by rx+tx descending, and returns how many were written. Ties keep the
 * lower device index first. O(DEVICE_COUNT * max) insertion into a
 * bounded sorted list, no allocation. */
int data_top_talkers(const Dataset *d, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < DEVICE_COUNT; i++) {
        float rate = d->devices[i].rx_mbps + d->devices[i].tx_mbps;
        /* Insertion into a descending list capped at max entries: scan up
         * from the end to find the position. */
        int pos = n < max ? n : max;
        while (pos > 0) {
            const Device *prev = &d->devices[out[pos - 1]];
            if (prev->rx_mbps + prev->tx_mbps >= rate)
                break;
            pos--;
        }
        /* Slower than every kept entry and the list is full: drop it. */
        if (pos >= max)
            continue;
        /* Shift the tail down one place; the last element falls off when full. */
        int last = n < max ? n : max - 1;
        for (int j = last; j > pos; j--)
            out[j] = out[j - 1];
        out[pos] = i;
        if (n < max)
            n++;
    }
    return n;
}
