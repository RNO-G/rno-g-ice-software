#!/usr/bin/env bash

################# THIS SCRIPT IS AI GENERATED #################

# -----------------------------------------------------------------------------
# show_runs.sh
#
# Takes a list of run directory paths (runXXXXXX), sorts them by run number and
# prints a configurable table of per-run information, pulled from
#
#   <run>/aux/runinfo.txt   "KEY = value"      -> referenced by KEY
#                                                 (RUN-START-TIME, STATION, ...)
#   <run>/cfg/acq.cfg       libconfig sections -> referenced by dotted path
#                                                 (calib.enable_cal,
#                                                  calib.sweep.start_atten, ...)
#
# Which columns are shown is fully configurable (-f / -p), runs can be filtered
# on any field (-w), and -L lists every field a run actually has.
#
# Examples
#   show_runs.sh /data/daq/run*                     # default columns
#   show_runs.sh -c /data/daq/run*                  # + channel/atten/sweep
#   show_runs.sh -p sweep /data/daq/run*            # full sweep settings
#   show_runs.sh -f run,start,enable_cal,atten /data/daq/run*
#   show_runs.sh -w calib.enable_cal=1 /data/daq/run*   # only pulser runs
#   show_runs.sh -L /data/daq/run001234             # what can I ask for?
# -----------------------------------------------------------------------------

set -uo pipefail

# =============================================================================
# Column presets — add your own here. Entries are comma-separated field specs,
# either "path" or "Label=path" (see FIELDS in --help).
# =============================================================================
declare -A PRESETS=(
    [default]="Run=runid,Duration,Events,Cal=calib.enable_cal,Phased,Coinc0,Coinc1"
    [times]="Run=runid,Start,End,Duration,Events"
    [cal]="Run=runid,Duration,Events,Cal=calib.enable_cal,calib.channel,calib.atten,sweep"
    [sweep]="Run=runid,Duration,Cal=calib.enable_cal,On=calib.sweep.enable,calib.sweep.start_atten,calib.sweep.stop_atten,calib.sweep.atten_step,calib.sweep.step_time"
    [info]="Run=runid,STATION,Start,End,Duration,Events,Hash=RNO-G-ICE-SOFTWARE-GIT-HASH"
)
PRESET_ORDER="default times cal sweep info"

MODE_FIELDS=""          # -f
PRESET="default"        # -p
FORMAT="table"          # -o
LIST_FIELDS=0           # -L
SHOW_CAL=0              # -c: append channel/atten/sweep columns
CAL_FIELDS="calib.channel,calib.atten,sweep"
declare -a WHERE=()     # -w

usage() {
    cat <<'USAGE'
Usage: show_runs.sh [options] <run_dir> [run_dir ...]
       find /data/daq -maxdepth 1 -name 'run*' | show_runs.sh [options]

Options:
  -c, --cal               append the calpulser detail columns (channel,
                          attenuation, sweep summary) to whatever -p/-f selected
  -p, --preset NAME       column preset (default: default), see PRESETS below
  -f, --fields SPEC       explicit comma-separated column list; overrides -p.
                          Repeat or comma-join to append more columns.
  -w, --where EXPR        only show runs matching EXPR; repeatable (all must
                          match).  EXPR is  FIELD=VALUE  (exact),
                          FIELD!=VALUE (differs) or FIELD~REGEX (ERE match).
  -o, --format FMT        table (default) | list | csv
  -L, --list-fields       print every available field of the first run and exit
  -h, --help              this help

FIELDS
  Pseudo fields : runid     numeric run number
                  run       run directory name (run000123)
                  start     RUN-START-TIME       end   RUN-END-TIME
                  acqstart  ACQ-START-TIME
                  duration  acquisition time, ACQ-START-TIME -> RUN-STOP-TIME
                  duration_wall  full run wall time, RUN-START-TIME -> RUN-END-TIME
                  nev       events written (also spelled "events")
                  phased    phased-array trigger summary ("off", "on,noRO", ...)
                  coinc0    first coincidence trigger  ("off", "3ch w5", ...)
                  coinc1    second coincidence trigger
                  sweep     compact calib.sweep.* summary ("off" or "31.5->0/0.5 @100s")
  runinfo keys  : STATION, RUN, RUN-START-TIME, RUN-END-TIME, DIDAQ-SAMPLERATE, ...
  config paths  : calib.enable_cal, calib.sweep.start_atten, radiant.trigger.rf0_enable, ...
  Pseudo field names are case-insensitive ("Duration" == "duration"), so a
  preset can capitalize them to set the column header.
  A trailing part of a config path is enough as long as it is unique, so
  "enable_cal", "sweep.start_atten" and "calib.sweep.start_atten" all work.
  Prefix a spec with "Label=" to set the column header, e.g. "Cal=calib.enable_cal".
USAGE
    echo
    echo "PRESETS"
    local p
    for p in $PRESET_ORDER; do printf '  %-9s %s\n' "$p" "${PRESETS[$p]}"; done
}

