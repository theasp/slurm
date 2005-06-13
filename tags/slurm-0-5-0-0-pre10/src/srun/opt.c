/*****************************************************************************\
 *  opt.c - options processing for srun
 *  $Id$
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <grondona1@llnl.gov>, et. al.
 *  UCRL-CODE-2002-040.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>		/* strcpy, strncasecmp */

#ifdef HAVE_STRINGS_H
#  include <strings.h>
#endif

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#if HAVE_GETOPT_H
#  include <getopt.h>
#else
#  include "src/common/getopt.h"
#endif

#include <stdarg.h>		/* va_start   */
#include <stdlib.h>		/* getenv     */
#include <pwd.h>		/* getpwuid   */
#include <ctype.h>		/* isdigit    */
#include <sys/param.h>		/* MAXPATHLEN */
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/utsname.h>

#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/srun/env.h"
#include "src/srun/opt.h"
#include "src/srun/attach.h"

/* generic OPT_ definitions -- mainly for use with env vars  */
#define OPT_NONE        0x00
#define OPT_INT         0x01
#define OPT_STRING      0x02
#define OPT_DEBUG       0x03
#define OPT_DISTRIB     0x04
#define OPT_NODES       0x05
#define OPT_OVERCOMMIT  0x06
#define OPT_CORE        0x07
#define OPT_CONN_TYPE	0x08
#define OPT_NODE_USE	0x09
#define OPT_NO_ROTATE	0x0a
#define OPT_GEOMETRY	0x0b

/* generic getopt_long flags, integers and *not* valid characters */
#define LONG_OPT_HELP     0x100
#define LONG_OPT_USAGE    0x101
#define LONG_OPT_XTO      0x102
#define LONG_OPT_LAUNCH   0x103
#define LONG_OPT_TIMEO    0x104
#define LONG_OPT_JOBID    0x105
#define LONG_OPT_TMP      0x106
#define LONG_OPT_MEM      0x107
#define LONG_OPT_MINCPU   0x108
#define LONG_OPT_CONT     0x109
#define LONG_OPT_UID      0x10a
#define LONG_OPT_GID      0x10b
#define LONG_OPT_MPI      0x10c
#define LONG_OPT_CORE	  0x10e
#define LONG_OPT_NOSHELL  0x10f
#define LONG_OPT_DEBUG_TS 0x110
#define LONG_OPT_CONNTYPE 0x111
#define LONG_OPT_NODE_USE 0x112
#define LONG_OPT_TEST_ONLY 0x113
#define LONG_OPT_NETWORK  0x114
#define LONG_OPT_EXCLUSIVE 0x115

/*---- forward declarations of static functions  ----*/

typedef struct env_vars env_vars_t;

/* return command name from its full path name */
static char * _base_name(char* command);

static List  _create_path_list(void);

/* Get a decimal integer from arg */
static int  _get_int(const char *arg, const char *what);

static void  _help(void);

/* set options based upon commandline args */
static void _opt_args(int, char **);

/* fill in default options  */
static void _opt_default(void);

/* set options based upon env vars  */
static void _opt_env(void);

/* list known options and their settings  */
static void  _opt_list(void);

/* verify options sanity  */
static bool _opt_verify(void);

static void  _print_version(void);

static void _process_env_var(env_vars_t *e, const char *val);

/* search PATH for command returns full path */
static char *_search_path(char *, bool, int);

static long  _to_bytes(const char *arg);

static bool  _under_parallel_debugger(void);

static void  _usage(void);
static bool  _valid_node_list(char **node_list_pptr);
static enum  distribution_t _verify_dist_type(const char *arg);
static bool  _verify_node_count(const char *arg, int *min, int *max);
static int   _verify_geometry(const char *arg, int *geometry);
static int   _verify_conn_type(const char *arg);
static int   _verify_node_use(const char *arg);

/*---[ end forward declarations of static functions ]---------------------*/

int initialize_and_process_args(int argc, char *argv[])
{
	/* initialize option defaults */
	_opt_default();

	/* initialize options with env vars */
	_opt_env();

	/* initialize options with argv */
	_opt_args(argc, argv);

	if (_verbose > 3)
		_opt_list();

	return 1;

}

static void _print_version(void)
{
	printf("%s %s\n", PACKAGE, SLURM_VERSION);
}

/*
 * If the node list supplied is a file name, translate that into 
 *	a list of nodes, we orphan the data pointed to
 * RET true if the node list is a valid one
 */
static bool _valid_node_list(char **node_list_pptr)
{
	FILE *fd;
	char *node_list;
	int c;
	bool last_space;

	if (strchr(*node_list_pptr, '/') == NULL)
		return true;	/* not a file name */

	fd = fopen(*node_list_pptr, "r");
	if (fd == NULL) {
		error ("Unable to open file %s: %m", *node_list_pptr);
		return false;
	}

	node_list = xstrdup("");
	last_space = false;
	while ((c = fgetc(fd)) != EOF) {
		if (isspace(c)) {
			last_space = true;
			continue;
		}
		if (last_space && (node_list[0] != '\0'))
			xstrcatchar(node_list, ',');
		last_space = false;
		xstrcatchar(node_list, (char)c);
	}
	(void) fclose(fd);

        /*  free(*node_list_pptr);	orphanned */
	*node_list_pptr = node_list;
	return true;
}

/* 
 * verify that a distribution type in arg is of a known form
 * returns the distribution_t or SRUN_DIST_UNKNOWN
 */
static enum distribution_t _verify_dist_type(const char *arg)
{
	int len = strlen(arg);
	enum distribution_t result = SRUN_DIST_UNKNOWN;

	if (strncasecmp(arg, "cyclic", len) == 0)
		result = SRUN_DIST_CYCLIC;
	else if (strncasecmp(arg, "block", len) == 0)
		result = SRUN_DIST_BLOCK;

	return result;
}

/*
 * verify that a connection type in arg is of known form
 * returns the connection_type or -1 if not recognized
 */
static int _verify_conn_type(const char *arg)
{
	int len = strlen(arg);

	if (!strncasecmp(arg, "MESH", len))
		return SELECT_MESH;
	else if (!strncasecmp(arg, "TORUS", len))
		return SELECT_TORUS;
	else if (!strncasecmp(arg, "NAV", len))
		return SELECT_NAV;

	error("invalid --conn-type argument %s ignored.", arg);
	return -1;
}

