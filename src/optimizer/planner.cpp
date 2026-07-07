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

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "execution/executor_delete.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "index/ix.h"
#include "record_printer.h"
#include "record/rm_scan.h"

/** 从 AST 获取 SelectStmt（兼容 ExplainAnalyze 包装） */
static std::shared_ptr<ast::SelectStmt> get_select_stmt(const std::shared_ptr<ast::TreeNode> &parse) {
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse)) {
        return x;
    }
    if (auto x = std::dynamic_pointer_cast<ast::ExplainAnalyze>(parse)) {
        return x->select;
    }
    return nullptr;
}

/** 统计表行数，用于连接顺序优化 */
static size_t get_table_row_count(SmManager *sm, const std::string &tab_name) {
    auto fh = sm->fhs_.at(tab_name).get();
    size_t cnt = 0;
    RmScan scan(fh);
    while (!scan.is_end()) {
        cnt++;
        scan.next();
    }
    return cnt;
}

/** 将 ON 子句中的 BinaryExpr 转为 Condition */
static void convert_join_conds(const std::shared_ptr<ast::JoinExpr> &je,
                               std::vector<Condition> &conds,
                               const std::map<std::string, std::string> &alias_to_tab) {
    for (auto &expr : je->conds) {
        Condition cond;
        TabCol lhs = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        if (!lhs.tab_name.empty() && alias_to_tab.count(lhs.tab_name)) {
            lhs.tab_name = alias_to_tab.at(lhs.tab_name);
        }
        cond.lhs_col = lhs;
        std::map<ast::SvCompOp, CompOp> op_map = {
            {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
            {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
        };
        cond.op = op_map.at(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            if (auto v = std::dynamic_pointer_cast<ast::IntLit>(rhs_val)) {
                cond.rhs_val.set_int(v->val);
            } else if (auto v = std::dynamic_pointer_cast<ast::FloatLit>(rhs_val)) {
                cond.rhs_val.set_float(v->val);
            } else if (auto v = std::dynamic_pointer_cast<ast::StringLit>(rhs_val)) {
                cond.rhs_val.set_str(v->val);
            }
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            TabCol rhs = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
            if (!rhs.tab_name.empty() && alias_to_tab.count(rhs.tab_name)) {
                rhs.tab_name = alias_to_tab.at(rhs.tab_name);
            }
            cond.rhs_col = rhs;
        }
        conds.push_back(cond);
    }
}

/** 收集某表在子树中需要的列（投影下推） */
static std::set<TabCol> collect_needed_cols_for_table(
    const std::string &tab_name, const std::vector<TabCol> &sel_cols,
    const std::vector<Condition> &conds) {
    std::set<TabCol> needed;
    for (auto &col : sel_cols) {
        if (col.tab_name == tab_name) {
            needed.insert(col);
        }
    }
    for (auto &cond : conds) {
        if (cond.lhs_col.tab_name == tab_name) {
            needed.insert(cond.lhs_col);
        }
        if (!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name) {
            needed.insert(cond.rhs_col);
        }
    }
    return needed;
}

/** 收集计划子树涉及的全部条件 */
static void collect_all_conds(const std::shared_ptr<Plan> &plan, std::vector<Condition> &conds) {
    if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        conds.insert(conds.end(), x->conds_.begin(), x->conds_.end());
    } else if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        conds.insert(conds.end(), x->conds_.begin(), x->conds_.end());
        collect_all_conds(x->subplan_, conds);
    } else if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        collect_all_conds(x->subplan_, conds);
    } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        conds.insert(conds.end(), x->conds_.begin(), x->conds_.end());
        collect_all_conds(x->left_, conds);
        collect_all_conds(x->right_, conds);
    } else if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
        collect_all_conds(x->subplan_, conds);
    }
}

static void collect_plan_tables(const std::shared_ptr<Plan> &plan, std::set<std::string> &tables) {
    if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        tables.insert(x->tab_name_);
    } else if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        collect_plan_tables(x->subplan_, tables);
    } else if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        collect_plan_tables(x->subplan_, tables);
    } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        collect_plan_tables(x->left_, tables);
        collect_plan_tables(x->right_, tables);
    } else if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
        collect_plan_tables(x->subplan_, tables);
    }
}

static bool subtree_contains_table(const std::shared_ptr<Plan> &plan, const std::string &tab) {
    std::set<std::string> tables;
    collect_plan_tables(plan, tables);
    return tables.count(tab) > 0;
}

