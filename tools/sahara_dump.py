#!/usr/bin/env python3
"""Read ranges of the phone's RAM while it sits in Qualcomm CrashDump (USB 05c6:900e, Sahara memory-debug mode).

usage: sahara_dump.py [--reset] OUTDIR [ADDR:LEN ...]     (numbers in any base; default: the UEFI's area and the boot framebuffer)
  --reset: after the reads, send Sahara's RESET (command 7), which reboots the phone out of CrashDump

The device says hello, we answer in memory-debug mode, it announces its memory table (command 0x10); after that it
serves READ_DATA_64 (0x11) requests. Wire format as seen in edl's log (bkerler/edl, GPL-3) -- this is a small
independent client that reads only the ranges asked for. One session per CrashDump entry.
"""
import os, struct, sys, time
import usb.core, usb.util

VID, PID = 0x05C6, 0x900E
DEFAULT = ['0x9c000000:0xa80000',   # the boot framebuffer: 1080 x 2376 x 4, where the DEBUG UEFI's text is drawn
           '0x80791000:0x100',      # PMIC_PON, PMON_HIS, RST_STAT, FSM_STS/CTRL: why the SoC reset
           '0x80000000:0xff1f0',    # CD_STRCT + CD_BTDDR: the crash-dump structures
           '0x9fff7000:0x9000']     # the stock UEFI log ring and info block (not ours)


def main():
    args = [a for a in sys.argv[1:] if a != '--reset']
    do_reset = '--reset' in sys.argv[1:]
    out = args[0]; os.makedirs(out, exist_ok=True)
    ranges = [tuple(int(x, 0) for x in a.split(':')) for a in (args[1:] or DEFAULT)]
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.exit('no 05c6:900e device (the phone must be in CrashDump)')
    try:
        dev.set_configuration()
    except usb.core.USBError:
        pass
    cfg = dev.get_active_configuration()
    intf = None
    for i in cfg:
        eps = {usb.util.endpoint_direction(e.bEndpointAddress): e for e in i if usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK}
        if len(eps) == 2:
            intf, ep_in, ep_out = i, eps[usb.util.ENDPOINT_IN], eps[usb.util.ENDPOINT_OUT]
            break
    if intf is None:
        sys.exit('no bulk interface')
    print('interface %d, in %#x out %#x' % (intf.bInterfaceNumber, ep_in.bEndpointAddress, ep_out.bEndpointAddress))

    def rx(n, timeout=5000):
        data = b''
        while len(data) < n:
            chunk = bytes(ep_in.read(min(n - len(data), 1 << 20) if n - len(data) >= 512 else 512, timeout))
            data += chunk
            if len(chunk) < 512 and len(data) < n and n < 512:
                break
        return data

    def rx_exact(n, timeout=20000):
        buf = bytearray()
        while len(buf) < n:
            want = n - len(buf)
            buf += bytes(ep_in.read(max(want, 512) if want < 512 else min(want, 1 << 20), timeout))
        return bytes(buf[:n])

    hello = bytes(ep_in.read(0x400, 5000))
    cmd, ln = struct.unpack_from('<II', hello)
    print('device sent cmd %#x len %#x: %s' % (cmd, ln, hello[:ln].hex()))
    if cmd != 1:
        sys.exit('not a HELLO: the Sahara session is not fresh (re-enter CrashDump)')
    ver, minver, maxpkt, mode = struct.unpack_from('<IIII', hello, 8)
    print('version %d min %d max packet %#x mode %d' % (ver, minver, maxpkt, mode))
    ep_out.write(struct.pack('<IIIIIIIIIIII', 2, 0x30, ver, minver, 0, mode, 1, 2, 3, 4, 5, 6))
    hdr = bytes(ep_in.read(0x400, 8000))
    cmd, ln = struct.unpack_from('<II', hdr)
    print('next: cmd %#x len %#x %s' % (cmd, ln, hdr[:ln].hex()))
    if cmd not in (0x10, 0x09):
        sys.exit('unexpected command')
    if cmd == 0x10:
        taddr, tlen = struct.unpack_from('<QQ', hdr, 8)
    else:
        taddr, tlen = struct.unpack_from('<II', hdr, 8)

    def read(addr, length, name):
        t0 = time.time()
        pkt = struct.pack('<IIQQ', 0x11 if cmd == 0x10 else 0x0A, 0x18, addr, length) if cmd == 0x10 else struct.pack('<IIII', 0x0A, 0x10, addr, length)
        ep_out.write(pkt, 15000)
        data = rx_exact(length)
        time.sleep(0.3)
        while True:                      # a zero-length terminator or leftovers must not block the next request
            try:
                if not bytes(ep_in.read(512, 300)):
                    break
            except usb.core.USBError:
                break
        open(os.path.join(out, name), 'wb').write(data)
        print('%-28s %#012x +%#x  %.1f MB/s' % (name, addr, length, length / 1e6 / max(time.time() - t0, 1e-6)))
        return data

    table = read(taddr, tlen, 'memory_table.bin')
    for i in range(0, len(table) - 63, 64):
        pref, base, size = struct.unpack_from('<QQQ', table, i)
        desc = table[i + 24:i + 44].split(b'\0')[0].decode('latin1'); fn = table[i + 44:i + 64].split(b'\0')[0].decode('latin1')
        print('  table: %-18s %-28s base %#014x size %#x' % (fn, desc, base, size))
    for addr, length in ranges:
        try:
            read(addr, length, 'ram_%x_%x.bin' % (addr, length))
        except usb.core.USBError as e:
            print('read %#x +%#x failed: %s' % (addr, length, e)); break
    print('done')
    if do_reset:
        try:
            ep_out.write(struct.pack('<II', 7, 8), 5000)
            print('Sahara RESET sent: the phone reboots')
        except usb.core.USBError as e:
            print('reset: %s' % e)

main()
