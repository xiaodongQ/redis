/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
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

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>

#include "dict.h"
#include "zmalloc.h"
#ifndef DICT_BENCHMARK_MAIN
#include "redisassert.h"
#else
#include <assert.h>
#endif

/* Using dictEnableResize() / dictDisableResize() we make possible to
 * enable/disable resizing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio. */
// 是否允许调整大小，1为可以
// 注意：即使设置为0也不是所有调整大小都不允许，实体个数/桶个数 > 强制resize的比率 则也会进行resize进行扩容
static int dict_can_resize = 1;
static unsigned int dict_force_resize_ratio = 5;

/* -------------------------- private prototypes ---------------------------- */

static int _dictExpandIfNeeded(dict *ht);
static unsigned long _dictNextPower(unsigned long size);
static long _dictKeyIndex(dict *ht, const void *key, uint64_t hash, dictEntry **existing);
static int _dictInit(dict *ht, dictType *type, void *privDataPtr);

/* -------------------------- hash functions -------------------------------- */

static uint8_t dict_hash_function_seed[16];

// 设置hash seed
void dictSetHashFunctionSeed(uint8_t *seed) {
    memcpy(dict_hash_function_seed,seed,sizeof(dict_hash_function_seed));
}

uint8_t *dictGetHashFunctionSeed(void) {
    return dict_hash_function_seed;
}

/* The default hashing function uses SipHash implementation
 * in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

// 对key进行siphash
uint64_t dictGenHashFunction(const void *key, int len) {
    // 用 siphash 算法计算哈希
    return siphash(key,len,dict_hash_function_seed);
}

// 对key进行siphash，key的字符都变为小写再计算
uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len) {
    return siphash_nocase(buf,len,dict_hash_function_seed);
}

/* ----------------------------- API implementation ------------------------- */

/* Reset a hash table already initialized with ht_init().
 * NOTE: This function should only be called by ht_destroy(). */
static void _dictReset(dictht *ht)
{
    ht->table = NULL;
    ht->size = 0;
    ht->sizemask = 0;
    ht->used = 0;
}

// 创建一个字典总类(里面包含新旧两个哈希表，都叫容易混淆，此处自己叫字典总类吧)
/* Create a new hash table */
dict *dictCreate(dictType *type,
        void *privDataPtr)
{
    // 这种用法没见过，d定义时就能同时使用？
    dict *d = zmalloc(sizeof(*d));

    _dictInit(d,type,privDataPtr);
    return d;
}

// 初始化一个哈希表，d 已经分配好了空间
/* Initialize the hash table */
int _dictInit(dict *d, dictType *type,
        void *privDataPtr)
{
    // 两个哈希表对应字段清零
    _dictReset(&d->ht[0]);
    _dictReset(&d->ht[1]);
    d->type = type;
    d->privdata = privDataPtr;
    d->rehashidx = -1;
    d->iterators = 0;
    return DICT_OK;
}

// 调整哈希表大小，每个元素用一个桶存储(没有冲突，使装载因子接近1)
/* Resize the table to the minimal size that contains all the elements,
 * but with the invariant of a USED/BUCKETS ratio near to <= 1 */
int dictResize(dict *d)
{
    unsigned long minimal;

    // 当前不允许调整大小 或者 正在rehash，则不允许调整
    if (!dict_can_resize || dictIsRehashing(d)) return DICT_ERR;
    // 两个哈希表，默认第0个在使用
    minimal = d->ht[0].used;
    if (minimal < DICT_HT_INITIAL_SIZE)
        minimal = DICT_HT_INITIAL_SIZE;
    return dictExpand(d, minimal);
}

// 哈希表扩容或者缩容，size是实体个数(桶个数)，而不是字节大小
// 扩缩容时新的哈希表都放在第二个表中(除了第一次初始化放在第一个哈希表中)
/* Expand or create the hash table */
int dictExpand(dict *d, unsigned long size)
{
    /* the size is invalid if it is smaller than the number of
     * elements already inside the hash table */
    if (dictIsRehashing(d) || d->ht[0].used > size)
        return DICT_ERR;

    // 新的哈希表
    dictht n; /* the new hash table */
    // 获取size对应的下一个2^n值，如7对应8 (即桶个数)
    unsigned long realsize = _dictNextPower(size);

    // 要重新分配(rehash)的大小和原来一样，则退出
    /* Rehashing to the same table size is not useful. */
    if (realsize == d->ht[0].size) return DICT_ERR;

    /* Allocate the new hash table and initialize all pointers to NULL */
    n.size = realsize;
    n.sizemask = realsize-1;
    // realsize 个实体指针
    n.table = zcalloc(realsize*sizeof(dictEntry*));
    n.used = 0;

    // 第一次初始化，则把新哈希表赋值给第1个哈希表(下标为0)，然后退出
    /* Is this the first initialization? If so it's not really a rehashing
     * we just set the first hash table so that it can accept keys. */
    if (d->ht[0].table == NULL) {
        d->ht[0] = n;
        return DICT_OK;
    }

    // 赋值给第2个哈希表(新哈希表)(0为第一个)
    /* Prepare a second hash table for incremental rehashing */
    d->ht[1] = n;
    // 设置为0，说明要从起始开始进行rehash
    d->rehashidx = 0;
    return DICT_OK;
}

