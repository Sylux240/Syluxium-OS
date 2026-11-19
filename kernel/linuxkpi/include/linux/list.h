#ifndef _LKPI_LINUX_LIST_H
#define _LKPI_LINUX_LIST_H

/* Linux's intrusive doubly linked list, same semantics as the original. */
#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

#define LIST_HEAD_INIT(name) {&(name), &(name)}
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head* l) { l->next = l; l->prev = l; }
static inline void __list_add(struct list_head* n, struct list_head* prev, struct list_head* next) {
    next->prev = n; n->next = next; n->prev = prev; prev->next = n;
}
static inline void list_add(struct list_head* n, struct list_head* head) { __list_add(n, head, head->next); }
static inline void list_add_tail(struct list_head* n, struct list_head* head) { __list_add(n, head->prev, head); }
static inline void __list_del(struct list_head* prev, struct list_head* next) { next->prev = prev; prev->next = next; }
static inline void list_del(struct list_head* e) { __list_del(e->prev, e->next); e->next = (struct list_head*)0x100; e->prev = (struct list_head*)0x122; }
static inline void list_del_init(struct list_head* e) { __list_del(e->prev, e->next); INIT_LIST_HEAD(e); }
static inline void list_move(struct list_head* e, struct list_head* head) { __list_del(e->prev, e->next); list_add(e, head); }
static inline void list_move_tail(struct list_head* e, struct list_head* head) { __list_del(e->prev, e->next); list_add_tail(e, head); }
static inline void list_replace(struct list_head* old, struct list_head* n) { n->next = old->next; n->next->prev = n; n->prev = old->prev; n->prev->next = n; }
static inline int list_empty(const struct list_head* head) { return READ_ONCE(head->next) == head; }
static inline int list_is_last(const struct list_head* e, const struct list_head* head) { return e->next == head; }
static inline int list_is_first(const struct list_head* e, const struct list_head* head) { return e->prev == head; }
static inline int list_is_singular(const struct list_head* head) { return !list_empty(head) && head->next == head->prev; }
static inline void __list_splice(const struct list_head* list, struct list_head* prev, struct list_head* next) {
    struct list_head* first = list->next; struct list_head* last = list->prev;
    first->prev = prev; prev->next = first; last->next = next; next->prev = last;
}
static inline void list_splice(const struct list_head* list, struct list_head* head) { if (!list_empty(list)) __list_splice(list, head, head->next); }
static inline void list_splice_tail(struct list_head* list, struct list_head* head) { if (!list_empty(list)) __list_splice(list, head->prev, head); }
static inline void list_splice_init(struct list_head* list, struct list_head* head) { if (!list_empty(list)) { __list_splice(list, head, head->next); INIT_LIST_HEAD(list); } }
static inline void list_splice_tail_init(struct list_head* list, struct list_head* head) { if (!list_empty(list)) { __list_splice(list, head->prev, head); INIT_LIST_HEAD(list); } }

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member) list_entry((ptr)->prev, type, member)
#define list_first_entry_or_null(ptr, type, member) (list_empty(ptr) ? NULL : list_first_entry(ptr, type, member))
#define list_next_entry(pos, member) list_entry((pos)->member.next, __typeof__(*(pos)), member)
#define list_prev_entry(pos, member) list_entry((pos)->member.prev, __typeof__(*(pos)), member)
#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_safe(pos, n, head) for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)
#define list_entry_is_head(pos, head, member) (&(pos)->member == (head))
#define list_for_each_entry(pos, head, member) \
    for (pos = list_first_entry(head, __typeof__(*pos), member); !list_entry_is_head(pos, head, member); pos = list_next_entry(pos, member))
#define list_for_each_entry_reverse(pos, head, member) \
    for (pos = list_last_entry(head, __typeof__(*pos), member); !list_entry_is_head(pos, head, member); pos = list_prev_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = list_first_entry(head, __typeof__(*pos), member), n = list_next_entry(pos, member); \
         !list_entry_is_head(pos, head, member); pos = n, n = list_next_entry(n, member))