/*
 * verify that a node usage type in arg is of known form
 * returns the node_use_type or -1 if not recognized
 */
static int _verify_node_use(const char *arg)
{
	int len = strlen(arg);

	if (!strncasecmp(arg, "VIRTUAL", len))
		return SELECT_VIRTUAL_NODE_MODE;
	else if (!strncasecmp(arg, "COPROCESSOR", len))
		return SELECT_COPROCESSOR_MODE;
	else if (!strncasecmp(arg, "NAV", len))
		return SELECT_NAV_MODE;

	error("invalid --node-use argument %s ignored.", arg);
	return -1;
}

/*
 * verify geometry arguments, must have proper count
 * returns -1 on error, 0 otherwise
 */
static int _verify_geometry(const char *arg, int *geometry)
{
	char* token, *delimiter = ",x", *next_ptr;
	int i, rc = 0;
	char* geometry_tmp = xstrdup(arg);
	char* original_ptr = geometry_tmp;

	token = strtok_r(geometry_tmp, delimiter, &next_ptr);
	for (i=0; i<SYSTEM_DIMENSIONS; i++) {
		if (token == NULL) {
			error("insufficient dimensions in --geometry");
			rc = -1;
			break;
		}
		geometry[i] = atoi(token);
		if (geometry[i] <= 0) {
			error("invalid --geometry argument");
			rc = -1;
			break;
		}
		geometry_tmp = next_ptr;
		token = strtok_r(geometry_tmp, delimiter, &next_ptr);
	}
	if (token != NULL) {
		error("too many dimensions in --geometry");
		rc = -1;
	}

	if (original_ptr)
		xfree(original_ptr);

	return rc;
}

/* 
 * verify that a node count in arg is of a known form (count or min-max)
 * OUT min, max specified minimum and maximum node counts
 * RET true if valid
 */
static bool 
_verify_node_count(const char *arg, int *min_nodes, int *max_nodes)
{
	char *end_ptr;
	int val1, val2;

	val1 = strtol(arg, &end_ptr, 10);
	if (end_ptr[0] == '\0') {
		*min_nodes = val1;
		return true;
	}

	if (end_ptr[0] != '-')
		return false;

	val2 = strtol(&end_ptr[1], &end_ptr, 10);
	if (end_ptr[0] == '\0') {
		*min_nodes = val1;
		*max_nodes = val2;
		return true;
	} else
		return false;

}

/* return command name from its full path name */
static char * _base_name(char* command)
{
	char *char_ptr, *name;
	int i;

	if (command == NULL)
		return NULL;

	char_ptr = strrchr(command, (int)'/');
	if (char_ptr == NULL)
		char_ptr = command;
	else
		char_ptr++;

	i = strlen(char_ptr);
	name = xmalloc(i+1);
	strcpy(name, char_ptr);
	return name;
}

/*
 * _to_bytes(): verify that arg is numeric with optional "G" or "M" at end
 * if "G" or "M" is there, multiply by proper power of 2 and return
 * number in bytes
 */
static long _to_bytes(const char *arg)
{
	char *buf;
	char *endptr;
	int end;
	int multiplier = 1;
	long result;

	buf = xstrdup(arg);

	end = strlen(buf) - 1;

	if (isdigit(buf[end])) {
		result = strtol(buf, &endptr, 10);

		if (*endptr != '\0')
			result = -result;

	} else {

		switch (toupper(buf[end])) {

		case 'G':
			multiplier = 1024;
			break;

		case 'M':
			/* do nothing */
			break;

		default:
			multiplier = -1;
		}

		buf[end] = '\0';

		result = multiplier * strtol(buf, &endptr, 10);

		if (*endptr != '\0')
			result = -result;
	}

	return result;
}

/*
 * print error message to stderr with opt.progname prepended
 */
#undef USE_ARGERROR
#if USE_ARGERROR
static void argerror(const char *msg, ...)
{
	va_list ap;
	char buf[256];

	va_start(ap, msg);
	vsnprintf(buf, sizeof(buf), msg, ap);

	fprintf(stderr, "%s: %s\n",
		opt.progname ? opt.progname : "srun", buf);
	va_end(ap);
}
#else
#  define argerror error
#endif				/* USE_ARGERROR */

/*
 * _opt_default(): used by initialize_and_process_args to set defaults
 */
static void _opt_default()
{
	char buf[MAXPATHLEN + 1];
	struct passwd *pw;
	int i;

	if ((pw = getpwuid(getuid())) != NULL) {
		strncpy(opt.user, pw->pw_name, MAX_USERNAME);
		opt.uid = pw->pw_uid;
	} else
		error("who are you?");

	opt.gid = getgid();

	if ((getcwd(buf, MAXPATHLEN)) == NULL) 
		fatal("getcwd failed: %m");
	opt.cwd = xstrdup(buf);

	opt.progname = NULL;

	opt.nprocs = 1;
	opt.nprocs_set = false;
	opt.cpus_per_task = 1; 
	opt.cpus_set = false;
	opt.min_nodes = 1;
	opt.max_nodes = 0;
	opt.nodes_set = false;
	opt.time_limit = -1;
	opt.partition = NULL;
	opt.max_threads = MAX_THREADS;

	opt.job_name = NULL;
	opt.jobid    = NO_VAL;
	opt.mpi_type = MPI_UNKNOWN;
	opt.dependency = NO_VAL;
	opt.account  = NULL;

	opt.distribution = SRUN_DIST_UNKNOWN;

	opt.ofname = NULL;
	opt.ifname = NULL;
	opt.efname = NULL;

	opt.core_type = CORE_DEFAULT;

	opt.labelio = false;
	opt.unbuffered = false;
	opt.overcommit = false;
	opt.batch = false;
	opt.share = false;
	opt.no_kill = false;
	opt.kill_bad_exit = false;

	opt.immediate	= false;

	opt.allocate	= false;
	opt.noshell	= false;
	opt.attach	= NULL;
	opt.join	= false;
	opt.max_wait	= slurm_get_wait_time();

	opt.quit_on_intr = false;
	opt.disable_status = false;
	opt.test_only   = false;

	opt.quiet = 0;
	_verbose = 0;
	opt.slurmd_debug = LOG_LEVEL_QUIET;

	/* constraint default (-1 is no constraint) */
	opt.mincpus	    = -1;
	opt.realmem	    = -1;
	opt.tmpdisk	    = -1;

	opt.hold	    = false;
	opt.constraints	    = NULL;
	opt.contiguous	    = false;
        opt.exclusive       = false;
	opt.nodelist	    = NULL;
	opt.exc_nodes	    = NULL;
	opt.max_launch_time = 120;/* 120 seconds to launch job             */
	opt.max_exit_timeout= 60; /* Warn user 60 seconds after task exit */
	opt.msg_timeout     = 5;  /* Default launch msg timeout           */

	for (i=0; i<SYSTEM_DIMENSIONS; i++)
		opt.geometry[i]	    = -1;
	opt.no_rotate	    = false;
	opt.conn_type	    = -1;
	opt.node_use        = -1;

	opt.euid	    = (uid_t) -1;
	opt.egid	    = (gid_t) -1;
	
	mode	= MODE_NORMAL;

	/*
	 * Reset some default values if running under a parallel debugger
	 */
	if ((opt.parallel_debug = _under_parallel_debugger())) {
		opt.max_launch_time = 120;
		opt.max_threads     = 1;
		opt.msg_timeout     = 15;
	}

}

