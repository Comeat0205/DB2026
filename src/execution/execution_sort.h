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
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    ColMeta sort_col_;
    bool is_desc_;
    size_t tuple_idx_;
    bool is_end_{false};
    std::vector<std::unique_ptr<RmRecord>> tuples_;  // 排序后的全部结果

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, TabCol sel_cols, bool is_desc) {
        prev_ = std::move(prev);
        sort_col_ = *prev_->get_col(prev_->cols(), sel_cols);
        is_desc_ = is_desc;
        tuple_idx_ = 0;
    }

    void beginTuple() override {
        tuples_.clear();
        tuple_idx_ = 0;
        // 拉取子算子全部结果后排序
        prev_->beginTuple();
        while (!prev_->is_end()) {
            tuples_.push_back(prev_->Next());
            prev_->nextTuple();
        }
        std::sort(tuples_.begin(), tuples_.end(), [this](const std::unique_ptr<RmRecord> &lhs,
                                                          const std::unique_ptr<RmRecord> &rhs) {
            int res = value_compare(lhs->data + sort_col_.offset, rhs->data + sort_col_.offset, sort_col_.type,
                                    sort_col_.len);
            if (is_desc_) {
                return res > 0;
            }
            return res < 0;
        });
        is_end_ = tuples_.empty();
    }

    void nextTuple() override {
        tuple_idx_++;
        if (tuple_idx_ >= tuples_.size()) {
            is_end_ = true;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*tuples_[tuple_idx_]);
    }

    bool is_end() const override { return is_end_; }

    size_t tupleLen() const override { return prev_->tupleLen(); }

    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }

    Rid &rid() override { return _abstract_rid; }
};
