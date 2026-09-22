#!/usr/bin/env bash
#
# Turn a fresh bare metal Ubuntu box into the benchmark box in docs/LINUX_SETUP.md.
#
# Run this once, as root, on a machine you own the whole of. It installs the
# toolchain, writes the isolation boot line, and reboots. Nothing here is
# reversible by accident but all of it is reversible on purpose, and the last
# section says how.
#
# This half does the part that needs a reboot. scripts/linux_box_run.sh is the
# other half and runs after the machine comes back.
#
#   sudo ./scripts/linux_box_setup.sh            # cores 2 and 3 isolated
#   sudo TICK_ISOL=4,5 ./scripts/linux_box_setup.sh
#
# A virtual machine or a container will run this script happily and the result
# will be a lie, because the isolated core is still a thread the host schedules.
# The script refuses on a hypervisor for that reason rather than warning about it.
#
set -euo pipefail

ISOL="${TICK_ISOL:-2,3}"

[[ $EUID -eq 0 ]] || { echo "run this as root" >&2; exit 1; }

# Refuse to produce isolation numbers on hardware that cannot give isolation.
# systemd-detect-virt exits non zero when it finds nothing, which is the case
# this script wants, so take the output and ignore the status.
HYPER="$(systemd-detect-virt 2>/dev/null)" || true
HYPER="${HYPER:-none}"
if [[ "$HYPER" != "none" ]]; then
    cat >&2 <<EOF
This machine reports itself as running under "$HYPER".

An isolated core inside a guest is still a vCPU thread that the host scheduler
can preempt, so the run would look isolated without being isolated. Use real
bare metal, or set TICK_ALLOW_VIRT=1 if you have a reason and intend to label
every number that comes off this box as virtualised.
EOF
    [[ "${TICK_ALLOW_VIRT:-0}" == "1" ]] || exit 1
fi

NCPU="$(nproc)"
for c in ${ISOL//,/ }; do
    [[ "$c" -lt "$NCPU" ]] || { echo "core $c does not exist, this box has $NCPU" >&2; exit 1; }
done

echo "== installing the toolchain"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
    build-essential cmake ninja-build git pkg-config \
    zlib1g-dev liburing-dev \
    python3 python3-pip curl ca-certificates >/dev/null

# cpupower and the kernel tools are per kernel version and are not always
# packaged for the running one. They set the governor, which changes the number
# but not whether the run works, so a missing package is reported rather than
# fatal. box_label.sh prints the governor as unknown if it cannot read it.
apt-get install -y -qq linux-tools-common "linux-tools-$(uname -r)" cpufrequtils >/dev/null 2>&1 \
    || echo "   cpupower not available for kernel $(uname -r), the governor will stay as it is"

# The frequency driver differs by vendor and the wrong parameter is silently
# ignored, which would leave the clock moving under the measurement.
VENDOR="$(awk -F: '/vendor_id/ {gsub(/ /,"",$2); print $2; exit}' /proc/cpuinfo)"
if [[ "$VENDOR" == "AuthenticAMD" ]]; then
    PSTATE="amd_pstate=disable"
else
    PSTATE="intel_pstate=disable"
fi

CMDLINE="isolcpus=${ISOL} nohz_full=${ISOL} rcu_nocbs=${ISOL} ${PSTATE} idle=poll"

echo "== writing the boot line"
echo "   $CMDLINE"
cp /etc/default/grub "/etc/default/grub.tickerplant.bak.$(date +%s)"
if grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=' /etc/default/grub; then
    sed -i "s|^GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT=\"${CMDLINE}\"|" /etc/default/grub
else
    echo "GRUB_CMDLINE_LINUX_DEFAULT=\"${CMDLINE}\"" >> /etc/default/grub
fi
update-grub 2>/dev/null || grub-mkconfig -o /boot/grub/grub.cfg

# irqbalance moves interrupts onto whichever core is quiet, which is exactly the
# core the receiver is on. Disable it now so it does not come back at boot.
systemctl disable --now irqbalance 2>/dev/null || true

cat <<EOF

== done, rebooting in 5 seconds

After the box comes back, from the repository root:

    ./scripts/linux_box_run.sh

To undo all of this, restore the saved /etc/default/grub backup, run
update-grub, and reboot.
EOF
sleep 5
reboot
