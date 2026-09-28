#pragma once
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/data.pb.h>
namespace asterion::protocol {
Json decode_csv_snapshot(const data::v1::CsvSnapshot& input);
data::v1::CsvSnapshot encode_csv_snapshot(const Json& input);
// Ordered, exact Decimal trade events; duplicates are retained, never cleaned
// silently.
data::v1::TradeDataset
make_trade_dataset(const v1::Contract& contract,
                   const google::protobuf::RepeatedPtrField<v1::Tick>& ticks);
Json decode_dataset(const data::v1::TradeDataset& dataset);
data::v1::TradeDataset encode_dataset(const Json& dataset);
std::string publication_id(const data::v1::DatasetPublication& publication);
Json decode_publication(const data::v1::DatasetPublication& publication);
data::v1::DatasetPublication encode_publication(const Json& publication);
void encode_settlement_days(const Json& days,
                            google::protobuf::RepeatedPtrField<data::v1::SettlementDay>& result);
Json decode_settlement_days(
    const google::protobuf::RepeatedPtrField<data::v1::SettlementDay>& days);
data::v1::SettlementCalendar
make_settlement_calendar(const v1::Contract&,
                         const google::protobuf::RepeatedPtrField<data::v1::SettlementDay>&);
Json decode_calendar(const data::v1::SettlementCalendar&);
data::v1::SettlementCalendar encode_calendar(const Json&);
data::v1::CalendarCsvSnapshot encode_calendar_snapshot(const Json&);
Json decode_calendar_snapshot(const data::v1::CalendarCsvSnapshot&);
std::string calendar_publication_id(const data::v1::CalendarPublication&);
Json decode_calendar_publication(const data::v1::CalendarPublication&);
data::v1::CalendarPublication encode_calendar_publication(const Json&);
} // namespace asterion::protocol
