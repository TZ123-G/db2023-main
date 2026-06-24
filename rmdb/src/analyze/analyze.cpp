/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"

#include <limits>

#include "common/datetime.h"

namespace {
bool is_integer_type(ColType type) {
    return type == TYPE_INT || type == TYPE_BIGINT;
}

bool is_numeric_type(ColType type) {
    return is_integer_type(type) || type == TYPE_FLOAT;
}

int64_t parse_integer_literal(const std::string &literal) {
    try {
        size_t idx = 0;
        long long value = std::stoll(literal, &idx, 10);
        if (idx != literal.size()) {
            throw NumericOverflowError(literal, "BIGINT");
        }
        return static_cast<int64_t>(value);
    } catch (const std::invalid_argument &) {
        throw NumericOverflowError(literal, "BIGINT");
    } catch (const std::out_of_range &) {
        throw NumericOverflowError(literal, "BIGINT");
    }
}

template <typename T, typename U>
void ensure_numeric_range(U value, const std::string &literal, ColType target_type) {
    if (value < static_cast<U>(std::numeric_limits<T>::lowest()) ||
        value > static_cast<U>(std::numeric_limits<T>::max())) {
        throw NumericOverflowError(literal, coltype2str(target_type));
    }
}

void coerce_value_to_col_type(Value &value, ColType target_type) {
    if (value.type == target_type) {
        return;
    }
    if (target_type == TYPE_DATETIME) {
        if (value.type != TYPE_STRING) {
            throw IncompatibleTypeError(coltype2str(target_type), coltype2str(value.type));
        }
        value.set_datetime(parse_datetime(value.str_val));
        return;
    }
    if (!is_numeric_type(value.type) || !is_numeric_type(target_type)) {
        throw IncompatibleTypeError(coltype2str(target_type), coltype2str(value.type));
    }
    if (target_type == TYPE_FLOAT) {
        if (value.type == TYPE_INT) {
            value.set_float(static_cast<float>(value.int_val));
        } else if (value.type == TYPE_BIGINT) {
            value.set_float(static_cast<float>(value.bigint_val));
        }
        return;
    }
    if (target_type == TYPE_BIGINT) {
        if (value.type == TYPE_INT) {
            value.set_bigint(static_cast<int64_t>(value.int_val));
        } else {
            ensure_numeric_range<int64_t>(value.float_val, std::to_string(value.float_val), target_type);
            value.set_bigint(static_cast<int64_t>(value.float_val));
        }
        return;
    }
    if (target_type == TYPE_INT) {
        if (value.type == TYPE_BIGINT) {
            ensure_numeric_range<int>(value.bigint_val, std::to_string(value.bigint_val), target_type);
            value.set_int(static_cast<int>(value.bigint_val));
        } else {
            ensure_numeric_range<int>(value.float_val, std::to_string(value.float_val), target_type);
            value.set_int(static_cast<int>(value.float_val));
        }
        return;
    }
}

std::string aggregate_name(ast::SvAggType type) {
    switch (type) {
        case ast::SV_AGG_COUNT:
            return "COUNT";
        case ast::SV_AGG_MAX:
            return "MAX";
        case ast::SV_AGG_MIN:
            return "MIN";
        case ast::SV_AGG_SUM:
            return "SUM";
        default:
            throw InternalError("Unexpected aggregate type");
    }
}
}  // namespace