/*---[ env var processing ]-----------------------------------------------*/

/*
 * try to use a similar scheme as popt. 
 * 
 * in order to add a new env var (to be processed like an option):
 *
 * define a new entry into env_vars[], if the option is a simple int
 * or string you may be able to get away with adding a pointer to the
 * option to set. Otherwise, process var based on "type" in _opt_env.
 */
struct env_vars {
	const char *var;
	int type;
	void *arg;
	void *set_flag;
};

env_vars_t env_vars[] = {
  {"SLURM_ACCOUNT",       OPT_STRING,     &opt.account,       NULL           },
  {"SLURMD_DEBUG",        OPT_INT,        &opt.slurmd_debug,  NULL           }, 
  {"SLURM_CPUS_PER_TASK", OPT_INT,        &opt.cpus_per_task, &opt.cpus_set  },
  {"SLURM_CONN_TYPE",     OPT_CONN_TYPE,  NULL,               NULL           },
  {"SLURM_CORE_FORMAT",   OPT_CORE,       NULL,               NULL           },
  {"SLURM_DEBUG",         OPT_DEBUG,      NULL,               NULL           },
  {"SLURM_DISTRIBUTION",  OPT_DISTRIB,    NULL,               NULL           },
  {"SLURM_GEOMETRY",      OPT_GEOMETRY,   NULL,               NULL           },
  {"SLURM_IMMEDIATE",     OPT_INT,        &opt.immediate,     NULL           },
  {"SLURM_JOBID",         OPT_INT,        &opt.jobid,         NULL           },
  {"SLURM_KILL_BAD_EXIT", OPT_INT,        &opt.kill_bad_exit, NULL           },
  {"SLURM_LABELIO",       OPT_INT,        &opt.labelio,       NULL           },
  {"SLURM_NNODES",        OPT_NODES,      NULL,               NULL           },
  {"SLURM_NO_ROTATE",     OPT_NO_ROTATE,  NULL,               NULL           },
  {"SLURM_NODE_USE",      OPT_NODE_USE,   NULL,               NULL           },
  {"SLURM_NPROCS",        OPT_INT,        &opt.nprocs,        &opt.nprocs_set},
  {"SLURM_OVERCOMMIT",    OPT_OVERCOMMIT, NULL,               NULL           },
  {"SLURM_PARTITION",     OPT_STRING,     &opt.partition,     NULL           },
  {"SLURM_REMOTE_CWD",    OPT_STRING,     &opt.cwd,           NULL           },
  {"SLURM_STDERRMODE",    OPT_STRING,     &opt.efname,        NULL           },
  {"SLURM_STDINMODE",     OPT_STRING,     &opt.ifname,        NULL           },
  {"SLURM_STDOUTMODE",    OPT_STRING,     &opt.ofname,        NULL           },
  {"SLURM_TIMELIMIT",     OPT_INT,        &opt.time_limit,    NULL           },
  {"SLURM_WAIT",          OPT_INT,        &opt.max_wait,      NULL           },
  {"SLURM_DISABLE_STATUS",OPT_INT,        &opt.disable_status,NULL           },
  {NULL, 0, NULL, NULL}
};


/*
 * _opt_env(): used by initialize_and_process_args to set options via
 *            environment variables. See comments above for how to
 *            extend srun to process different vars
 */
static void _opt_env()
{
	char       *val = NULL;
	env_vars_t *e   = env_vars;

	while (e->var) {
		if ((val = getenv(e->var)) != NULL) 
			_process_env_var(e, val);
		e++;
	}
}


static void
_process_env_var(env_vars_t *e, const char *val)
{
	char *end = NULL;
	enum distribution_t dt;

	debug2("now processing env var %s=%s", e->var, val);

	if (e->set_flag) {
		*((bool *) e->set_flag) = true;
	}

	switch (e->type) {
	case OPT_STRING:
	    *((char **) e->arg) = xstrdup(val);
	    break;
	case OPT_INT:
	    if (val != NULL) {
		    *((int *) e->arg) = (int) strtol(val, &end, 10);
		    if (!(end && *end == '\0')) 
			    error("%s=%s invalid. ignoring...", e->var, val);
	    }
	    break;

	case OPT_DEBUG:
	    if (val != NULL) {
		    _verbose = (int) strtol(val, &end, 10);
		    if (!(end && *end == '\0')) 
			    error("%s=%s invalid", e->var, val);
	    }
	    break;

	case OPT_DISTRIB:
	    dt = _verify_dist_type(val);
	    if (dt == SRUN_DIST_UNKNOWN) {
		    error("\"%s=%s\" -- invalid distribution type. " 
		          "ignoring...", e->var, val);
	    } else 
		    opt.distribution = dt;
	    break;

	case OPT_NODES:
	    opt.nodes_set = _verify_node_count( val, 
			                        &opt.min_nodes, 
					        &opt.max_nodes );
	    if (opt.nodes_set == false) {
		    error("\"%s=%s\" -- invalid node count. ignoring...",
			  e->var, val);
	    }
	    break;

	case OPT_OVERCOMMIT:
	    opt.overcommit = true;
	    break;

	case OPT_CORE:
	    opt.core_type = core_format_type (val);
	    break;

	case OPT_CONN_TYPE:
		opt.conn_type = _verify_conn_type(val);
		break;
	
	case OPT_NODE_USE:
		opt.node_use = _verify_node_use(val);
		break;

	case OPT_NO_ROTATE:
		opt.no_rotate = true;
		break;

	case OPT_GEOMETRY:
		if (_verify_geometry(val, opt.geometry)) {
			error("\"%s=%s\" -- invalid geometry, ignoring...",
				e->var, val);
		}
		break;

	default:
	    /* do nothing */
	    break;
	}
}

