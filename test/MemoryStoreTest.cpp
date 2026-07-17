/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "LlmConfig.h"
#include "MemoryStore.h"
#include "gtest/gtest.h"

using namespace ModLlm;

namespace
{
    ObjectGuid const BOT = ObjectGuid(HighGuid::Player, 100);
    uint32 const SUBJECT = 200;

    void ResetStore()
    {
        sLlmMemoryStore->Clear();
        sLlmConfig->memoryMaxNotesPerBot = 40;
        sLlmConfig->memoryMaxNotesPerSubject = 8;
        sLlmConfig->memoryMaxContentLength = 300;
    }
}

// cppcheck-suppress syntaxError
TEST(MemoryStoreTest, UpsertStoresAndFormats)
{
    ResetStore();

    EXPECT_EQ(sLlmMemoryStore->Upsert(BOT, "mera-friend", SUBJECT, "helped me clear the harpies"), "");
    EXPECT_EQ(sLlmMemoryStore->Format(BOT, SUBJECT, 10),
        "- [mera-friend] helped me clear the harpies\n");
}

TEST(MemoryStoreTest, SameSlugOverwrites)
{
    ResetStore();

    ASSERT_EQ(sLlmMemoryStore->Upsert(BOT, "plan", 0, "hit level 20"), "");
    ASSERT_EQ(sLlmMemoryStore->Upsert(BOT, "plan", 0, "hit level 30"), "");
    EXPECT_EQ(sLlmMemoryStore->Format(BOT, 0, 10), "- [plan] hit level 30\n");
}

TEST(MemoryStoreTest, SlugIsNormalized)
{
    ResetStore();

    ASSERT_EQ(sLlmMemoryStore->Upsert(BOT, "Mera The Mage!", SUBJECT, "met in the barrens"), "");
    EXPECT_EQ(sLlmMemoryStore->Format(BOT, SUBJECT, 10), "- [mera-the-mage] met in the barrens\n");
    EXPECT_TRUE(sLlmMemoryStore->Forget(BOT, "mera the mage"));
    EXPECT_EQ(MemoryStore::NormalizeSlug("  ...  "), "");
}

TEST(MemoryStoreTest, ForgetUnknownSlugFails)
{
    ResetStore();

    EXPECT_FALSE(sLlmMemoryStore->Forget(BOT, "nothing-here"));
}

TEST(MemoryStoreTest, FullMemoryRejectsNewSlugButAllowsOverwrite)
{
    ResetStore();
    sLlmConfig->memoryMaxNotesPerBot = 2;

    ASSERT_EQ(sLlmMemoryStore->Upsert(BOT, "one", 0, "first"), "");
    ASSERT_EQ(sLlmMemoryStore->Upsert(BOT, "two", 0, "second"), "");

    std::string error = sLlmMemoryStore->Upsert(BOT, "three", 0, "third");
    EXPECT_NE(error.find("memory is full"), std::string::npos);

    EXPECT_EQ(sLlmMemoryStore->Upsert(BOT, "two", 0, "second, updated"), "");

    ASSERT_TRUE(sLlmMemoryStore->Forget(BOT, "one"));
    EXPECT_EQ(sLlmMemoryStore->Upsert(BOT, "three", 0, "third"), "");
}

TEST(MemoryStoreTest, PerSubjectCapRejectsNewNote)
{
    ResetStore();
    sLlmConfig->memoryMaxNotesPerSubject = 1;

    ASSERT_EQ(sLlmMemoryStore->Upsert(BOT, "mera-1", SUBJECT, "first"), "");
    std::string error = sLlmMemoryStore->Upsert(BOT, "mera-2", SUBJECT, "second");
    EXPECT_NE(error.find("notes about them"), std::string::npos);

    // General notes and other subjects are unaffected.
    EXPECT_EQ(sLlmMemoryStore->Upsert(BOT, "general", 0, "note"), "");
    EXPECT_EQ(sLlmMemoryStore->Upsert(BOT, "other", SUBJECT + 1, "note"), "");
}

TEST(MemoryStoreTest, FormatScopesAndOrders)
{
    ResetStore();

    ASSERT_EQ(sLlmMemoryStore->Upsert(BOT, "goal-old", 0, "old goal"), "");
    ASSERT_EQ(sLlmMemoryStore->Upsert(BOT, "about-other", SUBJECT + 1, "someone else"), "");
    ASSERT_EQ(sLlmMemoryStore->Upsert(BOT, "goal-new", 0, "new goal"), "");
    ASSERT_EQ(sLlmMemoryStore->Upsert(BOT, "about-mera", SUBJECT, "about her"), "");

    // Subject notes first, then general newest-first; other subjects excluded.
    EXPECT_EQ(sLlmMemoryStore->Format(BOT, SUBJECT, 10),
        "- [about-mera] about her\n- [goal-new] new goal\n- [goal-old] old goal\n");

    // The cap trims general notes before subject notes.
    EXPECT_EQ(sLlmMemoryStore->Format(BOT, SUBJECT, 2),
        "- [about-mera] about her\n- [goal-new] new goal\n");

    // No actor (initiative): general notes only.
    EXPECT_EQ(sLlmMemoryStore->Format(BOT, 0, 10),
        "- [goal-new] new goal\n- [goal-old] old goal\n");
}

TEST(MemoryStoreTest, ContentIsTrimmedAndTruncated)
{
    ResetStore();
    sLlmConfig->memoryMaxContentLength = 5;

    EXPECT_NE(sLlmMemoryStore->Upsert(BOT, "empty", 0, "   "), "");
    ASSERT_EQ(sLlmMemoryStore->Upsert(BOT, "long", 0, "1234567890"), "");
    EXPECT_EQ(sLlmMemoryStore->Format(BOT, 0, 10), "- [long] 12345\n");
}