/** 单表条件所属表名；跨表条件返回空串 */
static std::string single_table_of_cond(const Condition &c) {
    if (c.is_rhs_val) {
        return c.lhs_col.tab_name;
    }
    if (c.lhs_col.tab_name == c.rhs_col.tab_name) {
        return c.lhs_col.tab_name;
    }
    return "";
}

static bool is_single_table_cond(const Condition &c, const std::string &tab) {
    return single_table_of_cond(c) == tab;
}

static void swap_cond_sides(Condition &cond) {
    static const std::map<CompOp, CompOp> swap_op = {
        {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
    };
    std::swap(cond.lhs_col, cond.rhs_col);
    cond.op = swap_op.at(cond.op);
}

/** 将条件追加到子树顶部的 Filter，若无 Filter 则新建 */
static void append_cond_to_subtree(std::shared_ptr<Plan> &plan, Condition cond) {
    if (auto filter = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        filter->conds_.push_back(std::move(cond));
        return;
    }
    plan = std::make_shared<FilterPlan>(T_Filter, plan, std::vector<Condition>{std::move(cond)});
}

/** 判断条件能否作为单表谓词写入 Scan */
static bool normalize_and_belongs_to_table(Condition &c, const std::string &tab) {
    if (c.is_rhs_val) {
        return c.lhs_col.tab_name == tab;
    }
    if (c.lhs_col.tab_name == tab && c.rhs_col.tab_name == tab) {
        return true;
    }
    return false;
}

/** 将 Join 上的 Filter 条件下推到左右子树 */
static std::shared_ptr<Plan> push_filters_down(const std::shared_ptr<Plan> &plan) {
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        join->left_ = push_filters_down(join->left_);
        join->right_ = push_filters_down(join->right_);
        return plan;
    }
    if (auto proj = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        proj->subplan_ = push_filters_down(proj->subplan_);
        return plan;
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        sort->subplan_ = push_filters_down(sort->subplan_);
        return plan;
    }
    if (auto filter = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        if (auto join = std::dynamic_pointer_cast<JoinPlan>(filter->subplan_)) {
            std::vector<Condition> remaining;
            for (auto &cond : filter->conds_) {
                Condition c = cond;
                std::string tab = single_table_of_cond(c);
                if (!tab.empty() && subtree_contains_table(join->left_, tab) &&
                    !subtree_contains_table(join->right_, tab)) {
                    append_cond_to_subtree(join->left_, std::move(c));
                } else if (!tab.empty() && subtree_contains_table(join->right_, tab) &&
                           !subtree_contains_table(join->left_, tab)) {
                    append_cond_to_subtree(join->right_, std::move(c));
                } else {
                    remaining.push_back(std::move(cond));
                }
            }
            join->left_ = push_filters_down(join->left_);
            join->right_ = push_filters_down(join->right_);
            if (remaining.empty()) {
                return join;
            }
            filter->conds_ = std::move(remaining);
            filter->subplan_ = join;
            return plan;
        }
        filter->subplan_ = push_filters_down(filter->subplan_);
        return plan;
    }
    return plan;
}

/** 将 Scan 上的条件拆分为独立 Filter 节点（IndexScan 保留条件以正确走索引） */
static std::shared_ptr<Plan> split_filters(const std::shared_ptr<Plan> &plan) {
    if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        if (x->conds_.empty() || x->tag == T_IndexScan) {
            return plan;
        }
        auto conds = std::move(x->conds_);
        x->conds_.clear();
        x->fed_conds_.clear();
        return std::make_shared<FilterPlan>(T_Filter, x, std::move(conds));
    }
    if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        x->left_ = split_filters(x->left_);
        x->right_ = split_filters(x->right_);
        return plan;
    }
    if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        x->subplan_ = split_filters(x->subplan_);
        return plan;
    }
    if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
        x->subplan_ = split_filters(x->subplan_);
        return plan;
    }
    if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        x->subplan_ = split_filters(x->subplan_);
        return plan;
    }
    return plan;
}

