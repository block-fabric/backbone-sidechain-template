# Sidechains on eCash through the enforcer alone

A sidechain of this template on the eCash networks (`ecash`, `ecashbeta`, `ecashtest`; regtest with
`-testmainenforcer`) can follow its mainchain through the bip300301_enforcer's gRPC interface **alone**:

```
sidechaind -chain=ecashbeta -mainchaingrpc=127.0.0.1:8123            # an eCash node + the enforcer (betanet's port)
sidechaind -chain=ecash -mainchaingrpc=127.0.0.1:50051 -mainchaingrpcauth=<token>   # or Backbone's -grpc
```

No JSON-RPC to the mainchain, no credentials for it: a plain eCash node (ecash-com/bitcoin) with the current
enforcer (betanet runs `17eb2ce`) is enough. Backbone keeps working (its interface is byte-compatible,
Backbone's `doc/ecash-grpc.md`), and so does JSON-RPC (`-mainchainrpcport`, the default) and the mixed mode
(`-mainchaingrpcport`, `doc/mainchain-grpc.md`). The Chains networks are untouched.

This needed new peg rules for the eCash networks, which take from the mainchain only what the enforcer
reports of each block. Our sidechains have not launched on eCash (Layers slot 46, FastBlocks 45 proposed),
so these are the rules those networks start with, not a fork.

## 1. The choice

The template's rules on the Chains networks need, for each mainchain block, the withdrawal bundles pending
after it **with their scores** (refunds and new bundles wait while a bundle of another branch has support:
a score of `pending_min_score`, or the lead of the slot on the rise). The enforcer reports per block what was
proposed (Submitted), failed (Failed) and paid (Succeeded), deposits and the BMM commitment, but scores only
as they are now (`GetWithdrawalBundleProposals`). Two options:

| | (a) Rules from the events alone (chosen) | (b) Replay the votes (M4) |
|---|---|---|
| Needs | GetChainTip, GetBlockHeaderInfo, GetBlockInfo, GetSidechains, GetChainInfo | raw coinbases of every mainchain block: the eCash node's JSON-RPC (`getblock`), so no "enforcer alone" |
| Consensus code | a flag on 3 places of the rules (below) | ~600 lines replaying `handle_m4_*` (RepeatPrevious, LeadingBy50, the failure step), and the active sidechains in slot order at every past height, which `GetSidechains` does not give |
| Risk | the residual risks of §4 | any divergence from the enforcer's vote counting forks the sidechain |

(b) is rejected: it needs the node's RPC, which is what this mode is meant to do without. (a) has no
blocker; it is how LayerTwo's own sidechains (thunder-rust, plain-bitnames) work.

## 2. The rules of the eCash networks (`SidechainParams::main_events_only`)

What a sidechain block may do is decided from the mainchain record, which in this mode holds only the
enforcer's events: per block its hash, previous hash, median time (computed from the headers, as
`GetMedianTimePast`), the BMM commitment, deposits (sequence, outpoint, address, value), and the withdrawal
bundle events (Submitted, Failed, Succeeded with the M6). A record built over JSON-RPC from Backbone has the
scores too; the rules do not read them, so both kinds of node agree (unit test
`events_only_rules_need_no_scores`).

| | Chains networks (unchanged) | eCash networks |
|---|---|---|
| Bundle selection | highest mainchain fee first, at most `max_bundle_withdrawals`, `bundle_retry_delay` after a failure | the same |
| One bundle at a time | this chain's own pending bundle holds the next | the same |
| A bundle of another branch, pending | holds refunds (and, without single payout, new bundles) while it has support (score) | **holds nothing** |
| This chain's bundle, not proposed in time | fails after `unproposed_expiry_blocks` | **stays until the mainchain closes it** |
| This chain's bundle closed | Failed: withdrawals wait again (refundable; rebundled after the retry delay). Succeeded: paid | the same, from the Failed/Succeeded events |
| Expiry | the mainchain's: Failed at its maximum age | the same: Failed at `withdrawal_bundle_max_age` (26,300 blocks; 10 on regtest) |
| A payout of another bundle (Succeeded with an M6 this chain did not make) | its outputs matched to waiting withdrawals (same script and amount, oldest first); this chain's bundle dropped if it held one of them | matched **first to withdrawals not in this chain's bundle**; this chain's bundle **kept** |
| Refund | not in this chain's bundle, and no supported foreign bundle | not in this chain's bundle |