// 渐进式rehash，把旧表映射到新表，分成n步进行
// 返回1表示还有数据(key或者说key对应的桶)需要从旧表移动到新表，0表示没有
/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time. */
int dictRehash(dict *d, int n) {
    int empty_visits = n*10; /* Max number of empty buckets to visit. */
    if (!dictIsRehashing(d)) return 0;

    // 由n指定分多少步循环操作 [0]为旧表
    while(n-- && d->ht[0].used != 0) {
        dictEntry *de, *nextde;

        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
        assert(d->ht[0].size > (unsigned long)d->rehashidx);
        // table为许多键值数据指针构成的连续空间(即桶构成的数组)，指针为NULL则说明该位置没有元素(空桶)，跳过没有键值的位置
        while(d->ht[0].table[d->rehashidx] == NULL) {
            d->rehashidx++;
            // 限制一个最大尝试次数 如果桶为空的总次数超出限制，则本次退出
            if (--empty_visits == 0) return 1;
        }
        // 找到了第一个非空的桶
        de = d->ht[0].table[d->rehashidx];
        // 把这个桶中的所有key(同一个桶中都是冲突的key)都移动到新哈希表中，本桶所有字典节点是一个链表结构
        /* Move all the keys in this bucket from the old to the new hash HT */
        while(de) {
            uint64_t h;

            // 先保存一下本字典节点的下一个节点，用于循环迭代
            nextde = de->next;
            // 对当前这个key计算hash，然后计算其在新哈希表中的存放位置 hash & sizemask ([1]的sizemask和[0]不同，所以位置不同)
            /* Get the index in the new hash table */
            h = dictHashKey(d, de->key) & d->ht[1].sizemask;
            // 新表中的桶，位置为上面重新计算所得
            // 这一步有什么用？ 用来截断原来的链，要不它的next还是指向老链表的下一个节点，就乱套了
            de->next = d->ht[1].table[h];
            // 本节点指针赋值给新哈希表的对应位置，直接用的原始数据的指针，不需重新分配内存
            d->ht[1].table[h] = de;
            // 老表节点数减1
            d->ht[0].used--;
            // 新表节点数加1
            d->ht[1].used++;
            // 迭代下一个字典节点
            de = nextde;
        }
        // 本桶处理完了，置为NULL 表示变成空桶
        d->ht[0].table[d->rehashidx] = NULL;
        // 移到下一个将要rehash处理的桶位置
        d->rehashidx++;
    }

    // 检查是不是已经对整个哈希表都rehash完了
    /* Check if we already rehashed the whole table... */
    if (d->ht[0].used == 0) {
        // 是则释放老表的空间
        zfree(d->ht[0].table);
        // 并把新表覆盖到老表位置(dictht结构中包含表示字典实体的二级指针)，此处为浅拷贝
        d->ht[0] = d->ht[1];
        // 重置[1]中的各成员变量，不涉及内存释放，上面操作的都是指针，只涉及浅拷贝
        _dictReset(&d->ht[1]);
        // rehash结束，把处理位置置-1表示无需rehash
        d->rehashidx = -1;
        // 返回0，表示rehash结束，其他情况都返回1，表示需要进行再次处理
        return 0;
    }

    /* More to rehash... */
    return 1;
}

// 获取当前毫秒的时间
long long timeInMilliseconds(void) {
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

// 在给定时间内，循环执行rehash，每次100步
/* Rehash for an amount of time between ms milliseconds and ms+1 milliseconds */
int dictRehashMilliseconds(dict *d, int ms) {
    // iterators 标识了正在使用的迭代器个数，类比STL，如果迭代器在使用的同时进行rehash，可能造成迭代器失效
    if (d->iterators > 0) return 0;

    // 当前毫秒时间
    long long start = timeInMilliseconds();
    int rehashes = 0;

    // 进行100步rehash
    while(dictRehash(d,100)) {
        rehashes += 100;
        // 大于传入的时间间隔则停止
        if (timeInMilliseconds()-start > ms) break;
    }
    // 返回经过的rehash步数
    return rehashes;
}

// 没有迭代器使用时，进行一步rehash(因为若迭代器正在使用过程中rehash，则rehash可能造成部分元素丢失或者重复)
// 这个函数会在普通的查询或者更新哈希表操作时被调用，以便哈希表自动迁移
/* This function performs just a step of rehashing, and only if there are
 * no safe iterators bound to our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some element can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
static void _dictRehashStep(dict *d) {
    if (d->iterators == 0) dictRehash(d,1);
}

// 添加一个元素(键值对)，key已存在则返回 DICT_ERR(值为1)
/* Add an element to the target hash table */
int dictAdd(dict *d, void *key, void *val)
{
    dictEntry *entry = dictAddRaw(d,key,NULL);

    if (!entry) return DICT_ERR;
    // dictSetVal 为预定义的宏，根据字典类型对应的函数设置键值对实体的val
    dictSetVal(d, entry, val);
    return DICT_OK;
}

/* Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as he wishes.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 */
// 添加一个字典实体(键值对，dictEntry*)，但是不设置值，而是返回这个键值对结构，由用户自己来设置值
// 如果key存在则会返回NULL，并把已存在的键值对放到existing中返回；如果key不存在则返回键值对供用户操作
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing)
{
    long index;
    dictEntry *entry;
    dictht *ht;

    // 如果正在rehash，则执行一步rehash操作
    if (dictIsRehashing(d)) _dictRehashStep(d);

    /* Get the index of the new element, or -1 if
     * the element already exists. */
    if ((index = _dictKeyIndex(d, key, dictHashKey(d,key), existing)) == -1)
        return NULL;

    /* Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed
     * more frequently. */
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
    entry = zmalloc(sizeof(*entry));
    entry->next = ht->table[index];
    ht->table[index] = entry;
    ht->used++;

    /* Set the hash entry fields. */
    dictSetKey(d, entry, key);
    return entry;
}