/** 投影下推：在连接分支上保留必要列 */
static std::shared_ptr<Plan> push_projection(const std::shared_ptr<Plan> &plan,
                                               const std::vector<TabCol> &sel_cols,
                                               const std::vector<Condition> &all_conds) {
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        join->left_ = push_projection(join->left_, sel_cols, all_conds);
        join->right_ = push_projection(join->right_, sel_cols, all_conds);
        return plan;
    }
    if (auto filter = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        filter->subplan_ = push_projection(filter->subplan_, sel_cols, all_conds);
        return plan;
    }
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        std::string tab = scan->tab_name_;
        auto needed = collect_needed_cols_for_table(tab, sel_cols, all_conds);
        if (needed.empty() || needed.size() >= scan->cols_.size()) {
            return plan;
        }
        std::vector<TabCol> proj_cols(needed.begin(), needed.end());
        return std::make_shared<ProjectionPlan>(T_Projection, plan, proj_cols);
    }
    if (auto proj = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        proj->subplan_ = push_projection(proj->subplan_, sel_cols, all_conds);
        return plan;
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        sort->subplan_ = push_projection(sort->subplan_, sel_cols, all_conds);
        return plan;
    }
    return plan;
}

// 最左匹配原则：选取匹配列数最多的索引，并调整 WHERE 条件顺序
/**
 * @brief 根据最左前缀原则选择可用索引，并重排条件顺序
 * @param tab_name 表名
 * @param curr_conds 该表的 WHERE 条件（会被重排）
 * @param index_col_names 输出：匹配到的索引列名（按索引定义顺序）
 * @return 是否找到可用索引
 */
bool Planner::get_index_cols(std::string tab_name, std::vector<Condition> &curr_conds,
                             std::vector<std::string> &index_col_names) {
    index_col_names.clear();
    // col_name -> (比较类型, 条件下标)；0=范围下界，1=等值，2=范围上界
    std::map<std::string, std::pair<int, int>> mp;
    for (size_t i = 0; i < curr_conds.size(); i++) {
        auto &cond = curr_conds[i];
        if (cond.lhs_col.tab_name != tab_name || !cond.is_rhs_val) {
            continue;
        }
        int op = -1;
        if (cond.op == OP_EQ) {
            op = 1;
        } else if (cond.op == OP_GT || cond.op == OP_GE) {
            op = 0;
        } else if (cond.op == OP_LT || cond.op == OP_LE) {
            op = 2;
        }
        if (op == -1) {
            continue;
        }
        if (mp.count(cond.lhs_col.col_name) && op == 2) {
            continue;  // 同列已有条件下，保留更紧的范围下界
        }
        mp[cond.lhs_col.col_name] = {op, static_cast<int>(i)};
    }

    TabMeta &tab = sm_manager_->db_.get_table(tab_name);
    int mx = 0;
    std::vector<int> ids;
    std::vector<ColMeta> cols;
    for (const auto &index : tab.indexes) {
        int cnt = 0;
        std::vector<int> tmp;
        for (const auto &col : index.cols) {
            if (!mp.count(col.name)) {
                break;
            }
            std::pair<int, int> val = mp[col.name];
            cnt++;
            tmp.push_back(val.second);
            if (val.first == 0) {
                break;  // 遇到范围下界后停止前缀扩展
            }
        }
        if (cnt > mx) {
            mx = cnt;
            ids = tmp;
            cols = index.cols;
        }
    }
    if (mx == 0) {
        return false;
    }

    std::vector<Condition> reordered;
    std::unordered_map<int, bool> vis;
    for (int id : ids) {
        reordered.push_back(curr_conds[id]);
        vis[id] = true;
    }
    for (size_t i = 0; i < curr_conds.size(); i++) {
        if (!vis.count(static_cast<int>(i))) {
            reordered.push_back(curr_conds[i]);
        }
    }
    curr_conds = std::move(reordered);
    for (const auto &col : cols) {
        index_col_names.push_back(col.name);
    }
    return true;
}

/**
 * @brief 表算子条件谓词生成
 *
 * @param conds 条件
 * @param tab_names 表名
 * @return std::vector<Condition>
 */
std::vector<Condition> pop_conds(std::vector<Condition> &conds, std::string tab_names) {
    std::vector<Condition> solved_conds;
    auto it = conds.begin();
    while (it != conds.end()) {
        if (is_single_table_cond(*it, tab_names)) {
            solved_conds.emplace_back(std::move(*it));
            it = conds.erase(it);
        } else {
            it++;
        }
    }
    return solved_conds;
}

