# Our codec vs MP3 and AAC

| codec | config | files | kbps @ 15 dB | kbps @ 20 dB | kbps @ 25 dB | dB @ 64 kbps | dB @ 128 kbps | dB @ 192 kbps | dB @ 256 kbps |
|---|---|---|---|---|---|---|---|---|---|
| aac | aac | 7 | 104.2 | 153.4 | 223.5 | 10.95 | 18.54 | 23.97 | 28.85 |
| mp3 | libmp3lame | 7 | 60.2 | 124.3 | 198.2 | 16.25 | 19.82 | 25.19 | 32.14 |
| ours | dct-lr | 7 | 30.5 | 54.5 | 86.0 | 23.86 | 32.71 | 38.66 | 43.63 |
| ours | dct-ms | 7 | 28.6 | 52.5 | 84.3 | 24.05 | 32.79 | 38.79 | 43.77 |
| ours | mdct-lr | 7 | 27.5 | 50.4 | 81.0 | 24.82 | 33.55 | 39.28 | 44.14 |
| ours | mdct-ms | 7 | 25.4 | 48.2 | 79.1 | 24.97 | 33.63 | 39.41 | 44.31 |

Averages over all files; '-' = outside the measured range of at least one file.