Code: `src/sidechain/state.cpp` (`MainPending`, `MainPendingNext`, the expiry and the payout matching in
`ApplyMainEvents`), `src/kernel/chainparams.cpp` (on for `ecash`, `ecashbeta`, `ecashtest` and
`-testmainenforcer`), the parameter fingerprint (`"main events only"`, `src/drivechain/scdb.cpp`).

Why each change:

- **No hold for a foreign bundle.** Without scores nothing tells a bundle of another branch of this chain
  from a bare proposal by any miner: one M3 in one block would freeze withdrawals and refunds for the
  bundle's whole life (26,300 blocks, half a year), again and again for one block every half year. Thunder
  and plain-bitnames do not hold either.
- **No unproposed expiry.** An enforcer's block producer keeps a bundle handed to it
  (`ProposeWithdrawalBundle`, its `bundle_proposals` table) and proposes it in whatever block it mines, until a
  Failed or Succeeded event deletes it (`apply_connected_block_policy`); nothing can tell it to drop it. A
  bundle failed on this side after N blocks could be proposed later, and paid beside the next bundle with the
  same withdrawals. So a bundle stays this chain's until the mainchain closes it.
- **Payout matching.** A foreign payout cannot make this chain's bundle unpayable (it stays pending, and its
  producers keep upvoting it). Matching the foreign payout to a waiting withdrawal of the same script and
  amount first means that when both are paid, each payment answers to one withdrawal; dropping this chain's
  bundle would free its other withdrawals to be bundled or refunded while it can still be paid.

### Handing the bundle over (policy, `SidechainParams::bundle_handoff_depth`)

With `-mainchaingrpc` the node hands its bundle to the block producer only once the mainchain block that
committed to it is `bundle_handoff_depth` blocks deep: 144 (a day) on `ecash` and `ecashbeta`, 12 on
`ecashtest`, 2 on regtest. A bundle takes three months of votes to be paid; a day is nothing next to it,
and it means a reorg shallower than that can never leave a handed bundle on a branch the sidechain left.
There is no vouching in this mode: the enforcer's interface cannot take a word back, and the rules need no
downvotes. The enforcer's default policy (KNOWN) upvotes the bundles handed to it; Backbone's upvotes the last
one handed. Over JSON-RPC (Backbone), the node keeps vouching as before (a policy: it downvotes a bundle a
reorg left behind), and hands its bundle at once, since it can take its word back.

## 3. Following over the interface

`MainClient` answers the JSON-RPC methods the follower calls from the interface (Connect protocol, JSON over
HTTP/1.1, `src/sidechain/mainclient.cpp`):

