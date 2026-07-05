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
#include "record/rm_scan.h"
#include "system/sm.h"

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator
    std::unique_ptr<RmRecord> rec_;
    bool is_end_{true};  // 扫描是否结束

    SmManager *sm_manager_;

    /** 判断单条记录是否满足一个条件 */
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

    /** 判断单条记录是否满足全部 WHERE 条件 */
    bool check_conditions(RmRecord *rec) {
        return std::all_of(fed_conds_.begin(), fed_conds_.end(),
                           [&](const Condition &cond) { return check_condition(rec, cond); });
    }

    /** 扫描到下一条满足条件的记录 */
    bool find_next_match() {
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            rec_ = fh_->get_record(rid_, context_);
            if (check_conditions(rec_.get())) {
                is_end_ = false;
                return true;
            }
            scan_->next();
        }
        is_end_ = true;
        rec_.reset();
        return false;
    }

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    void beginTuple() override {
        scan_ = std::make_unique<RmScan>(fh_);
        find_next_match();  // 定位首条匹配记录
    }

    void nextTuple() override {
        scan_->next();
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

    Rid &rid() override { return rid_; }
};