/* Add or Overwrite:
 * Add an element, discarding the old value if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation. */
// 添加或者覆盖键值对，若为覆盖则原来的val空间会被释放
int dictReplace(dict *d, void *key, void *val)
{
    dictEntry *entry, *existing, auxentry;

    /* Try to add the element. If the key
     * does not exists dictAdd will succeed. */
    entry = dictAddRaw(d,key,&existing);
    if (entry) {
        dictSetVal(d, entry, val);
        return 1;
    }

    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    auxentry = *existing;
    // 设置val，若为复杂结构则根据具体的valDup进行复制
    dictSetVal(d, existing, val);
    // 把原来的val空间释放
    dictFreeVal(d, &auxentry);
    return 0;
}

/* Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. */
// 添加一个字典实体，dictAddRaw的简要版本，若key存在则返回已存在的key，不存在则会新增
dictEntry *dictAddOrFind(dict *d, void *key) {
    dictEntry *entry, *existing;
    entry = dictAddRaw(d,key,&existing);
    return entry ? entry : existing;
}

// 查找并移除指定key的元素(键值对)，辅助函数(dictDelete()和dictUnlink()中会调用)
// 找到后移除并返回节点指针，并根据 nofree 标志区分是否不进行空间释放(若释放则返回的是空悬指针)；没找到则返回NULL
/* Search and remove an element. This is an helper function for
 * dictDelete() and dictUnlink(), please check the top comment
 * of those functions. */
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    uint64_t h, idx;
    dictEntry *he, *prevHe;
    int table;

    // 两个哈希表已使用的键值对都为0，则不需要继续判断，直接退出
    if (d->ht[0].used == 0 && d->ht[1].used == 0) return NULL;

    // 正在rehash时，删除元素也进行一步操作，渐进式rehash分摊在各个操作(键值对添加、删除、查找)中
    if (dictIsRehashing(d)) _dictRehashStep(d);
    // 对key进行hash计算，哈希函数由对应字典类型提供
    h = dictHashKey(d, key);

    for (table = 0; table <= 1; table++) {
        // 先找到该key对应的桶(对key进行hash后和sizemask按位于)
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        prevHe = NULL;
        while(he) {
            // 找到这个key则进行移除，链表操作 prev指向要删除节点的next
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
                /* Unlink the element from the list */
                if (prevHe)
                    prevHe->next = he->next;
                else
                    d->ht[table].table[idx] = he->next;
                // nofree 为非0则指定不释放空间
                if (!nofree) {
                    // nofree为false，则释放空间
                    dictFreeKey(d, he);
                    dictFreeVal(d, he);
                    // zfree并不把he置NULL
                    zfree(he);
                }
                d->ht[table].used--;
                // 返回该key对应的节点指针，若上面释放了空间，则此处he为空悬指针(指向已经销毁的对象或已经回收的地址)
                return he;
            }
            // 没找到则继续遍历链表
            prevHe = he;
            he = he->next;
        }
        // 没有在rehash则操作第一个哈希表即可，若在则要从两个哈希表中都移除该key
        if (!dictIsRehashing(d)) break;
    }
    // 没有找到则会返回NULL
    return NULL; /* not found */
}

/* Remove an element, returning DICT_OK on success or DICT_ERR if the
 * element was not found. */
int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0) ? DICT_OK : DICT_ERR;
}

/* Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
 * Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups:
 *
 *  entry = dictFind(...);
 *  // Do something with entry
 *  dictDelete(dictionary,entry);
 *
 * Thanks to this function it is possible to avoid this, and use
 * instead:
 *
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); // <- This does not need to lookup again.
 */
