#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "expiry/ExpiryManager.h"
#include "persistence/AOFManager.h"
#include "storage/StorageEngine.h"

using namespace flashdb;

namespace {

std::filesystem::path makeTempAofPath(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove(path);
    return path;
}

void writeAofFile(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream out(path);
    ASSERT_TRUE(out.is_open());
    out << contents;
}

} // namespace

TEST(AOFTest, SetWithoutExpiryClearsPreviousTTL) {
    auto path = makeTempAofPath("flashdb_aof_clear_ttl_test.aof");
    writeAofFile(path, "SET token old EX 60\nSET token fresh\n");

    StorageEngine storage;
    ExpiryManager expiry;
    storage.setExpiryManager(&expiry);

    {
        AOFManager aof(path.string());
        EXPECT_EQ(aof.loadAOF(storage, expiry), 2);
    }

    auto value = storage.get("token");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "fresh");
    EXPECT_FALSE(expiry.hasExpiry("token"));

    std::filesystem::remove(path);
}

TEST(AOFTest, InvalidSetExpiryDoesNotMutateStorage) {
    auto path = makeTempAofPath("flashdb_aof_invalid_set_ttl_test.aof");
    writeAofFile(path, "SET token value EX not_an_int\n");

    StorageEngine storage;
    ExpiryManager expiry;
    storage.setExpiryManager(&expiry);

    {
        AOFManager aof(path.string());
        EXPECT_EQ(aof.loadAOF(storage, expiry), 0);
    }

    EXPECT_FALSE(storage.get("token").has_value());
    EXPECT_FALSE(expiry.hasExpiry("token"));

    std::filesystem::remove(path);
}

TEST(AOFTest, SetExpiryOptionIsCaseInsensitiveOnReplay) {
    auto path = makeTempAofPath("flashdb_aof_lowercase_ex_test.aof");
    writeAofFile(path, "SET token value ex 60\n");

    StorageEngine storage;
    ExpiryManager expiry;
    storage.setExpiryManager(&expiry);

    {
        AOFManager aof(path.string());
        EXPECT_EQ(aof.loadAOF(storage, expiry), 1);
    }

    EXPECT_TRUE(storage.get("token").has_value());
    EXPECT_TRUE(expiry.hasExpiry("token"));

    std::filesystem::remove(path);
}
