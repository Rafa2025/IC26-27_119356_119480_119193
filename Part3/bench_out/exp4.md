# Exp 4: context design

| codec | config | files | kbps @ 20 dB | kbps @ 30 dB | kbps @ 40 dB | ratio @ 20 dB | ratio @ 30 dB | ratio @ 40 dB |
|---|---|---|---|---|---|---|---|---|
| ours | ctx0-none | 27 | 32.2 | 83.5 | 156.8 | 43.8 | 16.9 | 9.0 |
| ours | ctx1-band | 27 | 31.8 | 82.9 | 156.1 | 44.4 | 17.0 | 9.0 |
| ours | ctx2-neighbours | 27 | 29.5 | 79.3 | 152.1 | 47.8 | 17.8 | 9.3 |
| ours | ctx3-both | 27 | 29.6 | 79.4 | 152.0 | 47.7 | 17.8 | 9.3 |

Averages over the files; "(n)" = only n files cover that point, the others are outside their measured range; '-' = none does.
ratio = total original WAV size / total compressed size over those files.
