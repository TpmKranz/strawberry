/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2012, Martin Björklund <mbj4668@gmail.com>
 * Copyright 2016-2021, Jonas Kvinge <jonas@jkvinge.net>
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
#include <algorithm>
#include <type_traits>

#include <QtGlobal>
#include <QObject>
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QSet>
#include <QMap>
#include <QQueue>
#include <QVariant>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QTimer>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include <QtDebug>

#include "core/application.h"
#include "core/logging.h"
#include "core/networkaccessmanager.h"
#include "core/utilities.h"
#include "albumcoverfetcher.h"
#include "jsoncoverprovider.h"
#include "discogscoverprovider.h"

const char *DiscogsCoverProvider::kUrlSearch = "https://api.discogs.com/database/search";
const char *DiscogsCoverProvider::kAccessKeyB64 = "dGh6ZnljUGJlZ1NEeXBuSFFxSVk=";
const char *DiscogsCoverProvider::kSecretKeyB64 = "ZkFIcmlaSER4aHhRSlF2U3d0bm5ZVmdxeXFLWUl0UXI=";
const int DiscogsCoverProvider::kRequestsDelay = 1000;

DiscogsCoverProvider::DiscogsCoverProvider(Application *app, NetworkAccessManager *network, QObject *parent)
    : JsonCoverProvider("Discogs", false, false, 0.0, false, false, app, network, parent),
      timer_flush_requests_(new QTimer(this)) {

  timer_flush_requests_->setInterval(kRequestsDelay);
  timer_flush_requests_->setSingleShot(false);
  QObject::connect(timer_flush_requests_, &QTimer::timeout, this, &DiscogsCoverProvider::FlushRequests);

}

DiscogsCoverProvider::~DiscogsCoverProvider() {

  while (!replies_.isEmpty()) {
    QNetworkReply *reply = replies_.takeFirst();
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }

  timer_flush_requests_->stop();
  queue_search_requests_.clear();
  queue_release_requests_.clear();
  requests_search_.clear();

}

bool DiscogsCoverProvider::StartSearch(const QString &artist, const QString &album, const QString &title, const int id) {

  Q_UNUSED(title);

  if (artist.isEmpty() || album.isEmpty()) return false;

  std::shared_ptr<DiscogsCoverSearchContext> search = std::make_shared<DiscogsCoverSearchContext>(id, artist, album);

  requests_search_.insert(search->id, search);
  queue_search_requests_.enqueue(search);

  if (!timer_flush_requests_->isActive()) {
    timer_flush_requests_->start();
  }

  return true;

}

void DiscogsCoverProvider::CancelSearch(const int id) {

  if (requests_search_.contains(id)) requests_search_.remove(id);

}

void DiscogsCoverProvider::FlushRequests() {

  if (!queue_release_requests_.isEmpty()) {
    SendReleaseRequest(queue_release_requests_.dequeue());
    return;
  }

  if (!queue_search_requests_.isEmpty()) {
    SendSearchRequest(queue_search_requests_.dequeue());
    return;
  }

  timer_flush_requests_->stop();

}

void DiscogsCoverProvider::SendSearchRequest(std::shared_ptr<DiscogsCoverSearchContext> search) {

  ParamList params = ParamList() << Param("format", "album")
                                 << Param("artist", search->artist.toLower())
                                 << Param("release_title", search->album.toLower());

  switch (search->type) {
    case DiscogsCoverType_Master:
      params << Param("type", "master");
      break;
    case DiscogsCoverType_Release:
      params << Param("type", "release");
      break;
  }

  QNetworkReply *reply = CreateRequest(QUrl(kUrlSearch), params);
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, search]() { HandleSearchReply(reply, search->id); });

}

QNetworkReply *DiscogsCoverProvider::CreateRequest(QUrl url, const ParamList &params_provided) {

  ParamList params = ParamList() << Param("key", QByteArray::fromBase64(kAccessKeyB64))
                                 << Param("secret", QByteArray::fromBase64(kSecretKeyB64))
                                 << params_provided;

  QUrlQuery url_query;
  QStringList query_items;

  // Encode the arguments
  typedef QPair<QByteArray, QByteArray> EncodedParam;
  for (const Param &param : params) {
    EncodedParam encoded_param(QUrl::toPercentEncoding(param.first), QUrl::toPercentEncoding(param.second));
    query_items << QString(encoded_param.first + "=" + encoded_param.second);
    url_query.addQueryItem(encoded_param.first, encoded_param.second);
  }
  url.setQuery(url_query);

  // Sign the request
  const QByteArray data_to_sign = QString("GET\n%1\n%2\n%3").arg(url.host(), url.path(), query_items.join("&")).toUtf8();
  const QByteArray signature(Utilities::HmacSha256(QByteArray::fromBase64(kSecretKeyB64), data_to_sign));

  // Add the signature to the request
  url_query.addQueryItem("Signature", QUrl::toPercentEncoding(signature.toBase64()));

  QNetworkRequest req(url);
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#else
  req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif
  QNetworkReply *reply = network_->get(req);
  replies_ << reply;

  qLog(Debug) << "Discogs: Sending request" << url;

  return reply;

}

