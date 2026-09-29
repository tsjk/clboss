# What's new in CLBOSS v0.17.1

2026-09-29.  Applies to CLBOSS v0.17.1 with the `xrebalance` plugin
v0.4.5 or later, on Core Lightning v26.04 or later.

CLBOSS v0.17.1 is a maintenance release.  It fixes the handling of
splices and of peer blackouts, a crash during backups, and a
duplicate channel open, and it changes how the rebalancer's grant is
weighted.  The requirements are those of v0.17.0.

## Fixes

- Splices.  Core Lightning keeps a channel open and forwarding
  while its splice confirms.  CLBOSS took that for one channel
  closing and another opening, and reset the channel's age,
  complaint history, and fee state; it now keeps them.  The
  rebalancer leaves the channel alone until the splice locks in.
- Peer blackouts.  When every peer is unreachable at once
  (`lightningd --offline`, Tor down, a firewall), CLBOSS no longer
  counts that time against the peers; with `clboss-auto-close` it
  could close channels as they came back.  While `--offline` is
  set, CLBOSS also stops connecting to peers and opening channels.
- Backups.  Copying a live `data.clboss` with `sqlite3` could end
  CLBOSS, and `lightningd` with it.  CLBOSS now waits for the lock;
  "Backing up the CLBOSS database" in the README has the procedure.
- Duplicate channels.  The channel creator could open a second
  channel to a peer it already had one with.
- On-chain reserve.  A `clboss-min-onchain` below 30000 sat made
  every channel open fail; CLBOSS now raises it to 30000 and logs a
  warning.

## Rebalancer grant

`clboss-xrebalance-grant` credits every peer with an assumed
earnings rate, so that peers with no record can be rebalanced.  Real
forwards now replace the credit one for one, and it is gone once a
side has forwarded `clboss-xrebalance-grant-weight` percent of the
peer's capacity (new dynamic option, default 25).  In v0.17.0
forwards diluted the credit but never removed it; on a node that
sets a grant, active peers are now priced by their own record alone.

## Upgrading from v0.17.0

Install and restart.  Nothing needs setting, and no option was
removed.  The [CHANGELOG](../../CHANGELOG.md) has the full list of
changes and fixes.

## Credits

[Amperstrand](https://github.com/Amperstrand) contributed the
on-chain reserve floor and a fix to how channel candidates are
sampled.  [tsjk](https://github.com/tsjk) reported the duplicate
channel open and supplied the log that showed it.
[daywalker90](https://github.com/daywalker90) relayed the report of
the `--offline` problem from the Core Lightning Telegram group.
