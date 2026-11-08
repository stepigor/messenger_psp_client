#ifndef ST_VOICE_CODEC_H
#define ST_VOICE_CODEC_H
#include <stddef.h>
#include <stdint.h>
#define ST_VOICE_RATE 16000
#define ST_VOICE_FRAME 320
#define ST_VOICE_BITRATE 16000
#define ST_VOICE_COMPLEXITY 3
/* Explicit 32-bit input/output also exposes PSP length overflow in host tests. */
uint32_t st_voice_output_frames(uint32_t samples,unsigned rate);
typedef struct {
    uint64_t encode_us,decode_us,encode_cpu_us,decode_cpu_us;
    unsigned frames,bytes,max_encode_us,max_decode_us,max_frame_us,late_frames,cpu_frames;
    int lookahead;
} StVoiceTiming;
typedef struct {
    void *context;
    uint64_t (*clock)(void *context,int cpu);
    int (*progress)(void *context,unsigned frame,unsigned frames);
} StVoiceHooks;
typedef struct {int16_t input[4096];size_t count;uint32_t position;int initialized;} StVoiceResampler;
/* Continuous 44.1 kHz -> 16 kHz low-pass conversion, preserving fractional
 * phase and FIR history across firmware blocks. No allocation or file IO. */
size_t st_voice_stream(StVoiceResampler *state,const int16_t *input,size_t count,int16_t *output,size_t capacity);
/* Whole-clip laboratory conversion, separate from streaming capture above.
 * A 31-tap low-pass precedes 44.1 -> 16 kHz conversion. */
size_t st_voice_downsample(const int16_t *in,size_t count,int16_t *out,size_t capacity);
int16_t st_voice_sample(const int16_t *in,size_t count,size_t index,unsigned in_rate,unsigned out_rate);
/* Output has exactly count samples. Encoder delay and its flushed tail are
 * accounted for. -1 means cancelled, 0 codec failure, 1 complete. */
int st_voice_roundtrip(const int16_t *in,size_t count,int16_t *out,StVoiceTiming *timing,const StVoiceHooks *hooks);
#endif
