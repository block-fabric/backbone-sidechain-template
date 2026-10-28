// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_CHAINTYPE_H
#define BITCOIN_UTIL_CHAINTYPE_H

#include <optional>
#include <string>
#include <string_view>

enum class ChainType {
    //! Not networks of this chain (it runs on ecash, ecashbeta and ecashtest; regtest for the tests): the rules
    //! the eCash networks are built from (CEcashSidechainParams) and what the unit tests and benchmarks set up.
    //! -chain=main, test and signet (-testnet, -signet) are refused (ArgsManager::GetChainType).
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

/** Whether `chain` is a network of this sidechain on eCash (ecash, ecashbeta, ecashtest). */
constexpr bool IsEcashChain(ChainType chain)
{
    return chain == ChainType::ECASH || chain == ChainType::ECASH_BETANET || chain == ChainType::ECASH_TESTNET;
}

/**
 * The network without -chain or -regtest: eCash's main network (betanet with -chain=ecashbeta, in its own folder,
 * betanet/; the test network with -chain=ecashtest, in testnet4/).
 */
inline constexpr ChainType DEFAULT_CHAIN_TYPE{ChainType::ECASH};

#endif // BITCOIN_UTIL_CHAINTYPE_H
