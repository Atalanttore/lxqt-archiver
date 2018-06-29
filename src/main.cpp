// LXQt Archiver
// Copyright (C) 2018 The LXQt team
// Some of the following code is derived from Engrampa and File Roller

/*
 *  Engrampa
 *
 *  Copyright (C) 2001, 2003 Free Software Foundation, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Street #330, Boston, MA 02110-1301, USA.
 */

#include "mainwindow.h"
#include "progressdialog.h"
#include "archiver.h"
#include "passworddialog.h"
#include "core/fr-init.h"

#include "core/config.h"

#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <stdlib.h>
#include <glib/gi18n.h>
#include <glib.h>
#include <gio/gio.h>

#include <libfm-qt/libfmqt.h>

#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>

gint          ForceDirectoryCreation;

static char** remaining_args;
static char*  add_to = NULL;
static int    add;
static char*  extract_to = NULL;
static int    extract;
static int    extract_here;
static char*  default_url = NULL;

/* argv[0] from main(); used as the command to restart the program */
static const char* program_argv0 = NULL;

static const GOptionEntry options[] = {
    {
        "add-to", 'a', 0, G_OPTION_ARG_STRING, &add_to,
        N_("Add files to the specified archive and quit the program"),
        N_("ARCHIVE")
    },

    {
        "add", 'd', 0, G_OPTION_ARG_NONE, &add,
        N_("Add files asking the name of the archive and quit the program"),
        NULL
    },

    {
        "extract-to", 'e', 0, G_OPTION_ARG_STRING, &extract_to,
        N_("Extract archives to the specified folder and quit the program"),
        N_("FOLDER")
    },

    {
        "extract", 'f', 0, G_OPTION_ARG_NONE, &extract,
        N_("Extract archives asking the destination folder and quit the program"),
        NULL
    },

    {
        "extract-here", 'h', 0, G_OPTION_ARG_NONE, &extract_here,
        N_("Extract the contents of the archives in the archive folder and quit the program"),
        NULL
    },

    {
        "default-dir", '\0', 0, G_OPTION_ARG_STRING, &default_url,
        N_("Default folder to use for the '--add' and '--extract' commands"),
        N_("FOLDER")
    },

    {
        "force", '\0', 0, G_OPTION_ARG_NONE, &ForceDirectoryCreation,
        N_("Create destination folder without asking confirmation"),
        NULL
    },

    {
        G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &remaining_args,
        NULL,
        NULL
    },

    { NULL }
};


static char* get_uri_from_command_line(const char* path) {
    GFile* file;
    char*  uri;

    file = g_file_new_for_commandline_arg(path);
    uri = g_file_get_uri(file);
    g_object_unref(file);

    return uri;
}

