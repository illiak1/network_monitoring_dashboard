/* data.h - simulated network: devices, traffic history, alert log.
 *
 * Raylib-free on purpose, so tests/test_data.c can run it headless.
 * Parts: Ring (fixed-size sample history), Device (generator state plus
 * measured values), Alert (circular log), Dataset (everything, advanced in
 * fixed ticks by data_update) and read-only queries (summary, top talkers).
 * data.c implements it; dashboard.h embeds one Dataset and the panels and
 * pages read it through the accessors below.
 */
#ifndef DATA_H
#define DATA_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define DEVICE_COUNT 14            /* fleet size, matches DEVICE_SPECS in data.c */
#define RING_CAP 240               /* samples per history: 240 x 0.5 s = 120 s (longest chart window) */
#define ALERT_CAP 200              /* alert log entries; the oldest is overwritten when full */
#define SIM_TICK_S 0.5f            /* simulation step in s (2 Hz), independent of frame rate */
#define LINK_CAPACITY_MBPS 1000.0f /* uplink capacity per direction; basis of utilisation */

/* Device is flagged WARNING at or above these values. data.c lowers them to
 * 80% for leaving the WARNING state (hysteresis). */
#define DEVICE_LATENCY_WARN_MS 100.0f
#define DEVICE_LOSS_WARN_PCT 1.5f

/* Network-average gauge ranges and zone boundaries. Averages stay well below
 * single-device peaks, so the zones are tighter than the per-device limits. */
#define GAUGE_LATENCY_MAX_MS 100.0f
#define GAUGE_LATENCY_WARN_MS 40.0f
#define GAUGE_LATENCY_CRIT_MS 70.0f
#define GAUGE_LOSS_MAX_PCT 2.0f
#define GAUGE_LOSS_WARN_PCT 0.5f
#define GAUGE_LOSS_CRIT_PCT 1.0f

/* ---- Ring buffer ---- */

/* Fixed-capacity sample history. Index 0 is the oldest sample. */
typedef struct {
    float v[RING_CAP];
    int cap;     /* usable capacity, <= RING_CAP */
    int head;    /* next write position */
    int count;   /* valid samples, <= cap */
} Ring;

/* Empties the ring and sets its capacity (clamped to RING_CAP). */
void ring_init(Ring *r, int cap);
/* Appends a sample; overwrites the oldest one when full. */
void ring_push(Ring *r, float value);
/* Sample i, 0 = oldest, count - 1 = newest. i must be in [0, count). */
float ring_at(const Ring *r, int i);
/* Newest sample, 0 if the ring is empty. */
float ring_last(const Ring *r);
/* Copies samples oldest to newest into out (size >= r->count). Returns count. */
int ring_copy(const Ring *r, float *out);

/* ---- Devices ---- */

/* Order matters: the values are used as indices into label tables. */
typedef enum { STATUS_UP, STATUS_WARNING, STATUS_DOWN } DeviceStatus;
/* Indexes the name table in device_kind_name. */
typedef enum { KIND_ROUTER, KIND_FIREWALL, KIND_SWITCH, KIND_ACCESS_POINT, KIND_SERVER, KIND_STORAGE, KIND_GATEWAY } DeviceKind;
/* Injected incident: DEGRADED adds latency/loss, OUTAGE makes the host unreachable. */
typedef enum { FAULT_NONE, FAULT_DEGRADED, FAULT_OUTAGE } FaultState;