// 从字典中移除一个元素，不过不释放空间(key、value、字典实体都不释放)
// 返回值会返回这个节点实体，后续可通过dictFreeUnlinkedEntry(entry)释放这个字典实体的空间
// 当我们想要从哈希表中删除某些内容，但又想在实际删除条目之前使用其值时，这个函数非常有用
dictEntry *dictUnlink(dict *ht, const void *key) {
    // nofree 送1，表示移除该节点后不释放空间
    return dictGenericDelete(ht,key,1);
}

// 释放指定实体的空间(key、value、实体都进行释放)
/* You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL. */
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
    if (he == NULL) return;
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    zfree(he);
}

// 清理整个哈希表内存(用于新或老哈希表)，callback用于清理私有数据(只有第一个哈希表才会操作释放私有数据)
/* Destroy an entire dictionary */
int _dictClear(dict *d, dictht *ht, void(callback)(void *)) {
    unsigned long i;

    /* Free all the elements */
    for (i = 0; i < ht->size && ht->used > 0; i++) {
        dictEntry *he, *nextHe;

        // 65535 16位全1，i==0才会执行该语句块
        if (callback && (i & 65535) == 0) callback(d->privdata);

        // 桶为空则跳过
        if ((he = ht->table[i]) == NULL) continue;
        // 桶不为空，则该桶中链式节点都进行清理和空间释放
        while(he) {
            // 用于链表遍历操作，提前保存
            nextHe = he->next;
            // 用对应方法清理key内存
            dictFreeKey(d, he);
            // 用对应方法清理val内存
            dictFreeVal(d, he);
            // 释放桶中该字典实体的节点空间
            zfree(he);
            ht->used--;
            he = nextHe;
        }
    }
    // 释放字典实体指针table
    /* Free the table and the allocated cache structure */
    zfree(ht->table);
    // 重新初始化哈希表
    /* Re-initialize the table */
    _dictReset(ht);
    return DICT_OK; /* never fails */
}

// 释放字典空间(释放两个哈希表和私有数据空间，并释放该字典指针)
/* Clear & Release the hash table */
void dictRelease(dict *d)
{
    _dictClear(d,&d->ht[0],NULL);
    _dictClear(d,&d->ht[1],NULL);
    zfree(d);
}

