# What's new in CLBOSS v0.17.2

2026-10-13.  Applies to CLBOSS v0.17.2 with the `xrebalance` plugin
v0.4.5 or later, on Core Lightning v26.04 or later.

CLBOSS v0.17.2 is a maintenance release.  Every option is now
dynamic, channel candidates that used to read as unreachable get
funded, a connection leak is closed, and a new tool says which
channels want more capacity.  The requirements are those of v0.17.0.

## Options

Every CLBOSS option except `clboss-skip-cln-version-check` is
dynamic: `lightning-cli setconfig` changes it at runtime.  A value
the startup validation would have adjusted is refused with the
reason, so the value `lightningd` persists is the one in effect.

## Fixes

- Channel candidates.  The dowser measured a candidate with one
  probe at the largest channel size, so a candidate whose reachable
  flow fell between the smallest and the largest size read as no
  flow and was never funded.  It now searches for the flow the
  candidate can reach.
- Connections.  CLBOSS again keeps at most three connections to
  peers without channels.  Since 0.13.2 the limit never applied,
  and every candidate tested for uptime stayed connected.
- Duplicate funding.  The channel creator checks the channel list
  again right before funding and runs one creation cycle at a time,
  so a peer that got a channel while the plan was made is not
  funded again.
- Two channels to one peer.  Closing one of them archived the
  peer's complaints and reset its fee state and channel age, and a
  cooperative close showed the peer destroyed, re-created and
  destroyed again.  The create/destroy monitor now tracks channels
  by id.
- Lost candidate batches.  Two finders proposing the same node at
  the same time lost one batch.  The preinvestigator now runs one
  connect per node and answers every waiting case.
- Candidate addresses.  A candidate without a node_announcement, or
  with only a `dns` address, logged an error on every cycle.
- `clboss-status` dates.  Timestamps keep their full precision; the
  `now` field was rounded to six digits.

## Tools

`contrib/clboss-channel-sizing` reads CLBOSS's balance samples and
CLN's forwards and groups each channel as Grow, Shrink, Spent,
Right-sized, Liquidity-limited, Too young or Little or no traffic,
with a proposed splice amount.  It is experimental and work in
progress: the groups and the proposed changes are a reading of the
data, not a recommendation to act on.  It is in this release to show
the direction and to collect feedback, so please open an issue with
what it gets right and what it gets wrong.

## Upgrading from v0.17.1

Install and restart.  Nothing needs setting, and no option was
removed.  The [CHANGELOG](../../CHANGELOG.md) has the full list.

## Credits

- [Mohil-Ahuja](https://github.com/Mohil-Ahuja) contributed the
  per-channel tracking in the create/destroy monitor.
- [Amperstrand](https://github.com/Amperstrand) contributed the
  dowser's flow search and the preinvestigator's one connect per
  node.
- [johngribbin](https://github.com/johngribbin) reported the wrong
  dates in `clboss-status`.
- [tsjk](https://github.com/tsjk) reported the candidate whose only
  address is of type `dns`.
