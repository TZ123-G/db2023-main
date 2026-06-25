/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "planner.h"

#include <limits>
#include <memory>

#include "execution/executor_delete.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "index/ix.h"
#include "record_printer.h"
#include "predicate_inference.h"

bool Planner::get_index_cols(std::string tab_name, std::vector<Condition> curr_conds,
                             std::vector<std::string> &index_col_names) {
    index_col_names.clear();
    TabMeta &tab = sm_manager_->db_.get_table(tab_name);
    size_t best_prefix = 0;
    const IndexMeta *best_index = nullptr;
    for (const auto &index : tab.indexes) {
        size_t prefix = 0;
        for (const auto &col : index.cols) {
            bool has_equal = false;
            bool has_range = false;
            for (const auto &cond : curr_conds) {
                if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name ||
                    cond.lhs_col.col_name != col.name) {
                    continue;
                }
                has_equal = has_equal || cond.op == OP_EQ;
                has_range = has_range || cond.op == OP_LT || cond.op == OP_LE ||
                            cond.op == OP_GT || cond.op == OP_GE;
            }
            if (has_equal) {
                ++prefix;
                continue;
            }
            if (has_range) {
                ++prefix;
            }
            break;
        }
        if (prefix > best_prefix) {
            best_prefix = prefix;
            best_index = &index;
        }
    }
    if (best_index == nullptr) {
        return false;
    }
    for (const auto &col : best_index->cols) {
        index_col_names.push_back(col.name);
    }
    return true;
}

std::vector<Condition> pop_conds(std::vector<Condition> &conds, std::string tab_name) {
    std::vector<Condition> solved_conds;
    auto it = conds.begin();
    while (it != conds.end()) {
        if ((tab_name.compare(it->lhs_col.tab_name) == 0 && it->is_rhs_val) ||
            (it->lhs_col.tab_name.compare(it->rhs_col.tab_name) == 0)) {
            solved_conds.emplace_back(std::move(*it));
            it = conds.erase(it);
        } else {
            ++it;
        }
    }
    return solved_conds;
}

int push_conds(Condition *cond, std::shared_ptr<Plan> plan) {
    if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        if (x->tab_name_.compare(cond->lhs_col.tab_name) == 0) {
            return 1;
        } else if (x->tab_name_.compare(cond->rhs_col.tab_name) == 0) {
            return 2;
        } else {
            return 0;
        }
    } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        int left_res = push_conds(cond, x->left_);
        if (left_res == 3) {
            return 3;
        }
        int right_res = push_conds(cond, x->right_);
        if (right_res == 3) {
            return 3;
        }
        if (left_res == 0 || right_res == 0) {
            return left_res + right_res;
        }
        if (left_res == 2) {
            std::map<CompOp, CompOp> swap_op = {
                {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
            };
            std::swap(cond->lhs_col, cond->rhs_col);
            cond->op = swap_op.at(cond->op);
        }
        x->conds_.emplace_back(std::move(*cond));
        return 3;
    }
    return false;
}

std::shared_ptr<Plan> pop_scan(int *scantbl, std::string table, std::vector<std::string> &joined_tables,
                               std::vector<std::shared_ptr<Plan>> plans) {
    for (size_t i = 0; i < plans.size(); i++) {
        auto x = std::dynamic_pointer_cast<ScanPlan>(plans[i]);
        if (x->tab_name_.compare(table) == 0) {
            scantbl[i] = 1;
            joined_tables.emplace_back(x->tab_name_);
            return plans[i];
        }
    }
    return nullptr;
}

std::shared_ptr<Query> Planner::logical_optimization(std::shared_ptr<Query> query, Context *context) {
    query->conds = predicate_inference::infer(query->conds);
    return query;
}

bool Planner::plan_contains_column(const std::shared_ptr<Plan> &plan, const TabCol &column) const {
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        return scan->tab_name_ == column.tab_name &&
               std::any_of(scan->cols_.begin(), scan->cols_.end(),
                           [&](const ColMeta &col) { return col.name == column.col_name; });
    }
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        return plan_contains_column(join->left_, column) || plan_contains_column(join->right_, column);
    }
    return false;
}

size_t Planner::estimate_plan_rows(const std::shared_ptr<Plan> &plan) const {
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        auto file = sm_manager_->fhs_.find(scan->tab_name_);
        if (file == sm_manager_->fhs_.end()) {
            return std::numeric_limits<size_t>::max();
        }
        const auto header = file->second->get_file_hdr();
        size_t data_pages = header.num_pages > RM_FIRST_RECORD_PAGE
                                ? static_cast<size_t>(header.num_pages - RM_FIRST_RECORD_PAGE)
                                : 0;
        return data_pages * static_cast<size_t>(header.num_records_per_page);
    }
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        size_t left = estimate_plan_rows(join->left_);
        size_t right = estimate_plan_rows(join->right_);
        if (left == 0 || right == 0) {
            return 0;
        }
        if (left > std::numeric_limits<size_t>::max() / right) {
            return std::numeric_limits<size_t>::max();
        }
        return left * right;
    }
    return std::numeric_limits<size_t>::max();
}

