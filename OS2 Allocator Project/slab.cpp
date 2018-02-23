#include "slab.h"
#include "slabstruct.h"
#include "list.h"
#include "page.h"
#include "buddy.h"
#include "macros.h"
#include <iostream>
#include <cstring>
#include <mutex>
using namespace std;







static kmem_cache_t cache_cache;
static kmem_cache_t* buffers[13];
buddy* bud;

static void init_cache() {
	cache_cache.slabs_free.list_init();
	cache_cache.slabs_partial.list_init();
	cache_cache.slabs_full.list_init();
	cache_cache.slabCnt = 0;
	cache_cache.objCnt = 0;
	cache_cache.objsize = sizeof(kmem_cache_t);
	cache_cache.num = ((BLOCK_SIZE - sizeof(slab)) / (sizeof(kmem_cache_t) + sizeof(unsigned int))); //calculate how much can fit on a block after reserving space for slab desc and an int for indexing
	cache_cache.gfporder = 0;
	cache_cache.colour = ((BLOCK_SIZE - sizeof(slab)) % (sizeof(kmem_cache_t) + sizeof(unsigned int))) / 64 + 1; //calculate how many offsets
	cache_cache.colour_next = 0;
	cache_cache.ctor = nullptr;
	cache_cache.dtor = nullptr;
	sprintf_s(cache_cache.name, "cache_cache\0");
	cache_cache.growing = false;
	cache_cache.next.list_init();
	new(&cache_cache.spinlock) recursive_mutex();
}




void kmem_init(void * space, int block_num) {
	bud = new(space) buddy(space, block_num);
	init_cache();
	for (int i = 0; i < 13; i++) {
		char name[32];
		sprintf_s(name, "Size%d", 32 << i);
		buffers[i] = kmem_cache_create(name, 32 << i, nullptr, nullptr);
	}
}

kmem_cache_t * kmem_cache_create(const char * name, size_t size, void(*ctor)(void *), void(*dtor)(void *)) {
	lock_guard<recursive_mutex> guard(cache_cache.spinlock);
	kmem_cache_t* cachep = (kmem_cache_t*)kmem_cache_alloc(&(cache_cache));
	if (cachep == nullptr) { 
		return nullptr; 
	} //ERROR
	cachep->slabCnt = 0;
	cachep->objCnt = 0;
	cachep->ctor = ctor;
	cachep->dtor = dtor;
	cachep->objsize = size;
	cachep->colour_next = 0; //default
	cachep->growing = false; //default
	cachep->slabs_free.list_init();
	cachep->slabs_full.list_init();
	cachep->slabs_partial.list_init();
	if (strlen(name) < 63) {
		sprintf_s(cachep->name, name);
	} else {
		cachep->err.occured = true;
		sprintf_s(cachep->err.function, "Cache name too long");
		exit(1);
	}
	int i = 0;
	while ((BLOCK_SIZE << i) < size) i++;
	cachep->gfporder = i;
	if (size < (BLOCK_SIZE/8)) {
		cachep->num = ((BLOCK_SIZE - sizeof(slab)) / (size + sizeof(unsigned int)));
		cachep->colour = ((BLOCK_SIZE - sizeof(slab)) % (size + sizeof(unsigned int))) / 64 + 1;
	} else {
		cachep->num = (BLOCK_SIZE << i) / size;
		cachep->colour = ((BLOCK_SIZE << i) % size) / 64 + 1;
	}
	new(&cachep->spinlock) recursive_mutex();

	//put it in cache chain
	cachep->next.next = cache_cache.next.next;
	if (cachep->next.next != nullptr) {
		cachep->next.next->prev = &(cachep->next);
	}
	cachep->next.prev = &cache_cache.next;
	cache_cache.next.next = &cachep->next;

	return cachep;
}

