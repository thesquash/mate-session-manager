/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2006 Vincent Untz
 * Copyright (C) 2008 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * Authors:
 *	Vincent Untz <vuntz@gnome.org>
 */

#include <config.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "gsm-logout-dialog.h"
#ifdef HAVE_SYSTEMD
#include "gsm-systemd.h"
#endif
#include "gsm-consolekit.h"
#include "mdm.h"
#include "gsm-util.h"

#define GSM_ICON_LOGOUT   "system-log-out"
#define GSM_ICON_SHUTDOWN "system-shutdown"

#define SESSION_SCHEMA     "org.mate.session"
#define KEY_LOGOUT_TIMEOUT "logout-timeout"

#define LOCKDOWN_SCHEMA            "org.mate.lockdown"
#define KEY_USER_SWITCHING_DISABLE "disable-user-switching"

typedef enum {
        GSM_DIALOG_LOGOUT_TYPE_LOGOUT,
        GSM_DIALOG_LOGOUT_TYPE_SHUTDOWN
} GsmDialogLogoutType;

struct _GsmLogoutDialog
{
        GtkDialog            parent;
        GsmDialogLogoutType  type;
#ifdef HAVE_SYSTEMD
        GsmSystemd          *systemd;
#endif
        GsmConsolekit       *consolekit;

        GtkWidget           *primary_label;
        GtkWidget           *secondary_label;

        GtkWidget           *progress_overlay;
        GtkWidget           *progressbar;
        GtkStyleProvider    *progressbar_style_provider;
        GtkWidget           *progress_label;

        int                  timeout;
        unsigned int         timeout_id;

        unsigned int         default_response;
};

static GsmLogoutDialog *current_dialog = NULL;

static void gsm_logout_dialog_set_timeout  (GsmLogoutDialog *logout_dialog);

static void gsm_logout_dialog_destroy  (GsmLogoutDialog *logout_dialog,
                                        gpointer         data);

static void gsm_logout_dialog_show     (GsmLogoutDialog *logout_dialog,
                                        gpointer         data);

static void gsm_logout_dialog_progress_label_size_allocate (GtkWidget       *label,
                                                            GdkRectangle    *allocation,
                                                            gpointer         data);

enum {
        PROP_0,
        PROP_MESSAGE_TYPE
};

G_DEFINE_TYPE (GsmLogoutDialog, gsm_logout_dialog, GTK_TYPE_DIALOG);

static void
gsm_logout_dialog_class_init (GsmLogoutDialogClass *klass)
{
}

static void
gsm_logout_dialog_init (GsmLogoutDialog *logout_dialog)
{
        logout_dialog->timeout_id = 0;
        logout_dialog->timeout = 0;
        logout_dialog->default_response = GTK_RESPONSE_CANCEL;

        GtkStyleContext *context;
        context = gtk_widget_get_style_context (GTK_WIDGET (logout_dialog));
        gtk_style_context_add_class (context, "logout-dialog");

        gtk_window_set_skip_taskbar_hint (GTK_WINDOW (logout_dialog), TRUE);
        gtk_window_set_keep_above (GTK_WINDOW (logout_dialog), TRUE);
        gtk_window_stick (GTK_WINDOW (logout_dialog));
#ifdef HAVE_SYSTEMD
        if (LOGIND_RUNNING())
            logout_dialog->systemd = gsm_get_systemd ();
        else
#endif
        logout_dialog->consolekit = gsm_get_consolekit ();

        g_signal_connect (logout_dialog,
                          "destroy",
                          G_CALLBACK (gsm_logout_dialog_destroy),
                          NULL);

        g_signal_connect (logout_dialog,
                          "show",
                          G_CALLBACK (gsm_logout_dialog_show),
                          NULL);
}

static void
gsm_logout_dialog_destroy (GsmLogoutDialog *logout_dialog,
                           gpointer         data)
{
        if (logout_dialog->timeout_id != 0) {
                g_source_remove (logout_dialog->timeout_id);
                logout_dialog->timeout_id = 0;
        }
#ifdef HAVE_SYSTEMD
        if (logout_dialog->systemd) {
                g_object_unref (logout_dialog->systemd);
                logout_dialog->systemd = NULL;
        }
#endif

        if (logout_dialog->consolekit) {
                g_object_unref (logout_dialog->consolekit);
                logout_dialog->consolekit = NULL;
        }

        current_dialog = NULL;
}

