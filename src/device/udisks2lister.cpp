/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2016, Valeriy Malov <jazzvoid@gmail.com>
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
#include <utility>

#include <dbus/objectmanager.h>
#include <dbus/udisks2block.h>
#include <dbus/udisks2drive.h>
#include <dbus/udisks2filesystem.h>
#include <dbus/udisks2job.h>

#include <QtGlobal>
#include <QMutex>
#include <QList>
#include <QVariant>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QReadLocker>
#include <QWriteLocker>
#include <QDBusObjectPath>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusPendingReply>
#include <QDBusArgument>
#include <QJsonArray>
#include <QJsonObject>
#include <QtDebug>

#include "core/logging.h"
#include "core/utilities.h"

#include "udisks2lister.h"

constexpr char Udisks2Lister::udisks2_service_[];

Udisks2Lister::Udisks2Lister(QObject *parent) : DeviceLister(parent) {}

Udisks2Lister::~Udisks2Lister() = default;

QStringList Udisks2Lister::DeviceUniqueIDs() {
  QReadLocker locker(&device_data_lock_);
  return device_data_.keys();
}

QVariantList Udisks2Lister::DeviceIcons(const QString &id) {

  QReadLocker locker(&device_data_lock_);
  if (!device_data_.contains(id)) return QVariantList();
  QString path = device_data_[id].mount_paths.at(0);

  return QVariantList() << GuessIconForPath(path) << GuessIconForModel(DeviceManufacturer(id), DeviceModel(id));

}

QString Udisks2Lister::DeviceManufacturer(const QString &id) {

  QReadLocker locker(&device_data_lock_);
  if (!device_data_.contains(id)) return "";
  return device_data_[id].vendor;

}

QString Udisks2Lister::DeviceModel(const QString &id) {

  QReadLocker locker(&device_data_lock_);
  if (!device_data_.contains(id)) return "";
  return device_data_[id].model;

}

quint64 Udisks2Lister::DeviceCapacity(const QString &id) {

  QReadLocker locker(&device_data_lock_);
  if (!device_data_.contains(id)) return 0;
  return device_data_[id].capacity;

}

quint64 Udisks2Lister::DeviceFreeSpace(const QString &id) {

  QReadLocker locker(&device_data_lock_);
  if (!device_data_.contains(id)) return 0;
  return device_data_[id].free_space;

}

QVariantMap Udisks2Lister::DeviceHardwareInfo(const QString &id) {

  QReadLocker locker(&device_data_lock_);
  if (!device_data_.contains(id)) return QVariantMap();

  QVariantMap result;

  const PartitionData &data = device_data_[id];
  result[QT_TR_NOOP("D-Bus path")] = data.dbus_path;
  result[QT_TR_NOOP("Serial number")] = data.serial;
  result[QT_TR_NOOP("Mount points")] = data.mount_paths.join(", ");
  result[QT_TR_NOOP("Partition label")] = data.label;
  result[QT_TR_NOOP("UUID")] = data.uuid;

  return result;

}

QString Udisks2Lister::MakeFriendlyName(const QString &id) {

  QReadLocker locker(&device_data_lock_);
  if (!device_data_.contains(id)) return "";
  return device_data_[id].friendly_name;

}

QList<QUrl> Udisks2Lister::MakeDeviceUrls(const QString &id) {

  QReadLocker locker(&device_data_lock_);
  QList<QUrl> ret;
  if (!device_data_.contains(id)) return ret;
  ret << MakeUrlFromLocalPath(device_data_[id].mount_paths.at(0));
  return ret;

}

void Udisks2Lister::UnmountDevice(const QString &id) {

  QReadLocker locker(&device_data_lock_);
  if (!device_data_.contains(id)) return;

  OrgFreedesktopUDisks2FilesystemInterface filesystem(udisks2_service_, device_data_[id].dbus_path, QDBusConnection::systemBus());

  if (filesystem.isValid()) {
    auto unmount_result = filesystem.Unmount(QVariantMap());
    unmount_result.waitForFinished();

    if (unmount_result.isError()) {
      qLog(Warning) << "Failed to unmount " << id << ": " << unmount_result.error();
      return;
    }

    OrgFreedesktopUDisks2DriveInterface drive(udisks2_service_, device_data_[id].dbus_drive_path, QDBusConnection::systemBus());

    if (drive.isValid()) {
      auto eject_result = drive.Eject(QVariantMap());
      eject_result.waitForFinished();

      if (eject_result.isError()) {
        qLog(Warning) << "Failed to eject " << id << ": " << eject_result.error();
      }
    }

    device_data_.remove(id);
    emit DeviceRemoved(id);
  }

}