std::shared_ptr<Plan> Planner::make_join_plan(std::shared_ptr<Plan> left, std::shared_ptr<Plan> right,
                                              std::vector<Condition> conds) {
    bool has_hash_key = std::any_of(conds.begin(), conds.end(), [&](const Condition &cond) {
        if (cond.is_rhs_val || cond.op != OP_EQ) {
            return false;
        }
        bool lhs_left = plan_contains_column(left, cond.lhs_col);
        bool lhs_right = plan_contains_column(right, cond.lhs_col);
        bool rhs_left = plan_contains_column(left, cond.rhs_col);
        bool rhs_right = plan_contains_column(right, cond.rhs_col);
        return (lhs_left && rhs_right) || (lhs_right && rhs_left);
    });
    bool build_left = has_hash_key && estimate_plan_rows(left) <= estimate_plan_rows(right);
    bool buffer_left = !has_hash_key && estimate_plan_rows(left) <= estimate_plan_rows(right);
    return std::make_shared<JoinPlan>(has_hash_key ? T_HashJoin : T_NestLoop, std::move(left),
                                      std::move(right), std::move(conds), build_left, buffer_left);
}

void Planner::select_hash_joins(const std::shared_ptr<Plan> &plan) {
    auto join = std::dynamic_pointer_cast<JoinPlan>(plan);
    if (join == nullptr) {
        return;
    }
    select_hash_joins(join->left_);
    select_hash_joins(join->right_);
    bool has_hash_key = std::any_of(join->conds_.begin(), join->conds_.end(), [&](const Condition &cond) {
        if (cond.is_rhs_val || cond.op != OP_EQ) {
            return false;
        }
        return (plan_contains_column(join->left_, cond.lhs_col) &&
                plan_contains_column(join->right_, cond.rhs_col)) ||
               (plan_contains_column(join->right_, cond.lhs_col) &&
                plan_contains_column(join->left_, cond.rhs_col));
    });
    join->tag = has_hash_key ? T_HashJoin : T_NestLoop;
    join->build_left_ = has_hash_key && estimate_plan_rows(join->left_) <= estimate_plan_rows(join->right_);
    join->buffer_left_ = !has_hash_key && estimate_plan_rows(join->left_) <= estimate_plan_rows(join->right_);
}

std::shared_ptr<Plan> Planner::physical_optimization(std::shared_ptr<Query> query, Context *context) {
    std::shared_ptr<Plan> plan = make_one_rel(query);
    if (!query->order_bys.empty() || query->has_limit) {
        plan = generate_sort_plan(query, std::move(plan));
    }
    return plan;
}

std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query) {
    std::vector<std::string> tables = query->tables;
    std::vector<std::shared_ptr<Plan>> table_scan_executors(tables.size());
    for (size_t i = 0; i < tables.size(); i++) {
        auto curr_conds = pop_conds(query->conds, tables[i]);
        std::vector<std::string> index_cols;
        PlanTag scan_tag = get_index_cols(tables[i], curr_conds, index_cols) ? T_IndexScan : T_SeqScan;
        table_scan_executors[i] =
            std::make_shared<ScanPlan>(scan_tag, sm_manager_, tables[i], curr_conds, std::move(index_cols));
    }
    if (tables.size() == 1) {
        return table_scan_executors[0];
    }

    auto conds = std::move(query->conds);
    std::shared_ptr<Plan> table_join_executors;
    int scantbl[tables.size()];
    for (size_t i = 0; i < tables.size(); i++) {
        scantbl[i] = -1;
    }

    if (conds.size() >= 1) {
        std::vector<std::string> joined_tables;
        auto it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            std::shared_ptr<Plan> right = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
            std::vector<Condition> join_conds{*it};
            table_join_executors = make_join_plan(std::move(left), std::move(right), std::move(join_conds));
            it = conds.erase(it);
            break;
        }

        it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left_need_to_join_executors = nullptr;
            std::shared_ptr<Plan> right_need_to_join_executors = nullptr;
            bool isneedreverse = false;
            if (std::find(joined_tables.begin(), joined_tables.end(), it->lhs_col.tab_name) == joined_tables.end()) {
                left_need_to_join_executors =
                    pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            }
            if (std::find(joined_tables.begin(), joined_tables.end(), it->rhs_col.tab_name) == joined_tables.end()) {
                right_need_to_join_executors =
                    pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
                isneedreverse = true;
            }

            if (left_need_to_join_executors != nullptr && right_need_to_join_executors != nullptr) {
                std::vector<Condition> join_conds{*it};
                std::shared_ptr<Plan> temp_join_executors =
                    make_join_plan(std::move(left_need_to_join_executors),
                                   std::move(right_need_to_join_executors), std::move(join_conds));
                table_join_executors = make_join_plan(
                    std::move(temp_join_executors), std::move(table_join_executors), std::vector<Condition>());
            } else if (left_need_to_join_executors != nullptr || right_need_to_join_executors != nullptr) {
                if (isneedreverse) {
                    std::map<CompOp, CompOp> swap_op = {
                        {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
                    };
                    std::swap(it->lhs_col, it->rhs_col);
                    it->op = swap_op.at(it->op);
                    left_need_to_join_executors = std::move(right_need_to_join_executors);
                }
                std::vector<Condition> join_conds{*it};
                table_join_executors =
                    make_join_plan(std::move(left_need_to_join_executors),
                                   std::move(table_join_executors), std::move(join_conds));
            } else {
                push_conds(std::move(&(*it)), table_join_executors);
            }
            it = conds.erase(it);
        }
    } else {
        table_join_executors = table_scan_executors[0];
        scantbl[0] = 1;
    }

    for (size_t i = 0; i < tables.size(); i++) {
        if (scantbl[i] == -1) {
            table_join_executors =
                make_join_plan(std::move(table_join_executors), std::move(table_scan_executors[i]),
                               std::vector<Condition>());
        }
    }

    select_hash_joins(table_join_executors);
    return table_join_executors;
}

