/*
OBS Save Popup
Shows a small popup after every recording stops, asking for a file name and a folder.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <string>

#include "save-logic.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

bool g_dialog_open = false;

// ---------- remembered folder (stored in OBS's per-plugin config folder) ----------

std::string settings_file()
{
	char *dir = obs_module_config_path("");
	if (!dir)
		return "";
	os_mkdirs(dir);
	bfree(dir);

	char *file = obs_module_config_path("settings.json");
	if (!file)
		return "";
	std::string result(file);
	bfree(file);
	return result;
}

std::string load_last_folder()
{
	std::string f = settings_file();
	if (f.empty())
		return "";
	obs_data_t *data = obs_data_create_from_json_file(f.c_str());
	if (!data)
		return "";
	std::string folder = obs_data_get_string(data, "last_folder");
	obs_data_release(data);
	return folder;
}

void save_last_folder(const std::string &folder)
{
	std::string f = settings_file();
	if (f.empty())
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "last_folder", folder.c_str());
	if (!obs_data_save_json_safe(data, f.c_str(), "tmp", "bak"))
		obs_log(LOG_WARNING, "could not save settings to %s", f.c_str());
	obs_data_release(data);
}

// ---------- moving the file ----------

// Returns true when the file ended up where we wanted it.
bool apply(const std::string &original, const std::string &folder, const std::string &typed_name,
	   std::string &final_path, std::string &error)
{
	final_path = osp::build_target_path(folder, typed_name, original);
	if (osp::same_path(final_path, original))
		return true; // nothing to do

	// The recording was just finalized; if the file is briefly busy, retry a few times.
	for (int attempt = 0; attempt < 5; attempt++) {
		if (osp::move_file(original, final_path, error))
			return true;
		os_sleep_ms(400);
	}
	return false;
}

// ---------- the popup ----------

void show_popup(const std::string &original)
{
	QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());

	std::string last_folder = load_last_folder();
	std::string obs_folder = osp::folder_of(original);
	std::string start_folder = last_folder.empty() ? obs_folder : last_folder;

	QDialog dlg(parent);
	dlg.setWindowTitle("Save recording");
	dlg.setMinimumWidth(520);
	dlg.setWindowFlag(Qt::WindowStaysOnTopHint, true);

	auto *layout = new QVBoxLayout(&dlg);

	QFileInfo info(QString::fromUtf8(original.c_str()));
	auto *recorded = new QLabel(QString("Recording finished: <b>%1</b>").arg(info.fileName().toHtmlEscaped()));
	recorded->setTextFormat(Qt::RichText);
	layout->addWidget(recorded);

	layout->addWidget(new QLabel("File name (leave blank to keep the OBS name):"));
	auto *name_edit = new QLineEdit;
	name_edit->setPlaceholderText(info.completeBaseName());
	layout->addWidget(name_edit);

	layout->addWidget(new QLabel("Save in folder:"));
	auto *folder_row = new QHBoxLayout;
	auto *folder_edit = new QLineEdit(QDir::toNativeSeparators(QString::fromUtf8(start_folder.c_str())));
	auto *browse = new QPushButton("Browse...");
	folder_row->addWidget(folder_edit, 1);
	folder_row->addWidget(browse);
	layout->addLayout(folder_row);

	auto *buttons = new QHBoxLayout;
	buttons->addStretch(1);
	auto *save = new QPushButton("Save");
	save->setDefault(true);
	buttons->addWidget(save);
	layout->addLayout(buttons);

	QObject::connect(browse, &QPushButton::clicked, &dlg, [&]() {
		QString start = folder_edit->text();
		if (start.trimmed().isEmpty())
			start = QString::fromUtf8(start_folder.c_str());
		QString chosen = QFileDialog::getExistingDirectory(&dlg, "Choose a folder", start);
		if (!chosen.isEmpty())
			folder_edit->setText(QDir::toNativeSeparators(chosen));
	});
	QObject::connect(save, &QPushButton::clicked, &dlg, &QDialog::accept);

	name_edit->setFocus();
	int result = dlg.exec();

	std::string final_path, error;
	if (result == QDialog::Accepted) {
		std::string folder = folder_edit->text().trimmed().toUtf8().constData();
		std::string typed = name_edit->text().toUtf8().constData();
		if (folder.empty())
			folder = start_folder;

		if (apply(original, folder, typed, final_path, error)) {
			save_last_folder(folder);
			obs_log(LOG_INFO, "saved recording to: %s", final_path.c_str());
		} else {
			obs_log(LOG_ERROR, "could not move recording: %s", error.c_str());
			QMessageBox::warning(parent, "Save recording",
					     QString("The recording could not be moved. It is still at:\n%1\n\n%2")
						     .arg(QString::fromUtf8(original.c_str()),
							  QString::fromUtf8(error.c_str())));
		}
	} else {
		// Popup closed: keep the OBS name, move to the last folder used (if there is one).
		if (!last_folder.empty() && !osp::same_path(last_folder, obs_folder)) {
			if (apply(original, last_folder, "", final_path, error))
				obs_log(LOG_INFO, "popup closed; moved recording to last folder: %s",
					final_path.c_str());
			else
				obs_log(LOG_ERROR, "popup closed; could not move recording: %s", error.c_str());
		} else {
			obs_log(LOG_INFO, "popup closed; recording left where OBS saved it");
		}
	}
}

// ---------- OBS hook ----------

void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_RECORDING_STOPPED)
		return;

	char *raw = obs_frontend_get_last_recording();
	if (!raw || !*raw) {
		obs_log(LOG_WARNING, "recording stopped, but OBS did not report a file path");
		bfree(raw);
		return;
	}
	std::string path(raw);
	bfree(raw);

	// A failed recording also fires the stop event; only continue if the file really exists.
	if (!os_file_exists(path.c_str())) {
		obs_log(LOG_WARNING, "recording stopped, but the file does not exist: %s", path.c_str());
		return;
	}

	if (g_dialog_open) {
		obs_log(LOG_WARNING, "popup already open; leaving this recording where OBS saved it: %s", path.c_str());
		return;
	}

	QWidget *main_window = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!main_window)
		return;

	// Open the popup after OBS has finished handling the stop event.
	QMetaObject::invokeMethod(
		main_window,
		[path]() {
			g_dialog_open = true;
			show_popup(path);
			g_dialog_open = false;
		},
		Qt::QueuedConnection);
}

} // namespace

bool obs_module_load(void)
{
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	obs_log(LOG_INFO, "plugin unloaded");
}