void Udisks2Lister::UpdateDeviceFreeSpace(const QString &id) {
  QWriteLocker locker(&device_data_lock_);
  device_data_[id].free_space = Utilities::FileSystemFreeSpace(device_data_[id].mount_paths.at(0));
  emit DeviceChanged(id);
}

bool Udisks2Lister::Init() {

  udisks2_interface_ = std::make_unique<OrgFreedesktopDBusObjectManagerInterface>(udisks2_service_, "/org/freedesktop/UDisks2", QDBusConnection::systemBus());

  QDBusPendingReply<ManagedObjectList> reply = udisks2_interface_->GetManagedObjects();
  reply.waitForFinished();

  if (!reply.isValid()) {
    qLog(Warning) << "Error enumerating udisks2 devices:" << reply.error().name() << reply.error().message();
    udisks2_interface_.reset();
    return false;
  }

  QList<QDBusObjectPath> paths = reply.value().keys();
  for (const QDBusObjectPath &path : paths) {
    PartitionData partition_data = ReadPartitionData(path);

    if (!partition_data.dbus_path.isEmpty()) {
      QWriteLocker locker(&device_data_lock_);
      device_data_[partition_data.unique_id()] = partition_data;
    }
  }

  QStringList ids = device_data_.keys();
  for (const QString &id : ids) {
    emit DeviceAdded(id);
  }

  QObject::connect(udisks2_interface_.get(), &OrgFreedesktopDBusObjectManagerInterface::InterfacesAdded, this, &Udisks2Lister::DBusInterfaceAdded);
  QObject::connect(udisks2_interface_.get(), &OrgFreedesktopDBusObjectManagerInterface::InterfacesRemoved, this, &Udisks2Lister::DBusInterfaceRemoved);

  return true;

}

void Udisks2Lister::DBusInterfaceAdded(const QDBusObjectPath &path, const InterfacesAndProperties &interfaces) {

  for (auto interface = interfaces.constBegin(); interface != interfaces.constEnd(); ++interface) {

    if (interface.key() != "org.freedesktop.UDisks2.Job") continue;

    std::shared_ptr<OrgFreedesktopUDisks2JobInterface> job = std::make_shared<OrgFreedesktopUDisks2JobInterface>(udisks2_service_, path.path(), QDBusConnection::systemBus());

    if (!job->isValid()) continue;

    bool is_mount_job = false;
    if (job->operation() == "filesystem-mount") {
      is_mount_job = true;
    }
    else if (job->operation() == "filesystem-unmount") {
      is_mount_job = false;
    }
    else {
      continue;
    }

    auto mounted_partitions = job->objects();

    if (mounted_partitions.isEmpty()) {
      qLog(Warning) << "Empty Udisks2 mount/umount job " << path.path();
      continue;
    }

    {
      QMutexLocker locker(&jobs_lock_);
      qLog(Debug) << "Adding pending job | DBus Path = " << job->path() << " | IsMountJob = " << is_mount_job << " | First partition = " << mounted_partitions.at(0).path();
      mounting_jobs_[path].dbus_interface = job;
      mounting_jobs_[path].is_mount = is_mount_job;
      mounting_jobs_[path].mounted_partitions = mounted_partitions;
      QObject::connect(job.get(), &OrgFreedesktopUDisks2JobInterface::Completed, this, &Udisks2Lister::JobCompleted);
    }
  }
}

void Udisks2Lister::DBusInterfaceRemoved(const QDBusObjectPath &path, const QStringList &interfaces) {
  Q_UNUSED(interfaces);
  if (!isPendingJob(path)) RemoveDevice(path);
}

bool Udisks2Lister::isPendingJob(const QDBusObjectPath &job_path) {

  QMutexLocker locker(&jobs_lock_);

  if (!mounting_jobs_.contains(job_path)) return false;

  mounting_jobs_.remove(job_path);
  return true;

}

void Udisks2Lister::RemoveDevice(const QDBusObjectPath &device_path) {

  QWriteLocker locker(&device_data_lock_);
  QString id;
  for (const PartitionData &data : std::as_const(device_data_)) {
    if (data.dbus_path == device_path.path()) {
      id = data.unique_id();
      break;
    }
  }

  if (id.isEmpty()) return;

  qLog(Debug) << "UDisks2 device removed: " << device_path.path();
  device_data_.remove(id);

  emit DeviceRemoved(id);

}

