/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2018-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <memory>
#include <functional>
#include <algorithm>

#include <QtGlobal>
#include <QGuiApplication>
#include <QtConcurrent>
#include <QFuture>
#include <QFutureWatcher>
#include <QAbstractItemModel>
#include <QDialog>
#include <QScreen>
#include <QWindow>
#include <QHash>
#include <QMap>
#include <QDir>
#include <QFileInfo>
#include <QVariant>
#include <QString>
#include <QStringBuilder>
#include <QStringList>
#include <QUrl>
#include <QAction>
#include <QMenu>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolButton>
#include <QFlags>
#include <QShowEvent>
#include <QCloseEvent>
#include <QSettings>

#include "core/iconloader.h"
#include "core/musicstorage.h"
#include "core/tagreaderclient.h"
#include "core/utilities.h"
#include "widgets/freespacebar.h"
#include "widgets/linetextedit.h"
#include "collection/collectionbackend.h"
#include "organize.h"
#include "organizeformat.h"
#include "organizedialog.h"
#include "organizeerrordialog.h"
#include "ui_organizedialog.h"
#ifdef HAVE_GSTREAMER
#  include "transcoder/transcoder.h"
#endif

const char *OrganizeDialog::kSettingsGroup = "OrganizeDialog";
const char *OrganizeDialog::kDefaultFormat = "%albumartist/%album{ (Disc %disc)}/{%track - }{%albumartist - }%album{ (Disc %disc)} - %title.%extension";

OrganizeDialog::OrganizeDialog(TaskManager *task_manager, CollectionBackend *backend, QWidget *parentwindow, QWidget *parent)
    : QDialog(parent),
      parentwindow_(parentwindow),
      ui_(new Ui_OrganizeDialog),
      task_manager_(task_manager),
      backend_(backend),
      total_size_(0),
      devices_(false) {

  ui_->setupUi(this);

  setWindowFlags(windowFlags()|Qt::WindowMaximizeButtonHint);

  QPushButton *button_save = ui_->button_box->addButton("Save settings", QDialogButtonBox::ApplyRole);
  QObject::connect(button_save, &QPushButton::clicked, this, &OrganizeDialog::SaveSettings);
  button_save->setIcon(IconLoader::Load("document-save"));
  ui_->button_box->button(QDialogButtonBox::RestoreDefaults)->setIcon(IconLoader::Load("edit-undo"));
  QObject::connect(ui_->button_box->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, &OrganizeDialog::RestoreDefaults);

  ui_->aftercopying->setItemIcon(1, IconLoader::Load("edit-delete"));

  // Valid tags
  QMap<QString, QString> tags;
  tags[tr("Title")] = "title";
  tags[tr("Album")] = "album";
  tags[tr("Artist")] = "artist";
  tags[tr("Artist's initial")] = "artistinitial";
  tags[tr("Album artist")] = "albumartist";
  tags[tr("Composer")] = "composer";
  tags[tr("Performer")] = "performer";
  tags[tr("Grouping")] = "grouping";
  tags[tr("Track")] = "track";
  tags[tr("Disc")] = "disc";
  tags[tr("Year")] = "year";
  tags[tr("Original year")] = "originalyear";
  tags[tr("Genre")] = "genre";
  tags[tr("Comment")] = "comment";
  tags[tr("Length")] = "length";
  tags[tr("Bitrate", "Refers to bitrate in file organize dialog.")] = "bitrate";
  tags[tr("Sample rate")] = "samplerate";
  tags[tr("Bit depth")] = "bitdepth";
  tags[tr("File extension")] = "extension";

  // Naming scheme input field
  new OrganizeFormat::SyntaxHighlighter(ui_->naming);

  QObject::connect(ui_->destination, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &OrganizeDialog::UpdatePreviews);
  QObject::connect(ui_->naming, &LineTextEdit::textChanged, this, &OrganizeDialog::UpdatePreviews);
  QObject::connect(ui_->remove_problematic, &QCheckBox::toggled, this, &OrganizeDialog::UpdatePreviews);
  QObject::connect(ui_->remove_non_fat, &QCheckBox::toggled, this, &OrganizeDialog::UpdatePreviews);
  QObject::connect(ui_->remove_non_ascii, &QCheckBox::toggled, this, &OrganizeDialog::UpdatePreviews);
  QObject::connect(ui_->allow_ascii_ext, &QCheckBox::toggled, this, &OrganizeDialog::UpdatePreviews);
  QObject::connect(ui_->replace_spaces, &QCheckBox::toggled, this, &OrganizeDialog::UpdatePreviews);
  QObject::connect(ui_->remove_non_ascii, &QCheckBox::toggled, this, &OrganizeDialog::AllowExtASCII);

  // Get the titles of the tags to put in the insert menu
  QStringList tag_titles = tags.keys();
  std::stable_sort(tag_titles.begin(), tag_titles.end());

  // Build the insert menu
  QMenu *tag_menu = new QMenu(this);
  for (const QString &title : tag_titles) {
    QAction *action = tag_menu->addAction(title);
    QString tag = tags[title];
    QObject::connect(action, &QAction::triggered, this, [this, tag]() { InsertTag(tag); });
  }

  ui_->insert->setMenu(tag_menu);

}

