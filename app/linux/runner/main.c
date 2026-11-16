/*
 * Copyright (c) 2026 Joris Vink <joris@sanctorum.se>
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

#include <sys/types.h>
#include <sys/resource.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vigil.h"
#include "runner.h"
#include "flutter/generated_plugin_registrant.h"

static void	main_icon(void);
static void	main_error(const char *, ...)
		    __attribute__((format (printf, 1, 2)));

/*
 * Set up the window and the flutter view and run until the window
 * is closed.
 */
int
main(int argc, char **argv)
{
	struct rlimit		rl;
	FlView			*view;
	GdkRGBA			color;
	GtkWidget		*window;
	FlDartProject		*project;

	memset(&rl, 0, sizeof(rl));
	if (setrlimit(RLIMIT_CORE, &rl) == -1)
		fprintf(stderr, "vigil: setrlimit: %s\n", strerror(errno));

	gtk_init(&argc, &argv);
	main_icon();

	if (runner_single() == -1) {
		if (errno == EWOULDBLOCK) {
			main_error("Vigil is already running.");
			return (1);
		}

		main_error("Failed to lock the vigil directory: %s",
		    strerror(errno));
		return (1);
	}

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), "Vigil");
	gtk_window_set_default_size(GTK_WINDOW(window), 1120, 760);
	gtk_widget_set_size_request(window, 760, 520);

	g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	project = fl_dart_project_new();
	view = fl_view_new(project);
	g_object_unref(project);

	if (gdk_rgba_parse(&color, "#15120f") == FALSE) {
		fprintf(stderr, "vigil: bad background color\n");
		return (1);
	}

	fl_view_set_background_color(view, &color);
	gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view));

	fl_register_plugins(FL_PLUGIN_REGISTRY(view));

	if (runner_channel_init(view, GTK_WINDOW(window)) == -1) {
		main_error("Vigil could not set up its core.");
		return (1);
	}

	gtk_widget_show_all(window);
	gtk_widget_grab_focus(GTK_WIDGET(view));

	gtk_main();

	runner_stop();

	return (0);
}

/*
 * Set the icon for all our windows from data/vigil.png next to the
 * executable. Without it we run with the default icon.
 */
static void
main_icon(void)
{
	ssize_t		len;
	int		ret;
	char		*slash;
	GError		*error;
	char		exe[PATH_MAX], path[PATH_MAX];

	if ((len = readlink("/proc/self/exe", exe, sizeof(exe) - 1)) == -1) {
		fprintf(stderr, "vigil: readlink: %s\n", strerror(errno));
		return;
	}

	exe[len] = '\0';

	if ((slash = strrchr(exe, '/')) == NULL) {
		fprintf(stderr, "vigil: odd executable path '%s'\n", exe);
		return;
	}

	*slash = '\0';

	ret = snprintf(path, sizeof(path), "%s/data/vigil.png", exe);
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		fprintf(stderr, "vigil: icon path too long\n");
		return;
	}

	error = NULL;

	if (gtk_window_set_default_icon_from_file(path, &error) == FALSE) {
		fprintf(stderr, "vigil: %s\n", error->message);
		g_error_free(error);
	}
}

/*
 * Tell the user why we cannot start.
 */
static void
main_error(const char *fmt, ...)
{
	va_list		args;
	GtkWidget	*dialog;
	char		msg[256];

	va_start(args, fmt);
	(void)vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);

	fprintf(stderr, "vigil: %s\n", msg);

	dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
	    GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", msg);
	gtk_window_set_title(GTK_WINDOW(dialog), "Vigil");

	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
}