# ---------------------------------------------------------------------------
# format_time UNIX_SECONDS -> "2026-06-01 11:07:01 UTC"  (fractional part dropped)
# ---------------------------------------------------------------------------
format_time() {
    local secs="${1%.*}"
    date -u -d "@${secs}" "+%Y-%m-%d %H:%M:%S UTC" 2>/dev/null \
        || date -u -r "${secs}" "+%Y-%m-%d %H:%M:%S UTC"   # BSD/macOS fallback
}

# ---------------------------------------------------------------------------
# format_span SECONDS -> "2h 14m 03s"
# ---------------------------------------------------------------------------
format_span() {
    local s="$1" sign=""
    if (( s < 0 )); then sign="-"; s=$(( -s )); fi
    printf '%s%dh %02dm %02ds' "$sign" $(( s / 3600 )) $(( (s % 3600) / 60 )) $(( s % 60 ))
}

# ---------------------------------------------------------------------------
# flatten RUN_DIR
#   Emits "key<TAB>value" for everything the run knows about:
#   runinfo.txt keys verbatim, acq.cfg leaves as dotted paths.
# ---------------------------------------------------------------------------
flatten() {
    local run_dir="$1"

    awk -F'=' '/^[A-Za-z]/ && NF >= 2 {
        key = $1; sub(/[[:space:]]+$/, "", key)
        val = $0; sub(/^[^=]*=[[:space:]]*/, "", val)
        sub(/[[:space:]]+$/, "", val); gsub(/\r/, "", val)
        print key "\t" val
    }' "${run_dir}/aux/runinfo.txt" 2>/dev/null

    # libconfig dump: "name:" + "{" opens a section, "}" / "};" closes it,
    # "key=value;" is a leaf.  Comments are whole "//" lines in the dumped file.
    awk '
        { line = $0
          # drop a // comment, but not inside a quoted value ("/REV", "/dev/...")
          if (line !~ /"/) sub(/\/\/.*$/, "", line)
          gsub(/\r/, "", line)
          sub(/^[[:space:]]+/, "", line); sub(/[[:space:]]+$/, "", line)
          if (line == "") next

          if (line ~ /^[A-Za-z_][A-Za-z0-9_]*[[:space:]]*:[[:space:]]*\{?$/) {
              name = line; sub(/[[:space:]]*:.*$/, "", name)
              stack[++depth] = name
              pending = (line ~ /\{$/) ? 0 : 1     # "{" may sit on the next line
              next
          }
          if (line ~ /^\{/) { pending = 0; sub(/^\{[[:space:]]*/, "", line); if (line == "") next }
          if (line ~ /^\}/) { if (depth > 0) depth--; next }

          if (line ~ /^[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=/) {
              key = line; sub(/[[:space:]]*=.*$/, "", key)
              val = line; sub(/^[^=]*=[[:space:]]*/, "", val)
              sub(/;[[:space:]]*$/, "", val)
              sub(/[[:space:]]+$/, "", val)
              gsub(/^"|"$/, "", val)               # unquote strings
              path = ""
              for (i = 1; i <= depth; i++) path = path stack[i] "."
              print path key "\t" val
          }
        }' "${run_dir}/cfg/acq.cfg" 2>/dev/null
}

# ---------------------------------------------------------------------------
# load_run RUN_DIR -> fills the global V[] map (empty if the run has no files)
#   Besides "path -> value" it stores "@suffix -> path" for every trailing part
#   of a dotted path, so short field names resolve in O(1). "@AMBIG" marks a
#   suffix that more than one path ends with.
# ---------------------------------------------------------------------------
load_run() {
    local run_dir="$1" k v leaf sk rest
    V=()
    while IFS=$'\t' read -r k v; do
        [[ -z "$k" ]] && continue
        V["$k"]="$v"
        rest="$k"
        while [[ "$rest" == *.* ]]; do
            rest="${rest#*.}"
            sk="@${rest}"
            if [[ -n "${V[$sk]+x}" ]]; then
                [[ "${V[$sk]}" != "$k" ]] && V["$sk"]="@AMBIG"
            else
                V["$sk"]="$k"
            fi
        done
    done < <(flatten "$run_dir")
    [[ ${#V[@]} -gt 0 ]]
}

# ---------------------------------------------------------------------------
# raw_value FIELD -> value from V[], "" when unknown
# ---------------------------------------------------------------------------
raw_value() {
    local f="$1"
    if [[ -n "${V[$f]+x}" ]]; then printf '%s' "${V[$f]}"; return; fi
    local target="${V[@$f]:-}"
    if [[ -z "$target" ]]; then return; fi
    if [[ "$target" == "@AMBIG" ]]; then printf '<ambiguous>'; return; fi
    printf '%s' "${V[$target]:-}"
}

# ---------------------------------------------------------------------------
# Trigger summaries. The paths depend on the hardware generation: a DIDAQ
# station dumps didaq.trigger.{phased,coinc0,coinc1}, an older station the
# RADIANT RF triggers plus the FLOWER (lt) phased trigger. Both are tried, so
# one column works for either. Empty output (-> "-") means neither was found.
#   phased : "off" | "on[,noRO][,cons][,/2][,xb0x3]"
#   coincN : "off" | "3ch w5[,noRO][,x0x3]"        (window in 8-ns cycles)
#            "off" | "2ch w50ns[ m0xff]"           (RADIANT, window in ns)
# noRO = trigger is computed but does not trigger a readout.
# ---------------------------------------------------------------------------
phased_summary() {
    local en ro cons div mask
    en=$(raw_value "didaq.trigger.phased.enable")
    if [[ -n "$en" ]]; then
        [[ "$en" != 1 ]] && { printf 'off'; return; }
        ro=$(raw_value "didaq.trigger.phased.enable_readout")
        cons=$(raw_value "didaq.trigger.phased.require_consecutive")
        div=$(raw_value "didaq.trigger.phased.divide_by_2")
        mask=$(raw_value "didaq.trigger.phased.beam_exclude_mask")
        printf 'on'
        [[ "$ro"   == 0 ]] && printf ',noRO'
        [[ "$cons" == 1 ]] && printf ',cons'
        [[ "$div"  == 1 ]] && printf ',/2'
        [[ -n "$mask" && "$mask" != 0 && "$mask" != 0x0 ]] && printf ',xb%s' "$mask"
        return
    fi

    en=$(raw_value "lt.trigger.phased.enable_rf_phased_trigger")
    [[ -z "$en" ]] && return
    [[ "$en" != 1 ]] && { printf 'off'; return; }
    printf 'on'
    mask=$(raw_value "lt.trigger.phased.rf_phased_beam_mask")
    [[ -n "$mask" && "$mask" != 4095 && "$mask" != 0xfff ]] && printf ',beams%s' "$mask"
}

coinc_summary() {   # $1 = coincidence trigger index, 0 or 1
    local i="$1" en n w ro mask
    en=$(raw_value "didaq.trigger.coinc${i}.enable")
    if [[ -n "$en" ]]; then
        [[ "$en" != 1 ]] && { printf 'off'; return; }
        n=$(raw_value "didaq.trigger.coinc${i}.num_required")
        w=$(raw_value "didaq.trigger.coinc${i}.window")
        ro=$(raw_value "didaq.trigger.coinc${i}.enable_readout")
        mask=$(raw_value "didaq.trigger.coinc${i}.exclude_mask")
        printf '%sch w%s' "$n" "$w"
        [[ "$ro" == 0 ]] && printf ',noRO'
        [[ -n "$mask" && "$mask" != 0 && "$mask" != 0x0 ]] && printf ',x%s' "$mask"
        return
    fi

    en=$(raw_value "radiant.trigger.RF${i}.enabled")
    [[ -z "$en" ]] && return
    [[ "$en" != 1 ]] && { printf 'off'; return; }
    n=$(raw_value "radiant.trigger.RF${i}.num_coincidences")
    w=$(raw_value "radiant.trigger.RF${i}.window")
    mask=$(raw_value "radiant.trigger.RF${i}.mask")
    printf '%sch w%sns' "$n" "$w"
    [[ -n "$mask" && "$mask" != 0xffffff ]] && printf ' m%s' "$mask"
}

# ---------------------------------------------------------------------------
# field_value FIELD RUN_NAME -> raw value, pseudo fields included
# ---------------------------------------------------------------------------
field_value() {
    local f="$1" run_name="$2" s e v pf
    # Pseudo fields match case-insensitively, so a preset can spell one
    # "Duration" just to get a capitalized column header. An exact runinfo.txt
    # key always wins, so RUN stays the runinfo key and never becomes "run".
    pf="$f"; [[ -n "${V[$f]+x}" ]] || pf="${f,,}"
    case "$pf" in
        run)      printf '%s' "$run_name" ;;
        runid)    v=$(raw_value "RUN")
                  [[ -z "$v" && "$run_name" =~ ^run([0-9]+)$ ]] && v=$(( 10#${BASH_REMATCH[1]} ))
                  printf '%s' "$v" ;;
        start)    raw_value "RUN-START-TIME" ;;
        acqstart) raw_value "ACQ-START-TIME" ;;
        end)      raw_value "RUN-END-TIME" ;;
        nev|events) raw_value "TOTAL-NUMBER-OF-EVENTS-WRITTEN" ;;
        # Acquisition time proper: from when the acq threads start to when they
        # stop, i.e. without board setup and file flushing. Falls back to the
        # wall-clock endpoints when a station wrote the older key set.
        duration) s=$(raw_value "ACQ-START-TIME"); [[ -z "$s" ]] && s=$(raw_value "RUN-START-TIME")
                  e=$(raw_value "RUN-STOP-TIME");  [[ -z "$e" ]] && e=$(raw_value "RUN-END-TIME")
                  [[ -n "$s" && -n "$e" ]] && printf '%d' $(( ${e%.*} - ${s%.*} )) ;;
        duration_wall) s=$(raw_value "RUN-START-TIME"); e=$(raw_value "RUN-END-TIME")
                  [[ -n "$s" && -n "$e" ]] && printf '%d' $(( ${e%.*} - ${s%.*} )) ;;
        phased)   phased_summary ;;
        coinc0)   coinc_summary 0 ;;
        coinc1)   coinc_summary 1 ;;
        # Compact one-column summary of calib.sweep.*: "off" or "31.5->0/0.5 @100s"
        sweep)    v=$(raw_value "calib.sweep.enable")
                  if [[ -z "$v" ]]; then :
                  elif [[ "$v" != 1 ]]; then printf 'off'
                  else printf '%s->%s/%s @%ss' \
                       "$(raw_value calib.sweep.start_atten)" "$(raw_value calib.sweep.stop_atten)" \
                       "$(raw_value calib.sweep.atten_step)"  "$(raw_value calib.sweep.step_time)"
                  fi ;;
        *)        raw_value "$f" ;;
    esac
}