std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse) {
    std::shared_ptr<Query> query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse)) {
        query->tables = x->tabs;
        for (auto &tab_name : query->tables) {
            if (!sm_manager_->db_.is_table(tab_name)) {
                throw TableNotFoundError(tab_name);
            }
        }

        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);

        bool has_aggregate = false;
        bool has_plain_column = false;
        for (const auto &item : x->select_items) {
            has_aggregate = has_aggregate || item->agg_type != ast::SV_AGG_NONE;
            has_plain_column = has_plain_column || item->agg_type == ast::SV_AGG_NONE;
        }
        if (has_aggregate && has_plain_column) {
            throw InvalidAggregateError("aggregate expressions cannot be mixed with ordinary columns");
        }
        if (has_aggregate && x->has_sort) {
            throw InvalidAggregateError("ORDER BY is not supported for aggregate queries");
        }

        if (x->select_items.empty()) {
            for (auto &col : all_cols) {
                query->cols.push_back({.tab_name = col.tab_name, .col_name = col.name});
            }
        } else if (has_aggregate) {
            for (const auto &item : x->select_items) {
                AggregateExpr aggregate;
                aggregate.type = convert_sv_agg_type(item->agg_type);
                aggregate.is_star = item->is_star;
                if (item->is_star) {
                    if (aggregate.type != AGG_COUNT) {
                        throw InvalidAggregateError("only COUNT supports '*'");
                    }
                    aggregate.output_name = item->alias.empty() ? "COUNT(*)" : item->alias;
                } else {
                    aggregate.col = check_column(
                        all_cols, {.tab_name = item->col->tab_name, .col_name = item->col->col_name});
                    const auto col = std::find_if(all_cols.begin(), all_cols.end(), [&](const ColMeta &meta) {
                        return meta.tab_name == aggregate.col.tab_name && meta.name == aggregate.col.col_name;
                    });
                    if (col == all_cols.end()) {
                        throw ColumnNotFoundError(aggregate.col.tab_name + "." + aggregate.col.col_name);
                    }
                    if (aggregate.type == AGG_SUM && !is_numeric_type(col->type)) {
                        throw InvalidAggregateError("SUM only supports INT, BIGINT and FLOAT columns");
                    }
                    if ((aggregate.type == AGG_MAX || aggregate.type == AGG_MIN) &&
                        col->type == TYPE_DATETIME) {
                        throw InvalidAggregateError(aggregate_name(item->agg_type) +
                                                    " does not support DATETIME columns");
                    }
                    std::string argument_name = item->col->tab_name.empty()
                                                    ? item->col->col_name
                                                    : item->col->tab_name + "." + item->col->col_name;
                    aggregate.output_name =
                        item->alias.empty()
                            ? aggregate_name(item->agg_type) + "(" + argument_name + ")"
                            : item->alias;
                }
                query->aggregates.push_back(std::move(aggregate));
            }
        } else {
            for (const auto &item : x->select_items) {
                query->cols.push_back(
                    check_column(all_cols, {.tab_name = item->col->tab_name, .col_name = item->col->col_name}));
            }
        }

        get_clause(x->conds, query->conds);
        check_clause(query->tables, query->conds);
        for (const auto &order : x->orders) {
            query->order_bys.push_back(
                {.col = check_column(all_cols, {.tab_name = order->col->tab_name, .col_name = order->col->col_name}),
                 .is_desc = order->orderby_dir == ast::OrderBy_DESC});
        }
        query->has_limit = x->has_limit;
        query->limit = x->limit;
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        TabMeta &tab = sm_manager_->db_.get_table(x->tab_name);
        for (auto &sv_set_clause : x->set_clauses) {
            SetClause set_clause;
            set_clause.lhs = {.tab_name = x->tab_name, .col_name = sv_set_clause->col_name};
            auto col = tab.get_col(set_clause.lhs.col_name);
            set_clause.rhs = convert_sv_value(sv_set_clause->val);
            coerce_value_to_col_type(set_clause.rhs, col->type);
            set_clause.rhs.init_raw(col->len);
            query->set_clauses.push_back(std::move(set_clause));
        }
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        TabMeta &tab = sm_manager_->db_.get_table(x->tab_name);
        if (x->vals.size() != tab.cols.size()) {
            throw InvalidValueCountError();
        }
        for (size_t i = 0; i < x->vals.size(); ++i) {
            Value value = convert_sv_value(x->vals[i]);
            coerce_value_to_col_type(value, tab.cols[i].type);
            query->values.push_back(std::move(value));
        }
    }
    query->parse = std::move(parse);
    return query;
}

TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = tab_name;
    } else {
        auto pos = std::find_if(all_cols.begin(), all_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == all_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
        }
    }
    return target;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (auto &sel_tab_name : tab_names) {
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.push_back(cond);
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds) {
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    for (auto &cond : conds) {
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) {
            cond.rhs_col = check_column(all_cols, cond.rhs_col);
        }
        TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            coerce_value_to_col_type(cond.rhs_val, lhs_type);
            cond.rhs_val.init_raw(lhs_col->len);
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (lhs_type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
        }
    }
}

Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        int64_t literal = parse_integer_literal(int_lit->val);
        if (literal >= std::numeric_limits<int>::lowest() && literal <= std::numeric_limits<int>::max()) {
            val.set_int(static_cast<int>(literal));
        } else {
            val.set_bigint(literal);
        }
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
    };
    return m.at(op);
}

AggType Analyze::convert_sv_agg_type(ast::SvAggType type) {
    std::map<ast::SvAggType, AggType> m = {
        {ast::SV_AGG_COUNT, AGG_COUNT},
        {ast::SV_AGG_MAX, AGG_MAX},
        {ast::SV_AGG_MIN, AGG_MIN},
        {ast::SV_AGG_SUM, AGG_SUM},
    };
    return m.at(type);
}
