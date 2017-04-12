/*
 * Description: 
 *     History: yang@haipo.me, 2017/04/04, create
 */

# include "ut_mysql.h"
# include "me_trade.h"
# include "me_market.h"
# include "me_update.h"
# include "me_balance.h"

int load_orders(MYSQL *conn, const char *table)
{
    size_t query_limit = 1000;
    uint64_t last_id = 0;
    while (true) {
        sds sql = sdsempty();
        sql = sdscatprintf(sql, "SELECT `id`, `t`, `side`, `create_time`, `update_time`, `user_id`, `market`, "
                "`price`, `amount`, `fee`, `left`, `freeze`, `deal_stock`, `deal_money`, `deal_fee` FROM `%s` "
                "WHERE `id` > %"PRIu64" ORDER BY `id` LIMIT %zu", table, last_id, query_limit);
        log_trace("exec sql: %s", sql);
        int ret = mysql_real_query(conn, sql, sdslen(sql));
        if (ret != 0) {
            log_error("exec sql: %s fail: %d %s", sql, mysql_errno(conn), mysql_error(conn));
            sdsfree(sql);
            return -__LINE__;
        }
        sdsfree(sql);

        MYSQL_RES *result = mysql_store_result(conn);
        size_t num_rows = mysql_num_rows(result);
        for (size_t i = 0; i < num_rows; ++i) {
            MYSQL_ROW row = mysql_fetch_row(result);
            last_id = strtoull(row[0], NULL, 0);
            market_t *market = get_market(row[6]);
            if (market == NULL)
                continue;

            order_t *order = malloc(sizeof(order_t));
            memset(order, 0, sizeof(order_t));
            order->id = strtoull(row[0], NULL, 0);
            order->type = strtoul(row[1], NULL, 0);
            order->side = strtoul(row[2], NULL, 0);
            order->create_time = strtod(row[3], NULL);
            order->update_time = strtod(row[4], NULL);
            order->user_id = strtoul(row[5], NULL, 0);
            order->market = strdup(row[6]);
            order->price = decimal(row[7], market->money_prec);
            order->amount = decimal(row[8], market->stock_prec);
            order->fee = decimal(row[9], market->fee_prec);
            order->left = decimal(row[10], market->stock_prec);
            order->freeze = decimal(row[11], 0);
            order->deal_stock = decimal(row[12], 0);
            order->deal_money = decimal(row[13], 0);
            order->deal_fee = decimal(row[14], 0);

            if (!order->market || !order->price || !order->amount || !order->fee || !order->left ||
                    !order->freeze || !order->deal_stock || !order->deal_money || !order->deal_fee) {
                log_error("get order detail of order id: %"PRIu64" fail", order->id);
                return -__LINE__;
            }

            market_put_order(market, order);
        }
        mysql_free_result(result);

        if (num_rows < query_limit)
            break;
    }

    return 0;
}

int load_markets(MYSQL *conn, const char *table)
{
    sds sql = sdsempty();
    sql = sdscatprintf(sql, "SELECT `market`, `id_start` from `%s`", table);
    log_trace("exec sql: %s", sql);
    int ret = mysql_real_query(conn, sql, sdslen(sql));
    if (ret != 0) {
        log_error("exec sql: %s fail: %d %s", sql, mysql_errno(conn), mysql_error(conn));
        sdsfree(sql);
        return -__LINE__;
    }
    sdsfree(sql);

    MYSQL_RES *result = mysql_store_result(conn);
    size_t num_rows = mysql_num_rows(result);
    for (size_t i = 0; i < num_rows; ++i) {
        MYSQL_ROW row = mysql_fetch_row(result);
        market_t *market = get_market(row[0]);
        if (market == NULL)
            continue;
        market->id_start = strtoull(row[1], NULL, 0);
    }
    mysql_free_result(result);

    return 0;
}

int load_balance(MYSQL *conn, const char *table)
{
    size_t query_limit = 1000;
    uint64_t last_id = 0;
    while (true) {
        sds sql = sdsempty();
        sql = sdscatprintf(sql, "SELECT `id`, `user_id`, `asset`, `t`, `balance` FROM `%s` "
                "WHERE `id` > %"PRIu64" ORDER BY id LIMIT %zu", table, last_id, query_limit);
        log_trace("exec sql: %s", sql);
        int ret = mysql_real_query(conn, sql, sdslen(sql));
        if (ret != 0) {
            log_error("exec sql: %s fail: %d %s", sql, mysql_errno(conn), mysql_error(conn));
            sdsfree(sql);
            return -__LINE__;
        }
        sdsfree(sql);

        MYSQL_RES *result = mysql_store_result(conn);
        size_t num_rows = mysql_num_rows(result);
        for (size_t i = 0; i < num_rows; ++i) {
            MYSQL_ROW row = mysql_fetch_row(result);
            last_id = strtoull(row[0], NULL, 0);
            uint32_t user_id = strtoul(row[1], NULL, 0);
            const char *asset = row[2];
            if (!asset_exist(asset)) {
                continue;
            }
            uint32_t type = strtoul(row[3], NULL, 0);
            mpd_t *balance = decimal(row[4], asset_prec(asset));
            balance_set(user_id, type, asset, balance);
        }
        mysql_free_result(result);

        if (num_rows < query_limit)
            break;
    }

    return 0;
}

