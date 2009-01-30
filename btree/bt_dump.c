/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int  __wt_bt_dump_offpage(DB *, DBT *,
    WT_ITEM_OFFP *, FILE *, void (*)(u_int8_t *, u_int32_t, FILE *));
static void __wt_bt_hexprint(u_int8_t *, u_int32_t, FILE *);
static void __wt_bt_print_nl(u_int8_t *, u_int32_t, FILE *);

/* Check if the next page entry is part of a duplicate data set. */
#define	WT_DUP_AHEAD(item, yesno) {					\
	WT_ITEM *__item = WT_ITEM_NEXT(item);				\
	(yesno) = __item->type == WT_ITEM_DUP ||			\
	    __item->type == WT_ITEM_DUP_OVFL ||				\
	    __item->type == WT_ITEM_OFFPAGE ? 1 : 0;			\
}

/*
 * __wt_db_dump --
 *	Db.dump method.
 */
int
__wt_db_dump(wt_args_db_dump *argp)
{
	wt_args_db_dump_unpack;
	DBT last_key_ovfl, last_key_std, *last_key;
	ENV *env;
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
	WT_ITEM_OFFP offp;
	WT_PAGE *page, *ovfl_page;
	u_int32_t addr, i;
	int dup_ahead, ret;
	void (*func)(u_int8_t *, u_int32_t, FILE *);

	DB_FLAG_CHK(db, "Db.dump", flags, WT_APIMASK_DB_DUMP);

	if (LF_ISSET(WT_DEBUG))
		return (__wt_bt_dump_debug(db, NULL, stream));

	env = db->env;
	dup_ahead = ret = 0;
	func = flags == WT_PRINTABLES ? __wt_bt_print_nl : __wt_bt_hexprint;

	/*lint -esym(644,last_key)
	 * LINT complains last_key may be used before being set -- that's not
	 * true, on any well-formed page, last_key will be set by encountering
	 * an item of type WT_ITEM_KEY/KEY_OVFL before anything else.
	 */
	WT_CLEAR(last_key_std);
	WT_CLEAR(last_key_ovfl);

	for (addr = WT_ADDR_FIRST_PAGE;;) {
		if ((ret = __wt_bt_page_in(db, addr, 1, &page)) != 0)
			return (ret);

		WT_ITEM_FOREACH(page, item, i)
			switch (item->type) {
			case WT_ITEM_KEY:
				last_key_std.data = WT_ITEM_BYTE(item);
				last_key_std.size = item->len;
				last_key = &last_key_std;
				/*
				 * If we're about to dump an off-page duplicate
				 * set, don't write the key here, we'll write
				 * it in the off-page dump routine.
				 */
				WT_DUP_AHEAD(item, dup_ahead);
				if (!dup_ahead)
					func(WT_ITEM_BYTE(item),
					    item->len, stream);
				break;
			case WT_ITEM_DATA:
				func(WT_ITEM_BYTE(item), item->len, stream);
				break;
			case WT_ITEM_DUP:
				func(last_key->data, last_key->size, stream);
				func(WT_ITEM_BYTE(item), item->len, stream);
				break;
			case WT_ITEM_KEY_OVFL:
				/*
				 * If the overflow key has duplicate records,
				 * we'll need a copy of the key for display on
				 * each of those records.  Look ahead and see
				 * if it's a set of duplicates.
				 */
				WT_DUP_AHEAD(item, dup_ahead);
				/* FALLTHROUGH */
			case WT_ITEM_DATA_OVFL:
			case WT_ITEM_DUP_OVFL:
				ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
				if ((ret = __wt_bt_ovfl_in(db,
				    ovfl->addr, ovfl->len, &ovfl_page)) != 0)
					goto err;

				/*
				 * If we're already in a duplicate set, dump
				 * the key.
				 */
				if (item->type == WT_ITEM_DUP_OVFL)
					func(last_key->data,
					    last_key->size, stream);

				/*
				 * If we're starting a new duplicate set with
				 * an overflow key, save a copy of the key for
				 * later display.  Otherwise, dump this item.
				 */
				if (dup_ahead) {
					if ((ret = __wt_bt_data_copy_to_dbt(db,
					    WT_PAGE_BYTE(ovfl_page), ovfl->len,
					    &last_key_ovfl)) != 0)
						goto err;
					last_key = &last_key_ovfl;
					dup_ahead = 0;
				} else
					func(WT_PAGE_BYTE(ovfl_page),
					    ovfl->len, stream);

				if ((ret =
				    __wt_bt_page_out(db, ovfl_page, 0)) != 0)
					goto err;
				break;
			case WT_ITEM_OFFPAGE:
				/*
				 * !!!
				 * Don't pass __wt_bt_dump_offpage a pointer
				 * to the on-page OFFP structure, it writes
				 * the offp passed in.
				 */
				offp = *(WT_ITEM_OFFP *)WT_ITEM_BYTE(item);
				if ((ret = __wt_bt_dump_offpage(
				    db, last_key, &offp, stream, func)) != 0)
					goto err;
				break;
			WT_DEFAULT_FORMAT(db);
			}

		addr = page->hdr->nextaddr;
		if ((ret = __wt_bt_page_out(db, page, 0)) != 0)
			return (ret);
		if (addr == WT_ADDR_INVALID)
			break;
	}

	/* Discard any space allocated to hold an overflow key. */
	WT_FREE_AND_CLEAR(env, last_key_ovfl.data);

	if (0) {
err:		ret = WT_ERROR;
	}
	return (ret);
}

