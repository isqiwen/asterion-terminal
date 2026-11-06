#!/bin/bash
# Installs, upgrades and configures the historical data service on a host.
#
#   sudo bash Asterion-History-VERSION-linux-x86_64.sh       install, or upgrade an installation
#   sudo /opt/asterion/history/bin/setup.sh   change the settings
#
# The release, Asterion-History-VERSION-linux-x86_64.sh, is this script with the
# installation appended as a tar.gz after a line "__ARCHIVE__" (built by the
# CMake target asterion_history_setup); it unpacks that into a temporary
# directory. The installed copy, bin/setup.sh, carries nothing and only
# changes the settings.
#
# The installation is /opt/asterion/history: bin/ config/ logs/, and
# data/ unless the settings name another data directory.
# A first install asks for the settings; an upgrade replaces only bin/ and
# leaves config/ (history.json, logging.json) as it is
# (it offers to change the settings).
# Settings questions offer the current values as defaults; the Tushare token
# has none and is encrypted with systemd-creds,
# never written to a file in the clear. History runs as the system
# service asterion-history under the account that ran the install with sudo.
#
set -euo pipefail

ROOT=/opt/asterion/history
UNITS=/etc/systemd/system
CREDENTIALS=/etc/credstore.encrypted
UNIT=asterion-history.service
RESTART=asterion-history-restart
# The account History runs as: the sudo user at install, the owner of
# config/ afterwards.
ACCOUNT=
PROGRAMS=(bin/asterion_history bin/asterion_crash_reporter bin/setup.sh)
# This script: the release with its archive, or the installed bin/setup.sh.
SELF=$(readlink -f "${BASH_SOURCE[0]}")

# Defaults for new configurations only; existing instances keep their own values.
declare -Ar DEFAULTS=(
    [bind]='0.0.0.0'
    [port]='7450'
    [snapshots]='/opt/asterion/quotes/data/catalog'
    [clients]=''
    [acquisition.exchanges]='SHFE INE CZCE DCE CFFEX GFEX'
    [acquisition.tushare]='0'
)

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

restart() {
    systemctl restart "$UNIT"
    echo "History has (re)started. Status: systemctl status $UNIT; logs in $ROOT/logs"
}

# The default of a key; the data directory is beside the programs.
default() {
    if [[ $1 == data ]]; then printf '%s' "$ROOT/data"; else printf '%s' "${DEFAULTS[$1]}"; fi
}

# The current value of a key ("port", "acquisition.tushare") in the settings, or
# its default where the settings have none.
current() {
    if [[ ! -f $ROOT/config/history.json ]]; then
        default "$1"
        return
    fi
    python3 -I -c '
import json, sys
value = json.load(open(sys.argv[1]))
for key in sys.argv[2].split("."):
    if key not in value:
        print(sys.argv[3])
        raise SystemExit
    value = value[key]
if sys.argv[2] == "clients":
    value = " ".join(item["fingerprint"] + "=" + item["role"] for item in value)
elif sys.argv[2] == "acquisition.exchanges":
    value = " ".join(value) or "-"
elif sys.argv[2] == "acquisition.tushare":
    value = value["requests_per_minute"] if value else 0
print(value)' "$ROOT/config/history.json" "$1" "$(default "$1")"
}

