/* -*- Mode: c; c-basic-offset: 4; indent-tabs-mode: t; tab-width: 8; -*- */
/*
 * Copyright © 2006 Mozilla Corporation
 * Copyright © 2006 Red Hat, Inc.
 * Copyright © 2009 Chris Wilson
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of
 * the authors not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The authors make no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL,
 * INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Authors: Vladimir Vukicevic <vladimir@pobox.com>
 *          Carl Worth <cworth@cworth.org>
 *          Chris Wilson <chris@chris-wilson.co.uk>
 */

#define _GNU_SOURCE 1	/* for sched_getaffinity() */

#include "cairo-perf.h"
#include "cairo-stats.h"

#include "cairo-boilerplate-getopt.h"
#include <cairo-script-interpreter.h>

/* For basename */
#ifdef HAVE_LIBGEN_H
#include <libgen.h>
#endif

#include <sys/types.h>
#include <dirent.h>

#if HAVE_FCFINI
#include <fontconfig/fontconfig.h>
#endif

#ifdef HAVE_SCHED_H
#include <sched.h>
#endif

#define CAIRO_PERF_ITERATIONS_DEFAULT	15
#define CAIRO_PERF_LOW_STD_DEV		0.05
#define CAIRO_PERF_MIN_STD_DEV_COUNT	3
#define CAIRO_PERF_STABLE_STD_DEV_COUNT	3

/* Some targets just aren't that interesting for performance testing,
 * (not least because many of these surface types use a meta-surface
 * and as such defer the "real" rendering to later, so our timing
 * loops wouldn't count the real work, just the recording by the
 * meta-surface. */
static cairo_bool_t
target_is_measurable (cairo_boilerplate_target_t *target)
{
    if (target->content != CAIRO_CONTENT_COLOR_ALPHA)
	return FALSE;

    switch (target->expected_type) {
    case CAIRO_SURFACE_TYPE_IMAGE:
	if (strcmp (target->name, "pdf") == 0 ||
	    strcmp (target->name, "ps") == 0)
	{
	    return FALSE;
	}
	else
	{
	    return TRUE;
	}
    case CAIRO_SURFACE_TYPE_XLIB:
	if (strcmp (target->name, "xlib-fallback") == 0)
	{
	    return FALSE;
	}
	else
	{
	    return TRUE;
	}
    case CAIRO_SURFACE_TYPE_XCB:
    case CAIRO_SURFACE_TYPE_GLITZ:
    case CAIRO_SURFACE_TYPE_QUARTZ:
    case CAIRO_SURFACE_TYPE_WIN32:
    case CAIRO_SURFACE_TYPE_BEOS:
    case CAIRO_SURFACE_TYPE_DIRECTFB:
#if CAIRO_VERSION_MAJOR > 1 || (CAIRO_VERSION_MAJOR == 1 && CAIRO_VERSION_MINOR > 2)
    case CAIRO_SURFACE_TYPE_OS2:
#endif
	return TRUE;
    case CAIRO_SURFACE_TYPE_PDF:
    case CAIRO_SURFACE_TYPE_PS:
    case CAIRO_SURFACE_TYPE_SVG:
    default:
	return FALSE;
    }
}

cairo_bool_t
cairo_perf_can_run (cairo_perf_t	*perf,
		    const char		*name)
{
    unsigned int i;
    char *copy, *dot;

    if (perf->num_names == 0)
	return TRUE;

    copy = xstrdup (name);
    dot = strchr (copy, '.');
    if (dot != NULL)
	*dot = '\0';

    for (i = 0; i < perf->num_names; i++)
	if (strstr (copy, perf->names[i]))
	    break;

    free (copy);

    return i != perf->num_names;
}

static void
clear_surface (cairo_surface_t *surface)
{
    cairo_t *cr = cairo_create (surface);
    cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint (cr);
    cairo_destroy (cr);
}

static cairo_surface_t *
_similar_surface_create (void *closure,
			 cairo_content_t content,
			 double width, double height)
{
    return cairo_surface_create_similar (closure, content, width, height);
}

