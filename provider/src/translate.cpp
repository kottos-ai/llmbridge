#include "provider/translate.hpp"

#include <charconv>
#include <string>

#include "provider/json.hpp"

namespace kottos::provider
{
    namespace
    {
        long long to_ll(const std::string& s)
        {
            long long v = 0;
            std::from_chars(s.data(), s.data() + s.size(), v);
            return v;
        }

        // Extract message text from an OpenAI "content" field, which is either a
        // plain string or an array of parts ({type:"text", text:"..."}).
        std::string content_text(const json::Value* c)
        {
            if (!c) return {};
            if (c->is_string()) return c->str;
            if (c->is_array())
            {
                std::string out;
                for (const auto& part : c->arr)
                    if (part.str_or("type") == "text") out += part.str_or("text");
                return out;
            }
            return {};
        }
    } // namespace

    std::string openai_to_anthropic_request(std::string_view openai_body)
    {
        bool ok = false;
        json::Value v = json::parse(openai_body, ok);
        if (!ok || !v.is_object()) return {};

        std::string system;       // collected from role:"system" messages
        std::string messages = "["; // anthropic user/assistant turns
        bool first = true;
        if (const json::Value* msgs = v.find("messages"); msgs && msgs->is_array())
        {
            for (const auto& m : msgs->arr)
            {
                const std::string role = m.str_or("role");
                const std::string content = content_text(m.find("content"));
                if (role == "system")
                {
                    if (!system.empty()) system += "\n";
                    system += content;
                    continue;
                }
                if (!first) messages += ',';
                first = false;
                messages += "{\"role\":";
                json::append_escaped(messages, role);
                messages += ",\"content\":";
                json::append_escaped(messages, content);
                messages += '}';
            }
        }
        messages += ']';

        std::string out = "{\"model\":";
        json::append_escaped(out, v.str_or("model", "claude-3-5-sonnet-latest"));
        // Anthropic requires max_tokens; default if the OpenAI request omitted it.
        out += ",\"max_tokens\":" + v.num_or("max_tokens", "1024");
        if (!system.empty())
        {
            out += ",\"system\":";
            json::append_escaped(out, system);
        }
        if (std::string t = v.num_or("temperature"); !t.empty()) out += ",\"temperature\":" + t;
        if (std::string p = v.num_or("top_p"); !p.empty()) out += ",\"top_p\":" + p;
        out += ",\"messages\":" + messages + "}";
        return out;
    }

    std::string anthropic_to_openai_response(std::string_view anthropic_body)
    {
        bool ok = false;
        json::Value v = json::parse(anthropic_body, ok);
        if (!ok || !v.is_object()) return {};

        // content: array of blocks; join the text of text-blocks.
        std::string content;
        if (const json::Value* c = v.find("content"); c && c->is_array())
            for (const auto& blk : c->arr)
                if (blk.str_or("type") == "text") content += blk.str_or("text");

        // stop_reason -> OpenAI finish_reason.
        const std::string sr = v.str_or("stop_reason", "end_turn");
        const char* finish = sr == "max_tokens" ? "length" : sr == "tool_use" ? "tool_calls" : "stop";

        long long in_tok = 0, out_tok = 0;
        if (const json::Value* u = v.find("usage"))
        {
            in_tok = to_ll(u->num_or("input_tokens", "0"));
            out_tok = to_ll(u->num_or("output_tokens", "0"));
        }

        std::string out = "{\"id\":";
        json::append_escaped(out, v.str_or("id", "chatcmpl-kottos"));
        out += ",\"object\":\"chat.completion\",\"created\":0,\"model\":";
        json::append_escaped(out, v.str_or("model"));
        out += ",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":";
        json::append_escaped(out, content);
        out += "},\"finish_reason\":";
        json::append_escaped(out, finish);
        out += "}],\"usage\":{\"prompt_tokens\":" + std::to_string(in_tok) +
               ",\"completion_tokens\":" + std::to_string(out_tok) +
               ",\"total_tokens\":" + std::to_string(in_tok + out_tok) + "}}";
        return out;
    }

    // ── Google Gemini (generateContent) ─────────────────────────────────────

    std::string openai_to_gemini_request(std::string_view openai_body)
    {
        bool ok = false;
        json::Value v = json::parse(openai_body, ok);
        if (!ok || !v.is_object()) return {};

        std::string system;          // collected from role:"system" messages
        std::string contents = "[";  // gemini "contents" turns
        bool first = true;
        if (const json::Value* msgs = v.find("messages"); msgs && msgs->is_array())
        {
            for (const auto& m : msgs->arr)
            {
                const std::string role = m.str_or("role");
                const std::string content = content_text(m.find("content"));
                if (role == "system")
                {
                    if (!system.empty()) system += "\n";
                    system += content;
                    continue;
                }
                if (!first) contents += ',';
                first = false;
                // OpenAI "assistant" -> Gemini "model"; everything else -> "user".
                contents += "{\"role\":";
                json::append_escaped(contents, role == "assistant" ? "model" : "user");
                contents += ",\"parts\":[{\"text\":";
                json::append_escaped(contents, content);
                contents += "}]}";
            }
        }
        contents += ']';

        std::string out = "{\"contents\":" + contents;
        if (!system.empty())
        {
            out += ",\"systemInstruction\":{\"parts\":[{\"text\":";
            json::append_escaped(out, system);
            out += "}]}";
        }
        // generationConfig — only the keys the OpenAI request actually set.
        std::string gc;
        if (std::string mt = v.num_or("max_tokens"); !mt.empty()) gc += "\"maxOutputTokens\":" + mt;
        if (std::string t = v.num_or("temperature"); !t.empty())
        {
            if (!gc.empty()) gc += ',';
            gc += "\"temperature\":" + t;
        }
        if (std::string p = v.num_or("top_p"); !p.empty())
        {
            if (!gc.empty()) gc += ',';
            gc += "\"topP\":" + p;
        }
        if (!gc.empty()) out += ",\"generationConfig\":{" + gc + "}";
        out += "}";
        return out;
    }

