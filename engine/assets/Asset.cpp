#include "Asset.h"
#include <algorithm>
#include <random>
#include <stdexcept>

namespace afterlight {
void validatePersistentId(const std::string& id) {
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("Persistent ID must be 32 lowercase hex digits: " + id);
}
std::string newPersistentId() {
    static std::random_device random;
    static const char* digits = "0123456789abcdef";
    std::string result;
    for (int i = 0; i < 4; ++i) {
        uint32_t n = random();
        for (int k = 0; k < 8; ++k) {
            result += digits[n & 15];
            n >>= 4;
        }
    }
    return result;
}
AssetPath::AssetPath(std::string value) : value_(std::move(value)) {
    if (value_.compare(0, 6, "/Game/") != 0 || value_.size() == 6 || value_.back() == '/' ||
        value_.find("//") != std::string::npos ||
        value_.find_first_not_of("/ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
            std::string::npos)
        throw std::invalid_argument("Invalid virtual asset path: " + value_);
}
ObjectPath::ObjectPath(const std::string& value)
    : ObjectPath(AssetPath(value.substr(0, value.find(':'))),
                 value.find(':') == std::string::npos ? "" : value.substr(value.find(':') + 1)) {}
ObjectPath::ObjectPath(AssetPath p, std::string id) : map(std::move(p)), object(std::move(id)) {
    if (map.empty())
        throw std::invalid_argument("Object path requires a Map");
    validatePersistentId(object);
}
Json AssetRef::json() const {
    return {{"id", id}, {"path", path.string()}};
}
AssetRef AssetRef::fromJson(const Json& j) {
    AssetRef r{j.at("id").string(), AssetPath(j.at("path").string())};
    validatePersistentId(r.id);
    return r;
}
Json AssetHeader::json() const {
    Json j{{"id", id},
           {"type", type},
           {"name", name},
           {"version", version},
           {"storage", storage == PayloadStorage::Inline ? "embedded" : "external"},
           {"metadata", metadata}};
    if (storage == PayloadStorage::External)
        j["source"] = source;
    return j;
}
AssetHeader AssetHeader::fromJson(const Json& j) {
    AssetHeader h;
    h.id = j.at("id").string();
    validatePersistentId(h.id);
    h.type = j.at("type").string();
    h.name = j.at("name").string();
    h.version = j.at("version").uint();
    const auto& storage = j.at("storage").string();
    h.metadata = j.at("metadata");
    (void)h.metadata.members();
    if (h.type.empty() || h.version != 1 || (storage != "embedded" && storage != "external"))
        throw std::invalid_argument("Unsupported asset descriptor");
    h.storage = storage == "embedded" ? PayloadStorage::Inline : PayloadStorage::External;
    if (h.storage == PayloadStorage::External)
        h.source = j.at("source").string();
    else if (j.contains("source"))
        throw std::invalid_argument("Inline asset cannot declare an external payload address");
    return h;
}
} // namespace afterlight
