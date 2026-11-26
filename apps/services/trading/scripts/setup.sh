#!/bin/bash
# 在服务主机上安装、升级与管理 Trading 账户实例：
#
#   sudo bash Asterion-Trading-VERSION-linux-x86_64.sh   首次安装或离线升级（升级前须停止全部账户）
#   sudo asterion-trading                                打开账户管理面板（或 sudo /opt/asterion/trading/bin/setup.sh）
#   sudo asterion-trading fingerprint                    查看本机证书指纹
set -euo pipefail

NAME=Trading
PACKAGE=asterion-trading
ROOT=/opt/asterion/trading
UNIT=asterion-trading@.service
RESTART=asterion-trading-restart

declare -Ar DEFAULTS=(
    [bind]='0.0.0.0'
    [port]='7460'
    [terminals]=''
    [environment]='simulation'
    [broker.broker_id]='9999'
    [broker.user_id]=''
    [broker.app_id]='simnow_client_test'
    [broker.front]='tcp://182.254.243.31:40001'
    [market_front]='tcp://182.254.243.31:40011'
    [strategy_memory_limit_mb]='2048'
)

ACCOUNT_NAME='^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$'

python() {
    "$ROOT/runtime/python/bin/python3" "$@"
}

accounts() {
    local file
    for file in "$ROOT"/config/accounts/*.json; do
        [[ -e $file ]] && basename "$file" .json
    done
}

INSTANCE=

account_file() {
    echo "$ROOT/config/accounts/${1:-$INSTANCE}.json"
}

current() {
    if [[ ! -f $(account_file) && $1 == port ]]; then
        free_port
        return
    fi
    setting "$(account_file)" "$1" "${DEFAULTS[$1]}"
}

port_owner() {
    local other
    for other in $(accounts); do
        if [[ $other != "$INSTANCE" && $(setting "$(account_file "$other")" port "") == "$1" ]]; then
            echo "$other"
            return
        fi
    done
    return 1
}

free_port() {
    local port=${DEFAULTS[port]}
    while port_owner "$port" >/dev/null; do
        port=$((port + 1))
    done
    echo "$port"
}

ask_free_port() {
    local chosen owner
    while true; do
        chosen=$(ask_number "监听端口" "$(current port)" 1 65535)
        owner=$(port_owner "$chosen") || break
        echo "  端口 $chosen 已被账户 $owner 占用" >&2
    done
    printf '%s' "$chosen"
}

write_account() {
    python -I -c '
import json, sys
path, bind, port, terminals, environment, broker, user, app, front, market_front, memory_limit = sys.argv[1:]
identity = {"environment": environment, "broker": {"broker_id": broker, "user_id": user, "app_id": app, "front": front}}
with open(path, "w") as output:
    json.dump({"bind": bind, "port": int(port), "terminals": terminals.split(), "market_front": market_front, "strategy_memory_limit_mb": int(memory_limit), **identity}, output, indent=2)
    output.write("\n")' "$1" "$bind" "$port" "$terminals" "$environment" "$broker" "$user" "$app" "$front" "$market_front" "$strategy_memory_limit_mb"
}

current_account() {
    local bind port terminals environment broker user app front market_front strategy_memory_limit_mb
    bind=$(current bind) port=$(current port) terminals=$(current terminals)
    environment=$(current environment) broker=$(current broker.broker_id) user=$(current broker.user_id)
    app=$(current broker.app_id) front=$(current broker.front) market_front=$(current market_front)
    strategy_memory_limit_mb=$(current strategy_memory_limit_mb)
    write_account "$1"
}

write_memory_dropin() {
    local name=$1 limit_mb=$2 dir dropin
    dir=$UNITS/asterion-trading@$name.service.d
    mkdir -p "$dir"
    stage dropin "$dir/memory.conf"
    printf '[Service]\nMemoryMax=%sM\n' "$limit_mb" >"$dropin"
    chmod 0644 "$dropin"
    mv -f "$dropin" "$dir/memory.conf"
    systemctl daemon-reload
}

commit_account_settings() {
    local file=$1 credentials=$2 password=$3 auth_code=$4 webhook_file=$5 webhook=$6 clear_webhook=$7
    local config encrypted_password='' encrypted_auth_code='' encrypted_webhook=''
    stage config "$file"
    write_account "$config"
    private "$config"
    if [[ -n $password ]]; then
        install -d -m 0700 "$credentials"
        seal encrypted_password "$credentials/password" password "$password"
        seal encrypted_auth_code "$credentials/auth_code" auth_code "$auth_code"
        mv -f "$encrypted_password" "$credentials/password"
        mv -f "$encrypted_auth_code" "$credentials/auth_code"
    fi
    if [[ -n $webhook ]]; then
        install -d -m 0700 "$CREDENTIALS"
        seal encrypted_webhook "$webhook_file" "asterion.trading.$INSTANCE.webhook" "$webhook"
        mv -f "$encrypted_webhook" "$webhook_file"
    elif [[ -n $clear_webhook ]]; then
        rm -f "$webhook_file"
    fi
    mkdir -p "$ROOT/data/$INSTANCE" "$ROOT/data/owners"
    chmod 0700 "$ROOT/data/$INSTANCE" "$ROOT/data/owners"
    chown "$ACCOUNT:" "$ROOT/data/$INSTANCE" "$ROOT/data/owners"
    mv -f "$config" "$file"
    write_memory_dropin "$INSTANCE" "$strategy_memory_limit_mb"
    CHANGED=1
    echo "已保存 $file"
}

ask_webhook_value() {
    local allow_clear=$1 value
    while true; do
        if [[ $allow_clear == 1 ]]; then
            value=$(secret "脱机告警 Webhook URL（http(s)://...，填 - 清除；输入不回显）")
            if [[ $value == - ]]; then
                printf -- -
                return
            fi
        else
            value=$(secret "脱机告警 Webhook URL（http(s)://...；输入不回显）")
        fi
        if [[ ${#value} -le 1024 && $value =~ ^https?://[^[:space:]]+$ ]]; then
            printf '%s' "$value"
            return
        fi
        if [[ $allow_clear == 1 ]]; then
            echo "  请输入不超过 1024 字符的 http(s):// 地址，或填 - 清除" >&2
        else
            echo "  请输入不超过 1024 字符的 http(s):// 地址" >&2
        fi
    done
}

configure_account() {
    local file credentials bind port terminals environment broker user app front market_front strategy_memory_limit_mb
    local identity saved_identity password='' auth_code=''
    local webhook_file=$CREDENTIALS/asterion.trading.$INSTANCE.webhook webhook='' clear_webhook='' raw_hook=''
    file=$(account_file)
    credentials=$CREDENTIALS/asterion-trading-$INSTANCE
    CHANGES=()
    field bind "监听地址" bind '^[0-9A-Fa-f.:]+$'
    port=$(ask_free_port)
    note "监听端口" "$(current port)" "$port"
    field environment "柜台环境 (real/simulation)" environment '^(real|simulation)$'
    field broker "CTP 经纪商代码" broker.broker_id '^[A-Za-z0-9_]{1,10}$'
    field user "CTP 投资者账号" broker.user_id '^[A-Za-z0-9_]{1,15}$'
    field app "CTP AppID" broker.app_id '^[A-Za-z0-9_.-]{1,32}$'
    field front "CTP 交易前置" broker.front '^tcp://[^[:space:]]+:[0-9]+$'
    field market_front "CTP 行情前置" market_front '^tcp://[A-Za-z0-9.-]+:[0-9]+$'
    number_field strategy_memory_limit_mb "策略内存上限 (MiB)" strategy_memory_limit_mb 512 65536
    identity="$environment $broker $user"
    saved_identity="$(current environment) $(current broker.broker_id) $(current broker.user_id)"
    if [[ -f $file && -f $ROOT/data/$INSTANCE/journal.sqlite && $identity != "$saved_identity" ]]; then
        echo "  $ROOT/data/$INSTANCE 中已有交易记录，已永久绑定原柜台账户身份；未保存任何修改" >&2
        return
    fi
    terminals=$(current terminals)
    [[ -f $file ]] || terminals=$(add_pins)
    if ! [[ -f $file && $identity == "$saved_identity" && -f $credentials/password && -f $credentials/auth_code ]] ||
        ! confirm "保留已保存的 CTP 密码与授权码？" y; then
        password=$(secret "CTP 交易密码（输入不回显）")
        auth_code=$(secret "CTP 授权码（输入不回显）")
        CHANGES+=("CTP 密码与授权码: 已更新")
    fi
    if [[ -f $webhook_file ]]; then
        if ! confirm "保留已保存的脱机告警 Webhook URL？" y; then
            raw_hook=$(ask_webhook_value 1)
            if [[ $raw_hook == - ]]; then
                webhook='' clear_webhook=1
                CHANGES+=("脱机告警 Webhook URL: 已清除")
            else
                webhook=$raw_hook
                CHANGES+=("脱机告警 Webhook URL: 已更新")
            fi
        fi
    elif confirm "是否配置脱机告警 Webhook URL？" n; then
        webhook=$(ask_webhook_value 0)
        CHANGES+=("脱机告警 Webhook URL: 已设置")
    fi
    if [[ -f $file ]] && ! review; then return; fi
    commit_account_settings "$file" "$credentials" "$password" "$auth_code" "$webhook_file" "$webhook" "$clear_webhook"
}

trading_expect_ctp() {
    [[ -f $ROOT/data/$INSTANCE/journal.sqlite ]] && echo 1 || echo 0
}

check_account_login() {
    local credentials=$CREDENTIALS/asterion-trading-$INSTANCE output='' rc=0
    if [[ ! -f $credentials/password || ! -f $credentials/auth_code ]]; then
        echo "${C_RED}✗ 未找到账户 $INSTANCE 的加密 CTP 凭据，请先在配置中录入密码与授权码${C_RESET}"
        return 1
    fi
    echo "正在向柜台单次验证账户 $INSTANCE 的 CTP 凭据（不重试、不发送委托）..."
    output=$(systemd-run --wait --pipe --collect --quiet \
        -p "User=$ACCOUNT" \
        -p "UMask=0077" \
        -p "ReadWritePaths=$ROOT/data/$INSTANCE $ROOT/data/owners" \
        -p "LoadCredentialEncrypted=password:$credentials/password" \
        -p "LoadCredentialEncrypted=auth_code:$credentials/auth_code" \
        -p "NoNewPrivileges=yes" \
        -p "ProtectSystem=strict" \
        -p "ProtectHome=yes" \
        -p "PrivateTmp=yes" \
        -p "CapabilityBoundingSet=" \
        -p "RestrictSUIDSGID=yes" \
        -- "$ROOT/bin/asterion_trading" --account "$INSTANCE" --check-login 2>&1) || rc=$?
    report_ctp_check_result "$rc" "$output"
}

account_settings_menu() {
    local file credentials webhook_file choice
    local bind port terminals environment broker user app front market_front strategy_memory_limit_mb
    local password auth_code raw_hook encrypted_password encrypted_auth_code encrypted_webhook
    file=$(account_file)
    credentials=$CREDENTIALS/asterion-trading-$INSTANCE
    webhook_file=$CREDENTIALS/asterion.trading.$INSTANCE.webhook
    while true; do
        bind=$(current bind) port=$(current port) terminals=$(current terminals)
        environment=$(current environment) broker=$(current broker.broker_id) user=$(current broker.user_id)
        app=$(current broker.app_id) front=$(current broker.front) market_front=$(current market_front)
        strategy_memory_limit_mb=$(current strategy_memory_limit_mb)
        echo
        hr_single
        echo " ${C_BOLD}账户配置与凭据（$INSTANCE）${C_RESET}"
        hr_single
        echo " 1) 监听地址与端口        $bind:$port"
        echo " 2) 柜台前置与 AppID      $app / $front / $market_front"
        echo " 3) 策略内存上限          ${strategy_memory_limit_mb} MiB"
        echo " 4) ${C_YELLOW}更新 CTP 密码与授权码${C_RESET} $([[ -f $credentials/password ]] && echo 已加密保存 || echo 未设置)"
        echo " 5) ${C_YELLOW}脱机告警 Webhook URL${C_RESET}  $([[ -f $webhook_file ]] && echo 已配置 || echo 未配置)"
        echo " 6) ${C_CYAN}逐项向导修改全部${C_RESET}"
        echo " 0) 返回上级菜单"
        hr_single
        read -r -p "请输入对应数字 [0]> " choice || break
        CHANGES=()
        case ${choice:-0} in
            1)
                field bind "监听地址" bind '^[0-9A-Fa-f.:]+$'
                port=$(ask_free_port)
                note "监听端口" "$(current port)" "$port"
                review && commit_account_settings "$file" "$credentials" "" "" "$webhook_file" "" "" ;;
            2)
                field app "CTP AppID" broker.app_id '^[A-Za-z0-9_.-]{1,32}$'
                field front "CTP 交易前置" broker.front '^tcp://[^[:space:]]+:[0-9]+$'
                field market_front "CTP 行情前置" market_front '^tcp://[A-Za-z0-9.-]+:[0-9]+$'
                if review; then
                    commit_account_settings "$file" "$credentials" "" "" "$webhook_file" "" ""
                    if confirm "是否立即单次验证 CTP 凭据连通性？" y; then check_account_login || true; fi
                fi ;;
            3)
                number_field strategy_memory_limit_mb "策略内存上限 (MiB)" strategy_memory_limit_mb 512 65536
                review && commit_account_settings "$file" "$credentials" "" "" "$webhook_file" "" "" ;;
            4)
                password=$(secret "CTP 交易密码（输入不回显）")
                auth_code=$(secret "CTP 授权码（输入不回显）")
                CHANGES+=("CTP 密码与授权码: 已更新")
                if review; then
                    install -d -m 0700 "$credentials"
                    encrypted_password='' encrypted_auth_code=''
                    seal encrypted_password "$credentials/password" password "$password"
                    seal encrypted_auth_code "$credentials/auth_code" auth_code "$auth_code"
                    password='' auth_code=''
                    mv -f "$encrypted_password" "$credentials/password"
                    mv -f "$encrypted_auth_code" "$credentials/auth_code"
                    CHANGED=1
                    echo "已加密保存 CTP 密码与授权码"
                    if confirm "是否立即单次验证 CTP 凭据连通性？" y; then check_account_login || true; fi
                fi
                password='' auth_code='' ;;
            5)
                raw_hook=$(ask_webhook_value "$([[ -f $webhook_file ]] && echo 1 || echo 0)")
                if [[ $raw_hook == - ]]; then
                    CHANGES+=("脱机告警 Webhook URL: 已清除")
                    if review; then
                        rm -f "$webhook_file"
                        CHANGED=1
                        echo "已清除脱机告警 Webhook URL"
                    fi
                else
                    CHANGES+=("脱机告警 Webhook URL: $([[ -f $webhook_file ]] && echo 已更新 || echo 已设置)")
                    if review; then
                        install -d -m 0700 "$CREDENTIALS"
                        encrypted_webhook=''
                        seal encrypted_webhook "$webhook_file" "asterion.trading.$INSTANCE.webhook" "$raw_hook"
                        raw_hook=''
                        mv -f "$encrypted_webhook" "$webhook_file"
                        CHANGED=1
                        echo "已加密保存脱机告警 Webhook URL"
                    fi
                fi
                raw_hook='' ;;
            6)
                local before_changed=$CHANGED
                CHANGED=''
                configure_account
                if [[ -n $CHANGED ]]; then
                    if confirm "是否立即单次验证 CTP 凭据连通性？" y; then check_account_login || true; fi
                else
                    CHANGED=$before_changed
                fi ;;
            0) break ;;
            *) echo "  无效选项，请重新输入" >&2 ;;
        esac
    done
}

apply_account() {
    systemctl enable "asterion-trading@$INSTANCE.service" >/dev/null 2>&1
    if restart_after_trading "asterion-trading@$INSTANCE.service" "$RESTART-$INSTANCE" \
        "asterion_trading-$INSTANCE" "$(current port)" "$(trading_expect_ctp)"; then
        if [[ -f $ROOT/data/$INSTANCE/journal.sqlite ]]; then
            echo "重启后交易授权已重置，请在 Terminal 中重新授权账户 $INSTANCE。"
        else
            echo "账户 $INSTANCE 服务已就绪，请在 Terminal 中完成开通与授权。"
        fi
    fi
}

add_account() {
    INSTANCE=$(ask "账户实例名（与 Terminal 中填写的账户名一致）" "" "$ACCOUNT_NAME")
    if [[ -f $(account_file) ]]; then
        account_menu
        return
    fi
    CHANGED=
    configure_account
    [[ -n $CHANGED ]] || return 0
    if confirm "是否立即单次验证 CTP 凭据连通性？" y; then
        local rc=0
        check_account_login || rc=$?
        if ((rc == 1)) && confirm "是否进入配置菜单修正凭据或前置设置？" y; then
            account_settings_menu
        fi
    fi
    echo "请在 Terminal 中填写此主机地址、端口 $(current port)、账户名 $INSTANCE 与本机证书指纹。"
    firewall "$(current port)"
    apply_account
}

start_account() {
    cancel_restart "$RESTART-$INSTANCE"
    CHANGED=''
    if start_and_verify enable-now "asterion-trading@$INSTANCE.service" \
        "asterion_trading-$INSTANCE" "$(current port)" "$(trading_expect_ctp)"; then
        echo "请在 Terminal 中完成授权。"
    fi
}

DISABLED=

disable_account() {
    confirm "确认立即停止并停用账户 $INSTANCE？柜台上的挂单将保留，交易授权将结束。" n || return 0
    cancel_restart "$RESTART-$INSTANCE"
    systemctl disable --now "asterion-trading@$INSTANCE.service" >/dev/null 2>&1
    DISABLED=1
    echo "已停止并停用账户 $INSTANCE（配置与交易记录仍保留在 $ROOT/data/$INSTANCE）。"
}

account_menu() {
    local choice unit="asterion-trading@$INSTANCE.service" running_text boot_text env_badge timer_at pins
    CHANGED='' DISABLED=''
    while true; do
        if unit_running "$unit"; then
            running_text="${C_GREEN}● 运行中${C_RESET}"
        else
            running_text="${C_RED}○ 未运行${C_RESET}"
        fi
        if unit_enabled "$unit"; then
            boot_text="${C_GREEN}已开启开机自启！${C_RESET}"
        else
            boot_text="${C_YELLOW}未开启开机自启${C_RESET}"
        fi
        if [[ $(current environment) == real ]]; then
            env_badge="${C_RED}[实盘 real]${C_RESET}"
        else
            env_badge="${C_CYAN}[仿真 simulation]${C_RESET}"
        fi
        timer_at=''
        if systemctl is-active --quiet "$RESTART-$INSTANCE.timer"; then
            timer_at=$(systemctl show "$RESTART-$INSTANCE.timer" --property=NextElapseUSecRealtime --value)
            timer_at=${timer_at:-已排程}
        fi
        read -r -a pins <<<"$(current terminals)"
        echo
        hr_double
        printf ' %s账户管理: %s%s %s        监听: %s:%s\n' "$C_BOLD" "$INSTANCE" "$C_RESET" "$env_badge" "$(current bind)" "$(current port)"
        hr_single
        printf ' %s   %s   已授权终端: %d 台\n' "$running_text" "$boot_text" "${#pins[@]}"
        echo " 柜台身份: $(current broker.broker_id) / $(current broker.user_id) (AppID: $(current broker.app_id))"
        [[ -z $timer_at ]] || echo " ${C_YELLOW}待执行项: 收盘后自动重启 ($timer_at)${C_RESET}"
        [[ -z $CHANGED ]] || echo " ${C_YELLOW}待生效项: 返回上级（选 0）或选 1 重启后生效${C_RESET}"
        hr_double
        echo " 1) ${C_GREEN}启动 / 重启账户服务${C_RESET}"
        echo " 2) ${C_CYAN}配置与凭据设置${C_RESET}"
        echo " 3) ${C_RED}停止并停用账户${C_RESET}"
        echo " 4) ${C_YELLOW}开机自启开关${C_RESET}"
        echo " 5) ${C_CYAN}终端指纹管理${C_RESET}"
        echo " 6) 查看运行日志"
        echo " 7) ${C_CYAN}测试 CTP 凭据连通性${C_RESET}"
        echo
        echo " 0) 返回上级菜单"
        hr_double
        read -r -p "请输入对应数字 [0]> " choice || break
        case ${choice:-0} in
            1)
                if unit_running "$unit"; then
                    if confirm "确认立即重启账户 $INSTANCE？交易授权将结束，柜台挂单保留且不会重发。" n; then
                        if restart_now "$unit" "$RESTART-$INSTANCE" \
                            "asterion_trading-$INSTANCE" "$(current port)" "$(trading_expect_ctp)"; then
                            echo "请在 Terminal 中重新授权账户 $INSTANCE。"
                        fi
                    fi
                else
                    start_account
                fi ;;
            2) account_settings_menu ;;
            3)
                disable_account
                if [[ -n $DISABLED ]]; then return; fi ;;
            4)
                if unit_enabled "$unit"; then
                    systemctl disable "$unit" >/dev/null 2>&1
                    echo "已关闭开机自启：$unit"
                else
                    systemctl enable "$unit" >/dev/null 2>&1
                    echo "已开启开机自启：$unit"
                fi ;;
            5) pins_menu "$(account_file)" terminals ;;
            6) show_logs "asterion_trading-$INSTANCE" "$unit" ;;
            7) check_account_login || true ;;
            0) break ;;
            *) echo "  无效选项，请重新输入" >&2 ;;
        esac
    done
    if [[ -n $CHANGED ]]; then apply_account; fi
}

list_accounts() {
    local name unit state_label boot_label pending i=0
    echo "    账户              监听地址              运行状态  开机自启  待执行重启"
    for name in $(accounts); do
        i=$((i + 1))
        unit="asterion-trading@$name.service"
        if unit_running "$unit"; then state_label="${C_GREEN}运行中${C_RESET}"; else state_label="${C_RED}未运行${C_RESET}"; fi
        if unit_enabled "$unit"; then boot_label="${C_GREEN}已开启${C_RESET}"; else boot_label="${C_YELLOW}未开启${C_RESET}"; fi
        pending="无"
        if systemctl is-active --quiet "$RESTART-$name.timer"; then
            pending=$(systemctl show "$RESTART-$name.timer" --property=NextElapseUSecRealtime --value)
            pending=${pending:-已排程}
        fi
        printf '%2d) %-18s %-22s %s    %s    %s\n' "$i" "$name" \
            "$(setting "$(account_file "$name")" bind ""):$(setting "$(account_file "$name")" port "")" \
            "$state_label" "$boot_label" "$pending"
    done
    if ((i == 0)); then
        echo "    （尚未配置任何交易账户）"
    fi
}

menu() {
    local choice names add_index ver
    ver=$(service_version "$ROOT/bin/asterion_trading")
    while true; do
        mapfile -t names < <(accounts)
        add_index=$((${#names[@]} + 1))
        echo
        hr_double
        printf ' %s欢迎使用 Asterion Trading !%s                 Ver: %s\n' "$C_TITLE" "$C_RESET" "$ver"
        echo " 快捷命令: sudo $PACKAGE"
        echo " 证书指纹: $(certificate)"
        hr_single
        list_accounts
        hr_double
        ((${#names[@]} == 0)) || echo " 1-${#names[@]}) ${C_CYAN}进入对应账户管理${C_RESET}"
        echo " ${add_index}) ${C_GREEN}新增交易账户${C_RESET}"
        echo
        echo " 0) 退出脚本"
        hr_double
        read -r -p "请输入对应数字 [0]> " choice || break
        case ${choice:-0} in
            a) add_account ;;
            0) break ;;
            *)
                if [[ $choice =~ ^[0-9]{1,3}$ ]]; then
                    if ((choice >= 1 && choice <= ${#names[@]})); then
                        INSTANCE=${names[choice - 1]}
                        account_menu
                    elif ((choice == add_index)); then
                        add_account
                    else
                        echo "  无效选项，请重新输入" >&2
                    fi
                else
                    echo "  无效选项，请重新输入" >&2
                fi ;;
        esac
    done
}

unit_text() {
    cat <<EOF
[Unit]
Description=Asterion trading service, account %i
Wants=network-online.target time-sync.target
After=network-online.target time-sync.target
StartLimitIntervalSec=0

[Service]
User=$ACCOUNT
UMask=0077
ExecStart=$ROOT/bin/asterion_trading --account %i
ReadWritePaths=$ROOT/logs $ROOT/data/%i $ROOT/data/owners
LoadCredentialEncrypted=password:$CREDENTIALS/asterion-trading-%i/password
LoadCredentialEncrypted=auth_code:$CREDENTIALS/asterion-trading-%i/auth_code
ImportCredential=asterion.trading.%i.*
Restart=always
RestartSec=5
WatchdogSec=60
NotifyAccess=main
TimeoutStopSec=30
MemoryAccounting=yes
MemoryMax=2048M
MemorySwapMax=0
OOMPolicy=continue
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

install_once() {
    install_base "$1"
    mkdir -p "$ROOT"/{lib,config/accounts,data,data/owners}
    chmod 0700 "$ROOT/config/accounts" "$ROOT/data" "$ROOT/data/owners"
    chown "$ACCOUNT:" "$ROOT/config/accounts" "$ROOT/data" "$ROOT/data/owners"
    write_unit
}

place() {
    local name next
    next=$(mktemp -d "$ROOT/.program.XXXXXX")
    CLEANUP+=("$next")
    for name in bin lib runtime; do
        cp -a "$1/$name" "$next/$name"
    done
    chmod -R a+rX,go-w "$next"
    chmod 0755 "$next/bin/asterion_trading" "$next/bin/setup.sh" "$next/bin/cold_backup.py"
    "$next/bin/asterion_trading" --version >/dev/null 2>&1 || fail "当前发布包中的 Trading 无法在此主机运行"
    "$next/runtime/python/bin/python3" -I -B -c \
        'import sys, numpy, scipy, google.protobuf; assert sys.version_info[:3] == (3, 12, 12); assert numpy.__version__ == "2.3.5"; assert scipy.__version__ == "1.17.1"; assert google.protobuf.__version__ == "5.29.3"; sys.path.insert(0, sys.argv[1]); import asterion_strategy, asterion.strategy_pb2' \
        "$next/lib/strategy" || fail "当前发布包中的策略运行时无法在此主机运行"
    touch "$ROOT/.program-incomplete"
    for name in bin lib runtime; do
        rm -rf "${ROOT:?}/${name:?}"
        mv "$next/$name" "$ROOT/$name"
    done
    write_unit
    rm "$ROOT/.program-incomplete"
}

setup() {
    if [[ $1 == "$ROOT" ]]; then
        menu
    else
        mkdir -p "$ROOT"
        touch "$ROOT/.program.lock"
        chmod 0644 "$ROOT/.program.lock"
        local program_lock name state
        exec {program_lock}<"$ROOT/.program.lock"
        flock --exclusive --nonblock "$program_lock" || fail "请先停止本机所有 Trading 账户实例后再升级；未改动任何程序"
        if [[ -d $ROOT/data ]]; then
            for name in $(accounts); do
                state=$(systemctl show "asterion-trading@$name.service" --property=ActiveState --value 2>/dev/null || true)
                [[ $state != active && $state != activating && $state != reloading && $state != deactivating ]] ||
                    fail "请先停止本机所有 Trading 账户实例后再升级（$name 当前状态：$state）；未改动任何程序"
            done
            for name in $(accounts); do cancel_restart "$RESTART-$name"; done
            mkdir -p "$ROOT/data/owners"
            chmod 0700 "$ROOT/data/owners"
            chown "$ACCOUNT:" "$ROOT/data/owners"
            for name in $(accounts); do
                mkdir -p "$ROOT/data/$name"
                chmod 0700 "$ROOT/data/$name"
                chown "$ACCOUNT:" "$ROOT/data/$name"
            done
            place "$1"
            place_shortcut
            for INSTANCE in $(accounts); do
                refresh_settings "$(account_file)" current_account
                write_memory_dropin "$INSTANCE" "$(current strategy_memory_limit_mb)"
            done
            INSTANCE=
            exec {program_lock}<&-
            echo "已完成程序与离线策略运行时升级：$ROOT（业务数据未改动；请显式启动账户并在 Terminal 重新允许下单）。"
            menu
        else
            install_once "$1"
            place "$1"
            place_shortcut
            exec {program_lock}<&-
            echo
            hr_double
            echo " 已安装至 $ROOT（运行账户：$ACCOUNT）"
            echo " 后续管理可直接运行：sudo $PACKAGE"
            hr_single
            fingerprint
            hr_single
            add_account
            hr_double
        fi
    fi
}
