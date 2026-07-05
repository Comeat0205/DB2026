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

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

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
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = std::move(set_clauses);
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = std::move(conds);
        rids_ = std::move(rids);
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        // 预先序列化 SET 子句中的新值
        std::vector<int> set_cols;
        std::vector<int> set_lens;
        for (auto &set_clause : set_clauses_) {
            auto set_col = tab_.get_col(set_clause.lhs.col_name);
            if (set_col->type != set_clause.rhs.type) {
                throw IncompatibleTypeError(coltype2str(set_col->type), coltype2str(set_clause.rhs.type));
            }
            set_clause.rhs.init_raw(set_col->len);
            set_cols.push_back(set_col->offset);
            set_lens.push_back(set_col->len);
        }

        for (const auto &rid : rids_) {
            auto tuple = fh_->get_record(rid, context_);
            if (!check_conditions(tuple.get())) {
                continue;
            }
            RmRecord new_tuple(tuple->size, tuple->data);
            for (size_t i = 0; i < set_clauses_.size(); i++) {
                memcpy(new_tuple.data + set_cols[i], set_clauses_[i].rhs.raw->data, set_lens[i]);
            }
            std::vector<std::vector<char>> old_keys;
            std::vector<std::vector<char>> new_keys;
            old_keys.reserve(tab_.indexes.size());
            new_keys.reserve(tab_.indexes.size());
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                std::vector<char> old_key(index.col_tot_len);
                std::vector<char> new_key(index.col_tot_len);
                int offset = 0;
                for (size_t j = 0; j < index.col_num; ++j) {
                    memcpy(old_key.data() + offset, tuple->data + index.cols[j].offset, index.cols[j].len);
                    memcpy(new_key.data() + offset, new_tuple.data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                old_keys.push_back(std::move(old_key));
                new_keys.push_back(std::move(new_key));
            }

            // 先插入新键（唯一约束失败时不改动表与索引），再删旧键
            std::vector<size_t> updated_indexes;
            try {
                for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                    if (memcmp(old_keys[i].data(), new_keys[i].data(), tab_.indexes[i].col_tot_len) == 0) {
                        continue;
                    }
                    auto ih = sm_manager_->ihs_
                                  .at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, tab_.indexes[i].cols))
                                  .get();
                    ih->insert_entry(new_keys[i].data(), rid, context_->txn_);
                    updated_indexes.push_back(i);
                }
                for (size_t idx : updated_indexes) {
                    auto ih = sm_manager_->ihs_
                                  .at(sm_manager_->get_ix_manager()->get_index_name(tab_name_,
                                                                                     tab_.indexes[idx].cols))
                                  .get();
                    ih->delete_entry(old_keys[idx].data(), context_->txn_);
                }
            } catch (...) {
                for (size_t idx : updated_indexes) {
                    auto ih = sm_manager_->ihs_
                                  .at(sm_manager_->get_ix_manager()->get_index_name(tab_name_,
                                                                                     tab_.indexes[idx].cols))
                                  .get();
                    ih->delete_entry(new_keys[idx].data(), context_->txn_);
                }
                throw;
            }
            fh_->update_record(rid, new_tuple.data, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
