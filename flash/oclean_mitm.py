#!/usr/bin/env python3
"""
Root orchestrator to MITM the Oclean X Ultra 20's own Wi-Fi OTA traffic on your
LAN so its stock firmware fetches a firmware image *you* serve instead of the
cloud image. This is the first-install path (stock -> custom) on a unit with no
accessible UART. Your own brush, your own network. See flash/README.md.

What it does, as root:
  * enables IPv4 forwarding and REDIRECTs the brush's TCP :80/:443 to a local
    mitmproxy (transparent mode),
  * ARP-spoofs the brush<->gateway pair so that traffic actually reaches us,
  * runs mitmdump with the chosen addon (default: the flash addon),
  * (optional) captures all brush traffic to a pcap.
On Ctrl-C / SIGTERM it restores the ARP tables and removes every network change
it made.

Usage:
  sudo python3 oclean_mitm.py --brush 192.168.1.50 [--addon oclean_flash_addon.py]

Interface and gateway are auto-detected from the default route; override with
--iface / --gw if that guess is wrong. Run `--help` for all options.

Requires (in the Python that runs this, as root): scapy, mitmproxy. `dumpcap` or
`tshark` is optional (for the pcap). `mitmdump` must be on PATH or given with
--mitmdump.
"""
import argparse
import atexit
import os
import signal
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def sh(cmd):
    subprocess.run(cmd, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def sh_out(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()


def default_route():
    """Return (iface, gateway) from the system default route, or (None, None)."""
    out = sh_out("ip -4 route show default")
    iface = gw = None
    for tok in out.split():
        if tok == "dev":
            iface = out.split("dev", 1)[1].split()[0]
        if tok == "via":
            gw = out.split("via", 1)[1].split()[0]
    return iface, gw


def parse_args():
    env = os.environ.get
    iface_guess, gw_guess = default_route()
    p = argparse.ArgumentParser(
        description="MITM the Oclean X Ultra 20 OTA so it flashes your image.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--brush", default=env("OCLEAN_BRUSH"),
                   help="brush IP on your LAN (required; find it in your router's DHCP table)")
    p.add_argument("--iface", default=env("OCLEAN_IFACE", iface_guess),
                   help="LAN interface the brush is on")
    p.add_argument("--gw", default=env("OCLEAN_GW", gw_guess),
                   help="LAN gateway IP")
    p.add_argument("--port", type=int, default=int(env("OCLEAN_PORT", "8080")),
                   help="local mitmproxy listen port")
    p.add_argument("--addon", default=env("OCLEAN_ADDON", os.path.join(HERE, "oclean_flash_addon.py")),
                   help="mitmproxy addon script to load")
    p.add_argument("--out", default=env("OCLEAN_OUT", os.path.join(HERE, "captures")),
                   help="directory for logs / pcap (git-ignored)")
    p.add_argument("--mitmdump", default=env("OCLEAN_MITMDUMP", "mitmdump"),
                   help="mitmdump executable (PATH name or absolute path)")
    p.add_argument("--vpn-bypass", action="store_true",
                   default=env("OCLEAN_VPN_BYPASS", "") not in ("", "0", "false", "False"),
                   help="add `ip rule from <brush> lookup main` so forwarded brush traffic "
                        "skips a policy-routing VPN table (e.g. Tailscale). Harmless if you "
                        "have no such VPN.")
    p.add_argument("--no-pcap", action="store_true", help="do not capture a pcap of brush traffic")
    a = p.parse_args()
    if not a.brush:
        p.error("--brush is required (or set OCLEAN_BRUSH)")
    if not a.iface or not a.gw:
        p.error("could not auto-detect --iface/--gw; pass them explicitly "
                "(are you connected to the LAN the brush is on?)")
    return a


def main():
    args = parse_args()
    if os.geteuid() != 0:
        sys.exit("must run as root (sudo) — it changes iptables/ARP. See the module docstring.")

    os.makedirs(args.out, exist_ok=True)

    # scapy is only needed at runtime and only under root; import after the euid check.
    from scapy.all import ARP, send, getmacbyip, get_if_hwaddr, conf  # noqa: E402
    conf.iface = args.iface
    conf.verb = 0
    laptop_mac = get_if_hwaddr(args.iface)

    def resolve(ip, tries=3):
        for _ in range(tries):
            m = getmacbyip(ip)
            if m:
                return m
            time.sleep(0.5)
        return None

    print(f"[*] iface={args.iface} host_mac={laptop_mac} brush={args.brush} gw={args.gw}")
    gw_mac = resolve(args.gw, 6)
    print(f"[*] gateway mac: {gw_mac}")
    if not gw_mac:
        sys.exit("[!] cannot resolve the gateway MAC — are you on the brush's LAN?")

    # ---- network setup (root); each change records its own undo ----
    added = []

    def net_setup():
        sh("sysctl -w net.ipv4.ip_forward=1")
        if args.vpn_bypass:
            rule = f"from {args.brush} lookup main pref 1000"
            sh(f"ip rule add {rule}")
            added.append(f"ip rule del {rule}")
        for dport in (80, 443):
            sh(f"iptables -t nat -A PREROUTING -i {args.iface} -s {args.brush} "
               f"-p tcp --dport {dport} -j REDIRECT --to-port {args.port}")
            added.append(f"iptables -t nat -D PREROUTING -i {args.iface} -s {args.brush} "
                         f"-p tcp --dport {dport} -j REDIRECT --to-port {args.port}")
        print(f"[*] ip_forward on; redirect brush :80/:443 -> mitmproxy :{args.port}"
              + ("; vpn-bypass rule added" if args.vpn_bypass else ""))

    def net_teardown():
        for undo in reversed(added):
            sh(undo)
        print("[*] network rules removed")

    # ---- ARP spoofing ----
    stop = threading.Event()
    brush_mac_box = {"mac": None}

    def spoofer():
        announced = False
        while not stop.is_set():
            bm = brush_mac_box["mac"] or resolve(args.brush, 1)
            if bm:
                brush_mac_box["mac"] = bm
                if not announced:
                    print(f"[+] brush online (mac {bm}) — spoofing active; TRIGGER THE OTA NOW")
                    announced = True
                # tell the brush "the gateway is at us"; tell the gateway "the brush is at us"
                send(ARP(op=2, pdst=args.brush, hwdst=bm, psrc=args.gw, hwsrc=laptop_mac))
                send(ARP(op=2, pdst=args.gw, hwdst=gw_mac, psrc=args.brush, hwsrc=laptop_mac))
            time.sleep(2)

    def arp_restore():
        bm = brush_mac_box["mac"]
        if bm:
            for _ in range(5):
                send(ARP(op=2, pdst=args.brush, hwdst=bm, psrc=args.gw, hwsrc=gw_mac))
                send(ARP(op=2, pdst=args.gw, hwdst=gw_mac, psrc=args.brush, hwsrc=bm))
                time.sleep(0.3)
            print("[*] ARP tables restored")

    pcap_proc = None

    def cleanup(*_):
        stop.set()
        time.sleep(0.2)
        try:
            if pcap_proc:
                pcap_proc.terminate()
        except Exception:
            pass
        arp_restore()
        net_teardown()

    atexit.register(cleanup)
    signal.signal(signal.SIGINT, lambda *_: (cleanup(), sys.exit(0)))
    signal.signal(signal.SIGTERM, lambda *_: (cleanup(), sys.exit(0)))

    net_setup()
    threading.Thread(target=spoofer, daemon=True).start()

    # full packet capture of ALL brush traffic (catches non-80/443 channels too)
    if not args.no_pcap:
        pcap = os.path.join(args.out, "brush.pcap")
        for capbin in ("dumpcap", "tshark"):
            path = sh_out(f"command -v {capbin}")
            if path:
                try:
                    pcap_proc = subprocess.Popen(
                        [path, "-i", args.iface, "-f", f"host {args.brush}", "-w", pcap],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    print(f"[*] pcap of brush traffic -> {pcap} ({capbin})")
                    break
                except Exception as e:
                    print(f"[!] pcap start failed ({capbin}): {e}")

    env = dict(os.environ, BRUSH_IP=args.brush, OUT_DIR=args.out)
    print(f"[*] launching mitmproxy (transparent). addon={args.addon}  logs -> {args.out}")
    print("[*] waiting for the brush; when you see 'TRIGGER THE OTA NOW', start the firmware")
    print("    upgrade from the Oclean app (or reboot the brush on its charger).")
    print("[*] press Ctrl-C when the upgrade finishes (or to stop).\n")
    # transparent mode; block_global=false so it proxies LAN->WAN; ssl-insecure upstream
    # so we still reach the genuine origin even if its chain is odd.
    cmd = [args.mitmdump, "--mode", "transparent", "--listen-host", "0.0.0.0",
           "--listen-port", str(args.port), "--showhost", "--ssl-insecure",
           "--set", "block_global=false", "-s", args.addon]
    try:
        subprocess.run(cmd, env=env)
    finally:
        cleanup()


if __name__ == "__main__":
    main()