/*
 *  Get a decimal integer from arg.
 *
 *  Returns the integer on success, exits program on failure.
 * 
 */
static int
_get_int(const char *arg, const char *what)
{
	char *p;
	long int result = strtol(arg, &p, 10);

	if ((*p != '\0') || (result < 0L)) {
		error ("Invalid numeric value \"%s\" for %s.", arg, what);
		exit(1);
	}

	if (result > INT_MAX) {
		error ("Numeric argument (%ld) to big for %s.", result, what);
	}

	return (int) result;
}

/*
 * _opt_args() : set options via commandline args and popt
 */
static void _opt_args(int argc, char **argv)
{
	int opt_char, i;
	int option_index;
	struct utsname name;
	static struct option long_options[] = {
		{"attach",        required_argument, 0, 'a'},
		{"allocate",      no_argument,       0, 'A'},
		{"batch",         no_argument,       0, 'b'},
		{"cpus-per-task", required_argument, 0, 'c'},
		{"constraint",    required_argument, 0, 'C'},
		{"slurmd-debug",  required_argument, 0, 'd'},
		{"chdir",         required_argument, 0, 'D'},
		{"error",         required_argument, 0, 'e'},
		{"geometry",      required_argument, 0, 'g'},
		{"hold",          no_argument,       0, 'H'},
		{"input",         required_argument, 0, 'i'},
		{"immediate",     no_argument,       0, 'I'},
		{"join",          no_argument,       0, 'j'},
		{"job-name",      required_argument, 0, 'J'},
		{"no-kill",       no_argument,       0, 'k'},
		{"kill-on-bad-exit", no_argument,    0, 'K'},
		{"label",         no_argument,       0, 'l'},
		{"distribution",  required_argument, 0, 'm'},
		{"ntasks",        required_argument, 0, 'n'},
		{"nodes",         required_argument, 0, 'N'},
		{"output",        required_argument, 0, 'o'},
		{"overcommit",    no_argument,       0, 'O'},
		{"partition",     required_argument, 0, 'p'},
		{"dependency",    required_argument, 0, 'P'},
		{"quit-on-interrupt", no_argument,   0, 'q'},
		{"quiet",            no_argument,    0, 'Q'},
		{"relative",      required_argument, 0, 'r'},
		{"no-rotate",     no_argument,       0, 'R'},
		{"share",         no_argument,       0, 's'},
		{"time",          required_argument, 0, 't'},
		{"threads",       required_argument, 0, 'T'},
		{"unbuffered",    no_argument,       0, 'u'},
		{"account",       required_argument, 0, 'U'},
		{"verbose",       no_argument,       0, 'v'},
		{"version",       no_argument,       0, 'V'},
		{"nodelist",      required_argument, 0, 'w'},
		{"wait",          required_argument, 0, 'W'},
		{"exclude",       required_argument, 0, 'x'},
		{"disable-status", no_argument,      0, 'X'},
		{"no-allocate",   no_argument,       0, 'Z'},
		{"contiguous",       no_argument,       0, LONG_OPT_CONT},
                {"exclusive",        no_argument,       0, LONG_OPT_EXCLUSIVE},
		{"core",             required_argument, 0, LONG_OPT_CORE},
		{"mincpus",          required_argument, 0, LONG_OPT_MINCPU},
		{"mem",              required_argument, 0, LONG_OPT_MEM},
		{"mpi",              required_argument, 0, LONG_OPT_MPI},
		{"no-shell",         no_argument,       0, LONG_OPT_NOSHELL},
		{"tmp",              required_argument, 0, LONG_OPT_TMP},
		{"jobid",            required_argument, 0, LONG_OPT_JOBID},
		{"msg-timeout",      required_argument, 0, LONG_OPT_TIMEO},
		{"max-launch-time",  required_argument, 0, LONG_OPT_LAUNCH},
		{"max-exit-timeout", required_argument, 0, LONG_OPT_XTO},
		{"uid",              required_argument, 0, LONG_OPT_UID},
		{"gid",              required_argument, 0, LONG_OPT_GID},
		{"debugger-test",    no_argument,       0, LONG_OPT_DEBUG_TS},
		{"help",             no_argument,       0, LONG_OPT_HELP},
		{"usage",            no_argument,       0, LONG_OPT_USAGE},
		{"conn-type",        required_argument, 0, LONG_OPT_CONNTYPE},
		{"node-use",         required_argument, 0, LONG_OPT_NODE_USE},
		{"test-only",        no_argument,       0, LONG_OPT_TEST_ONLY},
		{"network",          required_argument, 0, LONG_OPT_NETWORK},
		{NULL,               0,                 0, 0}
	};
	char *opt_string = "+a:Abc:C:d:D:e:g:Hi:IjJ:kKlm:n:N:"
		"o:Op:P:qQr:R:st:T:uU:vVw:W:x:XZ";
	char **rest = NULL;

	opt.progname = xbasename(argv[0]);

	while((opt_char = getopt_long(argc, argv, opt_string,
			long_options, &option_index)) != -1) {
		switch (opt_char) {
		case (int)'?':
			fprintf(stderr, "Try \"srun --help\" for more "
				"information\n");
			exit(1);
			break;
		case (int)'a':
			if (opt.allocate || opt.batch) {
				error("can only specify one mode: "
				      "allocate, attach or batch.");
				exit(1);
			}
			mode = MODE_ATTACH;
			opt.attach = strdup(optarg);
			break;
		case (int)'A':
			if (opt.attach || opt.batch) {
				error("can only specify one mode: "
				      "allocate, attach or batch.");
				exit(1);
			}
			mode = MODE_ALLOCATE;
			opt.allocate = true;
			break;
		case (int)'b':
			if (opt.allocate || opt.attach) {
				error("can only specify one mode: "
				      "allocate, attach or batch.");
				exit(1);
			}
			mode = MODE_BATCH;
			opt.batch = true;
			break;
		case (int)'c':
			opt.cpus_set = true;
			opt.cpus_per_task = 
				_get_int(optarg, "cpus-per-task");
			break;
		case (int)'C':
			xfree(opt.constraints);
			opt.constraints = xstrdup(optarg);
			break;
		case (int)'d':
			opt.slurmd_debug = 
				_get_int(optarg, "slurmd-debug");
			break;
		case (int)'D':
			xfree(opt.cwd);
			opt.cwd = xstrdup(optarg);
			break;
		case (int)'e':
			xfree(opt.efname);
			opt.efname = xstrdup(optarg);
			break;
		case (int)'g':
			if (_verify_geometry(optarg, opt.geometry))
				exit(1);
			break;
		case (int)'H':
			opt.hold = true;
			break;
		case (int)'i':
			xfree(opt.ifname);
			opt.ifname = xstrdup(optarg);
			break;
		case (int)'I':
			opt.immediate = true;
			break;
		case (int)'j':
			opt.join = true;
			break;
		case (int)'J':
			xfree(opt.job_name);
			opt.job_name = xstrdup(optarg);
			break;
		case (int)'k':
			opt.no_kill = true;
			break;
		case (int)'K':
			opt.kill_bad_exit = true;
			break;
		case (int)'l':
			opt.labelio = true;
			break;
		case (int)'m':
			opt.distribution = _verify_dist_type(optarg);
			if (opt.distribution == SRUN_DIST_UNKNOWN) {
				error("distribution type `%s' " 
				      "is not recognized", optarg);
				exit(1);
			}
			break;
		case (int)'n':
			opt.nprocs_set = true;
			opt.nprocs = 
				_get_int(optarg, "number of tasks");
			break;
		case (int)'N':
			opt.nodes_set = 
				_verify_node_count(optarg, 
						   &opt.min_nodes,
						   &opt.max_nodes);
			if (opt.nodes_set == false) {
				error("invalid node count `%s'", 
				      optarg);
				exit(1);
			}
			break;
		case (int)'o':
			xfree(opt.ofname);
			opt.ofname = xstrdup(optarg);
			break;
		case (int)'O':
			opt.overcommit = true;
			break;
		case (int)'p':
			xfree(opt.partition);
			opt.partition = xstrdup(optarg);
			break;
		case (int)'P':
			opt.dependency = _get_int(optarg, "dependency");
			break;
		case (int)'q':
			opt.quit_on_intr = true;
			break;
		case (int) 'Q':
			opt.quiet++;
			break;
		case (int)'r':
			xfree(opt.relative);
			opt.relative = xstrdup(optarg);
			break;
		case (int)'R':
			opt.no_rotate = true;
			break;
		case (int)'s':
			opt.share = true;
			break;
		case (int)'t':
			opt.time_limit = _get_int(optarg, "time");
			break;
		case (int)'T':
			opt.max_threads = 
				_get_int(optarg, "max_threads");
			break;
		case (int)'u':
			opt.unbuffered = true;
			break;
		case (int)'U':
			opt.account = xstrdup(optarg);
			break;
		case (int)'v':
			_verbose++;
			break;
		case (int)'V':
			_print_version();
			exit(0);
			break;
		case (int)'w':
			xfree(opt.nodelist);
			opt.nodelist = xstrdup(optarg);
			if (!_valid_node_list(&opt.nodelist))
				exit(1);
			break;
		case (int)'W':
			opt.max_wait = _get_int(optarg, "wait");
			break;
		case (int)'x':
			xfree(opt.exc_nodes);
			opt.exc_nodes = xstrdup(optarg);
			if (!_valid_node_list(&opt.exc_nodes))
				exit(1);
			break;
		case (int)'X': 
			opt.disable_status = true;
			break;
		case (int)'Z':
			opt.no_alloc = true;
			uname(&name);
			if (strcasecmp(name.sysname, "AIX") == 0)
				opt.network = xstrdup("ip");
			break;
		case LONG_OPT_CONT:
			opt.contiguous = true;
			break;
                case LONG_OPT_EXCLUSIVE:
                        opt.exclusive = true;
                        break;
		case LONG_OPT_CORE:
			opt.core_type = core_format_type (optarg);
			if (opt.core_type == CORE_INVALID)
				error ("--core=\"%s\" Invalid -- ignoring.\n",
				      optarg);
			break;
		case LONG_OPT_MINCPU:
			opt.mincpus = _get_int(optarg, "mincpus");
			break;
		case LONG_OPT_MEM:
			opt.realmem = (int) _to_bytes(optarg);
			if (opt.realmem < 0) {
				error("invalid memory constraint %s", 
				      optarg);
				exit(1);
			}
			break;
		case LONG_OPT_MPI:
			if (strncasecmp(optarg, "lam",  3) == 0)
			       opt.mpi_type = MPI_LAM;	
			break;
		case LONG_OPT_NOSHELL:
			opt.noshell = true;
			break;
		case LONG_OPT_TMP:
			opt.tmpdisk = _to_bytes(optarg);
			if (opt.tmpdisk < 0) {
				error("invalid tmp value %s", optarg);
				exit(1);
			}
			break;
		case LONG_OPT_JOBID:
			opt.jobid = _get_int(optarg, "jobid");
			break;
		case LONG_OPT_TIMEO:
			opt.msg_timeout = 
				_get_int(optarg, "msg-timeout");
			break;
		case LONG_OPT_LAUNCH:
			opt.max_launch_time = 
				_get_int(optarg, "max-launch-time");
			break;
		case LONG_OPT_XTO:
			opt.max_exit_timeout = 
				_get_int(optarg, "max-exit-timeout");
			break;
		case LONG_OPT_UID:
			opt.euid = uid_from_string (optarg);
			if (opt.euid == (uid_t) -1)
				fatal ("--uid=\"%s\" invalid", optarg);
			break;
		case LONG_OPT_GID:
			opt.egid = gid_from_string (optarg);
			if (opt.egid == (gid_t) -1)
				fatal ("--gid=\"%s\" invalid", optarg);
			break;
		case LONG_OPT_DEBUG_TS:
			opt.debugger_test    = true;
			/* make other parameters look like debugger 
			 * is really attached */
			opt.parallel_debug   = true;
			MPIR_being_debugged  = 1;
			opt.max_launch_time = 120;
			opt.max_threads     = 1;
			opt.msg_timeout     = 15;
			break;
		case LONG_OPT_HELP:
			_help();
			exit(0);
		case LONG_OPT_USAGE:
			_usage();
			exit(0);
		case LONG_OPT_CONNTYPE:
			opt.conn_type = _verify_conn_type(optarg);
			break;
		case LONG_OPT_NODE_USE:
			opt.node_use = _verify_node_use(optarg);
			break;
		case LONG_OPT_TEST_ONLY:
			opt.test_only = true;
			break;
		case LONG_OPT_NETWORK:
			xfree(opt.network);
			opt.network = xstrdup(optarg);
			break;
		}
	}

	remote_argc = 0;
	if (optind < argc) {
		rest = argv + optind;
		while (rest[remote_argc] != NULL)
			remote_argc++;
	}
	remote_argv = (char **) xmalloc((remote_argc + 1) * sizeof(char *));
	for (i = 0; i < remote_argc; i++)
		remote_argv[i] = xstrdup(rest[i]);
	remote_argv[i] = NULL;	/* End of argv's (for possible execv) */

	if (remote_argc > 0) {
		char *fullpath;
		char *cmd       = remote_argv[0];
		bool search_cwd = (opt.batch || opt.allocate);
		int  mode       = (search_cwd) ? R_OK : R_OK | X_OK;

		if ((fullpath = _search_path(cmd, search_cwd, mode))) {
			xfree(remote_argv[0]);
			remote_argv[0] = fullpath;
		} 
	}

	if (!_opt_verify())
		exit(1);
}

