/* BD19 SDK linked-list primitives (common btcontroller_linked_list.c). */
#ifdef CONFIG_CPU_BD19
#include "btctrler/glue.h"

int btctler_linked_list_empty(btctler_linked_item_t **list)
{
    return *list == 0;
}

btctler_linked_item_t *btctler_linked_list_get_last_item(btctler_linked_item_t **list)
{
    btctler_linked_item_t *item = *list;
    if (!item) return 0;
    while (item->next) item = item->next;
    return item;
}

void btctler_linked_list_add(btctler_linked_item_t **list, btctler_linked_item_t *item)
{
    btctler_linked_item_t *cur = *list;
    while (cur) {
        if (cur == item) return;
        cur = cur->next;
    }
    item->next = *list;
    *list = item;
}

void btctler_linked_list_add_tail(btctler_linked_item_t **list, btctler_linked_item_t *item)
{
    btctler_linked_item_t *prev = (btctler_linked_item_t *)list;
    btctler_linked_item_t *cur;
    while ((cur = prev->next) != 0) {
        if (cur == item) return;
        prev = cur;
    }
    item->next = 0;
    prev->next = item;
}

int btctler_linked_list_remove(btctler_linked_item_t **list, btctler_linked_item_t *item)
{
    if (!item) return -1;
    btctler_linked_item_t *prev = (btctler_linked_item_t *)list;
    while (prev && prev->next != item) prev = prev->next;
    if (!prev) return -1;
    prev->next = item->next;
    return 0;
}

int btctler_linked_list_count(btctler_linked_item_t **list)
{
    int count = 0;
    for (btctler_linked_item_t *item = (btctler_linked_item_t *)list; item->next; item = item->next)
        count++;
    return count;
}

btctler_linked_item_t *btctler_linked_list_get_first_item(btctler_linked_item_t **list)
{
    return *list;
}

btctler_linked_item_t *btctler_linked_list_pop(btctler_linked_item_t **list)
{
    btctler_linked_item_t *item = *list;
    if (item) *list = item->next;
    return item;
}

void btctler_linked_list_iterator_init(btctler_linked_list_iterator_t *it, btctler_linked_item_t **list)
{
    it->advance_on_next = 0;
    it->prev = (btctler_linked_item_t *)list;
    it->curr = *list;
}

int btctler_linked_list_iterator_has_next(btctler_linked_list_iterator_t *it)
{
    btctler_linked_item_t *next;
    if (!it->advance_on_next) next = it->curr;
    else {
        next = it->prev->next;
        if (next == it->curr) next = next->next;
    }
    return next != 0;
}

btctler_linked_item_t *btctler_linked_list_iterator_next(btctler_linked_list_iterator_t *it)
{
    btctler_linked_item_t *next;
    if (it->advance_on_next) {
        next = it->prev->next;
        if (next == it->curr) {
            it->prev = next;
            next = next->next;
        }
        it->curr = next;
    } else {
        it->advance_on_next = 1;
        next = it->curr;
    }
    return next;
}

void btctler_linked_list_iterator_remove(btctler_linked_list_iterator_t *it)
{
    btctler_linked_item_t *next = it->curr->next;
    it->curr = next;
    it->prev->next = next;
    it->advance_on_next = 0;
}
#endif

#ifdef UNITTEST
#ifndef CONFIG_CPU_BD19
int main(void) { return 0; }
#else
#include "test.h"

int main(void)
{
    btctler_linked_item_t a = {0}, b = {0}, c = {0};
    btctler_linked_item_t *head = 0;
    CHECK(btctler_linked_list_empty(&head));
    btctler_linked_list_add(&head, &a);
    btctler_linked_list_add(&head, &b);
    btctler_linked_list_add(&head, &a); /* duplicate add is a no-op */
    btctler_linked_list_add_tail(&head, &c);
    btctler_linked_list_add_tail(&head, &a); /* duplicate tail is a no-op */
    CHECK(head == &b && b.next == &a && a.next == &c && c.next == 0);
    CHECK(btctler_linked_list_count(&head) == 3);
    CHECK(btctler_linked_list_get_first_item(&head) == &b);
    CHECK(btctler_linked_list_get_last_item(&head) == &c);

    btctler_linked_list_iterator_t it;
    btctler_linked_list_iterator_init(&it, &head);
    CHECK(btctler_linked_list_iterator_has_next(&it));
    CHECK(btctler_linked_list_iterator_next(&it) == &b);
    CHECK(btctler_linked_list_iterator_next(&it) == &a);
    btctler_linked_list_iterator_remove(&it);
    CHECK(head == &b && b.next == &c && btctler_linked_list_count(&head) == 2);
    CHECK(btctler_linked_list_iterator_has_next(&it));
    CHECK(btctler_linked_list_iterator_next(&it) == &c);
    CHECK(!btctler_linked_list_iterator_has_next(&it));

    CHECK(btctler_linked_list_remove(&head, &c) == 0 && b.next == 0);
    CHECK(btctler_linked_list_remove(&head, &a) == -1);
    CHECK(btctler_linked_list_pop(&head) == &b && head == 0);
    CHECK(btctler_linked_list_pop(&head) == 0);
    CHECK(btctler_linked_list_empty(&head));
    return TEST_DONE("btctrler_linked_list_bd19");
}
#endif
#endif
