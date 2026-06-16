#!/usr/bin/env python3

# Launcher for the slim OSv unikernel under QEMU.
#
# The kernel boots diskless via QEMU -kernel (PVH on x86_64, the loader.img
# preboot on aarch64) with the single application linked in. There is no
# filesystem, no block device for a root image, and no networking, so this
# script is a thin QEMU wrapper: memory/cpus, an optional emulated NVMe drive,
# optional PCI passthrough, and the serial console / gdb stub.

import subprocess
import sys
import argparse
import os
import errno

stty_params = None

devnull = open('/dev/null', 'w')

osv_base = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

host_arch = os.uname().machine

def stty_save():
    global stty_params
    p = subprocess.Popen(["stty", "-g"], stdout=subprocess.PIPE, stderr=devnull)
    stty_params, err = p.communicate()
    stty_params = stty_params.strip()

def stty_restore():
    if stty_params:
        subprocess.call(["stty", stty_params], stderr=devnull)

def cleanups():
    "cleanups after execution"
    stty_restore()

def format_args(args):
    def format_arg(arg):
        if arg == '':
            return '""'
        elif ' ' in arg:
            return '"%s"' % arg
        elif arg[0] == '-':
            return '\\\n' + arg
        else:
            return arg

    return ' '.join(map(format_arg, args))

def set_imgargs(options):
    # The slim kernel does not parse a guest command line; -append is passed for
    # completeness only. Honour an explicit --execute or a build/<opt>/cmdline
    # file if one exists, otherwise leave it empty.
    execute = options.execute or ""
    if not execute:
        cmdline_path = "build/%s/cmdline" % options.opt_path
        if os.path.exists(cmdline_path):
            execute = open(cmdline_path).read()
    options.osv_cmdline = execute

def start_osv_qemu(options):
    args = [
        "-m", options.memsize,
        "-smp", options.vcpus]

    if not options.novnc and options.hypervisor != 'qemu_microvm' and options.arch == 'x86_64':
        args += ["-vnc", options.vnc]
    else:
        args += ["--nographic"]

    if not options.nogdb:
        args += ["-gdb", "tcp::%s,server,nowait" % options.gdb]

    if options.graphics:
        args += ["-display", "sdl"]

    # Always loaded via QEMU -kernel; the kernel boots diskless.
    args += [
        "-kernel", options.kernel_file,
        "-append", options.osv_cmdline]

    if options.arch == 'aarch64':
        if options.hypervisor == 'qemu':
            args += ["-machine", "gic-version=%s" % options.gic_version, "-cpu", "cortex-a57"]
        args += ["-machine", "virt"]

    if options.hypervisor == 'qemu_microvm' and options.arch != 'aarch64':
        args += [
        "-M", "microvm,x-option-roms=off,pit=off,pic=off,rtc=off,auto-kernel-cmdline=on,acpi=off",
        "-nodefaults", "-no-user-config", "-no-reboot", "-global", "virtio-mmio.force-legacy=off"]

    # Optional emulated NVMe drive (e.g. a backing store for the in-kernel app).
    if options.emulated_nvme:
        args += [
        "-drive", "file=%s,if=none,id=nvm1" % options.emulated_nvme,
        "-device", "nvme,serial=deadbeef,drive=nvm1"]

    # PCI passthrough: one -device per address. The devices must be bound to
    # vfio-pci on the host, and QEMU must run with enough privilege (sudo).
    for pci in options.pass_pci or []:
        args += ["-device", "vfio-pci,host=%s" % pci]

    if options.no_shutdown:
        args += ["-no-reboot", "-no-shutdown"]

    if options.wait:
        args += ["-S"]

    # Networking was removed from the slim kernel: no NIC at all.
    args += ["-nic", "none"]

    if options.hypervisor != 'qemu_microvm':
        args += ["-device", "virtio-rng-pci"]

    if options.hypervisor == "kvm" or options.hypervisor == 'qemu_microvm':
        if options.arch == 'aarch64':
            args += ["-enable-kvm", "-cpu", "host", "-machine", "gic-version=max"]
        else:
            args += ["-enable-kvm", "-cpu", "host,+x2apic"]

    if options.hypervisor == 'qemu_microvm':
        args += ["-serial", "stdio"]
    elif options.detach:
        args += ["-daemonize"]
    elif options.arch == 'x86_64':
        signal_option = ('off', 'on')[options.with_signals]
        args += ["-chardev", "stdio,mux=on,id=stdio,signal=%s" % signal_option]
        args += ["-mon", "chardev=stdio,mode=readline"]
        args += ["-device", "isa-serial,chardev=stdio"]

    for a in options.pass_args or []:
        args += a.split()

    qemu_path = options.qemu_path or os.environ.get('QEMU_PATH') or ('qemu-system-%s' % options.arch)
    cmdline = [qemu_path] + args

    if options.dry_run:
        print(format_args(cmdline))
        return

    try:
        stty_save()
        ret = subprocess.call(cmdline, env=os.environ.copy())
        if ret != 0:
            sys.exit("qemu failed.")
    except OSError as e:
        if e.errno == errno.ENOENT:
            print("'%s' binary not found. Please install the qemu-system-%s package." %
                (qemu_path, options.arch), file=sys.stderr)
        else:
            print("OS error(%d): \"%s\" while running %s" %
                (e.errno, e.strerror, " ".join(cmdline)), file=sys.stderr)
    finally:
        cleanups()

