#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Adblock Plus filter lists (EasyList, EasyPrivacy) -> WebKit content-blocker JSON.

    convert.py OUT.json LIST.txt [LIST.txt ...]

Network rules become "block" rules, element hiding becomes "css-display-none", and @@ exceptions become
"ignore-previous-rules", which WebKit applies only to the rules before them in the same list: they go last. What
WebKit's rule format cannot say is skipped and counted (scriptlets, redirects, CSP, regex alternation, extended
CSS, mixed include/exclude domain lists); the summary goes to stderr.

WebKit's url-filter is a regex subset: no alternation (|), no counted repetition ({n}), only ASCII, case-insensitive
by default. ABP's separator (^) becomes a character class, so "||ads.example^" no longer matches a URL that ends
right after the host; URLs of real requests always have a path, so that case does not come up.
"""
import json
import re
import sys
from collections import Counter, defaultdict

# ABP resource type -> WebKit resource-type ("raw" covers fetch, XMLHttpRequest and WebSocket in WebKit).
TYPES = {
    "script": ["script"], "image": ["image"], "stylesheet": ["style-sheet"], "font": ["font"], "media": ["media"],
    "xmlhttprequest": ["raw"], "websocket": ["raw"], "other": ["raw"], "ping": ["ping"], "popup": ["popup"],
    "object": ["raw"], "subdocument": ["document"],
}
ALL_TYPES = ["image", "style-sheet", "script", "font", "raw", "media", "ping", "document", "svg-document"]
# Options that change nothing WebKit can express, and those that make a rule something else: skip the rule.
IGNORED = {"important", "1p", "3p", "first-party", "all", "~important"}
SKIP = {"redirect", "redirect-rule", "csp", "removeparam", "rewrite", "header", "replace", "badfilter", "denyallow",
        "cookie", "elemhide", "generichide", "specifichide", "genericblock", "permissions", "to", "method", "urltransform",
        "inline-script", "inline-font", "empty", "mp4", "webrtc", "strict3p", "strict1p", "ehide", "ghide", "shide",
        "doc", "frame", "xhr", "css", "match-case", "sitekey", "jsonprune", "hls", "urlskip", "uritransform"}
EXTENDED_CSS = (":-abp-", ":contains(", ":xpath(", ":upward(", ":style(", ":remove(", ":matches-css", ":watch-attr(",
                ":min-text-length(", ":others(", ":matches-path(", ":matches-attr(", ":matches-prop(", ":has-text(",
                ":if(", ":if-not(", ":nth-ancestor(", ":remove-attr(", ":remove-class(", ":-ext-", ":matches-media(")
SEP = "[/:?=&]"

skipped = Counter()


def domain_of(d):
    d = d.strip().lower()
    if not d or "*" in d or "/" in d:
        return None
    try:
        d = d.encode("idna").decode("ascii")
    except UnicodeError:
        return None
    return "*" + d


def domains(spec, sep):
    inc, exc = [], []
    for part in spec.split(sep):
        neg = part.startswith("~")
        d = domain_of(part[1:] if neg else part)
        if d is None:
            continue
        (exc if neg else inc).append(d)
    return inc, exc


def url_filter(pattern):
    """ABP pattern -> WebKit url-filter, or None when WebKit cannot express it."""
    if len(pattern) > 1 and pattern.startswith("/") and pattern.endswith("/"):
        return None  # regex rules: mostly use alternation or classes WebKit lacks
    if not pattern.isascii():
        return None
    out = ""
    if pattern.startswith("||"):
        out, pattern = "^[^:]+://+([^/]+\\.)?", pattern[2:]
    elif pattern.startswith("|"):
        out, pattern = "^", pattern[1:]
    end = ""
    if pattern.endswith("|"):
        end, pattern = "$", pattern[:-1]
    if "|" in pattern:
        return None
    for ch in pattern:
        if ch == "*":
            out += ".*"
        elif ch == "^":
            out += SEP
        elif ch in ".+?()[]{}\\$":
            out += "\\" + ch
        else:
            out += ch
    out += end
    while out.startswith(".*"):
        out = out[2:]
    while out.endswith(".*") and not out.endswith("\\.*"):
        out = out[:-2]
    return out or ".*"


def network_rule(line):
    exception = line.startswith("@@")
    if exception:
        line = line[2:]
    pattern, opts = line, ""
    dollar = line.rfind("$")
    if dollar > 0 and not line[dollar + 1:].startswith("/"):
        pattern, opts = line[:dollar], line[dollar + 1:]
    trigger = {}
    types, not_types = [], []
    for opt in [o for o in opts.split(",") if o]:
        name, _, value = opt.partition("=")
        neg = name.startswith("~")
        base = name[1:] if neg else name
        if base in TYPES:
            (not_types if neg else types).extend(TYPES[base])
        elif base == "document":
            if not exception:
                return "skip:document", None
            types.append("document")
        elif base == "third-party" or base == "3p":
            trigger["load-type"] = ["first-party" if neg else "third-party"]
        elif base == "first-party" or base == "1p":
            trigger["load-type"] = ["third-party" if neg else "first-party"]
        elif base == "domain":
            inc, exc = domains(value, "|")
            if inc and exc:
                exc = []  # ABP: "on inc except exc"; WebKit takes one or the other, and inc carries the intent
            if inc:
                trigger["if-domain"] = inc
            elif exc:
                trigger["unless-domain"] = exc
            else:
                return "skip:domain", None
        elif base in IGNORED:
            continue
        elif base in SKIP:
            return "skip:" + base, None
        else:
            return "skip:unknown-option", None
    if exception and types == ["document"] and pattern.startswith("||"):
        # @@||site^$document: no blocking on that site at all.
        host = pattern[2:].rstrip("^|/")
        d = domain_of(host)
        if d is None:
            return "skip:document", None
        return "exception", {"trigger": {"url-filter": ".*", "if-domain": [d]}, "action": {"type": "ignore-previous-rules"}}
    if not_types and not types:
        types = [t for t in ALL_TYPES if t not in set(not_types)]
    if "document" in types and not exception:
        trigger["load-context"] = ["child-frame"]
    f = url_filter(pattern)
    if f is None:
        return "skip:pattern", None
    if f == ".*" and not trigger.get("if-domain") and not types:
        return "skip:too-broad", None
    trigger = {"url-filter": f, **trigger}
    if types:
        trigger["resource-type"] = sorted(set(types))
    return ("exception" if exception else "block"), {
        "trigger": trigger, "action": {"type": "ignore-previous-rules" if exception else "block"}}


def css_ok(sel):
    return sel and sel.isascii() and not any(x in sel for x in EXTENDED_CSS) and "{" not in sel and "}" not in sel


def convert(paths):
    blocks, exceptions = [], []
    generic = []                         # selectors hidden everywhere
    generic_except = defaultdict(list)   # selector -> domains where #@# shows it again
    specific = defaultdict(list)         # (kind, domains) -> selectors
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                line = raw.strip()
                if not line or line.startswith("!") or line.startswith("["):
                    continue
                m = re.match(r"^([^#]*)#([@?$%]?)#(\+js)?(.*)$", line)
                if m and not line.startswith("@@") and "#" in line and not re.match(r"^[|/]", line):
                    doms, kind, js, sel = m.groups()
                    if js or kind in ("?", "$", "%"):
                        skipped["cosmetic:extended"] += 1
                        continue
                    if not css_ok(sel):
                        skipped["cosmetic:extended"] += 1
                        continue
                    inc, exc = domains(doms, ",") if doms else ([], [])
                    if kind == "@":
                        for d in inc:
                            generic_except[sel].append(d)
                        continue
                    if not doms:
                        generic.append(sel)
                    elif inc and not exc:
                        specific[("if-domain", tuple(sorted(inc)))].append(sel)
                    elif exc and not inc:
                        specific[("unless-domain", tuple(sorted(exc)))].append(sel)
                    else:
                        skipped["cosmetic:mixed-domains"] += 1
                    continue
                kind, rule = network_rule(line)
                if rule is None:
                    skipped[kind] += 1
                elif kind == "exception":
                    exceptions.append(rule)
                else:
                    blocks.append(rule)
    cosmetic = []
    plain = [s for s in dict.fromkeys(generic) if s not in generic_except]
    for i in range(0, len(plain), 500):
        cosmetic.append({"trigger": {"url-filter": ".*"}, "action": {"type": "css-display-none",
                                                                       "selector": ", ".join(plain[i:i + 500])}})
    for sel in dict.fromkeys(generic):
        if sel in generic_except:
            cosmetic.append({"trigger": {"url-filter": ".*", "unless-domain": sorted(set(generic_except[sel]))},
                             "action": {"type": "css-display-none", "selector": sel}})
    for (key, doms), sels in specific.items():
        cosmetic.append({"trigger": {"url-filter": ".*", key: list(doms)},
                         "action": {"type": "css-display-none", "selector": ", ".join(dict.fromkeys(sels))}})
    seen, out = set(), []
    for r in blocks + cosmetic + exceptions:
        k = json.dumps(r, sort_keys=True)
        if k not in seen:
            seen.add(k)
            out.append(r)
    return out, len(blocks), len(cosmetic), len(exceptions)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    rules, b, c, e = convert(sys.argv[2:])
    with open(sys.argv[1], "w", encoding="ascii") as fh:
        json.dump(rules, fh, separators=(",", ":"))
    print(f"{len(rules)} rules: {b} block, {c} hiding, {e} exceptions; skipped {sum(skipped.values())}: "
          + ", ".join(f"{k} {v}" for k, v in skipped.most_common()), file=sys.stderr)


if __name__ == "__main__":
    main()
