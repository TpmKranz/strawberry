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

#include <cmath>

#include <QtGlobal>
#include <QApplication>
#include <QObject>
#include <QWidget>
#include <QThread>
#include <QtConcurrentRun>
#include <QFuture>
#include <QFutureWatcher>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QCompleter>
#include <QDateTime>
#include <QDir>
#include <QFont>
#include <QFontMetrics>
#include <QHeaderView>
#include <QLocale>
#include <QMetaType>
#include <QVariant>
#include <QString>
#include <QStringBuilder>
#include <QUrl>
#include <QIcon>
#include <QPixmap>
#include <QPixmapCache>
#include <QPainter>
#include <QColor>
#include <QPen>
#include <QBrush>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QLineEdit>
#include <QScrollBar>
#include <QToolTip>
#include <QTreeView>
#include <QWhatsThis>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QtDebug>
#include <QtEvents>
#include <QLinearGradient>

#include "core/iconloader.h"
#include "core/logging.h"
#include "core/player.h"
#include "core/song.h"
#include "core/urlhandler.h"
#include "core/utilities.h"
#include "collection/collectionbackend.h"
#include "playlist/playlist.h"
#include "playlistdelegates.h"

const int QueuedItemDelegate::kQueueBoxBorder = 1;
const int QueuedItemDelegate::kQueueBoxCornerRadius = 3;
const int QueuedItemDelegate::kQueueBoxLength = 30;
const QRgb QueuedItemDelegate::kQueueBoxGradientColor1 = qRgb(102, 150, 227);
const QRgb QueuedItemDelegate::kQueueBoxGradientColor2 = qRgb(77, 121, 200);
const int QueuedItemDelegate::kQueueOpacitySteps = 10;
const float QueuedItemDelegate::kQueueOpacityLowerBound = 0.4;

const int PlaylistDelegateBase::kMinHeight = 19;

QueuedItemDelegate::QueuedItemDelegate(QObject *parent, int indicator_column)
    : QStyledItemDelegate(parent),
      indicator_column_(indicator_column) {}

void QueuedItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &idx) const {

  QStyledItemDelegate::paint(painter, option, idx);

  if (idx.column() == indicator_column_) {
    bool ok = false;
    const int queue_pos = idx.data(Playlist::Role_QueuePosition).toInt(&ok);
    if (ok && queue_pos != -1) {
      float opacity = static_cast<float>(kQueueOpacitySteps - qMin(kQueueOpacitySteps, queue_pos));
      opacity /= kQueueOpacitySteps;
      opacity *= float(1.0) - float(kQueueOpacityLowerBound);
      opacity += kQueueOpacityLowerBound;
      DrawBox(painter, option.rect, option.font, QString::number(queue_pos + 1), kQueueBoxLength, opacity);
    }
  }

}

void QueuedItemDelegate::DrawBox(QPainter *painter, const QRect line_rect, const QFont &font, const QString &text, int width, const float opacity) {

  QFont smaller = font;
  smaller.setPointSize(smaller.pointSize() - 1);
  smaller.setBold(true);

  if (width == -1) {
#if (QT_VERSION >= QT_VERSION_CHECK(5, 11, 0))
    width = QFontMetrics(font).horizontalAdvance(text + "  ");
#else
    width = QFontMetrics(font).width(text + "  ");
#endif
  }

  QRect rect(line_rect);
  rect.setLeft(rect.right() - width - kQueueBoxBorder);
  rect.setWidth(width);
  rect.setTop(rect.top() + kQueueBoxBorder);
  rect.setBottom(rect.bottom() - kQueueBoxBorder - 1);

  QRect text_rect(rect);
  text_rect.setBottom(text_rect.bottom() + 1);

  QLinearGradient gradient(rect.topLeft(), rect.bottomLeft());
  gradient.setColorAt(0.0, kQueueBoxGradientColor1);
  gradient.setColorAt(1.0, kQueueBoxGradientColor2);

  painter->save();

  painter->setOpacity(opacity);

  // Turn on antialiasing
  painter->setRenderHint(QPainter::Antialiasing);

  // Draw the box
  painter->translate(0.5, 0.5);
  painter->setPen(QPen(Qt::white, 1));
  painter->setBrush(gradient);
  painter->drawRoundedRect(rect, kQueueBoxCornerRadius, kQueueBoxCornerRadius);

  // Draw the text
  painter->setFont(smaller);
  painter->drawText(rect, Qt::AlignCenter, text);
  painter->translate(-0.5, -0.5);

  painter->restore();

}

