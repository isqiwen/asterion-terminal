# Market、Warehouse 与 Trading 共用的安装与运维面板逻辑。构建时追加在各服务自身的
# scripts/setup.sh 之后，二者合并为单文件发布包或已安装的 bin/setup.sh。

shopt -s inherit_errexit

UNITS=/etc/systemd/system
CREDENTIALS=/etc/credstore.encrypted
SHORTCUT_DIR=/usr/local/bin
ACCOUNT=
SELF=$(readlink -f "${BASH_SOURCE[0]}")

C_RESET='' C_BOLD='' C_DIM='' C_GREEN='' C_RED='' C_YELLOW='' C_CYAN='' C_TITLE=''
init_colors() {
    if [[ -t 1 && -z ${NO_COLOR:-} && ${TERM:-dumb} != dumb ]]; then
        C_RESET=$'\033[0m'
        C_BOLD=$'\033[1m'
        C_DIM=$'\033[2m'
        C_GREEN=$'\033[32m'
        C_RED=$'\033[31m'
        C_YELLOW=$'\033[33m'
        C_CYAN=$'\033[36m'
        C_TITLE=$'\033[30;43m'
    fi
}

hr_double() {
    echo "${C_DIM}============================================================${C_RESET}"
}

hr_single() {
    echo "${C_DIM}------------------------------------------------------------${C_RESET}"
}

fail() {
    echo "错误: $*" >&2
    exit 1
}

