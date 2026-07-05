/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"

#include <algorithm>
#include <mutex>

#include "ix_scan.h"

namespace {

/** 删除节点 pos 处连续 n 个键值对（供 redistribute/coalesce 使用） */
void node_erase_pairs(IxNodeHandle *node, int pos, int n) {
    if (pos < 0 || pos >= node->get_size() || n <= 0) {
        return;
    }
    n = std::min(n, node->get_size() - pos);
    auto kp = node->get_key(pos);
    memmove(kp, kp + n * node->col_len(), (node->get_size() - pos - n) * node->col_len());
    auto rp = node->get_rid(pos);
    memmove(rp, rp + n, (node->get_size() - pos - n) * sizeof(Rid));
    node->set_size(node->get_size() - n);
}

}  // namespace

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int IxNodeHandle::lower_bound(const char *target) const {
    int l = 0, r = page_hdr->num_key - 1;
    while (l <= r) {
        int mid = (l + r) >> 1;
        int res = ix_compare(target, get_key(mid), file_hdr->col_types_, file_hdr->col_lens_);
        if (res <= 0) {
            r = mid - 1;
        } else {
            l = mid + 1;
        }
    }
    return l;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[1,num_key)，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 * @note 注意此处的范围从1开始
 */
int IxNodeHandle::upper_bound(const char *target) const {
    int l = 0, r = get_size() - 1;
    while (l <= r) {
        int mid = (l + r) >> 1;
        int res = ix_compare(target, get_key(mid), file_hdr->col_types_, file_hdr->col_lens_);
        if (res < 0) {
            r = mid - 1;
        } else {
            l = mid + 1;
        }
    }
    return l;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 *
 * @param key 目标key
 * @param[out] value 传出参数，目标key对应的Rid
 * @return 目标key是否存在
 */
bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    assert(is_leaf_page());
    int key_idx = lower_bound(key);
    if (key_idx == get_size()) {
        return false;
    }
    if (ix_compare(key, get_key(key_idx), file_hdr->col_types_, file_hdr->col_lens_) != 0) {
        return false;
    }
    *value = get_rid(key_idx);
    return true;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @param key 目标key
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t IxNodeHandle::internal_lookup(const char *key) {
    int key_idx = upper_bound(key) - 1;
    key_idx = std::max(key_idx, 0);
    return value_at(key_idx);
}

/**
 * @brief 在指定位置插入n个连续的键值对
 * 将key的前n位插入到原来keys中的pos位置；将rid的前n位插入到原来rids中的pos位置
 *
 * @param pos 要插入键值对的位置
 * @param (key, rid) 连续键值对的起始地址，也就是第一个键值对，可以通过(key, rid)来获取n个键值对
 * @param n 键值对数量
 * @note [0,pos)           [pos,num_key)
 *                            key_slot
 *                            /      \
 *                           /        \
 *       [0,pos)     [pos,pos+n)   [pos+n,num_key+n)
 *                      key           key_slot
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    if (pos < 0 || pos > page_hdr->num_key || n <= 0) {
        return;
    }
    auto kp = get_key(pos);
    memmove(kp + n * file_hdr->col_tot_len_, kp, (page_hdr->num_key - pos) * file_hdr->col_tot_len_);
    memcpy(kp, key, n * file_hdr->col_tot_len_);
    auto rp = get_rid(pos);
    memmove(rp + n, rp, (page_hdr->num_key - pos) * sizeof(Rid));
    memcpy(rp, rid, n * sizeof(Rid));
    page_hdr->num_key += n;
}

/**
 * @brief 用于在结点中插入单个键值对。
 * 函数返回插入后的键值对数量
 *
 * @param (key, value) 要插入的键值对
 * @return int 键值对数量
 */
int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    if (pos < get_size()) {
        // key 重复则不插入
        if (ix_compare(key, get_key(pos), file_hdr->col_types_, file_hdr->col_lens_) == 0) {
            return page_hdr->num_key;
        }
    }
    insert_pair(pos, key, value);
    return page_hdr->num_key;
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 *
 * @param pos 要删除键值对的位置
 */
void IxNodeHandle::erase_pair(int pos) {
    node_erase_pairs(this, pos, 1);
}

/**
 * @brief 用于在结点中删除指定key的键值对。函数返回删除后的键值对数量
 *
 * @param key 要删除的键值对key值
 * @return 完成删除操作后的键值对数量
 */
int IxNodeHandle::remove(const char *key) {
    int key_idx = lower_bound(key);
    if (key_idx == get_size()) {
        return page_hdr->num_key;
    }
    if (ix_compare(key, get_key(key_idx), file_hdr->col_types_, file_hdr->col_lens_) != 0) {
        return page_hdr->num_key;
    }
    erase_pair(key_idx);
    return page_hdr->num_key;
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    // init file_hdr_
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    char *buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    delete[] buf;

    // disk_manager管理的fd对应的文件中，设置从file_hdr_->num_pages开始分配page_no
    int now_page_no = disk_manager_->get_fd2pageno(fd);
    disk_manager_->set_fd2pageno(fd, now_page_no + 1);
}

/**
 * @brief 用于查找指定键所在的叶子结点
 * @param key 要查找的目标key值
 * @param operation 查找到目标键值对后要进行的操作类型
 * @param transaction 事务参数，如果不需要则默认传入nullptr
 * @return [leaf node] and [root_is_latched] 返回目标叶子结点以及根结点是否加锁
 * @note need to Unlatch and unpin the leaf node outside!
 * 注意：用了FindLeafPage之后一定要unlatch叶结点，否则下次latch该结点会堵塞！
 */
std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    auto root = fetch_node(file_hdr_->root_page_);
    while (!root->is_leaf_page()) {
        auto child = fetch_node(root->internal_lookup(key));
        buffer_pool_manager_->unpin_page(root->get_page_id(), true);
        root = child;
    }
    return {root, false};
}

/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 *
 * @param key 查找的目标key值
 * @param result 用于存放结果的容器
 * @param transaction 事务指针
 * @return bool 返回目标键值对是否存在
 */
bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};
    auto leaf = find_leaf_page(key, Operation::FIND, transaction, false).first;
    Rid *rid_ptr = nullptr;
    if (leaf->leaf_lookup(key, &rid_ptr)) {
        result->push_back(*rid_ptr);
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
        return true;
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    return false;
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 * @param node 需要拆分的结点
 * @return 拆分得到的new_node
 * @note need to unpin the new node outside
 * 注意：本函数执行完毕后，原node和new node都需要在函数外面进行unpin
 */
IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    int left = node->get_min_size();
    int right = node->get_size() - left;
    auto new_node = create_node();
    memcpy(new_node->page_hdr, node->page_hdr, sizeof(IxPageHdr));
    new_node->set_size(0);
    new_node->insert_pairs(0, node->get_key(left), node->get_rid(left), right);
    node->set_size(left);

    if (node->is_leaf_page()) {
        // 更新叶子链表
        page_id_t old_next = node->get_next_leaf();
        new_node->set_next_leaf(old_next);
        new_node->set_prev_leaf(node->get_page_no());
        node->set_next_leaf(new_node->get_page_no());
        IxNodeHandle *next = fetch_node(old_next);
        next->set_prev_leaf(new_node->get_page_no());
        buffer_pool_manager_->unpin_page(next->get_page_id(), true);
    } else {
        for (int i = 0; i < right; i++) {
            maintain_child(new_node, i);
        }
    }
    return new_node;
}