/* 
 * _opt_verify : perform some post option processing verification
 *
 */
static bool _opt_verify(void)
{
	bool verified = true;

	/*
	 *  Do not set slurmd debug level higher than DEBUG2,
	 *   as DEBUG3 is used for slurmd IO operations, which
	 *   are not appropriate to be sent back to srun. (because
	 *   these debug messages cause the generation of more
	 *   debug messages ad infinitum)
	 */
	if (opt.slurmd_debug + LOG_LEVEL_ERROR > LOG_LEVEL_DEBUG2)
		opt.slurmd_debug = LOG_LEVEL_DEBUG2 - LOG_LEVEL_ERROR;

	if (opt.quiet && _verbose) {
		error ("don't specify both --verbose (-v) and --quiet (-Q)");
		verified = false;
	}

	if (opt.no_alloc && !opt.nodelist) {
		error("must specify a node list with -Z, --no-allocate.");
		verified = false;
	}

	if (opt.no_alloc && opt.exc_nodes) {
		error("can not specify --exclude list with -Z, --no-allocate.");
		verified = false;
	}

	if (opt.no_alloc && opt.relative) {
		error("do not specify -r,--relative with -Z,--no-allocate.");
		verified = false;
	}

	if (opt.relative && (opt.exc_nodes || opt.nodelist)) {
		error("-r,--relative not allowed with "
		      "-w,--nodelist or -x,--exclude.");
		verified = false;
	}

	if (opt.mincpus < opt.cpus_per_task)
		opt.mincpus = opt.cpus_per_task;

	if ((opt.job_name == NULL) && (remote_argc > 0))
		opt.job_name = _base_name(remote_argv[0]);

	if (mode == MODE_ATTACH) {	/* attach to a running job */
		if (opt.nodes_set || opt.cpus_set || opt.nprocs_set) {
			error("do not specific a node allocation "
			      "with --attach (-a)");
			verified = false;
		}

		/* if (constraints_given()) {
		 *	error("do not specify any constraints with "
		 *	      "--attach (-a)");
		 *	verified = false;
		 *}
		 */

	} else { /* mode != MODE_ATTACH */

		if ((remote_argc == 0) && (mode != MODE_ALLOCATE)) {
			error("must supply remote command");
			verified = false;
		}


		/* check for realistic arguments */
		if (opt.nprocs <= 0) {
			error("%s: invalid number of processes (-n %d)",
				opt.progname, opt.nprocs);
			verified = false;
		}

		if (opt.cpus_per_task <= 0) {
			error("%s: invalid number of cpus per task (-c %d)\n",
				opt.progname, opt.cpus_per_task);
			verified = false;
		}

		if ((opt.min_nodes <= 0) || (opt.max_nodes < 0) || 
		    (opt.max_nodes && (opt.min_nodes > opt.max_nodes))) {
			error("%s: invalid number of nodes (-N %d-%d)\n",
			      opt.progname, opt.min_nodes, opt.max_nodes);
			verified = false;
		}

		core_format_enable (opt.core_type);

		/* massage the numbers */
		if (opt.nodes_set && !opt.nprocs_set) {
			/* 1 proc / node default */
			opt.nprocs = opt.min_nodes;

		} else if (opt.nodes_set && opt.nprocs_set) {

			/* 
			 *  make sure # of procs >= min_nodes 
			 */
			if (opt.nprocs < opt.min_nodes) {

				info ("Warning: can't run %d processes on %d " 
				      "nodes, setting nnodes to %d", 
				      opt.nprocs, opt.min_nodes, opt.nprocs);

				opt.min_nodes = opt.nprocs;
				if (   opt.max_nodes 
				   && (opt.min_nodes > opt.max_nodes) )
					opt.max_nodes = opt.min_nodes;
			}

		} /* else if (opt.nprocs_set && !opt.nodes_set) */

	}

	if (opt.max_threads <= 0) {	/* set default */
		error("Thread value invalid, reset to 1");
		opt.max_threads = 1;
	} else if (opt.max_threads > MAX_THREADS) {
		error("Thread value exceeds defined limit, reset to %d", 
			MAX_THREADS);
		opt.max_threads = MAX_THREADS;
	}

	if (opt.labelio && opt.unbuffered) {
		error("Do not specify both -l (--label) and " 
		      "-u (--unbuffered)");
		exit(1);
	}

	/*
	 * --wait always overrides hidden max_exit_timeout
	 */
	if (opt.max_wait)
		opt.max_exit_timeout = opt.max_wait;

	if (opt.time_limit == 0)
		opt.time_limit = INFINITE;

	if ((opt.euid != (uid_t) -1) && (opt.euid != opt.uid)) 
		opt.uid = opt.euid;

	if ((opt.egid != (gid_t) -1) && (opt.egid != opt.gid)) 
		opt.gid = opt.egid;

        if ((opt.egid != (gid_t) -1) && (opt.egid != opt.gid))
	        opt.gid = opt.egid;

	if (opt.noshell && !opt.allocate) {
		error ("--no-shell only valid with -A (--allocate)");
		verified = false;
	}

	return verified;
}

