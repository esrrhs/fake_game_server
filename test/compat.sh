#!/usr/bin/env bash
# Shared POSIX/bash portability helpers for the FakeCC test suites.
#
# The suites are written for a GNU/Linux host and lean on two coreutils that
# macOS does not ship: `timeout` and `nproc`.  Rather than sprinkle `command -v`
# guards through every runner, this file defines shell functions with the same
# names and semantics, and each runner sources it before use.
#
# `timeout` must survive `export -f` + `xargs bash -c`, because run_e2e.sh fans a
# case out to worker shells that only see exported functions.  So it is defined
# with no dependency on non-exportable shell state.
#
# Sourced, never executed.

# Number of parallel jobs to use.  Falls back through the tools macOS does have
# (sysctl) before resorting to a conservative constant.
nproc() {
    # `command -v` would find this very function and recurse, so probe for the
    # real binary with `type -P` (which skips shell functions).
    if type -P nproc >/dev/null 2>&1; then
        nproc
        return
    fi
    local n
    # hw.logicalcpu (hw.physicalcpu is cores, which under-uses the machine).
    n=$(sysctl -n hw.logicalcpu 2>/dev/null || true)
    case "$n" in
        ''|*[!0-9]*) n=4 ;;
    esac
    echo "$n"
}

# Run a command with a wall-clock limit, GNU `timeout` semantics.
#
#   timeout SECONDS CMD [ARG...]
#
# Exit status: CMD's own status when it finishes in time, 124 when the limit is
# hit, and 128+N when CMD is killed by signal N.  Callers in the suites rely on
# 124 to report "timed out" and on 13x to report "killed by a signal", so both
# distinctions are preserved rather than flattened to 124.
#
# GNU timeout also accepts suffixes (30s, 5m); accept them so a caller can pass
# either form.
timeout() {
    local spec="${1:-}"
    [ $# -gt 0 ] && shift

    # Normalize "30" / "30s" / "5m" / "1h" to whole seconds.
    local secs
    case "$spec" in
        ''|*[!0-9smh]*) secs="$spec" ;;
        *s) secs="${spec%s}" ;;
        *m) secs=$(( ${spec%m} * 60 )) ;;
        *h) secs=$(( ${spec%h} * 3600 )) ;;
        *)  secs="$spec" ;;
    esac
    case "$secs" in
        ''|*[!0-9]*)
            echo "timeout: invalid time interval '$spec'" >&2
            return 2
            ;;
    esac

    "$@" &
    local pid=$!

    # Poll for completion.  `wait` cannot be interrupted portably in bash 3.2
    # (the version macOS ships), so poll `kill -0` and then reap.
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$secs" ]; then
            kill -TERM "$pid" 2>/dev/null || true
            # Give it a moment to die politely, then insist.
            local grace=0
            while kill -0 "$pid" 2>/dev/null && [ "$grace" -lt 5 ]; do
                sleep 1
                grace=$(( grace + 1 ))
                kill -TERM "$pid" 2>/dev/null || true
            done
            kill -KILL "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null
            return 124
        fi
        sleep 1
        waited=$(( waited + 1 ))
    done

    # Finished on its own: propagate the real status, preserving 128+N.
    wait "$pid"
    return $?
}

# Read ELF dynamic tags.  Linux ships readelf; macOS does not, so use otool and
# dyld-oriented spellings instead.  Prints the same shape of lines the callers
# grep for, i.e. lines that contain a tag name and its value.
#
#   elf_dynamic_dump BINARY
elf_dynamic_dump() {
    local bin="$1"
    if command -v readelf >/dev/null 2>&1; then
        LANG=C readelf -d "$bin" 2>/dev/null
        return
    fi
    # Mach-O fallback: map the two tags the suites actually query onto otool's
    # install-name/weak-reference vocabulary.
    local install
    install=$(otool -l "$bin" 2>/dev/null) || return 0
    printf '%s\n' "$install" | awk '
        /^ *cmd LC_LOAD_DYLIB/ { want = 1; next }
        want && /^ *name / {
            line = $0
            sub(/^ *name /, "", line)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", line)
            printf "(NEEDED) %s\n", line
            want = 0
        }
    '
}

