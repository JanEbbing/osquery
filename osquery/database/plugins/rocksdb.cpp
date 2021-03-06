/**
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under both the Apache 2.0 license (found in the
 *  LICENSE file in the root directory of this source tree) and the GPLv2 (found
 *  in the COPYING file in the root directory of this source tree).
 *  You may select, at your option, one of the above-listed licenses.
 */

#include <sys/stat.h>

#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/options.h>

#include <osquery/filesystem.h>
#include <osquery/logger.h>
#include <osquery/registry_factory.h>

#include "osquery/core/conversions.h"
#include "osquery/database/plugins/rocksdb.h"
#include "osquery/filesystem/fileops.h"

namespace fs = boost::filesystem;

namespace osquery {

/// Hidden flags created for internal stress testing.
HIDDEN_FLAG(int32, rocksdb_write_buffer, 16, "Max write buffer number");
HIDDEN_FLAG(int32, rocksdb_merge_number, 4, "Min write buffer number to merge");
HIDDEN_FLAG(int32, rocksdb_background_flushes, 4, "Max background flushes");
HIDDEN_FLAG(uint64, rocksdb_buffer_blocks, 256, "Write buffer blocks (4k)");

DECLARE_string(database_path);

/**
 * @brief Track external systems marking the RocksDB database as corrupted.
 *
 * This can be set using the RocksDBDatabasePlugin's static methods.
 * The two primary external systems are the RocksDB logger plugin and tests.
 */
std::atomic<bool> kRocksDBCorruptionIndicator{false};

/// Backing-storage provider for osquery internal/core.
REGISTER_INTERNAL(RocksDBDatabasePlugin, "database", "rocksdb");

void GlogRocksDBLogger::Logv(const char* format, va_list ap) {
  // Convert RocksDB log to string and check if header or level-ed log.
  std::string log_line;
  {
    char buffer[501] = {0};
    vsnprintf(buffer, 500, format, ap);
    va_end(ap);
    if (buffer[0] != '[' || (buffer[1] != 'E' && buffer[1] != 'W')) {
      return;
    }

    log_line = buffer;
  }

  // There is a spurious warning on first open.
  if (log_line.find("Error when reading") == std::string::npos) {
    // RocksDB calls are non-reentrant. Since this callback is made in the
    // context of a RocksDB API call, turn log forwarding off to prevent the
    // logger from trying to make a call back into RocksDB and causing a
    // deadlock.
    LOG(INFO) << "RocksDB: " << log_line;
  }

  // If the callback includes 'Corruption' then set the corruption indicator.
  if (log_line.find("Corruption:") != std::string::npos) {
    RocksDBDatabasePlugin::setCorrupted();
  }
}

Status RocksDBDatabasePlugin::setUp() {
  if (!DatabasePlugin::kDBAllowOpen) {
    LOG(WARNING) << RLOG(1629) << "Not allowed to set up database plugin";
  }

  if (!initialized_) {
    initialized_ = true;
    options_.OptimizeForSmallDb();

    // Set meta-data (mostly) handling options.
    options_.create_if_missing = true;
    options_.create_missing_column_families = true;
    options_.info_log_level = rocksdb::ERROR_LEVEL;
    options_.log_file_time_to_roll = 0;
    options_.keep_log_file_num = 10;
    options_.max_log_file_size = 1024 * 1024 * 1;
    options_.max_open_files = 128;
    options_.stats_dump_period_sec = 0;
    options_.max_manifest_file_size = 1024 * 500;

    // Performance and optimization settings.
    // Use rocksdb::kZSTD to use ZSTD database compression
    options_.compression = rocksdb::kNoCompression;
    options_.compaction_style = rocksdb::kCompactionStyleLevel;
    options_.arena_block_size = (4 * 1024);
    options_.write_buffer_size = (4 * 1024) * FLAGS_rocksdb_buffer_blocks;
    options_.max_write_buffer_number =
        static_cast<int>(FLAGS_rocksdb_write_buffer);
    options_.min_write_buffer_number_to_merge =
        static_cast<int>(FLAGS_rocksdb_merge_number);
    options_.max_background_flushes =
        static_cast<int>(FLAGS_rocksdb_background_flushes);

    // Create an environment to replace the default logger.
    if (logger_ == nullptr) {
      logger_ = std::make_shared<GlogRocksDBLogger>();
    }
    options_.info_log = logger_;

    column_families_.push_back(rocksdb::ColumnFamilyDescriptor(
        rocksdb::kDefaultColumnFamilyName, options_));

    for (const auto& cf_name : kDomains) {
      column_families_.push_back(
          rocksdb::ColumnFamilyDescriptor(cf_name, options_));
    }
  }

  // Consume the current settings.
  // A configuration update may change them, but that does not affect state.
  path_ = fs::path(FLAGS_database_path).make_preferred().string();

  if (pathExists(path_).ok() && !isReadable(path_).ok()) {
    return Status(1, "Cannot read RocksDB path: " + path_);
  }

  if (!DatabasePlugin::kDBChecking) {
    VLOG(1) << "Opening RocksDB handle: " << path_;
  }

  // Tests may trash calls to setUp, make sure subsequent calls do not leak.
  close();

  // Attempt to create a RocksDB instance and handles.
  auto s =
      rocksdb::DB::Open(options_, path_, column_families_, &handles_, &db_);

  if (s.IsCorruption()) {
    // The database is corrupt - try to repair it
    repairDB();
    s = rocksdb::DB::Open(options_, path_, column_families_, &handles_, &db_);
  }

  if (!s.ok() || db_ == nullptr) {
    LOG(INFO) << "Rocksdb open failed (" << s.code() << ":" << s.subcode()
              << ") " << s.ToString();
    if (kDBRequireWrite) {
      // A failed open in R/W mode is a runtime error.
      return Status(1, s.ToString());
    }

    if (!DatabasePlugin::kDBChecking) {
      LOG(INFO) << "Opening RocksDB failed: Continuing with read-only support";
    }
    // Also disable event publishers.
    Flag::updateValue("disable_events", "true");
    read_only_ = true;
  }

  // RocksDB may not create/append a directory with acceptable permissions.
  if (!read_only_ && platformChmod(path_, S_IRWXU) == false) {
    return Status(1, "Cannot set permissions on RocksDB path: " + path_);
  }
  return Status(0);
}

void RocksDBDatabasePlugin::tearDown() {
  close();
}

void RocksDBDatabasePlugin::close() {
  WriteLock lock(close_mutex_);
  for (auto handle : handles_) {
    delete handle;
  }
  handles_.clear();

  if (db_ != nullptr) {
    delete db_;
    db_ = nullptr;
  }

  if (isCorrupted()) {
    repairDB();
    setCorrupted(false);
  }
}

bool RocksDBDatabasePlugin::isCorrupted() {
  return kRocksDBCorruptionIndicator;
}

void RocksDBDatabasePlugin::setCorrupted(bool corrupted) {
  kRocksDBCorruptionIndicator = corrupted;
}

void RocksDBDatabasePlugin::repairDB() {
  // Try to backup the existing database.
  auto bpath = path_ + ".backup";
  if (pathExists(bpath).ok()) {
    if (!removePath(bpath).ok()) {
      LOG(ERROR) << "Cannot remove previous RocksDB database backup: " << bpath;
      return;
    } else {
      LOG(WARNING) << "Removed previous RocksDB database backup: " << bpath;
    }
  }

  if (movePath(path_, bpath).ok()) {
    LOG(WARNING) << "Backing up RocksDB database: " << bpath;
  } else {
    LOG(ERROR) << "Cannot backup the RocksDB database: " << bpath;
    return;
  }

  // ROCKSDB_LITE does not have a RepairDB method.
  LOG(WARNING) << "Destroying RocksDB database due to corruption";
}

rocksdb::DB* RocksDBDatabasePlugin::getDB() const {
  return db_;
}

rocksdb::ColumnFamilyHandle* RocksDBDatabasePlugin::getHandleForColumnFamily(
    const std::string& cf) const {
  size_t i = std::find(kDomains.begin(), kDomains.end(), cf) - kDomains.begin();
  if (i != kDomains.size()) {
    return handles_[i];
  } else {
    return nullptr;
  }
}

Status RocksDBDatabasePlugin::get(const std::string& domain,
                                  const std::string& key,
                                  std::string& value) const {
  if (getDB() == nullptr) {
    return Status(1, "Database not opened");
  }
  auto cfh = getHandleForColumnFamily(domain);
  if (cfh == nullptr) {
    return Status(1, "Could not get column family for " + domain);
  }
  auto s = getDB()->Get(rocksdb::ReadOptions(), cfh, key, &value);
  return Status(s.code(), s.ToString());
}

Status RocksDBDatabasePlugin::get(const std::string& domain,
                                  const std::string& key,
                                  int& value) const {
  std::string result;
  auto s = this->get(domain, key, result);
  if (s.ok()) {
    auto expectedValue = tryTo<int>(result);
    if (expectedValue.isError()) {
      return Status::failure("Could not deserialize str to int");
    } else {
      value = expectedValue.take();
    }
  }
  return s;
}
Status RocksDBDatabasePlugin::put(const std::string& domain,
                                  const std::string& key,
                                  const std::string& value) {
  return putBatch(domain, {std::make_pair(key, value)});
}

Status RocksDBDatabasePlugin::putBatch(const std::string& domain,
                                       const DatabaseStringValueList& data) {
  if (read_only_) {
    return Status(0, "Database in readonly mode");
  }

  auto cfh = getHandleForColumnFamily(domain);
  if (cfh == nullptr) {
    return Status(1, "Could not get column family for " + domain);
  }

  // Events should be fast, and do not need to force syncs.
  auto options = rocksdb::WriteOptions();
  if (kEvents == domain) {
    options.disableWAL = true;
  } else {
    options.sync = true;
  }

  rocksdb::WriteBatch batch;
  for (const auto& p : data) {
    const auto& key = p.first;
    const auto& value = p.second;

    batch.Put(cfh, key, value);
  }

  auto s = getDB()->Write(options, &batch);
  if (s.code() != 0 && s.IsIOError()) {
    // An error occurred, check if it is an IO error and remove the offending
    // specific filename or log name.
    std::string error_string = s.ToString();
    size_t error_pos = error_string.find_last_of(":");
    if (error_pos != std::string::npos) {
      return Status(s.code(), "IOError: " + error_string.substr(error_pos + 2));
    }
  }

  return Status(s.code(), s.ToString());
}

Status RocksDBDatabasePlugin::put(const std::string& domain,
                                  const std::string& key,
                                  int value) {
  return putBatch(domain, {std::make_pair(key, std::to_string(value))});
}

void RocksDBDatabasePlugin::dumpDatabase() const {}

Status RocksDBDatabasePlugin::remove(const std::string& domain,
                                     const std::string& key) {
  if (read_only_) {
    return Status(0, "Database in readonly mode");
  }

  auto cfh = getHandleForColumnFamily(domain);
  if (cfh == nullptr) {
    return Status(1, "Could not get column family for " + domain);
  }
  auto options = rocksdb::WriteOptions();

  // We could sync here, but large deletes will cause multi-syncs.
  // For example: event record expirations found in an expired index.
  if (kEvents != domain) {
    options.sync = true;
  }
  auto s = getDB()->Delete(options, cfh, key);
  return Status(s.code(), s.ToString());
}

Status RocksDBDatabasePlugin::removeRange(const std::string& domain,
                                          const std::string& low,
                                          const std::string& high) {
  if (read_only_) {
    return Status(0, "Database in readonly mode");
  }

  auto cfh = getHandleForColumnFamily(domain);
  if (cfh == nullptr) {
    return Status(1, "Could not get column family for " + domain);
  }
  auto options = rocksdb::WriteOptions();

  // We could sync here, but large deletes will cause multi-syncs.
  // For example: event record expirations found in an expired index.
  if (kEvents != domain) {
    options.sync = true;
  }
  auto s = getDB()->DeleteRange(options, cfh, low, high);
  if (low <= high) {
    s = getDB()->Delete(options, cfh, high);
  }
  return Status(s.code(), s.ToString());
}

Status RocksDBDatabasePlugin::scan(const std::string& domain,
                                   std::vector<std::string>& results,
                                   const std::string& prefix,
                                   size_t max) const {
  if (getDB() == nullptr) {
    return Status(1, "Database not opened");
  }

  auto cfh = getHandleForColumnFamily(domain);
  if (cfh == nullptr) {
    return Status(1, "Could not get column family for " + domain);
  }
  auto options = rocksdb::ReadOptions();
  options.verify_checksums = false;
  options.fill_cache = false;
  auto it = getDB()->NewIterator(options, cfh);
  if (it == nullptr) {
    return Status(1, "Could not get iterator for " + domain);
  }

  size_t count = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    auto key = it->key().ToString();
    if (key.find(prefix) == 0) {
      results.push_back(std::move(key));
      if (max > 0 && ++count >= max) {
        break;
      }
    }
  }
  delete it;
  return Status(0, "OK");
}
} // namespace osquery
