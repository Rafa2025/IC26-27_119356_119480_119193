# Exp 2: block size N (mdct)

| codec | config | files | kbps @ 20 dB | kbps @ 30 dB | kbps @ 40 dB | ratio @ 20 dB | ratio @ 30 dB | ratio @ 40 dB |
|---|---|---|---|---|---|---|---|---|
| ours | N1024 | 27 | 30.0 | 79.1 | 151.2 | 47.0 | 17.8 | 9.3 |
| ours | N2048 | 27 | 29.5 | 79.3 | 152.1 | 47.8 | 17.8 | 9.3 |
| ours | N256 | 27 | 35.1 | 86.9 | 161.9 | 40.3 | 16.2 | 8.7 |
| ours | N4096 | 27 | 31.2 | 83.8 | 158.6 | 45.2 | 16.8 | 8.9 |
| ours | N512 | 27 | 32.1 | 82.3 | 155.5 | 43.9 | 17.2 | 9.1 |
| ours | N8192 | 27 | 34.5 | 90.8 | 168.7 | 41.0 | 15.5 | 8.4 |

Averages over the files; "(n)" = only n files cover that point, the others are outside their measured range; '-' = none does.
ratio = total original WAV size / total compressed size over those files.
