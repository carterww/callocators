/*
 * Author: Carter Williams
 * Email: carterww@hotmail.com
 * Date: 2024-06-17
 * License: MIT
 *
 * This project will be contained in a single header file. The scope
 * of this library is simply to provide a mechanism for implementing linked
 * lists in C. This library was heavily inspired by the Linux kernel's linked
 * list implementation, and I wanted to provide a similar interface for my own
 * projects.
 *
 * The library will provide two types of linked lists: singly linked lists and
 * doubly linked lists. It may be simpler to provide only doubly linked lists,
 * but I did not want force the user to take on unnecessary overhead if they only
 * needed a singly linked list. Functions or macros that are denoted with an 's'
 * at the beginning of the name are for singly linked lists. Functions or macros
 * that are denoted with a 'd' at the beginning of the name are for doubly linked
 * lists. If neither of these prefixes are present, the function or macro is
 * intended to be used for both types of linked lists.
 *
 * Both types of linked lists will be circular. This means that the last node in
 * the list will point to the first node in the list. This is done because there
 * is almost no downside to having a circular list, but there can be many upsides.
 *
 * For a detailed explanation of the functions and macros, see the comments
 * in this file. For an overview of the library with short examples, see the Usage
 * section in the README.md file.
 */
#ifndef KETTE_H
#define KETTE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* 
 * Singly linked list node. Add this struct as a member to your struct
 * that you'd like to form into a linked list. See example.c on how this
 * is done.
 */
struct slink {
	struct slink *next;
};

/*
 * Doubly linked list node. Similar to the singly linked list node in every
 * aspect except the prev pointer.
 */
struct dlink {
	struct dlink *next;
	struct dlink *prev;
};

/*
 * Macro for getting the pointer to the struct that contains the
 * linked list node. This allows the user to get the pointer to the
 * container struct from the linked list node.
 *
 * @param ptr: The pointer to the linked list node.
 * @param type: The type of the struct that contains the linked list node.
 * @param member: The name of the member in the struct that is the linked list node.
 * @expands_to: A pointer to the struct that contains the linked list node.
 */
#define list_entry(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

/*
 * Macro for iterating through a linked list. This is a helper macro for
 * list_for_each and list_for_each_reverse. This macro should not be used
 * directly.
 */
#define __list_for_each(head_ptr, entry, entry_type, entry_member, direction)  \
	for ((entry = list_entry((head_ptr)->next, entry_type, entry_member)); \
	     &((entry)->entry_member) != (head_ptr);                           \
	     entry = list_entry((entry)->entry_member.direction, entry_type,   \
				entry_member))

/*
 * Macro for iterating through a linked list. This works with singly and doubly
 * linked lists. The macro will iterate through the list by following the next
 * pointer.
 *
 * @param head_ptr: The head of the list.
 * @param entry: The variable to hold the current entry in the list.
 * @param entry_type: The type of the struct that contains the linked list entry.
 * @param entry_member: The name of the member in the struct that is the linked list entry.
 * @example: list_for_each(&node.list, entry, struct container, list) { ... }
 */
#define list_for_each(head_ptr, entry, entry_type, entry_member) \
	__list_for_each(head_ptr, entry, entry_type, entry_member, next)

/*
 * Macro for iterating through a linked list in reverse. This only works with
 * doubly linked lists. Exactly the same as list_for_each, but follows the prev
 * pointer. See list_for_each for more information.
 */
#define dlist_for_each_reverse(head_ptr, entry, entry_type, entry_member) \
	__list_for_each(head_ptr, entry, entry_type, entry_member, prev)

/*
 * Returns a non-zero value if there are no other nodes in the list.
 *
 * @param ptr: The head of the list.
 */
#define list_empty(ptr) ((ptr)->next == (ptr))

/*
 * Initialize a singly linked list.
 *
 * @param head: The head of the list.
 * @example: struct slink head = SLIST_INIT(head);
 */
#define SLIST_INIT(head) \
	{                \
		&(head)  \
	}

/*
 * Declares and initializes a singly linked list.
 *
 * @param name: The name of the list.
 * @example: SLIST_HEAD(name);
 */
#define SLIST_HEAD(name) struct slink name = SLIST_INIT(name)

/*
 * Initialize a doubly linked list.
 *
 * @param head: The head of the list.
 */
#define DLIST_INIT(head)         \
	{                        \
		&(head), &(head) \
	}

/*
 * Declares and initializes a doubly linked list.
 *
 * @param name: The name of the list.
 */
