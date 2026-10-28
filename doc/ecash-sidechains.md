# Sidechains of Backbone on eCash

A sidechain made from this template runs on the eCash (ECX) networks, `-chain=ecash` (the default),
`ecashbeta` and `ecashtest`: a sidechain of backboned on eCash's main network, its betanet and its test
network (backboned's own `-chain` names), or of a plain eCash node with the bip300301_enforcer
(`-mainchaingrpc`). `regtest` is kept for the tests. `main`, `test` and `signet` (and `-testnet`, `-signet`)
are refused: they remain in the code only as the rules the eCash networks are built from.

The mainchain follows the drivechain rules of the bip300301_enforcer (votes counted per period; a bundle is paid
with more than 13,150 votes in 26,300 blocks, more than 5 in 10 on the test network and the eCash regtest), has
ten-minute blocks, uses `OP_NOP8` as `OP_DRIVECHAIN` and, above all, **does not pay one bundle per slot** (see
below).

## Networks

| `-chain=` | Mainchain (backboned) | RPC / P2P (template) | Mainchain RPC (default) | Data directory | Address prefix (template) |
|---|---|---|---|---|---|
| `ecash` | `-chain=ecash` | 8654 / 8655 | 8532 | `~/.<stem>` (`~/.sidechain`; Windows `%LOCALAPPDATA%\<Name>`, macOS `~/Library/Application Support/<Name>`), at its root | `esc` |
| `ecashbeta` | `-chain=ecashbeta` | 38654 / 38655 | 8532 | the same, `betanet/` | `besc` |
| `ecashtest` | `-chain=ecashtest` | 48654 / 48655 | 48532 | the same, `testnet4/` | `tesc` |
| `regtest` (tests) | `backboned -regtest -testecash` | 29654 / 29655 | 29554 | the same, `regtest/` | `rsc` |

- **Ports.** 8x54 RPC and 8x55 P2P on the main network, betanet's 30,000 above them (38x54), the test
  network's 40,000 above (48x54, beside Backbone's 48532).
- **The mainchain node.** The cookie is looked for in Backbone's data directory (`~/.backbone`, and its
  `betanet/`, `testnet4/`); `-mainchaindatadir`, `-mainchainrpcport` and `-mainchainrpccookiefile` as before.
  The network check of the link reads `getblockchaininfo`'s `network` (Backbone's name: `ecash`, `ecashbeta`,
  `ecashtest`; its `chain` is eCash's `main`/`testnet4`). A sidechain on regtest follows backboned `-regtest`,
  with `-testecash` for the eCash rules (and `-testmainenforcer` on the sidechain, below).
- **Identity.** Each eCash network has a genesis block of its own (its own message), message start, ports and
  address prefix (`CEcashSidechainParams`, `src/kernel/chainparams.cpp`; the CHANGE ME table there says what a
  new sidechain sets). The rules are those of the `MAIN` parameters (`TESTNET`'s for `ecashtest`).
- The app's supervisor of sidechain nodes passes `-chain=ecash|ecashbeta|ecashtest` for a mainchain on those
  networks (`webui/sidechainnodes.cpp`); the app's network list (`chain.json`) has the eCash networks and regtest.

## Without single payout: rules from the enforcer's events

The Chains mainchain fails a slot's other pending bundles when it pays one, and never lets a paid bundle be
proposed again. The enforcer does neither, until it adopts our safety rules at a height
(`getdrivechaininfo`'s `singlepayout`, Backbone's `safety_activation_height`). On the eCash networks the
sidechain's peg rules take only what the enforcer reports per block (`SidechainParams::main_events_only`), so
that a node can follow an eCash node and the enforcer through its interface alone (`-mainchaingrpc`):
no scores, no hold for another branch's bundle, this chain's bundle kept until the mainchain closes it, a
foreign payout matched outside it first, and bundles handed to an enforcer only once their commitment is a day
deep. Everything, with the safety argument: `doc/ecash-enforcer.md`.

- `getwithdrawalbundle` never says `held` on the eCash networks.
- **Votes.** Over JSON-RPC the sidechain node tells Backbone to upvote its bundle and downvote every other one of
  the slot (`vouchwithdrawalbundle`); Backbone's block template never votes for, proposes or pays a bundle that was
  paid, or pending at a payout of its slot, and does not broadcast its withdrawal (`sendwithdrawalbundle`). Through
  the enforcer alone there is no vouching: its block producer upvotes the bundles handed to it.
- The warning "paid a withdrawal bundle of this sidechain and left another one pending" is said only where the
  mainchain is meant to pay one bundle per slot.

What remains: a majority of eCash's hashrate can still pay a bundle twice (a paid bundle proposed again, or a
leftover one), against the votes of the miners who run the sidechain; the sidechain then matches the payout to
withdrawals as far as it can, and the escrow pays the difference. That is the enforcer's rule until it adopts
single payout and paid-bundle memory.

## Parameters tied to the mainchain's blocks

The mainchain's blocks are ten minutes on eCash against one on Chains; one block of the sidechain per mainchain
block. Per network (`Consensus::SidechainParams`, `nPowTargetSpacing`):

