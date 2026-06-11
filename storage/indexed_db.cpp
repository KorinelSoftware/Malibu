// storage/indexed_db.cpp
// IndexedDB object-store key/value core.

#include "malibu/storage/indexed_db.h"

namespace malibu::storage {

IndexedDB::Database& IndexedDB::open_database(const std::string& name, int version) {
    Database& db = dbs_[name];
    if (db.name.empty()) db.name = name;
    if (version > db.version) db.version = version;
    return db;
}

std::vector<std::string> IndexedDB::database_names() const {
    std::vector<std::string> names;
    names.reserve(dbs_.size());
    for (const auto& [n, db] : dbs_) names.push_back(n);
    return names;
}

IndexedDB::ObjectStore& IndexedDB::create_store(const std::string& db, const std::string& store) {
    return dbs_[db].stores[store];
}

void IndexedDB::put(const std::string& db, const std::string& store,
                    const std::string& key, const std::string& value) {
    dbs_[db].stores[store].records[key] = value;
}

std::optional<std::string> IndexedDB::get(const std::string& db, const std::string& store,
                                          const std::string& key) const {
    auto dbit = dbs_.find(db);
    if (dbit == dbs_.end()) return std::nullopt;
    auto sit = dbit->second.stores.find(store);
    if (sit == dbit->second.stores.end()) return std::nullopt;
    auto rit = sit->second.records.find(key);
    if (rit == sit->second.records.end()) return std::nullopt;
    return rit->second;
}

bool IndexedDB::delete_record(const std::string& db, const std::string& store, const std::string& key) {
    auto dbit = dbs_.find(db);
    if (dbit == dbs_.end()) return false;
    auto sit = dbit->second.stores.find(store);
    if (sit == dbit->second.stores.end()) return false;
    return sit->second.records.erase(key) != 0;
}

size_t IndexedDB::count(const std::string& db, const std::string& store) const {
    auto dbit = dbs_.find(db);
    if (dbit == dbs_.end()) return 0;
    auto sit = dbit->second.stores.find(store);
    if (sit == dbit->second.stores.end()) return 0;
    return sit->second.records.size();
}

} // namespace malibu::storage
