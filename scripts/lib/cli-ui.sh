#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build and test output helper for this repo's just recipes: numbered steps, live spinner,
# compact diagnostics.
# Source from bash:  source "$ROOT/scripts/lib/cli-ui.sh"
#
# NO_COLOR=1     plain text
# FORCE_COLOR=1  ANSI color even when piped
# FORCE_COLOR=0  plain text even on a TTY (empty FORCE_COLOR is ignored)
# NO_LIVE=1      one line per build action instead of the in-place spinner
# UI_VERBOSE=1   also print the lines the filter has no rule for
#
# Diagnostics: red [Error]: / orange [Warning]:, one line each. Compiler
# command dumps, xcodebuild argv, and rustc ASCII diagrams are omitted.
# Test runs (Swift Testing, XCTest, doctest) show a live pass count and one line per
# failed expectation. Timestamped runtime logs from the process under test
# are dropped. Other unrecognized lines are held back; when the command
# fails without a diagnostic, ui_run prints the last of them. The raw output
# of the last ui_run is kept at $UI_LAST_LOG.
# Optional sibling cli-ui.local.sh may define ui_spm_alias.

# shellcheck disable=SC2034  # exported for callers

if [[ "${CLI_UI_LOADED:-}" == "1" ]]; then
    return 0 2>/dev/null || true
fi
CLI_UI_LOADED=1

_ui_env_on() {
    case "${1-}" in
        ""|0|false|FALSE|no|NO|off|OFF) return 1 ;;
        *) return 0 ;;
    esac
}

_ui_init() {
    UI_T0="${UI_T0:-$(date +%s)}"
    UI_STEP_T0="$UI_T0"
    UI_TOTAL_STEPS="${UI_TOTAL_STEPS:-0}"
    UI_WIDTH="${UI_WIDTH:-56}"
    UI_ACTIVITY_PID="${UI_ACTIVITY_PID:-}"

    # Color when stdout is a TTY, unless NO_COLOR. FORCE_COLOR=1 forces on;
    # FORCE_COLOR=0 (or false) forces off. Empty FORCE_COLOR is ignored.
    if _ui_env_on "${FORCE_COLOR-}"; then
        UI_COLOR=1
    elif [[ -n "${FORCE_COLOR-}" ]] || [[ -n "${NO_COLOR:-}" ]] || [[ ! -t 1 ]]; then
        UI_COLOR=0
    else
        UI_COLOR=1
    fi

    # In-place progress only when stdout is a real terminal (logs stay line-based).
    if [[ -n "${NO_LIVE:-}" ]] || [[ ! -t 1 ]]; then
        UI_LIVE=0
    else
        UI_LIVE=1
    fi

    if (( UI_COLOR )); then
        UI_RESET=$'\033[0m'
        UI_BOLD=$'\033[1m'
        UI_DIM=$'\033[2m'
        UI_RED=$'\033[31m'
        UI_GREEN=$'\033[32m'
        UI_YELLOW=$'\033[33m'
        UI_ORANGE=$'\033[38;5;208m'
        UI_BLUE=$'\033[34m'
        UI_CYAN=$'\033[36m'
        UI_MAGENTA=$'\033[35m'
        UI_WHITE=$'\033[37m'
    else
        UI_RESET="" UI_BOLD="" UI_DIM=""
        UI_RED="" UI_GREEN="" UI_YELLOW="" UI_ORANGE="" UI_BLUE="" UI_CYAN=""
        UI_MAGENTA="" UI_WHITE=""
    fi
}
_ui_init

ui_elapsed() {
    local now
    now="$(date +%s)"
    echo $((now - UI_T0))
}

ui_step_elapsed() {
    local now
    now="$(date +%s)"
    echo $((now - UI_STEP_T0))
}

# Format seconds as 12s or 1m 05s
ui_fmt_secs() {
    local s="${1:-0}"
    if (( s < 60 )); then
        printf '%ds' "$s"
    else
        printf '%dm %02ds' $((s / 60)) $((s % 60))
    fi
}

ui_hr() {
    local char="${1:-─}"
    local n="$UI_WIDTH"
    local line=""
    local i
    for ((i = 0; i < n; i++)); do
        line+="$char"
    done
    printf '%s\n' "$line"
}

# Compact progress bar: filled/empty for step n of total
ui_bar() {
    local n="$1"
    local total="$2"
    local width="${3:-18}"
    if (( total <= 0 )); then
        printf '%s' ""
        return
    fi
    local filled=$(( (n * width) / total ))
    (( filled > width )) && filled=$width
    local empty=$((width - filled))
    local bar=""
    local i
    for ((i = 0; i < filled; i++)); do bar+="█"; done
    for ((i = 0; i < empty; i++)); do bar+="░"; done
    printf '%s' "$bar"
}

