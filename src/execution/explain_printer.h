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
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "executor_abstract.h"
#include "executor_filter.h"
#include "executor_nestedloop_join.h"
#include "executor_projection.h"
#include "executor_seq_scan.h"
#include "optimizer/plan.h"

/** EXPLAIN ANALYZE 计划树格式化与运行时统计 */
class ExplainPrinter {
   public:
    static void write_explain(std::shared_ptr<Plan> plan, AbstractExecutor *root,
                                const std::map<std::string, std::string> &tab_to_alias,
                                bool is_select_all, const std::vector<TabCol> &sel_cols,
                                const std::vector<ExplainAnalyzePlan::JoinInfo> &joins = {}) {
        std::map<Plan *, size_t> rows_map;
        collect_plan_stats(plan, root, rows_map);
        std::fstream outfile("output.txt", std::ios::out | std::ios::app);
        int join_idx = 0;
        print_plan(plan, 0, outfile, tab_to_alias, is_select_all, sel_cols, rows_map, joins, join_idx);
        outfile.close();
    }

   private:
    static std::string op_to_str(CompOp op) {
        static const std::map<CompOp, std::string> m = {
            {OP_EQ, "="}, {OP_NE, "<>"}, {OP_LT, "<"}, {OP_GT, ">"},
            {OP_LE, "<="}, {OP_GE, ">="},
        };
        return m.at(op);
    }

    static std::string col_prefix(const TabCol &col, const std::map<std::string, std::string> &tab_to_alias) {
        auto it = tab_to_alias.find(col.tab_name);
        std::string tab = (it != tab_to_alias.end()) ? it->second : col.tab_name;
        return tab + "." + col.col_name;
    }

    static std::string format_condition(const Condition &cond,
                                        const std::map<std::string, std::string> &tab_to_alias) {
        std::string s = col_prefix(cond.lhs_col, tab_to_alias) + op_to_str(cond.op);
        if (cond.is_rhs_val) {
            if (cond.rhs_val.type == TYPE_INT) {
                s += std::to_string(cond.rhs_val.int_val);
            } else if (cond.rhs_val.type == TYPE_FLOAT) {
                float v = cond.rhs_val.float_val;
                if (v == static_cast<float>(static_cast<int>(v))) {
                    s += std::to_string(static_cast<int>(v));
                } else {
                    s += std::to_string(v);
                }
            } else {
                s += "'" + cond.rhs_val.str_val + "'";
            }
        } else {
            s += col_prefix(cond.rhs_col, tab_to_alias);
        }
        return s;
    }

    static std::vector<std::string> format_conditions(const std::vector<Condition> &conds,
                                                      const std::map<std::string, std::string> &tab_to_alias) {
        std::vector<std::string> res;
        for (auto &cond : conds) {
            res.push_back(format_condition(cond, tab_to_alias));
        }
        std::sort(res.begin(), res.end());
        return res;
    }

    static std::string join_conditions_str(const std::vector<Condition> &conds,
                                           const std::map<std::string, std::string> &tab_to_alias) {
        auto formatted = format_conditions(conds, tab_to_alias);
        std::string s;
        for (size_t i = 0; i < formatted.size(); i++) {
            if (i > 0) s += ", ";
            s += formatted[i];
        }
        return s;
    }

    static std::string format_columns(const std::vector<TabCol> &cols,
                                      const std::map<std::string, std::string> &tab_to_alias) {
        std::vector<std::string> formatted;
        for (auto &col : cols) {
            formatted.push_back(col_prefix(col, tab_to_alias));
        }
        std::sort(formatted.begin(), formatted.end());
        std::string s;
        for (size_t i = 0; i < formatted.size(); i++) {
            if (i > 0) s += ", ";
            s += formatted[i];
        }
        return s;
    }

