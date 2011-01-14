/*
 * $Id: debugger.c 384736 2010-06-16 04:41:59Z deo $
 *
 * Copyright (c) 2010, Juniper Networks, Inc.
 * All rights reserved.
 *
 * Debugger for scripts
 *
 * The debugger ties into the libxslt debugger hooks via the
 * xsltSetDebuggerCallbacks() API.  This is a fairly weak and
 * undocumented API, so it is very likely that I am not using it
 * correctly.
 *
 * The API has three callbacks, one called to add stack frames
 * (addFrame), one called to drop (pop) stack frames (dropFrame), and
 * one called as each instruction is executed (Handler), where
 * "instruction" is an XSLT element.  Note that there is not always a
 * 1:1 mapping between SLAX statements and XSLT instructions due to
 * the nature of SLAX.
 *
 * The addFrame callback takes two arguments, a template and an
 * instruction.  The template node is NULL when executing initializers
 * for global variables, and when executing the <xsl:call-template>
 * instruction.  The instruction should never be NULL.  Sadly, addFrame
 * doesn't get the context pointer, so we have to grab it the next time
 * the handler is called (see DSF_FRESHADD).
 *
 * The dropFrame callback takes no arguments, but it only called when
 * the corresponding addFrame call returns non-zero.  We always record
 * frames. Our dropFrame just discards the top stack frame.
 *
 * The Handler callback is called for each instruction before it is
 * executed.  It takes four parameters, the instruction being
 * executed, the current context node (the node being examined aka
 * "."), the template being executed (which the instruction is part
 * of), and the XSLT transformation context.  If we are evaluating an
 * initializer for a global variable, both the template and the
 * context node will be NULL.  The instruction and context will never be
 * NULL.
 *
 * So this simple API puts all the work on our side of the fence,
 * which is fine.  Missing features include a real prototype for
 * xsltSetDebuggerCallbacks() and a means of passing opaque data
 * through the API to the callbacks, without which we have to resort
 * to global data.
 *
 * Our data consists of three items: slaxDebugState represents the
 * current state of the debugger; slaxDebugStack is the stack of stack
 * frames maintained by the addFrame and dropFrame callbacks; and
 * slaxDebugBreakpoint is the list of breakpoints.
 *
 * At our upper API, we have slaxDebugRegister() which turns on the
 * debugger and records callbacks for getting input and sending
 * output.  slaxDebugSetStylesheet() and slaxDebugSetIncludes() set
 * the current stylesheet and include/import search path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/queue.h>

#include <libxml/xmlsave.h>
#include <libxml/xmlIO.h>
#include <libxslt/variables.h>
#include <libxslt/transform.h>

#include "slaxinternals.h"
#include <libslax/slax.h>
#include "config.h"

#define MAXARGS	256

/* Add some values to the xsltDebugStatusCodes enum */
#define XSLT_DEBUG_LOCAL (XSLT_DEBUG_QUIT + 1)
#define XSLT_DEBUG_OVER (XSLT_DEBUG_LOCAL + 1) /* The "over" operation */
#define XSLT_DEBUG_DONE (XSLT_DEBUG_LOCAL + 2) /* The script is done */

/*
 * Information about the current point of debugging
 */
typedef struct slaxDebugState_s {
    xsltStylesheetPtr ds_script; /* Current top-level script/stylesheet */
    xmlNodePtr ds_inst;	/* Current libxslt node being executed */
    xmlNodePtr ds_node;	/* Current context node */
    xsltTemplatePtr ds_template; /* Current template being executed */
    xsltTransformContextPtr ds_ctxt; /* Transformation context */
    xmlNodePtr ds_last_inst;	/* Last libxslt node being executed */
    xmlNodePtr ds_stop_at;	/* Stopping point (from "cont xxx") */
    int ds_count;		/* Command count */
    int ds_flags;		/* Global state flags */
    int ds_stackdepth;		/* Current depth of call stack */
} slaxDebugState_t;

/* Flags for ds_flags */
#define DSF_OVER 	(1<<0)	/* Step over the current instruction */
#define DSF_DISPLAY	(1<<1)	/* Show instruction before next command */
#define DSF_CALLFLOW	(1<<2)	/* Report call flow */
#define DSF_INSHELL	(1<<3)	/* Inside the shell, so don't recurse */

#define DSF_RESTART	(1<<4)	/* Restart the debugger/script */
#define DSF_PROFILER	(1<<5)	/* Profiler is on */
#define DSF_FRESHADD	(1<<6)	/* Just did an "addFrame" */
#define DSF_CONTINUE	(1<<7)	/* Continue (or run) when restarted */

#define DSF_RELOAD	(1<<8)	/* Reload the script */

slaxDebugState_t slaxDebugState;
const char **slaxDebugIncludes;

/* Arguments to our command functions */
#define DC_ARGS \
        slaxDebugState_t *statep UNUSED, \
	const char *commandline UNUSED, \
	const char **argv UNUSED
#define DH_ARGS \
        slaxDebugState_t *statep UNUSED

/*
 * Commands supported in debugger
 */
typedef void (*slaxDebugCommandFunc_t)(DC_ARGS);
typedef void (*slaxDebugHelpFunc_t)(DH_ARGS);

typedef struct slaxDebugCommand_s {
    const char *dc_command;	/* Command name */
    int dc_min;			/* Minimum length */
    slaxDebugCommandFunc_t dc_func; /* Function pointer */
    const char *dc_help;	/* Help text */
    slaxDebugHelpFunc_t dc_helpfunc; /* Function to generate more help */
} slaxDebugCommand_t;

static slaxDebugCommand_t slaxDebugCmdTable[];

static slaxDebugCommand_t *
slaxDebugGetCommand (const char *name);

/*
 * Doubly linked list to store the templates call sequence
 */
typedef struct slaxDebugStackFrame_s {
    TAILQ_ENTRY(slaxDebugStackFrame_s) st_link;
    unsigned st_depth;           /* Stack depth */
    xsltTemplatePtr st_template; /* Template (parent) */
    xmlNodePtr st_inst;	  /* Instruction of the template (code) */
    xmlNodePtr st_caller;        /* Instruction of the caller */
    xsltTransformContextPtr st_ctxt; /* Transform context pointer */
    int st_locals_start;     /* Our first entry in ctxt->varsTab[] */
    int st_locals_stop;      /* Our last entry in ctxt->varsTab[] */
    unsigned st_flags; /* STF_* flags for this stack frame */
} slaxDebugStackFrame_t;

/* Flags for st_flags */
#define STF_STOPWHENPOP	(1<<0) /* Stop when this frame is popped */
#define STF_PARAM		(1<<1) /* Frame is a with-param insn */

TAILQ_HEAD(slaxDebugStack_s, slaxDebugStackFrame_s) slaxDebugStack;

/*
 * Double linked list to hold the breakpoints
 */
typedef struct slaxDebugBreakpoint_s {
    TAILQ_ENTRY(slaxDebugBreakpoint_s) dbp_link;
    xmlNodePtr dbp_inst;	/* Node we are breaking on */
    uint dbp_num;		/* Breakpoint number */
} slaxDebugBreakpoint_t;

TAILQ_HEAD(slaxDebugBpList_s, slaxDebugBreakpoint_s) slaxDebugBreakpoints;

static uint slaxDebugBreakpointNumber;

/*
 * Various display mode 
 */
int slaxDebugDisplayMode;
#define DEBUG_MODE_CLI		1 /* Normal CLI operations */
#define DEBUG_MODE_EMACS	2 /* gdb/emacs mode */
#define DEBUG_MODE_PROFILER	3 /* Profiler only */

#define NAME(_x) (((_x) && (_x)->name) ? (_x)->name : null)

/**
 * Return the current debugger state object.  This is currently
 * just a single global instance, but if we ever support threads,
 * it will be in thread-specific data.
 * @return current debugger state object
 */
static slaxDebugState_t *
slaxDebugGetState (void)
{
    return &slaxDebugState;
}

static xsltStylesheetPtr
slaxDebugGetFile (xsltStylesheetPtr style, const char *filename)
{
    char buf[BUFSIZ];
    char *slash;
    const char **inc;

    for ( ; style; style = style->next) {
	if (streq((const char *) style->doc->URL, filename))
	    return style;

	for (inc = slaxDebugIncludes; inc && *inc; inc++) {
	    snprintf(buf, sizeof(buf), "%s%s", *inc, filename);
	    if (streq((const char *) style->doc->URL, buf))
		return style;
	}
    
	/*
	 * Just the filname match
	 */
	slash = strrchr((const char *) style->doc->URL, '/');
	if (slash && streq(slash + 1, filename))
	    return style;

	if (style->imports) {
	    xsltStylesheetPtr answer;
	    answer = slaxDebugGetFile(style->imports, filename);
	    if (answer)
		return answer;
	}
    }

    return NULL;
}