static void
execute (cairo_perf_t		 *perf,
	 cairo_script_interpreter_t *csi,
	 cairo_surface_t	 *target,
	 const char		 *trace)
{
    static cairo_bool_t first_run = TRUE;
    unsigned int i;
    cairo_perf_ticks_t *times;
    cairo_stats_t stats = {0.0, 0.0};
    int low_std_dev_count;
    char *trace_cpy, *name, *dot;
    const cairo_script_interpreter_hooks_t hooks = {
	.closure = target,
	.surface_create = _similar_surface_create
    };

    trace_cpy = xstrdup (trace);
    name = basename (trace_cpy);
    dot = strchr (name, '.');
    if (dot)
	*dot = '\0';

    if (perf->list_only) {
	printf ("%s\n", name);
	free (trace_cpy);
	return;
    }

    if (first_run) {
	if (perf->raw) {
	    printf ("[ # ] %s.%-s %s %s %s ...\n",
		    "backend", "content", "test-size", "ticks-per-ms", "time(ticks)");
	}

	if (perf->summary) {
	    fprintf (perf->summary,
		     "[ # ] %8s %28s %8s %5s %5s %s\n",
		     "backend", "test", "min(s)", "median(s)",
		     "stddev.", "iterations");
	}
	first_run = FALSE;
    }

    times = perf->times;

    if (perf->summary) {
	fprintf (perf->summary,
		 "[%3d] %8s %28s ",
		 perf->test_number,
		 perf->target->name,
		 name);
	fflush (perf->summary);
    }

    cairo_script_interpreter_install_hooks (csi, &hooks);

    low_std_dev_count = 0;
    for (i = 0; i < perf->iterations; i++) {
	cairo_perf_yield ();
	cairo_perf_timer_start ();

	cairo_script_interpreter_run (csi, trace);
	clear_surface (target); /* queue a write to the sync'ed surface */

	cairo_perf_timer_stop ();
	times[i] = cairo_perf_timer_elapsed ();

	if (perf->raw) {
	    if (i == 0)
		printf ("[*] %s.%s %s.%d %g",
			perf->target->name,
			"rgba",
			name,
			0,
			cairo_perf_ticks_per_second () / 1000.0);
	    printf (" %lld", (long long) times[i]);
	    fflush (stdout);
	} else if (! perf->exact_iterations) {
	    if (i > CAIRO_PERF_MIN_STD_DEV_COUNT) {
		_cairo_stats_compute (&stats, times, i+1);

		if (stats.std_dev <= CAIRO_PERF_LOW_STD_DEV) {
		    if (++low_std_dev_count >= CAIRO_PERF_STABLE_STD_DEV_COUNT)
			break;
		} else {
		    low_std_dev_count = 0;
		}
	    }
	}
    }

    if (perf->summary) {
	_cairo_stats_compute (&stats, times, i);
	fprintf (perf->summary,
		 "%#8.3f %#8.3f %#5.2f%% %3d\n",
		 stats.min_ticks / (double) cairo_perf_ticks_per_second (),
		 stats.median_ticks / (double) cairo_perf_ticks_per_second (),
		 stats.std_dev * 100.0,
		 stats.iterations);
	fflush (perf->summary);
    }

    if (perf->raw) {
	printf ("\n");
	fflush (stdout);
    }

    perf->test_number++;
    free (trace_cpy);
}

static void
usage (const char *argv0)
{
    fprintf (stderr,
"Usage: %s [-l] [-r] [-v] [-i iterations] [test-names ... | traces ...]\n"
"       %s -l\n"
"\n"
"Run the cairo performance test suite over the given tests (all by default)\n"
"The command-line arguments are interpreted as follows:\n"
"\n"
"  -r	raw; display each time measurement instead of summary statistics\n"
"  -v	verbose; in raw mode also show the summaries\n"
"  -i	iterations; specify the number of iterations per test case\n"
"  -l	list only; just list selected test case names without executing\n"
"\n"
"If test names are given they are used as sub-string matches so a command\n"
"such as \"cairo-perf-trace firefox\" can be used to run all firefox traces.\n"
"Alternatively, you can specify a list of filenames to execute.\n",
	     argv0, argv0);
}

static void
parse_options (cairo_perf_t *perf, int argc, char *argv[])
{
    int c;
    const char *iters;
    char *end;
    int verbose = 0;

    if ((iters = getenv ("CAIRO_PERF_ITERATIONS")) && *iters)
	perf->iterations = strtol (iters, NULL, 0);
    else
	perf->iterations = CAIRO_PERF_ITERATIONS_DEFAULT;
    perf->exact_iterations = 0;

    perf->raw = FALSE;
    perf->list_only = FALSE;
    perf->names = NULL;
    perf->num_names = 0;
    perf->summary = stdout;

    while (1) {
	c = _cairo_getopt (argc, argv, "i:lrv");
	if (c == -1)
	    break;

	switch (c) {
	case 'i':
	    perf->exact_iterations = TRUE;
	    perf->iterations = strtoul (optarg, &end, 10);
	    if (*end != '\0') {
		fprintf (stderr, "Invalid argument for -i (not an integer): %s\n",
			 optarg);
		exit (1);
	    }
	    break;
	case 'l':
	    perf->list_only = TRUE;
	    break;
	case 'r':
	    perf->raw = TRUE;
	    perf->summary = NULL;
	    break;
	case 'v':
	    verbose = 1;
	    break;
	default:
	    fprintf (stderr, "Internal error: unhandled option: %c\n", c);
	    /* fall-through */
	case '?':
	    usage (argv[0]);
	    exit (1);
	}
    }

    if (verbose && perf->summary == NULL)
	perf->summary = stderr;

    if (optind < argc) {
	perf->names = &argv[optind];
	perf->num_names = argc - optind;
    }
}

static int
check_cpu_affinity (void)
{
#ifdef HAVE_SCHED_GETAFFINITY
    cpu_set_t affinity;
    int i, cpu_count;

    if (sched_getaffinity (0, sizeof (affinity), &affinity)) {
        perror ("sched_getaffinity");
        return -1;
    }

    for (i = 0, cpu_count = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET (i, &affinity))
            ++cpu_count;
    }

    if (cpu_count > 1) {
	fputs ("WARNING: cairo-perf has not been bound to a single CPU.\n",
	       stderr);
        return -1;
    }

    return 0;
