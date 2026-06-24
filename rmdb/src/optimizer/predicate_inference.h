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

#include <algorithm>
#include <array>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/common.h"

namespace predicate_inference {

struct DisjointSet {
    explicit DisjointSet(size_t size) : parent(size), rank(size, 0) {
        for (size_t i = 0; i < size; ++i) {
            parent[i] = i;
        }
    }

    size_t find(size_t x) {
        if (parent[x] != x) {
            parent[x] = find(parent[x]);
        }
        return parent[x];
    }

    void unite(size_t lhs, size_t rhs) {
        lhs = find(lhs);
        rhs = find(rhs);
        if (lhs == rhs) {
            return;
        }
        if (rank[lhs] < rank[rhs]) {
            std::swap(lhs, rhs);
        }
        parent[rhs] = lhs;
        if (rank[lhs] == rank[rhs]) {
            ++rank[lhs];
        }
    }

    std::vector<size_t> parent;
    std::vector<size_t> rank;
};

inline std::string value_key(const Value &value) {
    std::string key(reinterpret_cast<const char *>(&value.type), sizeof(value.type));
    if (value.raw != nullptr) {
        key.append(reinterpret_cast<const char *>(&value.raw->size), sizeof(value.raw->size));
        key.append(value.raw->data, value.raw->size);
    }
    return key;
}

inline std::string condition_key(const Condition &cond) {
    std::string key = cond.lhs_col.tab_name + '\0' + cond.lhs_col.col_name;
    key.append(reinterpret_cast<const char *>(&cond.op), sizeof(cond.op));
    key.push_back(cond.is_rhs_val ? '\1' : '\0');
    if (cond.is_rhs_val) {
        key += value_key(cond.rhs_val);
    } else {
        key += cond.rhs_col.tab_name + '\0' + cond.rhs_col.col_name;
    }
    return key;
}

inline std::vector<Condition> infer(const std::vector<Condition> &conditions) {
    std::map<TabCol, size_t> column_ids;
    std::vector<TabCol> columns;
    auto add_column = [&](const TabCol &column) {
        auto [it, inserted] = column_ids.emplace(column, columns.size());
        if (inserted) {
            columns.push_back(column);
        }
        return it->second;
    };

    for (const auto &condition : conditions) {
        add_column(condition.lhs_col);
        if (!condition.is_rhs_val) {
            add_column(condition.rhs_col);
        }
    }
    if (columns.empty()) {
        return conditions;
    }

    DisjointSet dsu(columns.size());
    for (const auto &condition : conditions) {
        if (!condition.is_rhs_val && condition.op == OP_EQ) {
            dsu.unite(column_ids.at(condition.lhs_col), column_ids.at(condition.rhs_col));
        }
    }

    std::map<size_t, size_t> class_ids;
    std::vector<std::vector<TabCol>> members;
    for (size_t i = 0; i < columns.size(); ++i) {
        size_t root = dsu.find(i);
        auto [it, inserted] = class_ids.emplace(root, members.size());
        if (inserted) {
            members.emplace_back();
        }
        members[it->second].push_back(columns[i]);
    }

    struct Edge {
        size_t to;
        bool strict;
    };
    std::vector<std::vector<Edge>> forward(members.size());
    std::vector<std::vector<Edge>> reverse(members.size());
    auto class_of = [&](const TabCol &column) {
        return class_ids.at(dsu.find(column_ids.at(column)));
    };
    auto add_edge = [&](size_t from, size_t to, bool strict) {
        if (from == to) {
            return;
        }
        auto duplicate = [&](const Edge &edge) { return edge.to == to && edge.strict == strict; };
        if (std::find_if(forward[from].begin(), forward[from].end(), duplicate) == forward[from].end()) {
            forward[from].push_back({to, strict});
            reverse[to].push_back({from, strict});
        }
    };

    for (const auto &condition : conditions) {
        if (condition.is_rhs_val || condition.op == OP_EQ || condition.op == OP_NE) {
            continue;
        }
        size_t lhs = class_of(condition.lhs_col);
        size_t rhs = class_of(condition.rhs_col);
        switch (condition.op) {
            case OP_LT:
                add_edge(lhs, rhs, true);
                break;
            case OP_LE:
                add_edge(lhs, rhs, false);
                break;
            case OP_GT:
                add_edge(rhs, lhs, true);
                break;
            case OP_GE:
                add_edge(rhs, lhs, false);
                break;
            default:
                break;
        }
    }

    std::vector<Condition> result;
    std::set<std::string> seen;
    for (const auto &condition : conditions) {
        if (seen.insert(condition_key(condition)).second) {
            result.push_back(condition);
        }
    }
    auto emit = [&](size_t class_id, CompOp op, const Value &value) {
        for (const auto &column : members[class_id]) {
            Condition inferred{
                .lhs_col = column,
                .op = op,
                .is_rhs_val = true,
                .rhs_col = {},
                .rhs_val = value,
            };
            if (seen.insert(condition_key(inferred)).second) {
                result.push_back(std::move(inferred));
            }
        }
    };

    auto propagate = [&](size_t start, const Value &value, const std::vector<std::vector<Edge>> &graph,
                         CompOp non_strict_op, CompOp strict_op, bool initially_strict, bool emit_start = true) {
        std::queue<std::pair<size_t, bool>> work;
        std::vector<std::array<bool, 2>> visited(
            members.size(), std::array<bool, 2>{false, false});
        work.push({start, initially_strict});
        while (!work.empty()) {
            auto [node, strict] = work.front();
            work.pop();
            if (visited[node][strict]) {
                continue;
            }
            visited[node][strict] = true;
            if (emit_start || node != start || strict != initially_strict) {
                emit(node, strict ? strict_op : non_strict_op, value);
            }
            for (const auto &edge : graph[node]) {
                work.push({edge.to, strict || edge.strict});
            }
        }
    };

    for (const auto &condition : conditions) {
        if (!condition.is_rhs_val) {
            continue;
        }
        size_t start = class_of(condition.lhs_col);
        switch (condition.op) {
            case OP_EQ:
                emit(start, OP_EQ, condition.rhs_val);
                propagate(start, condition.rhs_val, reverse, OP_LE, OP_LT, false, false);
                propagate(start, condition.rhs_val, forward, OP_GE, OP_GT, false, false);
                break;
            case OP_NE:
                emit(start, OP_NE, condition.rhs_val);
                break;
            case OP_LT:
                propagate(start, condition.rhs_val, reverse, OP_LE, OP_LT, true);
                break;
            case OP_LE:
                propagate(start, condition.rhs_val, reverse, OP_LE, OP_LT, false);
                break;
            case OP_GT:
                propagate(start, condition.rhs_val, forward, OP_GE, OP_GT, true);
                break;
            case OP_GE:
                propagate(start, condition.rhs_val, forward, OP_GE, OP_GT, false);
                break;
        }
    }
    return result;
}

}  // namespace predicate_inference
