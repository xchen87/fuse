/*
 * frame_filter.c — FUSE demo WASM module
 *
 * Wakes on CAMERA_FRAME_READY event (event 0).
 * Reads the latest camera frame and computes its Mean Absolute Deviation (MAD)
 * as a content-quality metric.  Frames below the variance threshold are dropped
 * (uniform / blank / low-signal).  Frames above the threshold are forwarded to
 * the compression stage by posting DATA_CAPTURED (event 1).
 *
 * Capabilities: FUSE_CAP_CAMERA | FUSE_CAP_LOG | FUSE_CAP_EVENT_POST
 *
 * Compiled freestanding (no libc, no WASI):
 *   clang --target=wasm32-unknown-unknown -nostdlib -O2 \
 *         -Wl,--no-entry -Wl,--export=module_step \
 *         -Wl,--allow-undefined -o frame_filter.wasm frame_filter.c
 */

__attribute__((import_module("env"), import_name("camera_last_frame")))
extern unsigned long long camera_last_frame(void *buf, unsigned int max_len);

__attribute__((import_module("env"), import_name("module_log_event")))
extern void module_log_event(const char *ptr, unsigned int len, unsigned int level);

__attribute__((import_module("env"), import_name("fuse_event_post")))
extern void fuse_event_post(unsigned int event_id);

/* Event IDs — must match app_config.json events section */
#define EVENT_DATA_CAPTURED  1u

/*
 * Acceptance threshold for mean absolute deviation.
 * A frame whose MAD is below this value is considered featureless and dropped.
 */
#define MAD_THRESHOLD  8u

#define MAX_FRAME_SIZE  (256u * 1024u)
#define LOG_BUF_SIZE    128u

static unsigned char s_frame[MAX_FRAME_SIZE];
static char          s_log[LOG_BUF_SIZE];

static unsigned int u32_to_str(unsigned int n, char *buf, unsigned int bufmax)
{
    char         tmp[10];
    unsigned int len = 0u;
    if (n == 0u) { if (bufmax == 0u) return 0u; buf[0] = '0'; return 1u; }
    while (n > 0u && len < 10u) { tmp[len++] = (char)('0' + (n % 10u)); n /= 10u; }
    unsigned int i;
    for (i = 0u; i < len && i < bufmax; ++i) buf[i] = tmp[len - 1u - i];
    return i;
}

static unsigned int str_append(char *dst, unsigned int pos, unsigned int max,
                               const char *src)
{
    while (*src != '\0' && pos < max) dst[pos++] = *src++;
    return pos;
}

/*
 * compute_mad — mean absolute deviation of byte values.
 * Pass 1: compute mean. Pass 2: sum absolute deviations, return mean deviation.
 * Overflow-safe for len <= 262144: max sum = 255*262144 = 66,846,720 < 2^32.
 */
static unsigned int compute_mad(const unsigned char *buf, unsigned int len)
{
    unsigned int i, sum = 0u, dev = 0u, mean, v;
    if (len == 0u) return 0u;
    for (i = 0u; i < len; ++i) sum += (unsigned int)buf[i];
    mean = sum / len;
    for (i = 0u; i < len; ++i) {
        v = (unsigned int)buf[i];
        dev += (v > mean) ? (v - mean) : (mean - v);
    }
    return dev / len;
}

__attribute__((export_name("module_step")))
void module_step(void)
{
    unsigned long long frame_bytes = camera_last_frame(s_frame, MAX_FRAME_SIZE);
    if (frame_bytes == 0u) {
        module_log_event("filter: no frame", 16u, 1u);
        return;
    }
    unsigned int len = (frame_bytes > MAX_FRAME_SIZE) ? MAX_FRAME_SIZE
                                                      : (unsigned int)frame_bytes;
    unsigned int mad = compute_mad(s_frame, len);
    unsigned int pos = 0u;

    if (mad >= MAD_THRESHOLD) {
        pos = str_append(s_log, pos, LOG_BUF_SIZE, "filter: accept frame=");
        pos += u32_to_str(len,  s_log + pos, LOG_BUF_SIZE - pos);
        pos = str_append(s_log, pos, LOG_BUF_SIZE, " mad=");
        pos += u32_to_str(mad, s_log + pos, LOG_BUF_SIZE - pos);
        module_log_event(s_log, pos, 1u);
        fuse_event_post(EVENT_DATA_CAPTURED);
    } else {
        pos = str_append(s_log, pos, LOG_BUF_SIZE, "filter: reject frame=");
        pos += u32_to_str(len,  s_log + pos, LOG_BUF_SIZE - pos);
        pos = str_append(s_log, pos, LOG_BUF_SIZE, " mad=");
        pos += u32_to_str(mad, s_log + pos, LOG_BUF_SIZE - pos);
        module_log_event(s_log, pos, 1u);
    }
}