static gboolean
gsm_logout_supports_system_suspend (GsmLogoutDialog *logout_dialog)
{
        gboolean ret;
        ret = FALSE;
#ifdef HAVE_SYSTEMD
        if (LOGIND_RUNNING())
            ret = gsm_systemd_can_suspend (logout_dialog->systemd);
        else
#endif
        ret = gsm_consolekit_can_suspend (logout_dialog->consolekit);
        return ret;
}

static gboolean
gsm_logout_supports_system_hibernate (GsmLogoutDialog *logout_dialog)
{
        gboolean ret;
        ret = FALSE;
#ifdef HAVE_SYSTEMD
        if (LOGIND_RUNNING())
            ret = gsm_systemd_can_hibernate (logout_dialog->systemd);
        else
#endif
        ret = gsm_consolekit_can_hibernate (logout_dialog->consolekit);
        return ret;
}

static gboolean
gsm_logout_supports_switch_user (GsmLogoutDialog *logout_dialog)
{
        GSettings *settings;
        gboolean   ret = FALSE;
        gboolean   locked;

        settings = g_settings_new (LOCKDOWN_SCHEMA);

        locked = g_settings_get_boolean (settings, KEY_USER_SWITCHING_DISABLE);
        g_object_unref (settings);

        if (!locked) {
#ifdef HAVE_SYSTEMD
            if (LOGIND_RUNNING())
                ret = gsm_systemd_can_switch_user (logout_dialog->systemd);
            else
#endif
            ret = gsm_consolekit_can_switch_user (logout_dialog->consolekit);
        }

        return ret;
}

static gboolean
gsm_logout_supports_reboot (GsmLogoutDialog *logout_dialog)
{
        gboolean ret;

#ifdef HAVE_SYSTEMD
        if (LOGIND_RUNNING())
            ret = gsm_systemd_can_restart (logout_dialog->systemd);
        else
#endif
        ret = gsm_consolekit_can_restart (logout_dialog->consolekit);
        if (!ret) {
                ret = mdm_supports_logout_action (MDM_LOGOUT_ACTION_REBOOT);
        }

        return ret;
}

static gboolean
gsm_logout_supports_shutdown (GsmLogoutDialog *logout_dialog)
{
        gboolean ret;

#ifdef HAVE_SYSTEMD
        if (LOGIND_RUNNING())
            ret = gsm_systemd_can_stop (logout_dialog->systemd);
        else
#endif
        ret = gsm_consolekit_can_stop (logout_dialog->consolekit);

        if (!ret) {
                ret = mdm_supports_logout_action (MDM_LOGOUT_ACTION_SHUTDOWN);
        }

        return ret;
}

static void
gsm_logout_dialog_show (GsmLogoutDialog *logout_dialog, gpointer user_data)
{
        gsm_logout_dialog_set_timeout (logout_dialog);
}