/**
 * @brief Insert key & value pair into internal page after split
 * 拆分(Split)后，向上找到old_node的父结点
 * 将new_node的第一个key插入到父结点，其位置在 父结点指向old_node的孩子指针 之后
 * 如果插入后>=maxsize，则必须继续拆分父结点，然后在其父结点的父结点再插入，即需要递归
 * 直到找到的old_node为根结点时，结束递归（此时将会新建一个根R，关键字为key，old_node和new_node为其孩子）
 *
 * @param (old_node, new_node) 原结点为old_node，old_node被分裂之后产生了新的右兄弟结点new_node
 * @param key 要插入parent的key
 * @note 一个结点插入了键值对之后需要分裂，分裂后左半部分的键值对保留在原结点，在参数中称为old_node，
 * 右半部分的键值对分裂为新的右兄弟节点，在参数中称为new_node（参考Split函数来理解old_node和new_node）
 * @note 本函数执行完毕后，new node和old node都需要在函数外面进行unpin
 */
void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                       Transaction *transaction) {
    if (old_node->is_root_page()) {
        auto new_root = create_node();
        new_root->page_hdr->next_free_page_no = IX_NO_PAGE;
        new_root->page_hdr->parent = IX_NO_PAGE;
        new_root->page_hdr->num_key = 0;
        new_root->page_hdr->is_leaf = false;
        new_root->page_hdr->prev_leaf = IX_NO_PAGE;
        new_root->page_hdr->next_leaf = IX_NO_PAGE;

        old_node->set_parent_page_no(new_root->get_page_no());
        new_node->set_parent_page_no(new_root->get_page_no());
        new_root->insert(old_node->get_key(0), {old_node->get_page_no(), -1});
        new_root->insert(key, {new_node->get_page_no(), -1});
        file_hdr_->root_page_ = new_root->get_page_no();
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
    } else {
        auto parent = fetch_node(old_node->get_parent_page_no());
        new_node->set_parent_page_no(parent->get_page_no());
        int cnt = parent->insert(key, {new_node->get_page_no(), -1});
        if (cnt == parent->get_max_size()) {
            auto split_node = split(parent);
            insert_into_parent(parent, split_node->get_key(0), split_node, transaction);
            buffer_pool_manager_->unpin_page(split_node->get_page_id(), true);
        }
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
    }
}

