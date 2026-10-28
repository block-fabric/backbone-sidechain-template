// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SIDECHAIN_MAINCLIENT_H
#define BITCOIN_SIDECHAIN_MAINCLIENT_H

#include <script/script.h>
#include <sync.h>
#include <uint256.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/result.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sidechain {

/** A call to the mainchain node failed: it could not be reached, or it answered with an error. */
class MainClientError : public std::runtime_error
{
public:
    //! Whether the node was reached and refused the call; if not, the node could not be reached.
    const bool rpc_error;
    //! The message of the node's error, as it gave it, if it refused the call.
    const std::string rpc_message;
    explicit MainClientError(const std::string& message, bool rpc_error_in, std::string rpc_message_in = {})
        : std::runtime_error{message}, rpc_error{rpc_error_in}, rpc_message{std::move(rpc_message_in)} {}
};

/**
 * The mainchain node of a sidechain network: Backbone (backboned, backbone-cli) on eCash for ecash, ecashbeta
 * and ecashtest (the same network names as Backbone's -chain), or a plain eCash node with its enforcer
 * (-mainchaingrpc). Regtest: a Backbone regtest node in its eCash mode (-testecash).
 */
//! The default JSON-RPC port of the mainchain node: Backbone's (8532, 8532, 48532; regtest 29554).
uint16_t DefaultMainchainRPCPort(ChainType chain);
//! The folder of the network in the data directory of the mainchain node ("" for the network at its root).
std::string MainchainDataSubdir(ChainType chain);
//! The name of the mainchain program, for messages: "Backbone".
std::string MainchainName(ChainType chain);

/** JSON-RPC client for the mainchain node that this sidechain node follows. */
class MainClient
{
public:
    struct Options {
        std::string host{"127.0.0.1"};
        uint16_t port{0};
        //! "user:password"; if empty, the cookie file is read for every call.
        std::string credentials;
        fs::path cookie_file;
        //! Name of the mainchain wallet that pays for merged mining; unset to use the only loaded wallet.
        std::string wallet;
        bool wallet_set{false};
        //! Port of the bip300301_enforcer's interface (Backbone's -grpcport, or an enforcer's) on `host`, for
        //! the requests of this node (BMM requests, withdrawal bundles, its word on them); 0: JSON-RPC for them.
        uint16_t enforcer_port{0};
        //! Its token (Backbone's -grpcauth), if it has one.
        std::string enforcer_token;
        //! The host of that interface, if not `host` (-mainchaingrpc).
        std::string enforcer_host;
        //! Everything through that interface, following the mainchain too: no JSON-RPC at all (-mainchaingrpc),
        //! for a mainchain that is an eCash node and a bip300301_enforcer (or Backbone's interface alone). The
        //! JSON-RPC methods this node calls are answered from the interface (Call); others are refused.
        bool enforcer_only{false};
        //! The network of this node (enforcer_only: the mainchain addresses of withdrawals, the network check).
        std::optional<ChainType> chain;
    };

    explicit MainClient(Options options) : m_options{std::move(options)} {}

    /**
     * Call a method of the mainchain node and return its result.
     * @param[in] wallet  whether the method is one of the wallet
     * @throws MainClientError
     */
    UniValue Call(const std::string& method, const UniValue& params, bool wallet = false) const;

    /**
     * Call a method of the bip300301_enforcer's interface ("cusf.mainchain.v1.WalletService/CreateNewAddress")
     * with the Connect protocol (JSON over HTTP/1.1, which the enforcer and Backbone serve beside gRPC), at
     * Options::enforcer_port. Requests and replies are in the proto3 JSON mapping.
     * @throws MainClientError (rpc_error: the interface answered with an error; rpc_message its code and message)
     */
    UniValue CallEnforcer(const std::string& procedure, const UniValue& request) const;

    /**
     * What the pages show of the peg of the sidechain in `slot` (getpeginfo), the same whichever way this node
     * reaches the mainchain: over JSON-RPC getdrivechaininfo, getsidechain and listwithdrawalbundles; through the
     * enforcer's interface GetChainInfo, GetChainTip, GetSidechains, GetSidechainProposals, GetCtip and
     * GetWithdrawalBundleProposals (PegInfoFromRpc, PegInfoFromEnforcer).
     * @throws MainClientError, std::runtime_error
     */
    UniValue PegInfo(uint32_t slot) const;
    bool HasEnforcer() const { return m_options.enforcer_port != 0; }
    bool EnforcerOnly() const { return m_options.enforcer_only; }
    //! host:port of the mainchain node as this node reaches it: the enforcer's interface if enforcer_only, else JSON-RPC.
    std::string Address() const;

    const Options& GetOptions() const { return m_options; }

private:
    /** Send an HTTP/1.1 request to `port` of `host` and return the whole reply. */
    std::string Exchange(const std::string& host, uint16_t port, const std::string& request) const;
    const std::string& EnforcerHost() const { return m_options.enforcer_host.empty() ? m_options.host : m_options.enforcer_host; }

