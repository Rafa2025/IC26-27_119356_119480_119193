# Exp 6: quantizer rounding offset

| codec | config | files | kbps @ 20 dB | kbps @ 30 dB | kbps @ 40 dB | ratio @ 20 dB | ratio @ 30 dB | ratio @ 40 dB |
|---|---|---|---|---|---|---|---|---|
| ours | round0.3 | 27 | 29.9 | 81.5 | 156.7 | 47.1 | 17.3 | 9.0 |
| ours | round0.35 | 27 | 29.5 | 80.1 | 153.9 | 47.8 | 17.6 | 9.2 |
| ours | round0.4 | 27 | 29.5 | 79.3 | 152.1 | 47.8 | 17.8 | 9.3 |
| ours | round0.45 | 27 | 30.0 | 79.5 | 151.5 | 47.1 | 17.7 | 9.3 |
| ours | round0.5 | 27 | 31.2 | 81.0 | 152.8 | 45.3 | 17.4 | 9.2 |

Averages over the files; "(n)" = only n files cover that point, the others are outside their measured range; '-' = none does.
ratio = total original WAV size / total compressed size over those files.
