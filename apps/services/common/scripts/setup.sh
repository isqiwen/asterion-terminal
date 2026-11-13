# The part of setup.sh that Market, Warehouse and Trading share. The build
# appends it to each service's own scripts/setup.sh; neither runs alone.
#
# The service part sets NAME (as messages name it: Market), PACKAGE (the
# release's top directory and certificate name: asterion-market), ROOT, UNIT
# (its systemd unit file) and PROGRAMS (bin/ and lib/ files that place_programs
# replaces), and defines python (the interpreter that reads its settings),
# unit_text and setup SOURCE, which main calls with the unpacked release or,
# for the installed copy, ROOT.

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

# ask PROMPT DEFAULT PATTERN: reads a value, offering the default.
ask() {
    local value
    while true; do
        read -r -p "$1 [$2]: " value || fail "Input ended"
        value=${value:-$2}
        if [[ $value =~ $3 ]]; then
            printf '%s' "$value"
            return
        fi
        echo "  Invalid value, please try again" >&2
    done
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
# when the file or the key is missing. A list prints space separated, an
# object in it as its values joined by "=".
setting() {
    if [[ ! -f $1 ]]; then
        printf '%s' "$3"
        return
    fi
    python -I -c '
import json, sys
value = json.load(open(sys.argv[1]))
for key in sys.argv[2].split("."):
    if not isinstance(value, dict) or key not in value or value[key] is None:
        print(sys.argv[3])
        raise SystemExit
    value = value[key]
if isinstance(value, list):
    value = " ".join(item["fingerprint"] if isinstance(item, dict) else str(item) for item in value)
print(value)' "$1" "$2" "$3"
}

# pins TEXT: the space-separated Terminal fingerprints as 64 lowercase hex
# digits; 1-64 distinct ones.
pins() {
    local item pin result=() seen=" "
    for item in $1; do
        pin=${item//:/}
        pin=${pin,,}
        [[ $pin =~ ^[0-9a-f]{64}$ && $seen != *" $pin "* ]] || fail "Invalid or duplicate Terminal fingerprint: $item"
        seen+="$pin "
        result+=("$pin")
    done
    ((${#result[@]} >= 1 && ${#result[@]} <= 64)) || fail "Register 1-64 Terminal fingerprints"
    printf '%s' "${result[*]}"
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

fingerprint() {
    echo "This host's certificate fingerprint, to enter in Terminal:"
    echo "  $(openssl x509 -noout -fingerprint -sha256 -in "$ROOT/config/tls/server.crt" | sed 's/^.*=//')"
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

# install_or_configure SOURCE: for a service with one instance, configures
# the installed copy, upgrades an installation (offering to change its
# settings) or installs and configures.
install_or_configure() {
    if [[ $1 == "$ROOT" ]]; then
        configure
    elif [[ -x $ROOT/${PROGRAMS[0]} ]]; then
        place_programs "$1"
        write_unit
        echo "Upgraded the programs in $ROOT; the configuration files are unchanged."
        if confirm "Change the settings?" n; then configure; fi
    else
        install_base "$1"
        write_unit
        systemctl enable "$UNIT" >/dev/null
        place_programs "$1"
        echo "Installed in $ROOT; $NAME runs as $ACCOUNT. Settings:"
        configure
        firewall "$(current port)"
    fi
    fingerprint
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