QByteArray DiscogsCoverProvider::GetReplyData(QNetworkReply *reply) {

  QByteArray data;

  if (reply->error() == QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
    data = reply->readAll();
  }
  else {
    if (reply->error() != QNetworkReply::NoError && reply->error() < 200) {
      // This is a network error, there is nothing more to do.
      QString error = QString("%1 (%2)").arg(reply->errorString()).arg(reply->error());
      Error(error);
    }
    else {
      // See if there is Json data containing "message" - then use that instead.
      data = reply->readAll();
      QString error;
      QJsonParseError json_error;
      QJsonDocument json_doc = QJsonDocument::fromJson(data, &json_error);
      if (json_error.error == QJsonParseError::NoError && !json_doc.isEmpty() && json_doc.isObject()) {
        QJsonObject json_obj = json_doc.object();
        if (json_obj.contains("message")) {
          error = json_obj["message"].toString();
        }
      }
      if (error.isEmpty()) {
        if (reply->error() != QNetworkReply::NoError) {
          error = QString("%1 (%2)").arg(reply->errorString()).arg(reply->error());
        }
        else {
          error = QString("Received HTTP code %1").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        }
      }
      Error(error);
    }
    return QByteArray();
  }

  return data;

}

void DiscogsCoverProvider::HandleSearchReply(QNetworkReply *reply, const quint64 id) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  if (!requests_search_.contains(id)) return;
  std::shared_ptr<DiscogsCoverSearchContext> search = requests_search_.value(id);

  QByteArray data = GetReplyData(reply);
  if (data.isEmpty()) {
    EndSearch(search);
    return;
  }

  QJsonObject json_obj = ExtractJsonObj(data);
  if (json_obj.isEmpty()) {
    EndSearch(search);
    return;
  }

  QJsonValue value_results;
  if (json_obj.contains("results")) {
    value_results = json_obj["results"];
  }
  else if (json_obj.contains("message")) {
    QString message = json_obj["message"].toString();
    Error(QString("%1").arg(message));
    EndSearch(search);
    return;
  }
  else {
    Error("Json object is missing results.", json_obj);
    EndSearch(search);
    return;
  }

  if (!value_results.isArray()) {
    Error("Missing results array.", value_results);
    EndSearch(search);
    return;
  }

  QJsonArray array_results = value_results.toArray();
  for (QJsonValueRef value_result : array_results) {

    if (!value_result.isObject()) {
      Error("Invalid Json reply, results value is not a object.");
      continue;
    }
    QJsonObject obj_result = value_result.toObject();
    if (!obj_result.contains("id") || !obj_result.contains("title") || !obj_result.contains("resource_url")) {
      Error("Invalid Json reply, results value object is missing ID, title or resource_url.", obj_result);
      continue;
    }
    quint64 release_id = obj_result["id"].toInt();
    QUrl resource_url(obj_result["resource_url"].toString());
    QString title = obj_result["title"].toString();

    if (title.contains(" - ")) {
      QStringList title_splitted = title.split(" - ");
      if (title_splitted.count() == 2) {
        QString artist = title_splitted.first();
        title = title_splitted.last();
        if (artist.compare(search->artist, Qt::CaseInsensitive) != 0 && title.compare(search->album, Qt::CaseInsensitive) != 0) continue;
      }
    }

    if (!resource_url.isValid()) continue;
    if (search->requests_release_.contains(release_id)) {
      continue;
    }
    StartReleaseRequest(search, release_id, resource_url);
  }

  if (search->requests_release_.count() == 0) {
    if (search->type == DiscogsCoverType_Master) {
      search->type = DiscogsCoverType_Release;
      queue_search_requests_.enqueue(search);
    }
    else {
      EndSearch(search);
    }
  }

}

void DiscogsCoverProvider::StartReleaseRequest(std::shared_ptr<DiscogsCoverSearchContext> search, const quint64 release_id, const QUrl &url) {

  DiscogsCoverReleaseContext release(search->id, release_id, url);
  search->requests_release_.insert(release_id, release);
  queue_release_requests_.enqueue(release);

  if (!timer_flush_requests_->isActive()) {
    timer_flush_requests_->start();
  }

}

