// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/mainclient.h>

#include <addresstype.h>
#include <base58.h>
#include <bech32.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <drivechain/sidechain.h>
#include <hash.h>
#include <netbase.h>
#include <primitives/transaction.h>
#include <rpc/util.h>
#include <streams.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <support/cleanse.h>
#include <util/sock.h>
#include <util/strencodings.h>
#include <util/threadinterrupt.h>
#include <util/time.h>
#include <util/moneystr.h>
#include <util/translation.h>

#include <algorithm>
#include <cassert>
#include <fstream>

#ifndef WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sidechain {
namespace {
constexpr auto TIMEOUT{60s};
//! Largest reply accepted, to bound what a misbehaving server can make this node hold.
constexpr size_t MAX_REPLY_SIZE{256 * 1024 * 1024};

std::string PercentEncode(const std::string& text)
{
    std::string encoded;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += static_cast<char>(c);
        } else {
            encoded += strprintf("%%%02X", c);
        }
    }
    return encoded;
}
} // namespace

std::optional<std::string> DecodeChunked(const std::string& body, size_t max_size, std::string& error)
{
    std::string joined;
    size_t pos{0};
    for (;;) {
        const size_t eol{body.find("\r\n", pos)};
        if (eol == std::string::npos) {
            error = "a truncated chunked reply";
            return std::nullopt;
        }
        const auto size{ToIntegral<uint64_t>(std::string_view{body}.substr(pos, eol - pos), 16)};
        if (!size) {
            error = "a malformed chunk";
            return std::nullopt;
        }
        if (*size == 0) return joined;
        // The chunk and its CRLF must be in the reply: a size beyond them is malformed, not a reason to skip ahead.
        const size_t start{eol + 2};
        if (*size > body.size() - start || body.size() - start - *size < 2 || body.compare(start + *size, 2, "\r\n") != 0) {
            error = "a malformed chunk";
            return std::nullopt;
        }
        if (joined.size() + *size > max_size) {
            error = "a reply too large";
            return std::nullopt;
        }
        joined.append(body, start, *size);
        pos = start + *size + 2; // always ahead of eol, so of pos
    }
}

uint16_t DefaultMainchainRPCPort(ChainType chain)
{
    switch (chain) {
    case ChainType::MAIN: return 9554;
    case ChainType::TESTNET: return 19554;
    case ChainType::SIGNET: return 39554;
    case ChainType::REGTEST: return 29554;
    // Backbone's, which are those of eCash's own node (betanet's too: it is its own client's main network).
    case ChainType::ECASH: return 8532;
    case ChainType::ECASH_BETANET: return 8532;
    case ChainType::ECASH_TESTNET: return 48532;
    }
    assert(false);
}

std::string MainchainDataSubdir(ChainType chain)
{
    switch (chain) {
    case ChainType::MAIN: return "";
    case ChainType::TESTNET: return "testnet";
    case ChainType::SIGNET: return "signet";
    case ChainType::REGTEST: return "regtest";
    // As Backbone (and eCash's own client) lays out its data directory.
    case ChainType::ECASH: return "";
    case ChainType::ECASH_BETANET: return "betanet";
    case ChainType::ECASH_TESTNET: return "testnet4";
    }
    assert(false);
}

std::string MainchainName(ChainType chain)
{
    return "Backbone";
}

util::Result<std::string> ReadPasswordFile(const fs::path& path)
{
    //! A password is one short line; a larger file is not one.
    constexpr size_t MAX_SIZE{4096};
    const std::string name{fs::PathToString(path)};
    std::string content;
#ifndef WIN32
    const int fd{open(path.c_str(), O_RDONLY | O_NOCTTY | O_CLOEXEC)};
    if (fd < 0) return util::Error{Untranslated(strprintf("Cannot open the password file %s", name))};
    struct stat st;
    char buffer[MAX_SIZE + 1];
    const auto fail{[&](const std::string& why) {
        close(fd);
        memory_cleanse(buffer, sizeof(buffer));
        return util::Error{Untranslated(strprintf("The password file %s %s", name, why))};
    }};
    if (fstat(fd, &st) != 0) return fail("cannot be read");
    if (!S_ISREG(st.st_mode)) return fail("is not a regular file");
    if (st.st_uid != geteuid()) return fail("is not owned by this user");
    if (st.st_mode & (S_IRWXG | S_IRWXO)) return fail("can be read or written by other users: make it readable by its owner only (chmod 600)");
    ssize_t total{0};
    while (total <= static_cast<ssize_t>(MAX_SIZE)) {
        const ssize_t n{read(fd, buffer + total, sizeof(buffer) - total)};
        if (n < 0) return fail("cannot be read");
        if (n == 0) break;
        total += n;
    }
    close(fd);
    content.assign(buffer, total);
    // Do not leave a copy of the password on the stack.
    memory_cleanse(buffer, sizeof(buffer));
#else
    std::ifstream file{path.std_path(), std::ios::binary};
    if (!file.is_open()) return util::Error{Untranslated(strprintf("Cannot open the password file %s", name))};
    content.resize(MAX_SIZE + 1);
    file.read(content.data(), content.size());
    content.resize(file.gcount());
#endif
    if (content.size() > MAX_SIZE) return util::Error{Untranslated(strprintf("The password file %s is too large to hold a password", name))};
    const size_t end{content.find_first_of("\r\n")};
    std::string password{content.substr(0, end)};
    memory_cleanse(content.data(), content.size());
    if (password.empty()) return util::Error{Untranslated(strprintf("The password file %s holds no password on its first line", name))};
    return password;
}