int QueuedItemDelegate::queue_indicator_size(const QModelIndex &idx) const {

  if (idx.column() == indicator_column_) {
    const int queue_pos = idx.data(Playlist::Role_QueuePosition).toInt();
    if (queue_pos != -1) {
      return kQueueBoxLength + kQueueBoxBorder * 2;
    }
  }
  return 0;

}


PlaylistDelegateBase::PlaylistDelegateBase(QObject *parent, const QString &suffix)
    : QueuedItemDelegate(parent), view_(qobject_cast<QTreeView*>(parent)), suffix_(suffix)
{
}

QString PlaylistDelegateBase::displayText(const QVariant &value, const QLocale&) const {

  QString text;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  switch (value.metaType().id()) {
#else
  switch (static_cast<QMetaType::Type>(value.type())) {
#endif
    case QMetaType::Int: {
      int v = value.toInt();
      if (v > 0) text = QString::number(v);
      break;
    }

    case QMetaType::Float:
    case QMetaType::Double: {
      double v = value.toDouble();
      if (v > 0) text = QString::number(v);
      break;
    }

    default:
      text = value.toString();
      break;
  }

  if (!text.isNull() && !suffix_.isNull()) text += " " + suffix_;
  return text;

}

QSize PlaylistDelegateBase::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &idx) const {

  QSize size = QueuedItemDelegate::sizeHint(option, idx);
  if (size.height() < kMinHeight) size.setHeight(kMinHeight);
  return size;

}

void PlaylistDelegateBase::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &idx) const {

  QueuedItemDelegate::paint(painter, Adjusted(option, idx), idx);

  // Stop after indicator
  if (idx.column() == Playlist::Column_Title) {
    if (idx.data(Playlist::Role_StopAfter).toBool()) {
      QRect rect(option.rect);
      rect.setRight(rect.right() - queue_indicator_size(idx));

      DrawBox(painter, rect, option.font, tr("stop"));
    }
  }

}

QStyleOptionViewItem PlaylistDelegateBase::Adjusted(const QStyleOptionViewItem &option, const QModelIndex &idx) const {

  if (!view_) return option;

  QPoint top_left(-view_->horizontalScrollBar()->value(), -view_->verticalScrollBar()->value());

  if (view_->header()->logicalIndexAt(top_left) != idx.column()) {
    return option;
  }

  QStyleOptionViewItem ret(option);

  if (idx.data(Playlist::Role_IsCurrent).toBool()) {
    // Move the text in a bit on the first column for the song that's currently playing
    ret.rect.setLeft(ret.rect.left() + 20);
  }

  return ret;

}

bool PlaylistDelegateBase::helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option, const QModelIndex &idx) {

  // This function is copied from QAbstractItemDelegate, and changed to show displayText() in the tooltip, rather than the index's naked Qt::ToolTipRole text.

  Q_UNUSED(option);

  if (!event || !view) return false;

  QString text = displayText(idx.data(), QLocale::system());

  // Special case: we want newlines in the comment tooltip
  if (idx.column() == Playlist::Column_Comment) {
    text = idx.data(Qt::ToolTipRole).toString().toHtmlEscaped();
    text.replace("\\r\\n", "<br />");
    text.replace("\\n", "<br />");
    text.replace("\r\n", "<br />");
    text.replace("\n", "<br />");
  }

  if (text.isEmpty() || !event) return false;

  switch (event->type()) {
    case QEvent::ToolTip: {
      QSize real_text = sizeHint(option, idx);
      QRect displayed_text = view->visualRect(idx);
      bool is_elided = displayed_text.width() < real_text.width();
      if (is_elided) {
        QToolTip::showText(event->globalPos(), text, view);
      }
      else {  // in case that another text was previously displayed
        QToolTip::hideText();
      }
      return true;
    }

    case QEvent::QueryWhatsThis:
      return true;

    case QEvent::WhatsThis:
      QWhatsThis::showText(event->globalPos(), text, view);
      return true;

    default:
      break;
  }
  return false;

}


