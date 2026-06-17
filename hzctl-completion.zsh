#compdef hzctl
# Zsh completion for hzctl.
# Install: cp hzctl-completion.zsh /usr/share/zsh/site-functions/_hzctl
# Or: fpath=(. $fpath); compdef _hzctl hzctl

_hzctl() {
    local -a cmds services
    cmds=(list status start stop restart reload daemon-reload enable disable show logs shutdown poweroff reboot help)

    if (( CURRENT == 2 )); then
        _values 'command' "${cmds[@]}"
        return
    fi

    local cmd="${words[2]}"
    if (( CURRENT == 3 )); then
        case "$cmd" in
            start|stop|restart|reload|status|enable|disable)
                services=(${(f)"$(hzctl list 2>/dev/null | awk 'NF>=1 && $1!~/^\(/ {print $1}')"})
                _values 'service' "${services[@]}"
                return
                ;;
        esac
    fi

    # Gentoo-style: service name first, action second
    if (( CURRENT == 2 )); then
        services=(${(f)"$(hzctl list 2>/dev/null | awk 'NF>=1 && $1!~/^\(/ {print $1}')"})
        _values 'service (Gentoo-style)' "${services[@]}"
        return
    fi
    if (( CURRENT == 3 )); then
        _values 'action' start stop restart status reload
        return
    fi
}

_hzctl "$@"
