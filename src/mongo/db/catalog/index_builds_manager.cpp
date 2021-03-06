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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_builds_manager.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/uncommitted_collections.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

/**
 * Returns basic info on index builders.
 */
std::string toSummary(const std::map<UUID, std::unique_ptr<MultiIndexBlock>>& builders) {
    str::stream ss;
    ss << "Number of builders: " << builders.size() << ": [";
    bool first = true;
    for (const auto& pair : builders) {
        if (!first) {
            ss << ", ";
        }
        ss << pair.first;
        first = false;
    }
    ss << "]";
    return ss;
}

}  // namespace

IndexBuildsManager::SetupOptions::SetupOptions() = default;

IndexBuildsManager::~IndexBuildsManager() {
    invariant(_builders.empty(),
              str::stream() << "Index builds still active: " << toSummary(_builders));
}

Status IndexBuildsManager::setUpIndexBuild(OperationContext* opCtx,
                                           Collection* collection,
                                           const std::vector<BSONObj>& specs,
                                           const UUID& buildUUID,
                                           OnInitFn onInit,
                                           SetupOptions options) {
    _registerIndexBuild(buildUUID);

    const auto& nss = collection->ns();
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X),
              str::stream() << "Unable to set up index build " << buildUUID << ": collection "
                            << nss.ns() << " is not locked in exclusive mode.");

    auto builder = invariant(_getBuilder(buildUUID));
    if (options.protocol == IndexBuildProtocol::kTwoPhase) {
        builder->setTwoPhaseBuildUUID(buildUUID);
    }

    // Ignore uniqueness constraint violations when relaxed, for single-phase builds on
    // secondaries. Secondaries can complete index builds in the middle of batches, which creates
    // the potential for finding duplicate key violations where there otherwise would be none at
    // consistent states.
    // Index builds will otherwise defer any unique key constraint checks until commit-time.
    if (options.indexConstraints == IndexConstraints::kRelax &&
        options.protocol == IndexBuildProtocol::kSinglePhase) {
        builder->ignoreUniqueConstraint();
    }

    std::vector<BSONObj> indexes;
    try {
        indexes = writeConflictRetry(opCtx, "IndexBuildsManager::setUpIndexBuild", nss.ns(), [&]() {
            return uassertStatusOK(builder->init(opCtx, collection, specs, onInit));
        });
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    return Status::OK();
}

Status IndexBuildsManager::startBuildingIndex(OperationContext* opCtx,
                                              Collection* collection,
                                              const UUID& buildUUID) {
    auto builder = invariant(_getBuilder(buildUUID));

    return builder->insertAllDocumentsInCollection(opCtx, collection);
}

StatusWith<std::pair<long long, long long>> IndexBuildsManager::startBuildingIndexForRecovery(
    OperationContext* opCtx, NamespaceString ns, const UUID& buildUUID, RepairData repair) {
    auto builder = invariant(_getBuilder(buildUUID));

    auto coll = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, ns);
    auto rs = coll ? coll->getRecordStore() : nullptr;

    // Iterate all records in the collection. Validate the records and index them
    // if they are valid.  Delete them (if in repair mode), or crash, if they are not valid.
    long long numRecords = 0;
    long long dataSize = 0;

    const char* curopMessage = "Index Build: scanning collection";
    ProgressMeterHolder progressMeter;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progressMeter.set(
            CurOp::get(opCtx)->setProgress_inlock(curopMessage, coll->numRecords(opCtx)));
    }

    auto cursor = rs->getCursor(opCtx);
    auto record = cursor->next();
    while (record) {
        opCtx->checkForInterrupt();
        // Cursor is left one past the end of the batch inside writeConflictRetry
        auto beginBatchId = record->id;
        Status status = writeConflictRetry(opCtx, "repairDatabase", ns.ns(), [&] {
            // In the case of WCE in a partial batch, we need to go back to the beginning
            if (!record || (beginBatchId != record->id)) {
                record = cursor->seekExact(beginBatchId);
            }
            WriteUnitOfWork wunit(opCtx);
            for (int i = 0; record && i < internalInsertMaxBatchSize.load(); i++) {
                RecordId id = record->id;
                RecordData& data = record->data;
                // We retain decimal data when repairing database even if decimal is disabled.
                auto validStatus = validateBSON(data.data(), data.size());
                if (!validStatus.isOK()) {
                    if (repair == RepairData::kNo) {
                        LOGV2_FATAL(31396,
                                    "Invalid BSON detected at {id}: {validStatus}",
                                    "Invalid BSON detected",
                                    "id"_attr = id,
                                    "error"_attr = redact(validStatus));
                    }
                    LOGV2_WARNING(20348,
                                  "Invalid BSON detected at {id}: {validStatus}. Deleting.",
                                  "Invalid BSON detected; deleting",
                                  "id"_attr = id,
                                  "error"_attr = redact(validStatus));
                    rs->deleteRecord(opCtx, id);
                    // Must reduce the progress meter's expected total after deleting an invalid
                    // document from the collection.
                    progressMeter->setTotalWhileRunning(coll->numRecords(opCtx));
                } else {
                    numRecords++;
                    dataSize += data.size();
                    auto insertStatus = builder->insertSingleDocumentForInitialSyncOrRecovery(
                        opCtx, data.releaseToBson(), id);
                    if (!insertStatus.isOK()) {
                        return insertStatus;
                    }
                    progressMeter.hit();
                }
                record = cursor->next();
            }

            // Time to yield; make a safe copy of the current record before releasing our cursor.
            if (record)
                record->data.makeOwned();

            cursor->save();  // Can't fail per API definition
            // When this exits via success or WCE, we need to restore the cursor
            ON_BLOCK_EXIT([opCtx, ns, &cursor]() {
                // restore CAN throw WCE per API
                writeConflictRetry(
                    opCtx, "retryRestoreCursor", ns.ns(), [&cursor] { cursor->restore(); });
            });
            wunit.commit();
            return Status::OK();
        });
        if (!status.isOK()) {
            return status;
        }
    }

    progressMeter.finished();
    std::set<RecordId> dups;

    Status status =
        builder->dumpInsertsFromBulk(opCtx, (repair == RepairData::kYes) ? &dups : nullptr);
    if (!status.isOK()) {
        return status;
    }

    // Delete duplicate documents and insert them into local lost and found.
    // TODO SERVER-49507: Reduce memory consumption when there are a large number of duplicate
    // records.
    auto dupRecordsDeleted = _moveDocsToLostAndFound(opCtx, ns, &dups);
    if (!dupRecordsDeleted.isOK()) {
        return dupRecordsDeleted.getStatus();
    }
    numRecords -= dupRecordsDeleted.getValue();

    return std::make_pair(numRecords, dataSize);
}

