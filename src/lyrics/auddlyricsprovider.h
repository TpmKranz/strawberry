/*
 * Strawberry Music Player
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

#ifndef AUDDLYRICSPROVIDER_H
#define AUDDLYRICSPROVIDER_H

#include "config.h"

#include <QtGlobal>
#include <QObject>
#include <QList>
#include <QVariant>
#include <QString>
#include <QJsonArray>

#include "jsonlyricsprovider.h"
#include "lyricsfetcher.h"

class QNetworkReply;
class NetworkAccessManager;

class AuddLyricsProvider : public JsonLyricsProvider {
  Q_OBJECT

 public:
  explicit AuddLyricsProvider(NetworkAccessManager *network, QObject *parent = nullptr);
  ~AuddLyricsProvider() override;

  bool StartSearch(const QString &artist, const QString &album, const QString &title, quint64 id) override;
  void CancelSearch(const quint64 id) override;

 private:
  void Error(const QString &error, const QVariant &debug = QVariant()) override;
  QJsonArray ExtractResult(QNetworkReply *reply, const QString &artist, const QString &title);

 private slots:
  void HandleSearchReply(QNetworkReply *reply, const quint64 id, const QString &artist, const QString &title);

 private:
  static const char *kUrlSearch;
  static const char *kAPITokenB64;
  static const int kMaxLength;
  QList<QNetworkReply*> replies_;

};

#endif  // AUDDLYRICSPROVIDER_H
