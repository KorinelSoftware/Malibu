#pragma once
// core/include/malibu/storage/indexed_db.h
// IndexedDB core (W3C IndexedDB) — a synchronous object-store key/value model.
// The async request/event surface is layered on top by the JS bindings; the
// engine layer keeps the durable state.

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace malibu::storage {

class IndexedDB {
public:
    struct ObjectStore {
        std::map<std::string, std::string> records;  // key -> serialized value
    };
    struct Database {
        std::string                        name;
        int                                version = 0;
        std::map<std::string, ObjectStore> stores;
    };

    // Open (creating or upgrading) a database. Returns the database.
    Database&  open_database(const std::string& name, int version);
    [[nodiscard]] bool has(const std::string& name) const { return dbs_.count(name) != 0; }
    void       delete_database(const std::string& name) { dbs_.erase(name); }
    [[nodiscard]] std::vector<std::string> database_names() const;

    ObjectStore& create_store(const std::string& db, const std::string& store);

    // Record operations within (db, store).
    void put(const std::string& db, const std::string& store,
             const std::string& key, const std::string& value);
    [[nodiscard]] std::optional<std::string> get(const std::string& db, const std::string& store,
                                                 const std::string& key) const;
    bool delete_record(const std::string& db, const std::string& store, const std::string& key);
    [[nodiscard]] size_t count(const std::string& db, const std::string& store) const;

    [[nodiscard]] bool empty() const noexcept { return dbs_.empty(); }

private:
    std::map<std::string, Database> dbs_;
};

} // namespace malibu::storage
