#include <gtest/gtest.h>

#include <syncbox/core/uuid.hpp>
#include <syncbox/core/version_model.hpp>

#include <set>
#include <string>

TEST(VersionModelTest, RevisionArithmetic) {
  const syncbox::Revision none = syncbox::Revision::none();
  EXPECT_FALSE(none.exists());
  EXPECT_EQ(none.next().value, 1);
  EXPECT_TRUE(none.next().exists());
  EXPECT_EQ(syncbox::Revision{7}.next().value, 8);
}

TEST(VersionModelTest, OpKindRoundTrip) {
  using syncbox::OpKind;
  const OpKind kinds[] = {OpKind::Create, OpKind::Update, OpKind::Delete, OpKind::Rename,
                          OpKind::Restore};
  for (const auto kind : kinds) {
    const auto parsed = syncbox::op_kind_from_string(syncbox::to_string(kind));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, kind);
  }
  EXPECT_FALSE(syncbox::op_kind_from_string("merge").has_value());
}

TEST(VersionModelTest, ContentHashHexRoundTrip) {
  const auto hash = syncbox::ContentHash::from_hex(
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash->to_hex(),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  EXPECT_FALSE(syncbox::ContentHash::from_hex("abc").has_value());
  EXPECT_FALSE(syncbox::ContentHash::from_hex(std::string(64, 'z')).has_value());
}

TEST(UuidTest, GeneratesUniqueCanonicalV7) {
  std::set<std::string> seen;
  for (int i = 0; i < 200; ++i) {
    const std::string uuid = syncbox::generate_uuid_v7();
    EXPECT_TRUE(syncbox::is_uuid(uuid)) << uuid;
    EXPECT_EQ(uuid.size(), 36U);
    EXPECT_EQ(uuid[14], '7');  // version nibble
    seen.insert(uuid);
  }
  EXPECT_EQ(seen.size(), 200U);
}

TEST(UuidTest, RejectsMalformedValues) {
  EXPECT_FALSE(syncbox::is_uuid(""));
  EXPECT_FALSE(syncbox::is_uuid("not-a-uuid"));
  EXPECT_FALSE(syncbox::is_uuid("0192f0c4-1111-7aaa-8bbb-00000000000"));
  EXPECT_FALSE(syncbox::is_uuid("0192f0c4_1111_7aaa_8bbb_000000000000"));
}
