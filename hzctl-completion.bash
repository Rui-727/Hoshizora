# Bash completion for hzctl.
# Install: cp hzctl-completion.bash /etc/bash_completion.d/hzctl
# Or: source hzctl-completion.bash in your .bashrc

_hzctl() {
    local cur prev cmds services
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    cmds="list status start stop restart reload daemon-reload enable disable show logs shutdown poweroff reboot help"

    # If we're completing the first word, suggest commands.
    if [ "$COMP_CWORD" -eq 1 ]; then
        COMPREPLY=( $(compgen -W "$cmds" -- "$cur") )
        return 0
    fi

    # If the previous word is a command that takes a service name, complete
    # from the running service list. deferred: calls hzctl list — slow if
    # hoshizora isn't running. Could cache, but YAGNI for an interactive shell.
    case "$prev" in
        start|stop|restart|reload|status|enable|disable)
            services=$(hzctl list 2>/dev/null | awk 'NF>=1 && $1!~/^\(/ {print $1}')
            COMPREPLY=( $(compgen -W "$services" -- "$cur") )
            return 0
            ;;
    esac

    # Gentoo-style: if first word is a service name, suggest actions.
    if [ "$COMP_CWORD" -eq 2 ]; then
        case "${COMP_WORDS[1]}" in
            start|stop|restart|reload|status) ;;
            *)
                COMPREPLY=( $(compgen -W "start stop restart status reload" -- "$cur") )
                return 0
                ;;
        esac
    fi

    return 0
}
complete -F _hzctl hzctl
