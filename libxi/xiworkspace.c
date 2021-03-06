/*
 * Copyright (c) 2016, Juniper Networks, Inc.
 * All rights reserved.
 * This SOFTWARE is licensed under the LICENSE provided in the
 * ../Copyright file. By downloading, installing, copying, or otherwise
 * using the SOFTWARE, you agree to be bound by the terms of that
 * LICENSE.
 *
 * Phil Shafer (phil@) July 2016
 *
 * A "workspace" is a space where we work, creating nodes, names,
 * atoms, etc.   It can commonize these pools from which these are
 * built, allowing multiple trees (documents) to share.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <assert.h>
#include <ctype.h>
#include <limits.h>

#include "slaxconfig.h"
#include <libslax/slaxdef.h>
#include <libslax/slax.h>
#include <parrotdb/pacommon.h>
#include <parrotdb/paconfig.h>
#include <parrotdb/pammap.h>
#include <parrotdb/pafixed.h>
#include <parrotdb/paarb.h>
#include <parrotdb/paistr.h>
#include <parrotdb/papat.h>
#include <parrotdb/pabitmap.h>
#include <libxi/xicommon.h>
#include <libxi/xisource.h>
#include <libxi/xirules.h>
#include <libxi/xitree.h>
#include <libxi/xiworkspace.h>
#include <libxi/xinodeset.h>
#include <libxi/xiparse.h>

xi_workspace_t *
xi_workspace_open (pa_mmap_t *pmp, const char *name)
{
    pa_istr_t *names = NULL;
    pa_pat_t *names_index = NULL;
    pa_fixed_t *ns_map = NULL;
    pa_pat_t *ns_map_index = NULL;
    pa_istr_t *pip = NULL;
    pa_arb_t *pap = NULL;
    pa_pat_t *ppp = NULL;
    xi_node_t *nodep = NULL;
    pa_fixed_t *nodes = NULL;
    xi_workspace_t *workp = NULL;
    char namebuf[PA_MMAP_HEADER_NAME_LEN];
    pa_fixed_t *nodeset_chunks = NULL, *nodeset_info = NULL;

    /* Holds the names of our elements, attributes, etc */
    xi_mk_name(namebuf, name, "names");
    xi_namepool_open(pmp, namebuf, &names, &names_index);
    if (names == NULL)
	goto fail;

    xi_mk_name(namebuf, name, "namespaces");
    xi_ns_open(pmp, namebuf, &ns_map, &ns_map_index);
    if (ns_map == NULL)
	goto fail;

    nodes = pa_fixed_open(pmp, xi_mk_name(namebuf, name, "nodes"), XI_SHIFT,
			 sizeof(*nodep), XI_MAX_ATOMS);
    if (nodes == NULL)
	goto fail;

    pap = pa_arb_open(pmp, xi_mk_name(namebuf, name, "data"));
    if (pap == NULL)
	goto fail;

    nodeset_chunks = pa_fixed_open(pmp,
			xi_mk_name(namebuf, name, "nodeset-chunks"), XI_SHIFT,
			XI_NODESET_CHUNK_SIZE, XI_MAX_ATOMS);
    if (nodeset_chunks == NULL)
	goto fail;

    /* Ensure that our freshly allocated data is zeroed */
    pa_fixed_set_flags(nodeset_chunks, PFF_INIT_ZERO);

    nodeset_info = pa_fixed_open(pmp,
			xi_mk_name(namebuf, name, "nodeset-info"), XI_SHIFT,
			sizeof(xi_nodeset_info_t), XI_MAX_ATOMS);
    if (nodeset_info == NULL)
	goto fail;

    /* Ensure that our freshly allocated data is zeroed */
    pa_fixed_set_flags(nodeset_info, PFF_INIT_ZERO);

    workp = calloc(1, sizeof(*workp));
    if (workp == NULL)
	goto fail;

    workp->xw_mmap = pmp;
    workp->xw_nodes = nodes;
    workp->xw_names = names;
    workp->xw_names_index = names_index;
    workp->xw_ns_map = ns_map;
    workp->xw_ns_map_index = ns_map_index;
    workp->xw_textpool = pap;
    workp->xw_nodeset_chunks = nodeset_chunks;
    workp->xw_nodeset_info = nodeset_info;

    return workp;

 fail:
    if (nodeset_chunks != NULL)
	pa_fixed_close(nodeset_chunks);
    if (nodeset_info != NULL)
	pa_fixed_close(nodeset_info);
    if (pap != NULL)
	pa_arb_close(pap);
    if (pip != NULL)
	pa_istr_close(pip);
    if (ppp != NULL)
	pa_pat_close(ppp);
    if (nodes != NULL)
	pa_fixed_close(nodes);
    if (names != NULL)
	pa_istr_close(names);
    if (names_index != NULL)
	pa_pat_close(names_index);
    if (ns_map != NULL)
	pa_fixed_close(ns_map);
    if (ns_map_index != NULL)
	pa_pat_close(ns_map_index);

    return NULL;
}