#define DLIST_HEAD(name) struct dlink name = DLIST_INIT(name)

#endif // KETTE_H

/*
 * This is a helper function used to find the previous node in a singly linked list.
 * It is used by the slist_del, slist_add_tail, and slist_splice functions. It takes
 * O(n) time to find the previous node. If this is called frequently, consider using
 * a doubly linked list.
 *
 * @param head: The head of the list.
 * @param prev: A pointer to a pointer that will hold the previous node. The previous
 * pointer is placed in this variable.
 */
static inline void slist_find_prev(struct slink *head, struct slink **prev)
{
	struct slink *next = head->next;
	*prev = head;
	while (next != head) {
		*prev = next;
		next = next->next;
	}
}

/*
 * Initialize a singly linked list to be empty. Allows for initialization after
 * compile time.
 *
 * @param head: The head of the list.
 */
static inline void slist_init(struct slink *head)
{
	head->next = head;
}

/*
 * Adds a node directly after the passed in head of the list. It takes O(1)
 * time to add a node to the list.
 *
 * @param new: The node to add to the list.
 * @param head: The head of the list.
 */
static inline void slist_add(struct slink *new, struct slink *head)
{
	struct slink *next = head->next;
	head->next = new;
	new->next = next;
}

/*
 * Adds a node to the end of the list. This takes O(n) time to add a node to the
 * list because slist_find_prev is called.
 *
 * @param new: The node to add to the list.
 * @param head: The head of the list.
 */
static inline void slist_add_tail(struct slink *new, struct slink *head)
{
	struct slink *prev;
	slist_find_prev(head, &prev);
	prev->next = new;
	new->next = head;
}

/*
 * Deletes a node from the list. This takes O(n) time to delete a node from the list
 * because slist_find_prev is called.
 *
 * @param node: The node to delete from the list.
 */
static inline void slist_del(struct slink *node)
{
	struct slink *prev;
	slist_find_prev(node, &prev);
	prev->next = node->next;
}

/*
 * Splices a list into another list. This takes O(n) time to splice the list into
 * the other list because slist_find_prev is called. Similarly to slist_add, this
 * function adds the list directly after the head.
 *
 * @param list: The list to splice into the head of the other list.
 * @param head: The head of the list to splice into.
 */
static inline void slist_splice(struct slink *list, struct slink *head)
{
	struct slink *list_tail;
	slist_find_prev(list, &list_tail);

	struct slink *head_next = head->next;
	head->next = list;
	list_tail->next = head_next;
}

/*
 * Initialize a doubly linked list to be empty. Allows for initialization after
 * compile time.
 *
 * @param head: The head of the list.
 */
static inline void dlist_init(struct dlink *head)
{
	head->next = head;
	head->prev = head;
}

/*
 * Adds a node directly after the passed in head of the list. This function
 * can be used to build a stack by adding nodes and deleting head.next.
 *
 * @param new: The node to add to the list.
 * @param head: The head of the list.
 */
static inline void dlist_add(struct dlink *new, struct dlink *head)
{
	struct dlink *next = head->next;
	// Fix old
	next->prev = new;
	head->next = new;
	// Make new
	new->next = next;
	new->prev = head;
}

/*
 * Adds a node to the end of the list. Unlike slist_add_tail, this function takes
 * O(1) time to add a node to the list. This function can be used to build a queue
 * by adding nodes to the tail and deleting head.next.
 *
 * @param new: The node to add to the list.
 * @param head: The head of the list.
 */
static inline void dlist_add_tail(struct dlink *new, struct dlink *head)
{
	dlist_add(new, head->prev);
}

/*
 * Deletes a node from the list. This takes O(1) time to delete a node from the list
 * unlike slist_del.
 *
 * @param node: The node to delete from the list.
 */
static inline void dlist_del(struct dlink *node)
{
	struct dlink *prev = node->prev;
	prev->next = node->next;
	node->next->prev = prev;
}

/*
 * Splices a list into another list. This takes O(1) time. Similarly to
 * dlist_add, this function adds the list directly after the head.
 *
 * @param list: The list to splice into the head of the other list.
 * @param head: The head of the list to splice into.
 */
static inline void dlist_splice(struct dlink *list, struct dlink *head)
{
	struct dlink *list_tail = list->prev;
	struct dlink *head_next = head->next;
	head->next = list;
	list->prev = head;

	list_tail->next = head_next;
	head_next->prev = list_tail;
}

#ifdef __cplusplus
}
#endif
