#compdef hzctl
# Zsh completion for hzctl (SOV: <name> <action>).
# Install: cp hzctl-completion.zsh /usr/share/zsh/site-functions/_hzctl
# Or: fpath=(. $fpath); compdef _hzctl hzctl

_hzctl() {
    local -a top_cmds actions services
    top_cmds=(list status show logs reload daemon-reload shutdown poweroff reboot help)
    actions=(start stop restart reload status enable disable)

    if (( CURRENT == 2 )); then
        # First word: top-level command OR service/target name.
        services=(${(f)"$(hzctl list 2>/dev/null | awk 'NF>=1 && $1!~/^\(/ {print $1}')"})
        _values 'command or service' "${top_cmds[@]}" "${services[@]}"
        return
    fi

    local first="${words[2]}"
    if (( CURRENT == 3 )); then
        # Second word: action verb (if first was a service) or arg (if first was top-level).
        case "$first" in
            list|show|reload|daemon-reload|shutdown|poweroff|reboot|help)
                return  # no useful completion
                ;;
            status)
                services=(${(f)"$(hzctl list 2>/dev/null | awk 'NF>=1 && $1!~/^\(/ {print $1}')"})
                _values 'service' "${services[@]}"
                return
                ;;
            logs)
                _values 'lines' 10 20 50 100
                return
                ;;
            *)
                # First word was a service/target — complete the action.
                _values 'action' "${actions[@]}"
                return
                ;;
        esac
    fi
}

_hzctl "$@"
