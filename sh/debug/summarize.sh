#!/system/bin/sh

# Summarises a SandboxID debug session log.
#
# Every grep below is matched against strings the module actually emits — see
# the LOG[DIWE] call sites in jni/module_hooks.cpp, jni/prop_hooks.cpp and
# jni/companion.cpp. A previous revision grepped for markers that the code
# never writes ('CRASH [', 'DEATH target', 'L7 SPB ... [SPOOF]', 'bind OK:'),
# so every section reported 0 and the two biggest layer failures in the field
# (layer-2 bind-mounts dead, a pltHookCommit batch-false while the hooks were
# live) were invisible.

IN="$1"
OUT="${2:-/proc/self/fd/1}"

if [ -z "$IN" ] || [ ! -f "$IN" ]; then
    echo "usage: $0 <session-log> [output-file]" >&2
    exit 1
fi

# Only the module's own lines carry our markers; tombstones (F libc / F DEBUG)
# are kept separate so an unrelated system crash cannot pad our counts.
# LOGD lines carry a literal "[D] " prefix (module_impl.hpp); LOGI/W/E do not,
# so the bracket group is optional. The alternation MUST be parenthesised —
# ERE alternation binds looser than concatenation, so an unparenthesised
# "A \|B foo" also matches a bare "foo" on any other tag's line.
MOD='(SandboxID|SandboxIDCompanion): (\[.\] )?'

TOTAL_LINES=$(wc -l < "$IN")
TOTAL_SIZE=$(du -h "$IN" | cut -f1)

# Layers, in the order the module installs them (module.cpp postAppSpecialize).
count() { grep -cE "${MOD}$1" "$IN"; }    # count of matching module lines

