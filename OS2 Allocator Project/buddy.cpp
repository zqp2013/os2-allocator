#include "buddy.h"
#include "slab.h"
#include "page.h"
#include "macros.h"
#include <cmath>
#include <iostream>
using namespace std;


buddy::buddy(void * space, unsigned long long size) {
	new(&this->spinlock) recursive_mutex();
	int neededSpace = 1;
	while ((sizeof(buddy) + sizeof(page) * (size - neededSpace) + sizeof(list_head) * ((int)log2(size - neededSpace)+1)) > BLOCK_SIZE*neededSpace) {
		neededSpace++;
	}
	size -= neededSpace;
	this->usable = (int)size;
	this->space = (void*)((unsigned long long)space + neededSpace * BLOCK_SIZE);
	this->pagesBase = new((void*)((unsigned long long)space + sizeof(buddy))) page[size];
	for (int i = 0; i < size; i++) {
		pagesBase[i].init_page();
	}
	this->maxBlock = (int)log2(size) + 1;
	void * tmp = this->space;
	
	this->avail = new((void*)((unsigned long long)space + sizeof(buddy) + sizeof(page) * (size))) list_head[maxBlock];
	for (int i = maxBlock - 1; i >= 0; i--) {
		if ((size >> i ) &1) {
			avail[i].next = avail[i].prev = (list_head*)tmp;
			unsigned long long index = ((unsigned long long) tmp - (unsigned long long) (this->space)) >> (unsigned long long)log2(BLOCK_SIZE);
			pagesBase[index].order = i;
			((list_head*)tmp)->next = nullptr;
			((list_head*)tmp)->prev = &(avail[i]);
			tmp = (void*)((unsigned long long) tmp + (BLOCK_SIZE << i));
		}
	}
}

void * buddy::kmem_getpages(unsigned long long order) {
	if (order < 0 || order > maxBlock) return nullptr; //ERROR
	lock_guard<recursive_mutex> guard(spinlock);
	unsigned long long bestAvail = order;
	while ((bestAvail < maxBlock) && ((avail[bestAvail].next) == nullptr)) {
		bestAvail++; //try to find best fit
	}
	if (bestAvail > maxBlock) return nullptr; //can't allocate this at the moment
	
	//remove the page from the level
	list_head* ret = avail[bestAvail].next;
	list_head* tmp = ret->next;
	avail[bestAvail].next = tmp;
	if (tmp != nullptr) {
		tmp->prev = &avail[bestAvail];
	}
	unsigned long long index = ((unsigned long long) ret - (unsigned long long) (this->space)) >> (unsigned long long) log2(BLOCK_SIZE);
	pagesBase[index].order = ~0; //so we know that this is now taken

	while (bestAvail > order) { //if we split higher order blocks to get the one we need
		bestAvail--;
		tmp = (list_head*)((unsigned long long) ret + (BLOCK_SIZE << bestAvail));
		tmp->next = nullptr;
		tmp->prev = &avail[bestAvail]; //avail[bestAvail] is surely empty, so we can put tmp at the begining
		avail[bestAvail].next = avail[bestAvail].prev = tmp;
		index = ((unsigned long long) tmp - (unsigned long long) (this->space)) >> (unsigned long long) log2(BLOCK_SIZE);
		pagesBase[index].order = (unsigned int) bestAvail;
	}
	return (void*)ret;
}

int buddy::kmem_freepages(void * from, unsigned long long order) {
	if( order < 0 || order > maxBlock || from == nullptr ) return 0; //ERROR
	lock_guard<recursive_mutex> guard(spinlock);
	list_head* tmp;
	while (true) { //while there's buddys to join
		unsigned long long mask = BLOCK_SIZE << order; // mask to figure out the buddy
		tmp = (list_head*)((unsigned long long) (this->space) + (((unsigned long long) from - (unsigned long long) (this->space)) ^ mask)); //find the adress of space's buddy
		unsigned long long index = ((unsigned long long)tmp - (unsigned long long) (this->space)) >> (unsigned long long) log2(BLOCK_SIZE); //page with this index will tell us if the buddy is free or taken (sort of like a map)
		if (index >= usable) {
			break;
		}
		if (tmp != nullptr && pagesBase[index].order == order) { //if the buddy's free
			tmp->prev->next = tmp->next;//take it out of the current buddy level
			if (tmp->next != nullptr) {
				tmp->next->prev = tmp->prev;
			}
			pagesBase[index].order = ~0; //the page isn't currently in the buddy free list
			order++; // to check for higher orders
			if ((void*)tmp < from) { //if the found buddy comes before  given buddy
				from = (void*)tmp;//adjust
			}
		} else {
			break;
		}
	}
	//put in available list
	tmp = avail[order].next;
	((list_head*)from)->next = tmp;
	if (tmp != nullptr) {
		tmp->prev = (list_head*)from;
	}
	((list_head*)from)->prev = &avail[order];
	avail[order].next = (list_head*)from;
	unsigned long long index = ((unsigned long long)from - (unsigned long long)(this->space)) >> (unsigned long long) log2(BLOCK_SIZE);
	pagesBase[index].order = (unsigned int) order; //from this page on it's free for 2^order
	return 1;
	
}