// 查找key对应的键值对实体
dictEntry *dictFind(dict *d, const void *key)
{
    dictEntry *he;
    uint64_t h, idx, table;

    if (dictSize(d) == 0) return NULL; /* dict is empty */
    // 查找时也进行一步rehash
    if (dictIsRehashing(d)) _dictRehashStep(d);
    h = dictHashKey(d, key);
    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        while(he) {
            if (key==he->key || dictCompareKeys(d, key, he->key))
                return he;
            he = he->next;
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;

    he = dictFind(d,key);
    return he ? dictGetVal(he) : NULL;
}

// 计算字典指纹(返回一个64位的整数)，反映当前字典的状态，使用字典中的一些属性进行异或操作计算得到
// 如果迭代器是不安全的，则需要检查字典指纹是否一样，不一样则禁止迭代操作
/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
long long dictFingerprint(dict *d) {
    long long integers[6], hash = 0;
    int j;

    integers[0] = (long) d->ht[0].table;
    integers[1] = d->ht[0].size;
    integers[2] = d->ht[0].used;
    integers[3] = (long) d->ht[1].table;
    integers[4] = d->ht[1].size;
    integers[5] = d->ht[1].used;

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    // 计算int类型的hash code，使用hash算法：Tomas Wang's 64 bit integer hash
        // [几种常见的hash函数](https://www.jianshu.com/p/bb64cd7593ab)
        // [Integer Hash Function](http://web.archive.org/web/20071223173210/http://www.concentric.net/~Ttwang/tech/inthash.htm)
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

// 获取一个不安全(safe为0)的字典迭代器(申请一个dictIterator类型大小，zmalloc申请时都会在前面多申请一个放长度大小的空间)
dictIterator *dictGetIterator(dict *d)
{
    dictIterator *iter = zmalloc(sizeof(*iter));

    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = 0;
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

// 获取一个安全的字典迭代器
dictIterator *dictGetSafeIterator(dict *d) {
    dictIterator *i = dictGetIterator(d);

    i->safe = 1;
    return i;
}

// 返回迭代器指向的下一个实体指针(如果当前迭代器指向空桶，则会找下一个非空的桶，返回其第一个实体)
dictEntry *dictNext(dictIterator *iter)
{
    while (1) {
        // 传入迭代器指向的实体为空，说明是空桶
        if (iter->entry == NULL) {
            // 定义指针 指向table标识的哈希表(0-旧 1-新)
            dictht *ht = &iter->d->ht[iter->table];
            // 旧哈希表
            if (iter->index == -1 && iter->table == 0) {
                if (iter->safe)
                    iter->d->iterators++; // 若是安全迭代器，则字典中正使用的迭代器数量加1
                else
                    iter->fingerprint = dictFingerprint(iter->d); // 计算当前字典的指纹，更新到迭代器中，不安全的迭代器并不更新使用数量
            }
            // 桶 索引加1 (当前指向的实体是空，说明是空桶)
            iter->index++;
            // 超过桶总数了
            if (iter->index >= (long) ht->size) {
                // 若正在rehash且迭代器指向旧哈希表
                if (dictIsRehashing(iter->d) && iter->table == 0) {
                    // 把迭代器中的哈希表 改成 新的哈希表
                    iter->table++;
                    // 索引指向第1个位置(下标为0，若没有使用则下标为-1)
                    iter->index = 0;
                    // 指针指向也改成新哈希表
                    ht = &iter->d->ht[1];
                } else {
                    // 如果没有在rehash，则退出while，并返回NULL
                    break;
                }
            }
            // 新索引(原索引++之后)对应的实体(此处由于是下一个桶的第一个实体，所以和桶的指针是重合的)，更新到 迭代器指向的实体
            iter->entry = ht->table[iter->index];
        } else {
            // 如果不是空桶，则直接更新下一个实体即可(nextEntry指向的是键值对实体，而不是桶)
            iter->entry = iter->nextEntry;
        }
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            // 更新下一个实体指针(键值对结构中的next，指向桶里面的链表下一个成员)
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }
    return NULL;
}

// 释放迭代器
void dictReleaseIterator(dictIterator *iter)
{

    if (!(iter->index == -1 && iter->table == 0)) {
        if (iter->safe)
            iter->d->iterators--; // 如果是安全迭代器，则将正使用的迭代器数量 减1
        else
            assert(iter->fingerprint == dictFingerprint(iter->d)); // 非安全迭代器，需要当前计算的指纹和之前一样，否则退出
    }
    // 释放迭代器内存
    zfree(iter);
}

// 返回一个随机实体，在实现随机算法时比较有用
/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
dictEntry *dictGetRandomKey(dict *d)
{
    dictEntry *he, *orighe;
    unsigned long h;
    int listlen, listele;

    if (dictSize(d) == 0) return NULL;
    // 进行一步rehash
    if (dictIsRehashing(d)) _dictRehashStep(d);
    // 找一个非空的桶
    if (dictIsRehashing(d)) {
        // 如果在rehash，则随机桶是从 旧表的rehashidx 到 新表的结束 之间返回
        do {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            h = d->rehashidx + (random() % (dictSlots(d) - d->rehashidx));
            he = (h >= d->ht[0].size) ? d->ht[1].table[h - d->ht[0].size] :
                                      d->ht[0].table[h];
        } while(he == NULL);
    } else {
        do {
            h = random() & d->ht[0].sizemask;
            he = d->ht[0].table[h];
        } while(he == NULL);
    }

    /* Now we found a non empty bucket, but it is a linked
     * list and we need to get a random element from the list.
     * The only sane way to do so is counting the elements and
     * select a random index. */
    // 从非空的桶里返回一个随机的节点(先遍历计算总的节点数，再生成随机位置，返回对应实体节点)
    listlen = 0;
    orighe = he;
    while(he) {
        he = he->next;
        listlen++;
    }
    listele = random() % listlen;
    he = orighe;
    while(listele--) he = he->next;
    return he;
}

/* This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements. */
// 从字典中返回一些(count指定数量)随机的实体，保存在des中(二级指针，用来表示实体指针的数组)
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned long j; /* internal hash table id, 0 or 1. */
    unsigned long tables; /* 1 or 2 tables? */
    unsigned long stored = 0, maxsizemask;
    unsigned long maxsteps;

    if (dictSize(d) < count) count = dictSize(d);
    maxsteps = count*10;

    /* Try to do a rehashing work proportional to 'count'. */
    for (j = 0; j < count; j++) {
        if (dictIsRehashing(d))
            _dictRehashStep(d);
        else
            break;
    }

    tables = dictIsRehashing(d) ? 2 : 1;
    maxsizemask = d->ht[0].sizemask;
    if (tables > 1 && maxsizemask < d->ht[1].sizemask)
        maxsizemask = d->ht[1].sizemask;

    /* Pick a random point inside the larger table. */
    unsigned long i = random() & maxsizemask;
    unsigned long emptylen = 0; /* Continuous empty entries so far. */
    while(stored < count && maxsteps--) {
        for (j = 0; j < tables; j++) {
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            if (tables == 2 && j == 0 && i < (unsigned long) d->rehashidx) {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                if (i >= d->ht[1].size)
                    i = d->rehashidx;
                else
                    continue;
            }
            if (i >= d->ht[j].size) continue; /* Out of range for this table. */
            dictEntry *he = d->ht[j].table[i];

            /* Count contiguous empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            if (he == NULL) {
                emptylen++;
                if (emptylen >= 5 && emptylen > count) {
                    i = random() & maxsizemask;
                    emptylen = 0;
                }
            } else {
                emptylen = 0;
                while (he) {
                    /* Collect all the elements of the buckets found non
                     * empty while iterating. */
                    *des = he;
                    des++;
                    he = he->next;
                    stored++;
                    if (stored == count) return stored;
                }
            }
        }
        i = (i+1) & maxsizemask;
    }
    return stored;
}