void
xi_namepool_open (pa_mmap_t *pmap, const char *basename,
		  pa_istr_t **namesp, pa_pat_t **names_indexp)
{
    char namebuf[PA_MMAP_HEADER_NAME_LEN];
    pa_istr_t *pip = NULL;
    pa_pat_t *ppp = NULL;

    /* The name pool holds the names of our elements, attributes, etc */
    pip = pa_istr_open(pmap, xi_mk_name(namebuf, basename, "data"),
		       XI_SHIFT, XI_ISTR_SHIFT, XI_MAX_ATOMS);
    if (pip == NULL)
	return;

    ppp = pa_pat_open(pmap, xi_mk_name(namebuf, basename, "index"),
		      pip, pa_pat_istr_key_func,
		      PA_PAT_MAXKEY, XI_SHIFT, XI_MAX_ATOMS);
    if (ppp == NULL) {
	pa_istr_close(pip);
	return;
    }

    *namesp = pip;
    *names_indexp = ppp;
}

static const uint8_t *
xi_ns_key_func (pa_pat_t *pp, pa_pat_node_t *node)
{
    return pa_fixed_atom_addr(pp->pp_data, node->ppn_data);
}

void
xi_ns_open (pa_mmap_t *pmap, const char *basename,
	    pa_fixed_t **nsp, pa_pat_t **ns_indexp)
{
    char namebuf[PA_MMAP_HEADER_NAME_LEN];
    pa_fixed_t *pfp = NULL;
    pa_pat_t *ppp = NULL;

    /* The ns pool holds the names of our elements, attributes, etc */
    pfp = pa_fixed_open(pmap, xi_mk_name(namebuf, basename, "data"),
			XI_SHIFT, sizeof(xi_ns_map_t), XI_MAX_ATOMS);
    if (pfp == NULL)
	return;

    ppp = pa_pat_open(pmap, xi_mk_name(namebuf, basename, "index"),
		      pfp, xi_ns_key_func,
		      PA_PAT_MAXKEY, XI_SHIFT, XI_MAX_ATOMS);
    if (ppp == NULL) {
	pa_fixed_close(pfp);
	return;
    }

    *nsp = pfp;
    *ns_indexp = ppp;
}

/*
 * Return a name atom for a string in the name pool.  Our patricia tree
 * has data atoms that are istr atoms, which we turn into name atoms.
 * It's some ugly "atom smashing" that keeps us type safe.  Think of it
 * as lead shielding.
 */
xi_name_atom_t
xi_namepool_atom (xi_workspace_t *xwp, const char *data, xi_boolean_t createp)
{
    uint16_t len = strlen(data) + 1;
    pa_pat_t *ppp = xwp->xw_names_index;

    pa_pat_data_atom_t datom = pa_pat_get_atom(ppp, len, data);
    if (pa_pat_data_is_null(datom) && createp) {
	/* Allocate the name from our pool and add it to the tree */
	pa_istr_atom_t iatom = pa_istr_string(xwp->xw_names, data);
	datom = pa_pat_data_atom(pa_istr_atom_of(iatom));
	if (pa_istr_is_null(atom))
	    pa_warning(0, "namepool create key failed for key '%s'", data);
	else if (!pa_pat_add(ppp, datom, len))
	    pa_warning(0, "duplicate key: %s", data);
    }

    xi_name_atom_t atom = xi_name_atom_t(pa_pat_data_atom_of(datom));
    return atom;
}

