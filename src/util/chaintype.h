// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_CHAINTYPE_H
#define BITCOIN_UTIL_CHAINTYPE_H

#include <optional>
#include <string>
#include <string_view>

enum class ChainType {
    //! The networks of a sidechain of the Chains (CHN) mainchain: legacy, during the move to Backbone.
    MAIN,
    TESTNET,
    SIGNET,
    REGTEST,
    //! The networks of a sidechain of Backbone, on eCash (ECX): of its main network, of its betanet and of its
    //! test network (the mainchain's -chain=ecash, ecashbeta and ecashtest).
    ECASH,
    ECASH_BETANET,
    ECASH_TESTNET,
};

std::string ChainTypeToString(ChainType chain);

std::optional<ChainType> ChainTypeFromString(std::string_view chain);

/** Whether `chain` is a network of a sidechain of Backbone on eCash, rather than of the Chains mainchain. */
constexpr bool IsEcashChain(ChainType chain)
{
    return chain == ChainType::ECASH || chain == ChainType::ECASH_BETANET || chain == ChainType::ECASH_TESTNET;
}

/**
 * The network without -chain, -testnet, -signet or -regtest: eCash's betanet. This chain is a sidechain of
 * eCash; the Chains networks (main, test, signet) are kept for development behind -chainsnetworks
 * (ArgsManager::GetChainType), regtest for the tests.
 */
inline constexpr ChainType DEFAULT_CHAIN_TYPE{ChainType::ECASH_BETANET};

/** Whether `chain` is a Chains (CHN) network other than regtest, selected only with -chainsnetworks. */
constexpr bool IsChainsDevChain(ChainType chain)
{
    return chain == ChainType::MAIN || chain == ChainType::TESTNET || chain == ChainType::SIGNET;
}

#endif // BITCOIN_UTIL_CHAINTYPE_H