OrganizeDialog::~OrganizeDialog() {
  delete ui_;
}

void OrganizeDialog::SetDestinationModel(QAbstractItemModel *model, const bool devices) {

  ui_->destination->setModel(model);

  ui_->eject_after->setVisible(devices);

  devices_ = devices;

}

void OrganizeDialog::showEvent(QShowEvent*) {

  LoadGeometry();
  LoadSettings();

}

void OrganizeDialog::closeEvent(QCloseEvent*) {

  if (!devices_) SaveGeometry();

}

void OrganizeDialog::accept() {

  SaveGeometry();
  SaveSettings();

  const QModelIndex destination = ui_->destination->model()->index(ui_->destination->currentIndex(), 0);
  std::shared_ptr<MusicStorage> storage = destination.data(MusicStorage::Role_StorageForceConnect).value<std::shared_ptr<MusicStorage>>();

  if (!storage) return;

  // It deletes itself when it's finished.
  const bool copy = ui_->aftercopying->currentIndex() == 0;
  Organize *organize = new Organize(task_manager_, storage, format_, copy, ui_->overwrite->isChecked(), ui_->mark_as_listened->isChecked(), ui_->albumcover->isChecked(), new_songs_info_, ui_->eject_after->isChecked(), playlist_);
  QObject::connect(organize, &Organize::Finished, this, &OrganizeDialog::OrganizeFinished);
  QObject::connect(organize, &Organize::FileCopied, this, &OrganizeDialog::FileCopied);
  if (backend_) {
    QObject::connect(organize, &Organize::SongPathChanged, backend_, &CollectionBackend::SongPathChanged);
  }

  organize->Start();

  QDialog::accept();

}

void OrganizeDialog::reject() {

  SaveGeometry();
  QDialog::reject();

}

void OrganizeDialog::LoadGeometry() {

  if (devices_) {
    AdjustSize();
  }
  else {
    QSettings s;
    s.beginGroup(kSettingsGroup);
    if (s.contains("geometry")) {
      restoreGeometry(s.value("geometry").toByteArray());
    }
    s.endGroup();
  }

  if (parentwindow_) {
    // Center the window on the same screen as the parentwindow.
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    QScreen *screen = parentwindow_->screen();
#else
    QScreen *screen = (parentwindow_->window() && parentwindow_->window()->windowHandle() ? parentwindow_->window()->windowHandle()->screen() : nullptr);
#endif
    if (screen) {
      const QRect sr = screen->availableGeometry();
      const QRect wr({}, size().boundedTo(sr.size()));
      resize(wr.size());
      move(sr.center() - wr.center());
    }
  }

}

void OrganizeDialog::SaveGeometry() {

  if (parentwindow_) {
    QSettings s;
    s.beginGroup(kSettingsGroup);
    s.setValue("geometry", saveGeometry());
    s.endGroup();
  }

}

void OrganizeDialog::AdjustSize() {

#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
  QScreen *screen = QWidget::screen();
#else
  QScreen *screen = (window() && window()->windowHandle() ? window()->windowHandle()->screen() : QGuiApplication::primaryScreen());
#endif
  int max_width = 0;
  int max_height = 0;
  if (screen) {
    max_width = static_cast<int>(float(screen->geometry().size().width()) / float(0.5));
    max_height = static_cast<int>(float(screen->geometry().size().height()) / float(1.5));
  }

  int min_width = 0;
  int min_height = 0;
  if (ui_->preview->isVisible()) {
    int h = ui_->layout_copying->sizeHint().height() +
            ui_->button_box->sizeHint().height() +
            ui_->eject_after->sizeHint().height() +
            ui_->free_space->sizeHint().height() +
            ui_->groupbox_naming->sizeHint().height();
    if (ui_->preview->count() > 0) h += ui_->preview->sizeHintForRow(0) * ui_->preview->count();
    else h += ui_->loading_page->sizeHint().height();
    min_width = std::min(ui_->preview->sizeHintForColumn(0), max_width);
    min_height = std::min(h, max_height);
  }

  setMinimumSize(min_width, min_height);
  adjustSize();

}

void OrganizeDialog::RestoreDefaults() {

  ui_->naming->setPlainText(kDefaultFormat);
  ui_->remove_problematic->setChecked(true);
  ui_->remove_non_fat->setChecked(false);
  ui_->remove_non_ascii->setChecked(false);
  ui_->allow_ascii_ext->setChecked(false);
  ui_->replace_spaces->setChecked(true);
  ui_->overwrite->setChecked(false);
  ui_->mark_as_listened->setChecked(false);
  ui_->albumcover->setChecked(true);
  ui_->eject_after->setChecked(false);

  SaveSettings();

}