pa_atom_t
xi_get_attrib (xi_workspace_t *xwp, xi_node_t *nodep, pa_atom_t name_atom)
{
    pa_atom_t node_atom;
    int attrib_seen = FALSE;
    xi_depth_t depth = nodep->xn_depth;

    if (!(nodep->xn_flags & XNF_ATTRIBS_PRESENT))
	return PA_NULL_ATOM;

#if 0 /* XXX */
    if (!(nodep->xn_flags & XNF_ATTRIBS_EXTRACTED))
	xi_node_attrib_extract(xwp, nodep);
#endif

    for (node_atom = nodep->xn_contents; node_atom != PA_NULL_ATOM;
	 node_atom = nodep->xn_next) {
	nodep = xi_node_addr(xwp, node_atom);
	if (nodep == NULL)	/* Should not occur */
	    break;

	if (nodep->xn_depth <= depth)
	    break;		/* Found end of children */

	if (nodep->xn_type != XI_TYPE_ATTRIB)
	    continue;

	attrib_seen = TRUE;

	if (nodep->xn_name == name_atom)
	    return nodep->xn_contents;
    }

    return PA_NULL_ATOM;
}

/*
 * Find the index of a given prefix-to-uri mapping.
 *
 * Note that we allow empty strings for either of these values, since
 * that's how we define the current namespace (when prefix is empty)
 * or the default namespace (when uri is empty).
 *
 * Note also that different return values from this do not imply
 * different namespace, just different prefix mappings.  One can use
 * distinct prefixes to access same namespace, like:
 *    <a xmlns="a.men"><amen:b xmlns:amen="a.men"/></a>
 * Retaining this information allows us to emit XML identical to the
 * original input.  The cost is an extra lookup in xw_ns_map to see
 * the underlaying atom numbers of the URI strings (which reside in
 * the name pool).  Another fine engineering trade off that's such to
 * bite me in the lower cheeks one day.
 */
pa_atom_t
xi_ns_find (xi_workspace_t *xwp, const char *prefix, const char *uri,
	    xi_boolean_t createp)
{
    pa_atom_t prefix_atom = PA_NULL_ATOM, uri_atom = PA_NULL_ATOM;

    if (prefix != NULL && *prefix != '\0') {
	prefix_atom = xi_namepool_atom(xwp, prefix, TRUE);
	if (prefix_atom == PA_NULL_ATOM)
	    return PA_NULL_ATOM;
    }

    if (uri != NULL && *uri != '\0') {
	uri_atom = xi_namepool_atom(xwp, uri, TRUE);
	if (uri_atom == PA_NULL_ATOM)
	    return PA_NULL_ATOM;
    }

    pa_pat_t *ppp = xwp->xw_ns_map_index;
    xi_ns_map_t ns = { prefix_atom, uri_atom };
    pa_atom_t atom = pa_pat_get_atom(ppp, sizeof(ns), &ns);
    if (atom == PA_NULL_ATOM && createp) {
	xi_ns_map_t *nsp = xi_ns_map_alloc(xwp, &atom);
	if (nsp == NULL) {
	    pa_warning(0, "namespace create key failed for '%s%s%s'",
		       prefix ?: "", prefix ? ":" : "", uri ?: "");
	    return PA_NULL_ATOM;
	}

	*nsp = ns;		/* Initialize newly allocated ns_map entry */

	/* Add it to the patricia tree */
	if (!pa_pat_add(ppp, atom, sizeof(ns))) {
	    xi_ns_map_free(xwp, atom);

	    pa_warning(0, "duplicate key failure for namespace '%s%s%s'",
		       prefix ?: "", prefix ? ":" : "", uri ?: "");
	    return PA_NULL_ATOM;
	}
    }

    return atom;

}