void DiscogsCoverProvider::SendReleaseRequest(const DiscogsCoverReleaseContext &release) {

  QNetworkReply *reply = CreateRequest(release.url);
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, release]() { HandleReleaseReply(reply, release.search_id, release.id); });

}

void DiscogsCoverProvider::HandleReleaseReply(QNetworkReply *reply, const quint64 search_id, const quint64 release_id) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  if (!requests_search_.contains(search_id)) return;
  std::shared_ptr<DiscogsCoverSearchContext> search = requests_search_.value(search_id);

  if (!search->requests_release_.contains(release_id)) return;
  const DiscogsCoverReleaseContext &release = search->requests_release_.value(release_id);

  QByteArray data = GetReplyData(reply);
  if (data.isEmpty()) {
    EndSearch(search, release.id);
    return;
  }

  QJsonObject json_obj = ExtractJsonObj(data);
  if (json_obj.isEmpty()) {
    EndSearch(search, release.id);
    return;
  }

  if (!json_obj.contains("artists") || !json_obj.contains("title")) {
    Error("Json reply object is missing artists or title.", json_obj);
    EndSearch(search, release.id);
    return;
  }

  if (!json_obj.contains("images")) {
    EndSearch(search, release.id);
    return;
  }

  QJsonValue value_artists = json_obj["artists"];
  if (!value_artists.isArray()) {
    Error("Json reply object artists is not a array.", value_artists);
    EndSearch(search, release.id);
    return;
  }
  QJsonArray array_artists = value_artists.toArray();
  int i = 0;
  QString artist;
  for (const QJsonValueRef value_artist : array_artists) {
    if (!value_artist.isObject()) {
      Error("Invalid Json reply, atists array value is not a object.");
      continue;
    }
    QJsonObject obj_artist = value_artist.toObject();
    if (!obj_artist.contains("name") ) {
      Error("Invalid Json reply, artists array value object is missing name.", obj_artist);
      continue;
    }
    artist = obj_artist["name"].toString();
    ++i;
    if (artist == search->artist) break;
  }

  if (artist.isEmpty()) {
    EndSearch(search, release.id);
    return;
  }
  if (i > 1 && artist != search->artist) artist = "Various artists";

  QString album = json_obj["title"].toString();
  if (artist != search->artist && album != search->album) {
    EndSearch(search, release.id);
    return;
  }

  QJsonValue value_images = json_obj["images"];
  if (!value_images.isArray()) {
    Error("Json images is not an array.");
    EndSearch(search, release.id);
    return;
  }
  QJsonArray array_images = value_images.toArray();

  if (array_images.isEmpty()) {
    Error("Invalid Json reply, images array is empty.");
    EndSearch(search, release.id);
    return;
  }

  for (const QJsonValueRef value_image : array_images) {

    if (!value_image.isObject()) {
      Error("Invalid Json reply, images array value is not an object.");
      continue;
    }
    QJsonObject obj_image = value_image.toObject();
    if (!obj_image.contains("type") || !obj_image.contains("resource_url") || !obj_image.contains("width") || !obj_image.contains("height")) {
      Error("Invalid Json reply, images array value object is missing type, resource_url, width or height.", obj_image);
      continue;
    }
    QString type = obj_image["type"].toString();
    if (type != "primary") {
      continue;
    }
    int width = obj_image["width"].toInt();
    int height = obj_image["height"].toInt();
    if (width < 300 || height < 300) continue;
    const float aspect_score = float(1.0) - float(std::max(width, height) - std::min(width, height)) / float(std::max(height, width));
    if (aspect_score < 0.85) continue;
    CoverProviderSearchResult result;
    result.artist = artist;
    result.album = album;
    result.image_url = QUrl(obj_image["resource_url"].toString());
    if (result.image_url.isEmpty()) continue;
    search->results.append(result);
  }

  emit SearchResults(search->id, search->results);
  search->results.clear();

  EndSearch(search, release.id);

}

void DiscogsCoverProvider::EndSearch(std::shared_ptr<DiscogsCoverSearchContext> search, const quint64 release_id) {

  if (search->requests_release_.contains(release_id)) {
    search->requests_release_.remove(release_id);
  }
  if (search->requests_release_.count() <= 0) {
    requests_search_.remove(search->id);
    emit SearchFinished(search->id, search->results);
  }

  if (queue_release_requests_.isEmpty() && queue_search_requests_.isEmpty()) {
    timer_flush_requests_->stop();
  }

}

void DiscogsCoverProvider::Error(const QString &error, const QVariant &debug) {

  qLog(Error) << "Discogs:" << error;
  if (debug.isValid()) qLog(Debug) << debug;

}
