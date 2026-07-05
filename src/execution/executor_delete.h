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

class DeleteExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Condition> conds_;  // delete的条件
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::vector<Rid> rids_;         // 需要删除的记录的位置
    std::string tab_name_;          // 表名称
    SmManager *sm_manager_;

    /** 判断记录是否满足单个 WHERE 条件 */
    bool check_condition(RmRecord *rec, const Condition &condition) {
        auto left_col = get_col(tab_.cols, condition.lhs_col);
        char *l_value = rec->data + left_col->offset;
        char *r_value;
        ColType r_type;
        if (condition.is_rhs_val) {
            r_value = condition.rhs_val.raw->data;
            r_type = condition.rhs_val.type;
        } else {
            auto r_col = get_col(tab_.cols, condition.rhs_col);
            r_value = rec->data + r_col->offset;
            r_type = r_col->type;
        }
        return evaluate_compare(l_value, r_value, r_type, left_col->len, condition.op);
    }

    bool check_conditions(RmRecord *rec) {
        return std::all_of(conds_.begin(), conds_.end(),
                           [&](const Condition &cond) { return check_condition(rec, cond); });
    }

   public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Condition> conds,
                   std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = std::move(conds);
        rids_ = std::move(rids);
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        for (const auto &rid : rids_) {
            auto tuple = fh_->get_record(rid, context_);
            if (!check_conditions(tuple.get())) {
                continue;
            }
            // 同步删除索引项
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                auto ih = sm_manager_->ihs_
                              .at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols))
                              .get();
                char *key = new char[index.col_tot_len];
                int offset = 0;
                for (size_t j = 0; j < index.col_num; ++j) {
                    memcpy(key + offset, tuple->data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                ih->delete_entry(key, context_->txn_);
                delete[] key;
            }
            fh_->delete_record(rid, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