void OrganizeDialog::LoadSettings() {

  QSettings s;
  s.beginGroup(kSettingsGroup);
  ui_->naming->setPlainText(s.value("format", kDefaultFormat).toString());
  ui_->remove_problematic->setChecked(s.value("remove_problematic", true).toBool());
  ui_->remove_non_fat->setChecked(s.value("remove_non_fat", false).toBool());
  ui_->remove_non_ascii->setChecked(s.value("remove_non_ascii", false).toBool());
  ui_->allow_ascii_ext->setChecked(s.value("allow_ascii_ext", false).toBool());
  ui_->replace_spaces->setChecked(s.value("replace_spaces", true).toBool());
  ui_->overwrite->setChecked(s.value("overwrite", false).toBool());
  ui_->albumcover->setChecked(s.value("albumcover", true).toBool());
  ui_->mark_as_listened->setChecked(s.value("mark_as_listened", false).toBool());
  ui_->eject_after->setChecked(s.value("eject_after", false).toBool());

  QString destination = s.value("destination").toString();
  int index = ui_->destination->findText(destination);
  if (index != -1 && !destination.isEmpty()) {
    ui_->destination->setCurrentIndex(index);
  }

  s.endGroup();

  AllowExtASCII(ui_->remove_non_ascii->isChecked());

}

void OrganizeDialog::SaveSettings() {

  QSettings s;
  s.beginGroup(kSettingsGroup);
  s.setValue("format", ui_->naming->toPlainText());
  s.setValue("remove_problematic", ui_->remove_problematic->isChecked());
  s.setValue("remove_non_fat", ui_->remove_non_fat->isChecked());
  s.setValue("remove_non_ascii", ui_->remove_non_ascii->isChecked());
  s.setValue("allow_ascii_ext", ui_->allow_ascii_ext->isChecked());
  s.setValue("replace_spaces", ui_->replace_spaces->isChecked());
  s.setValue("overwrite", ui_->overwrite->isChecked());
  s.setValue("mark_as_listened", ui_->overwrite->isChecked());
  s.setValue("albumcover", ui_->albumcover->isChecked());
  s.setValue("destination", ui_->destination->currentText());
  s.setValue("eject_after", ui_->eject_after->isChecked());
  s.endGroup();

}

bool OrganizeDialog::SetSongs(const SongList &songs) {

  total_size_ = 0;
  songs_.clear();

  for (const Song &song : songs) {
    if (!song.url().isLocalFile()) {
      continue;
    }

    if (song.filesize() > 0) total_size_ += song.filesize();

    songs_ << song;
  }

  ui_->free_space->set_additional_bytes(total_size_);
  UpdatePreviews();
  SetLoadingSongs(false);

  if (songs_future_.isRunning()) {
    songs_future_.cancel();
  }
  songs_future_ = QFuture<SongList>();

  return !songs_.isEmpty();

}

bool OrganizeDialog::SetUrls(const QList<QUrl> &urls) {

  QStringList filenames;

  // Only add file:// URLs
  for (const QUrl &url : urls) {
    if (url.scheme() == "file") {
      filenames << url.toLocalFile();
    }
  }

  return SetFilenames(filenames);

}

bool OrganizeDialog::SetFilenames(const QStringList &filenames) {

  songs_future_ = QtConcurrent::run(&OrganizeDialog::LoadSongsBlocking, filenames);
  QFutureWatcher<SongList> *watcher = new QFutureWatcher<SongList>();
  QObject::connect(watcher, &QFutureWatcher<SongList>::finished, this, [this, watcher]() {
    SetSongs(watcher->result());
    watcher->deleteLater();
  });
  watcher->setFuture(songs_future_);

  SetLoadingSongs(true);
  return true;

}

void OrganizeDialog::SetLoadingSongs(const bool loading) {

  if (loading) {
    ui_->preview_stack->setCurrentWidget(ui_->loading_page);
    ui_->button_box->button(QDialogButtonBox::Ok)->setEnabled(false);
  }
  else {
    ui_->preview_stack->setCurrentWidget(ui_->preview_page);
    // The Ok button is enabled by UpdatePreviews
  }

}

SongList OrganizeDialog::LoadSongsBlocking(const QStringList &filenames) {

  SongList songs;
  Song song;

  QStringList filenames_copy = filenames;
  while (!filenames_copy.isEmpty()) {
    const QString filename = filenames_copy.takeFirst();

    // If it's a directory, add all the files inside.
    if (QFileInfo(filename).isDir()) {
      const QDir dir(filename);
      for (const QString &entry : dir.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable)) {
        filenames_copy << dir.filePath(entry);
      }
      continue;
    }

    TagReaderClient::Instance()->ReadFileBlocking(filename, &song);
    if (song.is_valid()) songs << song;
  }

  return songs;

}