    /** A JSON-RPC method of the mainchain node, answered from the enforcer's interface (enforcer_only). */
    UniValue CallAsEnforcer(const std::string& method, const UniValue& params) const;
    /** getsidechainevents, from the enforcer's interface: GetChainTip, GetBlockHeaderInfo, GetBlockInfo. */
    UniValue EnforcerEvents(uint32_t slot, int first, int count) const;
    /** The tip of the enforcer (GetChainTip): its height and hash. */
    std::pair<int, uint256> EnforcerTip() const;
    /**
     * The headers of the enforcer's chain from `low` up to its tip (`tip`, at `tip_height`), kept from call to
     * call (GetBlockHeaderInfo, 2000 at a time, newest first): only what is new is asked for.
     */
    struct Header {
        uint256 hash;
        uint256 prev;
        int64_t time{0};
    };
    std::vector<Header> EnforcerHeaders(const uint256& tip, int tip_height, int low) const EXCLUSIVE_LOCKS_REQUIRED(!m_headers_mutex);

    const Options m_options;
    mutable Mutex m_headers_mutex;
    //! The headers known of the enforcer's chain, from height m_headers_base on, its tip last.
    mutable std::vector<Header> m_headers GUARDED_BY(m_headers_mutex);
    mutable int m_headers_base GUARDED_BY(m_headers_mutex){0};
    //! The enforcer's network (GetChainInfo), once asked; and whether its health was checked.
    mutable std::string m_enforcer_network GUARDED_BY(m_headers_mutex);
};

/**
 * One block of the enforcer's GetBlockInfo (an Info: header_info and block_info, the proto3 JSON mapping) as an
 * entry of Backbone's getsidechainevents, which the follower reads: hash, previous hash, the median time given
 * (`median_time`: of the block and the 10 before it, from their headers), the BMM commitment, deposits (and the
 * change of a payout: its treasury output, the bundle, what it paid), the bundles closed (Succeeded, Failed) and
 * proposed (Submitted). No "pending" scores: the interface has none (SidechainParams::main_events_only).
 * @throws std::runtime_error for an entry that cannot be read
 */
UniValue EnforcerBlockEvents(const UniValue& info, int64_t median_time);
/**
 * getpeginfo from the replies of a mainchain node over JSON-RPC: `drivechain` (getdrivechaininfo), `sidechain`
 * (getsidechain of the slot; null if none is active in it) and `bundles` (listwithdrawalbundles of the slot). With
 * the enforcer's rules (Backbone on eCash), the score a bundle needs and the blocks it has are those rules': more
 * votes than bundlethreshold within bundlemaxage blocks. What a reply does not say is left out, nothing made up.
 */
UniValue PegInfoFromRpc(uint32_t slot, const UniValue& drivechain, const UniValue& sidechain, const UniValue& bundles);
/** The replies of the enforcer's interface getpeginfo is made of (proto3 JSON mapping); a reply not had is null. */
struct EnforcerPegReplies {
    UniValue chain_info;  //!< GetChainInfo (bip300_constants: not sent by an enforcer older than them)
    int tip_height{-1};   //!< GetChainTip
    UniValue sidechains;  //!< GetSidechains
    UniValue proposals;   //!< GetSidechainProposals
    UniValue ctip;        //!< GetCtip of the slot
    UniValue bundles;     //!< GetWithdrawalBundleProposals of the slot
};
/**
 * getpeginfo from the enforcer's interface: the votes of each bundle as they are now (vote_count), the blocks it has
 * left and whether it is payable from the BIP300 constants (paid with more votes than the inclusion threshold, failed
 * older than the maximum age). No vote of the node's own ("vote"), no score of past blocks ("votehistory": false).
 */
UniValue PegInfoFromEnforcer(uint32_t slot, const EnforcerPegReplies& replies);
/** The median of the times of a block and of the (up to) 10 before it, as a node computes it (GetMedianTimePast). */
int64_t MedianTime(std::vector<int64_t> times);
/**
 * The output script of a mainchain address of the eCash networks (eCash's node uses Bitcoin's formats: bc1/1/3,
 * tb1/m/n/2 on its test network; on regtest bcrt1 and Backbone's rchn1), for validateaddress without the node.
 */
std::optional<CScript> MainchainScript(const std::string& address, ChainType chain);

/**
 * Read the password for the mainchain node from a file (-mainchainrpcpasswordfile), so that it is never
 * on the command line, where any local user can read it.
 *
 * The password is the first line of the file, without its line ending. On Unix the file must be a regular
 * file owned by this user and readable by no one else (no group or other permission bit), or it is refused.
 * On Windows its access list is not checked: keep it in a directory only this user can read (the data
 * directory is). Errors name the file but never contain what it holds.
 */
util::Result<std::string> ReadPasswordFile(const fs::path& path);

/**
 * The body of an HTTP/1.1 reply in the chunked transfer coding, put back together; nullopt (with `error`)
 * if a chunk is malformed (its size beyond the reply, no CRLF after it), the reply is truncated, or it is
 * larger than `max_size`.
 */
std::optional<std::string> DecodeChunked(const std::string& body, size_t max_size, std::string& error);

} // namespace sidechain

#endif // BITCOIN_SIDECHAIN_MAINCLIENT_H