static int runApp(int argc, char** argv) {
    char*        extract_to_uri = NULL;
    char*        add_to_uri = NULL;

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(true);

    if(remaining_args == NULL) {  /* No archive specified. */
        auto mainWin = new MainWindow();
        mainWin->show();
        return app.exec();
    }

    if(extract_to != NULL) {
        extract_to_uri = get_uri_from_command_line(extract_to);
    }
    else {
        // if we want to extract files but dest dir is not specified, choose one
        if(extract && !extract_here) {
            if(!extract_to_uri) {
                QFileDialog dlg;
                QUrl dirUrl = QFileDialog::getExistingDirectoryUrl(nullptr, QString(),
                                                                   default_url ? QUrl() : QUrl::fromEncoded(default_url),
                                                                   (QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog));
                if(dirUrl.isEmpty()) {
                    return 1;
                }
                extract_to_uri = g_strdup(dirUrl.toEncoded().constData());
            }
        }
    }

    if(add_to != NULL) {
        add_to_uri = get_uri_from_command_line(add_to);
    }
    else {
        // if we want to add files but archive path is not specified, choose one
        if(add) {
            QFileDialog dlg;
            if(default_url) {
                dlg.setDirectoryUrl(QUrl::fromEncoded(default_url));
            }
            dlg.setNameFilters(Archiver::supportedCreateNameFilters() << QObject::tr("All files (*)"));
            dlg.setAcceptMode(QFileDialog::AcceptSave);
            if(dlg.exec() == QDialog::Accepted) {
                auto url = dlg.selectedUrls()[0];
                if(url.isEmpty()) {
                    return 1;
                }
                add_to_uri = g_strdup(url.toEncoded().constData());
            }
            else {
                return 1;
            }
        }
    }

    if((add_to != NULL) || (add == 1)) {
        /* Add files to an archive */
        const char* filename = NULL;
        int i = 0;

        Fm::FilePathList filePaths;
        while((filename = remaining_args[i++]) != NULL) {
            filePaths.emplace_back(Fm::FilePath::fromPathStr(filename));
        }

        Archiver archiver;
        ProgressDialog dlg;

        dlg.setArchiver(&archiver);
        archiver.createNewArchive(add_to_uri);

        // we can only add files after the archive is fully created
        QObject::connect(&archiver, &Archiver::finish, &dlg, [&](FrAction action, ArchiverError err) {
            if(err.hasError()) {
                QMessageBox::critical(&dlg, dlg.tr("Error"), err.message());
                dlg.reject();
                return;
            }
            switch(action) {
            case FR_ACTION_CREATING_NEW_ARCHIVE:
                archiver.addDroppedItems(filePaths, nullptr, default_url, false, nullptr, false, FR_COMPRESSION_NORMAL, 0);
                break;
            case FR_ACTION_ADDING_FILES:
                dlg.accept();
                break;
            };
        });
        dlg.exec();
        return 0;
    }
    else if((extract_to != NULL) || (extract == 1) || (extract_here == 1)) {
        const char* filename = NULL;
        int i = 0;
        /* Extract all archives. */
        while((filename = remaining_args[i++]) != NULL) {
            auto archive_uri = Fm::CStrPtr{get_uri_from_command_line(filename)};

            Archiver archiver;
            ProgressDialog dlg;

            dlg.setArchiver(&archiver);
            archiver.openArchive(archive_uri.get(), nullptr);

            // we can only start archive extraction after its content is fully loaded
            QObject::connect(&archiver, &Archiver::finish, &dlg, [&](FrAction action, ArchiverError err) {
                if(err.hasError()) {
                    QMessageBox::critical(&dlg, dlg.tr("Error"), err.message());
                    dlg.reject();
                    return;
                }
                switch(action) {
                case FR_ACTION_LISTING_CONTENT: {            /* loading the archive from a remote location */
                    std::string password_;
                    if(archiver.isEncrypted()) {
                        password_ = PasswordDialog::askPassword().toStdString();
                    }

                    if(extract_here) {
                        archiver.extractHere(false, false, false, password_.empty() ? nullptr : password_.c_str());
                    }
                    else {
                        // a target dir is specified
                        archiver.extractAll(extract_to_uri, false, false, false, password_.empty() ? nullptr : password_.c_str());
                    }
                    break;
                }
                case FR_ACTION_EXTRACTING_FILES:
                    dlg.accept();
                    break;
                default:
                    break;
                };
            });
            dlg.exec();
            return 0;
        }
    }
    else { /* Open each archive in a window */
        const char* filename = NULL;
        int i = 0;
        while((filename = remaining_args[i++]) != NULL) {
            auto mainWindow = new MainWindow();
            auto file = Fm::FilePath::fromPathStr(filename);
            mainWindow->loadFile(file);
            mainWindow->show();
        }
    }
    g_free(add_to_uri);
    g_free(extract_to_uri);
    return app.exec();
}


int main(int argc, char** argv) {
    GOptionContext* context = NULL;
    GError*         error = NULL;
    int             status;

    program_argv0 = argv[0];

    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);

    context = g_option_context_new(N_("- Create and modify an archive"));
    g_option_context_set_translation_domain(context, GETTEXT_PACKAGE);
    g_option_context_add_main_entries(context, options, GETTEXT_PACKAGE);

    if(! g_option_context_parse(context, &argc, &argv, &error)) {
        g_critical("Failed to parse arguments: %s", error->message);
        g_error_free(error);
        g_option_context_free(context);
        return EXIT_FAILURE;
    }

    g_option_context_free(context);
    g_set_application_name(_("LXQt Archiver"));

    // initialize libfm-qt
    Fm::LibFmQt libfmQt;

    // FIXME: port command line parsing to Qt
    initialize_data(); // initialize the file-roller core
    status = runApp(argc, argv);
    release_data();

    return status;
}
