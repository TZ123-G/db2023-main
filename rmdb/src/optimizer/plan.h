/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/common.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "system/sm.h"

typedef enum PlanTag {
    T_Invalid = 1,
    T_Help,
    T_ShowTable,
    T_ShowIndex,
    T_DescTable,
    T_CreateTable,
    T_DropTable,
    T_CreateIndex,
    T_DropIndex,
    T_Insert,
    T_Update,
    T_Delete,
    T_select,
    T_Transaction_begin,
    T_Transaction_commit,
    T_Transaction_abort,
    T_Transaction_rollback,
    T_SeqScan,
    T_IndexScan,
    T_NestLoop,
    T_HashJoin,
    T_Sort,
    T_Projection,
    T_Aggregate
} PlanTag;

class Plan {
   public:
    PlanTag tag;
    virtual ~Plan() = default;
};

class ScanPlan : public Plan {
   public:
    ScanPlan(PlanTag tag, SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
             std::vector<std::string> index_col_names) {
        Plan::tag = tag;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager->db_.get_table(tab_name_);
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;
        fed_conds_ = conds_;
        index_col_names_ = std::move(index_col_names);
    }
    ~ScanPlan() {}

    std::string tab_name_;
    std::vector<ColMeta> cols_;
    std::vector<Condition> conds_;
    size_t len_;
    std::vector<Condition> fed_conds_;
    std::vector<std::string> index_col_names_;
};

class JoinPlan : public Plan {
   public:
    JoinPlan(PlanTag tag, std::shared_ptr<Plan> left, std::shared_ptr<Plan> right, std::vector<Condition> conds,
             bool build_left = false, bool buffer_left = false)
        : left_(std::move(left)),
          right_(std::move(right)),
          conds_(std::move(conds)),
          build_left_(build_left),
          buffer_left_(buffer_left) {
        Plan::tag = tag;
    }
    ~JoinPlan() {}

    std::shared_ptr<Plan> left_;
    std::shared_ptr<Plan> right_;
    std::vector<Condition> conds_;
    bool build_left_;
    bool buffer_left_;
};

class DMLPlan : public Plan {
   public:
    DMLPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::string tab_name, std::vector<Value> values,
            std::vector<Condition> conds, std::vector<SetClause> set_clauses)
        : subplan_(std::move(subplan)),
          tab_name_(std::move(tab_name)),
          values_(std::move(values)),
          conds_(std::move(conds)),
          set_clauses_(std::move(set_clauses)) {
        Plan::tag = tag;
    }
    ~DMLPlan() {}

    std::shared_ptr<Plan> subplan_;
    std::string tab_name_;
    std::vector<Value> values_;
    std::vector<Condition> conds_;
    std::vector<SetClause> set_clauses_;
};

class DDLPlan : public Plan {
   public:
    DDLPlan(PlanTag tag, std::string tab_name, std::vector<std::string> tab_col_names,
            std::vector<ColDef> col_defs)
        : tab_name_(std::move(tab_name)),
          tab_col_names_(std::move(tab_col_names)),
          col_defs_(std::move(col_defs)) {
        Plan::tag = tag;
    }
    ~DDLPlan() {}

    std::string tab_name_;
    std::vector<std::string> tab_col_names_;
    std::vector<ColDef> col_defs_;
};

class OtherPlan : public Plan {
   public:
    OtherPlan(PlanTag tag) { Plan::tag = tag; }
    OtherPlan(PlanTag tag, std::string tab_name) : tab_name_(std::move(tab_name)) { Plan::tag = tag; }
    ~OtherPlan() {}

    std::string tab_name_;
};

class ProjectionPlan : public Plan {
   public:
    ProjectionPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<TabCol> sel_cols)
        : subplan_(std::move(subplan)), sel_cols_(std::move(sel_cols)) {
        Plan::tag = tag;
    }
    ~ProjectionPlan() {}

    std::shared_ptr<Plan> subplan_;
    std::vector<TabCol> sel_cols_;
};

class AggregatePlan : public Plan {
   public:
    AggregatePlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<AggregateExpr> aggregates)
        : subplan_(std::move(subplan)), aggregates_(std::move(aggregates)) {
        Plan::tag = tag;
    }
    ~AggregatePlan() {}

    std::shared_ptr<Plan> subplan_;
    std::vector<AggregateExpr> aggregates_;
};

class SortPlan : public Plan {
   public:
    SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<OrderByClause> order_bys, bool has_limit,
             size_t limit)
        : subplan_(std::move(subplan)),
          order_bys_(std::move(order_bys)),
          has_limit_(has_limit),
          limit_(limit) {
        Plan::tag = tag;
    }
    ~SortPlan() {}

    std::shared_ptr<Plan> subplan_;
    std::vector<OrderByClause> order_bys_;
    bool has_limit_;
    size_t limit_;
};