static gboolean
gsm_logout_dialog_timeout (gpointer data)
{
        GsmLogoutDialog *logout_dialog;
        char            *seconds_warning;
        char            *secondary_text;
        static char     *session_type = NULL;
        static gboolean  is_not_login;

        logout_dialog = (GsmLogoutDialog *) data;

        if (!logout_dialog->timeout) {
                gtk_dialog_response (GTK_DIALOG (logout_dialog),
                                     logout_dialog->default_response);

                return FALSE;
        }

        switch (logout_dialog->type) {
        case GSM_DIALOG_LOGOUT_TYPE_LOGOUT:
                seconds_warning = ngettext ("You will be automatically logged "
                                            "out in %d second",
                                            "You will be automatically logged "
                                            "out in %d seconds",
                                            logout_dialog->timeout);
                break;

        case GSM_DIALOG_LOGOUT_TYPE_SHUTDOWN:
                seconds_warning = ngettext ("This system will be automatically "
                                            "shut down in %d second",
                                            "This system will be automatically "
                                            "shut down in %d seconds",
                                            logout_dialog->timeout);
                break;

        default:
                g_assert_not_reached ();
        }
        seconds_warning = g_strdup_printf (seconds_warning, logout_dialog->timeout);

        if (session_type == NULL) {
#ifdef HAVE_SYSTEMD
                if (LOGIND_RUNNING()) {
                    GsmSystemd *systemd;
                    systemd = gsm_get_systemd ();
                    session_type = gsm_systemd_get_current_session_type (systemd);
                    g_object_unref (systemd);
                    is_not_login = (g_strcmp0 (session_type, GSM_SYSTEMD_SESSION_TYPE_LOGIN_WINDOW) != 0);
                }
                else {
#endif
                GsmConsolekit *consolekit;
                consolekit = gsm_get_consolekit ();
                session_type = gsm_consolekit_get_current_session_type (consolekit);
                g_object_unref (consolekit);
                is_not_login = (g_strcmp0 (session_type, GSM_CONSOLEKIT_SESSION_TYPE_LOGIN_WINDOW) != 0);
#ifdef HAVE_SYSTEMD
                }
#endif
        }

        if (is_not_login) {
                char *name;

                name = g_locale_to_utf8 (g_get_real_name (), -1, NULL, NULL, NULL);

                if (!name || name[0] == '\0' || strcmp (name, "Unknown") == 0) {
                        name = g_locale_to_utf8 (g_get_user_name (), -1 , NULL, NULL, NULL);
                }

                if (!name) {
                        name = g_strdup (g_get_user_name ());
                }

                secondary_text = g_strdup_printf (_("You are currently logged in as \"%s\"."), name);

                g_free (name);
        } else {
                secondary_text = g_strdup (seconds_warning);
        }

        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (logout_dialog->progressbar),
                                       logout_dialog->timeout / 60.0);
        gtk_label_set_text (GTK_LABEL (logout_dialog->progress_label),
                            seconds_warning);

        gtk_label_set_text (GTK_LABEL (logout_dialog->secondary_label),
                            secondary_text);

        logout_dialog->timeout--;

        g_free (secondary_text);
        g_free (seconds_warning);

        return TRUE;
}

static void
gsm_logout_dialog_set_timeout (GsmLogoutDialog *logout_dialog)
{
        GSettings *settings;

        settings = g_settings_new (SESSION_SCHEMA);

        logout_dialog->timeout = g_settings_get_int (settings, KEY_LOGOUT_TIMEOUT);

        if (logout_dialog->timeout > 0) {
                /* Sets the secondary text */
                gsm_logout_dialog_timeout (logout_dialog);

                if (logout_dialog->timeout_id != 0) {
                        g_source_remove (logout_dialog->timeout_id);
                }

                logout_dialog->timeout_id = g_timeout_add (1000,
                                                           gsm_logout_dialog_timeout,
                                                           logout_dialog);
        }
        else {
                gtk_widget_hide (logout_dialog->progress_overlay);
        }

        g_object_unref (settings);
}

/*
 * This function is a callback which is called when the `size-allocate` signal
 * is emitted by our custom progress label.  This function determines the new
 * height of the progress label and then resizes the progress bar trough and
 * indicator to match the height of the label text.  This is necessary because
 * otherwise there is no guarantee that the label text will fit within the
 * constraints of the progress trough, and if the text does not fit within the
 * trough, the text will "fall off" the progress bar, which looks ugly.
 */
static void
gsm_logout_dialog_progress_label_size_allocate (GtkWidget       *label,
                                                GdkRectangle    *allocation,
                                                gpointer         data)
{
  GsmLogoutDialog *logout_dialog = GSM_LOGOUT_DIALOG (data);
  char            *css_text;

  css_text = g_strdup_printf ("progressbar trough,\n"
                              "progressbar progress\n"
                              "{\n"
                              "  min-height: %dpx;\n"
                              "}\n",
                              allocation->height);

  gtk_css_provider_load_from_data (GTK_CSS_PROVIDER (logout_dialog->progressbar_style_provider),
                                   css_text, -1, NULL);

  g_free (css_text);
}

