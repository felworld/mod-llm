/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "HistoryStore.h"

#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "LlmConfig.h"
#include "Log.h"
#include "QueryResult.h"

#include <fmt/args.h>
#include <fmt/format.h>

namespace ModLlm
{
    namespace
    {
        // In-memory cap per buffer; prompt formatting applies its own limit.
        constexpr size_t MAX_BUFFER_LINES = 50;
    }

    HistoryStore* HistoryStore::instance()
    {
        static HistoryStore instance;
        return &instance;
    }

    void HistoryStore::Load()
    {
        uint32 retentionDays = sLlmConfig->historyRetentionDays;
        if (retentionDays)
        {
            CharacterDatabase.Execute(
                "DELETE FROM mod_llm_history_pair WHERE created_at < NOW() - INTERVAL {} DAY", retentionDays);
            CharacterDatabase.Execute(
                "DELETE FROM mod_llm_history_room WHERE created_at < NOW() - INTERVAL {} DAY", retentionDays);
        }

        uint32 loadedPair = 0;
        uint32 loadedRoom = 0;

        if (QueryResult result = CharacterDatabase.Query(
            "SELECT bot_guid, player_guid, speaker, message FROM mod_llm_history_pair ORDER BY id ASC"))
        {
            std::lock_guard<std::mutex> lock(_mutex);
            do
            {
                Field* fields = result->Fetch();
                ObjectGuid botGuid = ObjectGuid(HighGuid::Player, fields[0].Get<uint32>());
                ObjectGuid playerGuid = ObjectGuid(HighGuid::Player, fields[1].Get<uint32>());
                bool botSpoke = fields[2].Get<uint8>() != 0;

                std::string speakerName;
                if (!sCharacterCache->GetCharacterNameByGuid(botSpoke ? botGuid : playerGuid, speakerName))
                    speakerName = botSpoke ? "them" : "someone";

                auto& buffer = _pairs[MakeKey(botGuid, playerGuid)];
                buffer.push_back({ speakerName, fields[3].Get<std::string>() });
                if (buffer.size() > MAX_BUFFER_LINES)
                    buffer.pop_front();

                ++loadedPair;
            } while (result->NextRow());
        }

        if (QueryResult result = CharacterDatabase.Query(
            "SELECT room_key, speaker_name, message FROM mod_llm_history_room ORDER BY id ASC"))
        {
            std::lock_guard<std::mutex> lock(_mutex);
            do
            {
                Field* fields = result->Fetch();
                auto& buffer = _rooms[fields[0].Get<std::string>()];
                buffer.push_back({ fields[1].Get<std::string>(), fields[2].Get<std::string>() });
                if (buffer.size() > MAX_BUFFER_LINES)
                    buffer.pop_front();

                ++loadedRoom;
            } while (result->NextRow());
        }

        LOG_INFO("module.llm", "Loaded {} pair and {} room history lines", loadedPair, loadedRoom);
    }

    void HistoryStore::SaveDirty()
    {
        std::vector<PendingPairRow> pairRows;
        std::vector<PendingRoomRow> roomRows;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            pairRows.swap(_pendingPairRows);
            roomRows.swap(_pendingRoomRows);
        }

        for (PendingPairRow& row : pairRows)
        {
            CharacterDatabase.EscapeString(row.text);
            CharacterDatabase.Execute(
                "INSERT INTO mod_llm_history_pair (bot_guid, player_guid, speaker, message) VALUES ({}, {}, {}, '{}')",
                row.botGuid, row.playerGuid, uint32(row.botSpoke ? 1 : 0), row.text);
        }

        for (PendingRoomRow& row : roomRows)
        {
            CharacterDatabase.EscapeString(row.roomKey);
            CharacterDatabase.EscapeString(row.speakerName);
            CharacterDatabase.EscapeString(row.text);
            CharacterDatabase.Execute(
                "INSERT INTO mod_llm_history_room (room_key, speaker_guid, speaker_name, message) VALUES ('{}', {}, '{}', '{}')",
                row.roomKey, row.speakerGuid, row.speakerName, row.text);
        }
    }

    void HistoryStore::AddPairLine(ObjectGuid botGuid, ObjectGuid playerGuid, bool botSpoke,
        std::string const& speakerName, std::string const& text)
    {
        if (!sLlmConfig->historyEnabled)
            return;

        std::lock_guard<std::mutex> lock(_mutex);
        auto& buffer = _pairs[MakeKey(botGuid, playerGuid)];
        buffer.push_back({ speakerName, text });
        if (buffer.size() > MAX_BUFFER_LINES)
            buffer.pop_front();

        _pendingPairRows.push_back({ botGuid.GetCounter(), playerGuid.GetCounter(), botSpoke, text });
    }

    void HistoryStore::AddRoomLine(std::string const& roomKey, ObjectGuid speakerGuid,
        std::string const& speakerName, std::string const& text)
    {
        if (!sLlmConfig->historyEnabled || roomKey.empty())
            return;

        std::lock_guard<std::mutex> lock(_mutex);
        auto& buffer = _rooms[roomKey];
        buffer.push_back({ speakerName, text });
        if (buffer.size() > MAX_BUFFER_LINES)
            buffer.pop_front();

        _pendingRoomRows.push_back({ roomKey, speakerGuid.GetCounter(), speakerName, text });
    }

    std::string HistoryStore::FormatPair(ObjectGuid botGuid, ObjectGuid playerGuid, uint32 maxLines)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _pairs.find(MakeKey(botGuid, playerGuid));
        return it != _pairs.end() ? FormatLines(it->second, maxLines) : "";
    }

    std::string HistoryStore::FormatRoom(std::string const& roomKey, uint32 maxLines)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _rooms.find(roomKey);
        return it != _rooms.end() ? FormatLines(it->second, maxLines) : "";
    }

    std::string HistoryStore::FormatLines(std::deque<Line> const& lines, uint32 maxLines)
    {
        size_t start = lines.size() > maxLines ? lines.size() - maxLines : 0;
        std::string result;
        for (size_t i = start; i < lines.size(); ++i)
        {
            fmt::dynamic_format_arg_store<fmt::format_context> args;
            args.push_back(fmt::arg("speaker", lines[i].speakerName));
            args.push_back(fmt::arg("message", lines[i].text));
            try
            {
                result += fmt::vformat(sLlmConfig->promptHistoryLine, args);
            }
            catch (fmt::format_error const&)
            {
                result += lines[i].speakerName + ": " + lines[i].text;
            }
            result += '\n';
        }
        return result;
    }
}