int push_conds(Condition *cond, std::shared_ptr<Plan> plan)
{
    if (!plan) {
        return 0;
    }
    if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        return push_conds(cond, x->subplan_) == 3 ? 3 : 0;
    }
    if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        return push_conds(cond, x->subplan_) == 3 ? 3 : 0;
    }
    if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
        return push_conds(cond, x->subplan_) == 3 ? 3 : 0;
    }
    if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        Condition c = *cond;
        if (normalize_and_belongs_to_table(c, x->tab_name_)) {
            x->conds_.push_back(c);
            x->fed_conds_.push_back(c);
            return 3;
        }
        return 0;
    }
    if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        Condition c = *cond;
        if (push_conds(&c, x->left_) == 3) {
            return 3;
        }
        c = *cond;
        if (push_conds(&c, x->right_) == 3) {
            return 3;
        }
        if (!cond->is_rhs_val) {
            bool lhs_in_left = subtree_contains_table(x->left_, cond->lhs_col.tab_name);
            bool rhs_in_right = subtree_contains_table(x->right_, cond->rhs_col.tab_name);
            bool lhs_in_right = subtree_contains_table(x->right_, cond->lhs_col.tab_name);
            bool rhs_in_left = subtree_contains_table(x->left_, cond->rhs_col.tab_name);
            if (lhs_in_left && rhs_in_right) {
                x->conds_.emplace_back(*cond);
                return 3;
            }
            if (lhs_in_right && rhs_in_left) {
                Condition jc = *cond;
                swap_cond_sides(jc);
                x->conds_.emplace_back(std::move(jc));
                return 3;
            }
        }
        return 0;
    }
    return 0;
}

std::shared_ptr<Plan> pop_scan(int *scantbl, std::string table, std::vector<std::string> &joined_tables, 
                std::vector<std::shared_ptr<Plan>> plans)
{
    for (size_t i = 0; i < plans.size(); i++) {
        auto x = std::dynamic_pointer_cast<ScanPlan>(plans[i]);
        if(x->tab_name_.compare(table) == 0)
        {
            scantbl[i] = 1;
            joined_tables.emplace_back(x->tab_name_);
            return plans[i];
        }
    }
    return nullptr;
}


std::shared_ptr<Query> Planner::logical_optimization(std::shared_ptr<Query> query, Context *context)
{
    //TODO 实现逻辑优化规则
    // 连接顺序优化在 make_one_rel 中基于副本进行，避免打乱输出列顺序
    return query;
}

std::shared_ptr<Plan> Planner::physical_optimization(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plan = make_one_rel(query);
    
    // 其他物理优化

    // 处理orderby
    plan = generate_sort_plan(query, std::move(plan)); 

    return plan;
}