    static void collect_tables(const std::shared_ptr<Plan> &plan, std::set<std::string> &tables) {
        if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            tables.insert(x->tab_name_);
        } else if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            collect_tables(x->subplan_, tables);
        } else if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            collect_tables(x->subplan_, tables);
        } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            collect_tables(x->left_, tables);
            collect_tables(x->right_, tables);
        } else if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
            collect_tables(x->subplan_, tables);
        }
    }

    static std::string indent_str(int depth) {
        return std::string(depth, '\t');
    }

    static size_t get_rows(Plan *plan, const std::map<Plan *, size_t> &rows_map) {
        auto it = rows_map.find(plan);
        return (it != rows_map.end()) ? it->second : 0;
    }

    static void multiply_subtree_rows_ptr(std::shared_ptr<Plan> plan, size_t factor,
                                          std::map<Plan *, size_t> &rows_map) {
        if (factor <= 1 || !plan) return;
        rows_map[plan.get()] = get_rows(plan.get(), rows_map) * factor;
        if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            multiply_subtree_rows_ptr(x->subplan_, factor, rows_map);
        } else if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            multiply_subtree_rows_ptr(x->subplan_, factor, rows_map);
        } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            multiply_subtree_rows_ptr(x->left_, factor, rows_map);
            multiply_subtree_rows_ptr(x->right_, factor, rows_map);
        } else if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
            multiply_subtree_rows_ptr(x->subplan_, factor, rows_map);
        }
    }

    static size_t count_executor_output(AbstractExecutor *exec) {
        size_t cnt = 0;
        for (exec->beginTuple(); !exec->is_end(); exec->nextTuple()) {
            exec->Next();
            cnt++;
        }
        return cnt;
    }

    static size_t count_scan_rows(AbstractExecutor *exec) {
        size_t cnt = 0;
        for (exec->beginTuple(); !exec->is_end(); exec->nextTuple()) {
            exec->Next();
            cnt++;
        }
        return cnt;
    }

    static void collect_plan_stats(std::shared_ptr<Plan> plan, AbstractExecutor *exec,
                                   std::map<Plan *, size_t> &rows_map) {
        if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
            collect_plan_stats(x->subplan_, exec, rows_map);
            rows_map[plan.get()] = get_rows(x->subplan_.get(), rows_map);
            return;
        }
        if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            auto proj = dynamic_cast<ProjectionExecutor *>(exec);
            collect_plan_stats(x->subplan_, proj->child(), rows_map);
            // 投影不改变行数
            rows_map[plan.get()] = get_rows(x->subplan_.get(), rows_map);
            return;
        }
        if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            auto filter = dynamic_cast<FilterExecutor *>(exec);
            collect_plan_stats(x->subplan_, filter->child(), rows_map);
            rows_map[plan.get()] = count_executor_output(exec);
            return;
        }
        if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            auto join = dynamic_cast<NestedLoopJoinExecutor *>(exec);
            collect_plan_stats(x->left_, join->left_child(), rows_map);
            size_t left_rows = get_rows(x->left_.get(), rows_map);
            collect_plan_stats(x->right_, join->right_child(), rows_map);
            // NLJ 内表重复执行，累计右侧子树行数
            multiply_subtree_rows_ptr(x->right_, left_rows, rows_map);
            rows_map[plan.get()] = count_executor_output(exec);
            return;
        }
        if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            rows_map[plan.get()] = count_scan_rows(exec);
            return;
        }
    }

    static void print_plan(std::shared_ptr<Plan> plan, int depth, std::ostream &os,
                           const std::map<std::string, std::string> &tab_to_alias,
                           bool is_select_all, const std::vector<TabCol> &sel_cols,
                           const std::map<Plan *, size_t> &rows_map,
                           const std::vector<ExplainAnalyzePlan::JoinInfo> &joins,
                           int &join_idx) {
        if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
            print_plan(x->subplan_, depth, os, tab_to_alias, is_select_all, sel_cols, rows_map, joins, join_idx);
            return;
        }
        if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            std::string cols_str;
            if (depth == 0 && is_select_all) {
                cols_str = "*";
            } else if (depth == 0) {
                cols_str = format_columns(sel_cols, tab_to_alias);
            } else {
                cols_str = format_columns(x->sel_cols_, tab_to_alias);
            }
            os << indent_str(depth) << "Project(columns=[" << cols_str
               << "], rows=" << get_rows(plan.get(), rows_map) << ")\n";
            print_plan(x->subplan_, depth + 1, os, tab_to_alias, is_select_all, sel_cols, rows_map, joins, join_idx);
            return;
        }
        if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            os << indent_str(depth) << "Filter(condition=["
               << join_conditions_str(x->conds_, tab_to_alias)
               << "], rows=" << get_rows(plan.get(), rows_map) << ")\n";
            print_plan(x->subplan_, depth + 1, os, tab_to_alias, is_select_all, sel_cols, rows_map, joins, join_idx);
            return;
        }
        if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            std::set<std::string> tables;
            collect_tables(plan, tables);
            auto conds = x->conds_;
            if (conds.empty() && join_idx < static_cast<int>(joins.size())) {
                conds = joins[join_idx].conds;
            }
            join_idx++;
            os << indent_str(depth) << "Join(tables=[";
            bool first = true;
            for (auto &t : tables) {
                if (!first) os << ", ";
                os << t;
                first = false;
            }
            os << "], condition=[" << join_conditions_str(conds, tab_to_alias)
               << "], rows=" << get_rows(plan.get(), rows_map) << ")\n";
            print_plan(x->left_, depth + 1, os, tab_to_alias, is_select_all, sel_cols, rows_map, joins, join_idx);
            print_plan(x->right_, depth + 1, os, tab_to_alias, is_select_all, sel_cols, rows_map, joins, join_idx);
            return;
        }
        if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            os << indent_str(depth) << "Scan(table=" << x->tab_name_
               << ", type=SeqScan, rows=" << get_rows(plan.get(), rows_map) << ")\n";
            return;
        }
    }
};