| Parameter | What | Chains main (test) | eCash `ecash`, `ecashbeta` | `ecashtest` | regtest `-testmainenforcer` |
|---|---|---|---|---|---|
| `pending_min_score` | score with which a foreign bundle holds refunds back: a tenth of the payout threshold (not used where `main_events_only`) | 6480 (30) | 1315 (of 13,150), unused | 2 (of 5), unused | 2 (of 5), unused |
| `bundle_retry_delay` | sidechain blocks after a failed bundle before the next | 144 = 2.4 h (20) | 15 = 2.5 h | 3 = 0.5 h | 5 |
| `unproposed_expiry_blocks` | mainchain blocks in which the mainchain must propose a committed bundle (not used where `main_events_only`) | 1440 = 1 day (60) | unused | unused | unused |
| `main_events_only` | the rules take only the enforcer's events (`doc/ecash-enforcer.md`) | false | true | true | true |
| `bundle_handoff_depth` (policy) | mainchain blocks deep a bundle's commitment is before it is handed to an enforcer (`-mainchaingrpc`) | (6, unused) | 144 = 1 day | 12 = 2 h | 2 |
| `main_single_payout` | the mainchain pays one bundle per slot | true | false | false | false |
| `refund_grace_blocks` (policy) | blocks in which a new withdrawal can still be refunded ahead of a bundle | 6 | 1 | 1 | 6 |
| `main_behind_margin` (policy) | mainchain blocks behind its headers before the link is "syncing" | 6 | 2 | 2 | 6 |
| `nPowTargetSpacing` | the sidechain's block interval: stale-tip and block download timers, the app's estimates | 60 s | 600 s | 600 s | 600 s |
| `PENDING_TREND_BLOCKS`/`MIN_RISE` | a rising foreign leader: 3 net upvotes in 12 mainchain blocks | 12/3 | 12/3 (same ratio: 13,150 in 26,300 is half a vote per block, as 64,800 in 129,600) | 12/3 | 12/3 |

Unchanged, as they count no time: BMM requests are good for the next mainchain block only (both mainchains);
deposits are paid by the first sidechain block after the mainchain block that has them (no maturity:
`coinbase_maturity` 0); withdrawal bundles live as the mainchain says (`bundle_max_age` 26,300 blocks, half a year,
on eCash; payment after more than 13,150 votes, at least about 91 days); the follower polls every second.

The parameter fingerprint of the drivechain database changes only where `main_single_payout` is false.

## Slots on eCash

LayerTwo Labs' sidechains on eCash (`LayerTwo-Labs/drivechain-frontends`, `sidechain_core/lib/config/sidechains.dart`,
read 2026-10-20): BBC 1, BitNames 2, BitAssets 4, Thunder 9, Truthcoin 13, Liquid signet 24, zSide 98, Photon 99,
FreeBank 130, CoinShift 255. Ours on Chains collide at 2 (Thunder), 4 (BitAssets) and 9 (Hivemind). Betanet's
list of active sidechains could not be read from public data here (no betanet node with its blocks on this
machine, and no public page found that lists betanet's slots); check it with `listactivesidechains` on a synced backboned
`-chain=ecashbeta` before proposing.

Proposed map, a block of our own away from LayerTwo's (so that their next sidechains, at low numbers, do not
collide either):

| Sidechain | Chains slot | eCash slot |
|---|---|---|
| Thunder | 2 | **40** |
| BitNames | 3 | **41** |
| BitAssets | 4 | **42** |
| zSide | 6 | **43** |
| Hivemind | 9 | **44** |
| FastBlocks | 5 | **45** |
| Layers | 7 | **46** |
| template | 0 | 0 (placeholder) |

The same slot on `ecash`, `ecashbeta` and `ecashtest`. A slot is taken by proposing (M1) and acking (M2) it:
on eCash's main network more than 1,815 acks in 2,016 blocks (provisional, the BIP300 specification's), on
betanet more than 1,008 in 2,016: a two-week commitment of a hashrate majority.

## Tests

- `src/test/drivechain_tests.cpp` `without_main_single_payout_a_supported_bundle_holds_new_ones_back` (the rules
  with scores, kept for a mainchain without single payout that reports them) and the `events_only_*` tests (the
  rules of the eCash networks, `doc/ecash-enforcer.md`).
- `src/test/sidechain_mainclient_tests.cpp` `ecash_networks`: names, ports, folders, data directory, identities
  (no two networks alike), the per-network parameters, the network check of the link.
- `test/functional/feature_sidechain_ecash.py`: against backboned `-regtest -testecash` (`MAINCHAIN_BIN_DIR`,
  default `../chains-ecash/build_node/bin`): proposal and acks to activation, the OP_NOP8 treasury, BMM, a deposit,
  a withdrawal bundle voted and paid (M3, M4, M6), a bundle failing at its maximum age and its withdrawal
  refunded, and a leftover bundle of another branch holding nothing back, the new one paid and the other
  failed, the withdrawal paid once. `feature_sidechain_enforcer.py`: the same through the enforcer's interface
  alone (`-mainchaingrpc`), with reorgs.
