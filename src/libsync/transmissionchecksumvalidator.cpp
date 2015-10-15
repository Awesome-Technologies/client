/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */
#include "config.h"
#include "filesystem.h"
#include "transmissionchecksumvalidator.h"
#include "syncfileitem.h"
#include "propagatorjobs.h"
#include "account.h"

#include <qtconcurrentrun.h>

namespace OCC {

QByteArray makeChecksumHeader(const QByteArray& checksumType, const QByteArray& checksum)
{
    QByteArray header = checksumType;
    header.append(':');
    header.append(checksum);
    return header;
}

bool parseChecksumHeader(const QByteArray& header, QByteArray* type, QByteArray* checksum)
{
    if (header.isEmpty()) {
        type->clear();
        checksum->clear();
        return true;
    }

    const auto idx = header.indexOf(':');
    if (idx < 0) {
        return false;
    }

    *type = header.left(idx);
    *checksum = header.mid(idx + 1);
    return true;
}

bool uploadChecksumEnabled()
{
    static bool enabled = qgetenv("OWNCLOUD_DISABLE_CHECKSUM_UPLOAD").isEmpty();
    return enabled;
}

bool downloadChecksumEnabled()
{
    static bool enabled = qgetenv("OWNCLOUD_DISABLE_CHECKSUM_DOWNLOAD").isEmpty();
    return enabled;
}

ComputeChecksum::ComputeChecksum(QObject* parent)
    : QObject(parent)
{
}

void ComputeChecksum::setChecksumType(const QByteArray& type)
{
    _checksumType = type;
}

QByteArray ComputeChecksum::checksumType() const
{
    return _checksumType;
}

void ComputeChecksum::start(const QString& filePath)
{
    const QString csType = checksumType();

    // Calculate the checksum in a different thread first.
    connect( &_watcher, SIGNAL(finished()),
             this, SLOT(slotCalculationDone()),
             Qt::UniqueConnection );
    if( csType == checkSumMD5C ) {
        _watcher.setFuture(QtConcurrent::run(FileSystem::calcMd5, filePath));

    } else if( csType == checkSumSHA1C ) {
        _watcher.setFuture(QtConcurrent::run( FileSystem::calcSha1, filePath));
    }
#ifdef ZLIB_FOUND
    else if( csType == checkSumAdlerC) {
        _watcher.setFuture(QtConcurrent::run(FileSystem::calcAdler32, filePath));
    }
#endif
    else {
        // for an unknown checksum or no checksum, we're done right now
        if( !csType.isEmpty() ) {
            qDebug() << "Unknown checksum type:" << csType;
        }
        emit done(QByteArray(), QByteArray());
    }
}

void ComputeChecksum::slotCalculationDone()
{
    QByteArray checksum = _watcher.future().result();
    emit done(_checksumType, checksum);
}


ValidateChecksumHeader::ValidateChecksumHeader(QObject *parent)
    : QObject(parent)
{
}

void ValidateChecksumHeader::start(const QString& filePath, const QByteArray& checksumHeader)
{
    // If the incoming header is empty no validation can happen. Just continue.
    if( checksumHeader.isEmpty() ) {
        emit validated(QByteArray());
        return;
    }

    if( !parseChecksumHeader(checksumHeader, &_expectedChecksumType, &_expectedChecksum) ) {
        qDebug() << "Checksum header malformed:" << checksumHeader;
        emit validationFailed(tr("The checksum header is malformed."));
        return;
    }

    auto calculator = new ComputeChecksum(this);
    calculator->setChecksumType(_expectedChecksumType);
    connect(calculator, SIGNAL(done(QByteArray,QByteArray)),
            SLOT(slotChecksumCalculated(QByteArray,QByteArray)));
    calculator->start(filePath);
}

void ValidateChecksumHeader::slotChecksumCalculated(const QByteArray& checksumType,
                                                    const QByteArray& checksum)
{
    if( checksumType != _expectedChecksumType ) {
        emit validationFailed(tr("The checksum header contained an unknown checksum type '%1'").arg(
                                  QString::fromLatin1(_expectedChecksumType)));
        return;
    }
    if( checksum != _expectedChecksum ) {
        emit validationFailed(tr("The downloaded file does not match the checksum, it will be resumed."));
        return;
    }
    emit validated(makeChecksumHeader(checksumType, checksum));
}

}
