#pragma once
#include "Json.h"
#include <memory>

namespace afterlight {
std::string newPersistentId();
void validatePersistentId(const std::string&);
// Virtual locators always resolve to .asset files, never to their payload files.
class AssetPath {
    std::string value_;

  public:
    AssetPath() = default; // Null reference.
    explicit AssetPath(std::string);
    const std::string& string() const {
        return value_;
    }
    bool empty() const {
        return value_.empty();
    }
    bool operator==(const AssetPath& p) const {
        return value_ == p.value_;
    }
    bool operator!=(const AssetPath& p) const {
        return !(*this == p);
    }
    bool operator<(const AssetPath& p) const {
        return value_ < p.value_;
    }
};
struct ObjectPath {
    AssetPath map;
    std::string object;
    explicit ObjectPath(const std::string&);
    ObjectPath(AssetPath map, std::string object);
    std::string string() const {
        return map.string() + ":" + object;
    }
};
// ID is the durable reference; path is a human-readable locator, refreshed on save.
struct AssetRef {
    std::string id;
    AssetPath path;
    Json json() const;
    static AssetRef fromJson(const Json&);
};
// Storage belongs to each asset envelope, independently of its semantic type/decoder.
enum class PayloadStorage { Inline, External };
struct AssetHeader {
    std::string id, type, name;
    uint32_t version = 1;
    PayloadStorage storage = PayloadStorage::Inline;
    std::string source; // External payload path, relative to this .asset; not an AssetPath.
    Json metadata = Json::object();
    Json json() const;
    static AssetHeader fromJson(const Json&);
};
class Asset {
    AssetHeader header_;
    AssetPath path_;
    friend class AssetManager;

  public:
    virtual ~Asset() = default;
    const AssetHeader& header() const {
        return header_;
    }
    AssetRef reference() const {
        return {header_.id, path_};
    }
};
struct DataAsset final : Asset {
    Json data;
};
struct ScriptAsset final : Asset {
    std::string source;
};
struct BinaryAsset final : Asset {
    std::string bytes;
};
} // namespace afterlight
