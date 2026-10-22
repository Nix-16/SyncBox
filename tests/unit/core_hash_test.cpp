#include <gtest/gtest.h>

#include <syncbox/core/hash.hpp>

#include <fstream>

#include "test_support.hpp"

TEST(HashTest, MatchesNistVectors) {
  const auto empty = syncbox::sha256("");
  ASSERT_TRUE(empty);
  EXPECT_EQ(empty.value().to_hex(),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  const auto abc = syncbox::sha256("abc");
  ASSERT_TRUE(abc);
  EXPECT_EQ(abc.value().to_hex(),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(HashTest, StreamingMatchesOneShot) {
  const std::string payload = "The quick brown fox jumps over the lazy dog";

  syncbox::Sha256Hasher hasher;
  ASSERT_TRUE(hasher.update(payload.data(), 10));
  ASSERT_TRUE(hasher.update(payload.data() + 10, payload.size() - 10));
  const auto streamed = hasher.finalize();
  ASSERT_TRUE(streamed);

  const auto one_shot = syncbox::sha256(payload);
  ASSERT_TRUE(one_shot);
  EXPECT_EQ(streamed.value(), one_shot.value());
}

TEST(HashTest, HashesFileInBlocks) {
  syncbox::test::TempDirectory directory;
  const auto path = directory.file("payload.bin");

  {
    std::ofstream stream(path, std::ios::binary);
    ASSERT_TRUE(stream.good());
    stream << "abc";
  }

  const auto file_hash = syncbox::sha256_file(path);
  ASSERT_TRUE(file_hash);
  EXPECT_EQ(file_hash.value().to_hex(),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  const auto missing = syncbox::sha256_file(directory.file("does-not-exist.bin"));
  EXPECT_FALSE(missing);
}

TEST(HashTest, FinalizeTwiceFails) {
  syncbox::Sha256Hasher hasher;
  ASSERT_TRUE(hasher.update("abc", 3));
  ASSERT_TRUE(hasher.finalize());
  EXPECT_FALSE(hasher.finalize());
}
