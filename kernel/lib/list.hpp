#pragma once

#include <lib/types.hpp>

// Intrusive doubly-linked list (same idea as Linux's list_head): the links
// live inside the objects, so adding/removing never allocates. An unlinked
// node points to itself.

struct ListNode {
    ListNode* prev;
    ListNode* next;

    constexpr ListNode() : prev(this), next(this) {}
    ListNode(const ListNode&) = delete;
    ListNode& operator=(const ListNode&) = delete;

    bool linked() const { return next != this; }

    void insert_after(ListNode* pos) {
        prev = pos;
        next = pos->next;
        pos->next->prev = this;
        pos->next = this;
    }

    void unlink() {
        prev->next = next;
        next->prev = prev;
        prev = next = this;
    }
};

template <typename T, ListNode T::*Member>
class IntrusiveList {
public:
    constexpr IntrusiveList() = default;
    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;

    bool empty() const { return !m_head.linked(); }
    usize size() const { return m_size; }

    T* front() const { return empty() ? nullptr : from_node(m_head.next); }
    T* back() const { return empty() ? nullptr : from_node(m_head.prev); }

    void push_back(T* item) { insert_after(item, m_head.prev); }
    void push_front(T* item) { insert_after(item, &m_head); }

    // Insert `item` right before `pos` (pos == nullptr means at the end).
    void insert_before(T* item, T* pos) {
        ListNode* p = pos ? &(pos->*Member) : &m_head;
        insert_after(item, p->prev);
    }

    T* pop_front() {
        T* item = front();
        if (item)
            remove(item);
        return item;
    }

    void remove(T* item) {
        (item->*Member).unlink();
        m_size--;
    }

    T* prev_of(T* item) const {
        ListNode* n = (item->*Member).prev;
        return n == &m_head ? nullptr : from_node(n);
    }

    T* next_of(T* item) const {
        ListNode* n = (item->*Member).next;
        return n == &m_head ? nullptr : from_node(n);
    }

    class Iterator {
    public:
        Iterator(ListNode* node, const ListNode* head) : m_node(node), m_head(head) {}
        T* operator*() const { return from_node(m_node); }
        Iterator& operator++() {
            m_node = m_node->next;
            return *this;
        }
        bool operator!=(const Iterator& other) const { return m_node != other.m_node; }

    private:
        ListNode* m_node;
        const ListNode* m_head;
    };

    // Don't remove items while iterating with these; use front()/next_of().
    Iterator begin() { return Iterator(m_head.next, &m_head); }
    Iterator end() { return Iterator(&m_head, &m_head); }

    static T* from_node(ListNode* node) {
        return reinterpret_cast<T*>(reinterpret_cast<u8*>(node) - member_offset());
    }

private:
    static usize member_offset() {
        // Offset of the member inside T, computed from the member pointer.
        alignas(T) static u8 probe_storage[sizeof(T)];
        auto* probe = reinterpret_cast<T*>(probe_storage);
        return reinterpret_cast<u8*>(&(probe->*Member)) - reinterpret_cast<u8*>(probe);
    }

    void insert_after(T* item, ListNode* pos) {
        (item->*Member).insert_after(pos);
        m_size++;
    }

    ListNode m_head;
    usize m_size = 0;
};
