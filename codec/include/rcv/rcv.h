/* RCV1 - real-time lossless screen-capture codec. Public C API.
 * The bitstream and behaviour are defined by RCV1_Codec_Plan.md (§6, §7).
 * Additions beyond the plan's §7 are marked and recorded in docs/DECISIONS.md. */
#ifndef RCV_RCV_H
#define RCV_RCV_H

#include <stddef.h>
#include <stdint.h>

#if defined(RCV_SHARED_BUILD)
#define RCV_API __declspec(dllexport)
#elif defined(RCV_SHARED)
#define RCV_API __declspec(dllimport)
#else
#define RCV_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RCV_SEQUENCE_HEADER_SIZE 32
#define RCV_DUP_PACKET_SIZE 8

typedef enum {
    RCV_OK = 0,
    RCV_ERR_INVALID_ARG,
    RCV_ERR_UNSUPPORTED,
    RCV_ERR_BUFFER_TOO_SMALL,
    RCV_ERR_BITSTREAM,
    RCV_ERR_NO_REFERENCE,
    RCV_ERR_OUT_OF_MEMORY
} rcv_status;

typedef enum { RCV_FMT_YUV420 = 0, RCV_FMT_GBR = 1 } rcv_format;
typedef enum { RCV_IN_I420, RCV_IN_NV12, RCV_IN_BGRA, RCV_IN_BGRX } rcv_input_layout;
typedef enum { RCV_ISA_AUTO, RCV_ISA_SCALAR, RCV_ISA_SSE41, RCV_ISA_AVX2 } rcv_isa;
typedef enum { RCV_OUT_I420, RCV_OUT_NV12, RCV_OUT_BGRA } rcv_output_layout;

typedef struct rcv_encoder rcv_encoder;
typedef struct rcv_decoder rcv_decoder;

typedef struct {
    rcv_format        format;
    rcv_input_layout  input_layout;
    uint16_t          coded_width, coded_height;
    uint16_t          display_width, display_height; /* 0 = same as coded */
    uint8_t           colour_matrix;      /* 0 BT.601, 1 BT.709 */
    uint8_t           full_range;         /* 1 = full */
    uint8_t           num_slices;         /* 0 = auto */
    uint8_t           num_threads;        /* 0 = auto: 2 if >= 4 logical CPUs, else 1 */
    uint8_t           predictor;          /* 1 = MED (default), 0 = LEFT */
    uint8_t           enable_skip;        /* temporal skip + auto-DUP, default 1 */
    uint8_t           enable_crc;         /* default 0 */
    uint16_t          keyframe_interval;  /* default 120; 0 = default */
    uint32_t          fps_num, fps_den;
    rcv_isa           isa;                /* default AUTO */
    void            (*on_worker_start)(void* user, int worker_index); /* caller sets priority/affinity */
    void*             user;
} rcv_encoder_config;

typedef struct {
    const uint8_t* plane[3];   /* I420: Y,U,V; NV12: Y,UV,NULL; BGRA: pixels,NULL,NULL */
    int32_t        stride[3];
} rcv_frame_in;

typedef struct {
    uint8_t  force_keyframe;
    uint8_t  near_level;       /* 0..3, YUV420 only. Not "near": <windows.h> defines near as a macro. */
} rcv_encode_params;

typedef struct {
    uint32_t packet_size;
    uint8_t  frame_type;       /* 0 DUP, 1 I, 2 P */
    uint8_t  is_keyframe;
    uint32_t blocks_skipped, blocks_total;
    uint32_t time_skip_us, time_encode_us, time_total_us;
} rcv_frame_info;

/* --- Addition: parsed sequence header (for tools and the host). --- */
typedef struct {
    rcv_format format;
    uint16_t   coded_width, coded_height;
    uint16_t   display_width, display_height;
    uint8_t    colour_matrix, full_range, chroma_siting;
    uint16_t   keyframe_interval;
    uint32_t   fps_num, fps_den;
} rcv_sequence_info;

/* --- Addition: fills a config with the documented defaults (MED, skip on, keyint 120, ...). --- */
RCV_API void        rcv_encoder_config_init(rcv_encoder_config* cfg);
/* --- Addition: validates and parses a 32-byte sequence header. --- */
RCV_API rcv_status  rcv_parse_sequence_header(const uint8_t seq_header[32], rcv_sequence_info* out);
/* --- Addition: short English name for a status code. --- */
RCV_API const char* rcv_status_string(rcv_status status);
/* --- Addition: best kernel level this CPU + OS can run (never RCV_ISA_AUTO). A config asking for
 *     a higher level than this fails with RCV_ERR_UNSUPPORTED. --- */
RCV_API rcv_isa     rcv_cpu_isa(void);

/* Encoder */
RCV_API rcv_status rcv_encoder_create(const rcv_encoder_config* cfg, rcv_encoder** out);
RCV_API void       rcv_encoder_destroy(rcv_encoder* enc);
/* Worst-case packet size for this config (§6.6); 0 if the config is invalid. */
RCV_API size_t     rcv_max_packet_size(const rcv_encoder_config* cfg);
RCV_API void       rcv_write_sequence_header(const rcv_encoder_config* cfg, uint8_t out[32]);

/* out_capacity must be >= rcv_max_packet_size(cfg), otherwise RCV_ERR_BUFFER_TOO_SMALL. */
RCV_API rcv_status rcv_encode_frame(rcv_encoder* enc, const rcv_frame_in* in,
                                    const rcv_encode_params* params, /* may be NULL */
                                    uint8_t* out, size_t out_capacity,
                                    rcv_frame_info* info);           /* may be NULL */
RCV_API rcv_status rcv_encode_duplicate(rcv_encoder* enc, uint8_t* out, size_t out_capacity,
                                        rcv_frame_info* info);       /* always 8 bytes */

/* Decoder */
RCV_API rcv_status rcv_decoder_create(const uint8_t seq_header[32], uint8_t num_threads,
                                      rcv_decoder** out);
RCV_API void       rcv_decoder_destroy(rcv_decoder* dec);
/* plane == NULL or plane[0] == NULL: decode into the reference only, no output copy. */
RCV_API rcv_status rcv_decode_frame(rcv_decoder* dec, const uint8_t* packet, size_t size,
                                    rcv_output_layout layout, uint8_t* const plane[3],
                                    const int32_t stride[3], rcv_frame_info* info);
RCV_API void       rcv_decoder_reset(rcv_decoder* dec);   /* call after a seek; next packet must be I */

#ifdef __cplusplus
}
#endif

#endif /* RCV_RCV_H */
