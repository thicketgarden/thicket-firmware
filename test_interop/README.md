# Thicket interop suite

Scenarios that check the stack **we pin and ship** against the Python
Reticulum and LXMF reference implementations. This is the evidence behind
`docs/parity-matrix.md`: a row there may only move off `none` if a scenario
here backs it, and the Evidence column has to name the scenario.

Why it lives in `thicket-firmware` and not in the microReticulum fork: every
patch we carry on a fork is a tax on every bump (A40). This suite tests the
stack *as Thicket pins it*, so it belongs where the pins are.

## Running

```
# once, or after a pin moves
bash test_interop/scripts/fetch_deps.sh

# everything
PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_all.sh

# one scenario
PATH="/tmp/rnsvenv/bin:$PATH" bash test_interop/run_cold_inbound.sh
```

`PATH` must reach a `python3` with `rns==1.4.2` (and `lxmf` for scenario 2).
On this machine the system `python3` is 3.10.10 linked against a missing
`openssl@1.1`; its `ssl` module does not import and it cannot `pip install`.
Use a venv built from `/opt/homebrew/bin/python3`:

```
/opt/homebrew/bin/python3 -m venv /tmp/rnsvenv
/tmp/rnsvenv/bin/pip install "rns==1.4.2" "lxmf==1.1.1"
```

## Scenarios

| Scenario | Runner | What it proves |
|---|---|---|
| cold inbound | `run_cold_inbound.sh` | A Python peer originates an encrypted packet to a C++ destination that has announced but never transmitted to it. C++ decrypts and validates 383 bytes (`ENCRYPTED_MDU`); Python validates the returned proof and a digest of the recovered plaintext. |
| LXMF delivery inbound | `run_lxmf_inbound.sh` | The Python LXMF reference sends to our `lxmf.delivery` address. C++ asserts signature, title, content, timestamp, field count, field wire bytes and source hash; Python asserts the message reached `DELIVERED`. |
| identity vectors | `run_identity_vectors.sh` | `Identity` key derivation, hashing, HKDF, Ed25519 signing/verification and decryption of a reference ciphertext, against fixed outputs of the Python reference. Python re-derives and diffs the vectors so they cannot rot. |
| link inbound | `run_link_inbound.sh` | Python establishes a Link **to** a C++ destination (the existing microReticulum link scenario is C++ to Python only), round-trips 200 bytes over it, idles it past five keepalive intervals, then cuts the wire through a UDP relay and requires the reference's watchdog to close the link with `TIMEOUT`. Takes about a minute; the phases are timed. |

UDP port pairs, so scenarios can coexist: cold inbound 14262/14263, LXMF
inbound 14272/14273, link inbound 14290-14293 (two of those four are the relay).
Identity vectors uses no network.

`python/lossy_relay.py` is the shared piece of test infrastructure: an
in-process UDP forwarder that can be told to drop everything. Neither RNS nor
microReticulum has a lossy interface mode, and faking loss inside one of them
would be testing the fake.

## Proving a scenario can fail

A test never seen to fail is not evidence (T28). Every scenario ships with
switches that break one assertion at a time, so the proof is re-runnable
rather than something you have to take on trust:

```
bash test_interop/run_cold_inbound.sh     --self-test-break payload
bash test_interop/run_cold_inbound.sh     --self-test-break coldness
bash test_interop/run_lxmf_inbound.sh     --self-test-break content
bash test_interop/run_lxmf_inbound.sh     --self-test-break field
bash test_interop/run_lxmf_inbound.sh     --self-test-break timestamp
bash test_interop/run_identity_vectors.sh --self-test-break ciphertext
bash test_interop/run_link_inbound.sh     --self-test-break payload
bash test_interop/run_link_inbound.sh     --self-test-break nocut
```

Each must end in `[driver] FAIL`. If one of them passes, that assertion is
dead and the scenario is worth less than it looks.

Breaks in the stack itself (not the test) cannot be switches, so they are
applied by hand to `.deps/microReticulum` and reverted with `git reset --hard`.
Recorded here because they are the strongest evidence the suite has:

| Break | Result |
|---|---|
| wrong HKDF salt in `Identity::decrypt` | cold inbound FAILs on both sides |
| corrupt one byte of every `Identity::sign` output | cold inbound FAILs; Python rejects the announce |
| `Identity::prove` signs the wrong material | cold inbound FAILs on the Python side only, isolating the proof assertion |
| suppress `proof_packet.prove()` in microLXMF | LXMF inbound FAILs on the Python side only |
| revert the X25519 clamping commit | identity vectors FAILs 4 checks — **and cold inbound still PASSes**, which is exactly why the vectors exist |
| comment out the keepalive reply in `Link::receive` | link inbound FAILs the idle phase, which is what proves the C++ side really is answering keepalives rather than the assertion being vacuous |

## How the dependencies are wired

`scripts/fetch_deps.sh` reads the microReticulum SHA out of the top-level
`platformio.ini` and materialises it at `.deps/microReticulum`, which the
scenario projects reach by `symlink://`. Two consequences worth knowing:

- The suite cannot drift from the firmware's microReticulum pin, because there
  is only one place that SHA is written.
- Dependencies that PlatformIO *can* fetch by URL (microLXMF, MsgPack, Crypto,
  microStore) do have to be re-stated in each scenario's `platformio.ini`.
  `fetch_deps.sh` compares every one of those against the top-level file and
  refuses to run on a mismatch, so the duplication cannot rot silently.

The LXMF scenario runs the firmware's own `scripts/patch_deps.py` as a
PlatformIO `pre:` script, so it compiles the same patched microLXMF and MsgPack
the firmware does.

## Known divergences this suite has found

1. **`RNS::Cryptography::hkdf()` ignores its `context` argument.**
   `Cryptography/HKDF.cpp` calls `HKDFCommon::extract(out, len)` and never
   passes `context`, though the underlying `attermann/Crypto` API accepts
   `extract(out, outLen, info, infoLen)`. The reference mixes context into
   every expansion block. No packet is affected today — `get_context()` returns
   empty on both sides — but it is a silently wrong public function. Pinned as
   a strict expected-failure in `identity_vectors/src/main.cpp`: if it is ever
   fixed, that check goes red and forces the exemption to be deleted.

2. **The microReticulum fork carries three crypto fixes that
   `platformio.ini` does not mention.** Its comment says the fork "carries
   exactly one change" (the Transport store init). `git log 40fa6288..HEAD`
   shows four: the store init, PKCS#7 padding, an HMAC double-feed, and X25519
   clamping on key import. The X25519 one is load-bearing and is demonstrated
   above.

3. **There is no Link watchdog.** `Link::start_watchdog()` (Link.cpp:884) has an
   empty body and `Link::__watchdog_job()` sits inside a `/*p TODO */` comment
   block, so `Link::send_keepalive()` — which is compiled — has no caller.
   Consequences: a C++ link initiator never sends keepalives, a PENDING link
   never times out, and an ACTIVE link never goes STALE. Answering an inbound
   keepalive does work (Link.cpp:1455-1460 is live), which is what the link
   scenario exercises. Pinned as a strict expected failure in
   `link_inbound_responder/src/main.cpp`.

4. **Link proof validation is disabled.** `PacketReceipt::validate_link_proof`
   (Packet.cpp:907) reads `//z if (link.validate(signature, _object->_hash)) {`
   followed by `if (false) {`, so a receipt for a packet sent over a Link never
   reaches DELIVERED and its delivery callback never fires. Not covered by a
   scenario yet — the LXMF scenario uses OPPORTUNISTIC delivery, which proves
   through the Destination rather than the Link. This is the same bug
   microLXMF's conformance work found and fixed in *torlando-tech's*
   microReticulum fork; our fork descends from attermann's and does not have
   that fix.

5. **IFAC is still pseudocode** on the transmit path (Transport.cpp:1111-1145,
   inside `/*p ... */`).

6. **`Channel` has no implementation.** `Channel.cpp` is 26 lines of includes
   and nothing else, `Link::get_channel()` is commented out (Link.cpp:1136), and
   the `Type::Packet::CHANNEL` branch of `Link::receive` is commented out
   (Link.cpp:1489). There is no API to open a channel, so T28's Channel
   scenario cannot be written against this pin.
