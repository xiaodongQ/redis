/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>

#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0
#define DICT_ERR 1

/* Unused arguments generate annoying warnings... */
#define DICT_NOTUSED(V) ((void) V)

// dict 字典结构，保存K-V值的结构体
// 实际键值对存放的位置，使用链表法解决哈希冲突
typedef struct dictEntry {
    void *key;
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    //下一字典结点
    struct dictEntry *next;
} dictEntry;

// dictType定义了一些字典集合操作的公共方法，不同类型实现可不同
typedef struct dictType {
    // 哈希计算方法
    uint64_t (*hashFunction)(const void *key);
    // 复制key
    void *(*keyDup)(void *privdata, const void *key);
    // 复制val
    void *(*valDup)(void *privdata, const void *obj);
    // 比较key
    int (*keyCompare)(void *privdata, const void *key1, const void *key2);
    // key的析构
    void (*keyDestructor)(void *privdata, void *key);
    // val的析构
    void (*valDestructor)(void *privdata, void *obj);
} dictType;

// 哈希表结构
/* This is our hash table structure. Every dictionary has two of this as we
 * implement incremental rehashing, for the old to the new table. */
typedef struct dictht {
    // 字典实体，dictEntry* 为一个键值数据指针，二级指针指向 键值数据指针构成的 连续内存，每个成员可能为NULL，表示没有key或者删除了原有成员
    dictEntry **table;
    // 桶数量
    unsigned long size;
    // size-1 用于计算hash值应该存放的位置 (hash & sizemask)
    unsigned long sizemask;
    // 已使用的实体数量(键值对，冲突的键值对算多个)
    unsigned long used;
} dictht;

// 字典主操作类
typedef struct dict {
    // 字典类型，通过成员操作函数区分不同操作
    dictType *type;
    // 私有数据指针
    void *privdata;
    // 2个哈希表结构，用于渐进式rehash
    dictht ht[2];
    // rehash时的下标 -1时表示未在进行rehash
    long rehashidx; /* rehashing not in progress if rehashidx == -1 */
    // 当前迭代器数量 类比STL，如果迭代器在使用的同时进行rehash，可能造成迭代器失效
    unsigned long iterators; /* number of iterators currently running */
} dict;

// 字典迭代器
/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
typedef struct dictIterator {
    // 当前字典
    dict *d;
    long index;
    int table, safe;
    // 字典实体
    dictEntry *entry, *nextEntry;
    // 指纹标记，避免不安全的迭代器滥用现象
    /* unsafe iterator fingerprint for misuse detection. */
    long long fingerprint;
} dictIterator;

// 字典扫描方法
typedef void (dictScanFunction)(void *privdata, const dictEntry *de);
// 扫描桶
typedef void (dictScanBucketFunction)(void *privdata, dictEntry **bucketref);

// 初始化哈希表的桶数目
/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_SIZE     4

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

// valDup函数不为NULL则用该函数复制val，否则直接用 = 赋值(基本类型)
#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        (entry)->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        (entry)->v.val = (_val_); \
} while(0)

#define dictSetSignedIntegerVal(entry, _val_) \
    do { (entry)->v.s64 = _val_; } while(0)

#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { (entry)->v.u64 = _val_; } while(0)

#define dictSetDoubleVal(entry, _val_) \
    do { (entry)->v.d = _val_; } while(0)

#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)

#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        (entry)->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        (entry)->key = (_key_); \
} while(0)

#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

#define dictHashKey(d, key) (d)->type->hashFunction(key)
#define dictGetKey(he) ((he)->key)
#define dictGetVal(he) ((he)->v.val)
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
#define dictGetDoubleVal(he) ((he)->v.d)
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
#define dictIsRehashing(d) ((d)->rehashidx != -1)

/* API */
// 为了和里面新旧两个哈希表区分，此处总类都叫做字典
//创建dict字典总类(包含新旧两个哈希表)
dict *dictCreate(dictType *type, void *privDataPtr);
// 字典扩缩容
int dictExpand(dict *d, unsigned long size);
// 字典新增键值对，key已存在则返回 DICT_ERR(值为1)
int dictAdd(dict *d, void *key, void *val);
// 添加一个字典实体(键值对，dictEntry*)，但是不设置值，而是返回这个键值对结构，由用户自己来设置值
// 如果key存在则会返回NULL，并把已存在的键值对放到existing中返回；如果key不存在则返回键值对供用户操作
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);
// 添加一个字典实体，dictAddRaw的简要版本，若key存在则返回已存在的key，不存在则会新增
dictEntry *dictAddOrFind(dict *d, void *key);
// 添加或者覆盖键值对，若为覆盖则原来的val空间会被释放
int dictReplace(dict *d, void *key, void *val);
// 查找并移除指定key的元素(键值对)，辅助函数(dictDelete()和dictUnlink()中会调用)
// 找到后移除并返回节点指针，并根据nofree标志区分是否不进行空间释放(若释放则返回的是空悬指针)；没找到则返回NULL
int dictDelete(dict *d, const void *key);
// 从字典中移除一个元素，不过不释放空间(key、value、字典实体都不释放)
// 返回值会返回这个节点实体，后续可通过dictFreeUnlinkedEntry(entry)释放这个字典实体的空间
// 当我们想要从哈希表中删除某些内容，但又想在实际删除条目之前使用其值时，这个函数非常有用
dictEntry *dictUnlink(dict *ht, const void *key);
// 释放指定实体的空间(key、value、实体都进行释放)
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);
// 释放字典空间(释放两个哈希表和私有数据空间，并释放该字典指针)
void dictRelease(dict *d);
// 查找key对应的键值对实体
dictEntry * dictFind(dict *d, const void *key);
// key对应的value
void *dictFetchValue(dict *d, const void *key);
// 调整哈希表，用最少的值容纳所有的字典集合
int dictResize(dict *d);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
dictEntry *dictGetFairRandomKey(dict *d);
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);
void dictGetStats(char *buf, size_t bufsize, dict *d);
uint64_t dictGenHashFunction(const void *key, int len);
uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len);
void dictEmpty(dict *d, void(callback)(void*));
void dictEnableResize(void);
void dictDisableResize(void);
// 渐进式rehash，把旧表映射到新表，分成n步进行
// 返回1表示还有数据(key或者说key对应的桶)需要从旧表移动到新表，0表示没有
int dictRehash(dict *d, int n);
// 在给定时间内，循环执行rehash，每次100步
int dictRehashMilliseconds(dict *d, int ms);
void dictSetHashFunctionSeed(uint8_t *seed);
uint8_t *dictGetHashFunctionSeed(void);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata);
uint64_t dictGetHash(dict *d, const void *key);
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash);

/* Hash table types */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#endif /* __DICT_H */
