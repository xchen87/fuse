/*
 * camera_compress.c — FUSE demo WASM module
 *
 * Captures the latest camera frame via camera_last_frame(), applies a simple
 * Run-Length Encoding (RLE) compression, and logs compression stats.
 *
 * Compiled freestanding (no libc, no WASI):
 *   clang --target=wasm32-unknown-unknown -nostdlib -O2 \
 *         -Wl,--no-entry -Wl,--export=module_step -Wl,--export=module_init \
 *         -Wl,--allow-undefined -o camera_compress.wasm camera_compress.c
 *
 * Policy requirements:
 *   FUSE_CAP_CAMERA | FUSE_CAP_TIMER | FUSE_CAP_LOG
 *   memory_pages_max=62, heap_size=262144, cpu_quota_us=1000
 */

/* ---------------------------------------------------------------------------
 * Native-function imports (provided by FUSE host bridge, module "env")
 * --------------------------------------------------------------------------- */
__attribute__((import_module("env"), import_name("timer_get_timestamp")))
extern unsigned long long timer_get_timestamp(void);

__attribute__((import_module("env"), import_name("camera_last_frame")))
extern unsigned long long camera_last_frame(void *buf, unsigned int max_len);

__attribute__((import_module("env"), import_name("module_log_event")))
extern void module_log_event(const char *ptr, unsigned int len,
                             unsigned int level);

/* ---------------------------------------------------------------------------
 * Static buffers  (WASM linear memory is zero-initialised at startup)
 * --------------------------------------------------------------------------- */
#define MAX_FRAME_SIZE  (512u * 1024u)   /* 512 KB — input frame             */
#define MAX_COMP_SIZE   (1024u * 1024u)  /* 1 MB  — RLE output (worst case)  */
#define LOG_BUF_SIZE    128u

static unsigned char s_frame[MAX_FRAME_SIZE];
static unsigned char s_comp[MAX_COMP_SIZE];
static char          s_log[LOG_BUF_SIZE];

/* ---------------------------------------------------------------------------
 * RLE compression
 *
 * Encodes runs of identical bytes as (count:u8, value:u8) pairs.
 * A single non-repeated byte still costs 2 output bytes (count=1, value).
 * Returns compressed byte count, or 0 if dst_max would be exceeded.
 * --------------------------------------------------------------------------- */
static unsigned int rle_compress(const unsigned char *src, unsigned int src_len,
                                 unsigned char *dst, unsigned int dst_max)
{
    unsigned int out = 0;
    unsigned int i   = 0;

    while (i < src_len) {
        unsigned char val   = src[i];
        unsigned int  count = 1u;

        while (count < 255u && (i + count) < src_len &&
               src[i + count] == val) {
            ++count;
        }

        if (out + 2u > dst_max) {
            return 0u;  /* overflow — caller logs FATAL */
        }
        dst[out++] = (unsigned char)count;
        dst[out++] = val;
        i += count;
    }
    return out;
}

/* ---------------------------------------------------------------------------
 * Minimal string helpers (no libc)
 * --------------------------------------------------------------------------- */

/* Converts n to decimal string in buf.  Returns number of chars written
 * (not NUL-terminated).  buf must have at least 10 bytes available. */
static unsigned int u32_to_str(unsigned int n, char *buf, unsigned int bufmax)
{
    char   tmp[10];
    unsigned int len = 0u;

    if (n == 0u) {
        if (bufmax == 0u) return 0u;
        buf[0] = '0';
        return 1u;
    }
    while (n > 0u && len < 10u) {
        tmp[len++] = (char)('0' + (n % 10u));
        n /= 10u;
    }
    unsigned int i;
    for (i = 0u; i < len && i < bufmax; ++i) {
        buf[i] = tmp[len - 1u - i];
    }
    return i;
}

/* Appends the NUL-terminated string src into dst[pos..dst_max-1].
 * Returns the new position. dst is NOT NUL-terminated by this function. */
static unsigned int str_append(char *dst, unsigned int pos, unsigned int max,
                               const char *src)
{
    while (*src != '\0' && pos < max) {
        dst[pos++] = *src++;
    }
    return pos;
}

/* ---------------------------------------------------------------------------
 * Module lifecycle exports
 * --------------------------------------------------------------------------- */

__attribute__((export_name("module_init")))
void module_init(void)
{
    /* Nothing to initialise — static buffers are zero at startup. */
}

__attribute__((export_name("module_step")))
void module_step(void)
{
    /* --- 1. Timestamp before capture --- */
    unsigned long long t0 = timer_get_timestamp();

    /* --- 2. Capture latest camera frame --- */
    unsigned long long frame_bytes =
        camera_last_frame(s_frame, MAX_FRAME_SIZE);

    if (frame_bytes == 0u) {
        const char *msg = "cam: no frame";
        module_log_event(msg, 13u, 1u);  /* INFO */
        return;
    }
    if (frame_bytes > MAX_FRAME_SIZE) {
        frame_bytes = MAX_FRAME_SIZE;
    }

    /* --- 3. Compress --- */
    unsigned int comp_sz = rle_compress(s_frame, (unsigned int)frame_bytes,
                                        s_comp, MAX_COMP_SIZE);
    if (comp_sz == 0u) {
        const char *msg = "compress: overflow";
        module_log_event(msg, 18u, 2u);  /* FATAL */
        return;
    }

    /* --- 4. Timestamp after compression --- */
    unsigned long long t1  = timer_get_timestamp();
    unsigned long long us  = (t1 > t0) ? (t1 - t0) : 0u;

    /* Clamp elapsed us to uint32_t range before logging.
     * In normal operation us is bounded by cpu_quota_us (1000), but the
     * clamp prevents silent truncation if the quota is ever raised. */
    unsigned int us_u32 = (us > 0xFFFFFFFFULL) ? 0xFFFFFFFFU
                                                : (unsigned int)us;

    /* --- 5. Build log string without sprintf ---
     * Format: "comp orig=XXXXXX out=XXXXXX us=XXXXXX"
     */
    unsigned int pos = 0u;
    pos = str_append(s_log, pos, LOG_BUF_SIZE, "comp orig=");
    pos += u32_to_str((unsigned int)frame_bytes, s_log + pos,
                      LOG_BUF_SIZE - pos);
    pos = str_append(s_log, pos, LOG_BUF_SIZE, " out=");
    pos += u32_to_str(comp_sz, s_log + pos, LOG_BUF_SIZE - pos);
    pos = str_append(s_log, pos, LOG_BUF_SIZE, " us=");
    pos += u32_to_str(us_u32, s_log + pos, LOG_BUF_SIZE - pos);

    module_log_event(s_log, pos, 1u);  /* INFO */
}