# True when the dynamic section of BINARY mentions SONAME.
has_needed() {
    local bin="$1" soname="$2"
    elf_dynamic_dump "$bin" | grep '(NEEDED)' | grep -F "[$soname]" >/dev/null
}

# True when the dynamic section of BINARY mentions a runpath containing NEEDLE.
has_runpath() {
    local bin="$1" needle="$2"
    elf_dynamic_dump "$bin" | grep '(RUNPATH)' | grep -F "$needle" >/dev/null
}

# ---------------------------------------------------------------------------
# Mach-O equivalents of the readelf queries the suites make.
#
# The ELF assertions are not merely unreadable off Linux -- leaving them there
# means the behaviour they were written to catch stops being checked at all,
# because a skipped assertion covers nothing.  So each one gets a Mach-O
# counterpart: same question, asked in the host's vocabulary.
#
# `macho_section_dump` lists sections as "name size align flags"; `macho_relocs`
# lists relocation records as "section index type symbol name".
# ---------------------------------------------------------------------------

is_elf_host() {
    command -v readelf >/dev/null 2>&1
}

macho_section_dump() {
    local bin="$1"
    [ -f "$bin" ] || return 1
    # Only the blocks that begin with a bare "Section" line; a load command's
    # own size/align/flags lines look the same and would otherwise be picked
    # up as if they described a section.
    otool -l "$bin" 2>/dev/null | awk '
        /^Section$/ { ins = 1; name = ""; seg = ""; size = ""; align = ""; fl = ""
                     next }
        !ins { next }
        /^ *sectname / { name = $2; next }
        /^ *segname /  { seg  = $2; next }
        /^ *size /     { size = $2; next }
        /^ *align /    { align = $2; sub(/^2\^/, "", align); next }
        # `flags` is the last field otool prints inside a section, so it can
        # close the record by itself.
        /^ *flags /    { fl = $2; sub(/^0x/, "", fl)
                         print seg "," name, size, align, fl; ins = 0; next }
    '
}

# True when BINARY has a section named NAME (with or without a segment prefix).
macho_has_section() {
    local bin="$1" name="$2"
    macho_section_dump "$bin" | awk -v n="$name" \
        '{ split($1, p, ","); if (p[2] == n) found = 1 } END { exit found ? 0 : 1 }'
}

# Print the alignment exponent of section NAME (Mach-O stores log2 of it).
macho_section_align_log() {
    local bin="$1" name="$2"
    macho_section_dump "$bin" | awk -v n="$name" \
        '{ split($1, p, ","); if (p[2] == n) { print $4; exit } }'
}

# List ARM64 relocation records as "section-index type symbolnum", one per line.
# otool heads each section with "Relocation information (SEG,SECT) N entries",
# which is what lets the section index be recovered; it prints the type as a
# decimal already, so no decoding is needed here.
macho_relocs() {
    local bin="$1"
    [ -f "$bin" ] || return 1
    otool -r "$bin" 2>/dev/null | awk '
        # Columns: address pcrel length extern type scattered symbolnum
        /^Relocation information/ { ++sect; next }
        NF >= 7 && $1 ~ /^[0-9a-f]+$/ && $2 ~ /^[01]$/ { print sect, $5, $7 }
    '
}

# True when BINARY has a relocation of type TYPE in section index SECTIDX.
# SECTIDX may be a name instead; then the index is looked up first.
macho_has_reloc_type() {
    local bin="$1" want="$2" sect="$3"
    local idx
    case "$sect" in
        ''|*[!0-9]*) idx=$(macho_section_index "$bin" "$sect") || return 1 ;;
        *) idx="$sect" ;;
    esac
    # macho_relocs prints "section type symbolnum".
    macho_relocs "$bin" | awk -v t="$want" -v s="$idx" '$1 == s && $2 == t { found = 1 } END { exit found ? 0 : 1 }'
}

# Print the 1-based section index that otool reports for section NAME.
macho_section_index() {
    local bin="$1" name="$2"
    macho_section_dump "$bin" | awk -v n="$name" '{ i++; split($1, p, ","); if (p[2] == n) { print i; found = 1; exit } } END { exit found ? 0 : 1 }'
}