/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/** 创建或恢复事务，并加入全局事务表 */
Transaction *TransactionManager::begin(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
    }
    std::lock_guard<std::mutex> lock(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    txn->set_state(TransactionState::GROWING);
    return txn;
}

/** 提交事务，清空写集与锁集 */
void TransactionManager::commit(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }
    txn->get_write_set()->clear();
    txn->get_lock_set()->clear();
    txn->set_state(TransactionState::COMMITTED);
}

/** 回滚事务，清空写集与锁集 */
void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }
    txn->get_write_set()->clear();
    txn->get_lock_set()->clear();
    txn->set_state(TransactionState::ABORTED);
}
