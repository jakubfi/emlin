//  Copyright (c) 2014 Jakub Filipowicz <jakubf@gmail.com>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc.,
//  51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <emelf.h>

#include "emlin.h"
#include "dh.h"

int edebug;

char *output_file;
int otype = O_EMELF;
int cpu = EMELF_CPU_MERA400;
int image_max = 32768;

struct emlin_object *objects;
struct emlin_object *entry;
int addr_top;

struct dh_table *names;

// -----------------------------------------------------------------------
void EDEBUG(char *format, ...)
{
	if (!edebug) return;
	fprintf(stderr, "DEBUG: ");
	va_list ap;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

// -----------------------------------------------------------------------
static int add_libdir(char *dir)
{

	return 0;
}

// -----------------------------------------------------------------------
static void usage()
{
	printf("Usage: emlin [options] input [input ...]\n");
	printf("Where options are one or more of:\n");
	printf("   -o <output> : set output file (a.out otherwise)\n");
	printf("   -O <otype>  : set output file type: raw, emelf (defaults to raw)\n");
	printf("   -L <dir>    : search for libraries in <dir>\n");
    printf("   -v          : print version and exit\n");
	printf("   -h          : print help and exit\n");
	printf("   -d          : print debug information to stderr\n");
}

// -----------------------------------------------------------------------
static int parse_args(int argc, char **argv)
{
	int option;
	struct emlin_object *obj;

	while ((option = getopt(argc, argv,"o:O:L:vhd")) != -1) {
		switch (option) {
			case 'o':
				output_file = strdup(optarg);
				break;
			case 'O':
				if (!strcasecmp(optarg, "raw")) {
					otype = O_RAW;
				} else if (!strcasecmp(optarg, "emelf")) {
					otype = O_EMELF;
				} else {
					printf("Unknown output type: '%s'.\n", optarg);
					return -1;
				}
				break;
			case 'L':
				add_libdir(optarg);
				break;
			case 'v':
				printf("EMLIN v%s - linker for MERA 400 EMELF objects\n", EMLIN_VERSION);
				exit(0);
				break;
			case 'h':
				usage();
				exit(0);
				break;
			case 'd':
				edebug = 1;
				break;
			default:
				return -1;
		}
	}

	if (!output_file) {
		output_file = strdup("a.out");
	}

	while (optind < argc) {
		if (!strcmp(argv[optind], output_file)) {
			printf("Input file '%s' is also listed as an output file.\n", argv[optind]);
			return -1;
		}

		// add object to list
		obj = calloc(1, sizeof(struct emlin_object));
		obj->filename = strdup(argv[optind]);
		obj->e = NULL;
		obj->offset = -1;
		obj->entry = -1;
		obj->next = objects;
		objects = obj;

		optind++;
	}

	return 0;
}

// -----------------------------------------------------------------------
static int load_names(struct emlin_object *obj)
{
	struct emelf_symbol *sym;
	int symcount;
	char *sym_name;
	struct emlin_object *sym_obj;

	// copy global symbol names and assign objects
	sym = obj->e->symbol;
	symcount = obj->e->symbol_count;
	while (sym && (symcount > 0)) {
		if (sym->flags & EMELF_SYM_GLOBAL) {
			sym_name = obj->e->symbol_names + sym->offset;
			sym_obj = dh_get(names, sym_name);
			if (sym_obj) {
				printf("%s: Symbol '%s' already defined in object '%s'\n", obj->filename, sym_name, sym_obj->filename);
				return -1;
			}
			EDEBUG("%s: adding global name: %s", obj->filename, sym_name);
			dh_add(names, sym_name, obj);
		}
		sym++;
		symcount--;
	}

	return 0;
}

// -----------------------------------------------------------------------
static int load_objects()
{
	struct emlin_object *obj = objects;
	FILE *f;
	int abi = EMELF_ABI_UNKNOWN;

	EDEBUG("==== Loading objects ====");

	while (obj) {
		EDEBUG("%s", obj->filename);
		f = fopen(obj->filename, "r");
		if (!f) {
			printf("Cannot open file '%s' for reading.\n", obj->filename);
			return -1;
		}

		obj->e = emelf_load(f);
		fclose(f);
		if (!obj->e) {
			printf("Cannot load object file: %s\n", obj->filename);
			return -1;
		}

		if (abi == EMELF_ABI_UNKNOWN) {
			abi = obj->e->eh.abi;
		} else if (abi != obj->e->eh.abi) {
			printf("Object ABI mismatch\n");
			return -1;
		}

		if (emelf_has_entry(obj->e)) {
			EDEBUG("%s has entry", obj->filename);
			if (entry) {
				printf("%s: entry point already defined in: %s\n", obj->filename, entry->filename);
				return -1;
			}
			entry = obj;
		}

		if (load_names(obj)) {
			return -1;
		}

		if (obj->e->eh.cpu == EMELF_CPU_MX16) {
			cpu = EMELF_CPU_MX16;
			image_max = 65536;
		}

		obj = obj->next;
	}

	return 0;
}

// -----------------------------------------------------------------------
static int link(struct emelf *e, struct emlin_object *obj)
{
	int res;
	struct emelf_reloc *reloc;
	int relcount;
	int sign;
	struct emlin_object *sym_obj;
	char *sym_name;
	struct emelf_symbol *sym;
	char rstr[1024];
	int rpos;

	EDEBUG("==== linking %s @ %i", obj->filename, addr_top);

	if (e->image_size + obj->e->image_size > image_max) {
		printf("%s: image too big (%i > %i [words]) for %s cpu\n",
			obj->filename,
			e->image_size + obj->e->image_size,
			image_max,
			cpu == EMELF_CPU_MX16 ? "MX-16" : "MERA-400"
		);
		return 1;
	}

	// copy image
	res = emelf_image_append(e, obj->e->image, obj->e->image_size);
	if (res != EMELF_E_OK)  {
		printf("%s: cannot append image.\n", obj->filename);
		return -1;
	}
	obj->offset = addr_top;
	addr_top += obj->e->image_size;

	// scan relocs
	reloc = obj->e->reloc;
	relcount = obj->e->reloc_count;
	while (reloc && (relcount > 0)) {

		rpos = 0;
		rpos += snprintf(rstr+rpos, 1024-rpos-1, "%s: reloc @ %i: ", obj->filename, reloc->addr + obj->offset);

		// @start reloc
		if (reloc->flags & EMELF_RELOC_BASE) {
			rpos += snprintf(rstr+rpos, 1024-rpos-1, " + (@start = %i)", obj->offset);
			e->image[reloc->addr + obj->offset] += obj->offset;
		}

		// symbol reloc
		if (reloc->flags & EMELF_RELOC_SYM) {
			// handle negative symbols
			if (reloc->flags & EMELF_RELOC_SYM_NEG) {
				sign = -1;
			} else {
				sign = 1;
			}

			// find object that defines symbol
			sym_name = obj->e->symbol_names + obj->e->symbol[reloc->sym_idx].offset;
			sym_obj = dh_get(names, sym_name);
			if (!sym_obj) {
				printf("%s: symbol '%s' not defined.\n", obj->filename, sym_name);
				return -1;
			}

			EDEBUG("%s: references '%s' in %s", obj->filename, sym_name, sym_obj->filename);

			// link the object referenced by symbol
			if (sym_obj->offset < 0) {
				if (link(e, sym_obj)) {
					return -1;
				}
			}

			// get the symbol
			sym = emelf_symbol_get(sym_obj->e, sym_name);
			if (!sym) {
				printf("%s: cannot get symbol '%s'.\n", sym_obj->filename, sym_name);
				return -1;
			}

			int16_t sym_value = sym->value;
			e->image[reloc->addr + obj->offset] += sign * sym_value;

			rpos += snprintf(rstr+rpos, 1024-rpos-1, " %s (%s:%s = %i", sign>0 ? "+" : "-", sym_obj->filename, sym_name, sym_value);

			if (sym->flags & EMELF_SYM_RELATIVE) {
				if (reloc->flags & EMELF_RELOC_BASE) {
					printf("%s: WARNING: relocating relative value by relative symbol '%s' value\n", obj->filename, sym_name);
				}
				rpos += snprintf(rstr+rpos, 1024-rpos-1, " + @start = %i)", sym_obj->offset);
				e->image[reloc->addr + obj->offset] += sign * sym_obj->offset;
			} else {
				rpos += snprintf(rstr+rpos, 1024-rpos-1, ")");
			}
			EDEBUG("%s", rstr);
		}
		reloc++;
		relcount--;
	}

	return 0;
}

// -----------------------------------------------------------------------
int main(int argc, char **argv)
{
	int ret = 1;
	int res;
	struct emelf *e = NULL;
	FILE *f;

	res = parse_args(argc, argv);
	if (res) {
		goto cleanup;
	}

	if (!objects) {
		printf("No input files.\n");
		goto cleanup;
	}

	names = dh_create(64000);

	if (load_objects()) {
		goto cleanup;
	}

	if (!entry) {
		printf("No program entry point defined.\n");
		goto cleanup;
	}

	e = emelf_create(EMELF_EXEC, cpu, EMELF_ABI_V1);

	if (link(e, entry)) {
		goto cleanup;
	}

	res = emelf_entry_set(e, entry->e->eh.entry);
	if (res != EMELF_E_OK) {
		printf("Failed to set program entry point.\n");
		goto cleanup;
	}

	f = fopen(output_file, "w");
	if (!f) {
		printf("Cannot open output file '%s'.\n", output_file);
		goto cleanup;
	}

	if (otype == O_EMELF) {
		res = emelf_write(e, f);
	} else {
		int pos = e->image_size;
		while (pos >= 0) {
			e->image[pos] = htons(e->image[pos]);
			pos--;
		}
		res = fwrite(e->image, sizeof(uint16_t), e->image_size, f);
		if (res <= 0) {
			res = EMELF_E_FWRITE;
		} else {
			res = EMELF_E_OK;
		}
	}

	if (res != EMELF_E_OK) {
		fclose(f);
		printf("Cannot write output file '%s'.\n", output_file);
		goto cleanup;
	}

	fclose(f);

	ret = 0;

cleanup:
	emelf_destroy(e);
	dh_destroy(names);
	free(output_file);

	struct emlin_object *o = objects;
	while (o) {
		struct emlin_object *next = o->next;
		free(o->filename);
		emelf_destroy(o->e);
		free(o);
		o = next;
	}

	return ret;
}

// vim: tabstop=4 autoindent
