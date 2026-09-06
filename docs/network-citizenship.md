# Network citizenship: what we may emit, and what we must not

Reticulum links are shared and often slow. A LoRa link at SF8 and 125 kHz moves
a few hundred bytes per second, and every packet we put on it is bandwidth
somebody else does not get. These rules bind everything in this repository that
opens a link, sends a packet, or fetches anything from a node we do not own.

They are not aspirational. **A limit that lives in a document is a promise; a
limit that lives in code is a limit.** Anything below that can be enforced by a
constant, a bound, or a compile-time check is enforced that way, and the value
is visible in configuration rather than buried.

## Where these came from

In late August 2026 a page-indexing bot hit a public node with **over 200,000
page and download requests in three days**, opening a fresh link for every
single page. The node operator's description, verbatim:

> It doesn't even fucking re-use links. *Every* page download happens over a new
> link. Who the well "wrote" that? [...] If it wasn't because this technology
> existed, I would have taken it as a DoS attempt, which *in all practical
> sense*, **it is**.

The same operator then wrote out what a considerate version would have done.
The list below is quoted from that post, because it is the clearest statement
of the network's expectations that exists and paraphrasing it would only blur
it. Source: `rns.recipes/forum/general/the-slopware-scrapers-have-arrived`,
2026-08-29, read in full 2026-09-06.

## Fetching from other people's nodes

> - Listen for announces, de-prioritize scanning higher-hop count nodes
> - Set a small hard max limit on network distance to actually scan
> - Limit requests per day per node to *one*, that's perfectly viable and causes
>   very little load. Every node you index still gets indexed every month or so.
> - Only index the front-page, and a maximum one or two levels down.
> - Set a hard maximum on pages indexed per node; I wouldn't place this at
>   higher than 20 or so.
> - Write a prioritization algorithm that decides what page to fetch and index
>   next, for the next one-per-day slot.
> - Measure link RTT. If above a sensible maximum, don't index the node **at
>   all** - it's probably on a low-bandwidth connection, and doesn't want to be
>   scraped.
> - Create an easy way to permanently signal to your scraper that a node
>   **never** wants to be contacted again. How much of a no-brainer is this?

Four rules of our own sit alongside those, and they came out of the same thread:

**Discover, never probe.** Nodes are learned from announces they chose to send.
Nothing enumerates, sweeps, or guesses at destination hashes.

**Reuse links.** One link per peer, held and reused. A link per request is the
specific behaviour that started this, and it is the easiest thing in the whole
list to get right.

**Skip what has not changed.** If a front page is unchanged, the fetch does not
happen.

**One stable identity, always.** Rotating identities to evade a block is the
line between careless and malicious, and the network reads it that way. If we
are blocked, we stop. A refusal means stop, not retry.

⚠ **The 20-page cap is contested, and the objection is a good one.** Another
operator pointed out that a legitimate site can run to hundreds of pages, and
proposed a `robots.txt`-style file served over a Reticulum request so the node
operator sets their own limit. **No such convention exists yet.** Until one
does, the low cap is the safe default, and any raise must come from a signal
the operator sent rather than from our own judgement about their site.

## Anything we ship that speaks on the network

This binds the firmware, not just a fetcher.

**Design for the slowest link the traffic can cross, not for the bench.** The
budget is the LoRa link, not the USB cable or the LAN the host tests on.

**Rate-limit every outbound class**: announces, link requests, retries.
Exponential backoff on failure. **No tight retry loops**, ever.

**Announce no more often than the application needs**, and keep announce app
data small. The hard ceiling is arithmetic: MTU is 500 bytes, the maximum data
unit is 464, and an announce payload already spends 180 on public key, name
hash, random hash, ratchet and signature. **That leaves about 284 bytes**, and
using most of them is not a target. *(Derived 2026-09-06 from `Reticulum.py`
and `Destination.announce` at the reference implementation's current master.)*

**Never poll on a timer when an announce or an event can tell you instead.**

**Bound concurrency and fail closed**: maximum simultaneous links, outstanding
requests, and queue depth. When a bound is hit the answer is refusal, not
growth.

**Bound resources per link** by size, time and count, so that one peer cannot
exhaust our memory or our airtime.

**Honour identification requirements and blocklists** on nodes we talk to.

**Log our own outbound traffic and read the logs.** The bot in that thread was
found by the operator reading his node's logs, not by its author. We should find
ours first.

**Test on the real transport** before anything ships: a LoRa pair, or a
throttled interface. A host loopback hides every problem this page is about.

**Be able to explain every packet type we emit**, why it is sent, and how often.
**If that explanation does not exist, the feature does not ship.**

## What this binds today

The firmware is a leaf. It announces, sends and receives messages, and answers
what is addressed to it. Nothing in the current tree fetches pages, so the first
section is forward-looking, and it is written down now because the point at
which the code exists is too late to decide.

The interface-discovery reader is the nearest live case. It is receive-only by
design: it reads announces that were already sent and originates nothing, which
is the shape this page asks for.