#else
    fputs ("WARNING: Cannot check CPU affinity for this platform.\n",
	   stderr);
    return -1;
#endif
}

static void
cairo_perf_fini (cairo_perf_t *perf)
{
    cairo_boilerplate_free_targets (perf->targets);
    free (perf->times);
#if 0 /* XXX */
    cairo_debug_reset_static_data ();
#if HAVE_FCFINI
    FcFini ();
#endif
#endif
}

static cairo_bool_t
have_trace_filenames (cairo_perf_t *perf)
{
    unsigned int i;

    if (perf->num_names == 0)
	return FALSE;

    for (i = 0; i < perf->num_names; i++)
	if (access (perf->names[i], R_OK) == 0)
	    return TRUE;

    return FALSE;
}

static void
cairo_perf_trace (cairo_perf_t *perf,
		  cairo_boilerplate_target_t *target,
		  cairo_script_interpreter_t *csi,
		  const char *trace)
{
    cairo_surface_t *surface;
    void *closure;

    surface = (target->create_surface) (NULL,
					CAIRO_CONTENT_COLOR_ALPHA,
					1, 1,
					1, 1,
					CAIRO_BOILERPLATE_MODE_PERF,
					0,
					&closure);
    if (surface == NULL) {
	fprintf (stderr,
		 "Error: Failed to create target surface: %s\n",
		 target->name);
	return;
    }

    cairo_perf_timer_set_synchronize (target->synchronize, closure);

    execute (perf, csi, surface, trace);

    cairo_surface_destroy (surface);

    if (target->cleanup)
	target->cleanup (closure);
}

static void
warn_no_traces (const char *message, const char *trace_dir)
{
    fprintf (stderr,
"Error: %s '%s'.\n"
"Have you cloned the cairo-traces repository and uncompressed the traces?\n"
"  git clone git://anongit.freedesktop.org/cairo-traces\n"
"  cd cairo-traces && make\n"
"Or set the env.var CAIRO_TRACE_DIR to point to your traces?\n",
	    message, trace_dir);
}

int
main (int argc, char *argv[])
{
    cairo_perf_t perf;
    cairo_script_interpreter_t *csi;
    const char *trace_dir = "cairo-traces";
    cairo_bool_t names_are_traces;
    unsigned int n;
    int i;

    parse_options (&perf, argc, argv);

    if (! perf.list_only && check_cpu_affinity ()) {
        fputs ("NOTICE: cairo-perf and the X server should be bound to CPUs (either the same\n"
	       "or separate) on SMP systems. Not doing so causes random results when the X\n"
	       "server is moved to or from cairo-perf's CPU during the benchmarks:\n"
	       "\n"
	       "    $ sudo taskset -cp 0 $(pidof X)\n"
	       "    $ taskset -cp 1 $$\n"
	       "\n"
	       "See taskset(1) for information about changing CPU affinity.\n\n",
	       stderr);
    }

    if (getenv ("CAIRO_TRACE_DIR") != NULL)
	trace_dir = getenv ("CAIRO_TRACE_DIR");

    perf.targets = cairo_boilerplate_get_targets (&perf.num_targets, NULL);
    perf.times = xmalloc (perf.iterations * sizeof (cairo_perf_ticks_t));

    csi = cairo_script_interpreter_create ();

    /* do we have a list of filenames? */
    names_are_traces = have_trace_filenames (&perf);

    for (i = 0; i < perf.num_targets; i++) {
        cairo_boilerplate_target_t *target = perf.targets[i];

	if (! perf.list_only && ! target_is_measurable (target))
	    continue;

	perf.target = target;
	perf.test_number = 0;

	if (names_are_traces) {
	    for (n = 0; n < perf.num_names; n++) {
		if (access (perf.names[n], R_OK) == 0)
		    cairo_perf_trace (&perf, target, csi, perf.names[n]);
	    }
	} else {
	    DIR *dir;
	    struct dirent *de;
	    int num_traces = 0;

	    dir = opendir (trace_dir);
	    if (dir == NULL) {
		warn_no_traces ("Failed to open directory", trace_dir);
		return 1;
	    }

	    while ((de = readdir (dir)) != NULL) {
		char *trace;
		const char *dot;

		dot = strrchr (de->d_name, '.');
		if (dot == NULL)
		    continue;
		if (strcmp (dot, ".trace"))
		    continue;

		num_traces++;
		if (! cairo_perf_can_run (&perf, de->d_name))
		    continue;

		xasprintf (&trace, "%s/%s", trace_dir, de->d_name);
		cairo_perf_trace (&perf, target, csi, trace);
		free (trace);

	    }
	    closedir (dir);

	    if (num_traces == 0) {
		warn_no_traces ("Found no traces in", trace_dir);
		return 1;
	    }
	}

	if (perf.list_only)
	    break;
    }

    cairo_script_interpreter_destroy (csi);
    cairo_perf_fini (&perf);

    return 0;
}