CLEANUP=()
cleanup() {
    ((${#CLEANUP[@]})) && rm -rf "${CLEANUP[@]}"
    return 0
}
trap cleanup EXIT

stage() {
    local file
    file=$(mktemp "$(dirname "$2")/.$(basename "$2").XXXXXX")
    CLEANUP+=("$file")
    printf -v "$1" '%s' "$file"
}

seal() {
    stage "$1" "$2"
    printf '%s' "$4" | systemd-creds encrypt --name="$3" - "${!1}" >/dev/null
}

private() {
    chown "$ACCOUNT:" "$1"
    chmod 0600 "$1"
}

place_shortcut() {
    mkdir -p "$SHORTCUT_DIR"
    ln -sfn "$ROOT/bin/setup.sh" "$SHORTCUT_DIR/$PACKAGE"
}

service_version() {
    local out
    out=$("$1" --version 2>/dev/null || true)
    out=${out##* }
    printf '%s' "${out:--}"
}

unit_running() {
    systemctl is-active --quiet "$1"
}

unit_enabled() {
    local out
    out=$(systemctl is-enabled "$1" 2>/dev/null) || true
    [[ $out == enabled ]]
}

# ask PROMPT DEFAULT PATTERN: 读取匹配正则的输入；交互终端预填默认值。
ask() {
    local value
    while true; do
        if [[ -t 0 ]]; then
            read -r -e -i "$2" -p "$1: " value || fail "输入意外结束"
        else
            read -r -p "$1 [$2]: " value || fail "输入意外结束"
        fi
        value=${value:-$2}
        if [[ $value =~ $3 ]]; then
            printf '%s' "$value"
            return
        fi
        echo "  输入格式无效，请重新输入" >&2
    done
}

ask_number() {
    local value
    while true; do
        value=$(ask "$1 ($3-$4)" "$2" '^[0-9]{1,9}$')
        value=$((10#$value))
        if ((value >= $3 && value <= $4)); then
            printf '%s' "$value"
            return
        fi
        echo "  请输入 $3 到 $4 之间的整数" >&2
    done
}

CHANGES=()

note() {
    [[ $2 == "$3" ]] || CHANGES+=("$1: ${2:-无} -> ${3:-无}")
}

field() {
    local value
    value=$(ask "$2" "$(current "$3")" "$4")
    note "$2" "$(current "$3")" "$value"
    printf -v "$1" '%s' "$value"
}

number_field() {
    local value
    value=$(ask_number "$2" "$(current "$3")" "$4" "$5")
    note "$2" "$(current "$3")" "$value"
    printf -v "$1" '%s' "$value"
}

review() {
    if ((${#CHANGES[@]} == 0)); then
        echo "未作任何修改。"
        return 1
    fi
    echo "待保存的变更："
    printf '  %s\n' "${CHANGES[@]}"
    confirm "确认保存以上变更？" y
}

secret() {
    local value
    while true; do
        read -r -s -p "$1: " value || fail "凭据输入意外结束"
        echo >&2
        [[ -n $value ]] && break
        echo "  此项不能为空" >&2
    done
    printf '%s' "$value"
}

confirm() {
    local answer
    if [[ $2 == y ]]; then
        read -r -p "$1 [Y/n]: " answer || fail "输入意外结束"
        [[ $answer != [nN]* ]]
    else
        read -r -p "$1 [y/N]: " answer || fail "输入意外结束"
        [[ $answer == [yY]* ]]
    fi
}

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

numbered() {
    local i=0 item
    for item; do
        i=$((i + 1))
        printf '  %d) %s\n' "$i" "$item"
    done
}

ask_pin() {
    local value
    while true; do
        read -r -p "粘贴 Terminal 证书指纹（直接回车结束）: " value || fail "输入意外结束"
        value=${value//[:[:space:]]/}
        value=${value,,}
        if [[ -z $value || $value =~ ^[0-9a-f]{64}$ ]]; then
            printf '%s' "$value"
            return
        fi
        echo "  证书指纹须为 64 位十六进制字符（见 Terminal 设置 → 关于）" >&2
    done
}

add_pins() {
    local pins=("$@") pin
    echo "请从 Terminal「设置 → 关于」复制并粘贴客户端证书指纹。" >&2
    while ((${#pins[@]} < 16)); do
        pin=$(ask_pin)
        if [[ -z $pin ]]; then
            ((${#pins[@]} == 0)) || break
            echo "  至少需要登记一台 Terminal" >&2
        elif [[ " ${pins[*]} " == *" $pin "* ]]; then
            echo "  该指纹已登记" >&2
        else
            pins+=("$pin")
        fi
    done
    printf '%s' "${pins[*]}"
}

remove_pins() {
    local pins=("$@") kept numbers number
    while true; do
        read -r -p "输入要移除的编号（空格分隔，直接回车取消）: " numbers || fail "输入意外结束"
        kept=("${pins[@]}")
        for number in $numbers; do
            if ! [[ $number =~ ^[0-9]{1,2}$ ]] || ((number < 1 || number > ${#pins[@]})); then
                echo "  请输入 1 到 ${#pins[@]} 之间的编号" >&2
                continue 2
            fi
            unset 'kept[number - 1]'
        done
        ((${#kept[@]} == 0)) || break
        echo "  至少须保留一台已授权 Terminal" >&2
    done
    printf '%s' "${kept[*]}"
}

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
    echo "已保存 $2"
}

pins_menu() {
    local file=$1 key=$2 choice list
    while true; do
        read -r -a list <<<"$(current "$key")"
        echo
        hr_single
        echo " ${C_BOLD}终端指纹管理${C_RESET}（已授权 ${#list[@]} 台）"
        hr_single
        if ((${#list[@]} == 0)); then
            echo "  （尚未登记任何 Terminal）"
        else
            numbered "${list[@]}"
        fi
        hr_single
        echo " 1) ${C_GREEN}添加终端指纹${C_RESET}"
        echo " 2) ${C_YELLOW}移除终端指纹${C_RESET}"
        echo " 0) 返回上级菜单"
        hr_single
        read -r -p "请输入对应数字 [0]> " choice || break
        case ${choice:-0} in
            1) edit_pins add "$file" "$key" ;;
            2) edit_pins remove "$file" "$key" ;;
            0) break ;;
            *) echo "  无效选项，请重新输入" >&2 ;;
        esac
    done
}

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
changes = [f"  {key}: 已移除" if key not in new else f"  {key}: 新增默认值 {show(new[key])}" if key not in old
           else f"  {key}: {show(old[key])} -> {show(new[key])}" for key in sorted(old.keys() | new.keys())
           if old.get(key, ...) != new.get(key, ...)]
if not changes:
    raise SystemExit(1)
print(f"已将 {sys.argv[1]} 升级为当前版本格式：")
print("\n".join(changes))' "$1" "$1" "$next" || return 0
    private "$next"
    mv -f "$next" "$1"
    CHANGED=1
}

data_directory() {
    mkdir -p "$2"
    chmod 0700 "$2"
    chown "$ACCOUNT:" "$2"
    if [[ $2 != "$1" && -d $1 && -n $(ls -A "$1") ]]; then
        echo "原有数据仍保留在 $1 未自动搬迁；如需继续使用，请先停止 $NAME 再将其内容移至 $2。"
    fi
}

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

cancel_restart() {
    systemctl stop "$1.timer" "$1.service" 2>/dev/null || true
}

restart_after_trading() {
    local at=now
    if unit_running "$1"; then
        # shellcheck disable=SC2046
        at=$(restart_time_at $(TZ=Asia/Shanghai date +'%u %H%M'))
    fi
    cancel_restart "$2"
    case $at in
        now)
            systemctl restart "$1"
            echo "已重启 $1（日志目录：$ROOT/logs）"
            return ;;
        "+1 "*) at="$(TZ=Asia/Shanghai date -d tomorrow +%F) ${at#+1 }" ;;
        *) at="$(TZ=Asia/Shanghai date +%F) $at" ;;
    esac
    systemd-run --quiet --collect --unit="$2" --on-calendar="$at:00 Asia/Shanghai" systemctl restart "$1"
    echo "$1 当前处于交易时段：变更已安排在北京时间 $at 收盘后自动重启生效（取消定时：systemctl stop $2.timer）"
}

restart_now() {
    [[ -z ${2:-} ]] || cancel_restart "$2"
    systemctl restart "$1"
    CHANGED=''
    echo "已重启 $1（日志目录：$ROOT/logs）"
}

show_logs() {
    local prefix=$1 latest='' file
    for file in "$ROOT"/logs/"$prefix"*.log; do
        [[ -f $file ]] && latest=$file
    done
    echo
    hr_single
    if [[ -n $latest ]]; then
        echo " 最近日志：$latest（末尾 30 行）"
        hr_single
        tail -n 30 "$latest"
    else
        echo " 暂无业务日志文件（目录：$ROOT/logs）"
        if [[ -n ${2:-} ]] && command -v journalctl >/dev/null 2>&1; then
            hr_single
            journalctl -u "$2" -n 20 --no-pager 2>/dev/null || true
        fi
    fi
    hr_single
}

certificate() {
    openssl x509 -noout -fingerprint -sha256 -in "$ROOT/config/tls/server.crt" | sed 's/^.*=//'
}

fingerprint() {
    echo "本机服务端证书指纹（请填入 Terminal）："
    echo "  $(certificate)"
}

firewall() {
    echo "防火墙放行提示：请开放 TCP 端口 $1（例如：sudo ufw allow $1/tcp）"
}

create_identity() {
    local directory=$ROOT/config/tls staging
    if [[ -f $directory/server.crt && -f $directory/server.key ]]; then return; fi
    [[ ! -e $directory/server.crt && ! -e $directory/server.key ]] || fail "$directory 中的 TLS 证书不完整，请保留现场并检查"
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

write_unit() {
    local unit
    stage unit "$UNITS/$UNIT"
    unit_text >"$unit"
    chmod 0644 "$unit"
    mv -f "$unit" "$UNITS/$UNIT"
    systemctl daemon-reload
}

place_programs() {
    local name next
    "$1/${PROGRAMS[0]}" --version >/dev/null 2>&1 || fail "当前发布包中的 $NAME 无法在此主机运行"
    next=$(mktemp -d "$ROOT/.program.XXXXXX")
    CLEANUP+=("$next")
    for name in "${PROGRAMS[@]}"; do
        mkdir -p "$(dirname "$next/$name")"
        install -m 0755 "$1/$name" "$next/$name"
    done
    touch "$ROOT/.program-incomplete"
    for name in "${PROGRAMS[@]}"; do
        mkdir -p "$(dirname "$ROOT/$name")"
        mv -f "$next/$name" "$ROOT/$name"
    done
    place_shortcut
}

CHANGED=

service_menu() {
    local choice running_text boot_text timer_at pins ver
    ver=$(service_version "$ROOT/${PROGRAMS[0]}")
    while true; do
        if unit_running "$UNIT"; then
            running_text="${C_GREEN}● 运行中${C_RESET}"
        else
            running_text="${C_RED}○ 未运行${C_RESET}"
        fi
        if unit_enabled "$UNIT"; then
            boot_text="${C_GREEN}已开启开机自启！${C_RESET}"
        else
            boot_text="${C_YELLOW}未开启开机自启${C_RESET}"
        fi
        timer_at=''
        if [[ -n ${TIMER:-} ]] && systemctl is-active --quiet "$TIMER.timer"; then
            timer_at=$(systemctl show "$TIMER.timer" --property=NextElapseUSecRealtime --value)
            timer_at=${timer_at:-已排程}
        fi
        read -r -a pins <<<"$(current clients)"
        echo
        hr_double
        printf ' %s欢迎使用 Asterion %s !%s%*sVer: %s\n' "$C_TITLE" "$NAME" "$C_RESET" "$((24 - ${#NAME}))" "" "$ver"
        echo " 快捷命令: sudo $PACKAGE"
        hr_single
        printf ' %s   %s\n' "$running_text" "$boot_text"
        echo " 监听地址: $(current bind):$(current port)    已授权终端: ${#pins[@]} 台"
        echo " 证书指纹: $(certificate)"
        [[ -z $timer_at ]] || echo " ${C_YELLOW}待执行项: 收盘后自动重启 ($timer_at)${C_RESET}"
        [[ -z $CHANGED ]] || echo " ${C_YELLOW}待生效项: 退出面板（选 0）或选 1 重启后生效${C_RESET}"
        hr_double
        echo " 1) ${C_GREEN}启动 / 重启服务${C_RESET}"
        echo " 2) ${C_CYAN}配置与凭据设置${C_RESET}"
        echo " 3) ${C_RED}停止服务${C_RESET}"
        echo " 4) ${C_YELLOW}开机自启开关${C_RESET}"
        echo " 5) ${C_CYAN}终端指纹管理${C_RESET}"
        echo " 6) 查看运行日志"
        echo
        echo " 0) 退出脚本"
        hr_double
        read -r -p "请输入对应数字 [0]> " choice || break
        case ${choice:-0} in
            1)
                if [[ -n ${TIMER:-} ]] && unit_running "$UNIT"; then
                    local window_at
                    # shellcheck disable=SC2046
                    window_at=$(restart_time_at $(TZ=Asia/Shanghai date +'%u %H%M'))
                    if [[ $window_at != now || -n $timer_at ]]; then
                        confirm "确认立即重启 $NAME（不等交易时段结束）？" n || continue
                    fi
                    restart_now "$UNIT" "$TIMER"
                else
                    apply
                    CHANGED=''
                fi ;;
            2) settings_menu ;;
            3)
                if confirm "确认停止 $NAME 服务？" n; then
                    [[ -z ${TIMER:-} ]] || cancel_restart "$TIMER"
                    systemctl stop "$UNIT"
                    CHANGED=''
                    echo "已停止 $UNIT"
                fi ;;
            4)
                if unit_enabled "$UNIT"; then
                    systemctl disable "$UNIT" >/dev/null 2>&1
                    echo "已关闭开机自启：$UNIT"
                else
                    systemctl enable "$UNIT" >/dev/null 2>&1
                    echo "已开启开机自启：$UNIT"
                fi ;;
            5) pins_menu "$ROOT/${SETTINGS:?}" clients ;;
            6) show_logs "asterion_${NAME,,}" "$UNIT" ;;
            0) break ;;
            *) echo "  无效选项，请重新输入" >&2 ;;
        esac
    done
}

install_or_configure() {
    if [[ $1 == "$ROOT" ]]; then
        service_menu
    else
        mkdir -p "$ROOT"
        touch "$ROOT/.program.lock"
        chmod 0644 "$ROOT/.program.lock"
        local program_lock
        exec {program_lock}<"$ROOT/.program.lock"
        flock --exclusive --nonblock "$program_lock" || fail "另一项 $NAME 安装或升级正在进行；未改动任何程序"
        if [[ -x $ROOT/${PROGRAMS[0]} ]]; then
            place_programs "$1"
            write_unit
            if declare -F place_exchange_rules >/dev/null; then place_exchange_rules; fi
            rm -f "$ROOT/.program-incomplete"
            exec {program_lock}<&-
            echo "已完成程序升级：$ROOT"
            refresh_settings "$ROOT/$SETTINGS" current_settings
            CHANGED=1
            service_menu
        else
            install_base "$1"
            place_programs "$1"
            write_unit
            if declare -F place_exchange_rules >/dev/null; then place_exchange_rules; fi
            rm -f "$ROOT/.program-incomplete"
            systemctl enable "$UNIT" >/dev/null
            exec {program_lock}<&-
            echo
            hr_double
            echo " 已安装至 $ROOT（运行账户：$ACCOUNT）"
            echo " 后续管理可直接运行：sudo $PACKAGE"
            hr_single
            configure
            hr_single
            firewall "$(current port)"
            fingerprint
            hr_double
        fi
    fi
    if [[ -n $CHANGED ]]; then apply; fi
}

main() {
    init_colors
    [[ $EUID -eq 0 ]] || fail "请使用 sudo 运行此脚本"
    if [[ ${1:-} == fingerprint ]]; then
        fingerprint
        return
    fi
    local source line
    line=$(LC_ALL=C awk '/^__ARCHIVE__$/ { print NR + 1; exit }' "$SELF")
    if [[ -n $line ]]; then
        # The services judge quote freshness and dates by this host's clock.
        [[ $(timedatectl show -p NTPSynchronized --value 2>/dev/null) == yes ]] ||
            fail "主机时钟未同步：先开启时间同步（如 sudo timedatectl set-ntp true）并等待同步完成，再安装或升级"
        source=$(mktemp -d)
        CLEANUP+=("$source")
        tail -n +"$line" "$SELF" | tar xz --no-same-owner -C "$source"
        source=$source/$PACKAGE
    elif [[ $(dirname "$SELF") == "$ROOT/bin" ]]; then
        source=$ROOT
    else
        fail "此脚本不包含安装包内容；请使用完整发布包 Asterion-$NAME-VERSION-linux-x86_64.sh 安装或升级"
    fi
    if [[ -d $ROOT/config ]]; then
        ACCOUNT=$(stat -c %U "$ROOT/config")
    else
        ACCOUNT=${SUDO_USER:-}
        [[ -n $ACCOUNT && $ACCOUNT != root ]] || fail "请从普通用户通过 sudo 执行安装（$NAME 将以该用户身份运行）"
    fi
    setup "$source"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
    exit
fi
return
