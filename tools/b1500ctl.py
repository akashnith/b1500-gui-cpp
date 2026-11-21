#!/usr/bin/env python3
"""Talk to a running B1500A Neuromorphic GUI from a terminal on this machine.

The GUI opens a loopback control port when the operator ticks the box on its
Assistant tab. This reads the port and session token the GUI wrote, and sends
commands to it. Nothing here can reach a GUI that has the box unticked, and
nothing here can bypass a safety gate in the GUI: a hardware run still raises
the confirmation dialog on screen and still needs output enabled there.

    python tools/b1500ctl.py state
    python tools/b1500ctl.py catalog --grep pund
    python tools/b1500ctl.py select fe.pund pund_amplitude_v=3 pund_width_s=1e-6
    python tools/b1500ctl.py run --wait 300
    python tools/b1500ctl.py samples --max 40
    python tools/b1500ctl.py note "P* is flat against top width - ramp-limited."

Standard library only, so it runs under `python -I` with no environment.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

HANDSHAKE_ENV = "B1500_BRIDGE_HANDSHAKE"


def handshake_path():
    """Where the GUI writes its port and token."""
    override = os.environ.get(HANDSHAKE_ENV)
    if override:
        return override
    roaming = os.environ.get("APPDATA")
    if roaming:
        # Matches QStandardPaths::AppDataLocation for this application.
        return os.path.join(roaming, "B1500A", "Neuromorphic GUI", "assistant-bridge.json")
    return os.path.expanduser("~/.local/share/B1500A/Neuromorphic GUI/assistant-bridge.json")


def load_handshake():
    path = handshake_path()
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except FileNotFoundError:
        sys.exit(
            "No control port is open.\n"
            "  Start the GUI, open its Assistant tab, and tick\n"
            "  'Let a terminal session on this machine read this window and run measurements'.\n"
            "  Expected handshake file: %s" % path
        )
    except (OSError, ValueError) as error:
        sys.exit("Could not read %s: %s" % (path, error))


def call(method, path, payload=None, timeout=120):
    handshake = load_handshake()
    url = "http://127.0.0.1:%d%s" % (handshake["port"], path)
    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    request = urllib.request.Request(url, data=data, method=method)
    request.add_header("X-Bridge-Token", handshake["token"])
    if data is not None:
        request.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        try:
            return json.loads(error.read().decode("utf-8"))
        except ValueError:
            return {"ok": False, "error": "HTTP %d" % error.code}
    except urllib.error.URLError as error:
        sys.exit(
            "Could not reach the GUI on 127.0.0.1:%d (%s).\n"
            "Is it still running with the control port enabled?" % (handshake["port"], error.reason)
        )


def parse_assignments(pairs):
    """Turn k=v arguments into a parameter dictionary."""
    parameters = {}
    for pair in pairs:
        if "=" not in pair:
            sys.exit("Expected key=value, got: %s" % pair)
        key, _, value = pair.partition("=")
        try:
            parameters[key.strip()] = float(value)
        except ValueError:
            sys.exit("Parameter %s needs a number, got: %s" % (key, value))
    return parameters


def show(payload):
    print(json.dumps(payload, indent=2, sort_keys=False))
    return 0 if payload.get("ok", True) else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("state", help="everything on screen in the GUI right now")

    catalog = sub.add_parser("catalog", help="every protocol and its parameters")
    catalog.add_argument("--grep", help="only protocols matching this text")
    catalog.add_argument("--family", help="only this family")
    catalog.add_argument("--full", action="store_true", help="include parameter specs")

    log = sub.add_parser("log", help="the GUI's run log")
    log.add_argument("-n", "--lines", type=int, default=60)

    select = sub.add_parser("select", help="choose a protocol and set parameters, without running")
    select.add_argument("protocol")
    select.add_argument("assignments", nargs="*", metavar="key=value")
    select.add_argument("--demo", action="store_true",
                        help="switch the GUI into demo mode first (it can never be "
                             "switched out of demo mode from here)")
    select.add_argument("--compliance", type=float, default=None, metavar="AMPS",
                        help='current compliance in amps. It is a page setting rather than a protocol parameter, so it does not go through key=value.')

    run = sub.add_parser("run", help="start a measurement")
    run.add_argument("protocol", nargs="?", help="omit to run what is already selected")
    run.add_argument("assignments", nargs="*", metavar="key=value")
    run.add_argument("--wait", type=int, default=0, metavar="SECONDS",
                     help="block until the run finishes, then print its result")
    run.add_argument("--demo", action="store_true",
                     help="switch the GUI into demo mode first")
    run.add_argument("--compliance", type=float, default=None, metavar="AMPS",
                     help='current compliance in amps. It is a page setting rather than a protocol parameter, so it does not go through key=value.')

    wait = sub.add_parser("wait", help="block until the next run finishes")
    wait.add_argument("--after", type=int, default=None)
    wait.add_argument("--timeout", type=int, default=120)

    result = sub.add_parser("result", help="metrics from the last run")
    result.add_argument("family", nargs="?")

    samples = sub.add_parser("samples", help="measured points from the last run")
    samples.add_argument("family", nargs="?")
    samples.add_argument("--max", type=int, default=100)

    note = sub.add_parser("note", help="write a line into the GUI's run log")
    note.add_argument("text", nargs="+")

    sub.add_parser("stop", help="stop the running measurement")

    args = parser.parse_args(argv)

    if args.command == "state":
        return show(call("GET", "/state"))

    if args.command == "catalog":
        payload = call("GET", "/catalog")
        protocols = payload.get("protocols", [])
        if args.family:
            protocols = [p for p in protocols
                         if p["family"].lower() == args.family.lower()]
        if args.grep:
            needle = args.grep.lower()
            protocols = [p for p in protocols
                         if needle in (p["id"] + p["name"] + p["description"]).lower()]
        if args.full:
            return show({"ok": True, "routing": payload.get("routing"), "protocols": protocols})
        print(payload.get("routing", ""))
        for protocol in protocols:
            print("%-22s %-10s %-7s %s" % (protocol["id"], protocol["family"],
                                           protocol["backend_at_defaults"], protocol["name"]))
        return 0

    if args.command == "log":
        payload = call("GET", "/log?lines=%d" % args.lines)
        for line in payload.get("lines", []):
            print(line)
        return 0

    if args.command == "select":
        body = {"protocol": args.protocol, "parameters": parse_assignments(args.assignments)}
        if args.demo:
            body["demo_mode"] = True
        if args.compliance is not None:
            body["compliance_a"] = args.compliance
        return show(call("POST", "/select", body))

    if args.command == "run":
        body = {}
        if args.protocol:
            body["protocol"] = args.protocol
        if args.assignments:
            body["parameters"] = parse_assignments(args.assignments)
        if args.demo:
            body["demo_mode"] = True
        if args.compliance is not None:
            body["compliance_a"] = args.compliance
        started = call("POST", "/run", body)
        if not started.get("ok") or not args.wait:
            return show(started)
        finished = call("GET", "/wait?after=%d&timeout_s=%d"
                        % (started["serial_before"], args.wait), timeout=args.wait + 30)
        return show(finished)

    if args.command == "wait":
        query = "/wait?timeout_s=%d" % args.timeout
        if args.after is not None:
            query += "&after=%d" % args.after
        return show(call("GET", query, timeout=args.timeout + 30))

    if args.command == "result":
        query = "/result" + ("?family=%s" % args.family if args.family else "")
        return show(call("GET", query))

    if args.command == "samples":
        query = "/samples?max=%d" % args.max
        if args.family:
            query += "&family=%s" % args.family
        return show(call("GET", query))

    if args.command == "note":
        return show(call("POST", "/note", {"text": " ".join(args.text)}))

    if args.command == "stop":
        return show(call("POST", "/stop", {}))

    return 1


if __name__ == "__main__":
    sys.exit(main())
