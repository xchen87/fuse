/*
 * frame_compress.c — FUSE demo WASM module
 *
 * Wakes on DATA_CAPTURED event (event 1), posted by frame_filter when a frame
 * passes the variance threshold.
 * Re-reads the latest camera frame (same HAL buffer) and applies Run-Length
 * Encoding (RLE) compression.  Logs compression stats.
 *
 * Capabilities: FUSE_CAP_CAMERA | FUSE_CAP_LOG
 *
 * Compiled freestanding (no libc, no WASI):
 *   clang --target=wasm32-unknown-unknown -nostdlib -O2 \
 *         -Wl,--no-entry -Wl,--export=module_step \
 *         -Wl,--allow-undefined -o frame_compress.wasm frame_compress.c
 */

__attribute__((import_module("env"), import_name("camera_last_frame")))
extern unsigned long long camera_last_frame(void *buf, unsigned int max_len);

__attribute__((import_module("env"), import_name("module_log_event")))
extern void module_log_event(const char *ptr, unsigned int len, unsigned int level);

#define MAX_FRAME_SIZE  (256u * 1024u)
#define MAX_COMP_SIZE   (512u * 1024u)   /* worst-case RLE: 2 bytes per input byte */
#define LOG_BUF_SIZE    128u

static unsigned char s_frame[MAX_FRAME_SIZE];
static unsigned char s_comp[MAX_COMP_SIZE];
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

static unsigned int rle_compress(const unsigned char *src, unsigned int src_len,
                                 unsigned char *dst,       unsigned int dst_max)
{
    unsigned int out = 0u, i = 0u;
    while (i < src_len) {
        unsigned char val   = src[i];
        unsigned int  count = 1u;
        while (count < 255u && (i + count) < src_len && src[i + count] == val)
            ++count;
        if (out + 2u > dst_max) return 0u;
        dst[out++] = (unsigned char)count;
        dst[out++] = val;
        i += count;
    }
    return out;
}

__attribute__((export_name("module_step")))
void module_step(void)
{
    unsigned long long frame_bytes = camera_last_frame(s_frame, MAX_FRAME_SIZE);
    if (frame_bytes == 0u) {
        module_log_event("compress: no frame", 18u, 2u);
        return;
    }
    unsigned int len = (frame_bytes > MAX_FRAME_SIZE) ? MAX_FRAME_SIZE
                                                      : (unsigned int)frame_bytes;
    unsigned int comp_sz = rle_compress(s_frame, len, s_comp, MAX_COMP_SIZE);
    if (comp_sz == 0u) {
        module_log_event("compress: overflow", 18u, 2u);
        return;
    }
    unsigned int pos = 0u;
    pos = str_append(s_log, pos, LOG_BUF_SIZE, "compress: orig=");
    pos += u32_to_str(len,     s_log + pos, LOG_BUF_SIZE - pos);
    pos = str_append(s_log, pos, LOG_BUF_SIZE, " out=");
    pos += u32_to_str(comp_sz, s_log + pos, LOG_BUF_SIZE - pos);
    module_log_event(s_log, pos, 1u);
}