static void
_freeF(void *data)
{
	xfree(data);
}

static List
_create_path_list(void)
{
	List l = list_create(&_freeF);
	char *path = xstrdup(getenv("PATH"));
	char *c, *lc;

	if (!path) {
		error("Error in PATH environment variable");
		list_destroy(l);
		return NULL;
	}

	c = lc = path;

	while (*c != '\0') {
		if (*c == ':') {
			/* nullify and push token onto list */
			*c = '\0';
			if (lc != NULL && strlen(lc) > 0)
				list_append(l, xstrdup(lc));
			lc = ++c;
		} else
			c++;
	}

	if (strlen(lc) > 0)
		list_append(l, xstrdup(lc));

	xfree(path);

	return l;
}

static char *
_search_path(char *cmd, bool check_current_dir, int access_mode)
{
	List         l        = _create_path_list();
	ListIterator i        = NULL;
	char *path, *fullpath = NULL;

	if (  (cmd[0] == '.' || cmd[0] == '/') 
           && (access(cmd, access_mode) == 0 ) ) {
		if (cmd[0] == '.')
			xstrfmtcat(fullpath, "%s/", opt.cwd);
		xstrcat(fullpath, cmd);
		goto done;
	}

	if (check_current_dir) 
		list_prepend(l, xstrdup(opt.cwd));

	i = list_iterator_create(l);
	while ((path = list_next(i))) {
		xstrfmtcat(fullpath, "%s/%s", path, cmd);

		if (access(fullpath, access_mode) == 0)
			goto done;

		xfree(fullpath);
		fullpath = NULL;
	}
  done:
	list_destroy(l);
	return fullpath;
}


