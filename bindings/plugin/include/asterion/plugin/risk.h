#ifndef ASTERION_PLUGIN_RISK_H
#define ASTERION_PLUGIN_RISK_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define AST_RISK_V1 "asterion.risk.pre-trade.v1"
/* Futures only. Decimal fields use signed int64 units scaled by 10^8.
 * The host supplies a consistent account snapshot, serialized with submission.
 * A successful call must explicitly write a valid decision. Non-OK is never permission. */
enum {
  AST_RISK_ALLOW = 0,
  AST_RISK_UNAVAILABLE = 1,
  AST_RISK_INVALID_CONTEXT = 2,
  AST_RISK_ORDER_QUANTITY = 3,
  AST_RISK_GROSS_QUANTITY = 4,
  AST_RISK_WORKING_ORDERS = 5
};
enum { AST_RISK_BUY = 0, AST_RISK_SELL = 1 };
enum {
  AST_RISK_OPEN = 0,
  AST_RISK_CLOSE_TODAY = 1,
  AST_RISK_CLOSE_YESTERDAY = 2,
  AST_RISK_CLOSE = 3
};
typedef struct {
  const char *venue, *symbol, *quote_currency;
  int64_t price_increment, quantity_increment, multiplier;
  const char *order_id, *order_venue, *order_symbol;
  uint32_t side, offset;
  int64_t quantity, limit_price, gross_position_quantity, pending_open_quantity;
  uint64_t working_orders;
} AstRiskContext;
typedef struct {
  uint32_t size, version;
  AstStatus (*evaluate)(void*, const AstRiskContext*, uint32_t* decision);
} AstRiskV1;
#ifdef __cplusplus
}
#endif
#endif
