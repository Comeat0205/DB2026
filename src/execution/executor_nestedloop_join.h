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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    std::vector<std::pair<ColMeta, ColMeta>> join_cols_;  // 左右表参与比较的列
    bool is_end_{false};

    size_t left_len_;
    size_t right_len_;
    std::unique_ptr<RmRecord> left_tuple_;
    std::vector<std::unique_ptr<RmRecord>> right_tuples_;  // 右表全部元组，物化到内存
    std::vector<std::unique_ptr<RmRecord>>::const_iterator right_tuples_iter_;
    std::unique_ptr<RmRecord> rm_record_;

    bool check_ith_condition(size_t i) {
        const auto &left_col = join_cols_.at(i).first;
        const auto &right_col = join_cols_.at(i).second;
        char *l_value = left_tuple_->data + left_col.offset;
        char *r_value = (*right_tuples_iter_)->data + right_col.offset;
        return evaluate_compare(l_value, r_value, left_col.type, left_col.len, fed_conds_.at(i).op);
    }

    bool check_conditions() {
        for (size_t i = 0; i < fed_conds_.size(); i++) {
            if (!check_ith_condition(i)) {
                return false;
            }
        }
        return true;
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        left_len_ = left_->tupleLen();
        right_len_ = right_->tupleLen();
        len_ = left_len_ + right_len_;
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        fed_conds_ = std::move(conds);

        // 预处理 join 列，转换为各自表内的 offset
        for (auto const &cond : fed_conds_) {
            assert(!cond.is_rhs_val);
            auto left_join_col = *get_col(cols_, cond.lhs_col);
            auto right_join_col = *get_col(cols_, cond.rhs_col);
            if (left_join_col.type != right_join_col.type) {
                throw IncompatibleTypeError(coltype2str(left_join_col.type), coltype2str(right_join_col.type));
            }
            if (left_join_col.offset >= left_len_) {
                left_join_col.offset -= left_len_;
            }
            if (right_join_col.offset >= left_len_) {
                right_join_col.offset -= left_len_;
            }
            join_cols_.emplace_back(left_join_col, right_join_col);
        }
    }

    void beginTuple() override {
        // 物化右表，再遍历左表做嵌套循环
        right_tuples_.clear();
        for (right_->beginTuple(); !right_->is_end(); right_->nextTuple()) {
            right_tuples_.emplace_back(right_->Next());
        }
        right_tuples_iter_ = right_tuples_.begin();
        left_->beginTuple();
        if (left_->is_end()) {
            is_end_ = true;
            return;
        }
        left_tuple_ = left_->Next();
        left_->nextTuple();
        nextTuple();
    }

    void nextTuple() override {
        while (!is_end_) {
            if (right_tuples_iter_ == right_tuples_.end()) {
                if (left_->is_end()) {
                    is_end_ = true;
                    return;
                }
                left_tuple_ = left_->Next();
                left_->nextTuple();
                right_tuples_iter_ = right_tuples_.begin();
            }

            if (check_conditions()) {
                RmRecord rm_record1(len_);
                memcpy(rm_record1.data, left_tuple_->data, left_len_);
                memcpy(rm_record1.data + left_len_, (*right_tuples_iter_)->data, right_len_);
                rm_record_ = std::make_unique<RmRecord>(rm_record1);
                right_tuples_iter_++;
                return;
            }
            right_tuples_iter_++;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) {
            return nullptr;
        }
        return std::move(rm_record_);
    }

    bool is_end() const override { return is_end_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    Rid &rid() override { return _abstract_rid; }

    AbstractExecutor *left_child() override { return left_.get(); }
    AbstractExecutor *right_child() override { return right_.get(); }
};