/*
 * Return XML node for given template name  
 */
static xmlNodePtr
slaxDebugGetTemplateNodebyName (slaxDebugState_t *statep, const char *name)
{
    xsltTemplatePtr tmp;

    for (tmp = statep->ds_script->templates; tmp; tmp = tmp->next) {
	if ((tmp->match && streq((const char *) tmp->match, name))
	    || (tmp->name && streq((const char *) tmp->name, name)))
	    return tmp->elem;
    }

    return NULL;
}

static xsltTemplatePtr
slaxDebugGetTemplate (slaxDebugState_t *statep, xmlNodePtr inst)
{
    xsltTemplatePtr tmp;

    for ( ; inst; inst = inst->parent) {
	if (slaxNodeIsXsl(inst, ELT_TEMPLATE)) {
	    
	    for (tmp = statep->ds_script->templates; tmp; tmp = tmp->next) {
		if (tmp->elem == inst)
		    return tmp;
	    }

	    return NULL;
	}
    }

    return NULL;
}

static xmlNodePtr
slaxDebugGetNodeByLine (xmlNodePtr node, int lineno)
{
    for ( ; node; node = node->next) {
	if (lineno == xmlGetLineNo(node))
	    return node;

	if (node->children) {
	    xmlNodePtr answer;
	    answer = slaxDebugGetNodeByLine(node->children, lineno);
	    if (answer)
		return answer;
	}
    }

    return NULL;
}

/*
 * Return xmlnode for the given line number
 */
static xmlNodePtr
slaxDebugGetNodeByFilename (slaxDebugState_t *statep,
		  const char *filename, int lineno)
{
    xsltStylesheetPtr style;
    xmlNodePtr node;

    style = slaxDebugGetFile(statep->ds_script, filename);
    if (style == NULL)
	return NULL;

    node = slaxDebugGetNodeByLine(style->doc->children, lineno);

    return node;
}

/*
 * Return xmlnode for the given script:linenum
 */
static xmlNodePtr
slaxDebugGetScriptNode (slaxDebugState_t *statep, const char *arg)
{
    xsltStylesheetPtr style = NULL;
    xmlNodePtr node;
    char *script = ALLOCADUP(arg);
    char *cp, *endp;
    int lineno;

    cp = strchr(script, ':');
    if (cp == NULL)
	return NULL;
    *cp++ = '\0';

    lineno = strtol(cp, &endp, 0);
    if (lineno <= 0 || cp == endp)
	return NULL;

    style = slaxDebugGetFile(statep->ds_script, script);
    if (!style)
	return NULL;

    /*
     * Get the node for the given linenumber from the stylesheet
     */
    node = slaxDebugGetNodeByLine((xmlNodePtr) style->doc, lineno);

    return node;
}

/*
 * Print the given XPath object
 */
static void
slaxDebugOutputXpath (xmlXPathObjectPtr xpath, const char *tag, int full)
{
    if (xpath == NULL)
	return;

    if (tag == NULL)
	tag = "";

    switch (xpath->type) {
    case XPATH_BOOLEAN:
	slaxOutput("%s[boolean] %s", tag, xpath->boolval ? "true" : "false");
	break;

    case XPATH_NUMBER:
	slaxOutput("%s[number] %lf", tag, xpath->floatval);
	break;

    case XPATH_STRING:
	if (xpath->stringval)
	    slaxOutput("%s[string] \"%s\"", tag, xpath->stringval);
	break;

    case XPATH_NODESET:
	if (full) {
	    slaxOutput("%s[node-set]%s (%d)", tag,
		       xpath->nodesetval ? "" : " [null]",
		       xpath->nodesetval ? xpath->nodesetval->nodeNr : 0);
	    if (xpath->nodesetval)
		slaxOutputNodeset(xpath->nodesetval);
	} else {
	    xmlNodeSetPtr ns = xpath->nodesetval;
	    if (ns->nodeNr == 0)
		ns = NULL;

	    slaxOutput("%s[node-set]%s (%d)%s%s%s", tag,
		       xpath->nodesetval ? "" : " [null]",
		       ns ? ns->nodeNr : 0,
		       ns ? " <" : "",
		       ns ? ns->nodeTab[0]->name : null,
		       ns ? "> ...." : "");
	}
	break;

    case XPATH_XSLT_TREE:
	slaxOutput("%s[rtf]%s (%d)", tag,
			xpath->nodesetval ? "" : " [null]",
			xpath->nodesetval ? xpath->nodesetval->nodeNr : 0);
	if (xpath->nodesetval)
	    slaxOutputNodeset(xpath->nodesetval);
	break;

    default:
	break;
    }
}
 
/*
 * Clear all breakpoints
 */
static void
slaxDebugClearBreakpoints (void)
{
    slaxDebugBreakpoint_t *dbp;

    for (;;) {
	dbp = TAILQ_FIRST(&slaxDebugBreakpoints);
	if (dbp == NULL)
	    break;
	TAILQ_REMOVE(&slaxDebugBreakpoints, dbp, dbp_link);
    }

    slaxDebugBreakpointNumber = 0;
}

/*
 * Clear stacktrace, basically delete the linked list
 */
static void
slaxDebugClearStacktrace (void)
{
    slaxDebugStackFrame_t *dsfp;

    for (;;) {
	dsfp = TAILQ_FIRST(&slaxDebugStack);
	if (dsfp == NULL)
	    break;
	TAILQ_REMOVE(&slaxDebugStack, dsfp, st_link);
    }

    slaxDebugBreakpointNumber = 0;
}

static void
slaxDebugMakeRelativePath (const char *src_f, const char *dest_f,
			   char *relative_path, int size)
{
    int slen, dlen, i, j, slash, n;
    const char *f;

    slen = strlen(src_f);
    dlen = strlen(dest_f);

    i = 0;
    j = 0;
    while (src_f[i] && dest_f[j] && src_f[i] == dest_f[j]) {
        i++;
        j++;
    }

    /*
     * Both the file are same, just return the filename
     */
    if (!src_f[i]) {
        f = strrchr(src_f, '/');
        f = f ? f + 1 : src_f;
        snprintf(relative_path, size, "%s", f);
        return;
    }

    /*
     * Find number of / in src_f
     */
    slash = 0;
    while (src_f[i]) {
        if ((src_f[i] == '/') && (src_f[i + 1] != '/'))
            slash++;
        i++;
    }

    /*
     * Both are in same dir, just return file
     */
    if (!slash) {
        snprintf(relative_path, size, "%s", (dest_f + j));
        return;
    }

    /*
     * Append number of ../
     */
    n = 0;
    while (slash) {
        n += snprintf(relative_path + n, size - n, "../");
        slash--;
    }

    snprintf(relative_path + n, size, "%s", (dest_f + j));
}

/*
 * Return the line for given linenumber from file.
 *
 * Very inefficient way, we read the file from begining till we reach the 
 * given linenumber, should be fine for now. May be we will optimize when we
 * implement 'list' command.
 */
static int
slaxDebugOutputScriptLines (slaxDebugState_t *statep, const char *filename,
			    int start, int stop)
{
    FILE *fp;
    int count = 0;
    const char *cp;
    char line[BUFSIZ];
    int len;

    if (filename == NULL || start == 0 || stop == 0)
	return TRUE;

    fp = fopen(filename, "r");
    if (fp == NULL) {
	/* 
	 * do not print error message, the line won't get displayed that is 
	 * fine
	 */
	return TRUE;
    }

    for (;;) {
	if (fgets(line, sizeof(line), fp) == NULL) {
	    count += 1;
	    break;
	}
	/*
	 * Since we are only reading line_size per iteration, if the current 
	 * line length is more than line_size then we may read same line in
	 * more than one iteration, so increment the count only when the last 
	 * character is '\n'
	 */
	if (line[strlen(line) - 1] == '\n')
	    count += 1;

	if (count >= start)
	    break;
    }

    cp = strrchr(filename, '/');
    cp = cp ? cp + 1 : filename;

    len = strlen(line);
    if (len > 0 && line[len - 1] == '\n')
	line[len - 1] = '\0';

    /* Ensure we get at least one line of output */
    if (count > stop)
	stop = count + 1;

    while (count < stop) {
	slaxOutput("%s:%d: %s", cp, count, line);

	if (fgets(line, sizeof(line), fp) == NULL) {
	    count += 1;
	    break;
	}

	len = strlen(line);
	if (len > 0 && line[len - 1] == '\n') {
	    line[len - 1] = '\0';
	    count += 1;
	}
    }

    if (slaxDebugDisplayMode == DEBUG_MODE_EMACS) {
	char rel_path[BUFSIZ];

	/*
	 * In emacs path should be relative to remote default directory,
	 * so print the relative path of the current file from main stylesheet
	 */
	cp = (const char *) statep->ds_script->doc->URL;
	slaxDebugMakeRelativePath(cp, filename, rel_path, sizeof(rel_path));
	
	slaxOutput("%c%c%s:%d:0", 26, 26, rel_path, start);
    }

    fclose(fp);
    return FALSE;
}

