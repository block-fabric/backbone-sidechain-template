// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep
#include <chainparams.h>
#include <chainparamsbase.h>
#include <kernel/chainparams.h>
#include <sidechain/follower.h>
#include <sidechain/mainclient.h>
#include <base58.h>
#include <bech32.h>
#include <core_io.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <util/strencodings.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs_helpers.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/translation.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <set>
#include <tuple>
#include <vector>

using namespace sidechain;

namespace {
fs::path Write(const fs::path& path, const std::string& content)
{
    std::ofstream file{path.std_path(), std::ios::binary | std::ios::trunc};
    file << content;
    return path;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(sidechain_mainclient_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(password_file_first_line)
{
    const fs::path path{Write(m_args.GetDataDirBase() / "pw", "s3cret\r\nignored\n")};
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    const auto password{ReadPasswordFile(path)};
    BOOST_REQUIRE(password);
    BOOST_CHECK_EQUAL(*password, "s3cret");
}

BOOST_AUTO_TEST_CASE(password_file_errors_hide_content)
{
    BOOST_CHECK(!ReadPasswordFile(m_args.GetDataDirBase() / "missing"));
    const fs::path empty{Write(m_args.GetDataDirBase() / "empty", "\nsecond-line\n")};
    fs::permissions(empty, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    const auto result{ReadPasswordFile(empty)};
    BOOST_REQUIRE(!result);
    BOOST_CHECK(util::ErrorString(result).original.find("second-line") == std::string::npos);
    const fs::path large{Write(m_args.GetDataDirBase() / "large", std::string(5000, 'x'))};
    fs::permissions(large, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    BOOST_CHECK(!ReadPasswordFile(large));
}

#ifndef WIN32
BOOST_AUTO_TEST_CASE(password_file_refused_when_others_can_read)
{
    const fs::path path{Write(m_args.GetDataDirBase() / "pw", "topsecret\n")};
    for (const fs::perms extra : {fs::perms::group_read, fs::perms::others_read, fs::perms::group_write, fs::perms::others_write}) {
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | extra, fs::perm_options::replace);
        const auto result{ReadPasswordFile(path)};
        BOOST_REQUIRE(!result);
        const std::string error{util::ErrorString(result).original};
        BOOST_CHECK(error.find("chmod 600") != std::string::npos);
        BOOST_CHECK(error.find("topsecret") == std::string::npos);
    }
    fs::permissions(path, fs::perms::owner_read, fs::perm_options::replace);
    BOOST_CHECK(ReadPasswordFile(path));
    // A directory is not a password file.
    fs::create_directories(m_args.GetDataDirBase() / "dir");
    fs::permissions(m_args.GetDataDirBase() / "dir", fs::perms::owner_all, fs::perm_options::replace);
    BOOST_CHECK(!ReadPasswordFile(m_args.GetDataDirBase() / "dir"));
}
#endif

// The state of the link to the mainchain node (getmainchaininfo, the alert of the apps), from what the
// follower knows: errors of MainClient and of the follower, and what the mainchain node said of itself.
BOOST_AUTO_TEST_CASE(link_error_kinds)
{
    using K = Follower::LinkError;
    // A real call to a port nobody listens on (port 1: privileged, nothing there).
    MainClient::Options options;
    options.port = 1;
    options.credentials = "u:p";
    std::string refused;
    try {
        MainClient{options}.Call("getblockcount", UniValue{UniValue::VARR});
    } catch (const MainClientError& e) {
        refused = e.what();
    }
    BOOST_CHECK_EQUAL(Follower::LinkErrorName(Follower::ClassifyError(refused)), "refused");
    BOOST_CHECK(Follower::ClassifyError("Cannot reach the mainchain node at 127.0.0.1:19554: timeout") == K::TIMEOUT);
    BOOST_CHECK(Follower::ClassifyError("Cannot reach the mainchain node at nowhere:1: unknown host") == K::UNREACHABLE);
    BOOST_CHECK(Follower::ClassifyError("Cannot reach the mainchain node at h:1: not an HTTP reply") == K::UNREACHABLE);
    BOOST_CHECK(Follower::ClassifyError("The mainchain node refused the credentials") == K::AUTH);
    BOOST_CHECK(Follower::ClassifyError("The mainchain node does not allow connections from this address (rpcallowip)") == K::AUTH);
    BOOST_CHECK(Follower::ClassifyError("Cannot reach the mainchain node at h:1: no credentials: set -mainchainrpcuser and -mainchainrpcpassword, or check that the cookie file x can be read") == K::AUTH);
    BOOST_CHECK(Follower::ClassifyError("The mainchain node is on another chain than the one on record (its block at height 0 is a, the record's b); check -mainchainrpcconnect and -mainchainrpcport") == K::WRONG_NETWORK);
    BOOST_CHECK(Follower::ClassifyError("The mainchain node is still syncing; waiting for it to catch up before following it") == K::NOT_SYNCED);
    BOOST_CHECK(Follower::ClassifyError("The mainchain node is behind the record (at height 5, the record at 9); waiting for it to catch up") == K::NOT_SYNCED);
    BOOST_CHECK(Follower::ClassifyError("The mainchain node answered getsidechainevents with an error: boom") == K::OTHER);
    BOOST_CHECK(Follower::ClassifyError("") == K::NONE);
    for (const K kind : {K::NONE, K::REFUSED, K::UNREACHABLE, K::AUTH, K::TIMEOUT, K::WRONG_NETWORK, K::NOT_SYNCED, K::OTHER}) {
        BOOST_CHECK_EQUAL(Follower::LinkErrorName(kind).empty(), kind == K::NONE);
    }
}

BOOST_AUTO_TEST_CASE(link_state_mapping)
{
    using S = Follower::LinkState;
    using K = Follower::LinkError;
    const auto link{[](const Follower::Status& s, const std::string& own = "regtest") { return Follower::Link(s, own); }};
    Follower::Status s;
    // Down: it does not answer, with the reason.
    s.error = "Cannot reach the mainchain node at 127.0.0.1:19554: connection refused";
    BOOST_CHECK(link(s) == std::pair(S::DOWN, K::REFUSED));
    s.error = "The mainchain node answered getsidechain with an error: no";
    BOOST_CHECK(link(s) == std::pair(S::DOWN, K::OTHER));
    s.error.clear();
    BOOST_CHECK(link(s) == std::pair(S::DOWN, K::OTHER));
    // It answers but has not got as far as the record: syncing.
    s.error = "The mainchain node is behind the record (at height 5, the record at 9); waiting for it to catch up";
    BOOST_CHECK(link(s) == std::pair(S::SYNCING, K::NOT_SYNCED));
    // Up and in step.
    s = {};
    s.connected = true;
    s.main_chain = "regtest";
    s.main_blocks = s.main_headers = 100;
    s.followed_height = 100;
    BOOST_CHECK(link(s) == std::pair(S::OK, K::NONE));
    BOOST_CHECK_EQUAL(s.Behind(), 0);
    // Not looked at yet: nothing known of its sync, ok.
    Follower::Status fresh;
    fresh.connected = true;
    BOOST_CHECK(link(fresh) == std::pair(S::OK, K::NONE));
    BOOST_CHECK_EQUAL(fresh.Behind(), 0);
    // A few blocks behind its headers is normal as blocks come in; more is syncing.
    s.main_headers = 100 + Follower::BEHIND_MARGIN;
    BOOST_CHECK(link(s) == std::pair(S::OK, K::NONE));
    s.main_headers = 101 + Follower::BEHIND_MARGIN;
    BOOST_CHECK(link(s) == std::pair(S::SYNCING, K::NOT_SYNCED));
    BOOST_CHECK_EQUAL(s.Behind(), 1 + Follower::BEHIND_MARGIN);
    // In initial block download: syncing, however close.
    s.main_headers = 100;
    s.main_ibd = true;
    BOOST_CHECK(link(s) == std::pair(S::SYNCING, K::NOT_SYNCED));
    // On another network than this node: down, whatever its sync.
    BOOST_CHECK(link(s, "test") == std::pair(S::DOWN, K::WRONG_NETWORK));
    BOOST_CHECK_EQUAL(Follower::LinkStateName(S::OK), "ok");
    BOOST_CHECK_EQUAL(Follower::LinkStateName(S::SYNCING), "syncing");
    BOOST_CHECK_EQUAL(Follower::LinkStateName(S::DOWN), "down");
}

BOOST_AUTO_TEST_CASE(ecash_networks)
{
    // The networks of a sidechain of Backbone on eCash, beside those of a sidechain of Chains.
    for (const auto& [name, type] : std::vector<std::pair<std::string, ChainType>>{{"ecash", ChainType::ECASH}, {"ecashbeta", ChainType::ECASH_BETANET}, {"ecashtest", ChainType::ECASH_TESTNET}}) {
        BOOST_CHECK(ChainTypeFromString(name) == type);
        BOOST_CHECK_EQUAL(ChainTypeToString(type), name);
        BOOST_CHECK(IsEcashChain(type));
    }
    BOOST_CHECK(!IsEcashChain(ChainType::MAIN) && !IsEcashChain(ChainType::REGTEST));
    // The mainchain node: Backbone's ports and folders for the eCash networks, Chains' for the others.
    BOOST_CHECK_EQUAL(DefaultMainchainRPCPort(ChainType::ECASH), 8532);
    BOOST_CHECK_EQUAL(DefaultMainchainRPCPort(ChainType::ECASH_BETANET), 8532);
    BOOST_CHECK_EQUAL(DefaultMainchainRPCPort(ChainType::ECASH_TESTNET), 48532);
    BOOST_CHECK_EQUAL(DefaultMainchainRPCPort(ChainType::MAIN), 9554);
    BOOST_CHECK_EQUAL(DefaultMainchainRPCPort(ChainType::REGTEST), 29554);
    BOOST_CHECK_EQUAL(MainchainDataSubdir(ChainType::ECASH), "");
    BOOST_CHECK_EQUAL(MainchainDataSubdir(ChainType::ECASH_BETANET), "betanet");
    BOOST_CHECK_EQUAL(MainchainDataSubdir(ChainType::ECASH_TESTNET), "testnet4");
    BOOST_CHECK_EQUAL(MainchainName(ChainType::ECASH), "Backbone");
    // Every network in its data directory (~/.sidechain).
    BOOST_CHECK(fs::PathToString(GetDefaultDataDir().filename()).find(CLIENT_BIN_NAME) != std::string::npos);

    // Identities of their own: genesis, message start, ports, address prefix; no two networks alike.
    std::set<uint256> genesis;
    std::set<std::string> hrps;
    std::set<uint16_t> ports;
    for (const ChainType type : {ChainType::MAIN, ChainType::TESTNET, ChainType::SIGNET, ChainType::REGTEST, ChainType::ECASH, ChainType::ECASH_BETANET, ChainType::ECASH_TESTNET}) {
        const auto params{CreateChainParams(m_args, type)};
        BOOST_CHECK(params->GetChainType() == type);
        BOOST_CHECK(genesis.insert(params->GenesisBlock().GetHash()).second);
        if (type != ChainType::SIGNET && type != ChainType::REGTEST) BOOST_CHECK(hrps.insert(params->Bech32HRP()).second);
        BOOST_CHECK(ports.insert(params->GetDefaultPort()).second);
        BOOST_CHECK(ports.insert(CreateBaseChainParams(type)->RPCPort()).second);
        BOOST_CHECK(GetNetworkForMagic(params->MessageStart()) == type);
        const Consensus::Params& consensus{params->GetConsensus()};
        if (IsEcashChain(type)) {
            BOOST_CHECK(consensus.sidechain.enabled);
            // No single payout on the enforcer's eCash, and ten-minute mainchain blocks.
            BOOST_CHECK(!consensus.sidechain.main_single_payout);
            BOOST_CHECK_EQUAL(consensus.nPowTargetSpacing, 600);
            BOOST_CHECK_EQUAL(consensus.sidechain.refund_grace_blocks, 1);
            BOOST_CHECK_EQUAL(consensus.sidechain.main_behind_margin, 2);
            BOOST_CHECK_EQUAL(consensus.sidechain.pending_min_score, type == ChainType::ECASH_TESTNET ? 2U : 1315U);
            BOOST_CHECK_EQUAL(consensus.sidechain.unproposed_expiry_blocks, type == ChainType::ECASH_TESTNET ? 12 : 144);
            BOOST_CHECK_EQUAL(consensus.sidechain.bundle_retry_delay, type == ChainType::ECASH_TESTNET ? 3 : 15);
            BOOST_CHECK_EQUAL(consensus.coinbase_maturity, 0);
            // The rules of what the enforcer reports alone, and the hand-off of bundles a day deep (two hours on the test network).
            BOOST_CHECK(consensus.sidechain.main_events_only);
            BOOST_CHECK_EQUAL(consensus.sidechain.bundle_handoff_depth, type == ChainType::ECASH_TESTNET ? 12 : 144);
        } else if (type != ChainType::REGTEST) {
            BOOST_CHECK(consensus.sidechain.main_single_payout);
            BOOST_CHECK(!consensus.sidechain.main_events_only);
        }
    }

    // The template's identities on eCash: slot 0, its address prefixes and message starts.
    for (const auto& [type, hrp, last] : std::vector<std::tuple<ChainType, std::string, uint8_t>>{
             {ChainType::ECASH, "esc", 0x21}, {ChainType::ECASH_BETANET, "besc", 0x22}, {ChainType::ECASH_TESTNET, "tesc", 0x23}}) {
        const auto params{CreateChainParams(m_args, type)};
        BOOST_CHECK_EQUAL(params->GetConsensus().sidechain.slot, 0U);
        BOOST_CHECK_EQUAL(params->Bech32HRP(), hrp);
        const MessageStartChars magic{0x5c, 0x1d, 0xec, last};
        BOOST_CHECK(params->MessageStart() == magic);
    }

    // The network of a mainchain node: Backbone's "network" (its "chain" is eCash's name), else Chains' "chain".
    UniValue backbone{UniValue::VOBJ};
    backbone.pushKV("chain", "main");
    backbone.pushKV("network", "ecashbeta");
    BOOST_CHECK_EQUAL(Follower::MainNetwork(backbone), "ecashbeta");
    UniValue chains{UniValue::VOBJ};
    chains.pushKV("chain", "test");
    BOOST_CHECK_EQUAL(Follower::MainNetwork(chains), "test");
    Follower::Status s;
    s.connected = true;
    s.main_chain = "main";
    BOOST_CHECK(Follower::Link(s, "ecash").second == Follower::LinkError::WRONG_NETWORK);
    s.main_chain = "ecash";
    BOOST_CHECK(Follower::Link(s, "ecash").first == Follower::LinkState::OK);
    // Two ten-minute blocks behind is syncing on eCash, not on Chains.
    s.main_blocks = s.main_headers = 103;
    s.followed_height = 100;
    BOOST_CHECK(Follower::Link(s, "ecash", 2).first == Follower::LinkState::SYNCING);
    BOOST_CHECK(Follower::Link(s, "ecash").first == Follower::LinkState::OK);
}

BOOST_AUTO_TEST_CASE(enforcer_block_events)
{
    // A GetBlockInfo entry (proto3 JSON, as the enforcer and Backbone write it) as Backbone's getsidechainevents
    // entry: hashes as ReverseHex are GetHex, as ConsensusHex (m6id, the BMM commitment) their bytes in order.
    const uint256 block{uint256::FromHex("00000000000000000000000000000000000000000000000000000000000000b1").value()};
    const uint256 prev{uint256::FromHex("00000000000000000000000000000000000000000000000000000000000000a0").value()};
    const uint256 side{uint256::FromHex("5100000000000000000000000000000000000000000000000000000000000052").value()};
    const uint256 x{uint256::FromHex("1100000000000000000000000000000000000000000000000000000000000022").value()};
    const uint256 y{uint256::FromHex("3300000000000000000000000000000000000000000000000000000000000044").value()};
    const uint256 z{uint256::FromHex("5500000000000000000000000000000000000000000000000000000000000066").value()};
    const uint256 deposit_txid{uint256::FromHex("00000000000000000000000000000000000000000000000000000000000000d1").value()};
    // The payout: the new treasury, two payouts, and the bundle's zero-value OP_RETURN.
    CMutableTransaction m6;
    m6.version = 2;
    m6.vin.resize(1);
    m6.vout.emplace_back(10 * COIN, CScript() << OP_NOP8 << std::vector<unsigned char>{3} << OP_TRUE);
    m6.vout.emplace_back(3 * COIN, CScript() << OP_0 << std::vector<unsigned char>(20, 0x01));
    m6.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>(32, 0x02));
    m6.vout.emplace_back(COIN / 2, CScript() << OP_1 << std::vector<unsigned char>(32, 0x03));
    const std::string json{strprintf(R"({"headerInfo":{"blockHash":{"hex":"%s"},"prevBlockHash":{"hex":"%s"},"height":7,"work":{"hex":"00"},"timestamp":"1700000000"},)"
                                     R"("blockInfo":{"bmmCommitment":{"hex":"%s"},"events":[)"
                                     R"({"withdrawalBundle":{"m6id":{"hex":"%s"},"event":{"submitted":{}}}},)"
                                     R"({"withdrawalBundle":{"m6id":{"hex":"%s"},"event":{"failed":{}}}},)"
                                     R"({"deposit":{"sequenceNumber":"4","outpoint":{"txid":{"hex":"%s"},"vout":1},"output":{"address":{"hex":"%s"},"valueSats":"150000000"}}},)"
                                     R"({"withdrawalBundle":{"m6id":{"hex":"%s"},"event":{"succeeded":{"sequenceNumber":"5","transaction":{"hex":"%s"}}}}}]}})",
                                     block.GetHex(), prev.GetHex(), HexStr(side), HexStr(x), HexStr(y), deposit_txid.GetHex(), HexStr(std::string{"s3_abc_def"}), HexStr(z),
                                     EncodeHexTx(CTransaction{m6}))};
    UniValue info;
    BOOST_REQUIRE(info.read(json));
    const UniValue out{EnforcerBlockEvents(info, 1699999000)};
    BOOST_CHECK_EQUAL(out["height"].getInt<int>(), 7);
    BOOST_CHECK_EQUAL(out["hash"].get_str(), block.GetHex());
    BOOST_CHECK_EQUAL(out["previousblockhash"].get_str(), prev.GetHex());
    BOOST_CHECK_EQUAL(out["mediantime"].getInt<int64_t>(), 1699999000);
    BOOST_CHECK_EQUAL(out["bmm"].get_str(), side.GetHex());
    BOOST_CHECK_EQUAL(out["proposed"].write(), strprintf(R"(["%s"])", x.GetHex()));
    BOOST_CHECK_EQUAL(out["bundles"].write(), strprintf(R"([{"hash":"%s","paid":false},{"hash":"%s","paid":true}])", y.GetHex(), z.GetHex()));
    BOOST_CHECK_EQUAL(out["pending"].size(), 0U);
    const UniValue& deposits{out["deposits"]};
    BOOST_REQUIRE_EQUAL(deposits.size(), 2U);
    BOOST_CHECK_EQUAL(deposits[0]["destination"].get_str(), "s3_abc_def");
    BOOST_CHECK_EQUAL(deposits[0]["amount"].getValStr(), "1.50000000");
    BOOST_CHECK_EQUAL(deposits[0]["txid"].get_str(), deposit_txid.GetHex());
    BOOST_CHECK_EQUAL(deposits[0]["burnindex"].getInt<int>(), 1);
    // The change of the payout: the bundle, and what it paid (not the treasury output, not the zero-value one).
    BOOST_CHECK_EQUAL(deposits[1]["destination"].get_str(), "D");
    BOOST_CHECK_EQUAL(deposits[1]["bundle"].get_str(), z.GetHex());
    BOOST_CHECK_EQUAL(deposits[1]["txid"].get_str(), m6.GetHash().GetHex());
    BOOST_CHECK_EQUAL(deposits[1]["burnindex"].getInt<int>(), 0);
    BOOST_CHECK_EQUAL(deposits[1]["payouts"].write(), strprintf(R"([{"amount":3.00000000,"script":"%s"},{"amount":0.50000000,"script":"%s"}])",
                                                                HexStr(m6.vout[1].scriptPubKey), HexStr(m6.vout[3].scriptPubKey)));
    // The genesis block has no previous block; an event of a kind not known is refused, not left out.
    UniValue genesis;
    BOOST_REQUIRE(genesis.read(strprintf(R"({"headerInfo":{"blockHash":{"hex":"%s"},"prevBlockHash":{"hex":"%s"}},"blockInfo":{}})", block.GetHex(), uint256{}.GetHex())));
    const UniValue first{EnforcerBlockEvents(genesis, 5)};
    BOOST_CHECK(!first.exists("previousblockhash"));
    BOOST_CHECK(!first.exists("bmm"));
    BOOST_CHECK_EQUAL(first["deposits"].size(), 0U);
    UniValue odd;
    BOOST_REQUIRE(odd.read(strprintf(R"({"headerInfo":{"blockHash":{"hex":"%s"}},"blockInfo":{"events":[{"somethingNew":{}}]}})", block.GetHex())));
    BOOST_CHECK_THROW(EnforcerBlockEvents(odd, 0), std::runtime_error);
    UniValue bad_m6id;
    BOOST_REQUIRE(bad_m6id.read(strprintf(R"({"headerInfo":{"blockHash":{"hex":"%s"}},"blockInfo":{"events":[{"withdrawalBundle":{"m6id":{"hex":"11"},"event":{"submitted":{}}}}]}})", block.GetHex())));
    BOOST_CHECK_THROW(EnforcerBlockEvents(bad_m6id, 0), std::runtime_error);

    // The median time of a block and the 10 before it (fewer near the genesis block), as GetMedianTimePast.
    BOOST_CHECK_EQUAL(MedianTime({5}), 5);
    BOOST_CHECK_EQUAL(MedianTime({3, 1}), 3);
    BOOST_CHECK_EQUAL(MedianTime({9, 1, 8, 2, 7, 3, 6, 4, 5, 10, 11}), 6);
}

BOOST_AUTO_TEST_CASE(mainchain_addresses)
{
    // eCash's node takes Bitcoin's address formats; validateaddress without it, for withdrawals (-mainchaingrpc).
    const auto hex{[](const std::optional<CScript>& script) { return script ? HexStr(*script) : std::string{"none"}; }};
    // BIP173/BIP350 test vectors.
    BOOST_CHECK_EQUAL(hex(MainchainScript("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", ChainType::ECASH)), "0014751e76e8199196d454941c45d1b3a323f1433bd6");
    BOOST_CHECK_EQUAL(hex(MainchainScript("BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4", ChainType::ECASH_BETANET)), "0014751e76e8199196d454941c45d1b3a323f1433bd6");
    BOOST_CHECK_EQUAL(hex(MainchainScript("bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0", ChainType::ECASH)), "512079be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    BOOST_CHECK_EQUAL(hex(MainchainScript("tb1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q0sl5k7", ChainType::ECASH_TESTNET)), "00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262");
    // A version 1 address checksummed as bech32 (not bech32m) is no address; nor one of another network.
    BOOST_CHECK_EQUAL(hex(MainchainScript("bc1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7k7grplx", ChainType::ECASH)), "none");
    BOOST_CHECK_EQUAL(hex(MainchainScript("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", ChainType::ECASH_TESTNET)), "none");
    BOOST_CHECK_EQUAL(hex(MainchainScript("tb1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q0sl5k7", ChainType::ECASH)), "none");
    // Base58: P2PKH and P2SH of the main network; the test networks' prefixes elsewhere.
    BOOST_CHECK_EQUAL(hex(MainchainScript("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2", ChainType::ECASH)), "76a91477bff20c60e522dfaa3350c39b030a5d004e839a88ac");
    BOOST_CHECK_EQUAL(hex(MainchainScript("3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy", ChainType::ECASH)), "a914b472a266d0bd89c13706a4132ccfb16f7c3b9fcb87");
    BOOST_CHECK_EQUAL(hex(MainchainScript("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2", ChainType::ECASH_TESTNET)), "none");
    std::vector<unsigned char> regtest_p2sh{196};
    regtest_p2sh.insert(regtest_p2sh.end(), 20, 0x42);
    BOOST_CHECK_EQUAL(hex(MainchainScript(EncodeBase58Check(regtest_p2sh), ChainType::REGTEST)), "a914" + HexStr(std::vector<unsigned char>(20, 0x42)) + "87");
    // Backbone's regtest addresses (rchn1) and a bitcoind's (bcrt1).
    for (const std::string hrp : {"rchn", "bcrt"}) {
        std::vector<unsigned char> data{0};
        const std::vector<unsigned char> program(20, 0x07);
        ConvertBits<8, 5, true>([&](unsigned char c) { data.push_back(c); }, program.begin(), program.end());
        BOOST_CHECK_EQUAL(hex(MainchainScript(bech32::Encode(bech32::Encoding::BECH32, hrp, data), ChainType::REGTEST)), "0014" + HexStr(std::vector<unsigned char>(20, 0x07)));
    }
    BOOST_CHECK_EQUAL(hex(MainchainScript("not an address", ChainType::ECASH)), "none");
    // Not for the Chains networks, whose node is asked.
    BOOST_CHECK_EQUAL(hex(MainchainScript("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2", ChainType::MAIN)), "none");
}

BOOST_AUTO_TEST_CASE(chunked_replies)
{
    // The chunked coding of a Connect reply (MainClient::CallEnforcer): put back together, or refused.
    std::string error;
    BOOST_CHECK_EQUAL(*DecodeChunked("3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n", 100, error), "abcde");
    BOOST_CHECK_EQUAL(*DecodeChunked("0\r\n\r\n", 100, error), "");
    // A size beyond the reply (or one that would wrap around): refused, not skipped over.
    BOOST_CHECK(!DecodeChunked("ff\r\nabc\r\n0\r\n\r\n", 100, error));
    BOOST_CHECK(!DecodeChunked("ffffffffffffffff\r\nabc\r\n", 100, error));
    // No CRLF after the chunk, a size that is no number, a truncated reply.
    BOOST_CHECK(!DecodeChunked("3\r\nabcXY0\r\n\r\n", 100, error));
    BOOST_CHECK(!DecodeChunked("zz\r\nabc\r\n", 100, error));
    BOOST_CHECK(!DecodeChunked("3\r\nabc\r\n", 100, error));
    BOOST_CHECK_EQUAL(error, "a truncated chunked reply");
    // Over the cap.
    BOOST_CHECK(!DecodeChunked("3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n", 5, error));
    BOOST_CHECK_EQUAL(error, "a reply too large");
}

BOOST_AUTO_TEST_CASE(peg_info_both_ways)
{
    // getpeginfo over JSON-RPC (Backbone with the enforcer's rules) and through the enforcer's interface: the same
    // fields, the same values, for the same mainchain (the votes needed: one more than the threshold; the blocks: its
    // maximum age); what a way does not report is left out.
    const auto read{[](const std::string& json) {
        UniValue v;
        BOOST_REQUIRE(v.read(json));
        return v;
    }};
    const uint256 m6id{uint256::FromHex("1100000000000000000000000000000000000000000000000000000000000022").value()};
    const uint256 ctip{uint256::FromHex("00000000000000000000000000000000000000000000000000000000000000c1").value()};
    // The description of the slot (its compact size first, as GetSidechains sends it) and its hash.
    const std::vector<unsigned char> description{'d', 'e', 's', 'c'};
    const std::string proposal_hash{Hash(description).GetHex()};

    const UniValue rpc{PegInfoFromRpc(46,
        read(R"({"withdrawalperiod":60,"withdrawalminscore":30,"rules":"enforcer","bundlethreshold":5,"bundlemaxage":10,"height":211,"activesidechains":1,"proposals":0,"escrowtotal":"115.98000000"})"),
        read(strprintf(R"({"slot":46,"title":"Starbase","proposalhash":"%s","activationheight":118,"escrow":{"txid":"%s","n":0,"amount":"115.98000000"}})", proposal_hash, ctip.GetHex())),
        read(strprintf(R"([{"slot":46,"hash":"%s","index":0,"height":205,"score":3,"lastupvote":210,"blocksleft":4,"payable":false,"known":true,"vote":"upvote"}])", m6id.GetHex())))};
    const UniValue grpc{PegInfoFromEnforcer(46, EnforcerPegReplies{
        .chain_info = read(R"({"network":"NETWORK_REGTEST","bip300Constants":{"withdrawalBundleMaxAge":10,"withdrawalBundleInclusionThreshold":5}})"),
        .tip_height = 211,
        .sidechains = read(strprintf(R"({"sidechains":[{"sidechainNumber":46,"description":{"hex":"04%s"},"activationHeight":118,"declaration":{"v0":{"title":"Starbase"}}}]})", HexStr(description))),
        .proposals = read(R"({})"),
        .ctip = read(strprintf(R"({"ctip":{"txid":{"hex":"%s"},"value":"11598000000","sequenceNumber":"3"}})", ctip.GetHex())),
        .bundles = read(strprintf(R"({"proposals":[{"m6id":{"hex":"%s"},"voteCount":3,"proposalHeight":205}]})", HexStr(m6id))),
    })};
    for (const UniValue* info : {&rpc, &grpc}) {
        const UniValue& v{*info};
        BOOST_CHECK_EQUAL(v["height"].getInt<int>(), 211);
        BOOST_CHECK_EQUAL(v["rules"].get_str(), "enforcer");
        BOOST_CHECK_EQUAL(v["withdrawalperiod"].getInt<int>(), 10);
        BOOST_CHECK_EQUAL(v["withdrawalminscore"].getInt<int>(), 6);
        BOOST_CHECK_EQUAL(v["activesidechains"].getInt<int>(), 1);
        BOOST_CHECK_EQUAL(v["proposals"].getInt<int>(), 0);
        BOOST_CHECK(v["sidechain"]["active"].get_bool());
        BOOST_CHECK_EQUAL(v["sidechain"]["activationheight"].getInt<int>(), 118);
        BOOST_CHECK_EQUAL(v["sidechain"]["proposalhash"].get_str(), proposal_hash);
        BOOST_CHECK_EQUAL(v["sidechain"]["title"].get_str(), "Starbase");
        BOOST_CHECK_EQUAL(v["escrow"]["txid"].get_str(), ctip.GetHex());
        BOOST_CHECK_EQUAL(v["escrow"]["amount"].getValStr(), "115.98000000");
        BOOST_REQUIRE_EQUAL(v["bundles"].size(), 1U);
        const UniValue& b{v["bundles"][0]};
        BOOST_CHECK_EQUAL(b["hash"].get_str(), m6id.GetHex());
        BOOST_CHECK_EQUAL(b["height"].getInt<int>(), 205);
        BOOST_CHECK_EQUAL(b["score"].getInt<int>(), 3);
        BOOST_CHECK_EQUAL(b["blocksleft"].getInt<int>(), 4);
        BOOST_CHECK(!b["payable"].get_bool());
    }
    // Only JSON-RPC says how the node votes, and has the votes of past blocks.
    BOOST_CHECK_EQUAL(rpc["bundles"][0]["vote"].get_str(), "upvote");
    BOOST_CHECK(grpc["bundles"][0].find_value("vote").isNull());
    BOOST_CHECK(rpc["votehistory"].get_bool());
    BOOST_CHECK(!grpc["votehistory"].get_bool());

    // The Chains rules: the period and the score of getdrivechaininfo. No sidechain in the slot: not active, no escrow.
    const UniValue chains{PegInfoFromRpc(5, read(R"({"withdrawalperiod":60,"withdrawalminscore":30,"height":12,"activesidechains":0,"proposals":1})"), NullUniValue, read("[]"))};
    BOOST_CHECK_EQUAL(chains["rules"].get_str(), "chains");
    BOOST_CHECK_EQUAL(chains["withdrawalperiod"].getInt<int>(), 60);
    BOOST_CHECK_EQUAL(chains["withdrawalminscore"].getInt<int>(), 30);
    BOOST_CHECK(!chains["sidechain"]["active"].get_bool());
    BOOST_CHECK(chains.find_value("escrow").isNull());

    // An enforcer without the BIP300 constants, GetSidechainProposals nor GetCtip (17eb2ce-like), and with a bundle
    // past its threshold: no votes needed nor blocks left said, nothing made up; the rest as it is.
    const UniValue old{PegInfoFromEnforcer(46, EnforcerPegReplies{
        .chain_info = read(R"({"network":"NETWORK_MAINNET"})"),
        .tip_height = 900,
        .sidechains = read(R"({})"),
        .proposals = NullUniValue,
        .ctip = NullUniValue,
        .bundles = read(strprintf(R"({"proposals":[{"m6id":{"hex":"%s"},"voteCount":13151,"proposalHeight":100}]})", HexStr(m6id))),
    })};
    BOOST_CHECK(old.find_value("withdrawalperiod").isNull());
    BOOST_CHECK(old.find_value("withdrawalminscore").isNull());
    BOOST_CHECK(old.find_value("proposals").isNull());
    BOOST_CHECK(old.find_value("escrow").isNull());
    BOOST_CHECK_EQUAL(old["activesidechains"].getInt<int>(), 0);
    BOOST_CHECK(!old["sidechain"]["active"].get_bool());
    BOOST_CHECK_EQUAL(old["bundles"][0]["score"].getInt<int>(), 13151);
    BOOST_CHECK(old["bundles"][0].find_value("payable").isNull());
    BOOST_CHECK(old["bundles"][0].find_value("blocksleft").isNull());
}

BOOST_AUTO_TEST_CASE(peg_info_from_rpc)
{
    // getpeginfo over JSON-RPC: the Chains rules (the period and the score of getdrivechaininfo), the slot and its
    // escrow (getsidechain), the bundles of the slot only (listwithdrawalbundles); what a reply does not say is left out.
    const auto read{[](const std::string& json) {
        UniValue v;
        BOOST_REQUIRE(v.read(json));
        return v;
    }};
    const std::string bundle(64, 'c');
    const std::string escrow_txid(64, 'e');
    const UniValue chains{PegInfoFromRpc(7,
        read(R"({"withdrawalperiod":60,"withdrawalminscore":30,"height":211,"activesidechains":2,"proposals":1})"),
        read(strprintf(R"({"slot":7,"title":"Layers","proposalhash":"%s","activationheight":118,"escrow":{"txid":"%s","n":1,"amount":115.98}})", std::string(64, 'a'), escrow_txid)),
        read(strprintf(R"([{"slot":7,"hash":"%s","index":0,"height":205,"score":3,"lastupvote":210,"blocksleft":54,"payable":false,"known":true,"vote":"upvote"},
                           {"slot":8,"hash":"%s","height":205,"score":9}])", bundle, std::string(64, 'd'))))};
    BOOST_CHECK_EQUAL(chains["height"].getInt<int>(), 211);
    BOOST_CHECK_EQUAL(chains["rules"].get_str(), "chains");
    BOOST_CHECK_EQUAL(chains["withdrawalperiod"].getInt<int>(), 60);
    BOOST_CHECK_EQUAL(chains["withdrawalminscore"].getInt<int>(), 30);
    BOOST_CHECK_EQUAL(chains["activesidechains"].getInt<int>(), 2);
    BOOST_CHECK_EQUAL(chains["proposals"].getInt<int>(), 1);
    BOOST_CHECK(chains["sidechain"]["active"].get_bool());
    BOOST_CHECK_EQUAL(chains["sidechain"]["slot"].getInt<int>(), 7);
    BOOST_CHECK_EQUAL(chains["sidechain"]["activationheight"].getInt<int>(), 118);
    BOOST_CHECK_EQUAL(chains["sidechain"]["title"].get_str(), "Layers");
    BOOST_CHECK_EQUAL(chains["escrow"]["txid"].get_str(), escrow_txid);
    BOOST_CHECK_EQUAL(chains["escrow"]["n"].getInt<int>(), 1);
    BOOST_CHECK_EQUAL(chains["escrow"]["amount"].getValStr(), "115.98000000");
    BOOST_REQUIRE_EQUAL(chains["bundles"].size(), 1U);
    const UniValue& b{chains["bundles"][0]};
    BOOST_CHECK_EQUAL(b["hash"].get_str(), bundle);
    BOOST_CHECK_EQUAL(b["height"].getInt<int>(), 205);
    BOOST_CHECK_EQUAL(b["score"].getInt<int>(), 3);
    BOOST_CHECK_EQUAL(b["blocksleft"].getInt<int>(), 54);
    BOOST_CHECK(!b["payable"].get_bool());
    BOOST_CHECK_EQUAL(b["vote"].get_str(), "upvote");
    BOOST_CHECK(chains["votehistory"].get_bool());

    // The enforcer's rules (Backbone on eCash): more votes than the threshold, within the maximum age.
    const UniValue enforcer{PegInfoFromRpc(46, read(R"({"withdrawalperiod":60,"withdrawalminscore":30,"rules":"enforcer","bundlethreshold":5,"bundlemaxage":10,"height":12})"), NullUniValue, read("[]"))};
    BOOST_CHECK_EQUAL(enforcer["rules"].get_str(), "enforcer");
    BOOST_CHECK_EQUAL(enforcer["withdrawalperiod"].getInt<int>(), 10);
    BOOST_CHECK_EQUAL(enforcer["withdrawalminscore"].getInt<int>(), 6);

    // No sidechain in the slot, replies of another shape: not active, no escrow, nothing made up.
    const UniValue none{PegInfoFromRpc(5, read(R"({"height":"x"})"), NullUniValue, read(R"({"hash":"00"})"))};
    BOOST_CHECK_EQUAL(none["height"].getInt<int>(), -1);
    BOOST_CHECK(!none["sidechain"]["active"].get_bool());
    BOOST_CHECK(none.find_value("escrow").isNull());
    BOOST_CHECK(none.find_value("withdrawalperiod").isNull());
    BOOST_CHECK(none.find_value("withdrawalminscore").isNull());
    BOOST_CHECK_EQUAL(none["bundles"].size(), 0U);
    // An escrow amount that is not one is left out.
    const UniValue bad{PegInfoFromRpc(5, read("{}"), read(R"({"escrow":{"txid":"ab","n":0,"amount":-1}})"), read("[]"))};
    BOOST_CHECK(bad["sidechain"]["active"].get_bool());
    BOOST_CHECK(bad.find_value("escrow").isNull());
}

BOOST_AUTO_TEST_SUITE_END()