std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan) {
    return std::make_shared<SortPlan>(T_Sort, std::move(plan), query->order_bys, query->has_limit, query->limit);
}

std::shared_ptr<Plan> Planner::generate_select_plan(std::shared_ptr<Query> query, Context *context) {
    query = logical_optimization(std::move(query), context);

    auto sel_cols = query->cols;
    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, context);
    if (!query->aggregates.empty()) {
        plannerRoot =
            std::make_shared<AggregatePlan>(T_Aggregate, std::move(plannerRoot), std::move(query->aggregates));
    } else {
        plannerRoot = std::make_shared<ProjectionPlan>(T_Projection, std::move(plannerRoot), std::move(sel_cols));
    }

    return plannerRoot;
}

std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query, Context *context) {
    std::shared_ptr<Plan> plannerRoot;
    if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
        std::vector<ColDef> col_defs;
        for (auto &field : x->fields) {
            if (auto sv_col_def = std::dynamic_pointer_cast<ast::ColDef>(field)) {
                ColDef col_def = {.name = sv_col_def->col_name,
                                  .type = interp_sv_type(sv_col_def->type_len->type),
                                  .len = sv_col_def->type_len->len};
                col_defs.push_back(col_def);
            } else {
                throw InternalError("Unexpected field type");
            }
        }
        plannerRoot = std::make_shared<DDLPlan>(T_CreateTable, x->tab_name, std::vector<std::string>(), col_defs);
    } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(query->parse)) {
        plannerRoot =
            std::make_shared<DDLPlan>(T_DropTable, x->tab_name, std::vector<std::string>(), std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
        plannerRoot = std::make_shared<DDLPlan>(T_CreateIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
        plannerRoot = std::make_shared<DDLPlan>(T_DropIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
        plannerRoot = std::make_shared<DMLPlan>(T_Insert, std::shared_ptr<Plan>(), x->tab_name, query->values,
                                                std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
        std::vector<std::string> index_cols;
        PlanTag scan_tag = get_index_cols(x->tab_name, query->conds, index_cols) ? T_IndexScan : T_SeqScan;
        std::shared_ptr<Plan> table_scan_executors =
            std::make_shared<ScanPlan>(scan_tag, sm_manager_, x->tab_name, query->conds, std::move(index_cols));

        plannerRoot = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, x->tab_name, std::vector<Value>(),
                                                query->conds, std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        std::vector<std::string> index_cols;
        PlanTag scan_tag = get_index_cols(x->tab_name, query->conds, index_cols) ? T_IndexScan : T_SeqScan;
        std::shared_ptr<Plan> table_scan_executors =
            std::make_shared<ScanPlan>(scan_tag, sm_manager_, x->tab_name, query->conds, std::move(index_cols));
        plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors, x->tab_name, std::vector<Value>(),
                                                query->conds, query->set_clauses);
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {
        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);
        std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);
        plannerRoot = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                std::vector<Condition>(), std::vector<SetClause>());
    } else {
        throw InternalError("Unexpected AST root");
    }
    return plannerRoot;
}
