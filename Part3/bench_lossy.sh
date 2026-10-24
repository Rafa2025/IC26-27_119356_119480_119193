#!/usr/bin/env bash
#
# bench_lossy.sh - rate-distortion benchmark of the lossy codec.
#
#   ./bench_lossy.sh [options] MODE input.wav|directory ...
#
# MODES
#   compare   our codec (dct/mdct x lr/ms, several SNR targets) against
#             MP3 (libmp3lame) and AAC (ffmpeg's native encoder) at several bitrates
#   exp1      dct vs mdct                         (--transform)
#   exp2      block size N = 256 .. 8192          (-b)
#   exp3      lr vs ms                            (--stereo)
#   exp4      contexts: none, band, neighbours, both   (CTX_MODE, build variants)
#   exp5      adaptation shift 4, 5, 6            (PROB_SHIFT, build variants)
#   exp6      quantizer rounding offset 0.5 .. 0.3 (--round)
#   exp7      FFT vs direct transform speed       (test_transform --speed)
#   all       exp1 .. exp7
#
# OPTIONS
#   -o DIR    output directory (default: bench_out)
#   -t LIST   SNR targets of our encoder, e.g. "20 30 40"
#             (default compare: "10 15 ... 70" in steps of 5, experiments: "15 20 25 30 35 40 45")
#   -r LIST   MP3/AAC bitrates in kbps (default "32 48 64 96 128 160 192 256 320")
#   -j N      files processed in parallel (default 1). More than 1 is fine for
#             the bitrate and SNR columns, but the times and memory are then
#             measured on a busy machine: keep 1 for the time/memory figures.
#
# OUTPUT, per mode, in DIR:
#   MODE.csv   one row per run:
#              file,codec,config,param,bytes,ratio,kbps,snr_db,lag,enc_s,enc_kb,dec_s,dec_kb
#              (ratio = original WAV size / compressed size)
#              (param = SNR target for ours, bitrate for mp3/aac)
#   MODE.md    kbps and compression ratio at equal SNR (and SNR at equal kbps), averaged over the files
#   MODE.svg   average rate-distortion curves
#
# Times are wall-clock seconds and memory is the peak resident set size,
# both from GNU time (/usr/bin/time). The SNR is measured by snr_tool.py on
# the decoded WAV; for MP3/AAC it first removes the decoder delay (0..4096 frames).

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SNR_TOOL="python3 $HERE/snr_tool.py"
GTIME=/usr/bin/time

OUT=bench_out
TARGETS=""
RATES="32 48 64 96 128 160 192 256 320"
JOBS=1

usage() { sed -n '3,40p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

while getopts "o:t:r:j:h" opt; do
    case $opt in
        o) OUT=$OPTARG ;;
        t) TARGETS=$OPTARG ;;
        r) RATES=$OPTARG ;;
        j) JOBS=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 2 ] || usage
MODE=$1
shift

[ -x "$GTIME" ] || { echo "GNU time not found at $GTIME" >&2; exit 1; }

# Input files: arguments, or every .wav in a directory argument.
FILES=()
for a in "$@"; do
    if [ -d "$a" ]; then
        while IFS= read -r -d '' f; do FILES+=("$f"); done \
            < <(find "$a" -maxdepth 1 -name '*.wav' -print0 | sort -z)
    else
        FILES+=("$a")
    fi