std::string MainClient::Exchange(const std::string& host, uint16_t port, const std::string& request) const
{
    const auto unreachable{[&](const std::string& why) {
        return MainClientError{strprintf("Cannot reach the mainchain node at %s:%u: %s", host, port, why), /*rpc_error_in=*/false};
    }};
    const auto address{Lookup(host, port, /*fAllowLookup=*/true)};
    if (!address) throw unreachable("unknown host");
    const std::unique_ptr<Sock> sock{ConnectDirectly(*address, /*manual_connection=*/true)};
    if (!sock) throw unreachable("connection refused");

    std::string reply;
    try {
        CThreadInterrupt interrupt;
        sock->SendComplete(request, TIMEOUT, interrupt);
        const auto deadline{SteadyClock::now() + TIMEOUT};
        char buffer[65536];
        while (true) {
            Sock::Event occurred{0};
            const auto remaining{std::chrono::duration_cast<std::chrono::milliseconds>(deadline - SteadyClock::now())};
            if (remaining <= 0ms || !sock->Wait(remaining, Sock::RecvEvent, &occurred)) throw std::runtime_error("timeout");
            if (!(occurred & Sock::RecvEvent)) continue;
            const ssize_t received{sock->Recv(buffer, sizeof(buffer), 0)};
            if (received < 0) {
                const int error{WSAGetLastError()};
                if (error == WSAEWOULDBLOCK || error == WSAEINTR || error == WSAEINPROGRESS) continue;
                throw std::runtime_error(NetworkErrorString(error));
            }
            if (received == 0) break;
            reply.append(buffer, received);
            if (reply.size() > MAX_REPLY_SIZE) throw std::runtime_error("reply too large");
        }
    } catch (const std::runtime_error& e) {
        throw unreachable(e.what());
    }

    return reply;
}

UniValue MainClient::CallEnforcer(const std::string& procedure, const UniValue& request) const
{
    const std::string body{request.write()};
    std::string auth;
    if (!m_options.enforcer_token.empty()) auth = "Authorization: Bearer " + m_options.enforcer_token + "\r\n";
    // An IP address or localhost as the Host: Backbone refuses other names (DNS rebinding).
    const std::string reply{Exchange(EnforcerHost(), m_options.enforcer_port, strprintf("POST /%s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n%sContent-Type: application/json\r\nConnect-Protocol-Version: 1\r\nContent-Length: %u\r\n\r\n%s",
                                                                      procedure, EnforcerHost(), m_options.enforcer_port, auth, body.size(), body))};
    const size_t header_end{reply.find("\r\n\r\n")};
    if (!reply.starts_with("HTTP/1.") || header_end == std::string::npos || reply.size() < 12) {
        throw MainClientError{strprintf("The enforcer interface at %s:%u did not answer %s over HTTP", EnforcerHost(), m_options.enforcer_port, procedure), /*rpc_error_in=*/false};
    }
    // Backbone's token refused (-grpcauth): said as the JSON-RPC credentials are, for the link's state.
    if (reply.compare(9, 3, "401") == 0) throw MainClientError{"The mainchain node refused the credentials (the token of its enforcer interface, -mainchaingrpcauth)", /*rpc_error_in=*/false};
    const std::string status{reply.substr(9, 3)};
    std::string payload{reply.substr(header_end + 4)};
    // A chunked reply (a Connect server may stream a unary one so): put back together.
    if (ToLower(reply.substr(0, header_end)).find("transfer-encoding: chunked") != std::string::npos) {
        std::string error;
        auto joined{DecodeChunked(payload, MAX_REPLY_SIZE, error)};
        if (!joined) throw MainClientError{strprintf("The enforcer interface answered %s with %s", procedure, error), /*rpc_error_in=*/false};
        payload = std::move(*joined);
    }
    UniValue answer;
    if (!answer.read(payload) || !answer.isObject()) {
        throw MainClientError{strprintf("The enforcer interface answered %s with HTTP status %s and no JSON", procedure, status), /*rpc_error_in=*/false};
    }
    if (status != "200") {
        // A Connect error: {"code": "invalid_argument", "message": ...}.
        const std::string code{answer.find_value("code").isStr() ? answer.find_value("code").get_str() : status};
        const std::string message{answer.find_value("message").isStr() ? answer.find_value("message").get_str() : ""};
        throw MainClientError{strprintf("The enforcer interface answered %s with an error: %s: %s", procedure, code, message), /*rpc_error_in=*/true, code + ": " + message};
    }
    return answer;
}

UniValue MainClient::Call(const std::string& method, const UniValue& params, bool wallet) const
{
    if (m_options.enforcer_only) return CallAsEnforcer(method, params);
    const auto unreachable{[&](const std::string& why) {
        return MainClientError{strprintf("Cannot reach the mainchain node at %s:%u: %s", m_options.host, m_options.port, why), /*rpc_error_in=*/false};
    }};

    std::string credentials{m_options.credentials};
    if (credentials.empty()) {
        std::ifstream file{m_options.cookie_file.std_path()};
        if (!file.is_open() || !std::getline(file, credentials)) {
            throw unreachable(strprintf("no credentials: set -mainchainrpcuser and -mainchainrpcpassword, or check that the cookie file %s can be read", fs::PathToString(m_options.cookie_file)));
        }
    }

    const std::string body{JSONRPCRequestObj(method, params, 1).write() + "\n"};
    std::string uri{"/"};
    if (wallet && m_options.wallet_set) uri = "/wallet/" + PercentEncode(m_options.wallet);
    const std::string request{strprintf("POST %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAuthorization: Basic %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\n\r\n%s",
                                        uri, m_options.host, EncodeBase64(credentials), body.size(), body)};

    const std::string reply{Exchange(m_options.host, m_options.port, request)};

    const size_t header_end{reply.find("\r\n\r\n")};
    if (!reply.starts_with("HTTP/1.") || header_end == std::string::npos || reply.size() < 12) throw unreachable("not an HTTP reply");
    const std::string status{reply.substr(9, 3)};
    if (status == "401") throw MainClientError{"The mainchain node refused the credentials", /*rpc_error_in=*/false};
    if (status == "403") throw MainClientError{"The mainchain node does not allow connections from this address (rpcallowip)", /*rpc_error_in=*/false};

    UniValue answer;
    if (!answer.read(reply.substr(header_end + 4)) || !answer.isObject()) {
        throw unreachable(strprintf("unexpected reply with HTTP status %s", status));
    }
    const UniValue& error{answer.find_value("error")};
    if (!error.isNull()) {
        const UniValue& message{error.find_value("message")};
        const std::string text{message.isStr() ? message.get_str() : error.write()};
        throw MainClientError{strprintf("The mainchain node answered %s with an error: %s", method, text), /*rpc_error_in=*/true, text};
    }
    return answer.find_value("result");
}