def choose_hypervisor(arch):
    if os.path.exists('/dev/kvm') and arch == host_arch:
        return 'kvm'
    return 'qemu'

def main(options):
    set_imgargs(options)
    if options.hypervisor not in ("none", "qemu", "qemu_microvm", "kvm"):
        print("Unrecognized hypervisor selected", file=sys.stderr)
        return
    start_osv_qemu(options)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(prog='run')
    parser.add_argument("-d", "--debug", action="store_true",
                        help="start debug version")
    parser.add_argument("-r", "--release", action="store_true",
                        help="start release version")
    parser.add_argument("-w", "--wait", action="store_true",
                        help="don't start OSv till otherwise specified, e.g. through the QEMU monitor or a remote gdb")
    parser.add_argument("-m", "--memsize", action="store", default="2G",
                        help="specify memory: ex. 1G, 2G, ...")
    parser.add_argument("-c", "--vcpus", action="store", default="4",
                        help="specify number of vcpus")
    parser.add_argument("-e", "--execute", action="store", default=None, metavar="CMD",
                        help="edit the kernel command line before execution")
    parser.add_argument("-p", "--hypervisor", action="store", default="auto",
                        help="choose hypervisor to run: kvm, qemu, qemu_microvm, none (plain qemu)")
    parser.add_argument("-D", "--detach", action="store_true",
                        help="run in background, do not connect the console")
    parser.add_argument("-H", "--no-shutdown", action="store_true",
                        help="don't restart qemu automatically (allow debugger to connect on early errors)")
    parser.add_argument("-s", "--with-signals", action="store_true", default=False,
                        help="qemu only. handle signals instead of passing keys to the guest. pressing ctrl+c from console will kill the emulator")
    parser.add_argument("-g", "--graphics", action="store_true",
                        help="Enable graphics mode.")
    parser.add_argument("--dry-run", action="store_true",
                        help="do not run, just print the command line")
    parser.add_argument("--vnc", action="store", default=":1",
                        help="specify vnc port number")
    parser.add_argument("--novnc", action="store_true",
                        help="disable vnc")
    parser.add_argument("--nogdb", action="store_true",
                        help="disable gdb")
    parser.add_argument("--gdb", action="store", default="1234",
                        help="specify gdb port number")
    parser.add_argument("--pass-args", action="append",
                        help="pass arguments to the underlying QEMU command line")
    parser.add_argument("--qemu-path", action="store",
                        help="specify qemu command path")
    parser.add_argument("--kernel-path", action="store",
                        help="path to the kernel. defaults to build/$mode/loader-stripped.elf (loader.img on aarch64)")
    parser.add_argument("--arch", action="store", choices=["x86_64", "aarch64"], default=host_arch,
                        help="specify QEMU architecture: x86_64, aarch64")
    parser.add_argument("--emulated-nvme", action="store", metavar="IMAGE",
                        help="path to a raw disk image to attach as an emulated NVMe device")
    parser.add_argument("--pass-pci", action="store", nargs='+', metavar="ADDR",
                        help="passthrough one or more PCI devices (bound to vfio-pci), e.g. 0000:01:00.0")
    parser.add_argument("--gic-version", action="store", default="max",
                        help="specify GIC version (only applicable on aarch64)")
    cmdargs = parser.parse_args()

    cmdargs.opt_path = "debug" if cmdargs.debug else "release" if cmdargs.release else "last"
    if cmdargs.arch == 'aarch64':
        default_kernel_file_name = "loader.img"
    else:
        default_kernel_file_name = "loader-stripped.elf"
    cmdargs.kernel_file = os.path.abspath(cmdargs.kernel_path or
        os.path.join(osv_base, "build/%s/%s" % (cmdargs.opt_path, default_kernel_file_name)))
    if not os.path.exists(cmdargs.kernel_file):
        raise Exception('Kernel file %s does not exist.' % cmdargs.kernel_file)

    if cmdargs.hypervisor == "auto":
        cmdargs.hypervisor = choose_hypervisor(cmdargs.arch)

    main(cmdargs)
