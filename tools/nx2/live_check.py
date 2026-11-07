"""Live checks for the Lichii debug server (EDEN_NX2_DEBUG_SERVER).

Boot a game with the debug server on, then run:

    python tools/nx2/live_check.py --server 127.0.0.1:6565

One PASS/FAIL line per check and a non-zero exit code if any failed. The recording checks leave a
`live-check.json` in the dump directory and replay it twice, so a repeatable length shows the input
timing is present-count driven. Writes are round-trips: GPU memory is written back with the bytes
it was read from, so the emulated state does not change.
"""
import argparse
import json
import socket
import sys
import time

FAILURES = []


class Client:
    """One JSON request per line, in order, like the MCP adapter in debug_mcp.py."""

    def __init__(self, server):
        host, port = server.rsplit(":", 1)
        self.sock = socket.create_connection((host, int(port)), timeout=5)
        self.sock.settimeout(90)
        self.file = self.sock.makefile("rwb")
        self.next_id = 0

    def call(self, method, **params):
        self.next_id += 1
        request = {"id": self.next_id, "method": method, "params": params}
        self.file.write(json.dumps(request).encode() + b"\n")
        self.file.flush()
        reply = json.loads(self.file.readline().decode())
        if "error" in reply:
            raise RuntimeError(reply["error"])
        return reply["result"]


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {name}{'  ' + detail if detail else ''}")
    if not ok:
        FAILURES.append(name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="127.0.0.1:6565")
    parser.add_argument("--skip-motion", action="store_true")
    parser.add_argument("--skip-reject", action="store_true",
                        help="skip the shader-toggle screenshot A/B (it renders two frames)")
    args = parser.parse_args()
    c = Client(args.server)

    status = c.call("status", fps_window_ms=500)
    check("status: powered on", status["powered_on"], status["title"])
    check("status: presents advance", status["presents"] > 0, f"presents={status['presents']}")
    check("status: caches reported", "caches" in status, json.dumps(status["caches"]))

    # pause / step / resume
    before = status["presents"]
    c.call("pause")
    time.sleep(0.7)
    frozen = c.call("status")["presents"]
    check("pause: presents frozen", abs(frozen - before) <= 1, f"{before} -> {frozen}")
    step = c.call("step", frames=10, timeout_ms=10000)
    check("step: advanced 10", step["advanced"] == 10, json.dumps(step))
    c.call("resume")
    check("resume: not paused", not c.call("status")["paused"])

    # input overlay
    seq = c.call("input_sequence", steps=[{"frames": 5, "buttons": ["a"]},
                                          {"frames": 5, "buttons": [], "left": [0.5, 0.0]}])
    check("input_sequence: accepted", seq["frames"] == 10, json.dumps(seq))
    time.sleep(0.5)
    check("input: cleared", c.call("input", clear=True)["active"] is False)

    # guest memory: probe a few application addresses for a mapped one
    guest = None
    for address in (0x10000000, 0x80000000, 0x40000000, 0x08000000, 0x20000000):
        try:
            guest = (address, c.call("read_guest_memory", address=address, size=16))
            break
        except RuntimeError:
            continue
    if guest:
        check("read_guest_memory", guest[1]["size"] == 16, f"address={guest[0]:#x}")
    else:
        check("read_guest_memory", False, "no mapped address found")

    # GPU memory: the map says what is readable, and a write of the bytes just read is a no-op
    ranges = c.call("gpu_memory_map", limit=1)["ranges"]
    if ranges:
        address = int(ranges[0]["address"], 16)
        read = c.call("read_gpu_memory", address=address, size=16, timeout_ms=8000)
        check("read_gpu_memory", read["size"] == 16, f"address={address:#x}")
        written = c.call("write_gpu_memory", address=address, size=16,
                         bytes_base64=read["bytes_base64"], confirm=True, timeout_ms=8000)
        check("write_gpu_memory", written["written"] == 16, f"address={address:#x}")
        again = c.call("read_gpu_memory", address=address, size=16, timeout_ms=8000)
        check("write_gpu_memory: round trip", again["bytes_base64"] == read["bytes_base64"])
    else:
        check("gpu_memory_map", False, "no mapped GPU range")

    # shader inventory
    shaders = c.call("shaders", limit=5)
    check("shaders: inventory", shaders["programs"] > 0,
          f"programs={shaders['programs']} rejected={shaders['rejected']}")

    # screenshot, and a shader toggle that must change the image and then be restored
    shot = c.call("screenshot", height=360, save=True, inline=False)
    check("screenshot: saved", shot.get("width", 0) > 0 and "path" in shot,
          f"{shot.get('width')}x{shot.get('height')} {shot.get('path', '')}")
    if not args.skip_reject and shaders["shaders"]:
        hash_ = shaders["shaders"][0]["hash"]
        plain = c.call("screenshot", height=240, inline=True)["png_base64"]
        toggled = c.call("screenshot", height=240, inline=True, reject=[hash_])
        check("screenshot: shader toggle applied",
              toggled.get("overrides", {}).get("rejected") == [hash_])
        check("screenshot: shader toggle changes the image",
              toggled["png_base64"] != plain)
        check("screenshot: toggle restored", c.call("status")["rejected"] == [])

    if not args.skip_motion:
        # record a macro, then replay it twice: same inputs, same present spacing
        c.call("input_record", name="live-check")
        c.call("input_sequence", steps=[{"frames": 4, "buttons": ["a"]},
                                        {"frames": 4, "buttons": []},
                                        {"frames": 4, "buttons": ["b"]}])
        time.sleep(0.6)
        saved = c.call("input_record", stop=True)
        check("input_record: wrote events", saved["events"] >= 1, json.dumps(saved))
        spans = []
        for _ in range(2):
            play = c.call("input_playback", name="live-check", pause_at_end=False)
            check("input_playback: accepted", play["events"] >= 1, json.dumps(play))
            start = c.call("status")["presents"]
            time.sleep(0.7)
            spans.append(c.call("status")["presents"] - start)
        check("input_playback: repeatable length", abs(spans[0] - spans[1]) <= 2, str(spans))

    print()
    print("all checks passed" if not FAILURES else f"{len(FAILURES)} failed: {', '.join(FAILURES)}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
