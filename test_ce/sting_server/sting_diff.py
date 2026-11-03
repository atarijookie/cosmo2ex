#!/usr/bin/env python3
"""Compare two logs of the STinG compatibility test (test_ce/test_sting).

    sting_diff.py STNGREAL.LOG STNGFAKE.LOG [--fuzzy]

The first log is the reference (real STinG). Every exact value ('=') of the reference must be in the second log with
the same value. Fuzzy values ('~') are only listed with --fuzzy. Exit code 1 when an exact value differs or is missing.
"""

import argparse
import re
import sys

ERRORS = {
    0: "E_NORMAL", -1: "E_OBUFFULL", -2: "E_NODATA", -3: "E_EOF", -4: "E_RRESET", -5: "E_UA", -6: "E_NOMEM",
    -7: "E_REFUSE", -8: "E_BADSYN", -9: "E_BADHANDLE", -10: "E_LISTEN", -11: "E_NOCCB", -12: "E_NOCONNECTION",
    -13: "E_CONNECTFAIL", -14: "E_BADCLOSE", -15: "E_USERTIMEOUT", -16: "E_CNTIMEOUT", -17: "E_CANTRESOLVE",
    -18: "E_BADDNAME", -19: "E_LOSTCARRIER", -20: "E_NOHOSTNAME", -21: "E_DNSWORKLIMIT", -22: "E_NONAMESERVER",
    -23: "E_DNSBADFORMAT", -24: "E_UNREACHABLE", -25: "E_DNSNOADDR", -26: "E_NOROUTINE", -27: "E_LOCKED",
    -28: "E_FRAGMENT", -29: "E_TTLEXCEED", -30: "E_PARAMETER", -31: "E_BIGBUF", -32: "E_FNAVAIL",
}

STATES = ["TCLOSED", "TLISTEN", "TSYN_SENT", "TSYN_RECV", "TESTABLISH", "TFIN_WAIT1", "TFIN_WAIT2", "TCLOSE_WAIT",
          "TCLOSING", "TLAST_ACK", "TTIME_WAIT"]

LINE = re.compile(r"^([0-9a-fA-F]{4}) ([#=~]) (.*)$")


class Log:
    def __init__(self, path):
        self.path = path
        self.comments = []
        self.names = {}         # test -> name
        self.order = []         # tests in log order
        self.values = {}        # (test, key) -> (cls, value)
        self.keys = {}          # test -> [keys in order]

        with open(path, "rb") as f:
            text = f.read().decode("latin-1")

        for raw in text.splitlines():
            line = raw.rstrip("\r\n")
            if line.startswith("## "):
                self.comments.append(line[3:])
                continue

            m = LINE.match(line)
            if not m:
                continue

            test, cls, rest = m.group(1).lower(), m.group(2), m.group(3)
            if cls == "#":
                if test not in self.names:
                    self.order.append(test)
                    self.keys[test] = []
                self.names[test] = rest
                continue

            key, _, value = rest.partition(" ")
            if test not in self.keys:               # value without header
                self.order.append(test)
                self.keys[test] = []
                self.names[test] = "?"
            if (test, key) not in self.values:
                self.keys[test].append(key)
            self.values[(test, key)] = (cls, value)

    def comment(self, name):
        for c in self.comments:
            if c.startswith(name + " "):
                return c[len(name) + 1:]
        return "?"


def explain(key, value):
    """add the error / state name to a number, so the diff is readable"""
    try:
        num = int(value)
    except ValueError:
        return value

    if "state" in key and 0 <= num < len(STATES):
        return "%s (%s)" % (value, STATES[num])
    if num < 0 and num in ERRORS:           # 0 can be a count or a flag too, only negative numbers are surely errors
        return "%s (%s)" % (value, ERRORS[num])
    return value


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference", help="log from real STinG")
    ap.add_argument("other", help="log from the STinG clone")
    ap.add_argument("--fuzzy", action="store_true", help="also list differing fuzzy values")
    args = ap.parse_args()

    ref = Log(args.reference)
    oth = Log(args.other)

    print("reference: %s  [%s / %s / %s]" % (ref.path, ref.comment("module"), ref.comment("author"),
                                             ref.comment("version")))
    print("other:     %s  [%s / %s / %s]" % (oth.path, oth.comment("module"), oth.comment("author"),
                                             oth.comment("version")))
    print()

    failedTests = 0
    exactBad = 0
    exactCount = 0
    fuzzyDiff = 0

    for test in ref.order:
        lines = []
        if test not in oth.names:
            lines.append("    test missing in the other log")
        else:
            for key in ref.keys[test]:
                cls, rv = ref.values[(test, key)]
                ov = oth.values.get((test, key))

                if cls == "=":
                    exactCount += 1
                    if ov is None:
                        lines.append("    %-14s %-30s | <missing>" % (key, explain(key, rv)))
                        exactBad += 1
                    elif ov[1] != rv:
                        lines.append("    %-14s %-30s | %s" % (key, explain(key, rv), explain(key, ov[1])))
                        exactBad += 1
                else:
                    if ov is not None and ov[1] != rv:
                        fuzzyDiff += 1
                        if args.fuzzy:
                            lines.append("  ~ %-14s %-30s | %s" % (key, explain(key, rv), explain(key, ov[1])))

            for key in oth.keys[test]:              # values only the other one has - its code took another path
                if (test, key) not in ref.values and oth.values[(test, key)][0] == "=":
                    lines.append("    %-14s %-30s | %s" % (key, "<missing>", explain(key, oth.values[(test, key)][1])))
                    exactBad += 1

        if lines:
            if any(not l.startswith("  ~") for l in lines):
                failedTests += 1
            print("%s %s" % (test, ref.names.get(test, "?")))
            print("    %-14s %-30s | %s" % ("key", "reference", "other"))
            for l in lines:
                print(l)
            print()

    extra = [t for t in oth.order if t not in ref.names]
    if extra:
        print("tests only in the other log: %s" % ", ".join(extra))
        print()

    print("%d tests, %d exact values, %d differences, %d tests failed; %d fuzzy values differ%s" % (
        len(ref.order), exactCount, exactBad, failedTests, fuzzyDiff, "" if args.fuzzy else " (--fuzzy to list)"))

    return 1 if exactBad or failedTests else 0


if __name__ == "__main__":
    sys.exit(main())