/**
 * Split the input given by users into tokens
 * @buf buffer to split
 * @args array for args
 * @maxargs length of args
 * @return number of args
 * @bug needs to handle escapes and quotes
 */
static int
slaxDebugSplitArgs (char *buf, const char **args, int maxargs)
{
    static const char wsp[] = " \t\n\r";
    int i;
    char *s;

    for (i = 0; (s = strsep(&buf, wsp)) && i < maxargs - 1; i++) {
	args[i] = s;

	if (buf)
	    buf += strspn(buf, wsp);
    }

    args[i] = NULL;

    return i;
}

/*
 * Check if the breakpoint is available for curnode being executed
 */
static int
slaxDebugCheckBreakpoint (slaxDebugState_t *statep,
			  xmlNodePtr node, int reached)
{
    slaxDebugBreakpoint_t *dbp;

    if (statep->ds_stop_at && statep->ds_stop_at == node) {
	if (reached) {
	    slaxOutput("Reached stop at %s:%ld",
			    node->doc->URL, xmlGetLineNo(node));
	    xsltSetDebuggerStatus(XSLT_DEBUG_INIT);
	}

	statep->ds_stop_at = NULL; /* One time only */
	return TRUE;
    }

    TAILQ_FOREACH(dbp, &slaxDebugBreakpoints, dbp_link) {
	if (dbp->dbp_inst == node) {
	    if (reached) {
		slaxOutput("Reached breakpoint %d, at %s:%ld", 
				dbp->dbp_num, node->doc->URL,
				xmlGetLineNo(node));
		xsltSetDebuggerStatus(XSLT_DEBUG_INIT);
	    }
	    return TRUE;
	}
    }

    return FALSE;
}

static char *
slaxDebugTemplateInfo (xsltTemplatePtr template, char *buf, int bufsiz)
{
    char *cp = buf, *ep = buf + bufsiz;

    if (template == NULL) {
	strlcpy(buf, "[global]", bufsiz);
	return buf;
    }

    if (template->name)
	SNPRINTF(cp, ep, "template %s ", template->name);

    if (template->match)
	SNPRINTF(cp, ep, "match %s", template->match);

    /* Trim trailing space */
    if (cp > buf && cp[-1] == ' ')
	cp[-1] = '\0';

    return buf;
}

static void
slaxDebugCallFlow (slaxDebugState_t *statep, xsltTemplatePtr template,
		   xmlNodePtr inst, const char *tag)
{
    char buf[BUFSIZ];

    slaxOutput("callflow: %u: %s <%s%s%s> in %s at %s%s%ld",
	statep->ds_stackdepth, tag,
	(inst && inst->ns && inst->ns->prefix) ? inst->ns->prefix : null,
	(inst && inst->ns && inst->ns->prefix) ? ":" : "",
	NAME(inst), slaxDebugTemplateInfo(template, buf, sizeof(buf)),
		    (inst->doc && inst->doc->URL) ? inst->doc->URL : null,
	(inst->doc && inst->doc->URL) ? ":" : "",
	inst ? xmlGetLineNo(inst) : 0);
}

static xmlNodePtr
slaxDebugGetNode (slaxDebugState_t *statep, const char *spec)
{
    xmlNodePtr node;
    int lineno;

    /*
     * Break on the current line
     */
    if (spec == NULL)
	return statep->ds_inst;

    /*
     * scriptname:linenumber format
     */
    if (strchr(spec, ':')) {
	node = slaxDebugGetScriptNode(statep, spec);

	/* If it wasn't foo:34, maybe it's foo:bar */
	if (node == NULL)
	    node = slaxDebugGetTemplateNodebyName(statep, spec);
	return node;
    }

    /*
     * simply linenumber, put breakpoint in the cur script
     */
    if ((lineno = atoi(spec)) > 0) {
	xmlDocPtr docp = statep->ds_inst
	    ? statep->ds_inst->doc : statep->ds_script->doc;
	const char *fname =  (const char *) docp->URL;
	return slaxDebugGetNodeByFilename(statep, fname, lineno);
    }

    /*
     * template name?
     */
    return slaxDebugGetTemplateNodebyName(statep, spec);
}

static int
slaxDebugCheckAbbrev (const char *name, int min, const char *value, int len)
{
    return (len >= min && strncmp(name, value, len) == 0);
}

static inline int
slaxDebugIsAbbrev (const char *name, const char *value)
{
    return value ? slaxDebugCheckAbbrev(name, 1, value, strlen(value)) : 0;
}

/*
 * 'break' command
 */
static void
slaxDebugCmdBreak (DC_ARGS)
{
    xmlNodePtr node = NULL;
    slaxDebugBreakpoint_t *bp;

    node = slaxDebugGetNode(statep, argv[1]);
    if (node == NULL) {
	slaxOutput("Target \"%s\" is not defined", argv[1]);
	return;
    }

    if (slaxDebugCheckBreakpoint(statep, node, FALSE)) {
	slaxOutput("Duplicate breakpoint");
	return; 
    }

    /*
     * Create a record of the breakpoint and add it to the list
     */
    bp = xmlMalloc(sizeof(*bp));
    if (bp == NULL)
	return;

    bzero(bp, sizeof(*bp));
    bp->dbp_num = ++slaxDebugBreakpointNumber;
    bp->dbp_inst = node;
    TAILQ_INSERT_TAIL(&slaxDebugBreakpoints, bp, dbp_link);

    slaxOutput("Breakpoint %d at file %s, line %ld",
		    bp->dbp_num, 
		    node->doc->URL, xmlGetLineNo(node));  
}

static int
slaxDebugCheckDone (slaxDebugState_t *statep UNUSED)
{
    int status = xsltGetDebuggerStatus();
    if (status == 0 || status == XSLT_DEBUG_DONE) {
	slaxOutput("The script is not being run.");
	return TRUE;
    }

    return FALSE;
}

static int
slaxDebugCheckStart (slaxDebugState_t *statep UNUSED)
{
    int status = xsltGetDebuggerStatus();

    if (status == 0) {
	statep->ds_flags |= DSF_RESTART | DSF_DISPLAY;
	return FALSE;

    } else if (status == XSLT_DEBUG_DONE) {
	slaxOutput("The script is not being run.");
	return TRUE;
    }

    return FALSE;
}

/*
 * 'continue' command
 */
static void
slaxDebugCmdContinue (DC_ARGS)
{
    xmlNodePtr node;

    if (slaxDebugCheckStart(statep))
	return;

    if (argv[1]) {
	node = slaxDebugGetNode(statep, argv[1]);
	if (node == NULL) {
	    slaxOutput("Unknown location: %s", argv[1]);
	    return;
	}

	statep->ds_stop_at = node;
    }

    xsltSetDebuggerStatus(XSLT_DEBUG_CONT);
    statep->ds_flags |= DSF_DISPLAY | DSF_CONTINUE;
}

/*
 * 'delete' command
 */
