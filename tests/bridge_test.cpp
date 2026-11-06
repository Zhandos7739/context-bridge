// Copyright (c) 2026 ProtocolTeam (Zhandos). All rights reserved.
// Viewing allowed. No copying, modification, commercial use, or redistribution
// without written permission from ProtocolTeam (Zhandos).

#include "bridge.hpp"
#include "json.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int fails = 0;

void expect(bool cond, const std::string& text) {
    if (!cond) {
        std::cerr << "FAIL " << text << "\n";
        ++fails;
    }
}

Config small() {
    Config config;
    config.max_context_tokens = 420;
    config.reserve_tokens = 40;
    config.compact_threshold = 0.55;
    config.min_keep_messages = 2;
    config.max_chunk_messages = 4;
    config.dump_chunk_tokens = 120;
    config.inject_protocol = false;
    return config;
}

ModelReply ok_reply(const std::vector<Message>&) {
    ModelReply reply;
    reply.text = "ok";
    return reply;
}

std::string filler(int index) {
    return "Filler about sourdough and train timetables number " + std::to_string(index) + " " + std::string(600, 'x');
}

std::string file_with(Store& store, const std::string& needle) {
    for (const auto& segment : store.list_segments()) {
        for (const auto& message : segment.messages) {
            if (message.content.find(needle) != std::string::npos) return segment.id;
        }
    }
    return {};
}

}  // namespace

int main() {
    expect(format_pointer("123994", "user asked about a laptop or tech") ==
               "file 123994: user asked about a laptop or tech; details and original text are in the file",
           "pointer sentence");
    auto directives = parse_directives("see [[ctx:read:12]] and <ctx-search q=\"laptop\">");
    expect(directives.size() == 2 && directives[0].file_id == "12" && directives[1].query == "laptop", "directives");

    auto dir = std::filesystem::temp_directory_path() / "context-bridge-test";
    std::filesystem::remove_all(dir);
    {
        Bridge bridge(Store(dir), small());
        bridge.run_turn("ZEBRA_LAPTOP_ALPHA I need a laptop and thermal paste.", ok_reply);
        for (int i = 0; i < 8; ++i) bridge.run_turn(filler(i), ok_reply);
        std::string hot;
        for (const auto& message : bridge.messages) hot += message.content;
        expect(hot.find("ZEBRA_LAPTOP_ALPHA") == std::string::npos, "original left the window");
        expect(!bridge.pointers().empty(), "pointer is in the window");
        expect(bridge.pointers()[0].content.find("details and original text are in the file") != std::string::npos, "clause");
        std::string id = file_with(bridge.store, "ZEBRA_LAPTOP_ALPHA");
        expect(!id.empty(), "original is in a numbered file");
        Segment loaded = bridge.store.read_segment(id);
        expect(loaded.messages[0].content.find("ZEBRA_LAPTOP_ALPHA") != std::string::npos, "reload");

        int step = 0;
        std::vector<std::vector<Message>> calls;
        auto reader = [&](const std::vector<Message>& messages) {
            calls.push_back(messages);
            ModelReply reply;
            if (step++ == 0) reply.text = "[[ctx:read:" + id + "]]";
            else reply.text = "The original mentions ZEBRA_LAPTOP_ALPHA.";
            return reply;
        };
        TurnResult turn = bridge.run_turn("re-read the file", reader);
        expect(turn.reply.find("ZEBRA_LAPTOP_ALPHA") != std::string::npos, "model saw the reloaded text");
        expect(calls.size() >= 2, "second call happened");
        bool saw = false;
        if (calls.size() >= 2) {
            for (const auto& message : calls[1]) {
                if (message.content.find("BEGIN FILE") != std::string::npos &&
                    message.content.find("ZEBRA_LAPTOP_ALPHA") != std::string::npos)
                    saw = true;
            }
        }
        expect(saw, "reload text was handed to the model");

        step = 0;
        auto tools = [&](const std::vector<Message>& messages) {
            calls.push_back(messages);
            ModelReply reply;
            if (step++ == 0) {
                ToolCall call;
                call.id = "t1";
                call.name = "read_context_file";
                call.arguments = "{\"file_id\":\"" + id + "\"}";
                reply.tool_calls.push_back(call);
            } else {
                reply.text = "tool reloaded ZEBRA_LAPTOP_ALPHA";
            }
            return reply;
        };
        TurnResult via_tool = bridge.run_turn("look up the laptop notes", tools);
        expect(via_tool.reply.find("ZEBRA_LAPTOP_ALPHA") != std::string::npos, "tool reload");
    }
    {
        Bridge restored(Store(dir), small());
        expect(!restored.pointers().empty(), "session restored the pointer");
    }

    expect(endpoint_for(AdapterKind::Ollama, "http://127.0.0.1:11434/v1") == "http://127.0.0.1:11434/api/chat", "ollama url");
    expect(endpoint_for(AdapterKind::LlamaCpp, "http://127.0.0.1:8080/v1") == "http://127.0.0.1:8080/v1/chat/completions",
           "llamacpp url");
    std::string body = request_body(AdapterKind::OpenAI, "gpt-test", {Message{"user", "hi", "", "", false, {}, ""}}, true);
    expect(body.find("\"model\":\"gpt-test\"") != std::string::npos, "request model");
    expect(body.find("read_context_file") != std::string::npos, "tools offered");
    expect(body.find("secret") == std::string::npos, "no extra meta");

    const char* sample =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"\","
        "\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"read_context_file\","
        "\"arguments\":\"{\\\"file_id\\\":\\\"9\\\"}\"}}]}}]}";
    ModelReply parsed = parse_response(AdapterKind::OpenAI, sample);
    expect(parsed.tool_calls.size() == 1 && parsed.tool_calls[0].name == "read_context_file", "parse tool name");
    expect(parsed.tool_calls[0].arguments.find("9") != std::string::npos, "parse tool id");

    Json round = parse_json("{\"a\":\"файл\",\"n\":1}");
    expect(round.at("a").str == "файл", "utf8 json");

    std::filesystem::remove_all(dir);
    if (fails) {
        std::cerr << fails << " failed\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
