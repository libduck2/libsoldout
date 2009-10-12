/* markdown.c - generic markdown parser */

/*
 * Copyright (c) 2009, Natacha Porté
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "markdown.h"

#include "array.h"

#include <stdio.h> /* only used for debug output */
#include <string.h>

#define TEXT_UNIT 64	/* unit for the copy of the input buffer */
#define WORK_UNIT 64	/* block-level working buffer */


/***************
 * LOCAL TYPES *
 ***************/

/* link_ref • reference to a link */
struct link_ref {
	struct buf *	id;
	struct buf *	link;
	struct buf *	title; };



/**********************
 * XHTML 1.0 RENDERER *
 **********************/

static void
xhtml_paragraph(struct buf *ob, struct buf *text) {
	if (ob->size) bufputc(ob, '\n');
	BUFPUTSL(ob, "<p>");
	if (text) bufput(ob, text->data, text->size);
	BUFPUTSL(ob, "</p>\n"); }

static void
xhtml_blockquote(struct buf *ob, struct buf *text) {
	if (ob->size) bufputc(ob, '\n');
	BUFPUTSL(ob, "<blockquote>\n");
	if (text) bufput(ob, text->data, text->size);
	BUFPUTSL(ob, "</blockquote>\n"); }

/* exported renderer structure */
struct mkd_renderer mkd_xhtml = {
	xhtml_paragraph,
	xhtml_blockquote };



/***************************
 * STATIC HELPER FUNCTIONS *
 ***************************/

/* is_ref • returns whether a line is a reference or not */
static int
is_ref(char *data, size_t beg, size_t end, size_t *last, struct array *refs) {
	size_t i = beg;
	size_t id_offset, id_end;
	size_t link_offset, link_end;
	size_t title_offset, title_end;
	size_t line_end;
	struct link_ref *lr;

	/* up to 3 optional leading spaces */
	while (i < beg + 3 && i < end && data[i] == ' ') i += 1;
	if (i >= end || i >= beg + 3) return 0;

	/* id part: anything but a newline between brackets */
	if (data[i] != '[') return 0;
	i += 1;
	id_offset = i;
	while (i < end && data[i] != '\n' && data[i] != '\r' && data[i] != ']')
		i += 1;
	if (i >= end || data[i] != ']') return 0;
	id_end = i;

	/* spacer: colon (space | tab)* newline? (space | tab)* */
	i += 1;
	if (i >= end || data[i] != ':') return 0;
	i += 1;
	while (i < end && (data[i] == ' ' || data[i] == '\t')) i += 1;
	if (i < end && (data[i] == '\n' || data[i] == '\r')) {
		i += 1;
		if (i < end && data[i] == '\r' && data[i - 1] == '\n') i += 1; }
	while (i < end && (data[i] == ' ' || data[i] == '\t')) i += 1;
	if (i >= end) return 0;

	/* link: whitespace-free sequence, optionally between angle brackets */
	if (data[i] == '<') i += 1;
	link_offset = i;
	while (i < end && data[i] != ' ' && data[i] != '\t'
			&& data[i] != '\n' && data[i] != '\r') i += 1;
	if (data[i - 1] == '>') link_end = i - 1;
	else link_end = i;

	/* optional spacer: (space | tab)* (newline | '\'' | '"' | '(' ) */
	while (i < end && (data[i] == ' ' || data[i] == '\t')) i += 1;
	if (i < end && data[i] != '\n' && data[i] != '\r'
			&& data[i] != '\'' && data[i] != '"' && data[i] != '(')
		return 0;
	line_end = 0;
	/* computing end-of-line */
	if (i >= end || data[i] == '\r' || data[i] == '\n') line_end = i;
	if (i + 1 < end && data[i] == '\n' && data[i + 1] == '\r')
		line_end = i + 1;

	/* optional (space|tab)* spacer after a newline */
	if (line_end) {
		i = line_end + 1;
		while (i < end && (data[i] == ' ' || data[i] == '\t')) i += 1; }

	/* optional title: any non-newline sequence enclosed in '"()
					alone on its line */
	title_offset = title_end = 0;
	if (i + 1 < end
	&& (data[i] == '\'' || data[i] == '"' || data[i] == '(')) {
		i += 1;
		title_offset = i;
		/* looking for EOL */
		while (i < end && data[i] != '\n' && data[i] != '\r') i += 1;
		if (i + 1 < end && data[i] == '\n' && data[i + 1] == '\r')
			title_end = i + 1;
		else	title_end = i;
		/* stepping back */
		i -= 1;
		while (i > title_offset && (data[i] == ' ' || data[i] == '\t'))
			i -= 1;
		if (i > title_offset
		&& (data[i] == '\'' || data[i] == '"' || data[i] == ')')) {
			line_end = title_end;
			title_end = i; } }
	if (!line_end) return 0; /* garbage after the link */

	/* a valid ref has been found, filling-in return structures */
	if (last) *last = line_end;
	if (refs && (lr = arr_item(refs, arr_newitem(refs))) != 0) {
		lr->id = bufnew(id_end - id_offset);
		bufput(lr->id, data + id_offset, id_end - id_offset);
		lr->link = bufnew(link_end - link_offset);
		bufput(lr->link, data + link_offset, link_end - link_offset);
		if (title_end > title_offset) {
			lr->title = bufnew(title_end - title_offset);
			bufput(lr->title, data + title_offset,
						title_end - title_offset); }
		else lr->title = 0; }
	return 1; }


