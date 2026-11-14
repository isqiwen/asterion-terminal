# The part of setup.sh that Market, Warehouse and Trading share. The build
# appends it to each service's own scripts/setup.sh; neither runs alone.
#
# The service part sets NAME (as messages name it: Market), PACKAGE (the
# release's top directory and certificate name: asterion-market), ROOT, UNIT
# (its systemd unit file) and PROGRAMS (bin/ and lib/ files that place_programs
# replaces), and defines python (the interpreter that reads its settings),
# current KEY (a setting of what the questions are about), unit_text and setup
# SOURCE, which main calls with the unpacked release or, for the installed
# copy, ROOT. A service with one instance also sets SETTINGS (its settings
# file in ROOT) and TIMER (its pending restart, if it defers one), and defines
# configure, current_settings PATH (the current values written in this
# release's format) and apply for install_or_configure.

# A failed question inside $(...) ends the script, not only that subshell.
shopt -s inherit_errexit

UNITS=/etc/systemd/system
CREDENTIALS=/etc/credstore.encrypted
# The account the service runs as: the sudo user at install, the owner of
# config/ afterwards.
ACCOUNT=
# This script: the release with its archive, or the installed bin/setup.sh.
SELF=$(readlink -f "${BASH_SOURCE[0]}")

fail() {
    echo "setup: $*" >&2
    exit 1
}

