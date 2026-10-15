#!/bin/sh
# qdc-web standalone v1: asynchronous SysV boot hook, no blocking boot waits.
case "${1:-}" in
    start)
        if [ -f /usrdata/qdc-web/private/standalone-enabled ] && [ -x /usrdata/qdc-web/standalone/service.sh ]; then
            /usrdata/qdc-web/standalone/service.sh start || true
        fi ;;
    stop) /usrdata/qdc-web/standalone/service.sh stop || true ;;
    *) exit 2 ;;
esac
exit 0
