#include <anof/infrastructure/storage/local/internal/BackupEncoding.h>

#include <charconv>
#include <set>
namespace anof::infrastructure::storage::maintenance {
namespace {
void keys(const Json& j, std::initializer_list<const char*> expected) {
    if (!j.is_object() || j.size() != expected.size()) throw std::invalid_argument("shape");
    for (auto name : expected)
        if (!j.contains(name)) throw std::invalid_argument("shape");
}
std::int64_t number(const Json& j, bool negative = false) {
    auto s = j.get<std::string>();
    std::int64_t n = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), n);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || s != std::to_string(n) || (!negative && n < 0)) throw std::invalid_argument("number");
    return n;
}
template <class T>
T id(const Json& j) {
    auto r = T::parse(j.get<std::string>());
    if (!r) throw std::invalid_argument("id");
    return r.value();
}
Sha256 hash(const Json& j) {
    auto r = Sha256::parse(j.get<std::string>());
    if (!r) throw std::invalid_argument("hash");
    return r.value();
}
Json digest(const BackupFileDigest& d) { return { { "size", std::to_string(d.size.value()) }, { "sha256", d.sha256.toString() } }; }
BackupFileDigest digest(const Json& j) {
    keys(j, { "size", "sha256" });
    return { ByteCount::from(number(j.at("size"))).value(), hash(j.at("sha256")) };
}
}  // namespace
Result<Json> parseStrict(std::string_view text, std::size_t limit) {
    if (text.size() > limit || text.substr(0, 3) == "\xef\xbb\xbf") return ErrorCode::IntegrityError;
    try {
        std::vector<std::set<std::string>> names;
        bool valid = true;
        auto j = Json::parse(text, [&](int depth, Json::parse_event_t event, Json& parsed) {if(depth>16)throw std::invalid_argument("depth");if(event==Json::parse_event_t::object_start)names.emplace_back();if(event==Json::parse_event_t::key){if(names.empty()||!names.back().insert(parsed.get<std::string>()).second)valid=false;}if(event==Json::parse_event_t::object_end)names.pop_back();return true; });
        if (!valid) return ErrorCode::IntegrityError;
        return j;
    } catch (const std::exception&) {
        return ErrorCode::IntegrityError;
    }
}
Json encode(const Blob& b) { return { { "id", b.id.toString() }, { "key", b.storageKey() }, { "size", std::to_string(b.size.value()) }, { "sha256", b.sha256.toString() } }; }
Result<Blob> object(const Json& j) {
    try {
        keys(j, { "id", "key", "size", "sha256" });
        Blob b{ id<BlobId>(j.at("id")), FileLength::from(number(j.at("size"))).value(), hash(j.at("sha256")) };
        if (j.at("key") != b.storageKey()) return ErrorCode::IntegrityError;
        return b;
    } catch (const std::exception&) {
        return ErrorCode::IntegrityError;
    }
}
Json encode(const BackupManifest& m) {
    auto index = digest(m.objects.file);
    index["count"] = std::to_string(m.objects.count.value());
    index["total_bytes"] = std::to_string(m.objects.totalBytes.value());
    return { { "backup_format", m.backupFormat }, { "backend", m.backend }, { "application_version", m.applicationVersion }, { "schema", m.schemaVersion }, { "format", m.formatVersion }, { "backup_id", m.id.toString() }, { "instance_id", m.instance.toString() }, { "created_at", std::to_string(m.createdAt.time_since_epoch().count()) }, { "database", digest(m.database) }, { "configuration", digest(m.configuration) }, { "object_index", index }, { "exclusions", { { "quarantine_objects", std::to_string(m.exclusions.quarantineObjects.value()) }, { "quarantine_bytes", std::to_string(m.exclusions.quarantineBytes.value()) }, { "unreferenced_objects", std::to_string(m.exclusions.unreferencedObjects.value()) }, { "unreferenced_bytes", std::to_string(m.exclusions.unreferencedBytes.value()) }, { "uploads", std::to_string(m.exclusions.uploads.value()) } } } };
}
Result<BackupManifest> manifest(const Json& j) {
    try {
        keys(j, { "backup_format", "backend", "application_version", "schema", "format", "backup_id", "instance_id", "created_at", "database", "configuration", "object_index", "exclusions" });
        auto& i = j.at("object_index");
        keys(i, { "size", "sha256", "count", "total_bytes" });
        auto& e = j.at("exclusions");
        keys(e, { "quarantine_objects", "quarantine_bytes", "unreferenced_objects", "unreferenced_bytes", "uploads" });
        for (auto key : { "backup_format", "schema", "format" })
            if (!j.at(key).is_number_integer()) return ErrorCode::IntegrityError;
        if (j.at("backup_format") != 1 || j.at("schema") != 5 || j.at("format") != 5) return ErrorCode::IntegrityError;
        BackupManifest m{ j.at("backup_format").get<int>(), j.at("backend").get<std::string>(), j.at("application_version").get<std::string>(), j.at("schema").get<std::int64_t>(), j.at("format").get<std::int64_t>(), id<BackupId>(j.at("backup_id")), id<InstanceId>(j.at("instance_id")), UtcTime{ std::chrono::microseconds{ number(j.at("created_at"), true) } }, digest(j.at("database")), digest(j.at("configuration")), { { ByteCount::from(number(i.at("size"))).value(), hash(i.at("sha256")) }, NodeCount::from(number(i.at("count"))).value(), ByteCount::from(number(i.at("total_bytes"))).value() }, { NodeCount::from(number(e.at("quarantine_objects"))).value(), ByteCount::from(number(e.at("quarantine_bytes"))).value(), NodeCount::from(number(e.at("unreferenced_objects"))).value(), ByteCount::from(number(e.at("unreferenced_bytes"))).value(), NodeCount::from(number(e.at("uploads"))).value() } };
        if (!validateBackupManifest(m)) return ErrorCode::IntegrityError;
        return m;
    } catch (const std::exception&) {
        return ErrorCode::IntegrityError;
    }
}
}  // namespace anof::infrastructure::storage::maintenance
