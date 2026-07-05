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
#include <climits>
#include <map>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据
    IxIndexHandle *ih_;                         // 索引文件句柄

    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    int index_cnt_{0};   // 索引前缀匹配到的条件数
    bool is_end_{true};  // 扫描是否结束

    SmManager *sm_manager_;

    /** 判断单条记录是否满足一个 WHERE 条件 */
    bool check_condition(RmRecord *rec, const Condition &condition) const {
        auto left_col = get_col(cols_, condition.lhs_col);
        char *l_value = rec->data + left_col->offset;
        char *r_value;
        ColType r_type;
        if (condition.is_rhs_val) {
            r_value = condition.rhs_val.raw->data;
            r_type = condition.rhs_val.type;
        } else {
            auto r_col = get_col(cols_, condition.rhs_col);
            r_value = rec->data + r_col->offset;
            r_type = r_col->type;
        }
        return evaluate_compare(l_value, r_value, r_type, left_col->len, condition.op);
    }

    /** 判断单条记录是否满足全部 WHERE 条件 */
    bool check_conditions(RmRecord *rec) {
        return std::all_of(fed_conds_.begin(), fed_conds_.end(),
                           [&](const Condition &cond) { return check_condition(rec, cond); });
    }

    /** 为索引键的某一列填充极大/极小边界值 */
    void fill_extreme(char *key, int offset, size_t col_idx, bool upper_flag) {
        auto &col = index_meta_.cols[col_idx];
        Value bound;
        switch (col.type) {
            case TYPE_INT:
                bound.set_int(upper_flag ? INT32_MAX : INT32_MIN);
                break;
            case TYPE_FLOAT:
                bound.set_float(upper_flag ? 1e38f : -1e38f);
                break;
            case TYPE_STRING:
                bound.set_str(upper_flag ? std::string(col.len, char(127)) : "");
                break;
            default:
                return;
        }
        bound.init_raw(col.len);
        memcpy(key + offset, bound.raw->data, col.len);
    }

    /** 为索引键剩余列填充边界值，用于范围扫描 */
    void fill_bound_key(char *key, int offset, size_t start_col, bool upper_flag) {
        for (size_t i = start_col; i < index_meta_.cols.size(); i++) {
            fill_extreme(key, offset, i, upper_flag);
            offset += index_meta_.cols[i].len;
        }
    }

    /** 扫描到下一条满足全部条件的记录 */
    bool find_next_match() {
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (check_conditions(rec.get())) {
                is_end_ = false;
                return true;
            }
            scan_->next();
        }
        is_end_ = true;
        return false;
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                      std::vector<std::string> index_col_names, Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        index_col_names_ = std::move(index_col_names);
        index_meta_ = *(tab_.get_index_meta(index_col_names_));

        auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        if (!sm_manager_->ihs_.count(ix_name)) {
            // 索引尚未打开则先打开
            sm_manager_->ihs_.emplace(ix_name, sm_manager_->get_ix_manager()->open_index(tab_name_, index_col_names_));
        }
        ih_ = sm_manager_->ihs_.at(ix_name).get();
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;

        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };
        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    /** 根据索引前缀条件构造扫描区间，并定位首条匹配记录 */
    void beginTuple() override {
        std::vector<char> key(index_meta_.col_tot_len, 0);

        int offset = 0;
        int i = 0;
        bool keep_matching = true;
        // 按索引列顺序构造前缀键
        for (; i < static_cast<int>(conds_.size()) && keep_matching; i++) {
            auto &cond = conds_[i];
            if (!cond.is_rhs_val || i >= static_cast<int>(index_col_names_.size()) ||
                cond.lhs_col.tab_name != tab_name_ || cond.lhs_col.col_name != index_col_names_[i] ||
                cond.op == OP_NE) {
                break;
            }
            if (cond.op == OP_GE || cond.op == OP_GT) {
                memcpy(key.data() + offset, cond.rhs_val.raw->data, index_meta_.cols[i].len);
                offset += index_meta_.cols[i].len;
                keep_matching = false;
            } else if (cond.op == OP_LE || cond.op == OP_LT) {
                fill_extreme(key.data(), offset, i, false);
                offset += index_meta_.cols[i].len;
                keep_matching = false;
            } else {
                memcpy(key.data() + offset, cond.rhs_val.raw->data, index_meta_.cols[i].len);
                offset += index_meta_.cols[i].len;
            }
        }
        index_cnt_ = i;
        bool upper_flag =
            index_cnt_ > 0 && (conds_[index_cnt_ - 1].op == OP_GT || conds_[index_cnt_ - 1].op == OP_GE);
        fill_bound_key(key.data(), offset, static_cast<size_t>(i), upper_flag);

        // 确定 IxScan 的起止位置
        Iid start = upper_flag ? ih_->upper_bound(key.data()) : ih_->lower_bound(key.data());
        Iid end = ih_->leaf_end();
        scan_ = std::make_unique<IxScan>(ih_, start, end, sm_manager_->get_bpm());
        find_next_match();
    }

    void nextTuple() override {
        if (!is_end_) {
            scan_->next();
        }
        find_next_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) {
            return nullptr;
        }
        return fh_->get_record(rid_, context_);
    }

    bool is_end() const override {
        if (is_end_) {
            return true;
        }
        if (scan_->is_end()) {
            return true;
        }
        auto rec = fh_->get_record(scan_->rid(), context_);
        // 检查索引前缀条件是否仍成立
        for (int i = 0; i < index_cnt_; i++) {
            if (!check_condition(rec.get(), conds_[i])) {
                return true;
            }
        }
        return false;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    Rid &rid() override { return rid_; }
};