int kmem_cache_shrink(kmem_cache_t * cachep) {
	if (cachep == nullptr) { //ERROR
		return 0;
	}
	lock_guard<recursive_mutex> guard(cachep->spinlock);
	if (cachep->growing) {
		cachep->growing = false;
		return 0;
	}
	unsigned long long cnt = 0;
	while (cachep->slabs_free.next != nullptr) { //while there are free slabs
		slab* tmp = list_entry(cachep->slabs_free.next, slab, list);
		tmp->list.prev->next = tmp->list.next;
		if (tmp->list.next != nullptr) {
			tmp->list.next->prev = tmp->list.prev;
		}
		void* adr;
		if (cachep->objsize < (BLOCK_SIZE/8)) {
			adr = tmp;
		} else {
			adr = (void*)((unsigned long long)(tmp->s_mem) - tmp->colouroff);
		}
		bud->kmem_freepages(adr, cachep->gfporder);
		cnt += cachep->gfporder;
		cachep->slabCnt--;
	}
	return (int) exp2(cnt);
}

void * kmem_cache_alloc(kmem_cache_t * cachep) {
	if (cachep == nullptr) {
		return nullptr; 
	} //ERROR
	lock_guard<recursive_mutex> guard(cachep->spinlock);
	slab* tmp;
	if (cachep->slabs_partial.next != nullptr) { //if there's a partial slab first fill it
		tmp = list_entry(cachep->slabs_partial.next, slab, list);
		cachep->slabs_partial.next = tmp->list.next;
		if (tmp->list.next != nullptr) {
			tmp->list.next->prev = tmp->list.prev;
		}
	} else if (cachep->slabs_free.next != nullptr) {//else, check if there are any free slabs and fill them
		tmp = list_entry(cachep->slabs_free.next, slab, list);
		cachep->slabs_free.next = tmp->list.next;
		if (tmp->list.next != nullptr) {
			tmp->list.next->prev = tmp->list.prev;
		}
	} else {//we need to alloc a new slab from buddy
		void* adr = bud->kmem_getpages(cachep->gfporder);
		if (adr == nullptr) {
			cachep->err.occured = true;
			sprintf_s(cachep->err.function, "kmem_cache_alloc\0");
			return nullptr;
		}
		if (cachep->objsize < (BLOCK_SIZE/8)) { //if objsize is an eight of BLOCK_SIZE we store slab descriptor on the slab
			tmp = (slab*)adr;
			tmp->init(cachep);	
		} else {// else put it in a buffer
			tmp = (slab*)kmalloc(sizeof(slab)+ (cachep->num * sizeof(unsigned int)));
			if (tmp == nullptr) {
				bud->kmem_freepages(adr, cachep->gfporder);
				cachep->err.occured = true;
				sprintf_s(cachep->err.function, "kmem_cache_alloc\0");
				return nullptr;
			}
			if (cachep->num <= 8) {
				tmp->init(cachep, adr);
			} else {
				cachep->err.occured = true;
				sprintf_s(cachep->err.function, "FATAL ERROR!\0");
				exit(1);
			}
		}
		unsigned long long index = ((unsigned long long) adr - (unsigned long long) (bud->space)) >> (unsigned long long) log2(BLOCK_SIZE);
		if (index >= bud->usable) {
			if (cachep->objsize > BLOCK_SIZE/8) {
				kfree(tmp);
			}
			bud->kmem_freepages(adr, cachep->gfporder);
			cachep->err.occured = true;
			sprintf_s(cachep->err.function, __func__);
			return nullptr;
		}
		page* pagep = &((bud->pagesBase)[index]);
		for (int i = 0; i < (1 << cachep->gfporder); i++) {
			page::set_cache(&pagep[i], cachep);
			page::set_slab(&pagep[i], tmp);
		}
		cachep->slabCnt++;
	}
	
	void* objp = (void*)((unsigned long long) tmp->s_mem + tmp->free*cachep->objsize);
	tmp->free = slab_buffer(tmp)[tmp->free];
	tmp->objCnt++;
	cachep->objCnt++;
	list_head* toPut;
	if ((tmp->objCnt < cachep->num) && tmp->free != ~0) { //if slab is partialy filled put it back in partial
		toPut = &cachep->slabs_partial;
	} else {//else put it back in full
		toPut = &cachep->slabs_full;
	}

	//update list
	tmp->list.prev = toPut;
	tmp->list.next = toPut->next;
	if (toPut->next != nullptr) {
		toPut->next->prev = &tmp->list;
	}
	toPut->next = &tmp->list;

	//set the flag for reaping
	cachep->growing = true;

	return objp;
}