    std::string gemini_to_openai_response(std::string_view gemini_body)
    {
        bool ok = false;
        json::Value v = json::parse(gemini_body, ok);
        if (!ok || !v.is_object()) return {};

        std::string content;
        std::string finish = "stop";
        if (const json::Value* cands = v.find("candidates"); cands && cands->is_array() && !cands->arr.empty())
        {
            const json::Value& c0 = cands->arr.front();
            if (const json::Value* cont = c0.find("content"))
                if (const json::Value* parts = cont->find("parts"); parts && parts->is_array())
                    for (const auto& part : parts->arr) content += part.str_or("text");
            // Gemini finishReason -> OpenAI finish_reason.
            const std::string fr = c0.str_or("finishReason", "STOP");
            finish = fr == "MAX_TOKENS" ? "length"
                   : (fr == "SAFETY" || fr == "RECITATION" || fr == "BLOCKLIST" ||
                      fr == "PROHIBITED_CONTENT") ? "content_filter"
                   : "stop";
        }

        long long in_tok = 0, out_tok = 0, total = 0;
        if (const json::Value* u = v.find("usageMetadata"))
        {
            in_tok = to_ll(u->num_or("promptTokenCount", "0"));
            out_tok = to_ll(u->num_or("candidatesTokenCount", "0"));
            total = to_ll(u->num_or("totalTokenCount", "0"));
        }
        if (total == 0) total = in_tok + out_tok;

        std::string out = "{\"id\":\"chatcmpl-kottos\",\"object\":\"chat.completion\",\"created\":0,\"model\":";
        json::append_escaped(out, v.str_or("modelVersion"));
        out += ",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":";
        json::append_escaped(out, content);
        out += "},\"finish_reason\":";
        json::append_escaped(out, finish);
        out += "}],\"usage\":{\"prompt_tokens\":" + std::to_string(in_tok) +
               ",\"completion_tokens\":" + std::to_string(out_tok) +
               ",\"total_tokens\":" + std::to_string(total) + "}}";
        return out;
    }

    // ── Cohere (Chat API v2, /v2/chat) ──────────────────────────────────────

    std::string openai_to_cohere_request(std::string_view openai_body)
    {
        bool ok = false;
        json::Value v = json::parse(openai_body, ok);
        if (!ok || !v.is_object()) return {};

        // Cohere v2 keeps OpenAI-style system/user/assistant message turns; the
        // body differences are top_p -> "p" and the response shape.
        std::string messages = "[";
        bool first = true;
        if (const json::Value* msgs = v.find("messages"); msgs && msgs->is_array())
        {
            for (const auto& m : msgs->arr)
            {
                const std::string role = m.str_or("role");
                const std::string content = content_text(m.find("content"));
                if (!first) messages += ',';
                first = false;
                messages += "{\"role\":";
                json::append_escaped(messages, role);
                messages += ",\"content\":";
                json::append_escaped(messages, content);
                messages += '}';
            }
        }
        messages += ']';

        std::string out = "{\"model\":";
        json::append_escaped(out, v.str_or("model", "command-r-plus"));
        out += ",\"messages\":" + messages;
        if (std::string mt = v.num_or("max_tokens"); !mt.empty()) out += ",\"max_tokens\":" + mt;
        if (std::string t = v.num_or("temperature"); !t.empty()) out += ",\"temperature\":" + t;
        if (std::string p = v.num_or("top_p"); !p.empty()) out += ",\"p\":" + p; // Cohere: top_p is "p"
        out += "}";
        return out;
    }

    std::string cohere_to_openai_response(std::string_view cohere_body)
    {
        bool ok = false;
        json::Value v = json::parse(cohere_body, ok);
        if (!ok || !v.is_object()) return {};

        // message.content: array of blocks; join the text of text-blocks.
        std::string content;
        if (const json::Value* msg = v.find("message"))
            if (const json::Value* c = msg->find("content"); c && c->is_array())
                for (const auto& blk : c->arr)
                    if (blk.str_or("type") == "text") content += blk.str_or("text");

        // Cohere finish_reason -> OpenAI finish_reason.
        const std::string fr = v.str_or("finish_reason", "COMPLETE");
        const char* finish = fr == "MAX_TOKENS" ? "length" : fr == "TOOL_CALL" ? "tool_calls" : "stop";

        long long in_tok = 0, out_tok = 0;
        if (const json::Value* u = v.find("usage"))
            if (const json::Value* t = u->find("tokens"))
            {
                in_tok = to_ll(t->num_or("input_tokens", "0"));
                out_tok = to_ll(t->num_or("output_tokens", "0"));
            }

        std::string out = "{\"id\":";
        json::append_escaped(out, v.str_or("id", "chatcmpl-kottos"));
        out += ",\"object\":\"chat.completion\",\"created\":0,\"model\":";
        json::append_escaped(out, v.str_or("model"));
        out += ",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":";
        json::append_escaped(out, content);
        out += "},\"finish_reason\":";
        json::append_escaped(out, finish);
        out += "}],\"usage\":{\"prompt_tokens\":" + std::to_string(in_tok) +
               ",\"completion_tokens\":" + std::to_string(out_tok) +
               ",\"total_tokens\":" + std::to_string(in_tok + out_tok) + "}}";
        return out;
    }
} // namespace kottos::provider