Status IndexBuildsManager::drainBackgroundWrites(
    OperationContext* opCtx,
    const UUID& buildUUID,
    RecoveryUnit::ReadSource readSource,
    IndexBuildInterceptor::DrainYieldPolicy drainYieldPolicy) {
    auto builder = invariant(_getBuilder(buildUUID));

    return builder->drainBackgroundWrites(opCtx, readSource, drainYieldPolicy);
}

Status IndexBuildsManager::retrySkippedRecords(OperationContext* opCtx,
                                               const UUID& buildUUID,
                                               Collection* collection) {
    auto builder = invariant(_getBuilder(buildUUID));
    return builder->retrySkippedRecords(opCtx, collection);
}

Status IndexBuildsManager::checkIndexConstraintViolations(OperationContext* opCtx,
                                                          const UUID& buildUUID) {
    auto builder = invariant(_getBuilder(buildUUID));

    return builder->checkConstraints(opCtx);
}

Status IndexBuildsManager::commitIndexBuild(OperationContext* opCtx,
                                            Collection* collection,
                                            const NamespaceString& nss,
                                            const UUID& buildUUID,
                                            MultiIndexBlock::OnCreateEachFn onCreateEachFn,
                                            MultiIndexBlock::OnCommitFn onCommitFn) {
    auto builder = invariant(_getBuilder(buildUUID));

    return writeConflictRetry(
        opCtx,
        "IndexBuildsManager::commitIndexBuild",
        nss.ns(),
        [this, builder, buildUUID, opCtx, collection, nss, &onCreateEachFn, &onCommitFn] {
            WriteUnitOfWork wunit(opCtx);
            auto status = builder->commit(opCtx, collection, onCreateEachFn, onCommitFn);
            if (!status.isOK()) {
                return status;
            }
            wunit.commit();
            return Status::OK();
        });
}

bool IndexBuildsManager::abortIndexBuild(OperationContext* opCtx,
                                         Collection* collection,
                                         const UUID& buildUUID,
                                         OnCleanUpFn onCleanUpFn) {
    auto builder = _getBuilder(buildUUID);
    if (!builder.isOK()) {
        return false;
    }

    // Since abortIndexBuild is special in that it can be called by threads other than the index
    // builder, ensure the caller has an exclusive lock.
    auto nss = collection->ns();
    UncommittedCollections::get(opCtx).invariantHasExclusiveAccessToCollection(opCtx, nss);

    builder.getValue()->abortIndexBuild(opCtx, collection, onCleanUpFn);
    return true;
}

bool IndexBuildsManager::abortIndexBuildWithoutCleanupForRollback(OperationContext* opCtx,
                                                                  Collection* collection,
                                                                  const UUID& buildUUID,
                                                                  const std::string& reason) {
    auto builder = _getBuilder(buildUUID);
    if (!builder.isOK()) {
        return false;
    }

    LOGV2(20347,
          "Index build aborted without cleanup for rollback: {uuid}: {reason}",
          "Index build aborted without cleanup for rollback",
          logAttrs(buildUUID),
          "reason"_attr = reason);

    builder.getValue()->abortWithoutCleanupForRollback(opCtx);
    return true;
}