done
[ ${#FILES[@]} -gt 0 ] || { echo "no input files" >&2; exit 1; }

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ------------------------------------------------------------------ helpers

# timed STATS_FILE cmd...  -> runs cmd, writes "<seconds> <peak KiB>" to STATS_FILE
timed() {
    local stats=$1; shift
    "$GTIME" -f "%e %M" -o "$stats" "$@"
}

# One CSV row. Arguments: file codec config param bytes snr lag enc_stats dec_stats frames rate
row() {
    local file=$1 codec=$2 config=$3 param=$4 bytes=$5 snr=$6 lag=$7 es=$8 ds=$9 frames=${10} rate=${11}
    local kbps ratio
    ratio=$(awk -v o="$(stat -c %s "$file")" -v b="$bytes" 'BEGIN { printf "%.3f", o / b }')
    kbps=$(awk -v b="$bytes" -v f="$frames" -v r="$rate" \
           'BEGIN { if (f > 0) printf "%.3f", b * 8 / (f / r) / 1000; else print 0 }')
    echo "$(basename "$file"),$codec,$config,$param,$bytes,$ratio,$kbps,$snr,$lag,$(tr ' ' ',' < "$es"),$(tr ' ' ',' < "$ds")"
}

# ours FILE CONFIG ENC DEC [encoder options...]: one row per SNR target.
ours() {
    local file=$1 config=$2 enc=$3 dec=$4; shift 4
    local w; w=$(mktemp -d "$WORK/ours.XXXX")
    local info; info=$($SNR_TOOL info "$file")
    local frames=${info%% *} rate; rate=$(echo "$info" | cut -d' ' -f2)
    for t in $TARGETS; do
        timed "$w/es" "$enc" "$@" --snr "$t" "$file" "$w/c.icd" 2>/dev/null
        timed "$w/ds" "$dec" "$w/c.icd" "$w/d.wav"
        read -r snr lag < <($SNR_TOOL snr "$file" "$w/d.wav")
        row "$file" ours "$config" "$t" "$(stat -c %s "$w/c.icd")" "$snr" "$lag" \
            "$w/es" "$w/ds" "$frames" "$rate"
    done
    rm -rf "$w"
}

# ff FILE CODEC: MP3 or AAC through ffmpeg, one row per bitrate.
ff() {
    local file=$1 codec=$2
    local w; w=$(mktemp -d "$WORK/ff.XXXX")
    local info; info=$($SNR_TOOL info "$file")
    read -r frames rate nc bits <<< "$info"
    local lib ext pcm
    case $codec in
        mp3) lib=libmp3lame; ext=mp3 ;;
        aac) lib=aac; ext=m4a ;;
    esac
    case $bits in 8) pcm=pcm_u8 ;; 16) pcm=pcm_s16le ;; *) pcm=pcm_s24le ;; esac
    local ffq=(ffmpeg -hide_banner -loglevel error -nostdin -y -threads 1)
    for r in $RATES; do
        timed "$w/es" "${ffq[@]}" -i "$file" -c:a "$lib" -b:a "${r}k" "$w/c.$ext"
        timed "$w/ds" "${ffq[@]}" -i "$w/c.$ext" -c:a "$pcm" -ar "$rate" -ac "$nc" "$w/d.wav"
        read -r snr lag < <($SNR_TOOL snr "$file" "$w/d.wav" --max-lag 4096)
        row "$file" "$codec" "$lib" "$r" "$(stat -c %s "$w/c.$ext")" "$snr" "$lag" \
            "$w/es" "$w/ds" "$frames" "$rate"
    done
    rm -rf "$w"
}

# for_files FUNCTION: run FUNCTION FILE for every file, JOBS at a time,
# collecting each file's rows in its own fragment, then in file order.
for_files() {
    local fn=$1 i=0
    for f in "${FILES[@]}"; do
        while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
        ( "$fn" "$f" > "$WORK/part.$(printf %04d $i)" ) &
        i=$((i + 1))
    done
    wait
    cat "$WORK"/part.* 2>/dev/null
    rm -f "$WORK"/part.*
}

ENC="$HERE/wav_lossy_enc"
DEC="$HERE/wav_lossy_dec"
variant() { echo "$HERE/build/ctx$1_shift$2"; }

# ------------------------------------------------------------------ modes