static int load_update_balance(json_t *params)
{
    if (json_array_size(params) != 6)
        return -__LINE__;

    // user_id
    if (!json_is_integer(json_array_get(params, 0)))
        return -__LINE__;
    uint32_t user_id = json_integer_value(json_array_get(params, 0));

    // type
    if (!json_is_integer(json_array_get(params, 1)))
        return -__LINE__;
    uint32_t type = json_integer_value(json_array_get(params, 1));
    if (type != BALANCE_TYPE_AVAILABLE && type != BALANCE_TYPE_FREEZE)
        return -__LINE__;

    // asset
    if (!json_is_string(json_array_get(params, 2)))
        return -__LINE__;
    const char *asset = json_string_value(json_array_get(params, 2));
    int prec = asset_prec(asset);
    if (prec < 0)
        return 0;

    // business
    if (!json_is_string(json_array_get(params, 3)))
        return -__LINE__;
    const char *business = json_string_value(json_array_get(params, 3));

    // business_id
    if (!json_is_integer(json_array_get(params, 4)))
        return -__LINE__;
    uint64_t business_id = json_integer_value(json_array_get(params, 4));

    // change
    if (!json_is_string(json_array_get(params, 5)))
        return -__LINE__;
    mpd_t *change = decimal(json_string_value(json_array_get(params, 5)), prec);
    if (change == NULL)
        return -__LINE__;

    int ret = update_user_balance(false, user_id, type, asset, business, business_id, change);
    mpd_del(change);

    if (ret < 0) {
        return -__LINE__;
    }

    return 0;
}

static int load_limit_order(json_t *params)
{
    if (json_array_size(params) != 6)
        return -__LINE__;

    // user_id
    if (!json_is_integer(json_array_get(params, 0)))
        return -__LINE__;
    uint32_t user_id = json_integer_value(json_array_get(params, 0));

    // market
    if (!json_is_string(json_array_get(params, 1)))
        return -__LINE__;
    const char *market_name = json_string_value(json_array_get(params, 1));
    market_t *market = get_market(market_name);
    if (market == NULL)
        return 0;

    // side
    if (!json_is_integer(json_array_get(params, 2)))
        return -__LINE__;
    uint32_t side = json_integer_value(json_array_get(params, 2));
    if (side != MARKET_ORDER_SIDE_ASK && side != MARKET_ORDER_SIDE_BID)
        return -__LINE__;

    // amount
    if (!json_is_string(json_array_get(params, 3)))
        return -__LINE__;
    mpd_t *amount = decimal(json_string_value(json_array_get(params, 3)), market->stock_prec);
    if (amount == NULL)
        return -__LINE__;
    if (mpd_cmp(amount, mpd_zero, &mpd_ctx) <= 0) {
        mpd_del(amount);
        return -__LINE__;
    }

    // price 
    if (!json_is_string(json_array_get(params, 4)))
        return -__LINE__;
    mpd_t *price = decimal(json_string_value(json_array_get(params, 4)), market->money_prec);
    if (price == NULL) {
        mpd_del(amount);
        return -__LINE__;
    }
    if (mpd_cmp(price, mpd_zero, &mpd_ctx) <= 0) {
        mpd_del(amount);
        mpd_del(price);
        return -__LINE__;
    }

    // fee
    if (!json_is_string(json_array_get(params, 5)))
        return -__LINE__;
    mpd_t *fee = decimal(json_string_value(json_array_get(params, 5)), market->fee_prec);
    if (fee == NULL) {
        mpd_del(amount);
        mpd_del(price);
        return -__LINE__;
    }
    if (mpd_cmp(fee, mpd_zero, &mpd_ctx) < 0 || mpd_cmp(fee, mpd_one, &mpd_ctx) >= 0) {
        mpd_del(amount);
        mpd_del(price);
        mpd_del(fee);
        return -__LINE__;
    }

    int ret = market_put_limit_order(false, market, user_id, side, amount, price, fee);
    mpd_del(amount);
    mpd_del(price);
    mpd_del(fee);

    if (ret < 0) {
        return -__LINE__;
    }

    return 0;
}