bool IndexBuildsManager::abortIndexBuildWithoutCleanupForShutdown(OperationContext* opCtx,
                                                                  Collection* collection,
                                                                  const UUID& buildUUID) {
    auto builder = _getBuilder(buildUUID);
    if (!builder.isOK()) {
        return false;
    }

    LOGV2(4841500, "Index build aborted without cleanup for shutdown", logAttrs(buildUUID));

    builder.getValue()->abortWithoutCleanupForShutdown(opCtx);
    return true;
}

bool IndexBuildsManager::isBackgroundBuilding(const UUID& buildUUID) {
    auto builder = invariant(_getBuilder(buildUUID));
    return builder->isBackgroundBuilding();
}

void IndexBuildsManager::verifyNoIndexBuilds_forTestOnly() {
    invariant(_builders.empty());
}

void IndexBuildsManager::_registerIndexBuild(UUID buildUUID) {
    stdx::unique_lock<Latch> lk(_mutex);

    auto mib = std::make_unique<MultiIndexBlock>();
    invariant(_builders.insert(std::make_pair(buildUUID, std::move(mib))).second);
}

void IndexBuildsManager::unregisterIndexBuild(const UUID& buildUUID) {
    stdx::unique_lock<Latch> lk(_mutex);

    auto builderIt = _builders.find(buildUUID);
    if (builderIt == _builders.end()) {
        return;
    }
    _builders.erase(builderIt);
}

StatusWith<MultiIndexBlock*> IndexBuildsManager::_getBuilder(const UUID& buildUUID) {
    stdx::unique_lock<Latch> lk(_mutex);
    auto builderIt = _builders.find(buildUUID);
    if (builderIt == _builders.end()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "No index build with UUID: " << buildUUID};
    }
    return builderIt->second.get();
}

StatusWith<long long> IndexBuildsManager::_moveDocsToLostAndFound(OperationContext* opCtx,
                                                                  NamespaceString nss,
                                                                  std::set<RecordId>* dupRecords) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX));

    long long recordsDeleted = 0;
    if (dupRecords->empty()) {
        return recordsDeleted;
    }

    auto originalCollection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
    auto collUUID = originalCollection->uuid();

    const NamespaceString lostAndFoundNss =
        NamespaceString(NamespaceString::kLocalDb, "system.lost_and_found." + collUUID.toString());
    Collection* localCollection =
        CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, lostAndFoundNss);

    // Create the collection if it doesn't exist.
    if (!localCollection) {
        Status status =
            writeConflictRetry(opCtx, "createLostAndFoundCollection", lostAndFoundNss.ns(), [&]() {
                WriteUnitOfWork wuow(opCtx);
                AutoGetOrCreateDb autoDb(opCtx, NamespaceString::kLocalDb, MODE_X);
                Database* db = autoDb.getDb();

                // Ensure the database exists.
                invariant(db);

                // Since we are potentially deleting documents with duplicate _id values, we need to
                // be able to insert into the lost and found collection without generating any
                // duplicate key errors on the _id value.
                CollectionOptions collOptions;
                collOptions.setNoIdIndex();
                localCollection = db->createCollection(opCtx, lostAndFoundNss, collOptions);

                // Ensure the collection exists.
                invariant(localCollection);
                wuow.commit();
                return Status::OK();
            });
        if (!status.isOK()) {
            return status;
        }
    }

    for (auto&& recordId : *dupRecords) {

        Status status =
            writeConflictRetry(opCtx, "writeDupDocToLostAndFoundCollection", nss.ns(), [&]() {
                WriteUnitOfWork wuow(opCtx);

                Snapshotted<BSONObj> doc;
                // If the record doesn’t exist, continue with other documents.
                if (!originalCollection->findDoc(opCtx, recordId, &doc)) {
                    return Status::OK();
                }

                // Write document to lost_and_found collection and delete from original collection.
                Status status =
                    localCollection->insertDocument(opCtx, InsertStatement(doc.value()), nullptr);
                if (!status.isOK()) {
                    return status;
                }

                originalCollection->deleteDocument(opCtx, kUninitializedStmtId, recordId, nullptr);
                wuow.commit();

                recordsDeleted++;
                return Status::OK();
            });
        if (!status.isOK()) {
            return status;
        }
    }
    StorageRepairObserver::get(opCtx->getServiceContext())
        ->invalidatingModification(str::stream()
                                   << "Moved " << recordsDeleted
                                   << " docs to lost and found: " << localCollection->ns());

    LOGV2(3956200,
          "Moved documents to lost and found.",
          "numDocuments"_attr = recordsDeleted,
          "lostAndFoundNss"_attr = lostAndFoundNss);
    return recordsDeleted;
}

}  // namespace mongo