void kmem_cache_free(kmem_cache_t * cachep, void * objp) {
	if (cachep == nullptr) { 
		return; 
	} //ERROR
	if (objp == nullptr) {
		cachep->err.occured = true;
		sprintf_s(cachep->err.function, "kmem_cache_free\0");
		return;
	}
	lock_guard<recursive_mutex> guard(cachep->spinlock);
	slab* slabp = page::get_slab(page::virtual_to_page(objp));
	if (slabp == nullptr || ((unsigned long long)slabp > (unsigned long long) bud->space + bud->usable*BLOCK_SIZE) || (unsigned long long)slabp < (unsigned long long)bud->space ) {
		cachep->err.occured = true;
		sprintf_s(cachep->err.function, "kmem_cache_free\0");
		return;
	}
	if (cachep->dtor != nullptr) {
		cachep->dtor(objp);
	}
	slabp->list.prev->next = slabp->list.next;
	if (slabp->list.next != nullptr) {
		slabp->list.next->prev = slabp->list.prev;
	}
	unsigned long long objNo = ((unsigned long long) objp - (unsigned long long) slabp->s_mem) / cachep->objsize;
	slab_buffer(slabp)[objNo] = (unsigned int) slabp->free;
	slabp->free = objNo;

	slabp->objCnt--;
	cachep->objCnt--;
	list_head * toPut;
	if (slabp->objCnt > 0) { //if there's still objects in slab return it to partial list
		toPut = &cachep->slabs_partial;
	} else { //else put it in free slabs
		toPut = &cachep->slabs_free;
	}

	slabp->list.next = toPut->next;
	slabp->list.prev = toPut;
	if (toPut->next != nullptr) {
		toPut->next->prev = &slabp->list;
	}
	toPut->next = &slabp->list;
}

void * kmalloc(size_t size) {
	unsigned long long min = 32;
	for (int i = 0; i < 13; i++) {
		if (size <= min << i) return kmem_cache_alloc(buffers[i]);
	}
	return nullptr;
}

void kfree(const void * objp) {
	if (objp == nullptr) return;
	kmem_cache_t* cachep = page::get_cache(page::virtual_to_page((void*)objp));
	if (cachep == nullptr) return;
	kmem_cache_free(cachep, (void*)objp);
}

void kmem_cache_destroy(kmem_cache_t * cachep) { 
	if (cachep == nullptr) { 
		return; 
	} //ERROR
	lock_guard<recursive_mutex> guard(cachep->spinlock);
	if (cachep->slabs_full.next == nullptr && cachep->slabs_partial.next == nullptr) { // can only destroy if all the slabs are empty
		cachep->growing = false;
		kmem_cache_shrink(cachep); //return all the slabs to buddy
		
		//remove from cache chain
		cachep->next.prev->next = cachep->next.next;
		if (cachep->next.next != nullptr) {
			cachep->next.next->prev = cachep->next.prev;
		}

		kmem_cache_free(&cache_cache, (void*)cachep); //remove from cache_cache
	}
}

void kmem_cache_info(kmem_cache_t * cachep) {
	if (cachep == nullptr) {
		return;
	}
	lock_guard<recursive_mutex> guard(cachep->spinlock);
	double percent = (double)100*cachep->objCnt / ((double)(cachep->num*cachep->slabCnt));
	printf_s("Name: '%s'\tObjSize: %d B\tCacheSize: %d Blocks\tSlabCnt: %d\tObjInSlab: %d\tFilled:%lf%%\n", cachep->name, (int) cachep->objsize, (int)((cachep->slabCnt)*exp2(cachep->gfporder)), (int) cachep->slabCnt, (int) cachep->num, percent);
}

int kmem_cache_error(kmem_cache_t * cachep) {
	if (cachep != nullptr) {
		lock_guard<recursive_mutex> guard(cachep->spinlock);
		if (cachep->err.occured) {
			printf_s("ERROR IN FUNCTION: %s\n", cachep->err.function);
			//cachep->err.occured = false;
		}
		return 1;
	}
	return 0;
}
