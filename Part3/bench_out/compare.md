# Our codec vs MP3 and AAC

| codec | config | files | kbps @ 15 dB | kbps @ 20 dB | kbps @ 25 dB | kbps @ 30 dB | ratio @ 15 dB | ratio @ 20 dB | ratio @ 25 dB | ratio @ 30 dB | dB @ 64 kbps | dB @ 128 kbps | dB @ 192 kbps | dB @ 256 kbps |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| aac | aac | 27 | 76.8 | 115.4 | 169.9 | 190.5 (22) | 18.4 | 12.2 | 8.3 | 7.4 (22) | 15.79 | 24.65 | 30.64 | 29.57 (18) |
| mp3 | libmp3lame | 27 | 65.5 (17) | 109.9 (24) | 189.3 (26) | 231.0 (25) | 21.5 (17) | 12.8 (24) | 7.5 (26) | 6.1 (25) | 17.95 | 21.15 | 26.36 | 36.10 |
| ours | dct-lr | 27 | 17.7 | 35.5 | 60.0 | 91.1 | 79.6 | 39.8 | 23.5 | 15.5 | 30.19 | 37.83 (26) | 44.27 (26) | 49.64 (26) |
| ours | dct-ms | 27 | 16.0 | 32.8 | 56.1 | 85.6 | 88.4 | 43.0 | 25.2 | 16.5 | 31.65 | 39.20 (26) | 45.74 (26) | 51.10 (26) |
| ours | mdct-lr | 27 | 15.6 | 32.1 | 55.1 | 84.7 | 90.5 | 44.0 | 25.6 | 16.7 | 31.29 (26) | 39.74 (26) | 45.75 (26) | 48.97 (24) |
| ours | mdct-ms | 27 | 14.0 | 29.5 | 51.4 | 79.3 | 101.1 | 47.8 | 27.5 | 17.8 | 32.48 (26) | 41.10 (26) | 47.21 (26) | 50.52 (24) |

Averages over the files; "(n)" = only n files cover that point, the others are outside their measured range; '-' = none does.
ratio = total original WAV size / total compressed size over those files.