static void
slaxDebugCmdDelete (DC_ARGS)
{
    static const char prompt[] = "Delete all breakpoints? (yes/no) ";
    uint num = 0;
    slaxDebugBreakpoint_t *dbpp;
    char *cp;

    /*
     * If no argument is provided ask for confirmation and delete all 
     * breakpoints
     */
    if (argv[1] == NULL) {
	cp = slaxInput(prompt, 0);

	if (!streq(cp, "y") && !streq(cp, "yes"))
	    return;

	slaxDebugClearBreakpoints();
	slaxOutput("Deleted all breakpoints");
	xmlFree(cp);
	return;
    }

    num = atoi(argv[1]);
    if (num <= 0) {
	slaxOutput("Invalid breakpoint number");
	return;
    }

    /*
     * Inefficient way of finding the patricia node for a breakpoint number,
     * but still ok, should be fine for debugger
     */
    TAILQ_FOREACH(dbpp, &slaxDebugBreakpoints, dbp_link) {
	if (dbpp->dbp_num == num) {
	    TAILQ_REMOVE(&slaxDebugBreakpoints, dbpp, dbp_link);
	    slaxOutput("Deleted breakpoint '%d'", num);
	    return;
	}
    }

    slaxOutput("Breakpoint '%d' not found", num);
}

/*
 * 'help' command
 */
static void
slaxDebugCmdHelp (DC_ARGS)
{
    slaxDebugCommand_t *cmdp;

    if (argv[1]) {
	cmdp = slaxDebugGetCommand(argv[1]);
	if (cmdp == NULL)
	    slaxOutput("Unknown command \"%s\".  Try \"help\".", argv[0]);
	else if (cmdp->dc_helpfunc)
	    cmdp->dc_helpfunc(statep);
	else if (cmdp->dc_help)
	    slaxOutput("  %s", cmdp->dc_help);
	else 
	    slaxOutput("No help is available");
	
    } else {
	slaxOutput("List of commands:");
	for (cmdp = slaxDebugCmdTable; cmdp->dc_command; cmdp++) {
	    if (cmdp->dc_help)
		slaxOutput("  %s", cmdp->dc_help);
	}
	slaxOutput("%s", "");	/* Avoid compiler warning */
	slaxOutput("Command name abbreviations are allowed");
    }
}

static void
slaxDebugHelpInfo (DH_ARGS)
{
    slaxOutput("List of commands:");
    slaxOutput("  info breakpoints  Display current breakpoints");
    slaxOutput("  info profile [brief]  Report profiling information");
}

static void
slaxDebugInfoBreakpoints (slaxDebugState_t *statep)
{
    xsltTemplatePtr template;
    slaxDebugBreakpoint_t *dbp;
    const char *tag;
    char buf[BUFSIZ];
    int hit = 0;

    TAILQ_FOREACH(dbp, &slaxDebugBreakpoints, dbp_link) {
	if (++hit == 1)
	    slaxOutput("List of breakpoints:");

	tag = (dbp->dbp_inst == statep->ds_node) ? "*" : " ";
	template = slaxDebugGetTemplate(statep, dbp->dbp_inst);

	slaxOutput("    #%d %s at %s:%ld",
		   dbp->dbp_num,
		   slaxDebugTemplateInfo(template, buf, sizeof(buf)),
		   (dbp->dbp_inst && dbp->dbp_inst->doc)
		   	? dbp->dbp_inst->doc->URL : null,
		   xmlGetLineNo(dbp->dbp_inst));
	}

    if (hit == 0)
	slaxOutput("No breakpoints.");
}

/*
 * 'info' command
 */
static void
slaxDebugCmdInfo (DC_ARGS)
{
    if (argv[1] == NULL || slaxDebugIsAbbrev("breakpoints", argv[1])) {
	slaxDebugInfoBreakpoints(statep);

    } else if (slaxDebugIsAbbrev("profile", argv[1])) {
	int brief = (argv[2] && slaxDebugIsAbbrev("brief", argv[2]));
	slaxProfReport(brief);

    } else if (slaxDebugIsAbbrev("help", argv[1])) {
	slaxDebugHelpInfo(statep);

    } else
	slaxOutput("Undefined command: \"%s\".  Try \"help\".", argv[1]);
}

/*
 * 'list' command
 */
static void
slaxDebugCmdList (DC_ARGS)
{
    xmlNodePtr node = slaxDebugGetNode(statep, argv[1]);

    if (node) {
	int line_no = xmlGetLineNo(node);
	if (node->doc) {
	    slaxDebugOutputScriptLines(statep, (const char *) node->doc->URL,
				       line_no, line_no + 10);
	} else {
	    slaxOutput("target lacks filename: %s", argv[1]);
	}
    }
}

/*
 * Set the display mode
 */
static void
slaxDebugCmdMode (DC_ARGS)
{
    if (streq(argv[1], "emacs"))
	slaxDebugDisplayMode = DEBUG_MODE_EMACS;
    else if (streq(argv[1], "cli"))
	slaxDebugDisplayMode = DEBUG_MODE_CLI;
}

/*
 * 'finish' command
 */
static void
slaxDebugCmdFinish (DC_ARGS)
{
    slaxDebugStackFrame_t *dsfp, *last = NULL;

    if (slaxDebugCheckDone(statep))
	return;

    /*
     * Walk the stack linked list in reverse order, looking for the
     * lowest template.  When we find it, turn on the "stop-when-pop"
     * flag so we'll stop when it's popped by dropFrame.  Then mark
     * the debugger for "continue".
     */
    TAILQ_FOREACH_REVERSE(dsfp, &slaxDebugStack, slaxDebugStack_s, st_link) {
	if (slaxNodeIsXsl(dsfp->st_inst, ELT_TEMPLATE)) {
	    dsfp->st_flags |= STF_STOPWHENPOP;
	    statep->ds_flags |= DSF_DISPLAY;
	    xsltSetDebuggerStatus(XSLT_DEBUG_CONT);
	    return;
	}
	last = dsfp;
    }

    if (last) {
	last->st_flags |= STF_STOPWHENPOP;
	statep->ds_flags |= DSF_DISPLAY;
	xsltSetDebuggerStatus(XSLT_DEBUG_CONT);
	return;
    }

    slaxOutput("template not found");
}

/*
 * 'step' command
 */
static void
slaxDebugCmdStep (DC_ARGS)
{
    if (slaxDebugCheckStart(statep))
	return;

    xsltSetDebuggerStatus(XSLT_DEBUG_STEP);
    statep->ds_flags |= DSF_DISPLAY;
}

/*
 * 'next' command.  Is we are on a "call", then act like "over".
 * Otherwise, act like "step".
 */
static void
slaxDebugCmdNext (DC_ARGS)
{
    if (slaxDebugCheckStart(statep))
	return;

    if (slaxNodeIsXsl(statep->ds_inst, ELT_CALL_TEMPLATE)) {
	xsltSetDebuggerStatus(XSLT_DEBUG_OVER);
	statep->ds_flags |= DSF_OVER | DSF_DISPLAY;
    } else {
	xsltSetDebuggerStatus(XSLT_DEBUG_STEP);
	statep->ds_flags |= DSF_DISPLAY;
    }
}

/*
 * 'over' command
 */
static void
slaxDebugCmdOver (DC_ARGS)
{
    if (slaxDebugCheckDone(statep))
	return;

    xsltSetDebuggerStatus(XSLT_DEBUG_OVER);
    statep->ds_flags |= DSF_OVER | DSF_DISPLAY;
}

/**
 * 
 * The caller must free the results with xmlXPathFreeObject(res);
 */
static xmlXPathObjectPtr
slaxDebugEvalXpath (slaxDebugState_t *statep, const char *expr)
{
    struct {
	xmlDocPtr o_doc;
	xmlNsPtr *o_nslist;
	xmlNodePtr o_node;
	int o_position;
	int o_contextsize;
	int o_nscount;
    } old;

    xmlNsPtr *nsList;
    int nscount;
    xmlXPathCompExprPtr comp;
    xmlXPathObjectPtr res;
    xmlNodePtr node = statep->ds_node;
    xmlNodePtr inst = statep->ds_inst;
    xsltTransformContextPtr ctxt;
    xmlXPathContextPtr xpctxt;

    if (slaxDebugCheckDone(statep))
	return NULL;

    ctxt = statep->ds_ctxt;
    if (ctxt == NULL)
	return NULL;

    xpctxt = ctxt->xpathCtxt;
    if (xpctxt == NULL)
	return NULL;

    comp = xsltXPathCompile(statep->ds_script, (const xmlChar *) expr);
    if (comp == NULL)
	return NULL;

    nsList = xmlGetNsList(inst->doc, inst);
    for (nscount = 0; nsList && nsList[nscount]; nscount++)
	continue;
    
    /* Save old values */
    old.o_doc = xpctxt->doc;
    old.o_node = xpctxt->node;
    old.o_position = xpctxt->proximityPosition;
    old.o_contextsize = xpctxt->contextSize;
    old.o_nscount = xpctxt->nsNr;
    old.o_nslist = xpctxt->namespaces;

    /* Fill in context */
    xpctxt->node = node;
    xpctxt->namespaces = nsList;
    xpctxt->nsNr = nscount;

    /* Run the compiled expression */
    res = xmlXPathCompiledEval(comp, xpctxt);

    /* Restore saved values */
    xpctxt->doc = old.o_doc;
    xpctxt->node = old.o_node;
    xpctxt->contextSize = old.o_contextsize;
    xpctxt->proximityPosition = old.o_position;
    xpctxt->nsNr = old.o_nscount;
    xpctxt->namespaces = old.o_nslist;

    xmlXPathFreeCompExpr(comp);
    xmlFree(nsList);

    return res;
}

