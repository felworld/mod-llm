/*
 * This file is part of mod-llm for Felworld. Released under the MIT license
 * (see the LICENSE file at the module root).
 */

// Registration functions defined in the individual script files.
void AddSC_llm_world();
void AddSC_llm_chat();
void AddSC_llm_event();
void AddSC_llm_command();

// Entry point invoked by the generated module script loader.
void Addmod_llmScripts()
{
    AddSC_llm_world();
    AddSC_llm_chat();
    AddSC_llm_event();
    AddSC_llm_command();
}