//
// Following the mainchain through the bip300301_enforcer's interface alone (-mainchaingrpc).
//

namespace {
const std::string VALIDATOR{"cusf.mainchain.v1.ValidatorService/"};
const std::string WALLET{"cusf.mainchain.v1.WalletService/"};
//! Headers asked for at a time (GetBlockHeaderInfo).
constexpr int HEADER_BATCH{2000};
//! Headers kept below the lowest asked for, for the follower's walks back (where the chains part).
constexpr int HEADER_KEEP_BELOW{4032};

/** A field of a message in the proto3 JSON mapping: its lowerCamelCase name, which servers write, or its own. */
const UniValue& Field(const UniValue& obj, const char* json_name, const char* name)
{
    if (!obj.isObject()) return NullUniValue;
    const UniValue& value{obj.find_value(json_name)};
    return value.isNull() ? obj.find_value(name) : value;
}

/** The hex of a ConsensusHex, ReverseHex or Hex field ({"hex": ...}), if set. */
std::optional<std::string> HexField(const UniValue& obj, const char* json_name, const char* name)
{
    const UniValue& value{Field(obj, json_name, name)};
    if (value.isNull()) return std::nullopt;
    const UniValue& hex{value.isObject() ? value.find_value("hex") : value};
    if (hex.isNull()) return std::string{};
    if (!hex.isStr()) throw std::runtime_error(strprintf("the enforcer sent a %s that is not hex", json_name));
    return hex.get_str();
}

/** An integer field (a uint64 is a string in the JSON mapping, a uint32 a number; a wrapper is its value), 0 if not set. */
uint64_t UIntField(const UniValue& obj, const char* json_name, const char* name)
{
    const UniValue& value{Field(obj, json_name, name)};
    if (value.isNull()) return 0;
    if (value.isNum()) return value.getInt<uint64_t>();
    if (value.isStr()) {
        if (const auto n{ToIntegral<uint64_t>(value.get_str())}) return *n;
    }
    throw std::runtime_error(strprintf("the enforcer sent a %s that is not a number", json_name));
}

/** A block hash or txid (ReverseHex: as GetHex writes it). */
uint256 ReverseHash(const std::optional<std::string>& hex, const std::string& what)
{
    const auto hash{hex ? uint256::FromHex(*hex) : std::nullopt};
    if (!hash) throw std::runtime_error(strprintf("the enforcer sent a %s that is not a hash", what));
    return *hash;
}

/** A hash in its bytes' order (ConsensusHex: m6id, the BMM commitment): the reverse of GetHex. */
uint256 ConsensusHash(const std::optional<std::string>& hex, const std::string& what)
{
    const auto bytes{hex ? TryParseHex<unsigned char>(*hex) : std::nullopt};
    if (!bytes || bytes->size() != uint256::size()) throw std::runtime_error(strprintf("the enforcer sent a %s that is not a hash", what));
    return uint256{std::span<const unsigned char>{*bytes}};
}

UniValue Request(std::initializer_list<std::pair<std::string, UniValue>> fields)
{
    UniValue obj(UniValue::VOBJ);
    for (const auto& [key, value] : fields) obj.pushKV(key, value);
    return obj;
}
UniValue HexValue(const std::string& hex) { return Request({{"hex", hex}}); }

/** The proposal hash of a sidechain of GetSidechains: SHA256d of its description (with no compact size first). */
uint256 EnforcerProposalHash(const UniValue& sidechain)
{
    const auto bytes{TryParseHex<unsigned char>(HexField(sidechain, "description", "description").value_or(""))};
    if (!bytes) throw std::runtime_error("the enforcer sent a sidechain description that is not hex");
    std::vector<unsigned char> description;
    try {
        SpanReader{*bytes} >> description;
    } catch (const std::ios_base::failure&) {
        throw std::runtime_error("the enforcer sent a malformed sidechain description");
    }
    return Hash(description);
}

/** An amount of coins of a JSON-RPC reply (a number, or a string as Backbone writes some), if it is one. */
std::optional<CAmount> Coins(const UniValue& value)
{
    if (!value.isNum() && !value.isStr()) return std::nullopt;
    const auto amount{ParseMoney(value.getValStr())};
    if (!amount || *amount < 0 || !MoneyRange(*amount)) return std::nullopt;
    return amount;
}

/** An integer of a JSON-RPC reply, if it is one. */
std::optional<int64_t> Int(const UniValue& obj, const std::string& key)
{
    const UniValue& value{obj.isObject() ? obj.find_value(key) : NullUniValue};
    if (!value.isNum()) return std::nullopt;
    try {
        return value.getInt<int64_t>();
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}
} // namespace

int64_t MedianTime(std::vector<int64_t> times)
{
    assert(!times.empty());
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

UniValue EnforcerBlockEvents(const UniValue& info, int64_t median_time)
{
    const UniValue& header{Field(info, "headerInfo", "header_info")};
    const UniValue& block{Field(info, "blockInfo", "block_info")};
    if (!header.isObject()) throw std::runtime_error("the enforcer sent a block without its header");
    UniValue out(UniValue::VOBJ);
    const uint64_t height{UIntField(header, "height", "height")};
    out.pushKV("height", height);
    out.pushKV("hash", ReverseHash(HexField(header, "blockHash", "block_hash"), "block hash").GetHex());
    if (height > 0) out.pushKV("previousblockhash", ReverseHash(HexField(header, "prevBlockHash", "prev_block_hash"), "block hash").GetHex());
    out.pushKV("time", UIntField(header, "timestamp", "timestamp"));
    out.pushKV("mediantime", median_time);
    if (const auto bmm{HexField(block, "bmmCommitment", "bmm_commitment")}) out.pushKV("bmm", ConsensusHash(bmm, "BMM commitment").GetHex());
    UniValue deposits(UniValue::VARR), closed(UniValue::VARR), proposed(UniValue::VARR);
    const UniValue& events{Field(block, "events", "events")};
    for (const UniValue& event : events.isArray() ? events.getValues() : std::vector<UniValue>{}) {
        if (const UniValue& deposit{Field(event, "deposit", "deposit")}; deposit.isObject()) {
            const UniValue& outpoint{Field(deposit, "outpoint", "outpoint")};
            const UniValue& output{Field(deposit, "output", "output")};
            // The destination: the bytes the deposit's OP_RETURN carries, as text (Backbone's "destination").
            const auto destination{TryParseHex<unsigned char>(HexField(output, "address", "address").value_or(""))};
            if (!destination) throw std::runtime_error("the enforcer sent a deposit address that is not hex");
            const uint64_t value{UIntField(output, "valueSats", "value_sats")};
            if (value > static_cast<uint64_t>(MAX_MONEY)) throw std::runtime_error("the enforcer sent a deposit of more than all the coins");
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("destination", std::string{destination->begin(), destination->end()});
            entry.pushKV("amount", ValueFromAmount(static_cast<CAmount>(value)));
            entry.pushKV("txid", ReverseHash(HexField(outpoint, "txid", "txid"), "txid").GetHex());
            entry.pushKV("burnindex", UIntField(outpoint, "vout", "vout"));
            deposits.push_back(std::move(entry));
            continue;
        }
        const UniValue& bundle{Field(event, "withdrawalBundle", "withdrawal_bundle")};
        if (!bundle.isObject()) throw std::runtime_error("the enforcer sent an event this node does not know");
        const uint256 m6id{ConsensusHash(HexField(bundle, "m6id", "m6id"), "m6id")};
        const UniValue& kind{Field(bundle, "event", "event")};
        if (!Field(kind, "submitted", "submitted").isNull()) {
            proposed.push_back(m6id.GetHex());
        } else if (!Field(kind, "failed", "failed").isNull()) {
            closed.push_back(Request({{"hash", m6id.GetHex()}, {"paid", false}}));
        } else if (const UniValue& succeeded{Field(kind, "succeeded", "succeeded")}; succeeded.isObject()) {
            // The payout (M6): its first output the new treasury, then what it paid. As Backbone reports it: an
            // escrow change of the withdrawal kind, the bundle, and the outputs with a value but the treasury's.
            CMutableTransaction tx;
            if (!DecodeHexTx(tx, HexField(succeeded, "transaction", "transaction").value_or(""), /*try_no_witness=*/true) || tx.vout.empty()) {
                throw std::runtime_error("the enforcer sent a payout that is not a transaction");
            }
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("destination", drivechain::WITHDRAWAL_RETURN_DEST);
            entry.pushKV("amount", ValueFromAmount(0));
            entry.pushKV("txid", tx.GetHash().GetHex());
            entry.pushKV("burnindex", 0);
            entry.pushKV("bundle", m6id.GetHex());
            UniValue payouts(UniValue::VARR);
            for (size_t i{1}; i < tx.vout.size(); ++i) {
                if (tx.vout[i].nValue == 0) continue;
                payouts.push_back(Request({{"amount", ValueFromAmount(tx.vout[i].nValue)}, {"script", HexStr(tx.vout[i].scriptPubKey)}}));
            }
            entry.pushKV("payouts", std::move(payouts));
            deposits.push_back(std::move(entry));
            closed.push_back(Request({{"hash", m6id.GetHex()}, {"paid", true}}));
        } else {
            throw std::runtime_error("the enforcer sent a withdrawal bundle event this node does not know");
        }
    }
    out.pushKV("deposits", std::move(deposits));
    out.pushKV("bundles", std::move(closed));
    out.pushKV("proposed", std::move(proposed));
    // Not reported by the interface; not used by the rules of the networks a node can follow so.
    out.pushKV("pending", UniValue{UniValue::VARR});
    return out;
}

std::optional<CScript> MainchainScript(const std::string& address, ChainType chain)
{
    std::vector<std::string> hrps;
    unsigned char pubkey_prefix{111}, script_prefix{196};
    switch (chain) {
    case ChainType::ECASH:
    case ChainType::ECASH_BETANET:
        hrps = {"bc"};
        pubkey_prefix = 0;
        script_prefix = 5;
        break;
    case ChainType::ECASH_TESTNET: hrps = {"tb"}; break;
    // A bitcoind's regtest, and Backbone's (whose regtest addresses keep Chains' prefix).
    case ChainType::REGTEST: hrps = {"bcrt", "rchn"}; break;
    default: return std::nullopt;
    }
    const auto decoded{bech32::Decode(address)};
    if (decoded.encoding != bech32::Encoding::INVALID) {
        if (std::find(hrps.begin(), hrps.end(), decoded.hrp) == hrps.end() || decoded.data.empty()) return std::nullopt;
        const int version{decoded.data[0]};
        std::vector<unsigned char> program;
        if (version > 16 || !ConvertBits<5, 8, false>([&](unsigned char c) { program.push_back(c); }, decoded.data.begin() + 1, decoded.data.end())) return std::nullopt;
        // BIP173 for version 0, BIP350 for the others.
        if ((version == 0) != (decoded.encoding == bech32::Encoding::BECH32)) return std::nullopt;
        if (program.size() < 2 || program.size() > 40) return std::nullopt;
        if (version == 0 && program.size() != 20 && program.size() != 32) return std::nullopt;
        CScript script;
        script << CScript::EncodeOP_N(version) << program;
        return script;
    }
    std::vector<unsigned char> data;
    if (!DecodeBase58Check(address, data, 21) || data.size() != 21) return std::nullopt;
    const uint160 hash{std::span<const unsigned char>{data}.subspan(1)};
    if (data[0] == pubkey_prefix) return GetScriptForDestination(PKHash{hash});
    if (data[0] == script_prefix) return GetScriptForDestination(ScriptHash{hash});
    return std::nullopt;
}

std::string MainClient::Address() const
{
    if (m_options.enforcer_only) return strprintf("%s:%u", EnforcerHost(), m_options.enforcer_port);
    return strprintf("%s:%u", m_options.host, m_options.port);
}

std::pair<int, uint256> MainClient::EnforcerTip() const
{
    UniValue tip;
    try {
        tip = CallEnforcer(VALIDATOR + "GetChainTip", UniValue{UniValue::VOBJ});
    } catch (const MainClientError& e) {
        // UNAVAILABLE "Validator is not synced": the enforcer has no tip yet.
        if (e.rpc_error && e.rpc_message.starts_with("unavailable")) {
            throw std::runtime_error(strprintf("The mainchain node is still syncing (its enforcer says %s); waiting for it to catch up", e.rpc_message));
        }
        throw;
    }
    const UniValue& info{Field(tip, "blockHeaderInfo", "block_header_info")};
    return {static_cast<int>(UIntField(info, "height", "height")), ReverseHash(HexField(info, "blockHash", "block_hash"), "block hash")};
}

std::vector<MainClient::Header> MainClient::EnforcerHeaders(const uint256& tip, int tip_height, int low) const
{
    // Headers from `hash` at `height` down to `down_to`, newest first, checked to follow each other.
    const auto walk{[&](uint256 hash, int height, int down_to) {
        std::vector<Header> got;
        while (height >= down_to) {
            const int count{std::min(HEADER_BATCH, height - down_to + 1)};
            const UniValue reply{CallEnforcer(VALIDATOR + "GetBlockHeaderInfo", Request({{"blockHash", HexValue(hash.GetHex())}, {"maxAncestors", count - 1}}))};
            const UniValue& infos{Field(reply, "headerInfos", "header_infos")};
            if (!infos.isArray() || infos.size() != static_cast<size_t>(count)) {
                throw std::runtime_error(strprintf("The mainchain node did not give the headers below height %d; asking again shortly", height));
            }
            for (const UniValue& info : infos.getValues()) {
                Header header{ReverseHash(HexField(info, "blockHash", "block_hash"), "block hash"), ReverseHash(HexField(info, "prevBlockHash", "prev_block_hash"), "block hash"),
                              static_cast<int64_t>(UIntField(info, "timestamp", "timestamp"))};
                if (header.hash != hash || static_cast<int>(UIntField(info, "height", "height")) != height) {
                    throw std::runtime_error(strprintf("The mainchain node sent blocks that do not follow each other (at height %d); asking again shortly", height));
                }
                got.push_back(header);
                hash = header.prev;
                --height;
            }
        }
        return got;
    }};
    LOCK(m_headers_mutex);
    const auto at{[&](int height) -> Header& { return m_headers[height - m_headers_base]; }};
    const int cached_tip{m_headers_base + static_cast<int>(m_headers.size()) - 1};
    if (!m_headers.empty()) {
        if (tip_height <= cached_tip && tip_height >= m_headers_base && at(tip_height).hash == tip) {
            // The tip went back on the same chain (or stayed).
            m_headers.resize(tip_height - m_headers_base + 1);
        } else if (tip_height > cached_tip) {
            // The new headers; if they do not build on the last one known, the chain changed below it: all again.
            const std::vector<Header> newer{walk(tip, tip_height, cached_tip + 1)};
            if (newer.back().prev == m_headers.back().hash) {
                m_headers.insert(m_headers.end(), newer.rbegin(), newer.rend());
            } else {
                m_headers.clear();
            }
        } else {
            m_headers.clear();
        }
    }
    if (m_headers.empty()) {
        const std::vector<Header> all{walk(tip, tip_height, low)};
        m_headers.assign(all.rbegin(), all.rend());
        m_headers_base = low;
    } else if (low < m_headers_base) {
        const std::vector<Header> older{walk(m_headers.front().prev, m_headers_base - 1, low)};
        m_headers.insert(m_headers.begin(), older.rbegin(), older.rend());
        m_headers_base = low;
    }
    // Not more than needed below what was asked for: the record moves up.
    if (const int drop{low - HEADER_KEEP_BELOW - m_headers_base}; drop > 0) {
        m_headers.erase(m_headers.begin(), m_headers.begin() + drop);
        m_headers_base += drop;
    }
    return {m_headers.begin() + (low - m_headers_base), m_headers.end()};
}

UniValue MainClient::EnforcerEvents(uint32_t slot, int first, int count) const
{
    UniValue result(UniValue::VARR);
    const auto [tip_height, tip]{EnforcerTip()};
    if (first < 0 || first > tip_height || count < 1) return result;
    // At most 1000 blocks a call, as the enforcer's own clients ask (the follower asks again for the rest).
    const int last{static_cast<int>(std::min<int64_t>(tip_height, int64_t{first} + std::min(count, 1000) - 1))};
    // The median time of a block takes the times of the 10 before it.
    const int low{std::max(0, first - 10)};
    const std::vector<Header> headers{EnforcerHeaders(tip, tip_height, low)};
    const int n{last - first + 1};
    const UniValue reply{CallEnforcer(VALIDATOR + "GetBlockInfo", Request({{"blockHash", HexValue(headers[last - low].hash.GetHex())}, {"sidechainId", static_cast<uint64_t>(slot)}, {"maxAncestors", n - 1}}))};
    const UniValue& infos{Field(reply, "infos", "infos")};
    // Newest first; fewer for blocks the enforcer has not connected (still syncing), none for a block it does not know.
    if (!infos.isArray() || infos.size() != static_cast<size_t>(n)) {
        throw std::runtime_error(strprintf("The mainchain node is still syncing: its enforcer has no info of the blocks from height %d; waiting for it to catch up", first));
    }
    for (int i{n - 1}; i >= 0; --i) {
        const UniValue& info{infos[i]};
        const int height{first + (n - 1 - i)};
        const UniValue& header{Field(info, "headerInfo", "header_info")};
        if (ReverseHash(HexField(header, "blockHash", "block_hash"), "block hash") != headers[height - low].hash || static_cast<int>(UIntField(header, "height", "height")) != height) {
            throw std::runtime_error(strprintf("The mainchain node sent blocks that do not follow each other (at height %d); asking again shortly", height));
        }
        std::vector<int64_t> times;
        for (int h{std::max(low, height - 10)}; h <= height; ++h) times.push_back(headers[h - low].time);
        result.push_back(EnforcerBlockEvents(info, MedianTime(std::move(times))));
    }
    return result;
}

UniValue MainClient::CallAsEnforcer(const std::string& method, const UniValue& params) const
{
    const auto param{[&](size_t i) -> const UniValue& { return params.isArray() && i < params.size() ? params[i] : NullUniValue; }};
    if (method == "getblockcount") return EnforcerTip().first;
    if (method == "getblockchaininfo") {
        const auto [height, tip]{EnforcerTip()};
        std::string network;
        {
            LOCK(m_headers_mutex);
            network = m_enforcer_network;
        }
        if (network.empty()) {
            // The interface served? (grpc.health.v1, as the enforcer's own clients check first.) An interface
            // without the health service says nothing either way.
            try {
                const UniValue health{CallEnforcer("grpc.health.v1.Health/Check", Request({{"service", "cusf.mainchain.v1.ValidatorService"}}))};
                if (health.find_value("status").isStr() && health.find_value("status").get_str() != "SERVING") {
                    throw std::runtime_error(strprintf("The enforcer interface at %s says its ValidatorService is %s", Address(), health.find_value("status").get_str()));
                }
            } catch (const MainClientError& e) {
                if (!e.rpc_error) throw;
                if (e.rpc_message.starts_with("not_found")) throw std::runtime_error(strprintf("The enforcer interface at %s does not serve the ValidatorService", Address()));
            }
            const UniValue info{CallEnforcer(VALIDATOR + "GetChainInfo", UniValue{UniValue::VOBJ})};
            const UniValue& value{Field(info, "network", "network")};
            const std::string name{value.isStr() ? value.get_str() : value.isNum() ? strprintf("%d", value.getInt<int>()) : "NETWORK_UNSPECIFIED"};
            // The enforcer's network is Bitcoin's (eCash's main network and its betanet are "mainnet", its test
            // network testnet4: "testnet"): this node's network if it is of that family.
            const ChainType chain{m_options.chain.value_or(ChainType::REGTEST)};
            const bool ours{(name == "NETWORK_MAINNET" && (chain == ChainType::ECASH || chain == ChainType::ECASH_BETANET)) ||
                            (name == "NETWORK_TESTNET" && chain == ChainType::ECASH_TESTNET) || (name == "NETWORK_REGTEST" && chain == ChainType::REGTEST)};
            network = ours ? ChainTypeToString(chain) : ToLower(name.starts_with("NETWORK_") ? name.substr(8) : name);
            LOCK(m_headers_mutex);
            m_enforcer_network = network;
        }
        UniValue info(UniValue::VOBJ);
        info.pushKV("network", network);
        info.pushKV("blocks", height);
        info.pushKV("headers", height);
        info.pushKV("bestblockhash", tip.GetHex());
        info.pushKV("initialblockdownload", false);
        return info;
    }
    if (method == "getsidechainevents") {
        return EnforcerEvents(param(0).getInt<uint32_t>(), param(1).getInt<int>(), param(2).isNull() ? 1 : param(2).getInt<int>());
    }
    if (method == "getsidechain") {
        const uint32_t slot{param(0).getInt<uint32_t>()};
        const UniValue reply{CallEnforcer(VALIDATOR + "GetSidechains", UniValue{UniValue::VOBJ})};
        const UniValue& sidechains{Field(reply, "sidechains", "sidechains")};
        for (const UniValue& sidechain : sidechains.isArray() ? sidechains.getValues() : std::vector<UniValue>{}) {
            if (UIntField(sidechain, "sidechainNumber", "sidechain_number") != slot) continue;
            // The proposal's hash: SHA256d of its description (ConsensusHex: with its compact size first).
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("slot", slot);
            obj.pushKV("activationheight", UIntField(sidechain, "activationHeight", "activation_height"));
            obj.pushKV("proposalhash", EnforcerProposalHash(sidechain).GetHex());
            return obj;
        }
        throw MainClientError{strprintf("No sidechain is active in slot %u", slot), /*rpc_error_in=*/true, "Sidechain not active"};
    }
    if (method == "validateaddress") {
        const auto script{param(0).isStr() && m_options.chain ? MainchainScript(param(0).get_str(), *m_options.chain) : std::nullopt};
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("isvalid", script.has_value());
        if (script) obj.pushKV("scriptPubKey", HexStr(*script));
        return obj;
    }
    if (method == "createsidechaindeposit") {
        // WalletService.CreateDepositTransaction (the enforcer's wallet): the address of this chain to credit, as it is.
        drivechain::DepositAddress deposit;
        if (!param(1).isStr() || drivechain::ParseDepositAddress(param(1).get_str(), deposit) != drivechain::DepositAddressKind::VALID) {
            throw MainClientError{"Not a deposit address", /*rpc_error_in=*/true, "Not a deposit address"};
        }
        const auto amount{param(2).isStr() ? ParseMoney(param(2).get_str()) : std::optional<CAmount>{AmountFromValue(param(2))}};
        if (!amount || *amount <= 0) throw MainClientError{"Not an amount", /*rpc_error_in=*/true, "Not an amount"};
        const UniValue reply{CallEnforcer(WALLET + "CreateDepositTransaction", Request({{"sidechainId", param(0).getInt<uint64_t>()}, {"address", deposit.address},
                                                                                       {"valueSats", strprintf("%d", *amount)}, {"feeSats", "10000"}}))};
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("txid", HexField(reply, "txid", "txid").value_or(""));
        return obj;
    }
    if (method == "getbestblockhash") return EnforcerTip().second.GetHex();
    // The wallet behind the interface (the enforcer's, or Backbone's -grpcwallet): the one that pays deposits and BMM
    // requests here. Its balance (getbalances, as the deposit helper reads it) and a new address of it.
    if (method == "getbalances") {
        const UniValue reply{CallEnforcer(WALLET + "GetBalance", UniValue{UniValue::VOBJ})};
        const auto coins{[&](const char* json_name, const char* name) {
            const uint64_t sats{UIntField(reply, json_name, name)};
            if (sats > static_cast<uint64_t>(MAX_MONEY)) throw std::runtime_error("the enforcer sent a balance of more than all the coins");
            return ValueFromAmount(static_cast<CAmount>(sats));
        }};
        UniValue mine(UniValue::VOBJ);
        mine.pushKV("trusted", coins("confirmedSats", "confirmed_sats"));
        mine.pushKV("untrusted_pending", coins("pendingSats", "pending_sats"));
        mine.pushKV("immature", ValueFromAmount(0));
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("mine", std::move(mine));
        return obj;
    }
    if (method == "getnewaddress") {
        const UniValue reply{CallEnforcer(WALLET + "CreateNewAddress", UniValue{UniValue::VOBJ})};
        const UniValue& address{Field(reply, "address", "address")};
        if (!address.isStr() || address.get_str().empty()) throw std::runtime_error("the enforcer sent no address");
        return address.get_str();
    }
    throw MainClientError{strprintf("The mainchain node is reached through the bip300301_enforcer's interface alone (-mainchaingrpc), which has no %s", method),
                          /*rpc_error_in=*/true, strprintf("%s is not available through the enforcer's interface", method)};
}


//
// getpeginfo: what the pages show of the peg, the same over JSON-RPC and through the enforcer's interface.
//

UniValue PegInfoFromRpc(uint32_t slot, const UniValue& drivechain, const UniValue& sidechain, const UniValue& bundles)
{
    UniValue out(UniValue::VOBJ);
    out.pushKV("height", Int(drivechain, "height").value_or(-1));
    const bool enforcer_rules{drivechain.isObject() && drivechain.find_value("rules").isStr() && drivechain.find_value("rules").get_str() == "enforcer"};
    out.pushKV("rules", enforcer_rules ? "enforcer" : "chains");
    // The votes a bundle needs, within how many blocks: the enforcer's rules pay it with more than the threshold.
    const auto threshold{Int(drivechain, "bundlethreshold")};
    const auto period{enforcer_rules ? Int(drivechain, "bundlemaxage") : Int(drivechain, "withdrawalperiod")};
    const auto min_score{enforcer_rules ? (threshold ? std::optional<int64_t>{*threshold + 1} : std::nullopt) : Int(drivechain, "withdrawalminscore")};
    if (period) out.pushKV("withdrawalperiod", *period);
    if (min_score) out.pushKV("withdrawalminscore", *min_score);
    if (const auto n{Int(drivechain, "activesidechains")}) out.pushKV("activesidechains", *n);
    if (const auto n{Int(drivechain, "proposals")}) out.pushKV("proposals", *n);

    UniValue side(UniValue::VOBJ);
    side.pushKV("slot", slot);
    side.pushKV("active", sidechain.isObject());
    if (sidechain.isObject()) {
        if (const auto h{Int(sidechain, "activationheight")}) side.pushKV("activationheight", *h);
        if (sidechain.find_value("proposalhash").isStr()) side.pushKV("proposalhash", sidechain.find_value("proposalhash").get_str());
        if (sidechain.find_value("title").isStr()) side.pushKV("title", sidechain.find_value("title").get_str());
    }
    out.pushKV("sidechain", std::move(side));
    const UniValue& escrow{sidechain.isObject() ? sidechain.find_value("escrow") : NullUniValue};
    if (escrow.isObject() && escrow.find_value("txid").isStr()) {
        if (const auto amount{Coins(escrow.find_value("amount"))}) {
            UniValue e(UniValue::VOBJ);
            e.pushKV("txid", escrow.find_value("txid").get_str());
            e.pushKV("n", Int(escrow, "n").value_or(0));
            e.pushKV("amount", ValueFromAmount(*amount));
            out.pushKV("escrow", std::move(e));
        }
    }

    UniValue list(UniValue::VARR);
    for (const UniValue& b : bundles.isArray() ? bundles.getValues() : std::vector<UniValue>{}) {
        if (!b.isObject() || !b.find_value("hash").isStr()) continue;
        if (const auto s{Int(b, "slot")}; s && *s != slot) continue;
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("hash", b.find_value("hash").get_str());
        entry.pushKV("height", Int(b, "height").value_or(-1));
        entry.pushKV("score", Int(b, "score").value_or(0));
        if (const auto left{Int(b, "blocksleft")}) entry.pushKV("blocksleft", std::max<int64_t>(0, *left));
        if (b.find_value("payable").isBool()) entry.pushKV("payable", b.find_value("payable").get_bool());
        if (b.find_value("vote").isStr()) entry.pushKV("vote", b.find_value("vote").get_str());
        list.push_back(std::move(entry));
    }
    out.pushKV("bundles", std::move(list));
    // getsidechainevents of a JSON-RPC node has the score of each pending bundle at each block.
    out.pushKV("votehistory", true);
    return out;
}

UniValue PegInfoFromEnforcer(uint32_t slot, const EnforcerPegReplies& replies)
{
    UniValue out(UniValue::VOBJ);
    out.pushKV("height", replies.tip_height);
    out.pushKV("rules", "enforcer");
    const UniValue& constants{Field(replies.chain_info, "bip300Constants", "bip300_constants")};
    std::optional<int64_t> max_age, threshold;
    if (constants.isObject()) {
        if (const uint64_t n{UIntField(constants, "withdrawalBundleMaxAge", "withdrawal_bundle_max_age")}) max_age = n;
        // (0 would be a threshold of no votes: proto3 leaves out a zero, so it is read as not reported.)
        if (const uint64_t n{UIntField(constants, "withdrawalBundleInclusionThreshold", "withdrawal_bundle_inclusion_threshold")}) threshold = n;
    }
    if (max_age) out.pushKV("withdrawalperiod", *max_age);
    if (threshold) out.pushKV("withdrawalminscore", *threshold + 1);

    const UniValue& sidechains{Field(replies.sidechains, "sidechains", "sidechains")};
    const std::vector<UniValue> active{sidechains.isArray() ? sidechains.getValues() : std::vector<UniValue>{}};
    out.pushKV("activesidechains", static_cast<int64_t>(active.size()));
    if (replies.proposals.isObject()) {
        const UniValue& proposals{Field(replies.proposals, "sidechainProposals", "sidechain_proposals")};
        out.pushKV("proposals", static_cast<int64_t>(proposals.isArray() ? proposals.size() : 0));
    }
    UniValue side(UniValue::VOBJ);
    side.pushKV("slot", slot);
    const UniValue* ours{nullptr};
    for (const UniValue& s : active) {
        if (UIntField(s, "sidechainNumber", "sidechain_number") == slot) ours = &s;
    }
    side.pushKV("active", ours != nullptr);
    if (ours) {
        side.pushKV("activationheight", UIntField(*ours, "activationHeight", "activation_height"));
        side.pushKV("proposalhash", EnforcerProposalHash(*ours).GetHex());
        const UniValue& title{Field(Field(Field(*ours, "declaration", "declaration"), "v0", "v0"), "title", "title")};
        if (title.isStr()) side.pushKV("title", title.get_str());
    }
    out.pushKV("sidechain", std::move(side));
    // The escrow (the Ctip): none before the first deposit.
    const UniValue& ctip{Field(replies.ctip, "ctip", "ctip")};
    if (ctip.isObject()) {
        const uint64_t value{UIntField(ctip, "value", "value")};
        if (value > static_cast<uint64_t>(MAX_MONEY)) throw std::runtime_error("the enforcer sent an escrow of more than all the coins");
        UniValue e(UniValue::VOBJ);
        e.pushKV("txid", ReverseHash(HexField(ctip, "txid", "txid"), "txid").GetHex());
        e.pushKV("n", UIntField(ctip, "vout", "vout"));
        e.pushKV("amount", ValueFromAmount(static_cast<CAmount>(value)));
        out.pushKV("escrow", std::move(e));
    }

    UniValue list(UniValue::VARR);
    const UniValue& proposals{Field(replies.bundles, "proposals", "proposals")};
    for (const UniValue& p : proposals.isArray() ? proposals.getValues() : std::vector<UniValue>{}) {
        UniValue entry(UniValue::VOBJ);
        const int64_t height{static_cast<int64_t>(UIntField(p, "proposalHeight", "proposal_height"))};
        const int64_t score{static_cast<int64_t>(UIntField(p, "voteCount", "vote_count"))};
        entry.pushKV("hash", ConsensusHash(HexField(p, "m6id", "m6id"), "m6id").GetHex());
        entry.pushKV("height", height);
        entry.pushKV("score", score);
        // Failed once older than the maximum age; paid with more votes than the threshold.
        if (max_age && replies.tip_height >= 0) entry.pushKV("blocksleft", std::max<int64_t>(0, *max_age - (replies.tip_height - height)));
        if (threshold) entry.pushKV("payable", score > *threshold);
        list.push_back(std::move(entry));
    }
    out.pushKV("bundles", std::move(list));
    // The interface reports the votes as they are now only: not those of each past block.
    out.pushKV("votehistory", false);
    return out;
}

UniValue MainClient::PegInfo(uint32_t slot) const
{
    if (!m_options.enforcer_only) {
        UniValue args(UniValue::VARR);
        args.push_back(static_cast<int64_t>(slot));
        UniValue sidechain;
        try {
            sidechain = Call("getsidechain", args);
        } catch (const MainClientError& e) {
            // None active in the slot (yet): said so, not an error.
            if (!e.rpc_error) throw;
        }
        return PegInfoFromRpc(slot, Call("getdrivechaininfo", UniValue{UniValue::VARR}), sidechain, Call("listwithdrawalbundles", args));
    }
    EnforcerPegReplies replies;
    replies.tip_height = EnforcerTip().first;
    replies.chain_info = CallEnforcer(VALIDATOR + "GetChainInfo", UniValue{UniValue::VOBJ});
    replies.sidechains = CallEnforcer(VALIDATOR + "GetSidechains", UniValue{UniValue::VOBJ});
    // Left out when refused, not an error: what an older interface does not serve, the bundles of a slot not active.
    const auto served{[&](const std::string& procedure, const UniValue& request) {
        try {
            return CallEnforcer(VALIDATOR + procedure, request);
        } catch (const MainClientError& e) {
            if (!e.rpc_error) throw;
            return UniValue{};
        }
    }};
    replies.proposals = served("GetSidechainProposals", UniValue{UniValue::VOBJ});
    replies.bundles = served("GetWithdrawalBundleProposals", Request({{"sidechainId", static_cast<uint64_t>(slot)}}));
    replies.ctip = served("GetCtip", Request({{"sidechainNumber", static_cast<uint64_t>(slot)}}));
    return PegInfoFromEnforcer(slot, replies);
}

} // namespace sidechain
