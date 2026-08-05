# UDPInterface2 (vendored, test-only)

A copy of microReticulum's example UDP interface with **one change**: the ports
are constructor arguments instead of compile-time defines.

## Origin

| | |
|---|---|
| Upstream | [attermann/microReticulum](https://github.com/attermann/microReticulum) |
| Path | `examples/common/udp_interface/` |
| Commit | `40fa628809d57140180c1c833559ab96fec992c1` (v0.5.0), via the pinned dep |
| Vendored | 2026-08-04 |
| License | Apache-2.0 — full text in `./LICENSE`, copied from the upstream repo root |
| Copyright | © 2026 Chad Attermann |

Same arrangement as `lib/LoRaInterface`, for the same reason: PlatformIO cannot
depend on a subdirectory of a repository.

## Why a copy exists at all

Upstream initialises `_local_port` and `_remote_port` from `DEFAULT_UDP_*_PORT`,
which are compile-time macros, and the constructor takes only a name. **That
makes two UDP interfaces on different ports impossible in one process.**

A forwarding node needs exactly that — one interface facing each neighbour — so
the transport-forwarding interop scenario cannot be written against the upstream
class. Modifying our microReticulum fork was rejected for it: a patch there is a
tax on every dependency bump, paid forever, for a change only a test needs.

## The modification, in full

```
UDPInterface2(const char* name, int local_port, int remote_port,
              const char* local_host = "127.0.0.1",
              const char* remote_host = "127.0.0.1");
```

The constructor assigns the four members that upstream leaves at their macro
defaults. **Nothing else is changed** beyond renaming the class and its header
guard so both can coexist in one binary — the socket handling, the framing and
the `InterfaceImpl` contract are upstream's, untouched.

## Scope

**Test-only.** It lives under `test_interop/`, not `lib/`, and nothing on the
firmware build path references it. The device has one LoRa interface and does
not need this.

⚠ **If upstream ever makes the ports settable, delete this.** Check on any
dependency bump — the whole file is a workaround for one missing setter.