/* helper function for printing options
 * 
 * warning: returns pointer to memory allocated on the stack.
 */
static char *print_constraints()
{
	char *buf = xstrdup("");

	if (opt.mincpus > 0)
		xstrfmtcat(buf, "mincpus=%d ", opt.mincpus);

	if (opt.realmem > 0)
		xstrfmtcat(buf, "mem=%dM ", opt.realmem);

	if (opt.tmpdisk > 0)
		xstrfmtcat(buf, "tmp=%ld ", opt.tmpdisk);

	if (opt.contiguous == true)
		xstrcat(buf, "contiguous ");
 
        if (opt.exclusive == true)
                xstrcat(buf, "exclusive ");

	if (opt.nodelist != NULL)
		xstrfmtcat(buf, "nodelist=%s ", opt.nodelist);

	if (opt.exc_nodes != NULL)
		xstrfmtcat(buf, "exclude=%s ", opt.exc_nodes);

	if (opt.constraints != NULL)
		xstrfmtcat(buf, "constraints=`%s' ", opt.constraints);

	return buf;
}

static char * 
print_commandline()
{
	int i;
	char buf[256];

	buf[0] = '\0';
	for (i = 0; i < remote_argc; i++)
		snprintf(buf, 256,  "%s", remote_argv[i]);
	return xstrdup(buf);
}

static char *
print_geometry()
{
	int i;
	char buf[32], *rc = NULL;

	if ((SYSTEM_DIMENSIONS == 0)
	||  (opt.geometry[0] < 0))
		return NULL;

	for (i=0; i<SYSTEM_DIMENSIONS; i++) {
		if (i > 0)
			snprintf(buf, sizeof(buf), "x%u", opt.geometry[i]);
		else
			snprintf(buf, sizeof(buf), "%u", opt.geometry[i]);
		xstrcat(rc, buf);
	}

	return rc;
}

#define tf_(b) (b == true) ? "true" : "false"

static void _opt_list()
{
	char *str;

	info("defined options for program `%s'", opt.progname);
	info("--------------- ---------------------");

	info("user           : `%s'", opt.user);
	info("uid            : %ld", (long) opt.uid);
	info("gid            : %ld", (long) opt.gid);
	info("cwd            : %s", opt.cwd);
	info("nprocs         : %d", opt.nprocs);
	info("cpus_per_task  : %d", opt.cpus_per_task);
	if (opt.max_nodes)
		info("nodes          : %d-%d", opt.min_nodes, opt.max_nodes);
	else
		info("nodes          : %d", opt.min_nodes);
	info("partition      : %s",
	     opt.partition == NULL ? "default" : opt.partition);
	info("job name       : `%s'", opt.job_name);
	info("distribution   : %s", format_distribution_t(opt.distribution));
	info("core format    : %s", core_format_name (opt.core_type));
	info("verbose        : %d", _verbose);
	info("slurmd_debug   : %d", opt.slurmd_debug);
	info("immediate      : %s", tf_(opt.immediate));
	info("label output   : %s", tf_(opt.labelio));
	info("unbuffered IO  : %s", tf_(opt.unbuffered));
	info("allocate       : %s", tf_(opt.allocate));
	info("attach         : `%s'", opt.attach);
	info("overcommit     : %s", tf_(opt.overcommit));
	info("batch          : %s", tf_(opt.batch));
	info("threads        : %d", opt.max_threads);
	if (opt.time_limit == INFINITE)
		info("time_limit     : INFINITE");
	else
		info("time_limit     : %d", opt.time_limit);
	info("wait           : %d", opt.max_wait);
	info("account        : %s", opt.account);
	if (opt.dependency == NO_VAL)
		info("dependency     : none");
	else
		info("dependency     : %u", opt.dependency);
	str = print_constraints();
	info("constraints    : %s", str);
	xfree(str);
	if (opt.conn_type >= 0)
		info("conn_type      : %u", opt.conn_type);
	str = print_geometry();
	info("geometry       : %s", str);
	xfree(str);
	if (opt.node_use >= 0)
		info("node_use       : %u", opt.node_use);
	if (opt.no_rotate)
		info("rotate         : no");
	else
		info("rotate         : yes");
	info("network        : %s", opt.network);
	str = print_commandline();
	info("remote command : `%s'", str);
	xfree(str);

}