ui_banner() {
    local title="$1"
    local subtitle="${2:-}"
    echo
    if (( UI_COLOR )); then
        printf '%s╭─ %s%s%s' "$UI_DIM" "$UI_RESET$UI_BOLD$UI_CYAN" "$title" "$UI_RESET"
        local pad=$(( UI_WIDTH - ${#title} - 4 ))
        (( pad < 0 )) && pad=0
        local i
        for ((i = 0; i < pad; i++)); do printf '%s─%s' "$UI_DIM" "$UI_RESET"; done
        printf '%s╮%s\n' "$UI_DIM" "$UI_RESET"
        if [[ -n "$subtitle" ]]; then
            printf '%s│%s  %s%s%s\n' "$UI_DIM" "$UI_RESET" "$UI_DIM" "$subtitle" "$UI_RESET"
        fi
        printf '%s╰' "$UI_DIM"
        local j
        for ((j = 0; j < UI_WIDTH; j++)); do printf '─'; done
        printf '╯%s\n' "$UI_RESET"
    else
        echo "=== $title ==="
        [[ -n "$subtitle" ]] && echo "  $subtitle"
    fi
    echo
}

ui_plan_header() {
    printf '%s%s%s\n' "$UI_BOLD" "$1" "$UI_RESET"
}

# Key/value row used in package plan summaries
ui_kv() {
    local key="$1"
    local value="$2"
    local accent="${3:-}"
    printf '  %s%-16s%s' "$UI_DIM" "$key" "$UI_RESET"
    if [[ -n "$accent" ]] && (( UI_COLOR )); then
        printf '%s%s%s\n' "$accent" "$value" "$UI_RESET"
    else
        printf '%s\n' "$value"
    fi
}

# Start a numbered step. Args: n [total] label
# If total omitted, uses UI_TOTAL_STEPS when set, else bare n.
ui_step() {
    _ui_live_end
    local n="$1"
    shift
    local total="$UI_TOTAL_STEPS"
    local label
    if [[ "${1:-}" =~ ^[0-9]+$ ]] && [[ $# -ge 2 ]]; then
        total="$1"
        shift
    fi
    label="$*"
    UI_STEP_T0="$(date +%s)"
    local elapsed
    elapsed="$(ui_fmt_secs "$(ui_elapsed)")"
    echo
    if (( total > 0 )); then
        local bar
        bar="$(ui_bar "$n" "$total")"
        if (( UI_COLOR )); then
            printf '  %s[%s%d%s/%d]%s %s%s%s  %s%s%s  %s(+%s)%s\n' \
                "$UI_CYAN$UI_BOLD" "$UI_RESET$UI_CYAN$UI_BOLD" "$n" \
                "$UI_DIM" "$total" "$UI_RESET" \
                "$UI_DIM" "$bar" "$UI_RESET" \
                "$UI_BOLD" "$label" "$UI_RESET" \
                "$UI_DIM" "$elapsed" "$UI_RESET"
        else
            printf '  [%d/%d] %s  (+%s)\n' "$n" "$total" "$label" "$elapsed"
        fi
    else
        if (( UI_COLOR )); then
            printf '  %s[%d]%s %s%s%s  %s(+%s)%s\n' \
                "$UI_CYAN$UI_BOLD" "$n" "$UI_RESET" \
                "$UI_BOLD" "$label" "$UI_RESET" \
                "$UI_DIM" "$elapsed" "$UI_RESET"
        else
            printf '  [%d] %s  (+%s)\n' "$n" "$label" "$elapsed"
        fi
    fi
}

ui_step_done() {
    _ui_live_end
    local detail="${1:-}"
    local took
    took="$(ui_fmt_secs "$(ui_step_elapsed)")"
    if [[ -n "$detail" ]]; then
        printf '  %s✓%s %s %s(%s)%s\n' "$UI_GREEN" "$UI_RESET" "$detail" "$UI_DIM" "$took" "$UI_RESET"
    else
        printf '  %s✓%s done %s(%s)%s\n' "$UI_GREEN" "$UI_RESET" "$UI_DIM" "$took" "$UI_RESET"
    fi
}

ui_detail() {
    printf '  %s%s%s\n' "$UI_DIM" "$*" "$UI_RESET"
}

ui_info() {
    printf '  %s→%s %s\n' "$UI_BLUE" "$UI_RESET" "$*"
}

ui_ok() {
    printf '  %s✓%s %s\n' "$UI_GREEN$UI_BOLD" "$UI_RESET" "$*"
}

ui_warn() {
    printf '  %s[Warning]: %s%s\n' "$UI_ORANGE$UI_BOLD" "$*" "$UI_RESET" >&2
}

ui_err() {
    printf '  %s[Error]: %s%s\n' "$UI_RED$UI_BOLD" "$*" "$UI_RESET" >&2
}

ui_skip() {
    printf '  %s·%s %s %s(skipped)%s\n' "$UI_YELLOW" "$UI_RESET" "$*" "$UI_DIM" "$UI_RESET"
}

ui_done() {
    local summary="${1:-Done}"
    local extra="${2:-}"
    local elapsed
    elapsed="$(ui_fmt_secs "$(ui_elapsed)")"
    echo
    if (( UI_COLOR )); then
        printf '%s%s%s  %s%s%s\n' \
            "$UI_GREEN$UI_BOLD" "$summary" "$UI_RESET" \
            "$UI_DIM" "in $elapsed" "$UI_RESET"
    else
        printf '%s  in %s\n' "$summary" "$elapsed"
    fi
    if [[ -n "$extra" ]]; then
        printf '  %s%s%s\n' "$UI_DIM" "$extra" "$UI_RESET"
    fi
    echo
}

# Heartbeat for a quiet build step.
ui_activity_start() {
    local label="$1"
    local interval="${2:-8}"
    ui_activity_stop
    (
        while true; do
            sleep "$interval"
            local elapsed
            elapsed="$(ui_fmt_secs "$(ui_elapsed)")"
            if (( UI_COLOR )); then
                printf '  %s…%s %s  %s(%s)%s\n' \
                    "$UI_DIM" "$UI_RESET" "$label" \
                    "$UI_DIM" "$elapsed" "$UI_RESET"
            else
                printf '  … %s  (%s)\n' "$label" "$elapsed"
            fi
        done
    ) &
    UI_ACTIVITY_PID=$!
}

ui_activity_stop() {
    if [[ -n "${UI_ACTIVITY_PID:-}" ]] && kill -0 "$UI_ACTIVITY_PID" 2>/dev/null; then
        kill "$UI_ACTIVITY_PID" 2>/dev/null || true
        wait "$UI_ACTIVITY_PID" 2>/dev/null || true
    fi
    UI_ACTIVITY_PID=""
}

# ── Build-output polish (live overwrite + path shorten + color) ───────────────

# Repo root for path shortening. Callers should set UI_REPO_ROOT before sourcing
# when possible; otherwise we infer from this file's location.
if [[ -z "${UI_REPO_ROOT:-}" ]]; then
    UI_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fi

_UI_SPIN=(⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏)
_UI_SPIN_ASCII=('|' '/' '-' '\\')
_ui_live=0
_ui_live_n=0
_ui_live_total=0
_ui_live_label=""
_ui_spin_i=0
_ui_cursor_hidden=0
_ui_eat_argv=0
_ui_diag_kind=""
_ui_diag_msg=""
_ui_diag_code=""
_ui_last_diag=""

_ui_cols() {
    local c="${COLUMNS:-}"
    if [[ -z "$c" ]] && command -v tput >/dev/null 2>&1; then
        c="$(tput cols 2>/dev/null || true)"
    fi
    if [[ -z "$c" ]]; then
        c="$(stty size 2>/dev/null | awk '{print $2}')" || true
    fi
    printf '%s' "${c:-80}"
}

# Strip CR and unicode thin/nbsp so Swift `[1 / 13]` matches the same as `[1/13]`.
_ui_prep_line() {
    local s="$1"
    s="${s//$'\r'/}"
    s="${s//$'\u200b'/}"
    s="${s// / }"
    s="${s// / }"
    s="${s// / }"
    printf '%s' "$s"
}

_ui_trim() {
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

_ui_bar_colored() {
    local n="$1" total="$2" width="${3:-14}"
    local filled empty i
    if (( total <= 0 )); then
        printf '%s' ""
        return
    fi
    filled=$(( (n * width) / total ))
    (( filled > width )) && filled=$width
    empty=$((width - filled))
    if (( UI_COLOR )); then
        printf '%s' "$UI_GREEN"
        for ((i = 0; i < filled; i++)); do printf '█'; done
        printf '%s' "$UI_RESET$UI_DIM"
        for ((i = 0; i < empty; i++)); do printf '░'; done
        printf '%s' "$UI_RESET"
    else
        for ((i = 0; i < filled; i++)); do printf '#'; done
        for ((i = 0; i < empty; i++)); do printf '-'; done
    fi
}

_ui_pretty_spm_label() {
    local rest="$1"
    rest="$(_ui_trim "$rest")"
    if [[ -z "$rest" ]]; then
        printf '%s' "${_ui_live_label:-working}"
        return
    fi
    local stripped
    stripped="$(ui_strip_spm_target "$rest")"
    stripped="$(_ui_trim "$stripped")"
    if [[ "$stripped" =~ ^Compiling[[:space:]]+(.+)$ ]]; then
        printf '%s' "${BASH_REMATCH[1]}"
        return
    fi
    case "$stripped" in
        Linking|Linking*) printf '%s' "linking"; return ;;
        Applying|Applying*) printf '%s' "applying"; return ;;
        "Emitting module"|"Emitting module"*) printf '%s' "emitting"; return ;;
    esac
    ui_spm_alias "$stripped"
}

# Empty [n/m] rest is normal near link; do not print "working".
_ui_spm_counter_label() {
    local n="$1"
    local total="$2"
    local rest="$3"
    rest="$(_ui_trim "$rest")"
    if [[ -z "$rest" ]]; then
        if (( total > 0 && n + 2 >= total )); then
            printf '%s' "linking"
        else
            printf '%s' "compiling"
        fi
        return
    fi
    _ui_pretty_spm_label "$rest"
}

# Default: stripped SPM target name. Override in cli-ui.local.sh.
ui_spm_alias() {
    local t="$1"
    t="${t%-product}"
    printf '%s' "$t"
}

# Compact xcodebuild action → one spinner label (filename or verb).
_ui_pretty_xcode_label() {
    local t="$1"
    local file
    if [[ "$t" =~ /([^/]+\.swift) ]]; then
        printf '%s' "${BASH_REMATCH[1]}"
        return
    fi
    if [[ "$t" =~ /([^/]+\.[cm]) ]]; then
        printf '%s' "${BASH_REMATCH[1]}"
        return
    fi
    case "$t" in
        Ld*|Linking*) printf '%s' "linking"; return ;;
        CodeSign*) printf '%s' "signing"; return ;;
        Touch*) printf '%s' "touch"; return ;;
        Validate*) printf '%s' "validate"; return ;;
        CopySwiftLibs*) printf '%s' "copy libs"; return ;;
        ProcessInfoPlistFile*) printf '%s' "Info.plist"; return ;;
        GenerateDSYMFile*) printf '%s' "dSYM"; return ;;
        ExtractAppIntentsMetadata*|AppIntentsSSUTraining*) printf '%s' "intents"; return ;;
        SwiftEmitModule*|SwiftDriver*) printf '%s' "emitting"; return ;;
        CompileSwift*|SwiftCompile*) printf '%s' "compiling"; return ;;
        CompileC*) printf '%s' "clang"; return ;;
    esac
    printf '%s' "xcodebuild"
}