QList<QDBusObjectPath> Udisks2Lister::GetMountedPartitionsFromDBusArgument(const QDBusArgument &input) {

  QList<QDBusObjectPath> result;

  input.beginArray();
  while (!input.atEnd()) {
    QDBusObjectPath extractedPath;
    input >> extractedPath;
    result.push_back(extractedPath);
  }
  input.endArray();

  return result;

}

void Udisks2Lister::JobCompleted(const bool success, const QString &message) {

  Q_UNUSED(message);

  OrgFreedesktopUDisks2JobInterface *job = qobject_cast<OrgFreedesktopUDisks2JobInterface*>(sender());
  QDBusObjectPath jobPath(job->path());

  if (!job->isValid() || !success || !mounting_jobs_.contains(jobPath)) {
    return;
  }

  qLog(Debug) << "Pending Job Completed | Path = " << job->path() << " | Mount? = " << mounting_jobs_[jobPath].is_mount << " | Success = " << success;

  for (const auto &mounted_object : mounting_jobs_[jobPath].mounted_partitions) {
    auto partition_data = ReadPartitionData(mounted_object);
    if (partition_data.dbus_path.isEmpty()) continue;

    mounting_jobs_[jobPath].is_mount ? HandleFinishedMountJob(partition_data) : HandleFinishedUnmountJob(partition_data, mounted_object);
  }

}

void Udisks2Lister::HandleFinishedMountJob(const PartitionData &partition_data) {

  qLog(Debug) << "UDisks2 mount job finished: Drive = " << partition_data.dbus_drive_path << " | Partition = " << partition_data.dbus_path;
  QWriteLocker locker(&device_data_lock_);
  device_data_[partition_data.unique_id()] = partition_data;

  emit DeviceAdded(partition_data.unique_id());

}

void Udisks2Lister::HandleFinishedUnmountJob(const PartitionData &partition_data, const QDBusObjectPath &mounted_object) {

  QWriteLocker locker(&device_data_lock_);
  QString id;
  for (PartitionData &data : device_data_) {
    if (data.mount_paths.contains(mounted_object.path())) {
      qLog(Debug) << "UDisks2 umount job finished, found corresponding device: Drive = " << data.dbus_drive_path << " | Partition = " << data.dbus_path;
      data.mount_paths.removeOne(mounted_object.path());
      if (data.mount_paths.empty()) id = data.unique_id();
      break;
    }
  }

  if (!id.isEmpty()) {
    qLog(Debug) << "Partition " << partition_data.dbus_path << " has no more mount points, removing it from device list";
    device_data_.remove(id);
    emit DeviceRemoved(id);
  }

}

Udisks2Lister::PartitionData Udisks2Lister::ReadPartitionData(const QDBusObjectPath &path) {

  PartitionData result;
  OrgFreedesktopUDisks2FilesystemInterface filesystem(udisks2_service_, path.path(), QDBusConnection::systemBus());
  OrgFreedesktopUDisks2BlockInterface block(udisks2_service_, path.path(), QDBusConnection::systemBus());

  if (filesystem.isValid() && block.isValid() && !filesystem.mountPoints().empty()) {
    OrgFreedesktopUDisks2DriveInterface drive(udisks2_service_, block.drive().path(), QDBusConnection::systemBus());

    if (drive.isValid() && drive.mediaRemovable()) {
      result.dbus_path = path.path();
      result.dbus_drive_path = block.drive().path();

      result.serial = drive.serial();
      result.vendor = drive.vendor();
      result.model = drive.model();

      result.label = block.idLabel();
      result.uuid = block.idUUID();
      result.capacity = drive.size();

      if (result.label.isEmpty()) {
        result.friendly_name = result.model + " " + result.uuid;
      }
      else {
        result.friendly_name = result.label;
      }

      for (const QByteArray &p : filesystem.mountPoints()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)  // Workaround a bytearray to string conversion issue with Qt 6
        QString mountpoint = QByteArray(p.data(), strlen(p.data()));
#else
        QString mountpoint = p;
#endif
        result.mount_paths.push_back(mountpoint);
      }

      result.free_space = Utilities::FileSystemFreeSpace(result.mount_paths.at(0));
    }
  }

  return result;

}

QString Udisks2Lister::PartitionData::unique_id() const {
  return QString("Udisks2/%1/%2/%3/%4/%5")
      .arg(serial, vendor, model)
      .arg(capacity)
      .arg(uuid);
}

Udisks2Lister::Udisks2Job::Udisks2Job() : is_mount(true) {}

Udisks2Lister::PartitionData::PartitionData() : capacity(0), free_space(0) {}
