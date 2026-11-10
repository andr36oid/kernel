/* SPDX-License-Identifier: GPL-2.0-only */
/* EROFS-local API adapters: 5.10 provides order-0 pagecache pages. */
#ifndef EROFS_FOLIO_COMPAT_H
#define EROFS_FOLIO_COMPAT_H
#include <linux/pagemap.h>
#include <linux/highmem.h>
#define folio_file_page(f, index) (f)
#define read_cache_folio(mapping,index,filler,data) read_cache_page(mapping,index,filler,data)
#define folio_test_locked(f) PageLocked(f)
#define folio_test_private(f) PagePrivate(f)
#define folio_size(f) PAGE_SIZE
#define folio_nr_pages(f) 1
#define folio_test_uptodate(f) PageUptodate(f)
#define iomap_read_folio(f,ops) iomap_readpage(f,ops)
#define filemap_read(iocb,to,flags) generic_file_buffered_read(iocb,to,flags)
/* DAX/idmapped mounts are not supported by this ARM64 SD-card target. */
#ifdef CONFIG_FS_DAX
#error "EROFS 6.1 compatibility currently targets CONFIG_FS_DAX=n"
#endif
#define IOMAP_DAX 0
static inline struct dax_device *erofs_compat_dax_get(struct block_device *b,
 loff_t *off, void *holder, const void *ops) { *off=0;return NULL; }
static inline void erofs_compat_dax_put(struct dax_device *d, void *holder) {}
#ifdef CONFIG_HIGHMEM
#error "EROFS compatibility targets ARM64 !HIGHMEM"
#endif
#define kmap_local_page(p) page_address(p)
#define kunmap_local(addr) do { (void)(addr); } while (0)

/* 6.1 order-0 expansion, adapted to 5.10 readahead_control fields. */
static inline void erofs_readahead_expand(struct readahead_control *ractl,
 loff_t new_start, size_t new_len)
{
 struct address_space *mapping = ractl->mapping;
 struct file_ra_state *ra = ractl->file ? &ractl->file->f_ra : NULL;
 pgoff_t new_index = new_start / PAGE_SIZE, new_nr_pages;
 gfp_t gfp = readahead_gfp_mask(mapping);

 while (ractl->_index > new_index) {
  unsigned long index = ractl->_index - 1;
  struct page *page = xa_load(&mapping->i_pages, index);
  if (page && !xa_is_value(page)) return;
  page = __page_cache_alloc(gfp);
  if (!page) return;
  if (add_to_page_cache_lru(page, mapping, index, gfp) < 0) { put_page(page); return; }
  ractl->_nr_pages++;
  ractl->_index = page->index;
 }
 new_len += new_start - readahead_pos(ractl);
 new_nr_pages = DIV_ROUND_UP(new_len, PAGE_SIZE);
 while (ractl->_nr_pages < new_nr_pages) {
  unsigned long index = ractl->_index + ractl->_nr_pages;
  struct page *page = xa_load(&mapping->i_pages, index);
  if (page && !xa_is_value(page)) return;
  page = __page_cache_alloc(gfp);
  if (!page) return;
  if (add_to_page_cache_lru(page, mapping, index, gfp) < 0) { put_page(page); return; }
  ractl->_nr_pages++;
  if (ra) { ra->size++; ra->async_size++; }
 }
}
#define readahead_expand erofs_readahead_expand
#endif