static GtkWidget *
gsm_get_dialog (GsmDialogLogoutType type,
                GdkScreen          *screen,
                guint32             activate_time)
{
        GsmLogoutDialog *logout_dialog;
        GtkWidget       *button_box;
        GtkWidget       *grid;
        GtkWidget       *dialog_icon;
        GtkSizeGroup    *size_group;
        const char      *primary_text;
        const char      *icon_name;

        if (current_dialog != NULL) {
                gtk_widget_destroy (GTK_WIDGET (current_dialog));
        }

        logout_dialog = g_object_new (GSM_TYPE_LOGOUT_DIALOG, NULL);

        current_dialog = logout_dialog;

        gtk_window_set_title (GTK_WINDOW (logout_dialog), "");
        gtk_window_set_resizable (GTK_WINDOW (logout_dialog), FALSE);

        /*
         * The following 2 lines are to stretch out the buttons at the bottom
         * of the dialog, as is in vogue in GTK+ 3:
         */
        button_box = gtk_dialog_get_action_area (GTK_DIALOG (logout_dialog));
        gtk_button_box_set_layout (GTK_BUTTON_BOX (button_box), GTK_BUTTONBOX_EXPAND);

        logout_dialog->type = type;

        icon_name = NULL;
        primary_text = NULL;

        switch (type) {
        case GSM_DIALOG_LOGOUT_TYPE_LOGOUT:
                icon_name    = GSM_ICON_LOGOUT;
                primary_text = _("Log out of this system now?");

                logout_dialog->default_response = GSM_LOGOUT_RESPONSE_LOGOUT;

                if (gsm_logout_supports_switch_user (logout_dialog)) {
                        gsm_util_dialog_add_button (GTK_DIALOG (logout_dialog),
                                                    _("_Switch User"), "system-users",
                                                    GSM_LOGOUT_RESPONSE_SWITCH_USER);
                }

                gsm_util_dialog_add_button (GTK_DIALOG (logout_dialog),
                                            _("_Cancel"), "process-stop",
                                            GTK_RESPONSE_CANCEL);

                gsm_util_dialog_add_button (GTK_DIALOG (logout_dialog),
                                            _("_Log Out"), "system-log-out",
                                            GSM_LOGOUT_RESPONSE_LOGOUT);

                break;
        case GSM_DIALOG_LOGOUT_TYPE_SHUTDOWN:
                icon_name    = GSM_ICON_SHUTDOWN;
                primary_text = _("Shut down this system now?");

                logout_dialog->default_response = GSM_LOGOUT_RESPONSE_SHUTDOWN;

                if (gsm_logout_supports_system_suspend (logout_dialog)) {
                        gsm_util_dialog_add_button (GTK_DIALOG (logout_dialog),
                                                    _("S_uspend"), "battery",
                                                    GSM_LOGOUT_RESPONSE_SLEEP);
                }

                if (gsm_logout_supports_system_hibernate (logout_dialog)) {
                        gsm_util_dialog_add_button (GTK_DIALOG (logout_dialog),
                                                    _("_Hibernate"), "drive-harddisk",
                                                    GSM_LOGOUT_RESPONSE_HIBERNATE);
                }

                if (gsm_logout_supports_reboot (logout_dialog)) {
                        gsm_util_dialog_add_button (GTK_DIALOG (logout_dialog),
                                                    _("_Restart"), "view-refresh",
                                                    GSM_LOGOUT_RESPONSE_REBOOT);
                }

                gsm_util_dialog_add_button (GTK_DIALOG (logout_dialog),
                                            _("_Cancel"), "process-stop",
                                            GTK_RESPONSE_CANCEL);

                if (gsm_logout_supports_shutdown (logout_dialog)) {
                        gsm_util_dialog_add_button (GTK_DIALOG (logout_dialog),
                                                    _("_Shut Down"), "system-shutdown",
                                                    GSM_LOGOUT_RESPONSE_SHUTDOWN);
                }
                break;
        default:
                g_assert_not_reached ();
        }

        /* Set up the dialog with all the necessary user interface jazz. */
        grid = gtk_grid_new ();
        gtk_grid_set_row_spacing     (GTK_GRID (grid), 15);
        gtk_grid_set_column_spacing  (GTK_GRID (grid), 10);
        gtk_widget_set_margin_top    (grid, 5);
        gtk_widget_set_margin_start  (grid, 5);
        gtk_widget_set_margin_bottom (grid, 15);
        gtk_widget_set_margin_end    (grid, 5);

        dialog_icon = gtk_image_new_from_icon_name (icon_name,
                                                    GTK_ICON_SIZE_DIALOG);
        gtk_widget_set_valign (dialog_icon, GTK_ALIGN_START);
        gtk_widget_set_halign (dialog_icon, GTK_ALIGN_START);
        gtk_grid_attach (GTK_GRID (grid), dialog_icon,
                         0, 0, 1, 3);

        logout_dialog->primary_label = gtk_label_new (primary_text);
        gtk_widget_set_halign (logout_dialog->primary_label, GTK_ALIGN_START);
        gtk_widget_set_hexpand (logout_dialog->primary_label, TRUE);
        gtk_grid_attach (GTK_GRID (grid), logout_dialog->primary_label,
                         1, 0, 1, 1);
        logout_dialog->secondary_label = gtk_label_new (NULL);
        gtk_widget_set_halign (logout_dialog->secondary_label, GTK_ALIGN_START);
        gtk_widget_set_hexpand (logout_dialog->secondary_label, TRUE);
        gtk_grid_attach (GTK_GRID (grid), logout_dialog->secondary_label,
                         1, 1, 1, 1);

        logout_dialog->progress_overlay = gtk_overlay_new ();
        gtk_widget_set_hexpand (logout_dialog->progress_overlay, TRUE);
        logout_dialog->progressbar = gtk_progress_bar_new ();
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (logout_dialog->progressbar), 1.0);
        gtk_container_add (GTK_CONTAINER (logout_dialog->progress_overlay),
                           logout_dialog->progressbar);

        logout_dialog->progress_label = gtk_label_new (NULL);
        gtk_widget_set_valign (logout_dialog->progress_label, GTK_ALIGN_CENTER);
        gtk_overlay_add_overlay (GTK_OVERLAY (logout_dialog->progress_overlay),
                                 logout_dialog->progress_label);
        size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
        gtk_size_group_add_widget (size_group, logout_dialog->progressbar);
        gtk_size_group_add_widget (size_group, logout_dialog->progress_label);
        gtk_grid_attach (GTK_GRID (grid), logout_dialog->progress_overlay,
                         0, 2, 2, 1);
        gtk_widget_show_all (grid);
        gtk_container_add (GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (logout_dialog))),
                           grid);

        logout_dialog->progressbar_style_provider = GTK_STYLE_PROVIDER (gtk_css_provider_new ());
        gtk_style_context_add_provider (gtk_widget_get_style_context (logout_dialog->progressbar),
                                        logout_dialog->progressbar_style_provider,
                                        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_signal_connect (logout_dialog->progress_label,
                          "size-allocate",
                          G_CALLBACK (gsm_logout_dialog_progress_label_size_allocate),
                          logout_dialog);

        gtk_window_set_icon_name (GTK_WINDOW (logout_dialog), icon_name);
        gtk_window_set_position (GTK_WINDOW (logout_dialog), GTK_WIN_POS_CENTER_ALWAYS);

        gtk_dialog_set_default_response (GTK_DIALOG (logout_dialog),
                                         logout_dialog->default_response);

        gtk_window_set_screen (GTK_WINDOW (logout_dialog), screen);

        return GTK_WIDGET (logout_dialog);
}

GtkWidget *
gsm_get_shutdown_dialog (GdkScreen *screen,
                         guint32    activate_time)
{
        return gsm_get_dialog (GSM_DIALOG_LOGOUT_TYPE_SHUTDOWN,
                               screen,
                               activate_time);
}

GtkWidget *
gsm_get_logout_dialog (GdkScreen *screen,
                       guint32    activate_time)
{
        return gsm_get_dialog (GSM_DIALOG_LOGOUT_TYPE_LOGOUT,
                               screen,
                               activate_time);
}