std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query)
{
    auto x = get_select_stmt(query->parse);
    std::vector<std::string> tables = query->tables;

    // 逗号连接时按行数升序优化连接顺序（不改变 query->tables 列顺序）
    if (query->joins.empty() && tables.size() > 1) {
        std::sort(tables.begin(), tables.end(),
                  [&](const std::string &a, const std::string &b) {
                      return get_table_row_count(sm_manager_, a) < get_table_row_count(sm_manager_, b);
                  });
    }

    // 构建 alias -> table 映射
    std::map<std::string, std::string> alias_to_tab;
    for (auto &[tab, alias] : query->tab_to_alias) {
        if (tab != alias) {
            alias_to_tab[alias] = tab;
        }
    }

    // Scan table，生成表扫描算子
    std::map<std::string, std::shared_ptr<Plan>> scan_map;
    for (auto &tab : tables) {
        auto curr_conds = pop_conds(query->conds, tab);
        std::vector<std::string> index_col_names;
        bool index_exist = !query->is_explain && get_index_cols(tab, curr_conds, index_col_names);
        if (!index_exist) {
            index_col_names.clear();
            scan_map[tab] = std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, tab, curr_conds, index_col_names);
        } else {
            scan_map[tab] = std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, tab, curr_conds, index_col_names);
        }
    }

    if (tables.size() == 1) {
        return scan_map[tables[0]];
    }

    // 有 JOIN ON 信息时按语法顺序构建左深连接树
    if (!query->joins.empty()) {
        std::shared_ptr<Plan> result;
        std::set<std::string> joined;
        for (auto &ji : query->joins) {
            std::shared_ptr<Plan> left, right;
            if (!result) {
                left = scan_map[ji.left];
                joined.insert(ji.left);
            } else {
                left = result;
            }
            right = scan_map[ji.right];
            joined.insert(ji.right);

            result = std::make_shared<JoinPlan>(T_NestLoop, left, right, ji.conds);
        }
        // 连接剩余逗号分隔的表（左深树）
        for (auto &tab : tables) {
            if (joined.find(tab) == joined.end()) {
                result = std::make_shared<JoinPlan>(T_NestLoop, result, scan_map[tab], std::vector<Condition>());
            }
        }
        // 下推剩余 WHERE 条件，未能下推的包装为 Filter
        std::vector<Condition> remaining_conds;
        for (auto &cond : query->conds) {
            Condition c = cond;
            if (push_conds(&c, result) != 3) {
                remaining_conds.push_back(std::move(cond));
            }
        }
        if (!remaining_conds.empty()) {
            result = std::make_shared<FilterPlan>(T_Filter, result, std::move(remaining_conds));
        }
        return result;
    }

    // 逗号连接：使用原有逻辑
    std::vector<std::shared_ptr<Plan>> table_scan_executors(tables.size());
    for (size_t i = 0; i < tables.size(); i++) {
        table_scan_executors[i] = scan_map[tables[i]];
    }
    // 只有一个表，不需要join。
    if(tables.size() == 1)
    {
        return table_scan_executors[0];
    }
    // 获取where条件
    auto conds = std::move(query->conds);
    std::shared_ptr<Plan> table_join_executors;
    
    int scantbl[tables.size()];
    for(size_t i = 0; i < tables.size(); i++)
    {
        scantbl[i] = -1;
    }
    // 假设在ast中已经添加了jointree，这里需要修改的逻辑是，先处理jointree，然后再考虑剩下的部分
    if(conds.size() >= 1)
    {
        // 有连接条件

        // 根据连接条件，生成第一层join
        std::vector<std::string> joined_tables(tables.size());
        auto it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left , right;
            left = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            right = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
            std::vector<Condition> join_conds{*it};
            //建立join
            // 判断使用哪种join方式
            if(enable_nestedloop_join && enable_sortmerge_join) {
                // 默认nested loop join
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            } else if(enable_nestedloop_join) {
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            } else if(enable_sortmerge_join) {
                table_join_executors = std::make_shared<JoinPlan>(T_SortMerge, std::move(left), std::move(right), join_conds);
            } else {
                // error
                throw RMDBError("No join executor selected!");
            }

            // table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            it = conds.erase(it);
            break;
        }
        // 根据连接条件，生成第2-n层join
        it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left_need_to_join_executors = nullptr;
            std::shared_ptr<Plan> right_need_to_join_executors = nullptr;
            bool isneedreverse = false;
            if (std::find(joined_tables.begin(), joined_tables.end(), it->lhs_col.tab_name) == joined_tables.end()) {
                left_need_to_join_executors = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            }
            if (std::find(joined_tables.begin(), joined_tables.end(), it->rhs_col.tab_name) == joined_tables.end()) {
                right_need_to_join_executors = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
                isneedreverse = true;
            } 

            if(left_need_to_join_executors != nullptr && right_need_to_join_executors != nullptr) {
                std::vector<Condition> join_conds{*it};
                std::shared_ptr<Plan> temp_join_executors = std::make_shared<JoinPlan>(T_NestLoop, 
                                                                    std::move(left_need_to_join_executors), 
                                                                    std::move(right_need_to_join_executors), 
                                                                    join_conds);
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(table_join_executors),
                                                                    std::move(temp_join_executors), 
                                                                    std::vector<Condition>());
                it = conds.erase(it);
            } else if(left_need_to_join_executors != nullptr || right_need_to_join_executors != nullptr) {
                std::shared_ptr<Plan> new_table;
                bool need_swap = false;
                if (left_need_to_join_executors != nullptr) {
                    new_table = std::move(left_need_to_join_executors);
                    need_swap = true;
                } else {
                    new_table = std::move(right_need_to_join_executors);
                }
                if (need_swap) {
                    std::map<CompOp, CompOp> swap_op = {
                        {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
                    };
                    std::swap(it->lhs_col, it->rhs_col);
                    it->op = swap_op.at(it->op);
                }
                std::vector<Condition> join_conds{*it};
                // 左深连接树：已有结果在左，新表在右
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(table_join_executors),
                                                                    std::move(new_table), join_conds);
                it = conds.erase(it);
            } else {
                Condition c = *it;
                if (push_conds(&c, table_join_executors) == 3) {
                    it = conds.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!conds.empty()) {
            table_join_executors = std::make_shared<FilterPlan>(T_Filter, table_join_executors, std::move(conds));
        }
    } else {
        table_join_executors = table_scan_executors[0];
        scantbl[0] = 1;
    }

    //连接剩余表（左深树：新表接在右侧）
    for (size_t i = 0; i < tables.size(); i++) {
        if(scantbl[i] == -1) {
            table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(table_join_executors), 
                                                    std::move(table_scan_executors[i]), std::vector<Condition>());
        }
    }

    return table_join_executors;

}


