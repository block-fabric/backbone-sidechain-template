// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparamsbase.h>

#include <common/args.h>
#include <tinyformat.h>
#include <util/chaintype.h>

#include <cassert>

void SetupChainParamsBaseOptions(ArgsManager& argsman)
{
    argsman.AddArg("-chain=<chain>", "Use the chain <chain> (default: ecash). Allowed values: ecash, ecashbeta, ecashtest (and regtest for tests): "
                   "sidechains of eCash's main network, its betanet and its test network, followed through Backbone (backboned) or a plain eCash node "
                   "with the bip300301_enforcer (-mainchaingrpc), in ~/.sidechain (betanet/, testnet4/). "
                   "The Chains (CHN) networks main, test and signet need -chainsnetworks (development only; in ~/.sidechain/chains).", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-chainsnetworks", "Allow the Chains (CHN) networks main, test and signet (-chain=main|test|signet, -testnet, -signet), kept for development only (default: 0)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-regtest", "Enter regression test mode, which uses a special chain in which blocks can be solved instantly. "
                 "This is intended for regression testing tools and app development. Equivalent to -chain=regtest.", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-sidechainslot=<n>", "Make the regtest chain a sidechain in slot <n> of the mainchain, with the rules of one: blocks are blind merged mined and coins come from deposits. (regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-testmainenforcer", "With -sidechainslot: the mainchain follows the drivechain rules of the bip300301_enforcer, as on the eCash networks (backboned -testecash): no single payout, and the parameters tied to its blocks scaled as there (see SidechainParams). (regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-sidechainactivationheight=<n>", "With -sidechainslot: the height of the mainchain block that activated the sidechain in its slot (SidechainParams::main_activation_height; default: 0). (regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-testactivationheight=name@height.", "Set the activation height of 'name' (segwit, bip34, dersig, cltv, csv). (test-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-testdrivechainparam=name@value", "Set the drivechain parameter 'name' (a field of DrivechainParams, such as withdrawal_period) on regtest. (test-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-testnet", "Use the test chain of Chains (CHN). Equivalent to -chain=test. Needs -chainsnetworks (development only).", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-vbparams=deployment:start:end[:min_activation_height]", "Use given start/end times and min_activation_height for specified version bits deployment (test-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-signet", "Use the signet chain of Chains (CHN). Equivalent to -chain=signet. Needs -chainsnetworks (development only). Note that the network is defined by the -signetchallenge parameter", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-signetchallenge", "Blocks must satisfy the given script to be considered valid (only for signet networks; defaults to the global default signet test network challenge)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-signetseednode", "Specify a seed node for the signet network, in the hostname[:port] format, e.g. sig.net:1234 (may be used multiple times to specify multiple seed nodes; defaults to the global default signet test network seed node(s))", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::CHAINPARAMS);
}

static std::unique_ptr<CBaseChainParams> globalChainBaseParams;

const CBaseChainParams& BaseParams()
{
    assert(globalChainBaseParams);
    return *globalChainBaseParams;
}

/**
 * Port numbers for incoming Tor connections (8334, 18334, 38334, 48334, 18445) have
 * been chosen arbitrarily to keep ranges of used ports tight.
 */
std::unique_ptr<CBaseChainParams> CreateBaseChainParams(const ChainType chain)
{
    switch (chain) {
    case ChainType::MAIN:
        return std::make_unique<CBaseChainParams>("", 9654);
    case ChainType::TESTNET:
        return std::make_unique<CBaseChainParams>("testnet", 19654);
    case ChainType::SIGNET:
        return std::make_unique<CBaseChainParams>("signet", 39654);
    case ChainType::REGTEST:
        return std::make_unique<CBaseChainParams>("regtest", 29654);
    // A sidechain of Backbone on eCash: the RPC port of its Chains network less 1000 (8x54), its betanet's
    // and its test network's in 38x54 and 48x54 (as Backbone's own test network, 48532). The main network
    // at the root of the Backbone data directory of this program, betanet and the test network in the
    // folders Backbone has them in.
    case ChainType::ECASH:
        return std::make_unique<CBaseChainParams>("", 8654);
    case ChainType::ECASH_BETANET:
        return std::make_unique<CBaseChainParams>("betanet", 38654);
    case ChainType::ECASH_TESTNET:
        return std::make_unique<CBaseChainParams>("testnet4", 48654);
    }
    assert(false);
}

void SelectBaseParams(const ChainType chain)
{
    globalChainBaseParams = CreateBaseChainParams(chain);
    gArgs.SelectConfigNetwork(ChainTypeToString(chain));
}