/*
 * 'print' command
 * @bugs need to be more like "eval" functionality (e.g. "print $x/name")
 */
static void
slaxDebugCmdPrint (DC_ARGS)
{
    xmlXPathObjectPtr res;
    char *cp = ALLOCADUP(commandline);

    /* Move over command name */
    while (*cp && !isspace((int) *cp))
	cp += 1;
    while (*cp && isspace((int) *cp))
	cp += 1;

    res = slaxDebugEvalXpath (statep, cp);
    if (res) {
	slaxDebugOutputXpath(res, NULL, TRUE);
	xmlXPathFreeObject(res);
	slaxOutput("%s", ""); /* Avoid compiler warning */
    }
}

static void
slaxDebugHelpProfile (DH_ARGS)
{
    slaxOutput("List of commands:");
    slaxOutput("  profile clear   Clear  profiling information");
    slaxOutput("  profile off     Disable profiling");
    slaxOutput("  profile on      Enable profiling");
    slaxOutput("  profile report [brief]  Report profiling information");
}

/*
 * 'profiler [on|off]' command
 */
static void
slaxDebugCmdProfiler (DC_ARGS)
{
    const char *arg = argv[1];
    int enable = !(statep->ds_flags & DSF_PROFILER);

    if (arg) {
	if (streq("on", arg) || slaxDebugIsAbbrev("yes", arg)
	    || slaxDebugIsAbbrev("enable", arg))
	    enable = TRUE;

	else if (streq("off", arg) || slaxDebugIsAbbrev("no", arg)
		 || slaxDebugIsAbbrev("disable", arg))
	    enable = FALSE;

	else if (slaxDebugIsAbbrev("clear", arg)) {
	    slaxOutput("Clearing profile information");
	    slaxProfClear();
	    return;

	} else if (slaxDebugIsAbbrev("report", arg)) {
	    int brief = (argv[2] && slaxDebugIsAbbrev("brief", argv[2]));
	    slaxProfReport(brief);
	    return;

	} else if (slaxDebugIsAbbrev("help", arg)) {
	    slaxDebugHelpProfile(statep);
	    return;

	} else {
	    slaxOutput("invalid setting: %s", arg);
	    return;
	}
    }

    if (enable) {
	statep->ds_flags |= DSF_PROFILER;
	slaxOutput("Enabling profiler");
    } else {
	statep->ds_flags &= ~DSF_PROFILER;
	slaxOutput("Disabling profiler");
    }
    
}

/*
 * 'where' command
 *  syntax: where [full]
 */
static void
slaxDebugCmdWhere (DC_ARGS)
{
    slaxDebugStackFrame_t *dsfp;
    const char *name;
    const char *filename;
    const char *tag;
    int num = 0;
    char template_info[BUFSIZ];
    char from_info[BUFSIZ];
    int full;
    xmlNodePtr caller;

    if (slaxDebugCheckDone(statep))
	return;

    full = slaxDebugIsAbbrev("full", argv[1]);

    /*
     * Walk the stack linked list in reverse order and print it
     */
    TAILQ_FOREACH(dsfp, &slaxDebugStack, st_link) {
	name = NULL;
	tag = "";

	if (dsfp->st_template) {
	    if (dsfp->st_template->match) {
		name = (const char *) dsfp->st_template->match;
	    } else if (dsfp->st_template->name) {
		name = (const char *) dsfp->st_template->name;
		tag = "()";
	    }
	}

	if (name == NULL && !full)
	    continue;

	if (name)
	    slaxDebugTemplateInfo(dsfp->st_template,
				  template_info, sizeof(template_info));
	else snprintf(template_info, sizeof(template_info),
		      "<%s%s%s>",
		      (dsfp->st_inst && dsfp->st_inst->ns
		       && dsfp->st_inst->ns->prefix)
		      ? dsfp->st_inst->ns->prefix : null,
		      (dsfp->st_inst && dsfp->st_inst->ns
		       && dsfp->st_inst->ns->prefix) ? ":" : "",
		      NAME(dsfp->st_inst));

	caller = dsfp->st_caller ?: dsfp->st_inst;

	filename = strrchr((const char *) caller->doc->URL, '/');
	filename = filename ? filename + 1 : (const char *) caller->doc->URL;

	if (dsfp->st_template && dsfp->st_template->match)
	    snprintf(from_info, sizeof(from_info),
		     " at %s:%ld", filename ?: "", xmlGetLineNo(caller));
	else from_info[0] = '\0';

	slaxOutput("#%d %s%s%s", num, template_info, tag, from_info);
	slaxLog("  locals %d .. %d",
		   dsfp->st_locals_start, dsfp->st_locals_stop);

	if (dsfp->st_inst && dsfp->st_ctxt) {
	    xsltStackElemPtr cur;
	    int start, stop, i;
	    char tbuf[BUFSIZ];
	    xsltTransformContextPtr ctxt = dsfp->st_ctxt;

	    /*
	     * We display the parameter list for the template
	     */
	    start = (dsfp->st_locals_start > 0) ? dsfp->st_locals_start : 0;
	    stop = dsfp->st_locals_stop ?:
		dsfp->st_locals_start ? ctxt->varsNr : 0;
	    for (i = start; i < stop; i++) {
		cur = ctxt->varsTab[i];
		if (cur == NULL)
		    continue;

		if (!full && cur->level >= 0)
		    continue;

		slaxLog("    $%s (%d)", cur->name, cur->level);
		snprintf(tbuf, sizeof(tbuf), "    $%s = ", cur->name);

		if (cur->value) {
		    slaxDebugOutputXpath(cur->value, tbuf, FALSE);
		} else {
		    slaxOutput("%sNULL", tbuf);
		}
	    }
	}

	num += 1;
    }

    if (num == 0)
	slaxOutput("call stack is empty");
}

static void
slaxDebugHelpCallFlow (DH_ARGS)
{
    slaxOutput("List of commands:");
    slaxOutput("  callflow off    Disable callflow tracing");
    slaxOutput("  callflow on     Enable callflow tracing");
}

/**
 * 'callflow' command
 */
static void
slaxDebugCmdCallFlow (DC_ARGS)
{
    const char *arg = argv[1];
    int enable = !(statep->ds_flags & DSF_CALLFLOW);

    if (arg) {
	if (streq("on", arg) || slaxDebugIsAbbrev("yes", arg)
		|| slaxDebugIsAbbrev("enable", arg))
	    enable = TRUE;

	else if (streq("off", arg) || slaxDebugIsAbbrev("no", arg)
		 || slaxDebugIsAbbrev("disable", arg))
	    enable = FALSE;

	else if (slaxDebugIsAbbrev("help", arg)) {
	    slaxDebugHelpCallFlow(statep);
	    return;

	} else {
	    slaxOutput("invalid setting: %s", arg);
	    return;
	}
    }

    if (enable) {
	statep->ds_flags |= DSF_CALLFLOW;
	slaxOutput("Enabling callflow");
    } else {
	statep->ds_flags &= ~DSF_CALLFLOW;
	slaxOutput("Disabling callflow");
    }
}
    
/*
 * 'reload' command
 */
static void
slaxDebugCmdReload (DC_ARGS)
{
    int status = xsltGetDebuggerStatus();
    int restart;

    if (status != 0 && status != XSLT_DEBUG_DONE
		&& status != XSLT_DEBUG_QUIT) {
	const char warning[] =
	    "The script being debugged has been started already.";
	const char prompt[]
	    = "Reload and restart it from the beginning? (y or n) ";
	char *input;

	slaxOutput(warning);
	input = slaxInput(prompt, 0);
	if (input == NULL)
	    return;

	restart = slaxDebugCheckAbbrev("yes", 1, input, strlen(input));
	xmlFree(input);

	if (!restart)
	    return;
    }

    /* Tell the xslt engine to stop */
    xsltStopEngine(statep->ds_ctxt);

    xsltSetDebuggerStatus(XSLT_DEBUG_QUIT);
    statep->ds_flags |= DSF_RELOAD;
}
    
