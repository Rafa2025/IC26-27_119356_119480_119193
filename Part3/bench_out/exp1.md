# Exp 1: dct vs mdct

| codec | config | files | kbps @ 20 dB | kbps @ 30 dB | kbps @ 40 dB | ratio @ 20 dB | ratio @ 30 dB | ratio @ 40 dB |
|---|---|---|---|---|---|---|---|---|
| ours | dct | 27 | 32.8 | 85.6 | 161.2 | 43.0 | 16.5 | 8.8 |
| ours | mdct | 27 | 29.5 | 79.3 | 152.1 | 47.8 | 17.8 | 9.3 |

Averages over the files; "(n)" = only n files cover that point, the others are outside their measured range; '-' = none does.
ratio = total original WAV size / total compressed size over those files.