void OrganizeDialog::SetCopy(const bool copy) {
  ui_->aftercopying->setCurrentIndex(copy ? 0 : 1);
}

void OrganizeDialog::SetPlaylist(const QString &playlist) {
  playlist_ = playlist;
}

void OrganizeDialog::InsertTag(const QString &tag) {
  ui_->naming->insertPlainText("%" + tag);
}

Organize::NewSongInfoList OrganizeDialog::ComputeNewSongsFilenames(const SongList &songs, const OrganizeFormat &format, const QString &extension) {

  // Check if we will have multiple files with the same name.
  // If so, they will erase each other if the overwrite flag is set.
  // Better to rename them: e.g. foo.bar -> foo(2).bar
  QHash<QString, int> filenames;
  Organize::NewSongInfoList new_songs_info;
  new_songs_info.reserve(songs.count());
  for (const Song &song : songs) {
    QString new_filename = format.GetFilenameForSong(song, extension);
    if (filenames.contains(new_filename)) {
      QString song_number = QString::number(++filenames[new_filename]);
      new_filename = Utilities::PathWithoutFilenameExtension(new_filename) + "(" + song_number + ")." + QFileInfo(new_filename).suffix();
    }
    filenames.insert(new_filename, 1);
    new_songs_info << Organize::NewSongInfo(song, new_filename);
  }
  return new_songs_info;

}

void OrganizeDialog::UpdatePreviews() {

  if (songs_future_.isRunning()) {
    return;
  }

  const QModelIndex destination = ui_->destination->model()->index(ui_->destination->currentIndex(), 0);
  std::shared_ptr<MusicStorage> storage;
  bool has_local_destination = false;

  if (destination.isValid()) {
    storage = destination.data(MusicStorage::Role_Storage).value<std::shared_ptr<MusicStorage>>();
    if (storage) {
      has_local_destination = !storage->LocalPath().isEmpty();
    }
  }

  // Update the free space bar
  quint64 capacity = destination.data(MusicStorage::Role_Capacity).toLongLong();
  quint64 free = destination.data(MusicStorage::Role_FreeSpace).toLongLong();

  if (capacity > 0) {
    ui_->free_space->show();
    ui_->free_space->set_free_bytes(free);
    ui_->free_space->set_total_bytes(capacity);
  }
  else {
    ui_->free_space->hide();
  }

  // Update the format object
  format_.set_format(ui_->naming->toPlainText());
  format_.set_remove_problematic(ui_->remove_problematic->isChecked());
  format_.set_remove_non_fat(ui_->remove_non_fat->isChecked());
  format_.set_remove_non_ascii(ui_->remove_non_ascii->isChecked());
  format_.set_allow_ascii_ext(ui_->allow_ascii_ext->isChecked());
  format_.set_replace_spaces(ui_->replace_spaces->isChecked());

  const bool format_valid = !has_local_destination || format_.IsValid();

  // Are we going to enable the ok button?
  bool ok = format_valid && !songs_.isEmpty();
  if (capacity != 0 && total_size_ > free) ok = false;

  ui_->button_box->button(QDialogButtonBox::Ok)->setEnabled(ok);
  if (!format_valid) return;

  QString extension;
#ifdef HAVE_GSTREAMER
  if (storage && storage->GetTranscodeMode() == MusicStorage::Transcode_Always) {
    const Song::FileType format = storage->GetTranscodeFormat();
    TranscoderPreset preset = Transcoder::PresetForFileType(format);
    extension = preset.extension_;
  }
#endif
  new_songs_info_ = ComputeNewSongsFilenames(songs_, format_, extension);

  // Update the previews
  ui_->preview->clear();
  ui_->groupbox_preview->setVisible(has_local_destination);
  ui_->groupbox_naming->setVisible(has_local_destination);
  if (has_local_destination) {
    for (const Organize::NewSongInfo &song_info : new_songs_info_) {
      QString filename = storage->LocalPath() + "/" + song_info.new_filename_;
      ui_->preview->addItem(QDir::toNativeSeparators(filename));
    }
  }

  if (devices_) {
    AdjustSize();
  }

}

void OrganizeDialog::OrganizeFinished(const QStringList &files_with_errors, const QStringList &log) {

  if (files_with_errors.isEmpty()) return;

  error_dialog_ = std::make_unique<OrganizeErrorDialog>();
  error_dialog_->Show(OrganizeErrorDialog::Type_Copy, files_with_errors, log);

}

void OrganizeDialog::AllowExtASCII(const bool checked) {
  ui_->allow_ascii_ext->setEnabled(checked);
}