# ---------------------------------------------------------------------------
# field_display FIELD RUN_NAME -> value as shown (times/durations formatted)
# ---------------------------------------------------------------------------
field_display() {
    local f="$1" v pf
    v=$(field_value "$@")
    pf="$f"; [[ -n "${V[$f]+x}" ]] || pf="${f,,}"
    case "$pf" in
        start|end|acqstart|RUN-START-TIME|RUN-END-TIME|ACQ-START-TIME|RUN-STOP-TIME)
                  [[ -n "$v" ]] && format_time "$v" || printf -- '-' ;;
        duration|duration_wall)
                  [[ -n "$v" ]] && format_span "$v" || printf -- '-' ;;
        *)        [[ -n "$v" ]] && printf '%s' "$v" || printf -- '-' ;;
    esac
}

# --- argument parsing -------------------------------------------------------
run_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--preset)  PRESET="${2:-}";  shift 2 ;;
        -f|--fields)  MODE_FIELDS="${MODE_FIELDS:+${MODE_FIELDS},}${2:-}"; shift 2 ;;
        -w|--where)   WHERE+=("${2:-}"); shift 2 ;;
        -o|--format)  FORMAT="${2:-}";  shift 2 ;;
        -c|--cal)     SHOW_CAL=1; shift ;;
        -L|--list-fields) LIST_FIELDS=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        --)           shift; run_args+=("$@"); break ;;
        -*)           echo "ERROR: unknown option '$1'" >&2; usage >&2; exit 2 ;;
        *)            run_args+=("$1"); shift ;;
    esac