do_compare() {
    local f=$1
    for tr in dct mdct; do
        for st in lr ms; do
            ours "$f" "$tr-$st" "$ENC" "$DEC" --transform "$tr" --stereo "$st"
        done
    done
    ff "$f" mp3
    ff "$f" aac
}
do_exp1() { for tr in dct mdct; do ours "$1" "$tr" "$ENC" "$DEC" --transform "$tr"; done; }
do_exp2() { for n in 256 512 1024 2048 4096 8192; do ours "$1" "N$n" "$ENC" "$DEC" -b "$n"; done; }
do_exp3() { for st in lr ms; do ours "$1" "$st" "$ENC" "$DEC" --stereo "$st"; done; }
do_exp4() {
    local names=(none band neighbours both) m
    for m in 0 1 2 3; do
        local v; v=$(variant $m 5)
        ours "$1" "ctx$m-${names[$m]}" "$v/wav_lossy_enc" "$v/wav_lossy_dec"
    done
}
do_exp5() {
    local s
    for s in 4 5 6; do
        local v; v=$(variant 2 $s)
        ours "$1" "shift$s" "$v/wav_lossy_enc" "$v/wav_lossy_dec"
    done
}
do_exp6() { for r in 0.5 0.45 0.4 0.35 0.3; do ours "$1" "round$r" "$ENC" "$DEC" --round "$r"; done; }

# run_mode NAME TITLE TABLE_OPTIONS...
run_mode() {
    local name=$1 title=$2; shift 2
    echo "== $name: $title (${#FILES[@]} files, targets: $TARGETS)" >&2
    {
        echo "file,codec,config,param,bytes,ratio,kbps,snr_db,lag,enc_s,enc_kb,dec_s,dec_kb"
        for_files "do_$name"
    } > "$OUT/$name.csv"
    {
        echo "# $title"
        echo
        $SNR_TOOL table "$OUT/$name.csv" "$@"
    } > "$OUT/$name.md"
    $SNR_TOOL plot "$OUT/$name.csv" "$OUT/$name.svg" --title "$title" || true
    cat "$OUT/$name.md" >&2
    echo "-> $OUT/$name.csv, $name.md, $name.svg" >&2
}

make -s -C "$HERE" all >&2
case $MODE in
    exp4|exp5|all) make -s -C "$HERE" variants >/dev/null ;;
esac

if [ "$MODE" = compare ]; then
    command -v ffmpeg > /dev/null || { echo "ffmpeg not found" >&2; exit 1; }
    : "${TARGETS:=10 15 20 25 30 35 40 45 50 55 60 65 70}"
    run_mode compare "Our codec vs MP3 and AAC" --at-snr 15,20,25,30 --at-kbps 64,128,192,256
    exit 0
fi

: "${TARGETS:=15 20 25 30 35 40 45}"
EXP_TABLE=(--at-snr 20,30,40)
case $MODE in
    exp1) run_mode exp1 "Exp 1: dct vs mdct" "${EXP_TABLE[@]}" ;;
    exp2) run_mode exp2 "Exp 2: block size N (mdct)" "${EXP_TABLE[@]}" ;;
    exp3) run_mode exp3 "Exp 3: lr vs ms" "${EXP_TABLE[@]}" ;;
    exp4) run_mode exp4 "Exp 4: context design" "${EXP_TABLE[@]}" ;;
    exp5) run_mode exp5 "Exp 5: adaptation shift" "${EXP_TABLE[@]}" ;;
    exp6) run_mode exp6 "Exp 6: quantizer rounding offset" "${EXP_TABLE[@]}" ;;
    exp7)
        make -s -C "$HERE" test_transform >&2
        "$HERE/test_transform" --speed | tee "$OUT/exp7.txt" >&2 ;;
    all)
        for e in exp1 exp2 exp3 exp4 exp5 exp6 exp7; do
            "$0" -o "$OUT" -t "$TARGETS" -j "$JOBS" "$e" "${FILES[@]}"
        done ;;
    *) usage ;;
esac