static int load_market_order(json_t *params)
{
    if (json_array_size(params) != 5)
        return -__LINE__;

    // user_id
    if (!json_is_integer(json_array_get(params, 0)))
        return -__LINE__;
    uint32_t user_id = json_integer_value(json_array_get(params, 0));

    // market
    if (!json_is_string(json_array_get(params, 1)))
        return -__LINE__;
    const char *market_name = json_string_value(json_array_get(params, 1));
    market_t *market = get_market(market_name);
    if (market == NULL)
        return 0;

    // side
    if (!json_is_integer(json_array_get(params, 2)))
        return -__LINE__;
    uint32_t side = json_integer_value(json_array_get(params, 2));
    if (side != MARKET_ORDER_SIDE_ASK && side != MARKET_ORDER_SIDE_BID)
        return -__LINE__;

    // amount
    if (!json_is_string(json_array_get(params, 3)))
        return -__LINE__;
    mpd_t *amount = decimal(json_string_value(json_array_get(params, 3)), market->stock_prec);
    if (amount == NULL)
        return -__LINE__;
    if (mpd_cmp(amount, mpd_zero, &mpd_ctx) <= 0) {
        mpd_del(amount);
        return -__LINE__;
    }

    // fee
    if (!json_is_string(json_array_get(params, 4)))
        return -__LINE__;
    mpd_t *fee = decimal(json_string_value(json_array_get(params, 4)), market->fee_prec);
    if (fee == NULL) {
        mpd_del(amount);
        return -__LINE__;
    }
    if (mpd_cmp(fee, mpd_zero, &mpd_ctx) < 0 || mpd_cmp(fee, mpd_one, &mpd_ctx) >= 0) {
        mpd_del(amount);
        mpd_del(fee);
        return -__LINE__;
    }

    int ret = market_put_market_order(false, market, user_id, side, amount, fee);
    mpd_del(amount);
    mpd_del(fee);

    if (ret < 0) {
        log_error("market_put_market_order market: %s, user: %u, side: %u, amount: %s, fee: %s fail: %d",
                market_name, user_id, side, mpd_to_sci(amount, 0), mpd_to_sci(fee, 0), ret);
        return -__LINE__;
    }

    return 0;
}

static int load_cancel_order(json_t *params)
{
    if (json_array_size(params) != 3)
        return -__LINE__;

    // user_id
    if (!json_is_integer(json_array_get(params, 0)))
        return -__LINE__;
    uint32_t user_id = json_integer_value(json_array_get(params, 0));

    // market
    if (!json_is_string(json_array_get(params, 1)))
        return -__LINE__;
    const char *market_name = json_string_value(json_array_get(params, 1));
    market_t *market = get_market(market_name);
    if (market == NULL)
        return 0;

    // order_id
    if (!json_is_integer(json_array_get(params, 2)))
        return -__LINE__;
    uint64_t order_id = json_integer_value(json_array_get(params, 2));

    order_t *order = market_get_order(market, order_id);
    if (order == NULL) {
        return -__LINE__;
    }

    int ret = market_cancel_order(false, market, order);
    if (ret < 0) {
        log_error("market_cancel_order id: %"PRIu64", user id: %u, market: %s", order_id, user_id, market_name);
        return -__LINE__;
    }

    return 0;
}

static int load_oper(json_t *detail)
{
    const char *method = json_string_value(json_object_get(detail, "method"));
    if (method == NULL)
        return -__LINE__;
    json_t *params = json_object_get(detail, "params");
    if (params == NULL || !json_is_array(params))
        return -__LINE__;

    int ret = 0;
    if (strcmp(method, "update_balance") == 0) {
        ret = load_update_balance(params);
    } else if (strcmp(method, "limit_order") == 0) {
        ret = load_limit_order(params);
    } else if (strcmp(method, "market_order") == 0) {
        ret = load_market_order(params);
    } else if (strcmp(method, "cancel_order") == 0) {
        ret = load_cancel_order(params);
    } else {
        return -__LINE__;
    }

    return ret;
}

int load_operlog(MYSQL *conn, const char *table)
{
    size_t query_limit = 1000;
    uint64_t last_id = 0;
    while (true) {
        sds sql = sdsempty();
        sql = sdscatprintf(sql, "SELECT `id`, `detail` from `%s` WHERE `id` > %"PRIu64" ORDER BY `id` LIMIT %zu",
                table, last_id, query_limit);
        log_trace("exec sql: %s", sql);
        int ret = mysql_real_query(conn, sql, sdslen(sql));
        if (ret != 0) {
            log_error("exec sql: %s fail: %d %s", sql, mysql_errno(conn), mysql_error(conn));
            sdsfree(sql);
            return -__LINE__;
        }
        sdsfree(sql);

        MYSQL_RES *result = mysql_store_result(conn);
        size_t num_rows = mysql_num_rows(result);
        for (size_t i = 0; i < num_rows; ++i) {
            MYSQL_ROW row = mysql_fetch_row(result);
            last_id = strtoull(row[0], NULL, 0);
            json_t *detail = json_loadb(row[1], strlen(row[1]), 0, NULL);
            if (detail == NULL) {
                log_error("invalid detail data: %s", row[1]);
                mysql_free_result(result);
                return -__LINE__;
            }
            ret = load_oper(detail);
            if (ret < 0) {
                json_decref(detail);
                log_error("load_oper: %s fail: %d", row[1], ret);
                mysql_free_result(result);
                return -__LINE__;
            }
            json_decref(detail);
        }
        mysql_free_result(result);

        if (num_rows < query_limit)
            break;
    }

    return 0;
}