_ui_live_paint() {
    ((_ui_live)) || return 0
    local cols frame nums label elapsed budget
    cols="$(_ui_cols)"
    (( cols < 48 )) && cols=48

    if (( UI_COLOR )); then
        frame="${_UI_SPIN[_ui_spin_i]}"
    else
        frame="${_UI_SPIN_ASCII[_ui_spin_i % 4]}"
    fi
    elapsed="$(ui_fmt_secs "$(ui_step_elapsed)")"
    label="${_ui_live_label}"
    nums=""
    if ((_ui_live_total > 0)); then
        nums="${_ui_live_n}/${_ui_live_total}"
    fi

    # Keep the line on one row so \\r overwrite does not leave wrapped junk.
    budget=$((cols - 8 - ${#nums} - ${#elapsed} - 18))
    (( budget < 8 )) && budget=8
    if (( ${#label} > budget )); then
        label="${label:0:$((budget - 1))}…"
    fi

    printf '\r\033[2K'
    if (( UI_COLOR )); then
        printf '    %s%s%s' "$UI_CYAN" "$frame" "$UI_RESET"
        if [[ -n "$nums" ]]; then
            printf '  %s%s%s%s/%s%s%s' \
                "$UI_BOLD$UI_CYAN" "$_ui_live_n" "$UI_RESET" \
                "$UI_DIM" "$UI_RESET$UI_CYAN" "$_ui_live_total" "$UI_RESET"
            printf '  %s' "$(_ui_bar_colored "$_ui_live_n" "$_ui_live_total" 14)"
        fi
        if [[ -n "$label" ]]; then
            printf '  %s%s%s' "$UI_BOLD" "$label" "$UI_RESET"
        fi
        printf '  %s%s%s' "$UI_DIM" "$elapsed" "$UI_RESET"
    else
        printf '    %s' "$frame"
        if [[ -n "$nums" ]]; then
            printf '  %s  %s' "$nums" "$(_ui_bar_colored "$_ui_live_n" "$_ui_live_total" 14)"
        fi
        [[ -n "$label" ]] && printf '  %s' "$label"
        printf '  %s' "$elapsed"
    fi
}

_ui_live_set() {
    _ui_live_n="${1:-0}"
    _ui_live_total="${2:-0}"
    _ui_live_label="${3:-}"
    _ui_live=1
    _ui_spin_i=$(( (_ui_spin_i + 1) % ${#_UI_SPIN[@]} ))
    _ui_live_paint
}

_ui_live_end() {
    if ((_ui_live)); then
        printf '\r\033[2K'
        _ui_live=0
    fi
    if ((_ui_cursor_hidden)); then
        printf '\033[?25h'
        _ui_cursor_hidden=0
    fi
}

_ui_sticky() {
    local rendered="$1"
    if ((_ui_live)); then
        printf '\r\033[2K'
        printf '%s\n' "$rendered"
        _ui_live_paint
    else
        printf '%s\n' "$rendered"
    fi
}

# Shorten absolute paths: $HOME → ~, repo root → relative, collapse ../ noise.
ui_shorten_text() {
    local s="$1"
    local home="${HOME:-}"
    local root="${UI_REPO_ROOT:-}"
    if [[ -n "$root" ]]; then
        s="${s//$root\//}"
        s="${s//$root/}"
    fi
    if [[ -n "$home" ]]; then
        s="${s//$home\//~/}"
        s="${s//$home/~}"
    fi
    local news
    while [[ "$s" == *"/../"* ]]; do
        news="$(sed -E 's|/[^/]+/\.\./|/|g' <<<"$s")"
        [[ "$news" == "$s" ]] && break
        s="$news"
    done
    while [[ "$s" =~ (^|[[:space:]\(])[A-Za-z0-9_.-]+/\.\./ ]]; do
        news="$(sed -E 's/(^|[[:space:]\(])[A-Za-z0-9_.-]+\/\.\.\//\1/g' <<<"$s")"
        [[ "$news" == "$s" ]] && break
        s="$news"
    done
    printf '%s' "$s"
}

# Drop SPM target noise without baking product names into the core.
ui_strip_spm_target() {
    local s="$1"
    s="$(sed -E \
        -e "s/[[:space:]]*\\(in target '[^']+' from project '[^']+'\\)//g" \
        -e "s/(Compiling)[[:space:]]+[A-Za-z0-9_-]+[[:space:]]+/\\1 /g" \
        -e "s/(Linking)[[:space:]]+[A-Za-z0-9_-]+([[:space:]]|\$)/\\1\\2/g" \
        -e "s/(Applying)[[:space:]]+[A-Za-z0-9_-]+([[:space:]]|\$)/\\1\\2/g" \
        -e "s/(Emitting module)[[:space:]]+[A-Za-z0-9_-]+([[:space:]]|\$)/\\1\\2/g" \
        <<<"$s")"
    printf '%s' "$s"
}

_ui_strip_ansi() {
    printf '%s' "$1" | sed -E $'s/\x1B\\[[0-9;?]*[ -/]*[@-~]//g'
}

# Thin spaces (U+2009) and nbsp so Swift `[1 / 13]` matches `[1/13]`.
ui_normalize_progress_line() {
    _ui_prep_line "$1"
}

UI_PROGRESS_OPEN=0
UI_PROGRESS_CUR=0
UI_PROGRESS_TOT=0
UI_PROGRESS_DETAIL=""

ui_progress_close() {
    if (( UI_PROGRESS_OPEN )); then
        printf '\n'
        UI_PROGRESS_OPEN=0
    fi
}

# Rewrite (or print) a live [cur/tot] line. Used by callers that colorize
# stdin themselves; ui_run uses the spinner instead.
ui_progress_update() {
    local cur="$1"
    local tot="$2"
    local detail="$3"
    UI_PROGRESS_CUR="$cur"
    UI_PROGRESS_TOT="$tot"
    UI_PROGRESS_DETAIL="$detail"
    if [[ -n "${UI_PROGRESS_STATE:-}" ]]; then
        printf '%s|%s|%s\n' "$cur" "$tot" "$detail" >"$UI_PROGRESS_STATE"
    fi

    local plain
    plain="$(printf '  [%s/%s] %s' "$cur" "$tot" "$detail")"

    if [[ ! -t 1 ]]; then
        local last="${UI_PROGRESS_LAST_PRINTED:-0}"
        if (( cur == tot || cur - last >= 5 || cur < last )); then
            printf '%s\n' "$plain"
            UI_PROGRESS_LAST_PRINTED=$cur
        fi
        return
    fi

    if (( UI_COLOR )); then
        printf '\r  %s[%s%s%s/%s%s%s]%s %s%s' \
            "$UI_CYAN$UI_BOLD" "$UI_RESET$UI_CYAN$UI_BOLD" "$cur" \
            "$UI_DIM" "$UI_CYAN" "$tot" "$UI_RESET$UI_CYAN$UI_BOLD" "$UI_RESET" \
            "$detail" $'\033[K'
    else
        printf '\r%s%s' "$plain" $'\033[K'
    fi
    UI_PROGRESS_OPEN=1
}

# Step markers: ▸ ✓ ✗, dim signing chatter. Prints with no trailing newline.
# Returns 1 if the line is not a marker.
_ui_marker_line() {
    local t="$1"
    if [[ "$t" =~ ^▸[[:space:]]+(.*)$ ]]; then
        if (( UI_COLOR )); then
            printf '  %s▸%s %s%s%s' "$UI_CYAN$UI_BOLD" "$UI_RESET" "$UI_BOLD" "${BASH_REMATCH[1]}" "$UI_RESET"
        else
            printf '  ▸ %s' "${BASH_REMATCH[1]}"
        fi
        return 0
    fi
    if [[ "$t" =~ ^✓[[:space:]]+(.*)$ ]]; then
        if (( UI_COLOR )); then
            printf '  %s✓%s %s' "$UI_GREEN$UI_BOLD" "$UI_RESET" "${BASH_REMATCH[1]}"
        else
            printf '  ✓ %s' "${BASH_REMATCH[1]}"
        fi
        return 0
    fi
    if [[ "$t" =~ ^✗[[:space:]]+(.*)$ ]]; then
        if (( UI_COLOR )); then
            printf '  %s✗%s %s' "$UI_RED$UI_BOLD" "$UI_RESET" "${BASH_REMATCH[1]}"
        else
            printf '  ✗ %s' "${BASH_REMATCH[1]}"
        fi
        return 0
    fi
    if [[ "$t" =~ ^(signing|re-signing|stamped|legacy) ]]; then
        printf '  %s%s%s' "$UI_DIM" "$t" "$UI_RESET"
        return 0
    fi
    return 1
}

_ui_emit_diag() {
    local kind="$1"
    local loc="$2"
    local msg="$3"
    local tag color body rendered
    msg="$(_ui_trim "$msg")"
    loc="$(_ui_trim "$loc")"
    [[ -z "$msg" ]] && return 0
    case "$kind" in
        error)
            tag="[Error]:"
            color="$UI_RED$UI_BOLD"
            ;;
        warning)
            tag="[Warning]:"
            color="$UI_ORANGE$UI_BOLD"
            ;;
        *)
            tag="[Note]:"
            color="$UI_DIM"
            ;;
    esac
    if [[ "${_ui_last_diag}" == "$kind|$loc|$msg" ]]; then
        return 0
    fi
    _ui_last_diag="$kind|$loc|$msg"
    if [[ "$kind" != note ]] && [[ "$msg" != *" FAILED" ]]; then
        _ui_diag_count=$((_ui_diag_count + 1))
    fi
    if [[ -n "$loc" ]]; then
        body="$loc  $msg"
    else
        body="$msg"
    fi
    if (( UI_COLOR )); then
        rendered="$(printf '  %s%s %s%s' "$color" "$tag" "$body" "$UI_RESET")"
    else
        rendered="$(printf '  %s %s' "$tag" "$body")"
    fi
    _ui_sticky "$rendered"
}

_ui_flush_diag() {
    local loc="${1:-}"
    [[ -z "${_ui_diag_kind}" ]] && return 0
    local msg="${_ui_diag_msg}"
    if [[ -n "${_ui_diag_code}" ]]; then
        msg="${msg} (${_ui_diag_code})"
    fi
    _ui_emit_diag "${_ui_diag_kind}" "$loc" "$msg"
    _ui_diag_kind=""
    _ui_diag_msg=""
    _ui_diag_code=""
}

_ui_hold_diag() {
    local kind="$1"
    local msg="$2"
    local code="${3:-}"
    _ui_flush_diag
    _ui_diag_kind="$kind"
    _ui_diag_msg="$msg"
    _ui_diag_code="$code"
}

_ui_note_useful() {
    local m="$1"
    [[ "$m" =~ (expected|found|candidate|did you mean|requires|in call to|protocol|argument) ]]
}

_ui_is_diag_spam() {
    local t="$1"
    # Real compiler diagnostics always win, even if the path is under Xcode.
    if [[ "$t" =~ :[0-9]+:[0-9]+:[[:space:]]+(error|warning|note|fatal[[:space:]]error): ]]; then
        return 1
    fi
    if [[ "$t" =~ ^(error|warning)\[[A-Z][0-9]+\]: ]]; then
        return 1
    fi
    case "$t" in
        "Failed frontend command:"|"error: Build failed"|"Command SwiftCompile failed with a nonzero exit code")
            return 0 ;;
    esac
    [[ "$t" == Failed\ frontend\ command:* ]] && return 0
    [[ "$t" == The\ following\ build\ commands\ failed:* ]] && return 0
    [[ "$t" == \(*failure*\) ]] && return 0
    [[ "$t" == Command\ line:* ]] && return 0
    [[ "$t" == builtin-Swift* ]] && return 0
    [[ "$t" == error:\ SwiftCompile* ]] && return 0
    [[ "$t" == error:\ emit-module* ]] && return 0
    [[ "$t" == error:\ compile\ command\ failed* ]] && return 0
    [[ "$t" == error:\ link\ command\ failed* ]] && return 0
    [[ "$t" == error:\ linker\ command\ failed* ]] && return 0
    [[ "$t" == error:\ Incremental\ compilation* ]] && return 0
    [[ "$t" == error:\ could\ not\ compile* ]] && return 0
    [[ "$t" == error:\ aborting\ due* ]] && return 0
    [[ "$t" == error:\ could\ not\ document* ]] && return 0
    [[ "$t" == warning:\ build\ failed* ]] && return 0
    [[ "$t" =~ ^warning:.*generated\ [0-9]+\ warnings? ]] && return 0
    [[ "$t" == For\ more\ information\ about\ this\ error* ]] && return 0
    [[ "$t" == Some\ errors\ have\ detailed\ explanations* ]] && return 0
    [[ "$t" == note:\ Run\ with\ \`RUST_BACKTRACE* ]] && return 0
    [[ "$t" == note:\ using\ incremental* ]] && return 0
    [[ "$t" == *Please\ submit\ a\ bug* ]] && return 0
    [[ "$t" == Stack\ dump:* ]] && return 0
    [[ "$t" == *swift-frontend* ]] && return 0
    [[ "$t" == /Applications/Xcode* ]] && return 0
    [[ "$t" == *Toolchains/*swift* ]] && return 0
    [[ "$t" == *-filelist\ * ]] && return 0
    [[ "$t" == *builtin-SwiftPerFileCompile* ]] && return 0
    if [[ "$t" =~ ^-[[:alnum:]._-]+ ]] && (( ${#t} > 48 )); then
        return 0
    fi
    return 1
}

_ui_is_compiler_argv() {
    local t="$1"
    ((_ui_eat_argv)) || return 1
    if [[ "$t" =~ :[0-9]+:[0-9]+:[[:space:]]+(error|warning|note|fatal[[:space:]]error): ]]; then
        return 1
    fi
    if [[ "$t" =~ ^\[([0-9]+)[[:space:]]*/[[:space:]]*([0-9]+)\] ]]; then
        return 1
    fi
    if [[ "$t" =~ ^(error|warning)\[[A-Z][0-9]+\]: ]]; then
        return 1
    fi
    # Failed frontend dumps are followed by the real failure footer; do not eat it.
    if [[ "$t" == "** BUILD FAILED **" || "$t" == "** TEST FAILED **" ]]; then
        return 1
    fi
    if [[ "$t" =~ ^(clang|ld|ld64|swiftc|xcodebuild|xcode-select):[[:space:]]+(error|warning|fatal[[:space:]]error): ]]; then
        return 1
    fi
    if [[ "$t" =~ ^(error|fatal[[:space:]]error|warning): ]]; then
        return 1
    fi
    return 0
}

_ui_is_rustc_snippet() {
    local s
    s="$(_ui_trim "$1")"
    # Globs, not ERE: `|` and `^` are operators in bash/zsh regex.
    [[ "$s" == "|"* ]] && return 0
    [[ "$s" == "^"* ]] && return 0
    [[ "$s" == "~"* ]] && return 0
    [[ "$s" == "= note:"* || "$s" == "= help:"* || "$s" == "= warning:"* ]] && return 0
    [[ "$s" =~ ^[0-9]+[[:space:]]+[|] ]] && return 0
    return 1
}

# Returns 0 if the line was a diagnostic (emitted or held / skipped).
_ui_try_diag() {
    local t="$1"
    local file loc kind msg code

    local rustc_loc_re='^[[:space:]]*-->[[:space:]]+(.+):([0-9]+):([0-9]+)'
    if [[ "$t" =~ $rustc_loc_re ]]; then
        loc="${BASH_REMATCH[1]}:${BASH_REMATCH[2]}:${BASH_REMATCH[3]}"
        if [[ -n "${_ui_diag_kind}" ]]; then
            _ui_flush_diag "$loc"
        fi
        return 0
    fi

    if _ui_is_rustc_snippet "$t"; then
        return 0
    fi

    if [[ "$t" =~ ^(error|warning)\[([A-Z][0-9]+)\]:[[:space:]]*(.*)$ ]]; then
        kind="${BASH_REMATCH[1]}"
        code="${BASH_REMATCH[2]}"
        msg="${BASH_REMATCH[3]}"
        _ui_hold_diag "$kind" "$msg" "$code"
        return 0
    fi

    if [[ "$t" =~ ^(.+):([0-9]+):([0-9]+):[[:space:]]+(error|warning|note|fatal[[:space:]]error):[[:space:]]+(.*)$ ]]; then
        file="${BASH_REMATCH[1]}"
        loc="${file}:${BASH_REMATCH[2]}:${BASH_REMATCH[3]}"
        kind="${BASH_REMATCH[4]}"
        msg="${BASH_REMATCH[5]}"
        [[ "$kind" == "fatal error" ]] && kind="error"
        _ui_flush_diag
        if [[ "$kind" == "note" ]] && ! _ui_note_useful "$msg"; then
            return 0
        fi
        _ui_emit_diag "$kind" "$loc" "$msg"
        return 0
    fi

    if [[ "$t" =~ ^(.+):([0-9]+):[[:space:]]+(error|warning|note|fatal[[:space:]]error):[[:space:]]+(.*)$ ]]; then
        loc="${BASH_REMATCH[1]}:${BASH_REMATCH[2]}"
        kind="${BASH_REMATCH[3]}"
        msg="${BASH_REMATCH[4]}"
        [[ "$kind" == "fatal error" ]] && kind="error"
        _ui_flush_diag
        if [[ "$kind" == "note" ]] && ! _ui_note_useful "$msg"; then
            return 0
        fi
        _ui_emit_diag "$kind" "$loc" "$msg"
        return 0
    fi

    if [[ "$t" =~ ^(clang|ld|ld64|swiftc|xcodebuild|xcode-select):[[:space:]]+(error|warning|fatal[[:space:]]error):[[:space:]]+(.*)$ ]]; then
        kind="${BASH_REMATCH[2]}"
        msg="${BASH_REMATCH[3]}"
        [[ "$kind" == "fatal error" ]] && kind="error"
        _ui_flush_diag
        _ui_emit_diag "$kind" "" "$msg"
        return 0
    fi

    if [[ "$t" =~ ^(error|fatal[[:space:]]error):[[:space:]]+(.*)$ ]]; then
        _ui_hold_diag "error" "${BASH_REMATCH[2]}"
        return 0
    fi
    if [[ "$t" =~ ^warning:[[:space:]]+(.*)$ ]]; then
        _ui_hold_diag "warning" "${BASH_REMATCH[1]}"
        return 0
    fi
    if [[ "$t" =~ ^note:[[:space:]]+(.*)$ ]]; then
        msg="${BASH_REMATCH[1]}"
        if _ui_note_useful "$msg"; then
            _ui_flush_diag
            _ui_emit_diag "note" "" "$msg"
        fi
        return 0
    fi

    return 1
}

_ui_sticky_colorize() {
    local line="$1"
    local trimmed
    trimmed="$(_ui_trim "$line")"

    if (( ! UI_COLOR )); then
        printf '  %s' "$trimmed"
        return
    fi
    if [[ "$trimmed" =~ ^(error|fatal error): ]]; then
        printf '  %s%s%s' "$UI_RED$UI_BOLD" "$trimmed" "$UI_RESET"
        return
    fi
    if [[ "$trimmed" =~ ^warning: ]]; then
        printf '  %s%s%s' "$UI_YELLOW" "$trimmed" "$UI_RESET"
        return
    fi
    if [[ "$trimmed" =~ ^note: ]]; then
        printf '  %s%s%s' "$UI_DIM" "$trimmed" "$UI_RESET"
        return
    fi
    if [[ "$trimmed" =~ ^(unchanged|wrote)[[:space:]] ]]; then
        printf '  %s·%s %s%s%s' "$UI_DIM" "$UI_RESET" "$UI_DIM" "$trimmed" "$UI_RESET"
        return
    fi
    printf '  %s' "$trimmed"
}

# Color one build line when not using live overwrite (piped / NO_LIVE),
# and when a caller feeds lines itself (step markers).
ui_colorize_build_line() {
    local line="$1"
    local n total rest trimmed marker
    line="$(_ui_prep_line "$(ui_strip_spm_target "$line")")"
    trimmed="$(_ui_trim "$line")"

    if [[ "$trimmed" =~ ^\[([0-9]+)[[:space:]]*/[[:space:]]*([0-9]+)\](.*)$ ]]; then
        n="${BASH_REMATCH[1]}"
        total="${BASH_REMATCH[2]}"
        rest="$(_ui_spm_counter_label "$n" "$total" "${BASH_REMATCH[3]}")"
        if [[ -t 1 ]] && [[ -z "${NO_LIVE:-}" ]]; then
            ui_progress_update "$n" "$total" "$rest"
            return
        fi
        if (( UI_COLOR )); then
            printf '  %s[%s/%s]%s %s\n' "$UI_CYAN$UI_BOLD" "$n" "$total" "$UI_RESET" "$rest"
        else
            printf '  [%s/%s] %s\n' "$n" "$total" "$rest"
        fi
        return
    fi

    ui_progress_close

    if marker="$(_ui_marker_line "$trimmed")"; then
        printf '%s\n' "$marker"
        return
    fi

    if [[ "$trimmed" =~ ^(Build\ complete!)[[:space:]]*\(([0-9.]+s)\) ]]; then
        if (( UI_COLOR )); then
            printf '  %s%s%s  %s(%s)%s\n' \
                "$UI_GREEN$UI_BOLD" "${BASH_REMATCH[1]}" "$UI_RESET" \
                "$UI_CYAN" "${BASH_REMATCH[2]}" "$UI_RESET"
        else
            printf '  %s (%s)\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}"
        fi
        return
    fi

    if [[ "$trimmed" =~ ^Build\ complete! ]]; then
        if (( UI_COLOR )); then
            printf '  %s%s%s\n' "$UI_GREEN$UI_BOLD" "$trimmed" "$UI_RESET"
        else
            printf '  %s\n' "$trimmed"
        fi
        return
    fi

    if [[ "$trimmed" =~ ^(Compiling|Finished|Checking|Downloading|Downloaded|Blocking)[[:space:]] ]]; then
        printf '  %s%s%s\n' "$UI_DIM" "$trimmed" "$UI_RESET"
        return
    fi

    _ui_sticky_colorize "$line"
    printf '\n'
}

_ui_is_chatter() {
    local t="$1"
    case "$t" in
        "Building for debugging..."|"Building for production..."|"Building for testing...")
            return 0 ;;
        "[Planning deferred tasks]"|"Planning deferred tasks")
            return 0 ;;
        "note: Building targets in dependency order"|"note: Target dependency graph for configuration")
            return 0 ;;
    esac
    if [[ "$t" =~ ^\[Planning[[:space:]] ]]; then
        return 0
    fi
    if [[ "$t" =~ ^note:[[:space:]]+(Building\ targets|Target\ dependency) ]]; then
        return 0
    fi
    case "$t" in
        cd|cd\ *)
            return 0 ;;
        CreateBuildDirectory*|WriteAuxiliaryFile*|MkDir*|CpHeader*|CopyPlistFile*|CopyStringsFile*|CopyTiffFile*|CopyPngFile*|PhaseScriptExecution*|ExecuteExternalTool*|CreateUniversalBinary*|ProcessProductPackaging*|RegisterWithLaunchServices*|RegisterExecutionPolicyException*|SetOwnerAndGroup*|SetMode*|ClangStatCache*|SwiftMergeGeneratedHeaders*|ConstructStubExecutorLinkFileList*|PruneExplicitPrecompiledModules*|PrecompileModule*|ScanDependencies*|SwiftDriverJobDiscovery*|ProcessProductPackagingDER*|SwiftExplicitDependencyGeneratePcm*|SwiftExplicitPrecompiledModule*|SwiftExplicit*|CompileAssetCatalog*|GenerateAssetSymbols*|LinkAssetCatalog*)
            return 0 ;;
        "Command line invocation:"|"Build settings from command line:"|"Resolve Package Graph"|"Resolved source packages:"|"ComputePackagePrebuildTargetDependencyGraph"|"Prepare packages"|"CreateBuildRequest"|"SendProjectDescription"|"CreateBuildOperation"|"ComputeTargetDependencyGraph"|"GatherProvisioningInputs"|"CreateBuildDescription")
            return 0 ;;
        Building\ project\ *)
            return 0 ;;
        CODE_SIGNING_ALLOWED*|ONLY_ACTIVE_ARCH*)
            return 0 ;;
        write-file\ *|builtin-*|Copy\ *|construct-stub-executor-link-file-list\ *)
            return 0 ;;
        Ignoring\ --strip-bitcode*|Starting\ appintentsmetadataprocessor*|Extracted\ no\ relevant*|Writing\ Metadata.appintents|Metadata\ root:*)
            return 0 ;;
    esac
    [[ "$t" == "Build description signature:"* ]] && return 0
    [[ "$t" == "Build description path:"* ]] && return 0
    [[ "$t" == PraxisUI:* ]] && return 0
    [[ "$t" == \{*platform:* ]] && return 0
    [[ "$t" == Target\ * ]] && return 0
    [[ "$t" == ➜* ]] && return 0
    [[ "$t" == \[*\]:* ]] && return 0
    [[ "$t" == http* ]] && return 0
    [[ "$t" == *appintentsmetadataprocessor* ]] && return 0
    [[ "$t" == *lsregister* ]] && return 0
    [[ "$t" == *libPreviewsJIT* ]] && return 0
    return 1
}

_ui_diag_count=0
_ui_in_log=0
_ui_skip_next=0
_ui_in_failing_list=0
_ui_test_summary=0
_ui_tests_passed=0
_ui_doctest_cases=""
_ui_doctest_loc=""
_ui_doctest_msg=""
_ui_last_action=""
_ui_tail=()
UI_TAIL_MAX="${UI_TAIL_MAX:-15}"

# A line logged by the process being built or tested (NSLog, os_log,
# xcodebuild's own [MT] lines), and the indented or bracketed lines that
# continue it: "2026-09-23 19:34:55.357274+0300 App[70256:614311] ..."
_ui_is_runtime_log() {
    local raw="$1" t="$2"
    local re='^[0-9]{4}-[0-9]{2}-[0-9]{2}[ T][0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?([+-][0-9]{2}:?[0-9]{2}|Z)?[[:space:]]+[^[:space:]]+\[[0-9]+(:[0-9a-fx]+)?\]'
    if [[ "$t" =~ $re ]]; then
        _ui_in_log=1
        return 0
    fi
    if ((_ui_in_log)); then
        if [[ "$raw" =~ ^[[:space:]] ]] || [[ "$t" == [\}\)\]]* ]]; then
            return 0
        fi
        _ui_in_log=0
    fi
    return 1
}

# Swift Testing starts each line with an SF Symbol from the private use
# planes (U+F0000 and up; lead byte F3 or F4 when the locale is C).
_ui_is_symbol_line() {
    local c
    [[ "$1" == ?" "* || "$1" == ??" "* || "$1" == ???" "* || "$1" == ????" "* ]] || return 1
    printf -v c '%d' "'${1:0:1}" 2>/dev/null || return 1
    (( c >= 983040 || c == 243 || c == 244 ))
}

# " with 1 argument colorMode → .grayscale" → "colorMode → .grayscale"
_ui_test_args() {
    local a="$1"
    a="${a# with }"
    if [[ "$a" =~ ^[0-9]+[[:space:]]+arguments?[[:space:]]+(.*)$ ]]; then
        a="${BASH_REMATCH[1]}"
    fi
    printf '%s' "$a"
}

_ui_test_progress() {
    local name="$1"
    ((UI_LIVE)) || return 0
    name="${name%%(*}"
    _ui_live_set 0 0 "${_ui_tests_passed} passed · ${name}"
}

# One doctest failure: the ERROR line, plus its values line when one follows.
_ui_doctest_flush() {
    [[ -n "$_ui_doctest_msg" ]] || return 0
    _ui_emit_diag "error" "$_ui_doctest_loc" "$_ui_doctest_msg"
    _ui_doctest_loc=""
    _ui_doctest_msg=""
}

# doctest console output. The test binary runs directly, since ctest hides
# passing output. Returns 0 if the line was one.
_ui_try_doctest_event() {
    local t="$1" issues line
    if [[ -n "$_ui_doctest_msg" && "$t" =~ ^values:\ (.*)$ ]]; then
        _ui_doctest_msg="$_ui_doctest_msg; values: ${BASH_REMATCH[1]}"
        _ui_doctest_flush
        return 0
    fi
    if [[ "$t" =~ ^(.+:[0-9]+):\ (FATAL\ )?ERROR:\ (.*)$ ]]; then
        _ui_doctest_flush
        _ui_doctest_loc="${BASH_REMATCH[1]}"
        _ui_doctest_msg="${BASH_REMATCH[3]}"
        return 0
    fi
    _ui_doctest_flush
    if [[ "$t" =~ ^\[doctest\]\ test\ cases:\ +([0-9]+)\ \|\ +([0-9]+)\ passed ]]; then
        _ui_doctest_cases="${BASH_REMATCH[1]}"
        _ui_tests_passed="${BASH_REMATCH[2]}"
        return 0
    fi
    if [[ "$t" =~ ^\[doctest\]\ assertions:\ +[0-9]+\ \|\ +[0-9]+\ passed\ \|\ +([0-9]+)\ failed ]]; then
        issues="${BASH_REMATCH[1]}"
        _ui_test_summary=1
        if ((issues > 0)); then
            _ui_emit_diag "error" "" "$issues issue$([[ "$issues" == 1 ]] || echo s) in ${_ui_doctest_cases:-?} tests"
        else
            ((UI_LIVE)) && _ui_live_end
            printf -v line '  %s✓%s %s tests passed' "$UI_GREEN$UI_BOLD" "$UI_RESET" "$_ui_tests_passed"
            _ui_sticky "$line"
        fi
        return 0
    fi
    # Banners, separators, TEST CASE headers, and the bare file:line: above them.
    [[ "$t" == "[doctest]"* || "$t" =~ ^=+$ || "$t" =~ ^TEST\ (CASE|SUITE): || "$t" =~ ^[^[:space:]]+:[0-9]+:$ ]]
}

# Swift Testing and XCTest events. Returns 0 if the line was one.
_ui_try_test_event() {
    local t="$1"
    local rest name args issues

    if ((_ui_skip_next)); then
        _ui_skip_next=0
        return 0
    fi
    _ui_try_doctest_event "$t" && return 0
    case "$t" in
        "Testing started")
            return 0 ;;
        "Test session results, code coverage, and logs:"|"Writing result bundle at path:")
            _ui_skip_next=1
            return 0 ;;
        "Failing tests:")
            _ui_in_failing_list=1
            return 0 ;;
    esac
    if ((_ui_in_failing_list)); then
        [[ "$t" == "**"* ]] || return 0
        _ui_in_failing_list=0
    fi

    # XCTest. Its failures are file:line: error: lines, parsed as diagnostics.
    if [[ "$t" =~ ^Test\ Suite\ \'.*\'\ (started|passed|failed)\ at ]] \
        || [[ "$t" =~ ^Executed\ [0-9]+\ tests?,\ with ]]; then
        return 0
    fi
    if [[ "$t" =~ ^Test\ Case\ \'(.*)\'\ (started|passed|failed|skipped) ]]; then
        [[ "${BASH_REMATCH[2]}" == passed ]] && _ui_tests_passed=$((_ui_tests_passed + 1))
        _ui_test_progress "${BASH_REMATCH[1]}"
        return 0
    fi

    _ui_is_symbol_line "$t" || return 1
    rest="${t#* }"

    if [[ "$rest" =~ ^(Test|Suite)\ (.+)\ recorded\ an\ issue(.*)\ at\ ([^[:space:]]+:[0-9]+:[0-9]+):\ (.*)$ ]]; then
        name="${BASH_REMATCH[2]}"
        args="$(_ui_test_args "${BASH_REMATCH[3]}")"
        [[ -n "$args" ]] && name="$name [$args]"
        _ui_emit_diag "error" "${BASH_REMATCH[4]}" "$name: ${BASH_REMATCH[5]}"
        return 0
    fi
    if [[ "$rest" =~ ^(Test|Suite)\ (.+)\ recorded\ an\ issue(.*):\ (.*)$ ]]; then
        name="${BASH_REMATCH[2]}"
        args="$(_ui_test_args "${BASH_REMATCH[3]}")"
        [[ -n "$args" ]] && name="$name [$args]"
        _ui_emit_diag "error" "" "$name: ${BASH_REMATCH[4]}"
        return 0
    fi
    if [[ "$rest" =~ ^Test\ run\ with\ ([0-9]+)\ tests?.*\ failed\ after\ .*\ with\ ([0-9]+)\ issues?\.?$ ]]; then
        _ui_test_summary=1
        issues="${BASH_REMATCH[2]}"
        _ui_emit_diag "error" "" "$issues issue$([[ "$issues" == 1 ]] || echo s) in ${BASH_REMATCH[1]} tests"
        return 0
    fi
    if [[ "$rest" =~ ^Test\ run\ with ]]; then
        ((UI_LIVE)) && _ui_live_end
        return 0
    fi
    if [[ "$rest" =~ ^Test\ (.+)\ passed\ after ]]; then
        _ui_tests_passed=$((_ui_tests_passed + 1))
        _ui_test_progress "${BASH_REMATCH[1]}"
        return 0
    fi
    if [[ "$rest" =~ ^Test\ (.+)\ started\.$ ]]; then
        _ui_test_progress "${BASH_REMATCH[1]}"
        return 0
    fi
    # Suites, skips, failure echoes, library version, doc comments.
    return 0
}

# A line no rule matched. UI_VERBOSE=1 prints it; otherwise it only keeps
# the spinner going and is remembered for the tail of a silent failure.
_ui_unknown_line() {
    local line="$1"
    if _ui_env_on "${UI_VERBOSE-}"; then
        _ui_sticky "$(_ui_sticky_colorize "$line")"
        return 0
    fi
    _ui_tail+=("$(_ui_trim "$line")")
    if (( ${#_ui_tail[@]} > UI_TAIL_MAX )); then
        _ui_tail=("${_ui_tail[@]:1}")
    fi
    if (( UI_LIVE )) && ! ((_ui_live)); then
        _ui_live_set 0 0 "working"
    fi
}

# What ui_run reads after the pipe: the diagnostic count, then the tail.
_ui_write_state() {
    [[ -n "${UI_STATE_FILE:-}" ]] || return 0
    {
        printf '%s\n' "$_ui_diag_count"
        if ((${#_ui_tail[@]})); then
            printf '%s\n' "${_ui_tail[@]}"
        fi
    } >"$UI_STATE_FILE"
}

_ui_handle_build_line() {
    local raw="$1"
    local line trimmed n total rest crate
    line="$(_ui_prep_line "$(ui_shorten_text "$(_ui_strip_ansi "$raw")")")"
    trimmed="$(_ui_trim "$line")"
    [[ -z "$trimmed" ]] && return 0

    if _ui_is_runtime_log "$raw" "$trimmed"; then
        return 0
    fi
    if _ui_try_test_event "$trimmed"; then
        return 0
    fi

    if _ui_is_compiler_argv "$trimmed"; then
        return 0
    fi
    _ui_eat_argv=0

    if _ui_is_diag_spam "$trimmed"; then
        if [[ "$trimmed" == Failed\ frontend* ]] \
            || [[ "$trimmed" == Command\ line:* ]] \
            || [[ "$trimmed" == *swift-frontend* ]] \
            || [[ "$trimmed" == error:\ SwiftCompile* ]]; then
            _ui_eat_argv=1
        fi
        return 0
    fi

    if _ui_try_diag "$trimmed"; then
        return 0
    fi

    _ui_flush_diag

    if _ui_is_chatter "$trimmed"; then
        if (( UI_LIVE )); then
            local live_label="${_ui_live_label:-planning}"
            case "$trimmed" in
                SwiftExplicit*|PrecompileModule*|PruneExplicitPrecompiledModules*)
                    live_label="modules" ;;
            esac
            _ui_live_set "$_ui_live_n" "$_ui_live_total" "$live_label"
        fi
        return 0
    fi

    local marker
    if marker="$(_ui_marker_line "$trimmed")"; then
        _ui_sticky "$marker"
        return 0
    fi

    # SwiftPM "Build complete!" is useful in NO_LIVE logs. xcodebuild's
    # ** BUILD SUCCEEDED ** is redundant next to ui_step_done.
    if [[ "$trimmed" =~ ^\*\*\ [A-Z\ ]+\ SUCCEEDED\ \*\*$ ]]; then
        if (( UI_LIVE )); then
            _ui_live_end
        fi
        return 0
    fi
    if [[ "$trimmed" =~ ^Build\ complete! ]]; then
        if (( UI_LIVE )); then
            _ui_live_end
        else
            ui_colorize_build_line "$line"
        fi
        return 0
    fi

    if [[ "$trimmed" =~ ^\*\*\ ([A-Z\ ]+\ FAILED)\ \*\*$ ]]; then
        # A failed test run has already said so, with its issue count.
        if [[ "$trimmed" != *BUILD* ]] && ((_ui_test_summary)); then
            return 0
        fi
        _ui_emit_diag "error" "" "${BASH_REMATCH[1]}"
        return 0
    fi

    # Cargo finished; same, the step check is enough.
    if [[ "$trimmed" =~ ^Finished[[:space:]] ]]; then
        if (( UI_LIVE )); then
            _ui_live_end
        else
            ui_colorize_build_line "$line"
        fi
        return 0
    fi

    # xcodebuild action lines: CompileSwift …/Foo.swift  /  Ld …/App
    if [[ "$trimmed" =~ ^(CompileSwift|SwiftCompile|CompileC|CompileXCStrings|SwiftDriver|Ld|Linking|CodeSign|Touch|Validate|CopySwiftLibs|ProcessInfoPlistFile|GenerateDSYMFile|Strip|SetOwnerAndGroup|SetMode|RegisterWithLaunchServices|RegisterExecutionPolicyException|ExtractAppIntentsMetadata|AppIntentsSSUTraining|SwiftEmitModule|SwiftDriver\ Job) ]]; then
        local xlabel
        xlabel="$(_ui_pretty_xcode_label "$trimmed")"
        if (( UI_LIVE )); then
            _ui_live_set "$_ui_live_n" "$_ui_live_total" "$xlabel"
        elif [[ "$xlabel" != "$_ui_last_action" ]]; then
            printf '  %s%s%s\n' "$UI_DIM" "$xlabel" "$UI_RESET"
        fi
        _ui_last_action="$xlabel"
        return 0
    fi

    if [[ "$trimmed" =~ ^(Created\ project\ at|⚙️) ]]; then
        if (( UI_LIVE )); then
            _ui_live_set 0 0 "xcodegen"
        else
            printf '  %s%s%s\n' "$UI_DIM" "$(ui_shorten_text "$trimmed")" "$UI_RESET"
        fi
        return 0
    fi

    # Swift progress: [3/12] Compiling Foo.swift
    if [[ "$trimmed" =~ ^\[([0-9]+)[[:space:]]*/[[:space:]]*([0-9]+)\](.*)$ ]]; then
        n="${BASH_REMATCH[1]}"
        total="${BASH_REMATCH[2]}"
        rest="$(_ui_spm_counter_label "$n" "$total" "${BASH_REMATCH[3]}")"
        if (( UI_LIVE )); then
            _ui_live_set "$n" "$total" "$rest"
        else
            ui_colorize_build_line "$line"
        fi
        return 0
    fi

    # Cargo crate ticks.
    if [[ "$trimmed" =~ ^(Compiling|Checking|Downloading|Downloaded|Updating)[[:space:]]+([^[:space:]]+) ]]; then
        crate="${BASH_REMATCH[2]}"
        if (( UI_LIVE )); then
            _ui_live_set "$_ui_live_n" "$_ui_live_total" "${BASH_REMATCH[1]} $crate"
        else
            ui_colorize_build_line "$line"
        fi
        return 0
    fi

    if [[ "$trimmed" =~ ^(unchanged|wrote)[[:space:]]+(.*)$ ]]; then
        if (( UI_LIVE )); then
            _ui_live_set 0 0 "${BASH_REMATCH[1]} ${BASH_REMATCH[2]##*/}"
            return 0
        fi
        ui_colorize_build_line "$line"
        return 0
    fi

    _ui_unknown_line "$line"
}

# Filter stdin → stdout. On a TTY, progress overwrites one line; diagnostics stay.
ui_filter_build() {
    local line st
    # Timeouts from `read -t` must not trip `set -e` in the caller.
    set +e
    trap '_ui_flush_diag; _ui_live_end; ui_progress_close; _ui_write_state' EXIT
    trap '_ui_flush_diag; _ui_live_end; ui_progress_close; exit 130' INT TERM
    if (( UI_LIVE )); then
        printf '\033[?25l'
        _ui_cursor_hidden=1
    fi
    if (( UI_LIVE )) && (( BASH_VERSINFO[0] >= 4 )); then
        while true; do
            IFS= read -r -t 0.08 line
            st=$?
            if (( st == 0 )); then
                _ui_handle_build_line "$line"
            elif (( st > 128 )); then
                if ((_ui_live)); then
                    _ui_spin_i=$(( (_ui_spin_i + 1) % ${#_UI_SPIN[@]} ))
                    _ui_live_paint
                fi
            else
                [[ -n "${line:-}" ]] && _ui_handle_build_line "$line"
                break
            fi
        done
    else
        while IFS= read -r line || [[ -n "$line" ]]; do
            _ui_handle_build_line "$line"
        done
    fi
    _ui_flush_diag
    _ui_live_end
    ui_progress_close
}

# Where ui_run keeps the raw output: UI_LOG_FILE, or one file per repo.
ui_log_path() {
    local name
    name="$(basename "${UI_REPO_ROOT:-$PWD}")"
    printf '%s' "${UI_LOG_FILE:-${TMPDIR:-/tmp}/cli-ui-${name// /-}.log}" | sed 's://*:/:g'
}

# Run a command with polished stdout+stderr. Preserves exit status.
# The raw output is kept at $UI_LAST_LOG. When the command fails without a
# diagnostic, the last unrecognized lines are printed so the cause shows.
ui_run() {
    local ec had_pipefail=0 state count line
    # Compilers write to a pipe here; keep their text plain so we can parse
    # diagnostics. Color is added by ui_filter_build on our TTY.
    export CARGO_TERM_COLOR="${CARGO_TERM_COLOR:-never}"
    UI_LAST_LOG="$(ui_log_path)"
    export UI_LAST_LOG
    state="$(mktemp "${TMPDIR:-/tmp}/cli-ui-state.XXXXXX")"
    [[ -o pipefail ]] && had_pipefail=1
    set +e
    set +o pipefail
    if command -v stdbuf >/dev/null 2>&1; then
        stdbuf -oL -eL "$@" 2>&1 | tee "$UI_LAST_LOG" | UI_STATE_FILE="$state" ui_filter_build
    else
        "$@" 2>&1 | tee "$UI_LAST_LOG" | UI_STATE_FILE="$state" ui_filter_build
    fi
    ec=${PIPESTATUS[0]}
    if (( ec != 0 )); then
        count="$(head -n 1 "$state" 2>/dev/null)"
        if [[ "${count:-0}" == 0 ]] && (( $(wc -l <"$state") > 1 )); then
            ui_err "$(basename "$1") exited with code $ec. Its last lines:"
            while IFS= read -r line; do
                printf '    %s%s%s\n' "$UI_DIM" "$line" "$UI_RESET"
            done < <(tail -n +2 "$state")
        fi
        ui_detail "Full output: $(ui_shorten_text "$UI_LAST_LOG")"
    fi
    rm -f "$state"
    (( had_pipefail )) && set -o pipefail
    set -e
    return "$ec"
}

_UI_SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$_UI_SELF_DIR/cli-ui.local.sh" ]]; then
    # shellcheck disable=SC1091
    source "$_UI_SELF_DIR/cli-ui.local.sh"
fi