#define list_for_each_entry_continue(pos, head, member) \
    for (pos = list_next_entry(pos, member); !list_entry_is_head(pos, head, member); pos = list_next_entry(pos, member))
/* RCU variants: writers publish with a release barrier, readers use plain
 * loads (x86 keeps dependent loads ordered). */
static inline void list_add_rcu(struct list_head* n, struct list_head* head) {
    struct list_head* next = head->next;
    n->next = next; n->prev = head;
    __atomic_store_n(&head->next, n, __ATOMIC_RELEASE);
    next->prev = n;
}
static inline void list_add_tail_rcu(struct list_head* n, struct list_head* head) {
    struct list_head* prev = head->prev;
    n->next = head; n->prev = prev;
    __atomic_store_n(&prev->next, n, __ATOMIC_RELEASE);
    head->prev = n;
}
static inline void list_del_rcu(struct list_head* e) { __list_del(e->prev, e->next); e->prev = (struct list_head*)0x122; }
static inline void list_replace_rcu(struct list_head* old, struct list_head* n) {
    n->next = old->next; n->prev = old->prev;
    __atomic_store_n(&n->prev->next, n, __ATOMIC_RELEASE);
    n->next->prev = n;
    old->prev = (struct list_head*)0x122;
}
#define list_for_each_entry_rcu(pos, head, member, ...) list_for_each_entry(pos, head, member)
#define list_for_each_entry_srcu(pos, head, member, ...) list_for_each_entry(pos, head, member)
#define list_for_each_entry_continue_rcu(pos, head, member) list_for_each_entry_continue(pos, head, member)
#define list_entry_rcu(ptr, type, member) container_of(READ_ONCE(ptr), type, member)
#define list_first_or_null_rcu(ptr, type, member) ({ struct list_head* __h = (ptr); struct list_head* __n = READ_ONCE(__h->next); \
    __n != __h ? list_entry(__n, type, member) : NULL; })
#define list_next_rcu(list) ((list)->next)
#define list_tail_rcu(head) ((head)->prev)
#define list_splice_init_rcu(list, head, sync) list_splice_init(list, head)
static inline size_t list_count_nodes(struct list_head* head) { size_t n = 0; struct list_head* p; list_for_each(p, head) n++; return n; }

/* hlist */
#define HLIST_HEAD_INIT {NULL}
#define HLIST_HEAD(name) struct hlist_head name = {NULL}
#define INIT_HLIST_HEAD(p) ((p)->first = NULL)
static inline void INIT_HLIST_NODE(struct hlist_node* h) { h->next = NULL; h->pprev = NULL; }
static inline int hlist_unhashed(const struct hlist_node* h) { return !h->pprev; }
static inline int hlist_empty(const struct hlist_head* h) { return !READ_ONCE(h->first); }
static inline void hlist_del(struct hlist_node* n) { struct hlist_node* next = n->next; struct hlist_node** pprev = n->pprev; *pprev = next; if (next) next->pprev = pprev; }
static inline void hlist_del_init(struct hlist_node* n) { if (!hlist_unhashed(n)) { hlist_del(n); INIT_HLIST_NODE(n); } }
static inline void hlist_add_head(struct hlist_node* n, struct hlist_head* h) { struct hlist_node* first = h->first; n->next = first; if (first) first->pprev = &n->next; h->first = n; n->pprev = &h->first; }
#define hlist_entry(ptr, type, member) container_of(ptr, type, member)
#define hlist_entry_safe(ptr, type, member) ({ __typeof__(ptr) ____ptr = (ptr); ____ptr ? hlist_entry(____ptr, type, member) : NULL; })
#define hlist_for_each_entry(pos, head, member) \
    for (pos = hlist_entry_safe((head)->first, __typeof__(*(pos)), member); pos; pos = hlist_entry_safe((pos)->member.next, __typeof__(*(pos)), member))
