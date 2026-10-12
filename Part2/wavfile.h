/*
 * wavfile.h - WAV container handling for the lossless codec.
 *
 * KEY IDEA (why the output is byte-identical to the input)
 * --------------------------------------------------------
 * We never try to "understand and regenerate" the WAV header. Instead the
 * file is cut into three byte ranges:
 *
 *      [ prefix ............ ][ audio frames ......... ][ suffix ......... ]
 *      everything before the   whole frames only,         everything after:
 *      first audio byte:       these are the ONLY bytes   odd pad byte, a
 *      RIFF header, fmt chunk, we compress                partial last frame,
 *      LIST chunks, "data"                                trailing chunks...
 *      chunk header...
 *
 * prefix and suffix are stored verbatim in the compressed file; the decoder
 * writes prefix + decoded audio + suffix. Whatever odd things a WAV file
 * contains (extra chunks, wrong sizes, padding), they come back unchanged.
 *
 * Supported audio: PCM, 8/16/24 bits, 1 or 2 channels.
 * Anything else makes wav_parse() return 0 and the codec falls back to
 * storing the whole file raw (still lossless, just not compressed).
 */
#ifndef WAVFILE_H
#define WAVFILE_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    int      channels;   /* 1 or 2 */
    int      bits;       /* 8, 16 or 24 bits per sample */
    uint32_t rate;       /* sample rate in Hz (informational only) */
    uint64_t data_off;   /* byte offset of the first audio byte in the file */
    uint64_t nframes;    /* number of WHOLE frames (1 frame = 1 sample/channel) */
} WavInfo;

/*
 * Walk the RIFF chunks of an in-memory WAV file and fill *info.
 * Returns 1 if the file is a WAV the codec can compress, 0 otherwise.
 */
int wav_parse(const uint8_t *file, uint64_t len, WavInfo *info);

/*
 * Copy the interleaved samples (L R L R ...) into one int32 array per
 * channel. chan[c] must have room for info->nframes values.
 * 8-bit WAV data is unsigned in the file, so 128 is subtracted to get
 * signed values centred on 0.
 */
void wav_deinterleave(const uint8_t *file, const WavInfo *info, int32_t **chan);

/*
 * Write a complete WAV file: prefix bytes, then the samples of all
 * channels re-interleaved and packed to `bits` bits, then suffix bytes.
 * Returns 0 on success, nonzero on a write error.
 */
int wav_rebuild(FILE *out,
                const uint8_t *prefix, uint64_t prefix_len,
                int32_t *const *chan, int channels, int bits, uint64_t nframes,
                const uint8_t *suffix, uint64_t suffix_len);

#endif /* WAVFILE_H */
