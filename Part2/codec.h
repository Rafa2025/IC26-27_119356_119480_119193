/*
 * codec.h - the lossless codec: ties all modules together.
 *
 * COMPRESSED FILE FORMAT (everything is written through the bit writer,
 * MSB first, no padding between fields; the file is padded with zero bits
 * only at the very end)
 * ---------------------------------------------------------------------
 *   magic        32 bits   "ICL1"
 *   mode          8 bits   0 = RAW (whole input stored as is)
 *                          1 = PCM (compressed audio)
 *
 *   mode 0:  length 64 bits, then `length` bytes.
 *
 *   mode 1:  channels      8 bits   (1 or 2)
 *            bits          8 bits   (8, 16 or 24)
 *            block_size   32 bits   samples per block, per channel
 *            nframes      64 bits   total frames (samples per channel)
 *            prefix_len   64 bits   bytes before the audio  } stored
 *            suffix_len   64 bits   bytes after the audio   } verbatim
 *            prefix bytes, suffix bytes
 *            then, for every block, for every channel:
 *                order     3 bits   predictor order (0..4)
 *                k         5 bits   Rice parameter  (0..30)
 *                n Rice-coded residuals (n = block_size, or fewer in the last block)
 *
 * For stereo, channel 0 carries MID and channel 1 carries SIDE.
 */
#ifndef CODEC_H
#define CODEC_H

#include <stdint.h>

#define CODEC_DEFAULT_BLOCK 4096
#define CODEC_MAX_BLOCK     (1u << 24)

/* Both return 0 on success, nonzero on error (message printed to stderr). */
int codec_encode(const char *in_path, const char *out_path,
                 uint32_t block_size, int verbose);
int codec_decode(const char *in_path, const char *out_path);

#endif /* CODEC_H */