QString LengthItemDelegate::displayText(const QVariant &value, const QLocale&) const {

  bool ok = false;
  qint64 nanoseconds = value.toLongLong(&ok);

  if (ok && nanoseconds > 0) return Utilities::PrettyTimeNanosec(nanoseconds);
  return QString();

}


QString SizeItemDelegate::displayText(const QVariant &value, const QLocale&) const {

  bool ok = false;
  int bytes = value.toInt(&ok);

  if (ok) return Utilities::PrettySize(bytes);
  return QString();

}

QString DateItemDelegate::displayText(const QVariant &value, const QLocale &locale) const {

  Q_UNUSED(locale);

  bool ok = false;
  qint64 time = value.toLongLong(&ok);

  if (!ok || time == -1) {
    return QString();
  }

  return QDateTime::fromSecsSinceEpoch(time).toString(QLocale::system().dateTimeFormat(QLocale::ShortFormat));

}

QString LastPlayedItemDelegate::displayText(const QVariant &value, const QLocale &locale) const {

  bool ok = false;
  const qint64 time = value.toLongLong(&ok);

  if (!ok || time == -1) {
    return tr("Never");
  }

  return Utilities::Ago(time, locale);

}

QString FileTypeItemDelegate::displayText(const QVariant &value, const QLocale &locale) const {

  Q_UNUSED(locale);

  bool ok = false;
  Song::FileType type = Song::FileType(value.toInt(&ok));

  if (!ok) return tr("Unknown");

  return Song::TextForFiletype(type);

}

QWidget *TextItemDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &idx) const {
  Q_UNUSED(option);
  Q_UNUSED(idx);
  return new QLineEdit(parent);
}

TagCompletionModel::TagCompletionModel(CollectionBackend *backend, const Playlist::Column column, QObject *parent) : QStringListModel(parent) {

  QString col = database_column(column);
  if (!col.isEmpty()) {
    setStringList(backend->GetAll(col));
  }

  if (QThread::currentThread() != backend->thread() && QThread::currentThread() != qApp->thread()) {
    backend->Close();
  }

}

QString TagCompletionModel::database_column(Playlist::Column column) {

  switch (column) {
    case Playlist::Column_Artist:       return "artist";
    case Playlist::Column_Album:        return "album";
    case Playlist::Column_AlbumArtist:  return "albumartist";
    case Playlist::Column_Composer:     return "composer";
    case Playlist::Column_Performer:    return "performer";
    case Playlist::Column_Grouping:     return "grouping";
    case Playlist::Column_Genre:        return "genre";
    default:
      qLog(Warning) << "Unknown column" << column;
      return QString();
  }

}

static TagCompletionModel *InitCompletionModel(CollectionBackend *backend, Playlist::Column column) {

  return new TagCompletionModel(backend, column);

}

TagCompleter::TagCompleter(CollectionBackend *backend, Playlist::Column column, QLineEdit *editor) : QCompleter(editor), editor_(editor) {

  QFuture<TagCompletionModel*> future = QtConcurrent::run(&InitCompletionModel, backend, column);
  QFutureWatcher<TagCompletionModel*> *watcher = new QFutureWatcher<TagCompletionModel*>();
  QObject::connect(watcher, &QFutureWatcher<TagCompletionModel*>::finished, this, &TagCompleter::ModelReady);
  watcher->setFuture(future);

}

TagCompleter::~TagCompleter() {
  delete model();
}

