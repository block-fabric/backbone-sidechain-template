Backbone sidechain template
===========================

A drivechain (BIP300/301) sidechain of **eCash** (ECX) with nothing in it but what every sidechain needs:
blind merged mining (BMM), deposits from eCash and withdrawals back to it. Fork it, give it a name and an
identity, add what makes your chain worth having, and propose it to eCash miners.

- **Source only.** There are no releases or binaries of the template: build it, or build your fork.
- **BMM only.** Every block of the chain is merged mined by eCash miners; there is no proof of stake or
  block producer of its own (a sidechain can add one through `sidechain::ChainHooks`, see
  [doc/sidechain.md](doc/sidechain.md)).
- **Its mainchain** is eCash, followed through either
  [Backbone](https://github.com/block-fabric/backbone) (`backboned`, a client of the eCash networks with the
  drivechain rules built in) or a plain eCash node with LayerTwo Labs' `bip300301_enforcer`.

It is derived from Bitcoin Core (v32). Its GUI is `sidechain-app`: the node and its wallet with an HTML
interface in a native web view (WebKitGTK on Linux, WebView2 on Windows), with the pages every sidechain
needs (wallet, peg, merged mining, explorer, node window, settings); see [doc/webui.md](doc/webui.md).

| | `ecash` (eCash main network, the default) | `ecashbeta` (betanet) | `ecashtest` (test network) |
|---|---|---|---|
| Slot on eCash | 0 (a placeholder: pick your own) | 0 | 0 |
| Addresses | `esc1…` | `besc1…` | `tesc1…` |
| P2P / RPC port | 8655 / 8654 | 38655 / 38654 | 48655 / 48654 |
| Data folder | `~/.sidechain` | `~/.sidechain/betanet` | `~/.sidechain/testnet4` |

`regtest` is kept for the tests. More on the eCash networks: [doc/ecash-sidechains.md](doc/ecash-sidechains.md)
and [doc/ecash-enforcer.md](doc/ecash-enforcer.md).

## 1. Build it

On Debian or Ubuntu (24.04):

```sh
sudo apt install build-essential cmake pkgconf python3 libevent-dev libboost-dev \
    libsqlite3-dev libzmq3-dev libgtk-3-dev libwebkit2gtk-4.1-dev nodejs npm
git clone https://github.com/block-fabric/backbone-sidechain-template.git my-sidechain
cd my-sidechain
cmake -B build                 # -DBUILD_WEBUI=OFF: a node without a window (no GTK, WebKitGTK or Node.js)
cmake --build build -j$(nproc)
```

The programs are in `build/bin`: `sidechain-app` (the wallet with its window), `sidechaind` (the node),
`sidechain-cli`, and `sidechain-wallet`, `sidechain-tx`, `sidechain-util`.

Tests: `ctest --test-dir build` and `build/test/functional/test_runner.py`. The tests against a mainchain
(`feature_sidechain*.py`) need a built Backbone: `MAINCHAIN_BIN_DIR` set to the folder of `backboned`, or a
Backbone tree next to this one (`../chains-ecash/build_node/bin`); they are skipped without one.

## 2. Pick a free slot

eCash has 256 sidechain slots (0 to 255). Taken or claimed as of October 2026: **1, 2, 4, 9, 13, 24, 98, 99,
130, 255** (LayerTwo Labs' sidechains) and **40 to 46** (Backbone's). Check the live list on a synced node
before you choose:

```sh
backbone-cli listactivesidechains          # active slots
backbone-cli listsidechainproposals        # proposals being acked
```

Use the same slot on `ecash`, `ecashbeta` and `ecashtest` unless one of them is taken there.

## 3. Give it a name and an identity

Two forks that keep the template's values are the same chain, so change all of these. They are listed in
one place, the **CHANGE ME** table above `CEcashSidechainParams` in
[src/kernel/chainparams.cpp](src/kernel/chainparams.cpp).

```sh
contrib/sidechain/rename.py "My Chain" mychain     # CLIENT_NAME, CLIENT_BIN_NAME, chain.json, the tests
(cd src/webui/app && npm ci && npm run i18n)       # the app's catalogs carry the name
```