/*
 * 'run' command
 */
static void
slaxDebugCmdRun (DC_ARGS)
{
    int status = xsltGetDebuggerStatus();
    int restart;

    if (status != 0 && status != XSLT_DEBUG_DONE
		&& status != XSLT_DEBUG_QUIT) {
	const char warning[] =
	    "The script being debugged has been started already.";
	const char prompt[] = "Start it from the beginning? (y or n) ";
	char *input;

	slaxOutput(warning);
	input = slaxInput(prompt, 0);
	if (input == NULL)
	    return;

	restart = slaxDebugCheckAbbrev("yes", 1, input, strlen(input));
	xmlFree(input);

	if (!restart)
	    return;
    }

    /* Tell the xslt engine to stop */
    xsltStopEngine(statep->ds_ctxt);

    xsltSetDebuggerStatus(XSLT_DEBUG_QUIT);
    statep->ds_flags |= DSF_RESTART | DSF_DISPLAY | DSF_CONTINUE;
}
    
/*
 * 'quit' command
 */
static void
slaxDebugCmdQuit (DC_ARGS)
{
    static const char prompt[] =
	"The script is running.  Exit anyway? (y or n) ";
    int status = xsltGetDebuggerStatus();

    if (status != 0 && status != XSLT_DEBUG_DONE) {
	char *input = slaxInput(prompt, 0);

	if (input == NULL
	    	|| !slaxDebugCheckAbbrev("yes", 1, input, strlen(input)))
	    return;
    }

    /*
     * Some parts of libxslt tests the global debug status value and
     * other parts use the context variable, so we have to set them
     * both.  If we've "quit", then there's no context to set.
     */
    xsltSetDebuggerStatus(XSLT_DEBUG_QUIT);
    if (statep->ds_ctxt)
	statep->ds_ctxt->debugStatus = XSLT_DEBUG_QUIT;

    /* Tell the xslt engine to stop */
    xsltStopEngine(statep->ds_ctxt);

}

/* ---------------------------------------------------------------------- */

static slaxDebugCommand_t slaxDebugCmdTable[] = {
    { "break",	       1, slaxDebugCmdBreak,
      "break [loc]     Add a breakpoint at [file:]line or template",
      NULL,
    },

    { "bt",	       1, slaxDebugCmdWhere, NULL, NULL }, /* Hidden */

    { "callflow",      2, slaxDebugCmdCallFlow,
      "callflow [val]  Enable call flow tracing",
      slaxDebugHelpCallFlow,
    },

    { "continue",      1, slaxDebugCmdContinue,
      "continue [loc]  Continue running the script",
      NULL,
    },

    { "delete",	       1, slaxDebugCmdDelete,
      "delete [num]    Delete all (or one) breakpoints",
      NULL,
    },
 
    { "finish",	       1, slaxDebugCmdFinish,
      "finish          Finish the current template",
      NULL,
    },

    { "help",	       1, slaxDebugCmdHelp,
      "help            Show this help message",
      NULL,
    },

    { "info",	       1, slaxDebugCmdInfo,
      "info            Showing info about the script being debugged",
      slaxDebugHelpInfo,
    },

    { "?",	       1, slaxDebugCmdHelp, NULL, NULL }, /* Hidden */

    { "list",	       1, slaxDebugCmdList,
      "list [loc]      List contents of the current script",
      NULL,
    },

    { "mode",	       1, slaxDebugCmdMode, NULL, NULL }, /* Hidden */

    { "next",	       1, slaxDebugCmdNext,
      "next            Execute the over instruction, stepping over calls",
      NULL,
    },

    { "over",	       1, slaxDebugCmdOver,
      "over            Execute the current instruction hierarchy",
      NULL,
    },

    { "print",	       1, slaxDebugCmdPrint,
      "print <xpath>   Print the value of an XPath expression",
      NULL,
    },

    { "profile",       2, slaxDebugCmdProfiler,
      "profile [val]   Turn profiler on or off",
      slaxDebugHelpProfile,
    },

    { "reload",	       3, slaxDebugCmdReload,
      "reload          Reload the script contents",
      NULL,
    },

    { "run",	       3, slaxDebugCmdRun,
      "run             Restart the script",
      NULL,
    },

    { "step",	       1, slaxDebugCmdStep,
      "step            Execute the next instruction, stepping into calls",
      NULL,
    },

    { "where",	       1, slaxDebugCmdWhere,
      "where           Show the backtrace of template calls",
      NULL,
    },

    { "quit",	       1, slaxDebugCmdQuit,
      "quit            Quit debugger",
      NULL,
    },

    { NULL, 0, NULL, NULL, NULL }
};

/**
 * Find a command with the given name
 * @name name of command
 * @return command structure for that command
 */
static slaxDebugCommand_t *
slaxDebugGetCommand (const char *name)
{
    slaxDebugCommand_t *cmdp;
    int len = strlen(name);

    for (cmdp = slaxDebugCmdTable; cmdp->dc_command; cmdp++) {
	if (slaxDebugCheckAbbrev(cmdp->dc_command, cmdp->dc_min, name, len))
	    return cmdp;
    }

    return NULL;
}

/**
 * Run the command entered by user
 * @input the command to be run
 */
static void
slaxDebugRunCommand (slaxDebugState_t *statep, char *input)
{
    const char *argv[MAXARGS];
    slaxDebugCommand_t *cmdp = NULL;
    char *input_copy = ALLOCADUP(input);

    slaxDebugSplitArgs(input, argv, MAXARGS);
    if (argv[0] == NULL)
	return;

    cmdp = slaxDebugGetCommand(argv[0]);
    if (cmdp) {
	cmdp->dc_func(statep, input_copy, argv);

    } else {
	slaxOutput("Unknown command \"%s\".  Try \"help\".", argv[0]);
    }
}

/**
 * Show the debugger prompt and wait for input
 */
static int 
slaxDebugShell (slaxDebugState_t *statep)
{
    static char prev_input[BUFSIZ];
    char *cp, *input;
    static char prompt[] = "(sdb) ";

    if ((statep->ds_flags & DSF_DISPLAY) && statep->ds_inst != NULL) {
	const char *filename = (const char *) statep->ds_inst->doc->URL;
	int line_no = xmlGetLineNo(statep->ds_inst);
	slaxDebugOutputScriptLines(statep, filename, line_no, line_no + 1);
	statep->ds_flags &= ~DSF_DISPLAY;
    }

    input = slaxInput(prompt, SIF_HISTORY);
    if (input == NULL)
	return -1;

    statep->ds_count += 1;

    /* Trim newlines */
    cp = input + strlen(input);
    if (cp > input && cp[-1] == '\n')
	*cp = '\0';

    cp = input;
    while (isspace((int) *cp))
	cp++;

    if (*cp == '\0') {
	cp = prev_input;
    } else {
	strlcpy(prev_input, cp, sizeof(prev_input));
    }

    statep->ds_flags |= DSF_INSHELL;
    slaxDebugRunCommand(statep, cp);
    statep->ds_flags &= ~DSF_INSHELL;

    if (input)
	xmlFree(input);

    return 0;
}

/**
 * Are we at the same spot as the last time we stopped?  This is
 * tricky question because (a) a single SLAX statement can turn into
 * multiple XSLT elements, (b) hitting the same breakpoint doesn't
 * mean you are on the same instruction, and (c) for-each loops can
 * have only one instruction.  We do our best to avoid these pits.
 *
 * @statep the slax debugger state handle
 * @inst the current instruction
 * @returns TRUE if the current instruction is that same as the last one
 */
static int
slaxDebugSameSlax (slaxDebugState_t *statep, xmlNodePtr inst)
{
    if (statep->ds_last_inst == NULL || inst == NULL)
	return FALSE;

    if (statep->ds_inst == inst)
	return TRUE;

    if (statep->ds_inst->doc && statep->ds_inst->doc == inst->doc) {
	int lineno = xmlGetLineNo(inst);
	if (lineno > 0 && lineno == xmlGetLineNo(statep->ds_inst))
	    return TRUE;
    }

    return FALSE;
}