std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    auto x = get_select_stmt(query->parse);
    if (!x || !x->has_sort) {
        return plan;
    }
    std::vector<std::string> tables = query->tables;
    std::vector<ColMeta> all_cols;
    for (auto &sel_tab_name : tables) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
    TabCol sel_col;
    for (auto &col : all_cols) {
        if(col.name.compare(x->order->cols->col_name) == 0 )
        sel_col = {.tab_name = col.tab_name, .col_name = col.name};
    }
    return std::make_shared<SortPlan>(T_Sort, std::move(plan), sel_col, 
                                    x->order->orderby_dir == ast::OrderBy_DESC);
}


/**
 * @brief select plan 生成
 *
 * @param sel_cols select plan 选取的列
 * @param tab_names select plan 目标的表
 * @param conds select plan 选取条件
 */
std::shared_ptr<Plan> Planner::generate_select_plan(std::shared_ptr<Query> query, Context *context) {
    //逻辑优化
    query = logical_optimization(std::move(query), context);

    //物理优化
    auto sel_cols = query->cols;
    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, context);

    // 选择下推：将 Scan 条件拆分为 Filter 节点
    plannerRoot = split_filters(plannerRoot);
    // 将 Join 上的 Filter 继续下推到左右子树
    plannerRoot = push_filters_down(plannerRoot);

    // 投影下推：非 SELECT * 时在连接分支保留必要列
    if (!query->is_select_all) {
        std::vector<Condition> all_conds;
        collect_all_conds(plannerRoot, all_conds);
        all_conds.insert(all_conds.end(), query->conds.begin(), query->conds.end());
        for (auto &j : query->joins) {
            all_conds.insert(all_conds.end(), j.conds.begin(), j.conds.end());
        }
        plannerRoot = push_projection(plannerRoot, sel_cols, all_conds);
    }

    plannerRoot = std::make_shared<ProjectionPlan>(T_Projection, std::move(plannerRoot), 
                                                        std::move(sel_cols));

    return plannerRoot;
}

// 生成DDL语句和DML语句的查询执行计划
std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plannerRoot;
    if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
        // create table;
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
        // drop table;
        plannerRoot = std::make_shared<DDLPlan>(T_DropTable, x->tab_name, std::vector<std::string>(), std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
        // create index;
        plannerRoot = std::make_shared<DDLPlan>(T_CreateIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::ShowIndex>(query->parse)) {
        plannerRoot = std::make_shared<DDLPlan>(T_ShowIndex, x->tab_name, std::vector<std::string>(), std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
        // drop index
        plannerRoot = std::make_shared<DDLPlan>(T_DropIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
        // insert;
        plannerRoot = std::make_shared<DMLPlan>(T_Insert, std::shared_ptr<Plan>(),  x->tab_name,  
                                                    query->values, std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
        // delete;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);
        
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }

        plannerRoot = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, x->tab_name,  
                                                std::vector<Value>(), query->conds, std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        // update;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);

        if (index_exist == false) {  // 该表没有索引
        index_col_names.clear();
            table_scan_executors = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }
        plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors, x->tab_name,
                                                     std::vector<Value>(), query->conds, 
                                                     query->set_clauses);
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {

        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);
        // 生成select语句的查询执行计划
        std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);
        plannerRoot = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                    std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::ExplainAnalyze>(query->parse)) {
        // EXPLAIN ANALYZE：生成优化计划并包装
        auto tab_to_alias = query->tab_to_alias;
        auto is_select_all = query->is_select_all;
        auto sel_cols = query->cols;
        auto joins = query->joins;
        std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);
        std::vector<ExplainAnalyzePlan::JoinInfo> plan_joins;
        for (auto &j : joins) {
            ExplainAnalyzePlan::JoinInfo pj;
            pj.left = j.left;
            pj.right = j.right;
            pj.conds = j.conds;
            plan_joins.push_back(std::move(pj));
        }
        plannerRoot = std::make_shared<ExplainAnalyzePlan>(projection, tab_to_alias, is_select_all, sel_cols, plan_joins);
    } else {
        throw InternalError("Unexpected AST root");
    }
    return plannerRoot;
}