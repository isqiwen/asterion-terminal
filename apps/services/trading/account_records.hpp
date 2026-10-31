#pragma once
#include "account_policy.hpp"
#include <asterion/futures/broker_execution.hpp>
#include <asterion/trading.pb.h>
#include <asterion/base/serialization.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace asterion::trading {

// Pure durable-header validation; never opens a broker or restores permission.
Json account_manifest(const Json& header);

// Account commands and journal records as types. JSON is only their durable
// form: it is read and written here. Business operations are decoded directly
// from Protobuf; the JSON command is retained only for journaling and deduplication.
struct SubmitOrder
{
    LimitOrder order;
    Offset offset;
};

struct CancelOrder
{
    std::string order_id;
};

struct Authorize
{
    std::string user_id;
};

struct Revoke
{};

struct ResolveOrder
{
    std::string order_id;
};

// The definition belongs to AccountPolicy, which alone interprets it.
struct ChangePolicy
{
    PolicyDefinition definition;
};

struct AccountRequest
{
    using Operation =
        std::variant<SubmitOrder, CancelOrder, Authorize, Revoke, ResolveOrder, ChangePolicy>;
    std::string id;
    Operation operation;
    // Durable form. A request ID seen again must carry exactly this content.
    Json command;
    static AccountRequest from_wire(const protocol::proto::Command& command);
    static AccountRequest from_record(const Json& command);

    template <class T>
    const T* as() const noexcept
    {
        return std::get_if<T>(&operation);
    }

    std::string_view action() const noexcept;
    // The order a submission, cancellation or resolution names; otherwise empty.
    std::string_view order_id() const noexcept;
};

// The owner's permission to send orders. It lasts until the owner revokes it,
// the policy changes or the process ends; recorded, never restored.
struct Authorization
{
    std::int64_t authorized_at_ms = 0;
    Json json() const;
};

struct CommandRecord
{
    AccountRequest request;
    std::string policy_revision;
    // SubmitOrder: the identity the broker will report.
    std::string broker_key, trading_day;
    // ChangePolicy: the revision and algorithm it installed.
    std::string new_policy_revision;
};

// A broker-confirmed final state that retires a recorded intent.
struct TerminalOrder
{
    std::string order_id, broker_key, trading_day;
    BrokerOrderStatus status = BrokerOrderStatus::rejected;
    Decimal filled;
    std::string exchange_order_id;
    int error_code = 0;
};

struct OrdersTerminal
{
    static constexpr std::size_t batch = 32;
    std::vector<TerminalOrder> orders;
};

// Positive local evidence that a recorded order never reached the SDK.
struct OrderNotSent
{
    std::string order_id;
    int error_code = 0;
};

// SDK acceptance of a cancel request, or its absence; never exchange cancellation.
struct CancelResult
{
    std::string request_id;
    bool acknowledged = false;
};

using JournalRecord = std::variant<CommandRecord, OrdersTerminal, OrderNotSent, CancelResult>;
// Any record after the journal header. Throws for content this engine never writes.
JournalRecord parse_record(const Json& record);

// A record with the identities the journal indexes in the same transaction.
struct JournalEntry
{
    Json body;
    std::string command_id, order_id, excluded_order;

    // Diagnostic identity only; never read back.
    struct Trace
    {
        std::string request_id, action, order_id, broker_key, outcome;
    } trace;
};

JournalEntry command_entry(const AccountRequest& request, std::string_view policy_revision);
JournalEntry submission_entry(
    const AccountRequest& request,
    std::string_view policy_revision,
    std::string_view broker_key,
    std::string_view trading_day);
JournalEntry policy_entry(
    const AccountRequest& request,
    std::string_view policy_revision,
    std::string_view new_policy_revision);
JournalEntry authorization_entry(
    const AccountRequest& request,
    std::string_view policy_revision,
    const Authorization& authorization);
JournalEntry entry(const OrdersTerminal& record);
JournalEntry entry(const OrderNotSent& record);
JournalEntry entry(const CancelResult& record);

std::string_view status_name(BrokerOrderStatus status) noexcept;

} // namespace asterion::trading
