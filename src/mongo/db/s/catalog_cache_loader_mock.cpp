/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/catalog_cache_loader_mock.h"

#include "mongo/db/operation_context.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/stdx/thread.h"

namespace mongo {

using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;

CatalogCacheLoaderMock::CatalogCacheLoaderMock(std::shared_ptr<ThreadPool> executor)
    : _executor(executor) {}

void CatalogCacheLoaderMock::initializeReplicaSetRole(bool isPrimary) {
    MONGO_UNREACHABLE;
}

void CatalogCacheLoaderMock::onStepDown() {
    MONGO_UNREACHABLE;
}

void CatalogCacheLoaderMock::onStepUp() {
    MONGO_UNREACHABLE;
}

void CatalogCacheLoaderMock::shutDown() {}

void CatalogCacheLoaderMock::notifyOfCollectionVersionUpdate(const NamespaceString& nss) {
    MONGO_UNREACHABLE;
}

void CatalogCacheLoaderMock::waitForCollectionFlush(OperationContext* opCtx,
                                                    const NamespaceString& nss) {
    MONGO_UNREACHABLE;
}

void CatalogCacheLoaderMock::waitForDatabaseFlush(OperationContext* opCtx, StringData dbName) {
    MONGO_UNREACHABLE;
}

SemiFuture<CollectionAndChangedChunks> CatalogCacheLoaderMock::getChunksSince(
    const NamespaceString& nss, ChunkVersion version) {

    return ExecutorFuture<void>(_executor)
        .then([this] {
            uassertStatusOK(_swCollectionReturnValue);
            uassertStatusOK(_swChunksReturnValue);

            // We swap the chunks out of _swChunksReturnValue to ensure if this task is
            // scheduled multiple times that we don't inform the ChunkManager about a chunk it
            // has already updated.
            std::vector<ChunkType> chunks;
            _swChunksReturnValue.getValue().swap(chunks);

            return CollectionAndChangedChunks(
                _swCollectionReturnValue.getValue().getUUID(),
                _swCollectionReturnValue.getValue().getEpoch(),
                _swCollectionReturnValue.getValue().getKeyPattern().toBSON(),
                _swCollectionReturnValue.getValue().getDefaultCollation(),
                _swCollectionReturnValue.getValue().getUnique(),
                std::move(chunks));
        })
        .semi();
}

SemiFuture<DatabaseType> CatalogCacheLoaderMock::getDatabase(StringData dbName) {
    return ExecutorFuture<void>(_executor)
        .then([this] {
            uassertStatusOK(_swDatabaseReturnValue);
            return DatabaseType(_swDatabaseReturnValue.getValue().getName(),
                                _swDatabaseReturnValue.getValue().getPrimary(),
                                _swDatabaseReturnValue.getValue().getSharded(),
                                _swDatabaseReturnValue.getValue().getVersion());
        })
        .semi();
}

void CatalogCacheLoaderMock::setCollectionRefreshReturnValue(
    StatusWith<CollectionType> statusWithCollectionType) {
    _swCollectionReturnValue = std::move(statusWithCollectionType);
}

void CatalogCacheLoaderMock::setChunkRefreshReturnValue(
    StatusWith<std::vector<ChunkType>> statusWithChunks) {
    _swChunksReturnValue = std::move(statusWithChunks);
}

void CatalogCacheLoaderMock::setDatabaseRefreshReturnValue(StatusWith<DatabaseType> swDatabase) {
    _swDatabaseReturnValue = std::move(swDatabase);
}

}  // namespace mongo
