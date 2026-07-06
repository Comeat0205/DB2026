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

#include "execution_defs.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

/** 过滤算子：对子算子输出执行选择运算 */
class FilterExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> child_;
    std::vector<Condition> conds_;
    std::vector<ColMeta> cols_;
    size_t len_;
    bool is_end_{true};
    std::unique_ptr<RmRecord> rec_;

    bool check_condition(RmRecord *rec, const Condition &condition) {
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

    bool check_conditions(RmRecord *rec) {
        return std::all_of(conds_.begin(), conds_.end(),
                           [&](const Condition &cond) { return check_condition(rec, cond); });
    }

    bool find_next_match() {
        while (!child_->is_end()) {
            rec_ = child_->Next();
            if (check_conditions(rec_.get())) {
                is_end_ = false;
                return true;
            }
            child_->nextTuple();
        }
        is_end_ = true;
        rec_.reset();
        return false;
    }

   public:
    FilterExecutor(std::unique_ptr<AbstractExecutor> child, std::vector<Condition> conds)
        : child_(std::move(child)), conds_(std::move(conds)) {
        cols_ = child_->cols();
        len_ = child_->tupleLen();
    }

    void beginTuple() override {
        child_->beginTuple();
        find_next_match();
    }

    void nextTuple() override {
        child_->nextTuple();
        find_next_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*rec_);
    }

    bool is_end() const override { return is_end_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    Rid &rid() override { return _abstract_rid; }

    AbstractExecutor *child() override { return child_.get(); }
};
