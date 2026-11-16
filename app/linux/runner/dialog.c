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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sodium.h>

#include "vigil.h"
#include "runner.h"

static GtkWidget	*dialog_error(GtkWidget *);
static void		dialog_fail(GtkWidget *, const char *);
static GtkWidget	*dialog_entry(GtkWidget *, const char *);
static GtkWidget	*dialog_new(GtkWindow *, const char *, GtkWidget **);

/*
 * Ask for the passphrase, twice when confirm is set (when it is chosen).
 * On success out holds a copy of len bytes the caller wipes and frees.
 * Returns -1 when the user cancelled.
 */
int
runner_passphrase(GtkWindow *parent, const char *reason, int confirm,
    char **out, size_t *len)
{
	int			ret;
	size_t			plen;
	GtkWidget		*dialog;
	const char		*pass, *again;
	GtkWidget		*box, *error, *first, *second;

	PRECOND(parent != NULL);
	PRECOND(reason != NULL);
	PRECOND(out != NULL);
	PRECOND(len != NULL);

	*len = 0;
	*out = NULL;

	ret = -1;
	second = NULL;
	dialog = dialog_new(parent, reason, &box);
	first = dialog_entry(box, "Passphrase");

	if (confirm)
		second = dialog_entry(box, "Repeat the passphrase");

	error = dialog_error(dialog);

	while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
		pass = gtk_entry_get_text(GTK_ENTRY(first));
		if ((plen = strlen(pass)) == 0) {
			dialog_fail(error, "The passphrase cannot be empty.");
			continue;
		}

		if (second != NULL) {
			again = gtk_entry_get_text(GTK_ENTRY(second));
			if (strcmp(pass, again)) {
				dialog_fail(error,
				    "The passphrases do not match.");
				gtk_entry_set_text(GTK_ENTRY(second), "");
				continue;
			}
		}

		if ((*out = malloc(plen)) == NULL)
			fatal("malloc");

		*len = plen;
		memcpy(*out, pass, plen);
		ret = 0;
		break;
	}

	gtk_entry_set_text(GTK_ENTRY(first), "");

	if (second != NULL)
		gtk_entry_set_text(GTK_ENTRY(second), "");

	gtk_widget_destroy(dialog);

	return (ret);
}

/*
 * Let the user pick a file. On success path holds the path from malloc()
 * that the caller must free. Returns -1 when the user cancelled.
 */
int
runner_pick(GtkWindow *parent, const char *title, char **path)
{
	int			ret;
	GtkWidget		*dialog;
	char			*filename;

	PRECOND(parent != NULL);
	PRECOND(title != NULL);
	PRECOND(path != NULL);

	*path = NULL;

	dialog = gtk_file_chooser_dialog_new(title, parent,
	    GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL,
	    "_Open", GTK_RESPONSE_ACCEPT, NULL);

	gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), TRUE);

	ret = -1;

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		filename = gtk_file_chooser_get_filename(
		    GTK_FILE_CHOOSER(dialog));

		if (filename != NULL) {
			if ((*path = strdup(filename)) != NULL)
				ret = 0;
			g_free(filename);
		}
	}

	gtk_widget_destroy(dialog);

	return (ret);
}

/*
 * Let the user pick where to save a file, suggesting the given name. On
 * success path holds the path from malloc() that the caller must free.
 * Returns -1 when the user cancelled.
 */
int
runner_save(GtkWindow *parent, const char *name, char **path)
{
	int			ret;
	GtkWidget		*dialog;
	char			*filename;

	PRECOND(parent != NULL);
	PRECOND(name != NULL);
	PRECOND(path != NULL);

	*path = NULL;

	dialog = gtk_file_chooser_dialog_new("Save", parent,
	    GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel", GTK_RESPONSE_CANCEL,
	    "_Save", GTK_RESPONSE_ACCEPT, NULL);

	gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), TRUE);
	gtk_file_chooser_set_do_overwrite_confirmation(
	    GTK_FILE_CHOOSER(dialog), TRUE);
	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), name);

	ret = -1;

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		filename = gtk_file_chooser_get_filename(
		    GTK_FILE_CHOOSER(dialog));

		if (filename != NULL) {
			if ((*path = strdup(filename)) != NULL)
				ret = 0;
			g_free(filename);
		}
	}

	gtk_widget_destroy(dialog);

	return (ret);
}

/*
 * Make a dialog with the given reason in it, box is where the entries
 * go.
 */
static GtkWidget *
dialog_new(GtkWindow *parent, const char *reason, GtkWidget **box)
{
	GtkWidget	*dialog, *label;

	PRECOND(parent != NULL);
	PRECOND(reason != NULL);
	PRECOND(box != NULL);

	dialog = gtk_dialog_new_with_buttons("Vigil", parent,
	    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
	    "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);

	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
	gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

	*box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_container_set_border_width(GTK_CONTAINER(*box), 18);
	gtk_box_set_spacing(GTK_BOX(*box), 8);

	label = gtk_label_new(reason);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 48);
	gtk_container_add(GTK_CONTAINER(*box), label);

	return (dialog);
}

/*
 * Add a hidden entry with the given label to box.
 */
static GtkWidget *
dialog_entry(GtkWidget *box, const char *text)
{
	GtkWidget	*entry, *label;

	PRECOND(box != NULL);
	PRECOND(text != NULL);

	label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_set_margin_top(label, 8);
	gtk_container_add(GTK_CONTAINER(box), label);

	entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
	gtk_entry_set_input_purpose(GTK_ENTRY(entry),
	    GTK_INPUT_PURPOSE_PASSWORD);

	gtk_container_add(GTK_CONTAINER(box), entry);

	return (entry);
}

/*
 * Add the label that tells what is wrong with the input, shows the
 * dialog and keeps the label hidden until there is something to say.
 */
static GtkWidget *
dialog_error(GtkWidget *dialog)
{
	GtkWidget	*box, *error;

	PRECOND(dialog != NULL);

	box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

	error = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(error), 0);
	gtk_container_add(GTK_CONTAINER(box), error);

	gtk_widget_show_all(dialog);
	gtk_widget_hide(error);

	return (error);
}

/*
 * Tell the user what is wrong with the input.
 */
static void
dialog_fail(GtkWidget *error, const char *text)
{
	PRECOND(error != NULL);
	PRECOND(text != NULL);

	gtk_label_set_text(GTK_LABEL(error), text);
	gtk_widget_show(error);
}
