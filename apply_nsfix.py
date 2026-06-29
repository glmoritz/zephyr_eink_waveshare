#!/usr/bin/env python3
# Apply the net_ipv6_send_ns() "already-pending" NS-packet leak fix to the
# Zephyr SDK tree, and emit a clean patch (against pristine, independent of the
# debug PKTL instrumentation) for the repo's patches/ directory.
import os, subprocess, sys

ZED = "/opt/toolchains/zephyr"
REL = "subsys/net/ip/ipv6_nbr.c"
LIVE = ZED + "/" + REL                 # current tree (has PKTL printks)
PRISTINE = LIVE + ".pktl.orig"         # pristine upstream (PKTL backup)
PATCH_OUT = "/workspace/projects/hello_eink/patches_out/ipv6_nbr_ns_pending_leak.patch"

ANCHOR = (
    "\t\t\t/* Let the system timeout and then send the NS again */\n"
    "\t\t\tnet_ipv6_nbr_unlock();\n"
    "\t\t\treturn 0;\n"
)
FIXED = (
    "\t\t\t/* Let the system timeout and then send the NS again.\n"
    "\t\t\t * A packet is already pending for this neighbor, so we do\n"
    "\t\t\t * NOT send this NS; release the solicitation packet built\n"
    "\t\t\t * above or it leaks -- net_send_data() below (which hands\n"
    "\t\t\t * pkt to the TX path that unrefs it) is never reached.\n"
    "\t\t\t */\n"
    "\t\t\tnet_pkt_unref(pkt);\n"
    "\t\t\tnet_ipv6_nbr_unlock();\n"
    "\t\t\treturn 0;\n"
)

def fixup(text, where):
    n = text.count(ANCHOR)
    if "net_pkt_unref(pkt);\n\t\t\tnet_ipv6_nbr_unlock();" in text:
        print("SKIP (already fixed): %s" % where); return text, False
    if n != 1:
        print("ERROR: anchor count %d (want 1) in %s" % (n, where)); sys.exit(1)
    return text.replace(ANCHOR, FIXED, 1), True

if not os.path.exists(PRISTINE):
    print("ERROR: pristine backup %s missing (run apply_pktl.py first)" % PRISTINE); sys.exit(1)

# 1) build pristine+fix in /tmp for a clean repo patch
pris = open(PRISTINE).read()
pris_fixed, _ = fixup(pris, "pristine")
open("/tmp/ipv6_nbr_pristine.c", "w").write(pris)
open("/tmp/ipv6_nbr_pristine_fixed.c", "w").write(pris_fixed)

# 2) apply the fix to the LIVE (PKTL) tree that actually gets built
live = open(LIVE).read()
live_fixed, changed = fixup(live, "live")
if changed:
    if not os.path.exists(LIVE + ".nsfix.orig"):
        open(LIVE + ".nsfix.orig", "w").write(live)
    open(LIVE, "w").write(live_fixed)
    print("OK: applied NS fix to live %s" % REL)

# 3) emit clean patch (pristine -> pristine+fix) with proper a/b headers
os.makedirs(os.path.dirname(PATCH_OUT), exist_ok=True)
diff = subprocess.run(
    ["diff", "-u", "/tmp/ipv6_nbr_pristine.c", "/tmp/ipv6_nbr_pristine_fixed.c"],
    capture_output=True, text=True).stdout
diff = diff.replace("--- /tmp/ipv6_nbr_pristine.c", "--- a/" + REL)
diff = diff.replace("+++ /tmp/ipv6_nbr_pristine_fixed.c", "+++ b/" + REL)
open(PATCH_OUT, "w").write(diff)
print("PATCH written: %s (%d lines)" % (PATCH_OUT, len(diff.splitlines())))
print("---- patch ----"); print(diff)