/* Determine if srun is under the control of a parallel debugger or not */
static bool _under_parallel_debugger (void)
{
	return (MPIR_being_debugged != 0);
}

static void _usage(void)
{
 	printf(
"Usage: srun [-N nnodes] [-n ntasks] [-i in] [-o out] [-e err]\n"
"            [-c ncpus] [-r n] [-p partition] [--hold] [-t minutes]\n"
"            [-D path] [--immediate] [--overcommit] [--no-kill]\n"
"            [--share] [--label] [--unbuffered] [-m dist] [-J jobname]\n"
"            [--jobid=id] [--batch] [--verbose] [--slurmd_debug=#]\n"
"            [--core=type] [-T threads] [-W sec] [--attach] [--join] \n"
"            [--contiguous] [--mincpus=n] [--mem=MB] [--tmp=MB] [-C list]\n"
"            [--mpi=type] [--account=name] [--dependency=jobid]\n"
"            [--kill-on-bad-exit]\n"
#ifdef HAVE_BGL		/* Blue gene specific options */
"            [--geometry=XxYxZ] [--conn-type=type] [--no-rotate]\n"
"            [--node-use=type]\n"
#endif
"            [-w hosts...] [-x hosts...] executable [args...]\n");
}

static void _help(void)
{
        printf (
"Usage: srun [OPTIONS...] executable [args...]\n"
"\n"
"Parallel run options:\n"
"  -n, --ntasks=ntasks         number of tasks to run\n"
"  -N, --nodes=N               number of nodes on which to run (N = min[-max])\n"
"  -c, --cpus-per-task=ncpus   number of cpus required per task\n"
"  -i, --input=in              location of stdin redirection\n"
"  -o, --output=out            location of stdout redirection\n"
"  -e, --error=err             location of stderr redirection\n"
"  -r, --relative=n            run job step relative to node n of allocation\n"
"  -p, --partition=partition   partition requested\n"
"  -H, --hold                  submit job in held state\n"
"  -t, --time=minutes          time limit\n"
"  -D, --chdir=path            change remote current working directory\n"
"  -I, --immediate             exit if resources are not immediately available\n"
"  -O, --overcommit            overcommit resources\n"
"  -k, --no-kill               do not kill job on node failure\n"
"  -K, --kill-on-bad-exit      kill the job if any task terminates with a\n"
"                              non-zero exit code\n"
"  -s, --share                 share nodes with other jobs\n"
"  -l, --label                 prepend task number to lines of stdout/err\n"
"  -u, --unbuffered            do not line-buffer stdout/err\n"
"  -m, --distribution=type     distribution method for processes to nodes\n"
"                              (type = block|cyclic)\n"
"  -J, --job-name=jobname      name of job\n"
"      --jobid=id              run under already allocated job\n"
"      --mpi=type              type of MPI being used\n"
"  -b, --batch                 submit as batch job for later execution\n"
"  -T, --threads=threads       set srun launch fanout\n"
"  -W, --wait=sec              seconds to wait after first task exits\n"
"                              before killing job\n"
"  -q, --quit-on-interrupt     quit on single Ctrl-C\n"
"  -X, --disable-status        Disable Ctrl-C status feature\n"
"  -v, --verbose               verbose mode (multiple -v's increase verbosity)\n"
"  -Q, --quiet                 quiet mode (suppress informational messages)\n"
"  -d, --slurmd-debug=level    slurmd debug level\n"
"      --core=type             change default corefile format type\n"
"                              (type=\"list\" to list of valid formats)\n"
"  -P, --dependency=jobid      defer job until specified jobid completes\n"
"  -U, --account=name          charge job to specified account\n"
"\n"
"Allocate only:\n"
"  -A, --allocate              allocate resources and spawn a shell\n"
"      --no-shell              don't spawn shell in allocate mode\n"
"\n"
"Attach to running job:\n"
"  -a, --attach=jobid          attach to running job with specified id\n"
"  -j, --join                  when used with --attach, allow forwarding of\n"
"                              signals and stdin.\n"
"\n"
"Constraint options:\n"
"      --mincpus=n             minimum number of cpus per node\n"
"      --mem=MB                minimum amount of real memory\n"
"      --tmp=MB                minimum amount of temporary disk\n"
"      --contiguous            demand a contiguous range of nodes\n"
"  -C, --constraint=list       specify a list of constraints\n"
"  -w, --nodelist=hosts...     request a specific list of hosts\n"
"  -x, --exclude=hosts...      exclude a specific list of hosts\n"
"  -Z, --no-allocate           don't allocate nodes (must supply -w)\n"
"\n"
"Consumable resources related options\n" 
"      --exclusive             allocate nodes in exclusive mode when\n" 
"                              cpu consumable resource is enabled\n"
"\n"
#ifdef HAVE_BGL				/* Blue gene specific options */
  "Blue Gene related options:\n"
  "  -g, --geometry=XxYxZ        geometry constraints of the job\n"
  "  -R, --no-rotate             disable geometry rotation\n"
  "      --conn-type=type        constraint on type of connection, MESH or TORUS\n"
  "                              if not set, then tries to fit TORUS else MESH\n"
  "      --node-use=type         mode of node use VIRTUAL or COPROCESSOR\n"
"\n"
#endif
"Help options:\n"
"      --help                  show this help message\n"
"      --usage                 display brief usage message\n"
"\n"
"Other options:\n"
"  -V, --version               output version information and exit\n"
"\n"
);

}