/**
 * @brief 将指定键值对插入到B+树中
 * @param (key, value) 要插入的键值对
 * @param transaction 事务指针
 * @return page_id_t 插入到的叶结点的page_no
 */
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};
    auto leaf = find_leaf_page(key, Operation::INSERT, transaction).first;
    int old_cnt = leaf->get_size();
    int cnt = leaf->insert(key, value);
    if (old_cnt == cnt) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
        // 唯一索引：重复键抛出异常
        throw IndexDuplicateKeyError();
    }

    maintain_parent(leaf);
    int pos = leaf->lower_bound(key);
    page_id_t res = leaf->get_page_no();

    if (cnt == leaf->get_max_size()) {
        auto new_node = split(leaf);
        insert_into_parent(leaf, new_node->get_key(0), new_node, transaction);
        if (file_hdr_->last_leaf_ == leaf->get_page_no()) {
            file_hdr_->last_leaf_ = new_node->get_page_no();
            auto header = fetch_node(IX_LEAF_HEADER_PAGE);
            header->set_prev_leaf(new_node->get_page_no());
            buffer_pool_manager_->unpin_page(header->get_page_id(), true);
        }
        if (pos >= leaf->get_size()) {
            res = new_node->get_page_no();
        }
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
        buffer_pool_manager_->unpin_page(new_node->get_page_id(), true);
    } else {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    }
    return res;
}

/**
 * @brief 用于删除B+树中含有指定key的键值对
 * @param key 要删除的key值
 * @param transaction 事务指针
 */
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};
    auto leaf = find_leaf_page(key, Operation::DELETE, transaction, false).first;
    int old_cnt = leaf->get_size();
    int idx = leaf->lower_bound(key);
    int now_cnt = leaf->remove(key);
    if (old_cnt == now_cnt) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
        return false;
    }
    if (idx == 0) {
        maintain_parent(leaf);
    }
    if (!coalesce_or_redistribute(leaf)) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    }
    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑，用于删除键值对后调用
 *
 * @param node 执行完删除操作的结点
 * @param transaction 事务指针
 * @param root_is_latched 传出参数：根节点是否上锁，用于并发操作
 * @return 是否需要删除结点
 * @note User needs to first find the sibling of input page.
 * If sibling's size + input page's size >= 2 * page's minsize, then redistribute.
 * Otherwise, merge(Coalesce).
 */
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    if (node->get_size() >= node->get_min_size() && node->get_size() <= node->get_max_size()) {
        return false;
    }
    if (node->is_root_page()) {
        if (adjust_root(node)) {
            if (node->is_leaf_page()) {
                erase_leaf(node);
            }
            release_node_handle(*node);
            buffer_pool_manager_->unpin_page(node->get_page_id(), true);
            delete node;
            return true;
        }
        buffer_pool_manager_->unpin_page(node->get_page_id(), true);
        return false;
    }

    auto parent = fetch_node(node->get_parent_page_no());
    int pos = parent->find_child(node);
    // 优先选取前驱兄弟
    int idx = pos - 1;
    if (idx < 0) {
        idx = pos + 1;
    }
    auto neighbor = fetch_node(parent->value_at(idx));
    bool deleted = false;
    if (node->get_size() + neighbor->get_size() >= node->get_min_size() * 2) {
        redistribute(neighbor, node, parent, pos - idx);
        buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
    } else {
        if (coalesce(&neighbor, &node, &parent, pos - idx, transaction, root_is_latched)) {
            if (!coalesce_or_redistribute(parent)) {
                buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
            }
        } else {
            buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        }
        if (pos > idx) {
            buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
            deleted = true;
        }
    }
    return deleted;
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 * @param old_root_node 原根节点
 * @return bool 根结点是否需要被删除
 * @note size of root page can be less than min size and this method is only called within coalesce_or_redistribute()
 */
bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        old_root_node->set_next_leaf(IX_LEAF_HEADER_PAGE);
        old_root_node->set_prev_leaf(IX_LEAF_HEADER_PAGE);
        old_root_node->set_parent_page_no(IX_NO_PAGE);
        return false;
    }
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        page_id_t child_page_no = old_root_node->remove_and_return_only_child();
        auto child = fetch_node(child_page_no);
        child->set_parent_page_no(IX_NO_PAGE);
        file_hdr_->root_page_ = child_page_no;
        release_node_handle(*old_root_node);
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
        return true;
    }
    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 * Redistribute key & value pairs from one page to its sibling page. If index == 0, move sibling page's first key
 * & value pair into end of input "node", otherwise move sibling page's last key & value pair into head of input "node".
 *
 * @param neighbor_node sibling page of input "node"
 * @param node input from method coalesceOrRedistribute()
 * @param parent the parent of "node" and "neighbor_node"
 * @param index node在parent中的rid_idx
 * @note node是之前刚被删除过一个key的结点
 * index=0，则neighbor是node后继结点，表示：node(left)      neighbor(right)
 * index>0，则neighbor是node前驱结点，表示：neighbor(left)  node(right)
 * 注意更新parent结点的相关kv对
 */
void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    int sum = neighbor_node->get_size() + node->get_size();
    int target = sum / 2;
    auto left = neighbor_node;
    auto right = node;
    if (index < 0) {
        std::swap(left, right);
    }
    if (left->get_size() < target) {
        int pos = left->get_size();
        int cnt = target - pos;
        left->insert_pairs(pos, right->get_key(0), right->get_rid(0), cnt);
        node_erase_pairs(right, 0, cnt);
        for (int i = pos; i < pos + cnt; i++) {
            maintain_child(left, i);
        }
    } else if (left->get_size() > target) {
        int cnt = left->get_size() - target;
        right->insert_pairs(0, left->get_key(target), left->get_rid(target), cnt);
        node_erase_pairs(left, target, cnt);
        for (int i = 0; i < cnt; i++) {
            maintain_child(right, i);
        }
    }
    maintain_parent(right);
}

/**
 * @brief 合并(Coalesce)函数是将node和其直接前驱进行合并，也就是和它左边的neighbor_node进行合并；
 * 假设node一定在右边。如果上层传入的index=0，说明node在左边，那么交换node和neighbor_node，保证node在右边；合并到左结点，实际上就是删除了右结点；
 * Move all the key & value pairs from one page to its sibling page, and notify buffer pool manager to delete this page.
 * Parent page must be adjusted to take info of deletion into account. Remember to deal with coalesce or redistribute
 * recursively if necessary.
 *
 * @param neighbor_node sibling page of input "node" (neighbor_node是node的前结点)
 * @param node input from method coalesceOrRedistribute() (node结点是需要被删除的)
 * @param parent parent page of input "node"
 * @param index node在parent中的rid_idx
 * @return true means parent node should be deleted, false means no deletion happend
 * @note Assume that *neighbor_node is the left sibling of *node (neighbor -> node)
 */
bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    auto left = *neighbor_node;
    auto right = *node;
    if (index < 0) {
        std::swap(left, right);
    }
    int cnt = right->get_size();
    int pos = left->get_size();
    left->insert_pairs(pos, right->get_key(0), right->get_rid(0), cnt);
    for (int i = pos; i < pos + cnt; i++) {
        maintain_child(left, i);
    }
    (*parent)->remove(right->get_key(0));
    if (right->is_leaf_page() && file_hdr_->last_leaf_ == right->get_page_no()) {
        file_hdr_->last_leaf_ = left->get_page_no();
    }
    buffer_pool_manager_->unpin_page(right->get_page_id(), true);
    if (right->is_leaf_page()) {
        erase_leaf(right);
    }
    release_node_handle(*right);
    delete right;
    return (*parent)->get_size() < (*parent)->get_min_size();
}

/**
 * @brief 这里把iid转换成了rid，即iid的slot_no作为node的rid_idx(key_idx)
 * node其实就是把slot_no作为键值对数组的下标
 * 换而言之，每个iid对应的索引槽存了一对(key,rid)，指向了(要建立索引的属性首地址,插入/删除记录的位置)
 *
 * @param iid
 * @return Rid
 * @note iid和rid存的不是一个东西，rid是上层传过来的记录位置，iid是索引内部生成的索引槽位置
 */
Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        throw IndexEntryNotFoundError();
    }
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    return *node->get_rid(iid.slot_no);
}