/**
 * Called as callback function from libxslt before each statement is executed.
 * Here is where we handle all our debugger logic.
 *
 * @inst instruction being executed (never null)
 * @node context node (if null, we're just starting to evaluate a
 *       global variable) (and called from xsltEvalGlobalVariable)
 * @template  template being executed (if null, we're called while
 *        evaluating a global variable)
 * @ctxt transformation context
 */
static void
slaxDebugHandler (xmlNodePtr inst, xmlNodePtr node,
		  xsltTemplatePtr template, xsltTransformContextPtr ctxt)
{
    slaxDebugState_t *statep = slaxDebugGetState();
    slaxDebugStackFrame_t *dsfp;
    int status;
    char buf[BUFSIZ];

    /* We don't want to be recursive (via the 'print' command) */
    if (statep->ds_flags & DSF_INSHELL)
	return;

    slaxLog("handleFrame: template %p/[%s], node %p/%s/%d, "
	      "inst %p/%s/%d ctxt %x",
	      template, slaxDebugTemplateInfo(template, buf, sizeof(buf)),
	      node, NAME(node), node ? node->type : 0,
	      inst, NAME(inst), inst ? xmlGetLineNo(inst) : 0, ctxt);

    /*
     * We do not debug text nodes
     */
    if (inst && inst->type == XML_TEXT_NODE)
	return;

    /*
     * When we ask to quit, libxslt might keep going for a while
     */
    status = xsltGetDebuggerStatus();
    if (status == XSLT_DEBUG_DONE || status == XSLT_DEBUG_QUIT)
	return;

    /*
     * If we are on the same _line_ as we were at the previous
     * invocation of slaxDebugShell(), then we want to continue
     * on.  This is required since a single SLAX statement can
     * turn into multiple XSLT elements.  This "same as"
     * condition is very tender.  See also the code in addFrame.
     */
    if (slaxDebugSameSlax(statep, inst))
	return;

    if (statep->ds_flags & DSF_PROFILER)
	slaxProfExit();

    /*
     * Fill in the current state
     */
    statep->ds_inst = inst;
    statep->ds_node = node;
    statep->ds_template = template;
    statep->ds_ctxt = ctxt;

    /*
     * The addFrame callback doesn't get passed the context pointer
     * and we need it to properly record the "varsBase" field, which
     * tells us where local variables start.  Without that, we have
     * to record it on the next handler call.
     */
    if (ctxt && (statep->ds_flags & DSF_FRESHADD)) {
	statep->ds_flags &= ~DSF_FRESHADD;
	dsfp = TAILQ_LAST(&slaxDebugStack, slaxDebugStack_s);
	if (dsfp) {
	    dsfp->st_ctxt = ctxt;
	    dsfp->st_locals_start = ctxt->varsNr;

	    dsfp = TAILQ_PREV(dsfp, slaxDebugStack_s, st_link);
	    if (dsfp) {
		dsfp->st_ctxt = ctxt;
		dsfp->st_locals_start = ctxt->varsBase;
		dsfp->st_locals_stop = ctxt->varsNr;

		/*
		 * Record the last local index for the previous stack frame.
		 */
		dsfp = TAILQ_PREV(dsfp, slaxDebugStack_s, st_link);
		if (dsfp && dsfp->st_ctxt == ctxt)
		    dsfp->st_locals_stop = ctxt->varsBase;
	    }
	}
    }

    slaxDebugCheckBreakpoint(statep, inst, TRUE);

#if 0
    if (statep->ds_flags & DSF_OVER) {
	/*
	 * If we got here with the over flag still on, then there was
	 * no addFrame/dropFrame, so we must have "over"ed a non-call.
	 * Turn off the flag and get out of over mode.
	 */
	statep->ds_flags &= ~DSF_OVER;
	xsltSetDebuggerStatus(XSLT_DEBUG_INIT);
    }
#endif

    statep->ds_flags &= ~DSF_CONTINUE;

    for (;;) {

	status = xsltGetDebuggerStatus();

	switch (status) {
	case XSLT_DEBUG_INIT:
	    /*
	     * If slaxDebugShell() fails, we are looking at EOF,
	     * so shut it down and get out of here.
	     */
	    if (slaxDebugShell(statep) < 0) {
		xsltSetDebuggerStatus(XSLT_DEBUG_QUIT);
		return;
	    }

	    /* Record the last instruction we looked at */
	    statep->ds_last_inst = statep->ds_inst;
	    break;

	case XSLT_DEBUG_QUIT:
	    return;

	case XSLT_DEBUG_STEP:
	    xsltSetDebuggerStatus(XSLT_DEBUG_INIT);
	    /* Fallthru */

	case XSLT_DEBUG_OVER:
	case XSLT_DEBUG_NEXT:
	case XSLT_DEBUG_CONT:
	case XSLT_DEBUG_NONE:
	    if (statep->ds_flags & DSF_PROFILER)
		slaxProfEnter(inst);

	    return;
	}
    }
}

/**
 * Called from libxslt as callback function when template is executed.
 *
 * @template  template being executed (if null, we're called while
 *        evaluating a global variable)
 * @inst instruction node
 */
static int 
slaxDebugAddFrame (xsltTemplatePtr template, xmlNodePtr inst)
{
    slaxDebugState_t *statep = slaxDebugGetState();
    slaxDebugStackFrame_t *dsfp;
    char buf[BUFSIZ];

    /* We don't want to be recursive (via the 'print' command) */
    if (statep->ds_flags & DSF_INSHELL)
	return 0;

    if (statep->ds_flags & DSF_PROFILER)
	slaxProfExit();

    slaxLog("addFrame: template %p/[%s], inst %p/%s/%d (inst %p/%s)",
	      template, slaxDebugTemplateInfo(template, buf, sizeof(buf)),
	      inst, NAME(inst), inst ? xmlGetLineNo(inst) : 0,
	      statep->ds_inst, NAME(statep->ds_inst));

    /*
     * This should never happen, except when it does.
     * Seems to be when the engine can't find the instruction, like
     * an unknown function or template.  Ignore it instead of making
     * a core file.
     */
    if (inst == NULL)
	return 0;

    /*
     * They are two distinct calls for addFrame when a template is
     * invoked.  The sequence goes like this:
     *
     * - handler(call-template)
     * - addFrame(template)
     * - handler(template)
     * - addFrame(template)
     *
     * Looks like both xsltApplyXSLTTemplate() and
     * xsltApplySequenceConstructor() have calls to the debugger.  We
     * pick the outer addFrame to record so we want to skip the inner
     * set.  Our clue is that for the inner set, the template
     * instruction is the same one we recorded in Handler.
     */
    if (inst == statep->ds_inst
	&& (streq((const char *) inst->name, ELT_CALL_TEMPLATE)
	    || streq((const char *) inst->name, ELT_TEMPLATE)))
	return 0;

    if (statep->ds_flags & DSF_CALLFLOW)
	slaxDebugCallFlow(statep, template, inst, "enter");

    /*
     * Store the template backtrace in linked list
     */
    dsfp = xmlMalloc(sizeof(*dsfp));
    if (!dsfp) {
	slaxOutput("memory allocation failure");
	return 0;
    }

    bzero(dsfp, sizeof(*dsfp));
    dsfp->st_depth = statep->ds_stackdepth++;
    dsfp->st_template = template;
    dsfp->st_inst = inst;
    dsfp->st_caller = statep->ds_inst;

    if (inst->ns && inst->ns->href && inst->name
	&& streq((const char *) inst->ns->href, XSL_URI)) {
	if (streq((const char *) inst->name, ELT_WITH_PARAM))
	    dsfp->st_flags |= STF_PARAM;
    }

    if (!(dsfp->st_flags & STF_PARAM)) {
	/* If we're 'over'ing, mark this frame as "stop when pop" */
	if (statep->ds_flags & DSF_OVER) {
	    statep->ds_flags &= ~DSF_OVER;
	    dsfp->st_flags |= STF_STOPWHENPOP;
	    xsltSetDebuggerStatus(XSLT_DEBUG_CONT);
	}
    }

    TAILQ_INSERT_TAIL(&slaxDebugStack, dsfp, st_link);

    statep->ds_flags |= DSF_FRESHADD;

    /*
     * return value > 0 makes libxslt to call slaxDebugDropFrame()
     */
    return 1;
}

/*
 * Called from libxslt when the template execution is over
 */