done

if [[ -z "$MODE_FIELDS" ]]; then
    if [[ -z "${PRESETS[$PRESET]+x}" ]]; then
        echo "ERROR: unknown preset '${PRESET}' (have: ${PRESET_ORDER})" >&2; exit 2
    fi
    MODE_FIELDS="${PRESETS[$PRESET]}"
fi
(( SHOW_CAL )) && MODE_FIELDS="${MODE_FIELDS},${CAL_FIELDS}"

case "$FORMAT" in
    table|list|csv) ;;
    *) echo "ERROR: unknown format '${FORMAT}' (expected table|list|csv)" >&2; exit 2 ;;
esac

# No directories on the command line -> read them from stdin (one per line).
if [[ ${#run_args[@]} -eq 0 && ! -t 0 ]]; then
    while IFS= read -r line; do
        [[ -n "$line" ]] && run_args+=("$line")
    done
fi
if [[ ${#run_args[@]} -eq 0 ]]; then
    echo "ERROR: no run directories given." >&2; usage >&2; exit 2
fi

# --- column specs -> parallel label / field arrays --------------------------
labels=() fields=()
IFS=',' read -r -a specs <<< "$MODE_FIELDS"
for spec in "${specs[@]}"; do
    spec="${spec#"${spec%%[![:space:]]*}"}"; spec="${spec%"${spec##*[![:space:]]}"}"
    [[ -z "$spec" ]] && continue
    if [[ "$spec" == *=* ]]; then
        labels+=("${spec%%=*}"); fields+=("${spec#*=}")
    else
        labels+=("${spec##*.}"); fields+=("$spec")   # header = last path component
    fi
done
# Disambiguate duplicate headers by falling back to the full field name.
for (( i = 0; i < ${#labels[@]}; i++ )); do
    for (( j = i + 1; j < ${#labels[@]}; j++ )); do
        if [[ "${labels[i]}" == "${labels[j]}" ]]; then
            labels[i]="${fields[i]}"; labels[j]="${fields[j]}"
        fi
    done
done

# --- sort by run number -----------------------------------------------------
mapfile -t run_dirs < <(
    for d in "${run_args[@]}"; do
        d="${d%/}"
        base=$(basename "$d")
        if [[ "$base" =~ ^run([0-9]+)$ ]]; then
            printf '%d\t%s\n' "$((10#${BASH_REMATCH[1]}))" "$d"
        else
            printf '%d\t%s\n' 999999999 "$d"
        fi
    done | sort -k1,1n -k2,2 | cut -f2-
)

declare -A V=()

# --- -L: dump everything the first readable run has -------------------------
if (( LIST_FIELDS )); then
    for run_dir in "${run_dirs[@]}"; do
        if load_run "$run_dir"; then
            echo "# available fields in $(basename "$run_dir")"
            for k in "${!V[@]}"; do
                [[ "$k" == @* ]] && continue
                printf '%s = %s\n' "$k" "${V[$k]}"
            done | sort
            exit 0
        fi
    done
    echo "ERROR: none of the given directories had aux/runinfo.txt or cfg/acq.cfg." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# passes_filters RUN_NAME -> 0 when the run matches every -w expression
# ---------------------------------------------------------------------------
passes_filters() {
    local run_name="$1" expr field op want got
    for expr in ${WHERE+"${WHERE[@]}"}; do
        if   [[ "$expr" == *"!="* ]]; then field="${expr%%!=*}"; op="ne"; want="${expr#*!=}"
        elif [[ "$expr" == *"~"*  ]]; then field="${expr%%~*}";  op="re"; want="${expr#*~}"
        elif [[ "$expr" == *"="*  ]]; then field="${expr%%=*}";  op="eq"; want="${expr#*=}"
        else echo "ERROR: bad --where expression '${expr}'" >&2; exit 2
        fi
        got=$(field_value "$field" "$run_name")
        case "$op" in
            eq) [[ "$got" == "$want" ]] || return 1 ;;
            ne) [[ "$got" != "$want" ]] || return 1 ;;
            re) [[ "$got" =~ $want   ]] || return 1 ;;
        esac
    done
    return 0
}

# ---------------------------------------------------------------------------
# csv_escape / render_table
# ---------------------------------------------------------------------------
csv_escape() {
    local v="$1"
    if [[ "$v" == *[,\"]* ]]; then printf '"%s"' "${v//\"/\"\"}"; else printf '%s' "$v"; fi
}

render_table() {   # TSV on stdin, first line is the header
    awk -F'\t' '
        function rowline(r,   i, v, line, fmt) {
            line = ""
            for (i = 1; i <= maxf; i++) {
                v = cell[r, i]
                # numeric columns are right-aligned, everything else left.
                # Width is baked into the format string: mawk rejects "%*s".
                fmt = num[i] ? ("%" (w[i] + 0) "s") : ("%-" (w[i] + 0) "s")
                line = line sprintf(fmt, v)
                if (i < maxf) line = line "  "
            }
            sub(/[[:space:]]+$/, "", line)
            return line
        }
        { rows = NR; if (NF > maxf) maxf = NF
          for (i = 1; i <= NF; i++) {
              cell[NR, i] = $i
              if (length($i) > w[i]) w[i] = length($i)
              if (NR > 1 && $i !~ /^-?[0-9]+(\.[0-9]+)?$/ && $i != "-") txt[i] = 1
          } }
        END {
            for (i = 1; i <= maxf; i++) num[i] = (rows > 1 && !txt[i])
            sep = ""
            for (i = 1; i <= maxf; i++) {
                for (j = 0; j < w[i]; j++) sep = sep "─"
                if (i < maxf) sep = sep "──"
            }
            head = rowline(1)
            print head; print sep
            for (r = 2; r <= rows; r++) print rowline(r)
            print sep; print head          # header repeated as footer
        }'
}

# --- collect and print ------------------------------------------------------
shown=0 skipped=0
tsv=""
(( ${#WHERE[@]} > 0 )) && echo "Filter: ${WHERE[*]}"

if [[ "$FORMAT" == "csv" ]]; then
    row=""
    for (( i = 0; i < ${#labels[@]}; i++ )); do row+="${row:+,}$(csv_escape "${labels[i]}")"; done
    echo "$row"
elif [[ "$FORMAT" == "table" ]]; then
    row=""
    for (( i = 0; i < ${#labels[@]}; i++ )); do row+="${row:+$'\t'}${labels[i]}"; done
    tsv="${row}"$'\n'
fi

for run_dir in "${run_dirs[@]}"; do
    run_name=$(basename "$run_dir")
    if ! load_run "$run_dir"; then
        echo "[${run_name}] WARNING: no aux/runinfo.txt or cfg/acq.cfg — skipping." >&2
        skipped=$((skipped + 1))
        continue
    fi
    passes_filters "$run_name" || continue
    shown=$((shown + 1))

    case "$FORMAT" in
        table)
            row=""
            for (( i = 0; i < ${#fields[@]}; i++ )); do
                row+="${row:+$'\t'}$(field_display "${fields[i]}" "$run_name")"
            done
            tsv+="${row}"$'\n'
            ;;
        csv)
            row=""
            for (( i = 0; i < ${#fields[@]}; i++ )); do
                row+="${row:+,}$(csv_escape "$(field_display "${fields[i]}" "$run_name")")"
            done
            echo "$row"
            ;;
        list)
            echo "${run_name}"
            for (( i = 0; i < ${#fields[@]}; i++ )); do
                printf '  %-22s %s\n' "${labels[i]}:" "$(field_display "${fields[i]}" "$run_name")"
            done
            echo
            ;;
    esac
done

if [[ "$FORMAT" == "table" ]]; then
    printf '%s' "$tsv" | render_table
fi

if [[ "$FORMAT" != "csv" ]]; then
    printf '%d run(s) shown' "$shown"
    (( skipped > 0 )) && printf ', %d skipped' "$skipped"
    printf '.\n'
fi

(( shown > 0 ))