| Follower needs | From |
|---|---|
| `getblockchaininfo` (tip, network) | `GetChainTip`; `GetChainInfo` once (MAINNET for ecash/ecashbeta, TESTNET for ecashtest, REGTEST); `grpc.health.v1.Health/Check` once (NOT_FOUND/NOT_SERVING refused) |
| `getblockcount` | `GetChainTip` (UNAVAILABLE "Validator is not synced" read as syncing) |
| `getsidechainevents slot height count` | `GetBlockHeaderInfo` (headers kept between calls, 2000 a call, each checked to follow the one before: hashes, heights, previous hashes), then `GetBlockInfo` of the last block with `max_ancestors` (at most 1000 a call), newest first, its count and each header checked against the headers; median times from the headers |
| `getsidechain` (activation height, proposal hash) | `GetSidechains` (`activation_height`; SHA256d of the description without its compact size) |
| `validateaddress` (withdrawal destinations) | decoded here: bech32/bech32m and base58 with eCash's (Bitcoin's) prefixes; `bcrt` and Backbone's `rchn` on regtest |
| `createsidechaindeposit` (the app's deposit button) | `WalletService.CreateDepositTransaction` (fee 10,000 sats) |
| BMM request | `GetChainTip`, `WalletService.CreateBmmCriticalDataTransaction` (already in `ecash-grpc`) |
| withdrawal bundle | `BlockProducerService.ProposeWithdrawalBundle` (legacy serialization), after the hand-off depth |
| `getbestblockhash` | `GetChainTip` |
| `getbalances`, `getnewaddress` (the app's deposit helper, "an address of the wallet" for a withdrawal) | `WalletService.GetBalance` (confirmed as trusted, pending as untrusted_pending), `WalletService.CreateNewAddress`: the wallet behind the interface, the one that pays deposits and BMM requests |
| anything else | refused: "reached through the enforcer's interface alone" |

The follower is unchanged otherwise: it polls every second (`SubscribeEvents` is not needed: a reorg shows as
a tip that no longer extends the record, and the follower walks back to the fork as over JSON-RPC; the
mainchain going back without a new branch is waited for, as over JSON-RPC). A block with an event of a kind
this node does not know is refused, not skipped. Deposits: `address` is the OP_RETURN data (Backbone's
`destination`), `value_sats` what the deposit added. Payouts: Succeeded's transaction is the M6; its output 0
the new treasury, the outputs after it with a value are the payouts (as Backbone's `getsidechainevents`).

Options: `-mainchaingrpc=[http://]host:port`, `-mainchaingrpcauth=<token>` (Backbone's `-grpcauth`; the
enforcer has none). Refused with `-mainchaingrpcport`, and on the Chains networks. TLS: neither the enforcer
nor Backbone serve it, and this node has no TLS library; an `https://` address is refused. Reach a remote
interface through a tunnel (`ssh -L 50051:127.0.0.1:50051 host`, stunnel) as `http://127.0.0.1:50051`.
`getmainchaininfo` says `"via": "enforcer"`.

### What the app shows of the peg: `getpeginfo`

The app's pages (Home's Drivechains card, Drivechains: Deposit, Withdraw, Bundle, Merged mining) read the peg from the
node's `getpeginfo`, the same in every mode, and never ask the mainchain node for `getdrivechaininfo` or
`listwithdrawalbundles` themselves (`peg.call`, which the enforcer's interface refuses):

| `getpeginfo` | Over JSON-RPC (Backbone) | Through the enforcer's interface |
|---|---|---|
| `height`, `rules` | `getdrivechaininfo` | `GetChainTip`; `enforcer` |
| `withdrawalperiod`, `withdrawalminscore` (votes a bundle needs) | `bundlemaxage`, `bundlethreshold` + 1 with the enforcer's rules; `withdrawalperiod`, `withdrawalminscore` with the Chains rules | `GetChainInfo.bip300_constants`: `withdrawal_bundle_max_age`, `withdrawal_bundle_inclusion_threshold` + 1 (left out by an enforcer without them) |
| `activesidechains`, `proposals` | `getdrivechaininfo` | `GetSidechains`, `GetSidechainProposals` |
| `sidechain` (active, activation height, proposal hash, title) | `getsidechain` | `GetSidechains` |
| `escrow` | `getsidechain` | `GetCtip` |
| `bundles` (hash, height, score, blocks left, payable) | `listwithdrawalbundles`, and how the node votes (`vote`) | `GetWithdrawalBundleProposals` (vote count, proposal height); blocks left and payable from the constants |
| `votehistory` (the score of each pending bundle at each block, in `getsidechainevents`) | true | false: the interface reports the votes as they are now only |

What the enforcer does not report is left out quietly: the last score of a bundle already closed shows as "–"
("not reported by the enforcer's interface"), and Merged mining's node panel names the interface and its wallet
instead of a cookie file. Code: `MainClient::PegInfo`, `PegInfoFromRpc`, `PegInfoFromEnforcer`; the app's
`lib/sidestate.svelte.ts` (`loadPegInfo`: a node without `getpeginfo`, over JSON-RPC only, falls back to the two
calls). Tests: `peg_info_both_ways` (unit), `feature_sidechain_enforcer.py` and `feature_sidechain_ecash.py`
(`getpeginfo` against Backbone's own answers), `src/webui/app/src/test/sidechain.test.ts` (the pages in each mode).

## 4. Safety

Assumptions:

- **A1** (finality): no reorg of the mainchain, or of this chain by BMM, takes back the block that committed
  to a bundle after the bundle was handed over (`bundle_handoff_depth` mainchain blocks later).
- **A2** (honest majority): a bundle that no honest block producer holds does not get the votes to be paid
  (more than 13,150 in 26,300 blocks). An enforcer with the ALL policy upvotes the first pending bundle of
  anybody; such miners are not counted as honest here (that policy pays whatever a miner proposes, whatever
  the sidechain's rules).
- **A3** (liveness): some block producer that runs this sidechain mines now and then.

**No double spend of sidechain funds.** A withdrawal is burned on this chain once. It is then either waiting,
in this chain's single pending bundle, refunded, or paid:

1. *Bundled twice by this chain:* a bundle takes only waiting withdrawals; one in the pending bundle is not
   waiting; the bundle leaves only by a Failed (the enforcer dropped it: not pending, deleted from every
   producer's table) or a Succeeded (paid; deleted likewise). No expiry on this side, so never while it can
   still be paid.
2. *Refunded and paid:* a refund takes only a waiting withdrawal not in the bundle. By A1, every bundle an
   honest producer holds is one this chain's active branch committed: its pending one (whose withdrawals
   are not refundable) or one already closed. By A2 no other bundle is paid.
3. *Paid twice by two bundles:* by A1 honest producers never hold two bundles of this chain that are pending
   together (the next one is committed only after the previous one closed, and handed after that). A bundle of
   a branch left behind deeper than A1 allows, or a miner's own, paid against A2: its outputs are matched to
   waiting withdrawals first, so a withdrawal it paid is not paid again by a later bundle; the escrow then
   loses only what the miners took.
4. *Deposits:* credited from Deposit events of the record, which is per mainchain block; a block taken back
   by a reorg takes its deposits back with the sidechain blocks built on it (functional test: credited once,
   on the branch that has it).

**No stuck funds.** A waiting withdrawal can always be refunded (nothing holds refunds but this chain's own
bundle) or bundled (after the retry delay; nothing foreign holds new bundles). A bundled withdrawal is
paid or failed within `withdrawal_bundle_max_age` blocks of the bundle's proposal, and by A3 it is proposed.
Without A3 -- no producer of this sidechain ever mines -- a bundle may stay unproposed and its withdrawals
locked; but then no bundle could ever be paid either (paying needs a majority of upvotes). The price of
safety over the enforcer's keep-until-closed tables; the template's Chains rules keep their expiry.

**Reorg safety.** The state of a sidechain block is a function of the block, the blocks before it and the
record up to its peg height; the record follows the enforcer's active chain (`Mainchain::Truncate` at the
fork, then the new branch), and blocks whose commitment moved are checked again (`Follower::Act`,
`CheckActiveChain`), as over JSON-RPC. Undo data restores the state of each block taken back. Tests: deposit,
bundle commitment (before the hand-off) and payout (Succeeded taken back and back again) across reorgs.

Over JSON-RPC (Backbone) the bundle is handed over at once, and A1 is not needed for it: the node takes its word
back when its chain leaves the bundle (`vouchwithdrawalbundle`), so Backbone's producers downvote it, and
Backbone never upvotes a bundle that was pending at a payout of its slot.

What remains, as on the enforcer's eCash with any sidechain: a hashrate majority can pay any bundle; a reorg
deeper than the hand-off depth after a hand-off can leave two bundles with the same withdrawals held by honest
producers. Both go away with the enforcer's withdrawal-bundle safety rules (single payout, paid-bundle memory,
upvote and idle expiry, proposed to the enforcer), which the rules here would take
as they come: as Failed events.

## 5. Tests

- `src/test/drivechain_tests.cpp`: `events_only_rules_need_no_scores` (a foreign bundle with any score holds
  nothing; the same state with and without scores in the record), `events_only_bundle_stays_until_closed` (no
  expiry; Failed: refundable, rebundled after the delay; Succeeded: paid), `events_only_foreign_payout_spares_the_bundle`
  (the bundle kept, the payout matched outside it first; the Chains behaviour unchanged),
  `events_only_reorg_of_the_record`.
- `src/test/sidechain_mainclient_tests.cpp`: `enforcer_block_events` (a GetBlockInfo entry as a
  getsidechainevents entry: byte orders, deposits, Submitted/Failed/Succeeded, the M6's payouts, unknown
  events refused, median times), `mainchain_addresses` (BIP173/350 vectors, base58, networks), `ecash_networks`.
- `test/functional/feature_sidechain_enforcer.py`: against `backboned -regtest -testecash -grpc`, the sidechain
  nodes with no JSON-RPC access (a port nothing listens on, no cookie): activation, BMM requested by the node,
  deposits, a deposit across a reorg, a withdrawal handed over at depth 2, Submitted, voted, Succeeded, the
  payout across a reorg, a Failed bundle refunded, a reorg across a bundle's commitment before its hand-off
  (never proposed; paid once), a foreign bundle upvoted and holding nothing back, two nodes in agreement.
- `test/functional/feature_sidechain_ecash.py` (JSON-RPC, and `--mainchaingrpc`): the leftover bundle of another
  branch now holds nothing back; the new one is vouched for and paid, the other downvoted and failed.

### To repeat against the real enforcer (not on this PC)

ecash-com/bitcoin (`betanet` branch) on regtest with the enforcer `17eb2ce` (`--network-preset` none,
`--op-drivechain nop8`, `--enable-wallet --enable-mempool --enable-block-template-server`), the sidechain with
`-chain=regtest -testmainenforcer -mainchaingrpc=127.0.0.1:50051` and no RPC credentials:

1. Health, GetChainInfo (REGTEST), GetChainTip while the enforcer syncs (UNAVAILABLE read as syncing).
2. Activation (the enforcer's own M1/M2 through `CreateSidechainProposal` and `SetSidechainAck`), seen by
   GetSidechains: activation height and proposal hash equal to Backbone's for the same description.
3. BMM: `requestbmmblock` through `CreateBmmCriticalDataTransaction`; the next block's `bmm_commitment`.
4. Deposits through `CreateDepositTransaction` and through a raw M5: credited, destination text equal.
5. Withdrawal: `ProposeWithdrawalBundle` after the hand-off depth; Submitted in the next block the producer
   mines; votes by its KNOWN policy; Succeeded with the M6, payouts matched; escrow (`GetCtip`) down by the bundle.
6. Failed: `SetWithdrawalBundlePolicy` NONE (or ALARM) until the maximum age; Failed event; refund.
7. Reorgs (`invalidateblock` on the eCash node): across a deposit, a bundle commitment before hand-off, a
   Succeeded; the sidechain's state equal to that of a node that only saw the final branch.
8. A foreign M3 (GetCoinbasePSBT with a ProposeBundle, mined by hand): no hold.
9. The same scenario with one sidechain node over the enforcer and one over Backbone's JSON-RPC on a peered
   backboned: `getsidechainstate` hashes equal at every step (the records agree on every field the rules read).
10. Betanet shadow: `-chain=ecashbeta -mainchaingrpc=127.0.0.1:8123` beside the betanet enforcer, following from
    the activation height; and a week of following with restarts of the enforcer.