#define hlist_for_each_entry_safe(pos, n, head, member) \
    for (pos = hlist_entry_safe((head)->first, __typeof__(*pos), member); pos && ({ n = pos->member.next; 1; }); pos = hlist_entry_safe(n, __typeof__(*pos), member))
static inline void hlist_add_head_rcu(struct hlist_node* n, struct hlist_head* h) {
    struct hlist_node* first = h->first; n->next = first; n->pprev = &h->first;
    if (first) first->pprev = &n->next;
    __atomic_store_n(&h->first, n, __ATOMIC_RELEASE);
}
static inline void hlist_del_rcu(struct hlist_node* n) { hlist_del(n); n->pprev = NULL; }
static inline void hlist_del_init_rcu(struct hlist_node* n) { if (!hlist_unhashed(n)) { hlist_del(n); n->pprev = NULL; } }
#define hlist_for_each_entry_rcu(pos, head, member, ...) hlist_for_each_entry(pos, head, member)
#define hlist_for_each_entry_rcu_bh(pos, head, member) hlist_for_each_entry(pos, head, member)
#define hlist_for_each(pos, head) for (pos = (head)->first; pos; pos = pos->next)
#define hlist_for_each_safe(pos, n, head) for (pos = (head)->first; pos && ({ n = pos->next; 1; }); pos = n)
static inline void hlist_add_before(struct hlist_node* n, struct hlist_node* next) {
    n->pprev = next->pprev; n->next = next; next->pprev = &n->next; *(n->pprev) = n;
}
static inline void hlist_add_behind(struct hlist_node* n, struct hlist_node* prev) {
    n->next = prev->next; prev->next = n; n->pprev = &prev->next; if (n->next) n->next->pprev = &n->next;
}
#define hlist_add_behind_rcu hlist_add_behind
#define hlist_add_before_rcu hlist_add_before
#define hlist_first_rcu(head) ((head)->first)
#define hlist_next_rcu(node) ((node)->next)

#define list_safe_reset_next(pos, n, member) n = list_next_entry(pos, member)
#define list_for_each_entry_from(pos, head, member) for (; !list_entry_is_head(pos, head, member); pos = list_next_entry(pos, member))
#define list_for_each_entry_safe_reverse(pos, n, head, member) \
    for (pos = list_last_entry(head, __typeof__(*pos), member), n = list_prev_entry(pos, member); \
         !list_entry_is_head(pos, head, member); pos = n, n = list_prev_entry(n, member))
#define list_for_each_prev(pos, head) for (pos = (head)->prev; pos != (head); pos = pos->prev)
#define list_prepare_entry(pos, head, member) ((pos) ? : list_entry(head, __typeof__(*pos), member))
#define list_last_entry_or_null(ptr, type, member) (list_empty(ptr) ? NULL : list_last_entry(ptr, type, member))
static inline void list_replace_init(struct list_head* old, struct list_head* n) { list_replace(old, n); INIT_LIST_HEAD(old); }
static inline int list_empty_careful(const struct list_head* head) { return head->next == head && head->prev == head; }
static inline void list_cut_position(struct list_head* list, struct list_head* head, struct list_head* entry) {
    if (list_empty(head)) return;
    if (head == entry) { INIT_LIST_HEAD(list); return; }
    struct list_head* new_first = entry->next;
    list->next = head->next; list->next->prev = list; list->prev = entry; entry->next = list;
    head->next = new_first; new_first->prev = head;
}
static inline void list_rotate_left(struct list_head* head) { if (!list_empty(head)) list_move_tail(head->next, head); }
#define list_for_each_entry_continue_reverse(pos, head, member) \
    for (pos = list_prev_entry(pos, member); !list_entry_is_head(pos, head, member); pos = list_prev_entry(pos, member))

LKPI_EXTERN_C_END

#endif