void TagCompleter::ModelReady() {

  QFutureWatcher<TagCompletionModel*> *watcher = static_cast<QFutureWatcher<TagCompletionModel*>*>(sender());
  TagCompletionModel *model = watcher->result();
  watcher->deleteLater();
  setModel(model);
  setCaseSensitivity(Qt::CaseInsensitive);
  editor_->setCompleter(this);

}

QWidget *TagCompletionItemDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem&, const QModelIndex&) const {

  QLineEdit *editor = new QLineEdit(parent);
  new TagCompleter(backend_, column_, editor);

  return editor;

}

QString NativeSeparatorsDelegate::displayText(const QVariant &value, const QLocale&) const {

  const QString string_value = value.toString();

  QUrl url;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  if (value.metaType().id() == QMetaType::QUrl) {
#else
  if (value.type() == QVariant::Url) {
#endif
    url = value.toUrl();
  }
  else if (string_value.contains("://")) {
    url = QUrl::fromEncoded(string_value.toLatin1());
  }
  else {
    return QDir::toNativeSeparators(string_value);
  }

  if (url.isLocalFile()) {
    return QDir::toNativeSeparators(url.toLocalFile());
  }
  return string_value;

}

SongSourceDelegate::SongSourceDelegate(QObject *parent) : PlaylistDelegateBase(parent) {}

QString SongSourceDelegate::displayText(const QVariant &value, const QLocale&) const {
  Q_UNUSED(value);
  return QString();
}

QPixmap SongSourceDelegate::LookupPixmap(const Song::Source source, const QSize size) const {

  QPixmap pixmap;
  QString cache_key = QString("%1-%2x%3").arg(Song::TextForSource(source)).arg(size.width()).arg(size.height());
  if (QPixmapCache::find(cache_key, &pixmap)) {
    return pixmap;
  }

  QIcon icon(Song::IconForSource(source));
  pixmap = icon.pixmap(size.height());
  QPixmapCache::insert(cache_key, pixmap);

  return pixmap;

}

void SongSourceDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &idx) const {

  // Draw the background
  PlaylistDelegateBase::paint(painter, option, idx);

  QStyleOptionViewItem option_copy(option);
  initStyleOption(&option_copy, idx);

  const Song::Source source = Song::Source(idx.data().toInt());
  QPixmap pixmap = LookupPixmap(source, option_copy.decorationSize);

  QWidget *parent_widget = qobject_cast<QWidget*>(parent());
  int device_pixel_ratio = parent_widget->devicePixelRatio();

  // Draw the pixmap in the middle of the rectangle
  QRect draw_rect(QPoint(0, 0), option_copy.decorationSize / device_pixel_ratio);
  draw_rect.moveCenter(option_copy.rect.center());

  painter->drawPixmap(draw_rect, pixmap);

}

RatingItemDelegate::RatingItemDelegate(QObject *parent) : PlaylistDelegateBase(parent) {}

void RatingItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &idx) const {

  // Draw the background
  option.widget->style()->drawPrimitive(QStyle::PE_PanelItemViewItem, &option, painter, option.widget);

  // Don't draw anything else if the user can't set the rating of this item
  if (!idx.data(Playlist::Role_CanSetRating).toBool()) return;

  const bool hover = mouse_over_index_.isValid() && (mouse_over_index_ == idx || (selected_indexes_.contains(mouse_over_index_) && selected_indexes_.contains(idx)));

  const double rating = (hover ? RatingPainter::RatingForPos(mouse_over_pos_, option.rect) : idx.data().toDouble());

  painter_.Paint(painter, option.rect, rating);

}

QSize RatingItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &idx) const {

  QSize size = PlaylistDelegateBase::sizeHint(option, idx);
  size.setWidth(size.height() * RatingPainter::kStarCount);
  return size;

}

QString RatingItemDelegate::displayText(const QVariant &value, const QLocale&) const {

  if (value.isNull() || value.toDouble() <= 0) return QString();

  // Round to the nearest 0.5
  const double rating = double(lround(value.toDouble() * RatingPainter::kStarCount * 2)) / 2;

  return QString::number(rating, 'f', 1);

}
