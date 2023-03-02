static void show_about_dialog()
{
  GtkWidget *dialog = gtk_about_dialog_new();
  gtk_widget_set_name (dialog, "about-dialog");
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), PACKAGE_NAME);
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), darktable_package_version);
  char *copyright = g_strdup_printf(_("Copyright © darktable authors 2009-2022\nCopyright © Aurélien Pierre 2022-%s"), darktable_last_commit_year);
  gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog), copyright);
  g_free(copyright);
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
                                _("Organize and develop images from digital cameras"));
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "https://ansel.photos");
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog), _("Website"));
  char *icon = g_strdup("ansel");
  gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog), icon);
  g_free(icon);

  const char *str = _("all those of you that made previous releases possible");

#include "tools/darktable_authors.h"

  const char *final[] = {str, NULL };
  gtk_about_dialog_add_credit_section (GTK_ABOUT_DIALOG(dialog), _("and..."), final);
  gtk_about_dialog_set_translator_credits(GTK_ABOUT_DIALOG(dialog), _("translator-credits"));

  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

void open_doc_callback(GtkWidget *widget)
{
  // TODO: use translated URL when doc gets translated
  gtk_show_uri_on_window(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), "https://ansel.photos/en/doc", GDK_CURRENT_TIME, NULL);
}

void open_booking_callback(GtkWidget *widget)
{
  gtk_show_uri_on_window(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
  "https://photo.aurelienpierre.com/private-lessons-on-retouching-with-darktable/?lang=en", GDK_CURRENT_TIME, NULL);
}

void open_donate_callback(GtkWidget *widget)
{
  // TODO: use translated URL when doc gets translated
  gtk_show_uri_on_window(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), "https://liberapay.com/aurelienpierre", GDK_CURRENT_TIME, NULL);
}

void append_help(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  add_sub_menu_entry(menus, lists, _("Online documentation"), index, NULL, open_doc_callback, NULL, NULL, NULL);
  add_sub_menu_entry(menus, lists, _("Book a training session"), index, NULL, open_booking_callback, NULL, NULL, NULL);
  add_menu_separator(menus[index]);
  add_sub_menu_entry(menus, lists, _("Donate"), index, NULL, open_donate_callback, NULL, NULL, NULL);
  add_sub_menu_entry(menus, lists, _("About"), index, NULL, show_about_dialog, NULL, NULL, NULL);
}