{
    echo "==============================================================="
    echo "SandboxID — session log summary"
    echo "Source  : $(basename "$IN")"
    echo "Size    : $TOTAL_SIZE ($TOTAL_LINES lines)"
    echo "Digest  : $(date '+%Y-%m-%d %H:%M:%S %Z')"
    echo "==============================================================="
    echo ""

    echo "--- Session header ---"
    head -15 "$IN" | grep -vE '^[[:space:]]*$'
    echo ""

    # ------------------------------------------------ targets
    echo "--- Target whitelist ---"
    TL=$(grep -E "${MOD}target.txt loaded" "$IN" | tail -1)
    if [ -n "$TL" ]; then
        echo "  $(echo "$TL" | sed -E 's/^.*SandboxIDCompanion: //')"
    else
        echo "  (target.txt never loaded in this session)"
    fi
    echo ""

    echo "--- Process injections ---"
    printf "  %-28s : %s\n" "zygote forks seen"      "$(count 'preAppSpecialize pkg=' )"
    printf "  %-28s : %s\n" "companion handshakes"   "$(count 'companion invoked:')"
    printf "  %-28s : %s\n" "REJECT (not in target)" "$(count 'REJECT pkg=')"
    printf "  %-28s : %s\n" "ACCEPT (companion side)" "$(count 'ACCEPT pkg=')"
    printf "  %-28s : %s\n" "targets activated"      "$(count 'target active ')"
    echo ""
    echo "  Packages spawned (top 20, incl. rejected):"
    grep -oE "${MOD}preAppSpecialize pkg='[^']+'" "$IN" \
        | sed -E "s/.*preAppSpecialize pkg='([^']+)'/\1/" \
        | sort | uniq -c | sort -rn | head -20 \
        | awk '{printf "    %-6s %s\n", $1"x", $2}'
    echo ""
    echo "  Active targets (spoofing actually running):"
    grep -oE "${MOD}target pkg='[^']+'" "$IN" \
        | sed -E "s/.*target pkg='([^']+)'/\1/" \
        | sort | uniq -c | sort -rn \
        | awk '{printf "    %-6s %s\n", $1"x", $2}'
    echo ""
    echo ""

    # ------------------------------------------------ layer 1: Java + props
    echo "--- Layer 1: Build.* reflection + SystemProperties ---"
    printf "  %-28s : %s\n" "BUILD fields spoofed (runs)" "$(count 'BUILD hook: ')"
    BH=$(grep -oE "${MOD}BUILD hook: [0-9]+" "$IN" | grep -oE '[0-9]+$' | awk '{s+=$1} END{print s+0}')
    printf "  %-28s : %s\n" "total Build fields spoofed"  "$BH"
    printf "  %-28s : %s\n" "PROP native_get calls"       "$(count 'PROP native_get\(')"
    printf "  %-28s : %s\n" "prop values spoofed"         "$(count 'PROP SPOOF ')"
    printf "  %-28s : %s\n" "prop values hidden (absent)" "$(count 'PROP HIDE ')"
    # The trailing quote anchors the match: spoof lines are "TYPED SPB 'key'",
    # suppress lines "TYPED SPB SUPPRESS 'key'" and fallback lines
    # "TYPED SPI(id) 'key'" (jni/typed_props.cpp), so a bare trailing space
    # matched spoofs AND suppressions, inflating this row by the suppress count.
    # The group also keeps every alternative under the ${MOD} tag anchor - in
    # the unparenthesised form only the first alternative was anchored and the
    # other two matched anywhere in the log.
    printf "  %-28s : %s\n" "typed get spoofed (SPB/SPI/SPL)" \
        "$(count "TYPED (SPB|SPI|SPL) '")"
    printf "  %-28s : %s\n" "typed get suppressed"        "$(count 'TYPED .*SUPPRESS ')"
    printf "  %-28s : %s\n" "leak sensors installed"      "$(count 'TYPED leak sensors installed')"
    echo ""
    echo "  Top 25 spoofed keys:"
    grep -oE "${MOD}(PROP SPOOF|NATIVE_PROP_GET SPOOF) '[^']+'" "$IN" \
        | sed -E "s/.*SPOOF '//; s/'$//" \
        | sort | uniq -c | sort -rn | head -25 \
        | awk '{n=$1; $1=""; sub(/^ /,""); printf "    %5dx  %s\n", n, $0}'
    echo ""
    echo "  Top 25 typed-get keys (getInt/getBoolean/getLong):"
    grep -oE "${MOD}TYPED (SPB|SPI|SPL) '[^']+'" "$IN" \
        | sed -E "s/.*TYPED (SPB|SPI|SPL) '//; s/'$//" \
        | sort | uniq -c | sort -rn | head -25 \
        | awk '{n=$1; $1=""; sub(/^ /,""); printf "    %5dx  %s\n", n, $0}'
    echo ""
    echo "  Hidden keys (persona must not answer):"
    grep -oE "${MOD}(PROP HIDE|NATIVE_PROP_GET HIDE|NATIVE_PROP HIDE) '[^']+'" "$IN" \
        | sed -E "s/.*HIDE '//; s/' .*//; s/'$//" | sort | uniq -c | sort -rn \
        | awk '{n=$1; $1=""; sub(/^ /,""); printf "    %5dx  %s\n", n, $0}'
    echo ""

    # ------------------------------------------------ layer 9: in-process PLT
    echo "--- Layer 9: in-process PLT hooks (native reads) ---"
    NR_OK=$(count 'NATIVE_READ hooks installed')
    NR_FAIL=$(count 'NATIVE_READ: pltHookCommit failed')
    NR_NOBS=$(count 'NATIVE_READ: no hookable libs')
    # The install line's own parenthesised summary contains parens ("lib(s)",
    # "registration(s)"), so a \([^)]*\) span stops early and loses the count.
    # Select the line, then pull "N live trampoline(s)" off it directly.
    NR_LIVE=$(grep -E "${MOD}NATIVE_READ hooks installed" "$IN" \
        | grep -oE '[0-9]+ live trampoline\(s\)' | grep -oE '[0-9]+' \
        | awk '{s+=$1} END{print s+0}')
    NR_TROLL=$(count 'NATIVE_READ: pltHookCommit\(\) reported failure')
    NPG=$(count 'NATIVE_PROP_GET SPOOF ')
    printf "  %-28s : %s\n" "installations reported OK"   "$NR_OK"
    printf "  %-28s : %s\n" "live trampolines (sum)"       "$NR_LIVE"
    printf "  %-28s : %s\n" "commit reported failure"      "$NR_FAIL"
    printf "  %-28s : %s\n" "  ...but trampolines landed"  "$NR_TROLL"
    printf "  %-28s : %s\n" "no hookable libs"             "$NR_NOBS"
    printf "  %-28s : %s\n" "native prop reads spoofed"    "$NPG"
    echo ""
    # The file hooks log nothing per read (they would flood), so liveness is
    # judged from what the install path itself observed.
    #
    # The post-commit probe that re-reads our own orig_* pointers is ground
    # truth, not a provider return code. Its LOGW ("pltHookCommit() reported
    # failure but N trampoline(s) landed") falls through to the unconditional
    # install LOGI, so a session with that LOGW ALWAYS also has the install
    # line. Testing the install line first therefore always won with ':' and
    # this NOTE could never print - the exact false-negative session it was
    # written for (commit "failing" 8/8 while 348 reads were spoofed through
    # those same hooks) reported a clean silent verdict. Troll must come first.
    if [ "$NR_TROLL" -gt 0 ]; then
        echo "  NOTE: pltHookCommit() reported failure $NR_TROLL time(s), but the"
        echo "  post-commit probe found live trampolines each time, so the hooks ARE"
        echo "  installed. The provider's bool is a batch verdict over EVERY"
        echo "  registration - Magisk folds all GOT slots into one AND via"
        echo "  lsplt's HookInfos::DoHook, ReZygisk returns !any_failed - so one"
        echo "  symbol a library does not import flips it false while the rest"
        echo "  landed. L9 is LIVE; 'pltHookCommit failed' is a partial-failure"
        echo "  report, not a false negative and not a dead layer."
    elif [ "$NR_OK" -gt 0 ]; then
        :   # the install path confirmed liveness
    elif [ "$NPG" -gt 0 ]; then
        echo "  WARNING: pltHookCommit() returned false every time, yet $NPG native"
        echo "  prop reads were served with spoofed values through those same hooks."
        echo "  The provider's bool is a batch verdict over every registration, not"
        echo "  a per-hook report, so it cannot tell these two apart. L9 is LIVE."
        echo "  See the NOTE above for why a batch false is not a dead layer."
    elif [ "$NR_FAIL" -gt 0 ]; then
        echo "  WARNING: L9 is OFF - pltHookCommit failed, no trampolines landed and"
        echo "  no native read was ever spoofed. Only Build.* and SystemProperties"
        echo "  are spoofed in those processes."
    fi
    echo ""

    # ------------------------------------------------ layer 2: companion mounts
    echo "--- Layer 2: companion bind-mounts (on-disk build.prop) ---"
    # Both branches are logged by jni/module.cpp (the true branch is LOGD, so
    # it appears only in a debug capture). The old EXOK inverted this with
    # grep -cv, which is provably 0: the only line matching 'exemptFd(fd=' was
    # the failure branch, so every match also contained 'returned false'.
    EXF=$(grep -E "${MOD}exemptFd\(fd=" "$IN" | grep -c 'returned false')
    EXOK=$(grep -E "${MOD}exemptFd\(fd=" "$IN" | grep -c 'returned true')
    SKIP=$(count 'layer-2 bind-mounts skipped')
    M_OK=$(count 'MOUNTS: companion applied')
    M_FAIL=$(count 'MOUNTS: failed to')
    H_OK=$(count 'HIDE: companion detached')
    H_FAIL=$(count 'HIDE: failed to')
    printf "  %-28s : %s\n" "exemptFd() returned false"   "$EXF"
    printf "  %-28s : %s\n" "exemptFd() returned true"    "$EXOK"
    printf "  %-28s : %s\n" "socket EOF, mount skipped"    "$SKIP"
    printf "  %-28s : %s\n" "mount request OK"            "$M_OK"
    printf "  %-28s : %s\n" "mount request FAILED"        "$M_FAIL"
    printf "  %-28s : %s\n" "hide request OK"             "$H_OK"
    printf "  %-28s : %s\n" "hide request FAILED"         "$H_FAIL"
    echo ""
    # 'mount pid=N: A ok, B fail, C skip' — the companion's per-target result.
    grep -oE "${MOD}mount pid=[0-9]+: [0-9]+ ok, [0-9]+ fail, [0-9]+ skip" "$IN" \
        | sed -E 's/^.*pid=([0-9]+): /pid \1: /' | head -15 | while IFS= read -r r; do
            printf "    %s\n" "$r"
        done
    BIND_OK=$(grep -oE "${MOD}mount pid=[0-9]+: [0-9]+ ok" "$IN" \
        | grep -oE '[0-9]+ ok' | grep -oE '^[0-9]+' | awk '{s+=$1} END{print s+0}')
    echo ""
    printf "  %-28s : %s\n" "total bind-mounts applied"   "$BIND_OK"
    echo ""
    if [ "$M_FAIL" -gt 0 ] && [ "$M_OK" -eq 0 ]; then
        echo "  WARNING: layer 2 is DEAD - every mount request failed and zero"
        echo "  bind-mounts were applied. On-disk build.prop readers inside the"
        echo "  target see the REAL device. If exemptFd() also returned false, the"
        echo "  zygote reaped the companion socket during app specialization; the"
        echo "  zygisk provider either does not support exemptFd or (ReZygisk)"
        echo "  declares it void so its bool return is garbage. In-process layers"
        echo "  (Build.*, SystemProperties, L9 reads) are unaffected."
    elif [ "$EXF" -gt 0 ] && [ "$M_OK" -gt 0 ]; then
        echo "  NOTE: exemptFd() reported false but the companion socket survived"
        echo "  and mounts were applied - the return is a false negative (ReZygisk"
        echo "  declares the slot void; the socket probe is what mattered)."
    fi
    echo ""

    # ------------------------------------------------ watchdog
    echo "--- Crash watchdog ---"
    WD=$(count 'crash watchdog armed for')
    printf "  %-28s : %s\n" "watchdogs armed"             "$WD"
    # The handler writes its marker through liblog (ANDROID_LOG_FATAL) as well
    # as write(2): second-stage init dup2()s fds 0/1/2 to /dev/null, so a bare
    # write(2) never reaches logcat in an app process. If this stays 0 while
    # tombstones below are non-zero, the crash was in a process the watchdog
    # had not armed (a rejected package), or in code that never returned here.
    OWN=$(count 'SandboxID CRASH sig=')
    printf "  %-28s : %s\n" "module-marked crashes"       "$OWN"
    # The watcher (companion.cpp watch_target_death) deliberately logs nothing
    # when a target dies - it only reaps - so there is no death event to count.
    echo ""

    # ------------------------------------------------ tombstones
    echo "--- Native tombstones in the log (any process, not just targets) ---"
    # logcat's 'beginning of crash' appears once per crash buffer, not per
    # tombstone; the *** *** *** banner opens each individual tombstone.
    TB=$(grep -cF -- '*** *** *** *** *** *** ***' "$IN")
    printf "  %-28s : %s\n" "tombstones" "$TB"
    grep -E 'F libc' "$IN" | grep -oE 'Fatal signal [0-9]+ \([A-Z]+\)' \
        | sort | uniq -c | sort -rn | head -5 \
        | awk '{n=$1; $1=""; sub(/^ /,""); printf "    %-6s %s\n", n"x", $0}'
    echo "  Crashing packages:"
    grep -E 'F DEBUG   : Cmdline: ' "$IN" | sed -E 's/.*Cmdline: //' \
        | sort | uniq -c | sort -rn | head -10 \
        | awk '{printf "    %-6s %s\n", $1"x", $2}'
    echo ""
    echo "  Abort messages (addresses/tids normalised):"
    grep -E 'F DEBUG   : Abort message: ' "$IN" | sed -E 's/.*Abort message: //' \
        | sed -E "s/0x[0-9a-fA-F]+/0xN/g; s/tid=[0-9]+/tid=N/g; s/Thread\[[0-9]+,/Thread[N,/" \
        | cut -c1-110 | sort | uniq -c | sort -rn | head -10 \
        | awk '{n=$1; $1=""; sub(/^ /,""); printf "    %5dx  %s\n", n, $0}'
    echo ""

    # ------------------------------------------------ java
    echo "--- Java FATAL EXCEPTION (first 15 stack lines) ---"
    FN=$(grep -c 'FATAL EXCEPTION' "$IN")
    if [ "$FN" -gt 0 ]; then
        awk '/FATAL EXCEPTION/{flag=15} flag>0{print "  " $0; flag--}' "$IN" | head -60
    else
        echo "  (none)"
    fi
    echo ""

    # ------------------------------------------------ companion errors
    echo "--- All module ERROR lines ---"
    EN=$(grep -cE "^[0-9]{2}-[0-9]{2} [0-9:.]+ +[0-9]+ +[0-9]+ E (SandboxID|SandboxIDCompanion): " "$IN")
    if [ "$EN" -gt 0 ]; then
        grep -E "^[0-9]{2}-[0-9]{2} [0-9:.]+ +[0-9]+ +[0-9]+ E (SandboxID|SandboxIDCompanion): " "$IN" \
            | sed -E "s/^[0-9]{2}-[0-9]{2} [0-9:.]+ +[0-9]+ +[0-9]+ E //" \
            | sort | uniq -c | sort -rn | head -20 \
            | awk '{n=$1; $1=""; sub(/^ /,""); printf "  %5dx  %s\n", n, $0}'
    else
        echo "  (none)"
    fi
    echo ""

    echo "--- Last 30 lines of session ---"
    tail -30 "$IN" | awk '{print "  " $0}'
    echo ""

    echo "==============================================================="
    echo "End of summary.  Full log lives next to this file:"
    echo "  $IN"
    echo "==============================================================="

} > "$OUT" 2>&1

if [ "$OUT" != "/proc/self/fd/1" ]; then
    echo "summary: $OUT ($(du -h "$OUT" | cut -f1), $(wc -l < "$OUT") lines)" >&2
fi
