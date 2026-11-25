#!/bin/bash
# 在服务主机上安装、升级与管理 Warehouse：
#
#   sudo bash Asterion-Warehouse-VERSION-linux-x86_64.sh   首次安装或升级
#   sudo asterion-warehouse                                打开管理面板（或 sudo /opt/asterion/warehouse/bin/setup.sh）
#   sudo asterion-warehouse fingerprint                    查看本机证书指纹
set -euo pipefail

NAME=Warehouse
PACKAGE=asterion-warehouse
ROOT=/opt/asterion/warehouse
UNIT=asterion-warehouse.service
SETTINGS=config/warehouse.json
PROGRAMS=(bin/asterion_warehouse bin/asterion_crash_reporter bin/setup.sh bin/cold_backup.py)

declare -Ar DEFAULTS=(
    [bind]='0.0.0.0'
    [port]='7450'
    [clients]=''
    [acquisition.exchanges]='SHFE INE CZCE DCE CFFEX GFEX'
    [acquisition.tushare.requests_per_minute]='500'
    [acquisition.update_at]='06:00'
)

python() {
    python3 "$@"
}

current() {
    local default
    if [[ $1 == data ]]; then default=$ROOT/data; else default=${DEFAULTS[$1]}; fi
    setting "$ROOT/$SETTINGS" "$1" "$default"
}

EXCHANGES=" SHFE INE CZCE DCE CFFEX GFEX "

ask_exchanges() {
    local value exchange picked valid
    while true; do
        value=$(ask "公开补缺交易所（空格分隔，填 - 关闭）" "$1" '^[A-Z -]+$')
        if [[ $value == - ]]; then
            printf -- -
            return
        fi
        picked=() valid=1
        for exchange in $value; do
            [[ $EXCHANGES == *" $exchange "* && " ${picked[*]} " != *" $exchange "* ]] || valid=
            picked+=("$exchange")
        done
        if [[ -n $valid && ${#picked[@]} -gt 0 ]]; then
            printf '%s' "${picked[*]}"
            return
        fi
        echo "  请从以下交易所中选择不重复项:$EXCHANGES" >&2
    done
}

ask_tushare_token() {
    local token
    while true; do
        token=$(secret "Tushare Token（输入不回显）")
        [[ ${#token} -gt 256 || $token =~ [[:space:]] ]] || break
        echo "  最多 256 个字符且不能包含空白字符" >&2
    done
    printf '%s' "$token"
}

write_settings() {
    python -I -c '
import json, sys
path, bind, port, data, clients, exchanges, budget, update_at = sys.argv[1:]
settings = {"bind": bind, "port": int(port), "data": data,
    "clients": [{"fingerprint": pin} for pin in clients.split()],
    "acquisition": {"exchanges": exchanges.split(), "tushare": {"requests_per_minute": int(budget)},
        "update_at": None if update_at == "-" else update_at}}
with open(path, "w") as output:
    json.dump(settings, output, ensure_ascii=False, indent=2)
    output.write("\n")' "$1" "$bind" "$port" "$data" "$clients" "$exchanges" "$budget" "$update_at"
}

current_settings() {
    local bind port data clients exchanges budget update_at
    bind=$(current bind) port=$(current port) data=$(current data) clients=$(current clients)
    exchanges=$(current acquisition.exchanges) budget=$(current acquisition.tushare.requests_per_minute)
    update_at=$(current acquisition.update_at)
    update_at=${update_at:--}
    write_settings "$1"
}

commit_warehouse_settings() {
    local previous=$1 token=$2 settings encrypted_token=''
    stage settings "$ROOT/$SETTINGS"
    write_settings "$settings"
    private "$settings"
    [[ -z $token ]] || seal encrypted_token "$CREDENTIALS/asterion.warehouse.tushare.token" asterion.warehouse.tushare.token "$token"
    [[ -z $encrypted_token ]] || mv -f "$encrypted_token" "$CREDENTIALS/asterion.warehouse.tushare.token"
    data_directory "$previous" "$data"
    mv -f "$settings" "$ROOT/$SETTINGS"
    CHANGED=1
    echo "已保存 $ROOT/$SETTINGS"
    write_unit
}

configure() {
    local bind port data previous clients exchanges budget update_at previous_update token=''
    CHANGES=()
    field bind "监听地址" bind '^[0-9A-Fa-f.:]+$'
    number_field port "监听端口" port 1 65535
    previous=$(current data)
    field data "数据存储目录" data '^/[^[:space:]]*$'
    exchanges=$(current acquisition.exchanges)
    exchanges=$(ask_exchanges "${exchanges:--}")
    [[ $exchanges != - ]] || exchanges=
    note "公开补缺交易所" "$(current acquisition.exchanges)" "$exchanges"
    number_field budget "Tushare 每分钟请求上限" acquisition.tushare.requests_per_minute 1 500
    previous_update=$(current acquisition.update_at)
    previous_update=${previous_update:--}
    update_at=$(ask "每日自动更新时间，北京时间（HH:MM，填 - 关闭）" "$previous_update" '^(([01][0-9]|2[0-3]):[0-5][0-9]|-)$')
    note "每日自动更新" "$previous_update" "$update_at"
    clients=$(current clients)
    [[ -f $ROOT/$SETTINGS ]] || clients=$(add_pins)
    if ! [[ -f $CREDENTIALS/asterion.warehouse.tushare.token ]] || ! confirm "保留已保存的 Tushare Token？" y; then
        token=$(ask_tushare_token)
        CHANGES+=("Tushare Token: 已更新")
    fi
    if [[ -f $ROOT/$SETTINGS ]] && ! review; then return; fi
    commit_warehouse_settings "$previous" "$token"
}

settings_menu() {
    local choice bind port data previous clients exchanges budget update_at previous_update token encrypted_token
    while true; do
        bind=$(current bind) port=$(current port) data=$(current data) clients=$(current clients)
        exchanges=$(current acquisition.exchanges) budget=$(current acquisition.tushare.requests_per_minute)
        update_at=$(current acquisition.update_at)
        previous_update=${update_at:--}
        echo
        hr_single
        echo " ${C_BOLD}配置与凭据设置（Warehouse）${C_RESET}"
        hr_single
        echo " 1) 监听地址与端口        $bind:$port"
        echo " 2) 数据存储目录          $data"
        echo " 3) 公开补缺交易所        ${exchanges:-已关闭}"
        echo " 4) Tushare 每分钟限速    $budget"
        echo " 5) 每日自动更新时间      ${update_at:-已关闭}"
        echo " 6) ${C_YELLOW}更新 Tushare Token${C_RESET}    $([[ -f $CREDENTIALS/asterion.warehouse.tushare.token ]] && echo 已加密保存 || echo 未设置)"
        echo " 7) ${C_CYAN}逐项向导修改全部${C_RESET}"
        echo " 0) 返回上级菜单"
        hr_single
        read -r -p "请输入对应数字 [0]> " choice || break
        CHANGES=()
        previous=$data
        update_at=$previous_update
        case ${choice:-0} in
            1)
                field bind "监听地址" bind '^[0-9A-Fa-f.:]+$'
                number_field port "监听端口" port 1 65535
                review && commit_warehouse_settings "$previous" "" ;;
            2)
                field data "数据存储目录" data '^/[^[:space:]]*$'
                review && commit_warehouse_settings "$previous" "" ;;
            3)
                exchanges=$(ask_exchanges "${exchanges:--}")
                [[ $exchanges != - ]] || exchanges=
                note "公开补缺交易所" "$(current acquisition.exchanges)" "$exchanges"
                review && commit_warehouse_settings "$previous" "" ;;
            4)
                number_field budget "Tushare 每分钟请求上限" acquisition.tushare.requests_per_minute 1 500
                review && commit_warehouse_settings "$previous" "" ;;
            5)
                update_at=$(ask "每日自动更新时间，北京时间（HH:MM，填 - 关闭）" "$previous_update" '^(([01][0-9]|2[0-3]):[0-5][0-9]|-)$')
                note "每日自动更新" "$previous_update" "$update_at"
                review && commit_warehouse_settings "$previous" "" ;;
            6)
                token=$(ask_tushare_token)
                CHANGES+=("Tushare Token: 已更新")
                if review; then
                    encrypted_token=''
                    seal encrypted_token "$CREDENTIALS/asterion.warehouse.tushare.token" asterion.warehouse.tushare.token "$token"
                    token=''
                    mv -f "$encrypted_token" "$CREDENTIALS/asterion.warehouse.tushare.token"
                    CHANGED=1
                    echo "已加密保存 Tushare Token"
                fi
                token='' ;;
            7) configure ;;
            0) break ;;
            *) echo "  无效选项，请重新输入" >&2 ;;
        esac
    done
}

