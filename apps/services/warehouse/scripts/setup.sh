#!/bin/bash
# Installs, upgrades and configures Warehouse on a service host.
#
#   sudo bash Asterion-Warehouse-VERSION-linux-x86_64.sh       install, or upgrade an installation
#   sudo /opt/asterion/warehouse/bin/setup.sh   change the settings
#   sudo /opt/asterion/warehouse/bin/setup.sh fingerprint   show this host's certificate fingerprint
#
# The release, Asterion-Warehouse-VERSION-linux-x86_64.sh, is this script
# followed by apps/services/common/scripts/setup.sh, with the installation
# appended as a tar.gz after a line "__ARCHIVE__" (built by the CMake target
# asterion_warehouse_setup). The installed copy, bin/setup.sh, carries nothing
# and only changes the settings.
#
# The installation is /opt/asterion/warehouse: bin/ config/ logs/, and
# data/ unless the settings name another data directory.
# A first install asks for the settings; an upgrade replaces only bin/ and
# leaves config/ (warehouse.json, logging.json) as it is
# (it offers to change the settings).
# Settings questions offer the current values as defaults; the Tushare token
# is encrypted with systemd-creds, never written to a file in the clear.
# Warehouse runs as the system service asterion-warehouse under the account
# that ran the install with sudo.
set -euo pipefail

NAME=Warehouse
PACKAGE=asterion-warehouse
ROOT=/opt/asterion/warehouse
UNIT=asterion-warehouse.service
PROGRAMS=(bin/asterion_warehouse bin/asterion_crash_reporter bin/setup.sh)

# Defaults for new configurations only; existing instances keep their own values.
declare -Ar DEFAULTS=(
    [bind]='0.0.0.0'
    [port]='7450'
    [clients]=''
    [acquisition.exchanges]='SHFE INE CZCE DCE CFFEX GFEX'
    [acquisition.tushare.requests_per_minute]='500'
)

python() {
    python3 "$@"
}

# The current value of a key ("port", "acquisition.exchanges"), or its
# default; the data directory defaults to one beside the programs.
current() {
    local default
    if [[ $1 == data ]]; then default=$ROOT/data; else default=${DEFAULTS[$1]}; fi
    setting "$ROOT/config/warehouse.json" "$1" "$default"
}

configure() {
    local bind port data previous settings clients exchanges budget token='' encrypted_token=''
    bind=$(ask "Listen address" "$(current bind)" '^[0-9A-Fa-f.:]+$')
    port=$(ask "Listen port" "$(current port)" '^[0-9]{1,5}$')
    ((port >= 1 && port <= 65535)) || fail "The port must be between 1 and 65535"
    previous=$(current data)
    data=$(ask "Data directory" "$previous" '^/[^[:space:]]*$')
    clients=$(ask "Terminal fingerprints as <fingerprint>=read or <fingerprint>=manage (space separated)" \
        "$(current clients)" '^([0-9A-Fa-f:]+=(read|manage))( [0-9A-Fa-f:]+=(read|manage))*$')
    clients=$(pins "$clients")
    exchanges=$(current acquisition.exchanges)
    exchanges=$(ask "Public exchanges (- for none)" "${exchanges:--}" '^[A-Z[:space:]-]+$')
    budget=$(ask "Tushare requests per minute (1-500)" "$(current acquisition.tushare.requests_per_minute)" '^[0-9]{1,3}$')
    ((budget >= 1 && budget <= 500)) || fail "Tushare requests per minute must be 1-500"
    if ! [[ -f $CREDENTIALS/asterion.warehouse.tushare.token ]] || ! confirm "Keep the saved Tushare token?" y; then
        token=$(secret "Tushare token (not shown)")
        [[ ${#token} -le 256 && ! $token =~ [[:space:]] ]] || fail "Tushare token must be at most 256 characters without whitespace"
    fi

    # Everything is asked; now the files, each staged and moved into place.
    stage settings "$ROOT/config/warehouse.json"
    python -I -c '
import json, sys
path, bind, port, data, clients, exchanges, budget = sys.argv[1:]
exchanges = [] if exchanges == "-" else exchanges.split()
if len(set(exchanges)) != len(exchanges) or set(exchanges) - {"SHFE", "INE", "CZCE", "DCE", "CFFEX", "GFEX"}:
    raise SystemExit("setup: invalid or duplicate acquisition exchanges")
settings = {"bind": bind, "port": int(port), "data": data,
    "clients": [dict(zip(("fingerprint", "role"), item.split("="))) for item in clients.split()],
    "acquisition": {"exchanges": exchanges, "tushare": {"requests_per_minute": int(budget)}}}
with open(path, "w") as output:
    json.dump(settings, output, ensure_ascii=False, indent=2)
    output.write("\n")' "$settings" "$bind" "$port" "$data" "$clients" "$exchanges" "$budget"
    private "$settings"
    [[ -z $token ]] || seal encrypted_token "$CREDENTIALS/asterion.warehouse.tushare.token" asterion.warehouse.tushare.token "$token"
    token=''
    [[ -z $encrypted_token ]] || mv -f "$encrypted_token" "$CREDENTIALS/asterion.warehouse.tushare.token"
    data_directory "$previous" "$data"
    mv -f "$settings" "$ROOT/config/warehouse.json"
    echo "Saved $ROOT/config/warehouse.json"
    # The service may write only its logs and its data directory.
    write_unit
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
# It finds config/ and logs/ beside its bin/; config/warehouse.json names its data.
ExecStart=$ROOT/bin/asterion_warehouse
ReadWritePaths=$ROOT/logs $(current data)
# The Tushare token, decrypted at start.
ImportCredential=asterion.warehouse.*
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

setup() {
    install_or_configure "$1"
    echo "Client roles and providers take effect after restart. Provider credentials stay on this host."
    systemctl restart "$UNIT"
    echo "Restarted $UNIT. Logs in $ROOT/logs"
}