/* is_empty • returns whether the line is blank */
static int
is_empty(char *data, size_t size) {
	size_t i;
	for (i = 0; i < size && data[i] != '\n'; i += 1)
		if (data[i] != ' ' && data[i] != '\t') return 0;
	return 1; }


/* prefix_quote • returns blockquote prefix length */
static size_t
prefix_quote(char *data, size_t size) {
	size_t i = 0;
	if (i < size && data[i] == ' ') i += 1;
	if (i < size && data[i] == ' ') i += 1;
	if (i < size && data[i] == ' ') i += 1;
	if (i < size && data[i] == '>') {
		if (i + 1 < size && (data[i + 1] == ' ' || data[i+1] == '\t'))
			return i + 2;
		else return i + 1; }
	else return 0; }


/* parse_block • parsing of one block, returning next char to parse */
static void parse_block(struct buf *ob, struct mkd_renderer *rndr,
			char *data, size_t size);


static size_t
parse_blockquote(struct buf *ob, struct mkd_renderer *rndr,
			char *data, size_t size) {
	size_t beg, end, pre, work_size = 0;
	char *work_data = 0;
	struct buf *out = bufnew(WORK_UNIT);

	beg = 0;
	while (beg < size) {
		for (end = beg + 1; end < size && data[end - 1] != '\n';
							end += 1);
		pre = prefix_quote(data + beg, end - beg);
		if (pre) beg += pre; /* skipping prefix */
		else if (is_empty(data + beg, end - beg)
		&& (end >= size || prefix_quote(data + end, size - end) == 0)){
			/* empty line followed by non-quote line */
			break; }
		if (beg < end) { /* copy into the in-place working buffer */
			/* bufput(work, data + beg, end - beg); */
			if (!work_data)
				work_data = data + beg;
			else if (data + beg != work_data + work_size)
				memmove(work_data + work_size, data + beg,
						end - beg);
			work_size += end - beg; }
		beg = end; }

	parse_block(out, rndr, work_data, work_size);
	rndr->blockquote(ob, out);
	return end; }


static size_t
parse_paragraph(struct buf *ob, struct mkd_renderer *rndr,
			char *data, size_t size) {
	size_t i = 0, end = 0;
	struct buf work = { data, 0, 0, 0, 0 }; /* volatile working buffer */

	while (i < size) {
		for (end = i + 1; end < size && data[end - 1] != '\n';
								end += 1);
		if (is_empty(data + i, size - i))
			break;
		i = end; }

	work.size = end;
	while (work.size && data[work.size - 1] == '\n')
		work.size -= 1;
	rndr->paragraph(ob, &work);
	return end; }


/* parse_block • parsing of one block, returning next char to parse */
static void
parse_block(struct buf *ob, struct mkd_renderer *rndr,
			char *data, size_t size) {
	size_t beg, end;
	char *txt_data;
	beg = 0;
	while (beg < size) {
		txt_data = data + beg;
		end = size - beg;
		if (prefix_quote(txt_data, end))
			beg += parse_blockquote(ob, rndr, txt_data, end);
		else
			beg += parse_paragraph(ob, rndr, txt_data, end); } }



/**********************
 * EXPORTED FUNCTIONS *
 **********************/

/* markdown • parses the input buffer and renders it into the output buffer */
void
markdown(struct buf *ob, struct buf *ib, struct mkd_renderer *rndr, int flags){
	struct array refs;
	struct link_ref *lr;
	struct buf *text = bufnew(TEXT_UNIT);
	size_t i, beg, end;

	/* first pass: looking for references, copying everything else */
	arr_init(&refs, sizeof (struct link_ref));
	beg = 0;
	while (beg < ib->size) /* iterating over lines */
		if (is_ref(ib->data, beg, ib->size, &end, &refs))
			beg = end;
		else { /* skipping to the next line */
			end = beg;
			while (end < ib->size
			&& ib->data[end] != '\n' && ib->data[end] != '\r')
				end += 1;
			/* adding the line body if present */
			if (end > beg) bufput(text, ib->data + beg, end - beg);
			while (end < ib->size
			&& (ib->data[end] == '\n' || ib->data[end] == '\r')) {
				/* add one \n per newline */
				if (ib->data[end] == '\n'
				|| (end + 1 < ib->size
						&& ib->data[end + 1] != '\n'))
					bufputc(text, '\n');
				end += 1; }
			beg = end; }

	/* adding a final newline if not already present */
	if (!text->size) return;
	if (text->data[text->size - 1] != '\n'
	&&  text->data[text->size - 1] != '\r')
		bufputc(text, '\n');

	/* second pass: actual rendering */
	parse_block(ob, rndr, text->data, text->size);

/* debug: printing the reference list */
BUFPUTSL(ob, "(refs");
lr = refs.base;
for (i = 0; i < refs.size; i += 1) {
	BUFPUTSL(ob, "\n\t(\"");
	bufput(ob, lr->id->data, lr->id->size);
	BUFPUTSL(ob, "\" \"");
	bufput(ob, lr->link->data, lr->link->size);
	if (lr->title) {
		BUFPUTSL(ob, "\" \"");
		bufput(ob, lr->title->data, lr->title->size); }
	BUFPUTSL(ob, "\")"); }
BUFPUTSL(ob, ")\n"); }

/* vim: set filetype=c: */