/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

#include "MemoryStore.h"

#include "DatabaseEnv.h"
#include "LlmConfig.h"
#include "Log.h"
#include "QueryResult.h"
#include "StringFormat.h"

#include <algorithm>
#include <cctype>

namespace ModLlm
{
    namespace
    {
        constexpr size_t MAX_SLUG_LENGTH = 48; // matches the schema
    }

    MemoryStore* MemoryStore::instance()
    {
        static MemoryStore instance;
        return &instance;
    }

    void MemoryStore::Load()
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT bot_guid, slug, subject_guid, content FROM mod_llm_memory ORDER BY updated_at ASC, slug ASC");
        if (!result)
            return;

        uint32 loaded = 0;
        std::lock_guard<std::mutex> lock(_mutex);
        do
        {
            Field* fields = result->Fetch();
            _notes[fields[0].Get<uint32>()].push_back({ fields[1].Get<std::string>(),
                fields[2].Get<uint32>(), fields[3].Get<std::string>(), ++_sequence });
            ++loaded;
        } while (result->NextRow());

        LOG_INFO("module.llm", "Loaded {} memory notes", loaded);
    }

    void MemoryStore::SaveDirty()
    {
        std::vector<PendingRow> rows;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            rows.swap(_pending);
        }

        for (PendingRow& row : rows)
        {
            CharacterDatabase.EscapeString(row.slug);
            if (row.isDelete)
            {
                CharacterDatabase.Execute(
                    "DELETE FROM mod_llm_memory WHERE bot_guid = {} AND slug = '{}'", row.botGuid, row.slug);
            }
            else
            {
                CharacterDatabase.EscapeString(row.content);
                CharacterDatabase.Execute(
                    "REPLACE INTO mod_llm_memory (bot_guid, slug, subject_guid, content) VALUES ({}, '{}', {}, '{}')",
                    row.botGuid, row.slug, row.subjectGuid, row.content);
            }
        }
    }

    std::string MemoryStore::Upsert(ObjectGuid botGuid, std::string const& slug, uint32 subjectGuid,
        std::string content)
    {
        std::string normalized = NormalizeSlug(slug);
        if (normalized.empty())
            return "invalid slug: use short lowercase words joined by dashes";

        while (!content.empty() && std::isspace(static_cast<unsigned char>(content.back())))
            content.pop_back();
        if (content.empty())
            return "empty note";
        if (content.size() > sLlmConfig->memoryMaxContentLength)
            content.resize(sLlmConfig->memoryMaxContentLength);

        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<Note>& notes = _notes[botGuid.GetCounter()];

        auto it = std::find_if(notes.begin(), notes.end(),
            [&](Note const& note) { return note.slug == normalized; });
        if (it == notes.end())
        {
            if (notes.size() >= sLlmConfig->memoryMaxNotesPerBot)
                return Acore::StringFormat(
                    "your memory is full ({} notes) - forget one first, or overwrite one by reusing its slug",
                    notes.size());

            if (subjectGuid)
            {
                auto aboutSubject = std::count_if(notes.begin(), notes.end(),
                    [&](Note const& note) { return note.subjectGuid == subjectGuid; });
                if (aboutSubject >= int64(sLlmConfig->memoryMaxNotesPerSubject))
                    return Acore::StringFormat(
                        "you already have {} notes about them - overwrite one by reusing its slug, or forget one",
                        aboutSubject);
            }

            notes.push_back({ std::move(normalized), subjectGuid, content, ++_sequence });
            it = notes.end() - 1;
        }
        else
        {
            it->subjectGuid = subjectGuid;
            it->content = content;
            it->sequence = ++_sequence;
        }

        _pending.push_back({ botGuid.GetCounter(), it->slug, subjectGuid, std::move(content), false });
        return "";
    }

    bool MemoryStore::Forget(ObjectGuid botGuid, std::string const& slug)
    {
        std::string normalized = NormalizeSlug(slug);

        std::lock_guard<std::mutex> lock(_mutex);
        auto mapIt = _notes.find(botGuid.GetCounter());
        if (mapIt == _notes.end())
            return false;

        auto it = std::find_if(mapIt->second.begin(), mapIt->second.end(),
            [&](Note const& note) { return note.slug == normalized; });
        if (it == mapIt->second.end())
            return false;

        mapIt->second.erase(it);
        _pending.push_back({ botGuid.GetCounter(), std::move(normalized), 0, "", true });
        return true;
    }

    std::string MemoryStore::Format(ObjectGuid botGuid, uint32 subjectGuid, uint32 maxLines)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto mapIt = _notes.find(botGuid.GetCounter());
        if (mapIt == _notes.end())
            return "";

        std::vector<Note const*> picked;
        for (Note const& note : mapIt->second)
            if (note.subjectGuid == subjectGuid || note.subjectGuid == 0)
                picked.push_back(&note);

        // Notes about the subject before general ones, newest first within
        // each group, so the cap trims idle general notes rather than what
        // the bot knows about who it is talking to.
        std::sort(picked.begin(), picked.end(), [&](Note const* a, Note const* b)
        {
            bool aSubject = subjectGuid && a->subjectGuid == subjectGuid;
            bool bSubject = subjectGuid && b->subjectGuid == subjectGuid;
            if (aSubject != bSubject)
                return aSubject;
            return a->sequence > b->sequence;
        });

        std::string result;
        for (size_t i = 0; i < picked.size() && i < maxLines; ++i)
            result += Acore::StringFormat("- [{}] {}\n", picked[i]->slug, picked[i]->content);
        return result;
    }

    std::string MemoryStore::NormalizeSlug(std::string const& slug)
    {
        std::string normalized;
        for (char c : slug)
        {
            unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc))
                normalized += char(std::tolower(uc));
            else if ((c == ' ' || c == '-' || c == '_') && !normalized.empty() && normalized.back() != '-')
                normalized += '-';
        }
        while (!normalized.empty() && normalized.back() == '-')
            normalized.pop_back();
        if (normalized.size() > MAX_SLUG_LENGTH)
            normalized.resize(MAX_SLUG_LENGTH);
        return normalized;
    }

    void MemoryStore::Clear()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _notes.clear();
        _pending.clear();
        _sequence = 0;
    }
}