/* This is like dictGetRandomKey() from the POV of the API, but will do more
 * work to ensure a better distribution of the returned element.
 *
 * This function improves the distribution because the dictGetRandomKey()
 * problem is that it selects a random bucket, then it selects a random
 * element from the chain in the bucket. However elements being in different
 * chain lengths will have different probabilities of being reported. With
 * this function instead what we do is to consider a "linear" range of the table
 * that may be constituted of N buckets with chains of different lengths
 * appearing one after the other. Then we report a random element in the range.
 * In this way we smooth away the problem of different chain lenghts. */
// 和dictGetRandomKey类似，返回一个随机实体，不过它更公平(dictGetRandomKey中先获取随机桶再获取桶里随机节点，不过不同链长度概率不同)
#define GETFAIR_NUM_ENTRIES 15
dictEntry *dictGetFairRandomKey(dict *d) {
    dictEntry *entries[GETFAIR_NUM_ENTRIES];
    unsigned int count = dictGetSomeKeys(d,entries,GETFAIR_NUM_ENTRIES);
    /* Note that dictGetSomeKeys() may return zero elements in an unlucky
     * run() even if there are actually elements inside the hash table. So
     * when we get zero, we call the true dictGetRandomKey() that will always
     * yeld the element if the hash table has at least one. */
    if (count == 0) return dictGetRandomKey(d);
    unsigned int idx = rand() % count;
    return entries[idx];
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static unsigned long rev(unsigned long v) {
    unsigned long s = CHAR_BIT * sizeof(v); // bit size; must be power of 2
    unsigned long mask = ~0UL;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times. 部分元素可能返回多次
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the dictionary entry
 * 'de' as second argument.
 *
 * HOW IT WORKS.
 *
 * 主要的想法是 递增一个游标(cursor)
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * 需要这个策略是因为哈希表在遍历时可能会 resize
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 */
// 遍历字典的所有元素
/*
 * 该算法使用游标 cursor 来遍历字典，关于cursor可以看上面的注释
 * cursor的演变是采用了reverse binary iteration方法，也就是每次是向cursor的最高位加1，并向低位方向进位
 * 比如1101的下一个数是0011，因为1101的前三个数为110，最高位加1，并且向低位进位就是001，所以最终得到0011
 *  高位加1后，若要进位，则向低位进位，而不是往高位进位
 * 
 * 在Redis中，字典的哈希表长度始终为2的n次方。因此m0(sizemask)始终是一个低n位全为1，其余为全为0的数
 * 整个计算过程，都是在v的低n位数中进行的，比如长度为16的哈希表，则n=4，因此v是从0到15这几个数之间的转换
 * 
 * 计算一个哈希表节点索引的方法是hashkey&mask，其中，mask的值永远是哈希表大小减1
 * 哈希表长度为8，则mask为111，因此，节点的索引值就取决于hashkey的低三位
 * 
 * 算法比较复杂，先mark。。
 * 
 * 可参考[Redis源码解析：04字典的遍历dictScan](https://blog.csdn.net/gqtcgq/article/details/50533336)
*/
unsigned long dictScan(dict *d,
                       unsigned long v,
                       dictScanFunction *fn,
                       dictScanBucketFunction* bucketfn,
                       void *privdata)
{
    dictht *t0, *t1;
    const dictEntry *de, *next;
    unsigned long m0, m1;

    // 总实体数为0则说明返回
    if (dictSize(d) == 0) return 0;

    /* Having a safe iterator means no rehashing can happen, see _dictRehashStep.
     * This is needed in case the scan callback tries to do dictFind or alike. */
    // 使用安全迭代器数量+1，该数量>0时不能进行rehash渐进操作(为0再进行每步渐进rehash)
    d->iterators++;

    if (!dictIsRehashing(d)) {
        // 没有在rehash则直接用旧哈希表
        t0 = &(d->ht[0]);
        m0 = t0->sizemask;

        /* Emit entries at cursor */
        // bucketfn 为传入的遍历桶的函数指针
        // v 为哈希值? 计算得到当前哈希值所在的桶位置
        if (bucketfn) bucketfn(privdata, &t0->table[v & m0]);
        // 桶位置
        de = t0->table[v & m0];
        // 遍历桶对应的链表节点
        while (de) {
            next = de->next;
            // fn 为传入的字典遍历函数指针
            fn(privdata, de);
            de = next;
        }

        // 用于保留v的低n位数(m0的低n位都是1，取反变为 110000 形式(假设低4位))，其余位全置为1
        /* Set unmasked bits so incrementing the reversed cursor
         * operates on the masked bits */
        v |= ~m0;

        /* Increment the reverse cursor */
        // rev函数用来对无符号整数进行二进制位的翻转
        v = rev(v);
        v++;
        v = rev(v);

    } else {
        t0 = &d->ht[0];
        t1 = &d->ht[1];

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (t0->size > t1->size) {
            t0 = &d->ht[1];
            t1 = &d->ht[0];
        }

        m0 = t0->sizemask;
        m1 = t1->sizemask;

        /* Emit entries at cursor */
        if (bucketfn) bucketfn(privdata, &t0->table[v & m0]);
        de = t0->table[v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        do {
            /* Emit entries at cursor */
            if (bucketfn) bucketfn(privdata, &t1->table[v & m1]);
            de = t1->table[v & m1];
            while (de) {
                next = de->next;
                fn(privdata, de);
                de = next;
            }

            /* Increment the reverse cursor not covered by the smaller mask.*/
            v |= ~m1;
            v = rev(v);
            v++;
            v = rev(v);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    /* undo the ++ at the top */
    // 使用完安全迭代器，进行-1
    d->iterators--;

    return v;
}

/* ------------------------- private functions ------------------------------ */

// 哈希表是否需要扩容，若需要则进行扩容
/* Expand the hash table if needed */
static int _dictExpandIfNeeded(dict *d)
{
    // 正在rehash则直接返回ok (0)
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d)) return DICT_OK;

    // 如果哈希表为空，则初始化为4个桶
    /* If the hash table is empty expand it to the initial size. */
    if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    if (d->ht[0].used >= d->ht[0].size &&
        (dict_can_resize ||
         d->ht[0].used/d->ht[0].size > dict_force_resize_ratio)) // 字典节点个数 / 总桶数，即装载因子
    {
        return dictExpand(d, d->ht[0].used*2);
    }
    return DICT_OK;
}

// 获取 size 值对应的下一个 2^n 值
/* Our hash table capability is a power of two */
static unsigned long _dictNextPower(unsigned long size)
{
    unsigned long i = DICT_HT_INITIAL_SIZE;

    if (size >= LONG_MAX) return LONG_MAX + 1LU;
    while(1) {
        if (i >= size)
            return i;
        i *= 2;
    }
}

// 返回一个空闲桶的索引，该空闲桶可以用给定“键”的哈希条目填充
// 如果key已经存在，则返回-1，并把已存在的键值对放到existing出参中
// 如果正在rehash，则每次返回的是第二个哈希表的内容(新表)
/* Returns the index of a free slot that can be populated with
 * a hash entry for the given 'key'.
 * If the key already exists, -1 is returned
 * and the optional output parameter may be filled.
 *
 * Note that if we are in the process of rehashing the hash table, the
 * index is always returned in the context of the second (new) hash table. */
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing)
{
    unsigned long idx, table;
    dictEntry *he;
    if (existing) *existing = NULL;

    // 获取前先检查是否进行扩缩容，若需扩缩容但执行失败，则返回失败
    /* Expand the hash table if needed */
    if (_dictExpandIfNeeded(d) == DICT_ERR)
        return -1;
    // 两个哈希表
    for (table = 0; table <= 1; table++) {
        // 传入的hash值 在哈希表中的位置
        idx = hash & d->ht[table].sizemask;
        // 根据上面的位置取字典实体
        /* Search if this slot does not already contain the given key */
        he = d->ht[table].table[idx];
        // he不为NULL则表示该hash值(hash值相同不一定代表key相同)原来就存在于哈希表
        while(he) {
            // 比较key是否相等，判断传入的key(而不是hash值)是否已存在
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
                if (existing) *existing = he;
                return -1;
            }
            // 不是相同的key，则说明是哈希冲突，即不同key进行hash后hash值相同，继续判断链表下一个节点
            he = he->next;
        }
        // 不在rehash则break循环，使用第一个哈希表(老表)的位置
        if (!dictIsRehashing(d)) break;
    }
    return idx;
}

// 清理字典，传入的回调函数指针用来清理私有数据
void dictEmpty(dict *d, void(callback)(void*)) {
    // 清理整个哈希表内存(用于新或老哈希表)，callback用于清理私有数据(只有第一个哈希表才会操作释放私有数据)
    _dictClear(d,&d->ht[0],callback);
    _dictClear(d,&d->ht[1],callback);
    d->rehashidx = -1;
    d->iterators = 0;
}

// 设置全局的 dict_can_resize，标识是否允许resize
void dictEnableResize(void) {
    dict_can_resize = 1;
}

void dictDisableResize(void) {
    dict_can_resize = 0;
}

// 计算key的hash值
uint64_t dictGetHash(dict *d, const void *key) {
    return dictHashKey(d, key);
}

/* Finds the dictEntry reference by using pointer and pre-calculated hash.
 * oldkey is a dead pointer and should not be accessed.
 * the hash value should be provided using dictGetHash.
 * no string / key comparison is performed.
 * return value is the reference to the dictEntry if found, or NULL if not found. */
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash) {
    dictEntry *he, **heref;
    unsigned long idx, table;

    if (dictSize(d) == 0) return NULL; /* dict is empty */
    for (table = 0; table <= 1; table++) {
        idx = hash & d->ht[table].sizemask;
        heref = &d->ht[table].table[idx];
        he = *heref;
        while(he) {
            if (oldptr==he->key)
                return heref;
            heref = &he->next;
            he = *heref;
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

/* ------------------------------- Debugging ---------------------------------*/

// 获取指定哈希表的状态信息(实体个数、桶个数、非空桶个数、平均每个非空桶的实体数量、不同实体个数对应的桶数量等信息)
#define DICT_STATS_VECTLEN 50
size_t _dictGetStatsHt(char *buf, size_t bufsize, dictht *ht, int tableid) {
    // slots 非空桶的个数
    // maxchainlen 单个桶中包含实体的最大个数(链的最大长度)
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    // 总的实体个数
    unsigned long totchainlen = 0;
    // 用来记录 不同链长度各自对应的桶个数，如下标为0的值表示实体个数为0的桶有多少个
    unsigned long clvector[DICT_STATS_VECTLEN];
    size_t l = 0;

    // 实体数量为0
    if (ht->used == 0) {
        return snprintf(buf,bufsize,
            "No stats available for empty dictionaries\n");
    }

    /* Compute stats. */
    for (i = 0; i < DICT_STATS_VECTLEN; i++) clvector[i] = 0;
    // 遍历所有桶
    for (i = 0; i < ht->size; i++) {
        dictEntry *he;

        if (ht->table[i] == NULL) {
            // 空桶数量+1
            clvector[0]++;
            continue;
        }
        // 非空桶数量+1
        slots++;
        /* For each hash entry on this slot... */
        // 这个桶中的实体个数(冲突链长度)
        chainlen = 0;
        he = ht->table[i];
        while(he) {
            chainlen++;
            he = he->next;
        }
        // 如果实体个数>=50，则都算到下标为49的个数中
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN-1)]++;
        // 更新单个桶中包含实体的最大个数(链的最大长度)
        if (chainlen > maxchainlen) maxchainlen = chainlen;
        // 总的实体个数
        totchainlen += chainlen;
    }

    /* Generate human readable stats. */
    l += snprintf(buf+l,bufsize-l,
        "Hash table %d stats (%s):\n"
        " table size: %ld\n"
        " number of elements: %ld\n"
        " different slots: %ld\n"
        " max chain length: %ld\n"
        " avg chain length (counted): %.02f\n"  // 平均每个非空桶的实体数量(用叠加计算出的总实体个数计算)
        " avg chain length (computed): %.02f\n" // 平均每个非空桶的实体数量(用哈希表中的used计算)
        " Chain length distribution:\n",
        tableid, (tableid == 0) ? "main hash table" : "rehashing target",
        ht->size, ht->used, slots, maxchainlen,
        (float)totchainlen/slots, (float)ht->used/slots);

    for (i = 0; i < DICT_STATS_VECTLEN-1; i++) {
        if (clvector[i] == 0) continue;
        if (l >= bufsize) break;
        // 打印 每个链长度 对应的桶个数
        l += snprintf(buf+l,bufsize-l,
            "   %s%ld: %ld (%.02f%%)\n",
            (i == DICT_STATS_VECTLEN-1)?">= ":"",
            i, clvector[i], ((float)clvector[i]/ht->size)*100);
    }

    /* Unlike snprintf(), teturn the number of characters actually written. */
    if (bufsize) buf[bufsize-1] = '\0';
    return strlen(buf);
}

// 获取新旧两个哈希表的状态信息((实体个数、桶个数、非空桶个数、平均每个非空桶的实体数量、不同实体个数对应的桶数量等信息))
void dictGetStats(char *buf, size_t bufsize, dict *d) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    l = _dictGetStatsHt(buf,bufsize,&d->ht[0],0);
    buf += l;
    bufsize -= l;
    if (dictIsRehashing(d) && bufsize > 0) {
        _dictGetStatsHt(buf,bufsize,&d->ht[1],1);
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize-1] = '\0';
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef DICT_BENCHMARK_MAIN

#include "sds.h"

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

int compareCallback(void *privdata, const void *key1, const void *key2) {
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(void *privdata, void *val) {
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

dictType BenchmarkDictType = {
    hashCallback,
    NULL,
    NULL,
    compareCallback,
    freeCallback,
    NULL
};

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg) do { \
    elapsed = timeInMilliseconds()-start; \
    printf(msg ": %ld items in %lld ms\n", count, elapsed); \
} while(0);

/* dict-benchmark [count] */
int main(int argc, char **argv) {
    long j;
    long long start, elapsed;
    dict *dict = dictCreate(&BenchmarkDictType,NULL);
    long count = 0;

    if (argc == 2) {
        count = strtol(argv[1],NULL,10);
    } else {
        count = 5000000;
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        int retval = dictAdd(dict,sdsfromlonglong(j),(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    assert((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict)) {
        dictRehashMilliseconds(dict,100);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(rand() % count);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict,key);
        assert(de == NULL);
        sdsfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        int retval = dictDelete(dict,key);
        assert(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict,key,(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
}
#endif