static void 
slaxDebugDropFrame (void)
{
    slaxDebugState_t *statep = slaxDebugGetState();
    slaxDebugStackFrame_t *dsfp;
    xsltTemplatePtr template;
    char buf[BUFSIZ];
    xmlNodePtr inst;

    /* We don't want to be recursive (via the 'print' command) */
    if (statep->ds_flags & DSF_INSHELL)
	return;

    if (statep->ds_flags & DSF_PROFILER)
	slaxProfExit();

    dsfp = TAILQ_LAST(&slaxDebugStack, slaxDebugStack_s);
    if (dsfp == NULL) {
	slaxLog("dropFrame: null");
	return;
    }

    template = dsfp->st_template;
    inst = dsfp->st_inst;

    slaxLog("dropFrame: %s (%p), inst <%s%s%s> (%p; line %d%s)",
	      slaxDebugTemplateInfo(template, buf, sizeof(buf)),
	      template, 
	      (inst && inst->ns && inst->ns->prefix) ? inst->ns->prefix : null,
	      (inst && inst->ns && inst->ns->prefix) ? ":" : "",
	      NAME(dsfp->st_inst), inst,
	      dsfp->st_inst ? xmlGetLineNo(dsfp->st_inst) : 0,
	      (dsfp->st_flags & STF_STOPWHENPOP) ? " stopwhenpop" : "");

    if (dsfp->st_flags & STF_STOPWHENPOP)
	xsltSetDebuggerStatus(XSLT_DEBUG_INIT);

    /* 'Pop' the stack frame */
    TAILQ_REMOVE(&slaxDebugStack, dsfp, st_link);
    statep->ds_stackdepth -= 1;	/* Reduce depth of stack */

    if (statep->ds_flags & DSF_CALLFLOW)
	slaxDebugCallFlow(statep, template, inst, "exit");

    /*
     * If we're popping stack frames, then we're not on the same
     * instruction.  Clear the last instruction pointer.
     */
    statep->ds_last_inst = NULL;

    xmlFree(dsfp);
}

/*
 * Register debugger
 */
int
slaxDebugInit (void)
{
    static int done_register;
    slaxDebugState_t *statep = slaxDebugGetState();

    if (done_register)
	return FALSE;
    done_register = TRUE;

    TAILQ_INIT(&slaxDebugBreakpoints);
    TAILQ_INIT(&slaxDebugStack);

    /* Start with the current line */
    statep->ds_flags |= DSF_DISPLAY;

    xsltSetDebuggerStatus(XSLT_DEBUG_INIT);
    xsltSetDebuggerCallbacksHelper(slaxDebugHandler, slaxDebugAddFrame,
				   slaxDebugDropFrame);

    slaxDebugDisplayMode = DEBUG_MODE_CLI;

    slaxOutput("sdb: The SLAX Debugger (version %s)", PACKAGE_VERSION);
    slaxOutput("Type 'help' for help");

    return FALSE;
}

/**
 * Set the top-most stylesheet
 *
 * @stylep the stylesheet aka script
 */
void
slaxDebugSetStylesheet (xsltStylesheetPtr script)
{
    slaxDebugState_t *statep = slaxDebugGetState();

    if (statep) {
	statep->ds_script = script;
	statep->ds_inst = NULL;
	statep->ds_template = NULL;
	statep->ds_node = NULL;
	statep->ds_last_inst = NULL;
	statep->ds_stop_at = NULL;
    }
}

/**
 * Set a search path for included and imported files
 *
 * @includes array of search paths
 */
void
slaxDebugSetIncludes (const char **includes)
{
    slaxDebugIncludes = includes;
}

static xsltStylesheetPtr
slaxDebugReload (const char *scriptname)
{
    FILE *fp;
    xmlDocPtr docp;
    xsltStylesheetPtr newp = NULL;

    fp = fopen(scriptname, "r");
    if (fp == NULL) {
	slaxOutput("could not open file '%s': %s",
		   scriptname, strerror(errno));
    } else {
	docp = slaxLoadFile(scriptname, fp, NULL, 0);
	fclose(fp);
	if (docp == NULL) {
	    slaxOutput("could not parse file '%s'", scriptname);
	} else {
	    newp = xsltParseStylesheetDoc(docp);
	    if (newp && newp->errors == 0)
		return newp;
		
	    slaxOutput("%d errors parsing script: '%s'",
		       newp ? newp->errors : 1, scriptname);
	    if (newp) {
		xsltFreeStylesheet(newp);
		newp = NULL;
	    } else if (docp) {
		xmlFreeDoc(docp);
	    }
	}
    }

    slaxOutput("Reload failed.");
    return NULL;
}

/**
 * Apply a stylesheet to an input document, returning the results.
 *
 * @style stylesheet aka script
 * @doc input document
 * @params set of parameters
 * @returns output document
 */
void
slaxDebugApplyStylesheet (const char *scriptname, xsltStylesheetPtr style,
			  const char *docname UNUSED, xmlDocPtr doc,
			  const char **params)
{
    slaxDebugState_t *statep = slaxDebugGetState();
    xmlDocPtr res;
    int status;
    xsltStylesheetPtr new_style, save_style = NULL;

    slaxDebugSetStylesheet(style);
    slaxProfOpen(style->doc);
    statep->ds_flags |= DSF_PROFILER;

    xsltSetDebuggerStatus(0);

    for (;;) {
	if (slaxDebugShell(statep) < 0)
	    break;

	/*
	 * Lots of flag interactions here, based on how we got
	 * to this spot.
	 */
	if (statep->ds_flags & DSF_RESTART) {
	restart:
	    statep->ds_flags &= ~DSF_RESTART;
	    statep->ds_flags |= DSF_DISPLAY;

	    if (statep->ds_flags & DSF_CONTINUE) {
		xsltSetDebuggerStatus(XSLT_DEBUG_CONT);
		statep->ds_flags &= ~DSF_CONTINUE;
	    } else 
		xsltSetDebuggerStatus(XSLT_DEBUG_INIT);

	} else if (statep->ds_flags & DSF_RELOAD) {
	reload:
	    statep->ds_flags &= ~DSF_RELOAD;
	    xsltSetDebuggerStatus(0);

	    if (save_style) {
		xsltFreeStylesheet(save_style);
		save_style = NULL;
	    }

	    new_style = slaxDebugReload(scriptname);
	    if (new_style) {
		/* Out with the old */
		slaxDebugClearBreakpoints();
		slaxDebugClearStacktrace();
		slaxProfClear();
		slaxProfClose();

		/* In with the new */
		save_style = style = new_style;
		slaxDebugSetStylesheet(style);
		slaxProfOpen(style->doc);

		statep->ds_flags &= ~(DSF_RESTART & DSF_DISPLAY);
		xsltSetDebuggerStatus(0);

		slaxOutput("Reloading complete.");
	    }

	    continue;

	} else {
	    status = xsltGetDebuggerStatus();
	    if (status == XSLT_DEBUG_QUIT)
		break;
	    
	    continue;	/* Until the user says "run", we go nothing */
	}

	res = xsltApplyStylesheet(style, doc, params);

	status = xsltGetDebuggerStatus();
	if (status == XSLT_DEBUG_QUIT) {
	    xmlFreeAndEasy(res);
	    res = NULL;

	    if (statep->ds_flags & DSF_RELOAD) {
		slaxOutput("Reloading script...");
		goto reload;

	    } else if (statep->ds_flags & DSF_RESTART) {
		slaxOutput("Restarting script.");
		slaxProfClear();
		slaxDebugClearStacktrace();
		goto restart;

	    } else {
		/* Quit without restart == exit */
		break;
	    }
	}

	/*
	 * We fell out the bottom of the script.  Show the output,
	 * cleanup, and loop in the shell until something
	 * interesting happens.
	 */
	if (res) {
	    xsltSaveResultToFile(stdout, res, style);
	    xmlFreeDoc(res);
	    res = NULL;
	}

	/* Clean up state pointers (all free'd by now) */
	statep->ds_ctxt = NULL;
	statep->ds_inst = NULL;
	statep->ds_node = NULL;
	statep->ds_template = NULL;
	statep->ds_last_inst = NULL;
	statep->ds_stop_at = NULL;

	slaxOutput("Script exited normally.");

	statep->ds_flags &= ~(DSF_RESTART & DSF_DISPLAY);
	xsltSetDebuggerStatus(XSLT_DEBUG_DONE);
	slaxProfClear();
    }

    /* Free our resources */
    slaxDebugClearBreakpoints();
    slaxDebugClearStacktrace();
    slaxProfClose();

    if (save_style)
	xsltFreeStylesheet(save_style);
}