/**
 * @brief FindLeafPage + lower_bound
 *
 * @param key
 * @return Iid
 * @note 上层传入的key本来是int类型，通过(const char *)&key进行了转换
 * 可用*(int *)key转换回去
 */
Iid IxIndexHandle::lower_bound(const char *key) {
    std::scoped_lock lock{root_latch_};
    IxNodeHandle *node = find_leaf_page(key, Operation::FIND, nullptr).first;
    int key_idx = node->lower_bound(key);
    Iid iid = {.page_no = node->get_page_no(), .slot_no = key_idx};
    if (key_idx == node->get_size()) {
        if (node->get_page_no() == file_hdr_->last_leaf_) {
            iid = leaf_end();
        } else {
            iid = {.page_no = node->get_next_leaf(), .slot_no = 0};
        }
    }
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    return iid;
}

/**
 * @brief FindLeafPage + upper_bound
 *
 * @param key
 * @return Iid
 */
Iid IxIndexHandle::upper_bound(const char *key) {
    std::scoped_lock lock{root_latch_};
    IxNodeHandle *node = find_leaf_page(key, Operation::FIND, nullptr).first;
    int key_idx = node->upper_bound(key);
    Iid iid = {.page_no = node->get_page_no(), .slot_no = key_idx};
    if (key_idx == node->get_size()) {
        if (node->get_page_no() == file_hdr_->last_leaf_) {
            iid = leaf_end();
        } else {
            iid = {.page_no = node->get_next_leaf(), .slot_no = 0};
        }
    }
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    return iid;
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 * 用处在于可以作为IxScan的最后一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_end() const {
    IxNodeHandle *node = fetch_node(file_hdr_->last_leaf_);
    Iid iid = {.page_no = file_hdr_->last_leaf_, .slot_no = node->get_size()};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    return iid;
}

/**
 * @brief 指向第一个叶子的第一个结点
 * 用处在于可以作为IxScan的第一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_begin() const {
    Iid iid = {.page_no = file_hdr_->first_leaf_, .slot_no = 0};
    return iid;
}

/**
 * @brief 获取一个指定结点
 *
 * @param page_no
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 */
IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);

    return node;
}

/**
 * @brief 创建一个新结点
 *
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 * 注意：对于Index的处理是，删除某个页面后，认为该被删除的页面是free_page
 * 而first_free_page实际上就是最新被删除的页面，初始为IX_NO_PAGE
 * 在最开始插入时，一直是create node，那么first_page_no一直没变，一直是IX_NO_PAGE
 * 与Record的处理不同，Record将未插入满的记录页认为是free_page
 */
IxNodeHandle *IxIndexHandle::create_node() {
    file_hdr_->num_pages_++;

    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    // 从3开始分配page_no，第一次分配之后，new_page_id.page_no=3，file_hdr_.num_pages=4
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    auto *node = new IxNodeHandle(file_hdr_, page);
    node->page_hdr->next_free_page_no = IX_NO_PAGE;
    node->page_hdr->parent = IX_NO_PAGE;
    node->page_hdr->num_key = 0;
    node->page_hdr->is_leaf = true;
    node->page_hdr->prev_leaf = IX_NO_PAGE;
    node->page_hdr->next_leaf = IX_NO_PAGE;
    return node;
}

/**
 * @brief 从node开始更新其父节点的第一个key，一直向上更新直到根节点
 *
 * @param node
 */
void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    IxNodeHandle *curr = node;
    while (curr->get_parent_page_no() != IX_NO_PAGE) {
        // Load its parent
        IxNodeHandle *parent = fetch_node(curr->get_parent_page_no());
        int rank = parent->find_child(curr);
        char *parent_key = parent->get_key(rank);
        char *child_first_key = curr->get_key(0);
        if (memcmp(parent_key, child_first_key, file_hdr_->col_tot_len_) == 0) {
            assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
            break;
        }
        memcpy(parent_key, child_first_key, file_hdr_->col_tot_len_);  // 修改了parent node
        curr = parent;

        assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
    }
}

/**
 * @brief 要删除leaf之前调用此函数，更新leaf前驱结点的next指针和后继结点的prev指针
 *
 * @param leaf 要删除的leaf
 */
void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->is_leaf_page());

    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);

    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());  // 注意此处是SetPrevLeaf()
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
}

/**
 * @brief 删除node时，更新file_hdr_.num_pages
 *
 * @param node
 */
void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    file_hdr_->num_pages_--;
}

/**
 * @brief 将node的第child_idx个孩子结点的父节点置为node
 */
void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        // Current node is inner node, load its child and set its parent to current node
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
    }
}