unit_text() {
    cat <<EOF
[Unit]
Description=Asterion historical data
Wants=network-online.target time-sync.target
After=network-online.target time-sync.target
StartLimitIntervalSec=0

[Service]
User=$ACCOUNT
UMask=0077
ExecStart=$ROOT/bin/asterion_warehouse
ReadWritePaths=$ROOT/logs $(current data)
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

RELEASE_SOURCE=

place_exchange_rules() {
    [[ -n $RELEASE_SOURCE ]] || return 0
    local staged
    stage staged "$ROOT/config/exchange_rules.json"
    python -I -c '
import json, pathlib, sys
shipped, installed, staged = map(pathlib.Path, sys.argv[1:])
data = json.loads(shipped.read_text())
if installed.is_file():
    try:
        current = json.loads(installed.read_text())
        through = current.get("dce_settlement_fees", {}).get("verified_through", "")
        if isinstance(through, str) and through > data["dce_settlement_fees"]["verified_through"]:
            data["dce_settlement_fees"]["verified_through"] = through
            for name, rules in current.get("dce_settlement_fees", {}).get("products", {}).items():
                data["dce_settlement_fees"]["products"].setdefault(name, rules)
    except Exception:
        pass
staged.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
' "$RELEASE_SOURCE/config/exchange_rules.json" "$ROOT/config/exchange_rules.json" "$staged"
    chown "$ACCOUNT:" "$staged"
    chmod 0644 "$staged"
    mv -f "$staged" "$ROOT/config/exchange_rules.json"
}

apply() {
    place_exchange_rules
    start_and_verify restart "$UNIT" "asterion_${NAME,,}" "$(current port)" 0
}

setup() {
    [[ $1 == "$ROOT" ]] || RELEASE_SOURCE=$1
    install_or_configure "$1"
}
