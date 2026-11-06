// Copyright (c) 2026 ProtocolTeam (Zhandos). All rights reserved.
// Viewing allowed. No copying, modification, commercial use, or redistribution
// without written permission from ProtocolTeam (Zhandos).

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

enum class AdapterKind { Local, OpenAI, Ollama, LlamaCpp, HuggingFace };

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

struct Message {
    std::string role;
    std::string content;
    std::string kind;
    std::string file_id;
    bool pinned = false;
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id;
};

struct Segment {
    std::string id;
    std::string kind;
    std::string summary;
    std::vector<Message> messages;
};

struct Config {
    int max_context_tokens = 4096;
    int reserve_tokens = 512;
    double compact_threshold = 0.75;
    int min_keep_messages = 4;
    int max_chunk_messages = 8;
    int dump_chunk_tokens = 0;
    bool inject_protocol = true;
    int max_tool_hops = 3;
};

struct Directive {
    std::string kind;
    std::string file_id;
    std::string query;
};

struct ModelReply {
    std::string text;
    std::vector<ToolCall> tool_calls;
    std::string error;
};

struct TurnEvent {
    std::string type;
    std::string file_id;
    std::string detail;
};

struct TurnResult {
    std::string reply;
    std::vector<TurnEvent> events;
    std::string error;
};

int count_tokens(const std::string& text);
int count_messages(const std::vector<Message>& messages);
std::string format_pointer(const std::string& file_id, const std::string& summary);
std::string extractive_summary(const std::vector<Message>& messages);
std::vector<Directive> parse_directives(const std::string& text);
std::string strip_directives(const std::string& text);
std::string render_segment_text(const Segment& segment);

class Store {
public:
    explicit Store(std::filesystem::path root);
    std::string write_segment(Segment segment);
    Segment read_segment(const std::string& id) const;
    std::vector<Segment> list_segments() const;
    void save_session(const std::vector<Message>& messages) const;
    std::vector<Message> load_session() const;
    const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;
    int next_id_ = 1;
    void load_manifest();
    void save_manifest() const;
};

class Bridge {
public:
    Bridge(Store store, Config config);
    std::vector<Message> messages;
    Store store;
    Config config;
    std::string warning;

    int prompt_budget() const;
    int threshold_tokens() const;
    int prompt_tokens() const;
    std::vector<Message> pointers() const;
    void ensure_budget(std::vector<TurnEvent>* events);
    bool dump_once(std::vector<TurnEvent>* events, bool relax_if_needed, bool force = false);
    std::string read_for_model(const std::string& file_id) const;
    std::string search_for_model(const std::string& query) const;
    void save();
    TurnResult run_turn(const std::string& user_text, const std::function<ModelReply(const std::vector<Message>&)>& ask);

private:
    void seed();
    void split(std::vector<Message>& pinned, std::vector<Message>& head, std::vector<Message>& tail) const;
};

ModelReply local_reply(const std::vector<Message>& messages);

std::string endpoint_for(AdapterKind kind, const std::string& base_url);
std::string request_body(AdapterKind kind, const std::string& model, const std::vector<Message>& messages, bool tools);
ModelReply parse_response(AdapterKind kind, const std::string& body);
