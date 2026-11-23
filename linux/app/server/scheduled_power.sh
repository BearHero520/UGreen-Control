#!/bin/bash
# Container power actions must not masquerade as NAS power management.
echo '定时开关机尚未适配；请使用 DSM / TrueNAS 系统设置。' >&2
exit 95