/*
 * __wt_bt_dump_offpage --
 *	Dump a set of off-page duplicates.
 */
static int
__wt_bt_dump_offpage(DB *db, DBT *key, WT_ITEM_OFFP *offp,
    FILE *stream, void (*func)(u_int8_t *, u_int32_t, FILE *))
{
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
	WT_PAGE *page, *ovfl_page;
	u_int32_t addr, i;
	int ret;

	page = NULL;

	/* Walk down the duplicates tree to the first leaf page. */
	for (;;) {
		if ((ret = __wt_bt_page_in(db, offp->addr,
		    offp->level == WT_LEAF_LEVEL ? 1 : 0, &page)) != 0)
			goto err;

		if (offp->level == WT_LEAF_LEVEL)
			break;
		__wt_bt_first_offp(page, offp);

		if ((ret = __wt_bt_page_out(db, page, 0)) != 0) {
			page = NULL;
			goto err;
		}
	}

	for (;;) {
		WT_ITEM_FOREACH(page, item, i) {
			func(key->data, key->size, stream);
			switch (item->type) {
			case WT_ITEM_DUP:
				func(WT_ITEM_BYTE(item), item->len, stream);
				break;
			case WT_ITEM_DUP_OVFL:
				ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
				if ((ret = __wt_bt_ovfl_in(db,
				    ovfl->addr, ovfl->len, &ovfl_page)) != 0)
					goto err;
				func(
				    WT_PAGE_BYTE(ovfl_page), ovfl->len, stream);
				if ((ret =
				    __wt_bt_page_out(db, ovfl_page, 0)) != 0)
					goto err;
				break;
			WT_DEFAULT_FORMAT(db);
			}
		}

		addr = page->hdr->nextaddr;
		if ((ret = __wt_bt_page_out(db, page, 0)) != 0) {
			page = NULL;
			goto err;
		}
		if (addr == WT_ADDR_INVALID)
			break;

		if ((ret = __wt_bt_page_in(db, addr, 1, &page)) != 0)
			goto err;
	}

	if (0) {
err:		ret = WT_ERROR;
		if (page != NULL)
			(void)__wt_bt_page_out(db, page, 0);
	}
	return (ret);
}

static const char hex[] = "0123456789abcdef";

/*
 * __wt_bt_print_nl --
 *	Output a single key/data entry in printable characters, where possible.
 *	In addition, terminate with a <newline> character, unless the entry is
 *	itself terminated with a <newline> character.
 */
static void
__wt_bt_print_nl(u_int8_t *data, u_int32_t len, FILE *stream)
{
	if (data[len - 1] == '\n')
		--len;
	__wt_bt_print(data, len, stream);
	fprintf(stream, "\n");
}

/*
 * __wt_bt_print --
 *	Output a single key/data entry in printable characters, where possible.
 */
void
__wt_bt_print(u_int8_t *data, u_int32_t len, FILE *stream)
{
	int ch;

	for (; len > 0; --len, ++data) {
		ch = data[0];
		if (isprint(ch))
			fprintf(stream, "%c", ch);
		else
			fprintf(stream, "%x%x",
			    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
	}
}

/*
 * __wt_bt_hexprint --
 *	Output a single key/data entry in hex.
 */
static void
__wt_bt_hexprint(u_int8_t *data, u_int32_t len, FILE *stream)
{
	for (; len > 0; --len, ++data)
		fprintf(stream, "%x%x",
		    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
	fprintf(stream, "\n");
}