# ask KEY PROMPT PATTERN: reads a value, offering the current one.
ask() {
    local prompt=$2 pattern=$3 default value
    default=$(current "$1")
    while true; do
        read -r -p "$prompt [${default}]: " value || fail "Input ended"
        value=${value:-$default}
        if [[ $value =~ $pattern ]]; then
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

configure() {
    local bind port data previous snapshots settings
    local clients exchanges budget token='' encrypted_token=''
    bind=$(ask bind "Listen address" '^[0-9A-Fa-f.:]+$')
    port=$(ask port "Listen port" '^[0-9]{1,5}$')
    ((port >= 1 && port <= 65535)) || fail "The port must be between 1 and 65535"
    previous=$(current data)
    data=$(ask data "Data directory (warehouse)" '^/[^[:space:]]*$')
    snapshots=$(ask snapshots "Contract snapshots Quotes leaves" '^/[^[:space:]]*$')
    echo "Register Terminal fingerprints as <fingerprint>=read or <fingerprint>=manage (space separated)."
    clients=$(ask clients "Permitted clients" '^[0-9A-Fa-f:=[:space:]readmng]+$')
    exchanges=$(ask acquisition.exchanges "Public exchanges (- for none)" '^[A-Z[:space:]-]+$')
    budget=$(ask acquisition.tushare "Tushare requests per minute (0 disables)" '^[0-9]{1,3}$')
    ((budget <= 500)) || fail "Tushare budget must be 0-500"
    if ((budget > 0)); then
        token=$(secret "Tushare token (not shown)")
        [[ ${#token} -le 256 && ! $token =~ [[:space:]] ]] || fail "Tushare token must be at most 256 characters without whitespace"
    fi

    # Replaced whole, every file staged first; only the service's account may read it.
    stage settings "$ROOT/config/history.json"
    python3 -I -c '
import json, sys
bind, port, data, snapshots, clients, exchanges, budget = sys.argv[2:]
import re
peers = []
for item in clients.split():
    pin, role = item.rsplit("=", 1)
    pin = pin.replace(":", "").lower()
    if not re.fullmatch("[a-f0-9]{64}", pin) or role not in ("read", "manage") or any(p["fingerprint"] == pin for p in peers):
        raise SystemExit("Invalid or duplicate client fingerprint/role")
    peers.append({"fingerprint": pin, "role": role})
if not 1 <= len(peers) <= 64:
    raise SystemExit("Register 1-64 clients")
exchanges = [] if exchanges == "-" else exchanges.split()
if len(set(exchanges)) != len(exchanges) or set(exchanges) - {"SHFE", "INE", "CZCE", "DCE", "CFFEX", "GFEX"}:
    raise SystemExit("Invalid or duplicate acquisition exchanges")
settings = {"bind": bind, "port": int(port), "data": data, "snapshots": snapshots, "clients": peers,
    "acquisition": {"exchanges": exchanges, "tushare": {"requests_per_minute": int(budget)} if int(budget) else None}}
with open(sys.argv[1], "w") as output:
    json.dump(settings, output, ensure_ascii=False, indent=2)
    output.write("\n")' "$settings" "$bind" "$port" "$data" "$snapshots" \
        "$clients" "$exchanges" "$budget"
    chown "$ACCOUNT:" "$settings"
    chmod 0600 "$settings"
    if [[ -n $token ]]; then
        stage encrypted_token "$CREDENTIALS/asterion.history.tushare.token"
        printf '%s' "$token" | systemd-creds encrypt --name=asterion.history.tushare.token - "$encrypted_token" >/dev/null
    fi
    token=''
    [[ -z $encrypted_token ]] || mv -f "$encrypted_token" "$CREDENTIALS/asterion.history.tushare.token"
    mkdir -p "$data"
    chmod 0700 "$data"
    chown "$ACCOUNT:" "$data"
    mv -f "$settings" "$ROOT/config/history.json"
    echo "Saved $ROOT/config/history.json"
    # The service may write only its logs and its data directory.
    write_unit
    if [[ $data != "$previous" && -d $previous && -n $(ls -A "$previous") ]]; then
        echo "The existing data stays in $previous; it is not moved. To keep using it, stop History and move its contents to $data."
    fi
}

unit_text() {
    cat <<EOF
[Unit]
Description=Asterion historical data
Wants=network-online.target
After=network-online.target
# Never give up restarting: a service that stays down serves nothing.
StartLimitIntervalSec=0

[Service]
User=$ACCOUNT
UMask=0077
# It finds config/ and logs/ beside its bin/; config/history.json names its data.
ExecStart=$ROOT/bin/asterion_history
ReadWritePaths=$ROOT/logs $(current data)
# The Tushare token, decrypted at start.
ImportCredential=asterion.history.*
Restart=always
RestartSec=5
TimeoutStopSec=60
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
CapabilityBoundingSet=
RestrictSUIDSGID=yes

[Install]
WantedBy=multi-user.target
EOF
}

# install: the directories, the default settings and the unit.
prepare_identity() {
    local directory=$ROOT/config/tls
    if [[ -f $directory/server.crt && -f $directory/server.key ]]; then return; fi
    [[ ! -e $directory/server.crt && ! -e $directory/server.key ]] || fail "Incomplete TLS identity in $directory; preserve and inspect it"
    local staging
    staging=$(mktemp -d "$ROOT/config/.tls-XXXXXX")
    CLEANUP+=("$staging")
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 36500 \
        -subj "/CN=asterion-history" -keyout "$staging/server.key" -out "$staging/server.crt" 2>/dev/null
    chown -R "$ACCOUNT:" "$staging"
    chmod 0700 "$staging"
    chmod 0600 "$staging/server.key"
    rmdir "$directory"
    mv "$staging" "$directory"
}

install_once() {
    mkdir -p "$ROOT"/{bin,config/tls,logs} "$CREDENTIALS"
    chmod 0700 "$ROOT/config" "$ROOT/logs" "$CREDENTIALS"
    chown "$ACCOUNT:" "$ROOT/config" "$ROOT/logs"
    if [[ ! -f $ROOT/config/logging.json ]]; then
        local logging
        stage logging "$ROOT/config/logging.json"
        cat "$1/config/logging.json" >"$logging"
        chown "$ACCOUNT:" "$logging"
        mv -f "$logging" "$ROOT/config/logging.json"
    fi
    prepare_identity
    write_unit
    systemctl enable "$UNIT" >/dev/null
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

# Replaces the programs in bin/, one rename each: a running
# service keeps the files it started with until it restarts.
place() {
    local name next
    "$1/bin/asterion_history" --version >/dev/null 2>&1 || fail "History in this release does not run on this host"
    for name in "${PROGRAMS[@]}"; do
        stage next "$ROOT/$name"
        install -m 0755 "$1/$name" "$next"
        mv -f "$next" "$ROOT/$name"
    done
}

main() {
    [[ $EUID -eq 0 ]] || fail "Run it with sudo"
    local source line
    line=$(LC_ALL=C awk '/^__ARCHIVE__$/ { print NR + 1; exit }' "$SELF")
    if [[ -n $line ]]; then
        unpacked=$(mktemp -d)
        CLEANUP+=("$unpacked")
        tail -n +"$line" "$SELF" | tar xz --no-same-owner -C "$unpacked"
        source=$unpacked/asterion-history
    elif [[ $(dirname "$SELF") == "$ROOT/bin" ]]; then
        source=$ROOT
    else
        fail "This copy carries no programs: install or upgrade with the released Asterion-History-VERSION-linux-x86_64.sh"
    fi
    if [[ -d $ROOT/config ]]; then
        ACCOUNT=$(stat -c %U "$ROOT/config")
    else
        ACCOUNT=${SUDO_USER:-}
        [[ -n $ACCOUNT && $ACCOUNT != root ]] || fail "Run it with sudo from your own account: History will run as that account"
    fi
    if [[ $source == "$ROOT" ]]; then
        configure
    elif [[ -x $ROOT/bin/asterion_history ]]; then
        place "$source"
        write_unit
        echo "Upgraded the programs in $ROOT; the configuration files are unchanged."
        local answer
        read -r -p "Change the settings? [y/N]: " answer
        [[ $answer == [yY]* ]] && configure
    else
        install_once "$source"
        place "$source"
        echo "Installed in $ROOT; History runs as $ACCOUNT. Settings:"
        configure
        port=$(current port)
        echo "History requires pinned mutual TLS on TCP $port; register its fingerprint in each Terminal."
    fi
    openssl x509 -noout -fingerprint -sha256 -in "$ROOT/config/tls/server.crt"
    echo "Client roles and providers take effect after restart. Provider credentials stay on this host."
    restart
}

# Bash must stop here, run or sourced: the release's archive follows.
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
    exit
fi
return