| What | Where | Template's value |
|---|---|---|
| Name, program stem | `CMakeLists.txt` `CLIENT_NAME`, `CLIENT_BIN_NAME` (`rename.py`) | `Sidechain Template`, `sidechain` |
| Slot | `.slot` in `CEcashSidechainParams` | `0` |
| First block | `.genesis_message`, `.genesis_time` (a new message is a new chain) | `Sidechain template on Backbone, …` |
| Message start | `.message_start`, four bytes per network | `5c1dec21`, `5c1dec22`, `5c1dec23` |
| P2P ports | `.default_port` | 8655, 38655, 48655 |
| Address prefix | `.bech32_hrp` (`contrib/sidechain/rename-hrp.py <old> <new>` fixes the tests) | `esc`, `besc`, `tesc` |
| RPC ports | `CreateBaseChainParams` in `src/chainparamsbase.cpp` | 8654, 38654, 48654 |
| The app | `src/webui/app/src/chain/chain.json`: coin, URI scheme, colour and mark, the ports and prefixes above, links | |
| Activation height | `main_activation_height`, once the slot activates (step 4) | `0` |

The ports and prefixes in `chain.json` must match the chain parameters (a unit test checks them). Then
regenerate `test/functional/data/util/getchainparams-*.json` with `build/bin/<stem>-util -chain=<network>
getchainparams` and update `MAGIC_BYTES` in `test/functional/test_framework/messages.py`.
[doc/sidechain.md](doc/sidechain.md) ("Making your own sidechain") has the details and where your own rules go.

## 4. Propose the slot (M1)

A slot is taken by a proposal (M1) that eCash miners then ack (M2): on eCash's main network more than 1,815
acks in 2,016 blocks, on betanet more than 1,008 in 2,016. A proposal goes into a block the proposing node
builds, so it is made by a miner, or by a pool that agrees to carry it:

```sh
# With Backbone (its next block template carries the proposal, and acks it after):
backbone-cli createsidechainproposal <slot> "My Chain" "What it is, in one line" [<hash of the release> [<commit>]]
backbone-cli -chain=ecashbeta createsidechainproposal <slot> "My Chain" "What it is"     # betanet first
```

With an eCash node and the enforcer: the enforcer's gRPC `cusf.mainchain.v1.BlockProducerService/CreateSidechainProposal`.
Follow it with `listsidechainproposals`. Once the slot is active, put the height of the block that activated
it (`activationheight` in `getsidechain <slot>`) into `main_activation_height` for that network and release
again: on `ecash` a node with your own first block refuses to start without it.

## 5. Run it

**With Backbone** (`backboned`, RPC on), the node finds it by itself: JSON-RPC at 127.0.0.1 on Backbone's port
(8532 for `ecash` and `ecashbeta`, 48532 for `ecashtest`), with the cookie in `~/.backbone` (`betanet/`,
`testnet4/`):

```sh
sidechain-app                        # eCash's main network, the default
sidechain-app -chain=ecashbeta       # betanet
sidechaind -chain=ecashtest          # the test network, without a window
```

**With a plain eCash node and the enforcer**, through the enforcer's gRPC interface alone (no JSON-RPC to
the eCash node):

```sh
sidechaind -mainchaingrpc=127.0.0.1:50051                          # the enforcer's --serve-grpc-addr
sidechaind -mainchaingrpc=127.0.0.1:50051 -mainchaingrpcauth=<token>   # or Backbone's -grpc
```

Then:

- **Deposit**: `getdepositaddress` on the sidechain gives `s<slot>_<address>_<checksum>`; send to it from eCash
  with `backbone-cli createsidechaindeposit <slot> <deposit address> <amount>` (or the app's Peg page).
- **Merged mining**: `setbmm true <address>` asks eCash miners for a block whenever the waiting fees pay for one;
  `getbmminfo` shows how it goes.
- **Withdraw**: `createwithdrawal <eCash address> <amount>`; withdrawals are paid in bundles eCash miners vote on.

The networks have no DNS seeds: give the node a peer with `addnode`. Data and `sidechain.conf` are in
`~/.sidechain` (Windows `%LOCALAPPDATA%\Sidechain Template`, macOS `~/Library/Application Support/Sidechain Template`).
Try it all on regtest first: [doc/sidechain.md](doc/sidechain.md), "Trying it on regtest".

License
-------

Released under the terms of the MIT license, as Bitcoin Core. See [COPYING](COPYING) or
https://opensource.org/license/MIT.