# Files this run writes before moving them into place, and the unpacked
# release: removed however the script ends, so nothing half written stays.
CLEANUP=()
cleanup() {
    ((${#CLEANUP[@]})) && rm -rf "${CLEANUP[@]}"
    return 0
}
trap cleanup EXIT

# stage VARIABLE PATH: sets VARIABLE to a fresh name beside PATH for its next
# version, removed on exit unless moved into place. It runs in this shell, not
# in a command substitution, so the name reaches CLEANUP.
stage() {
    local file
    file=$(mktemp "$(dirname "$2")/.$(basename "$2").XXXXXX")
    CLEANUP+=("$file")
    printf -v "$1" '%s' "$file"
}

# seal VARIABLE PATH NAME VALUE: stages PATH as VARIABLE holding VALUE encrypted
# by systemd-creds under NAME; the caller moves it into place.
seal() {
    stage "$1" "$2"
    printf '%s' "$4" | systemd-creds encrypt --name="$3" - "${!1}" >/dev/null
}

# private FILE: only the service's account may read it.
private() {
    chown "$ACCOUNT:" "$1"
    chmod 0600 "$1"
}

# ask PROMPT DEFAULT PATTERN: reads a value matching PATTERN. At a terminal the
# default is already typed in, to accept with Enter or edit.
ask() {
    local value
    while true; do
        if [[ -t 0 ]]; then
            read -r -e -i "$2" -p "$1: " value || fail "Input ended"
        else
            read -r -p "$1 [$2]: " value || fail "Input ended"
        fi
        value=${value:-$2}
        if [[ $value =~ $3 ]]; then
            printf '%s' "$value"
            return
        fi
        echo "  Invalid value, please try again" >&2
    done
}

# ask_number PROMPT DEFAULT MIN MAX: reads a whole number from MIN to MAX.
ask_number() {
    local value
    while true; do
        value=$(ask "$1 ($3-$4)" "$2" '^[0-9]{1,9}$')
        value=$((10#$value))
        if ((value >= $3 && value <= $4)); then
            printf '%s' "$value"
            return
        fi
        echo "  Must be from $3 to $4" >&2
    done
}

# What the settings questions changed, as "label: old -> new".
CHANGES=()

# note LABEL OLD NEW: records the change, if there is one.
note() {
    [[ $2 == "$3" ]] || CHANGES+=("$1: ${2:-none} -> ${3:-none}")
}

# field VARIABLE LABEL KEY PATTERN: asks for KEY, offering its current value.
field() {
    local value
    value=$(ask "$2" "$(current "$3")" "$4")
    note "$2" "$(current "$3")" "$value"
    printf -v "$1" '%s' "$value"
}

# number_field VARIABLE LABEL KEY MIN MAX: field for a whole number.
number_field() {
    local value
    value=$(ask_number "$2" "$(current "$3")" "$4" "$5")
    note "$2" "$(current "$3")" "$value"
    printf -v "$1" '%s' "$value"
}

# review: shows CHANGES and whether to save them; nothing changed is not saved.
review() {
    if ((${#CHANGES[@]} == 0)); then
        echo "No changes."
        return 1
    fi
    echo "Changes:"
    printf '  %s\n' "${CHANGES[@]}"
    confirm "Save them?" y
}

# secret PROMPT: reads a value without echo; there is no default.
secret() {
    local value
    while true; do
        read -r -s -p "$1: " value || fail "Credential input ended"
        echo >&2
        [[ -n $value ]] && break
        echo "  Required" >&2
    done
    printf '%s' "$value"
}

# confirm PROMPT y|n: whether the answer is yes; Enter gives the default.
confirm() {
    local answer
    if [[ $2 == y ]]; then
        read -r -p "$1 [Y/n]: " answer || fail "Input ended"
        [[ $answer != [nN]* ]]
    else
        read -r -p "$1 [y/N]: " answer || fail "Input ended"
        [[ $answer == [yY]* ]]
    fi
}

# setting FILE KEY DEFAULT: the dotted KEY of a JSON settings FILE, or DEFAULT
# when the file, the key or an object on its way is missing or null; a null
# value prints empty. A list prints space separated, a client in it as its
# fingerprint.
setting() {
    if [[ ! -f $1 ]]; then
        printf '%s' "$3"
        return
    fi
    python -I -c '
import json, sys
value = json.load(open(sys.argv[1]))
keys = sys.argv[2].split(".")
for at, key in enumerate(keys):
    if not isinstance(value, dict) or key not in value or (value[key] is None and at < len(keys) - 1):
        print(sys.argv[3])
        raise SystemExit
    value = value[key]
if value is None:
    value = ""
elif isinstance(value, list):
    value = " ".join(item["fingerprint"] if isinstance(item, dict) else str(item) for item in value)
print(value)' "$1" "$2" "$3"
}

# numbered ITEMS...: one item per line, numbered from 1.
numbered() {
    local i=0 item
    for item; do
        i=$((i + 1))
        printf '    %d) %s\n' "$i" "$item"
    done
}

# ask_pin: a Terminal certificate fingerprint as 64 lowercase hex digits, or
# nothing when Enter is pressed alone.
ask_pin() {
    local value
    while true; do
        read -r -p "Terminal fingerprint (Enter to finish): " value || fail "Input ended"
        value=${value//[:[:space:]]/}
        value=${value,,}
        if [[ -z $value || $value =~ ^[0-9a-f]{64}$ ]]; then
            printf '%s' "$value"
            return
        fi
        echo "  A fingerprint is 64 hex digits, as Terminal shows it in Settings > About" >&2
    done
}

# add_pins PINS...: PINS and the fingerprints entered after them, at least one
# and at most 64 in all.
add_pins() {
    local pins=("$@") pin
    echo "Paste each Terminal's fingerprint from its Settings > About." >&2
    while ((${#pins[@]} < 16)); do
        pin=$(ask_pin)
        if [[ -z $pin ]]; then
            ((${#pins[@]} == 0)) || break
            echo "  Register at least one Terminal" >&2
        elif [[ " ${pins[*]} " == *" $pin "* ]]; then
            echo "  Already registered" >&2
        else
            pins+=("$pin")
        fi
    done
    printf '%s' "${pins[*]}"
}

# remove_pins PINS...: PINS without the ones chosen by their numbers; at least
# one stays.
remove_pins() {
    local pins=("$@") kept numbers number
    while true; do
        read -r -p "Numbers to remove (space separated, Enter for none): " numbers || fail "Input ended"
        kept=("${pins[@]}")
        for number in $numbers; do
            if ! [[ $number =~ ^[0-9]{1,2}$ ]] || ((number < 1 || number > ${#pins[@]})); then
                echo "  Choose numbers from 1 to ${#pins[@]}" >&2
                continue 2
            fi
            unset 'kept[number - 1]'
        done
        ((${#kept[@]} == 0)) || break
        echo "  At least one Terminal stays registered" >&2
    done
    printf '%s' "${kept[*]}"
}

# edit_pins add|remove FILE KEY: adds or removes Terminals in the fingerprint
# list KEY of the settings FILE, leaving the rest of it as it is.
edit_pins() {
    local pins next settings
    pins=$(current "$3")
    # shellcheck disable=SC2086
    if [[ $1 == add ]]; then next=$(add_pins $pins); else next=$(remove_pins $pins); fi
    [[ $next != "$pins" ]] || return 0
    stage settings "$2"
    # shellcheck disable=SC2086
    python -I -c '
import json, sys
path, final, key, *pins = sys.argv[1:]
settings = json.load(open(final))
settings[key] = [{"fingerprint": pin} for pin in pins] if key == "clients" else pins
with open(path, "w") as output:
    json.dump(settings, output, ensure_ascii=False, indent=2)
    output.write("\n")' "$settings" "$2" "$3" $next
    private "$settings"
    mv -f "$settings" "$2"
    CHANGED=1
    echo "Saved $2"
}

# refresh_settings FILE WRITER: on upgrade, FILE in this release's format.
# WRITER PATH writes the current values to PATH, so keys new in this release
# take their defaults and keys it no longer reads are dropped. Each change is
# shown; FILE is replaced only when its content changes.
refresh_settings() {
    local next
    [[ -f $1 ]] || return 0
    stage next "$1"
    "$2" "$next"
    python -I -c '
import json, sys
def flat(value, prefix=""):
    if not isinstance(value, dict):
        return {prefix: value}
    return {k: v for key, item in value.items() for k, v in flat(item, prefix + "." + key if prefix else key).items()}
old, new = (flat(json.load(open(path))) for path in sys.argv[2:])
show = lambda value: json.dumps(value, ensure_ascii=False)
changes = [f"  {key}: removed" if key not in new else f"  {key}: added, {show(new[key])}" if key not in old
           else f"  {key}: {show(old[key])} -> {show(new[key])}" for key in sorted(old.keys() | new.keys())
           if old.get(key, ...) != new.get(key, ...)]
if not changes:
    raise SystemExit(1)
print(f"Updated {sys.argv[1]} for this release:")
print("\n".join(changes))' "$1" "$1" "$next" || return 0
    private "$next"
    mv -f "$next" "$1"
    CHANGED=1
}

# status UNIT [TIMER]: whether UNIT runs and starts at boot, and when TIMER
# restarts it.
status() {
    local state enabled
    state=$(systemctl show "$1" --property=ActiveState --value)
    enabled=$(systemctl is-enabled "$1" 2>/dev/null) || true
    printf '%s, %s' "${state:-unknown}" "${enabled:-unknown}"
    if [[ -n ${2:-} ]] && systemctl is-active --quiet "$2.timer"; then
        printf ', restarts %s' "$(systemctl show "$2.timer" --property=NextElapseUSecRealtime --value)"
    fi
}

# data_directory PREVIOUS DATA: makes DATA the service's own; data in another
# PREVIOUS directory is not moved.
data_directory() {
    mkdir -p "$2"
    chmod 0700 "$2"
    chown "$ACCOUNT:" "$2"
    if [[ $2 != "$1" && -d $1 && -n $(ls -A "$1") ]]; then
        echo "The existing data stays in $1; it is not moved. To keep using it, stop $NAME and move its contents to $2."
    fi
}

# restart_time_at WEEKDAY HHMM: the end of the trading window at that Beijing
# time (1 is Monday), "+1 02:45" for tomorrow's, or "now" outside one.
restart_time_at() {
    local weekday=$1 minute=$((10#${2:0:2} * 60 + 10#${2:2:2}))
    if ((weekday <= 5 && minute >= 525 && minute < 930)); then
        echo "15:30"
    elif ((weekday <= 5 && minute >= 1245)); then
        echo "+1 02:45"
    elif ((weekday >= 2 && weekday <= 6 && minute < 165)); then
        echo "02:45"
    else
        echo now
    fi
}

# A pending restart owns a timer; stopping it also removes a completed
# transient service so the same unit can be scheduled again.
cancel_restart() {
    systemctl stop "$1.timer" "$1.service" 2>/dev/null || true
}

# restart_after_trading UNIT TIMER: restarts UNIT now, or, while it runs in a
# trading window (Beijing weekdays 08:45-15:30 and 20:45-02:45, holidays
# included), with the timer TIMER when that window ends.
restart_after_trading() {
    local at=now
    if systemctl is-active --quiet "$1"; then
        # shellcheck disable=SC2046
        at=$(restart_time_at $(TZ=Asia/Shanghai date +'%u %H%M'))
    fi
    cancel_restart "$2"
    case $at in
        now)
            systemctl restart "$1"
            echo "Restarted $1. Logs in $ROOT/logs"
            return ;;
        "+1 "*) at="$(TZ=Asia/Shanghai date -d tomorrow +%F) ${at#+1 }" ;;
        *) at="$(TZ=Asia/Shanghai date +%F) $at" ;;
    esac
    systemd-run --quiet --collect --unit="$2" --on-calendar="$at:00 Asia/Shanghai" systemctl restart "$1"
    echo "$1 is running in trading hours: the change takes effect when it restarts at $at Beijing time. To cancel: systemctl stop $2.timer"
}

# restart_now UNIT TIMER: restarts UNIT at once, in place of its pending
# restart and of the saved changes waiting for one.
restart_now() {
    cancel_restart "$2"
    systemctl restart "$1"
    CHANGED=''
    echo "Restarted $1. Logs in $ROOT/logs"
}

certificate() {
    openssl x509 -noout -fingerprint -sha256 -in "$ROOT/config/tls/server.crt" | sed 's/^.*=//'
}

fingerprint() {
    echo "This host's certificate fingerprint, to enter in Terminal:"
    echo "  $(certificate)"
}

firewall() {
    echo "Open TCP port $1 in the firewall, e.g. sudo ufw allow $1/tcp"
}

# This host's certificate, created once.
create_identity() {
    local directory=$ROOT/config/tls staging
    if [[ -f $directory/server.crt && -f $directory/server.key ]]; then return; fi
    [[ ! -e $directory/server.crt && ! -e $directory/server.key ]] || fail "Incomplete TLS identity in $directory; preserve and inspect it"
    staging=$(mktemp -d "$ROOT/config/.tls-XXXXXX")
    CLEANUP+=("$staging")
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 36500 \
        -subj "/CN=$PACKAGE" -keyout "$staging/server.key" -out "$staging/server.crt" 2>/dev/null
    chown -R "$ACCOUNT:" "$staging"
    chmod 0700 "$staging"
    chmod 0600 "$staging/server.key"
    [[ ! -d $directory ]] || rmdir "$directory"
    mv "$staging" "$directory"
}

# install_base SOURCE: the directories, the default logging settings and this
# host's certificate.
install_base() {
    mkdir -p "$ROOT"/{bin,config,logs} "$CREDENTIALS"
    chmod 0700 "$ROOT/config" "$ROOT/logs" "$CREDENTIALS"
    chown "$ACCOUNT:" "$ROOT/config" "$ROOT/logs"
    if [[ ! -f $ROOT/config/logging.json ]]; then
        local logging
        stage logging "$ROOT/config/logging.json"
        cat "$1/config/logging.json" >"$logging"
        chown "$ACCOUNT:" "$logging"
        mv -f "$logging" "$ROOT/config/logging.json"
    fi
    create_identity
}

# The unit of this release, on install and on every upgrade.
write_unit() {
    local unit
    stage unit "$UNITS/$UNIT"
    unit_text >"$unit"
    chmod 0644 "$unit"
    mv -f "$unit" "$UNITS/$UNIT"
    systemctl daemon-reload
}

# place_programs SOURCE: replaces PROGRAMS one rename each; a running service
# keeps the files it started with until it restarts.
place_programs() {
    local name next
    "$1/${PROGRAMS[0]}" --version >/dev/null 2>&1 || fail "$NAME in this release does not run on this host"
    for name in "${PROGRAMS[@]}"; do
        mkdir -p "$(dirname "$ROOT/$name")"
        stage next "$ROOT/$name"
        install -m 0755 "$1/$name" "$next"
        mv -f "$next" "$ROOT/$name"
    done
}

# Whether this run saved something that takes effect when the service restarts.
CHANGED=

# service_menu: the installed copy's choices for a service with one instance.
service_menu() {
    local choice waiting
    while true; do
        # A service that defers its restart can still be restarted at once.
        waiting=''
        if [[ -n ${TIMER:-} ]] && { [[ -n $CHANGED ]] || systemctl is-active --quiet "$TIMER.timer"; }; then
            waiting=1
        fi
        echo
        echo "Asterion $NAME  $ROOT"
        echo "  Service      $(status "$UNIT" "${TIMER:-}")"
        [[ -z $CHANGED ]] || echo "  Pending      a restart on Done applies the saved changes"
        echo "  Listen       $(current bind):$(current port)"
        echo "  Certificate  $(certificate)"
        echo "  Terminals"
        # shellcheck disable=SC2046
        numbered $(current clients)
        echo
        echo "  1) Add Terminals"
        echo "  2) Remove Terminals"
        echo "  3) Change settings"
        [[ -z $waiting ]] || echo "  r) Restart now"
        echo "  q) Done"
        read -r -p "Choose [q]: " choice || break
        case ${choice:-q} in
            1) edit_pins add "$ROOT/$SETTINGS" clients ;;
            2) edit_pins remove "$ROOT/$SETTINGS" clients ;;
            3) configure ;;
            r)
                if [[ -z $waiting ]]; then
                    echo "  Unknown choice" >&2
                elif confirm "Restart $NAME now instead of after the trading window?" n; then
                    restart_now "$UNIT" "$TIMER"
                fi ;;
            q) break ;;
            *) echo "  Unknown choice" >&2 ;;
        esac
    done
}

# install_or_configure SOURCE: for a service with one instance, opens the
# installed copy's menu, upgrades an installation (then opens the menu) or
# installs and configures; then applies what changed, once.
install_or_configure() {
    if [[ $1 == "$ROOT" ]]; then
        service_menu
    elif [[ -x $ROOT/${PROGRAMS[0]} ]]; then
        place_programs "$1"
        write_unit
        echo "Upgraded the programs in $ROOT."
        refresh_settings "$ROOT/$SETTINGS" current_settings
        CHANGED=1
        service_menu
    else
        install_base "$1"
        write_unit
        systemctl enable "$UNIT" >/dev/null
        place_programs "$1"
        echo "Installed in $ROOT; $NAME runs as $ACCOUNT. Settings:"
        configure
        firewall "$(current port)"
        fingerprint
    fi
    if [[ -n $CHANGED ]]; then apply; fi
}

main() {
    [[ $EUID -eq 0 ]] || fail "Run it with sudo"
    if [[ ${1:-} == fingerprint ]]; then
        fingerprint
        return
    fi
    local source line
    line=$(LC_ALL=C awk '/^__ARCHIVE__$/ { print NR + 1; exit }' "$SELF")
    if [[ -n $line ]]; then
        source=$(mktemp -d)
        CLEANUP+=("$source")
        tail -n +"$line" "$SELF" | tar xz --no-same-owner -C "$source"
        source=$source/$PACKAGE
    elif [[ $(dirname "$SELF") == "$ROOT/bin" ]]; then
        source=$ROOT
    else
        fail "This copy carries no programs: install or upgrade with the released Asterion-$NAME-VERSION-linux-x86_64.sh"
    fi
    if [[ -d $ROOT/config ]]; then
        ACCOUNT=$(stat -c %U "$ROOT/config")
    else
        ACCOUNT=${SUDO_USER:-}
        [[ -n $ACCOUNT && $ACCOUNT != root ]] || fail "Run it with sudo from your own account: $NAME will run as that account"
    fi
    setup "$source"
}

# Bash must stop here, run or sourced: the release's archive follows.
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
    exit
fi
return