typedef struct {
    const char *name;
    const char *ip;
    DeviceKind kind;
    DeviceStatus status;
    float latency_ms;
    float loss_pct;
    float rx_mbps;         /* inbound rate */
    float tx_mbps;         /* outbound rate */
    float uptime_s;        /* reset to 0 when an outage starts */
    Ring latency_hist;     /* last 60 ticks = 30 s */

    /* Generator state. */
    float base_latency_ms; /* idle latency */
    float rx_weight;       /* inbound Mbps at load 1.0 */
    float tx_weight;       /* outbound Mbps at load 1.0 */
    float phase_lat;       /* phase of the 31 s latency sine, rad */
    float phase_load;      /* phase of the load sine, rad */
    float period_load;     /* period of the load sine, s */
    float burst_chance;    /* per-tick probability of a traffic burst */
    float lat_noise;       /* low-pass filtered latency noise, ms */
    float load_walk;        /* mean-reverting random walk added to the load */
    float burst;           /* current burst amplitude, decays every tick */
    FaultState fault;
    float fault_timer;     /* seconds until the fault ends */
    float fault_level;     /* 0..1 envelope, eased so effects ramp in and out */
    float fault_extra_lat; /* ms added at fault_level 1 */
    float fault_extra_loss; /* percentage points added at fault_level 1 */
} Device;

/* ---- Alerts ---- */

/* Order matters: used as an array index (Summary.open_alerts, name table). */
typedef enum { SEV_INFO, SEV_WARNING, SEV_CRITICAL, SEV_COUNT } AlertSeverity;

typedef struct {
    uint32_t serial;      /* monotonic id, stable identity for UI animation keys */
    uint32_t tick;        /* tick_count at creation, used to detect fresh alerts */
    AlertSeverity severity;
    time_t when;          /* wall-clock time, back-dated for warm-up alerts */
    int device;           /* index into Dataset.devices, -1 if not device-bound */
    bool acknowledged;
    char source[24];
    char message[80];
} Alert;

/* ---- Aggregates ---- */

/* Snapshot computed by data_summary. */
typedef struct {
    int up, warning, down;
    float in_mbps, out_mbps;   /* sum over all devices */
    float utilization;    /* 0..1 of link capacity, max of both directions */
    float avg_latency_ms; /* over devices that are not DOWN */
    float avg_loss_pct;   /* over devices that are not DOWN */
    int open_alerts[SEV_COUNT];  /* unacknowledged, per severity */
} Summary;

/* ---- Dataset ---- */

/* Complete simulation state. Advance with data_update only. */
typedef struct {
    uint32_t rng_state;   /* xorshift32 state, never 0 */
    uint32_t tick_count;
    float tick_acc;       /* seconds since the last tick, [0, SIM_TICK_S) */
    double sim_time;      /* tick_count * SIM_TICK_S, in s */
    float time_shift;     /* back-dates alerts raised during warm-up */

    Device devices[DEVICE_COUNT];
    Ring bw_in;           /* aggregate inbound Mbps per tick */
    Ring bw_out;          /* aggregate outbound Mbps per tick */
    Ring net_latency;     /* network-average latency per tick */
    Ring net_loss;        /* network-average loss per tick */

    Alert alerts[ALERT_CAP];  /* circular, newest at alert_head - 1 */
    int alert_head;           /* next write position */
    int alert_count;
    uint32_t alert_serial;    /* last serial handed out */
    bool util_alarm;          /* utilisation alert is currently raised (hysteresis) */
} Dataset;

/* ---- Simulation control ---- */

/* Seeds the generators and pre-fills the histories so charts start populated. */
void data_init(Dataset *d, uint32_t seed);
/* Advances the simulation by dt seconds (fixed internal tick of SIM_TICK_S). */
void data_update(Dataset *d, double dt);

/* ---- Queries ---- */

/* Fills out with counts, aggregate rates, averages and open alerts. */
void data_summary(const Dataset *d, Summary *out);
/* Fills out[] with device indices sorted by rx+tx descending. Returns count. */
int data_top_talkers(const Dataset *d, int *out, int max);

/* Number of alerts in the log (<= ALERT_CAP). */
int data_alert_count(const Dataset *d);
/* i = 0 is the newest alert. */
const Alert *data_alert(const Dataset *d, int i);
/* Flips the acknowledged flag of alert i (age index); out of range is ignored. */
void data_alert_toggle_ack(Dataset *d, int i);
/* Marks every alert in the log acknowledged. */
void data_alert_ack_all(Dataset *d);

/* Display names for the enums above. */
const char *device_kind_name(DeviceKind k);
const char *device_status_name(DeviceStatus s);
const char *alert_severity_name(AlertSeverity s);

#endif
